/* bench_gc.c — how much does this collector retain that is not live?
 *
 * The earlier bench measured two constructed extremes: a case with the
 * stack deliberately scrubbed (0% retention) and a case with an array of
 * live pointers deliberately kept (100%).  Both were arrangements, not
 * measurements — neither says anything about a real program.
 *
 * This one runs a workload that does ordinary things: recursion, nested
 * allocation, temporaries that fall out of use without anyone clearing
 * them.  Nothing is scrubbed and nothing is deliberately retained.
 *
 * The one thing that must stay artificial is GROUND TRUTH.  False
 * retention is (what the collector marked) minus (what was genuinely
 * live), and the collector cannot supply the second term — if it could
 * tell a real pointer from an integer that looks like one, it would not
 * be conservative.  So the workload is built so its live set is known by
 * construction: a full binary tree of depth d has exactly 2^(d+1) - 1
 * nodes, and nothing else in the program is reachable.
 */

/* clock_gettime and CLOCK_MONOTONIC are POSIX, not ISO C.  GCC's default
   gnu11 mode exposes them anyway; a strict -std=c11 build would not. */
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "memlib.h"
#include "mm.h"
#include "gc.h"

/* ── Node layout ───────────────────────────────────────────────────
 * Two child pointers in the first two payload words.  Anything past
 * that is payload the program never writes — which matters, see the
 * size table below. */
#define LEFT(p) (((void **)(p))[0])
#define RIGHT(p) (((void **)(p))[1])

static size_t g_node_bytes = 16; /* requested size, varied by table 2 */

/* g_use_malloc - which allocator the workload uses.
 *
 * mm_calloc zeroes the REQUESTED bytes.  mm_malloc zeroes nothing at all,
 * so a recycled block arrives holding whatever its predecessor left
 * behind — and for this allocator that includes the PRED/SUCC free-list
 * pointers written into the first two payload words while the block sat
 * on the free list.  Those are real heap addresses.
 *
 * A program using mm_malloc initialises the fields it cares about, so the
 * workload does too.  What stays stale is everything past them, and the
 * collector scans that as candidates like any other payload word. */
static int g_use_malloc;

/* g_root - the live set, and the bench's only root.  In .bss, which
   gc_collect scans. */
static void *g_root;
static size_t g_live_nodes;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void *alloc_node(void)
{
    void *p = g_use_malloc ? mm_malloc(g_node_bytes)
                           : mm_calloc(1, g_node_bytes);
    if (p == NULL)
    {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    return p;
}

/* build_tree - a full binary tree.  DEPTH 0 is a single node.
   Recursive on purpose: the construction frames hold child pointers,
   and they are dead by the time anything collects. */
static void *build_tree(int depth, size_t *count)
{
    void *n = alloc_node();

    (*count)++;
    /* Set both links on every node, leaves included.  A program that used
       mm_malloc would initialise its own fields; leaving them stale would
       be a bug in the workload, not a property of the collector. */
    LEFT(n) = NULL;
    RIGHT(n) = NULL;
    if (depth > 0)
    {
        LEFT(n) = build_tree(depth - 1, count);
        RIGHT(n) = build_tree(depth - 1, count);
    }
    return n;
}

/* drop_trees - build trees and let every reference to them die with this
   frame.  Genuine garbage, produced the way a program produces it. */
__attribute__((noinline)) static void drop_trees(int depth, int n, size_t *garbage)
{
    int i;

    for (i = 0; i < n; i++)
    {
        size_t c = 0;
        build_tree(depth, &c);
        *garbage += c;
    }
}

/* descend - recurse to DEPTH, allocating one node per level.
 *
 * Each level touches its node once and never again, so by the time the
 * collection happens at the bottom that local is DEAD in the language's
 * sense — the program will not use it — while the frame holding it is
 * still very much alive on the stack.
 *
 * That is the realistic case, and the one the previous bench could not
 * produce: no scrubbing, no deliberate retention, just an ordinary call
 * chain with ordinary leftovers in it. */
__attribute__((noinline)) static void descend(int depth, size_t *garbage,
                                              double *ms)
{
    void *tmp = alloc_node();

    LEFT(tmp) = NULL; /* used once, here, and never referenced again */
    (*garbage)++;

    if (depth > 0)
    {
        descend(depth - 1, garbage, ms);
    }
    else
    {
        double t0 = now_ms();
        gc_collect();
        *ms = now_ms() - t0;
    }
}

/* reset_heap - drop the live set and reclaim it.
 *
 * One collection is enough, verified rather than assumed: after dropping
 * the root, a single pass leaves 0 blocks allocated at both -O0 and -O2,
 * and adding a second changes no figure in any table below.
 *
 * It works because this runs at shallow stack depth.  The previous row's
 * deep descend() frames sit BELOW gc_stack_lo and are never scanned, and
 * the tree-building frames that returned get overwritten by gc_collect's
 * own call chain — gc_build_table, gc_mark_range, gc_lookup, then
 * gc_sweep into mm_free and coalesce.
 *
 * That is a property of this call shape, not a general law.  M4's orphan
 * test needed two passes because a tiny function left one pointer in a
 * slot gc_collect happened not to write. */
__attribute__((noinline)) static void reset_heap(void)
{
    g_root = NULL;
    g_live_nodes = 0;
    gc_collect();
}

/* plant_live - build the live tree.  noinline so the construction frames
   are gone before anything measures. */
__attribute__((noinline)) static void plant_live(int depth)
{
    size_t c = 0;
    g_root = build_tree(depth, &c);
    g_live_nodes = c;
}

static void print_header(const char *what)
{
    printf("\n  %s\n\n", what);
    printf("  %6s %7s %8s %8s %8s %10s %10s\n",
           "var", "live", "garbage", "marked", "false", "false-ret%", "collect ms");
    printf("  ------ ------- -------- -------- -------- ---------- ----------\n");
}

/* one_run - plant a live tree, make garbage, recurse, collect, report.
   Returns the false-retention percentage. */
static double one_run(const char *label, int live_depth, int garbage_trees,
                      int garbage_depth, int recursion)
{
    size_t marked, marked_bytes, total, total_bytes;
    size_t garbage = 0, live, false_blocks;
    double ms = 0.0, pct;

    reset_heap();
    plant_live(live_depth);
    drop_trees(garbage_depth, garbage_trees, &garbage);

    descend(recursion, &garbage, &ms);

    gc_stats(&marked, &marked_bytes, &total, &total_bytes);
    live = g_live_nodes;
    false_blocks = marked > live ? marked - live : 0;
    pct = garbage ? 100.0 * (double)false_blocks / (double)garbage : 0.0;

    printf("  %6s %7zu %8zu %8zu %8zu %9.1f%% %10.3f\n",
           label, live, total - live, marked, false_blocks, pct, ms);
    return pct;
}

/* timed_run - set up, collect, and report the BEST of REPS attempts.
 *
 * A single wall-clock reading is not trustworthy here: repeated runs of
 * the same measurement spread by more than 2x under ordinary system
 * load.  The minimum is the run that suffered least interference, which
 * is the conventional estimator for this kind of microbenchmark.  The
 * retention figures need no such treatment — they are deterministic. */
static double timed_run(int live_depth, int garbage_trees, int garbage_depth,
                        int recursion, int reps, size_t *total_out)
{
    double best = -1.0;
    int r;

    for (r = 0; r < reps; r++)
    {
        size_t marked, mb, total, tb, g = 0;
        double ms = 0.0;

        reset_heap();
        plant_live(live_depth);
        drop_trees(garbage_depth, garbage_trees, &g);
        descend(recursion, &g, &ms);
        gc_stats(&marked, &mb, &total, &tb);

        if (best < 0.0 || ms < best)
            best = ms;
        if (total_out)
            *total_out = total;
    }
    return best;
}

/* quiet_run - one_run without the printed row, for side-by-side tables. */
static double quiet_run(int live_depth, int garbage_trees, int garbage_depth,
                        int recursion, size_t *marked_out, size_t *false_out)
{
    size_t marked, marked_bytes, total, total_bytes;
    size_t garbage = 0, live, false_blocks;
    double ms = 0.0;

    reset_heap();
    plant_live(live_depth);
    drop_trees(garbage_depth, garbage_trees, &garbage);
    descend(recursion, &garbage, &ms);

    gc_stats(&marked, &marked_bytes, &total, &total_bytes);
    live = g_live_nodes;
    false_blocks = marked > live ? marked - live : 0;
    if (marked_out)
        *marked_out = marked;
    if (false_out)
        *false_out = false_blocks;
    return garbage ? 100.0 * (double)false_blocks / (double)garbage : 0.0;
}

static void region_table(void)
{
    size_t r, sc, ac, tot_sc = 0, tot_ac = 0;

    printf("\n  %-12s %10s %10s %9s\n", "region", "words", "accepted", "hit-rate");
    printf("  ------------ ---------- ---------- ---------\n");
    for (r = 0; r < GC_NREGIONS; r++)
    {
        gc_region_stats((int)r, &sc, &ac);
        tot_sc += sc;
        tot_ac += ac;
        printf("  %-12s %10zu %10zu %8.2f%%\n",
               gc_region_label((int)r), sc, ac,
               sc ? 100.0 * (double)ac / (double)sc : 0.0);
    }
    printf("  ------------ ---------- ---------- ---------\n");
    printf("  %-12s %10zu %10zu %8.2f%%\n", "total", tot_sc, tot_ac,
           tot_sc ? 100.0 * (double)tot_ac / (double)tot_sc : 0.0);
}

/* noise_run - does an arbitrary 32-bit integer pass the pointer test?
 *
 * CLAUDE.md predicts heavy retention on i386 because a 4-byte word is a
 * complete pointer, so any int is a candidate.  True in principle; this
 * measures the magnitude.  A candidate is accepted only if it lands in
 * the MAPPED heap, so the chance is heap size over the 4 GiB address
 * space — small heap, small chance. */
__attribute__((noinline)) static void noise_run(size_t words)
{
    volatile unsigned int noise[8192];
    size_t marked, marked_bytes, total, total_bytes, k, heap;
    unsigned int seed = 12345u, checksum = 0u;

    for (k = 0; k < words && k < 8192; k++)
    {
        seed = seed * 1103515245u + 12345u;
        noise[k] = seed;
    }

    gc_collect();

    for (k = 0; k < words && k < 8192; k++)
        checksum ^= noise[k];
    (void)checksum;

    gc_stats(&marked, &marked_bytes, &total, &total_bytes);
    heap = (size_t)((char *)gc_heap_hi() - (char *)gc_heap_lo() + 1);

    printf("  %6zu %9.1f %12.6f %14.3f %8zu\n",
           words, (double)heap / 1024.0,
           100.0 * (double)heap / 4294967296.0,
           (double)words * (double)heap / 4294967296.0,
           marked > g_live_nodes ? marked - g_live_nodes : 0);
}

int main(void)
{
    static const int depths[] = {0, 1, 2, 4, 8, 16, 32, 64, 128};
    static const size_t sizes[] = {16, 20, 24, 28, 64, 100, 104};
    static const int live_depths[] = {2, 4, 6, 8, 10, 12, 14};
    static const int tree_counts[] = {1, 10, 50, 100, 200, 400};
    size_t i;

    mem_init();
    mm_init();
    gc_init(__builtin_frame_address(0));

    printf("Conservative GC — false retention under a realistic workload\n");
    printf("i386, %zu-byte pointers.  Live set is a binary tree rooted in a\n",
           sizeof(void *));
    printf("global; its size is known by construction, so false retention is\n");
    printf("measurable.  Nothing is scrubbed and nothing is deliberately kept.\n");

    /* ---- 1. retention against how deep the stack is at collection ---- */
    print_header("[1] recursion depth at the moment of collection"
                 "  (live tree depth 6 = 127 nodes, 20 garbage trees of depth 3)");
    for (i = 0; i < sizeof depths / sizeof depths[0]; i++)
    {
        char label[16];
        snprintf(label, sizeof label, "d=%d", depths[i]);
        one_run(label, 6, 20, 3, depths[i]);
    }

    /* ---- 2. retention against allocation size ----
       mm_calloc zeroes the REQUESTED size, not the padded block size.  A
       request of 100 gets a 104-byte payload whose last 4 bytes still
       hold whatever the block contained when it was free — which for this
       allocator means PRED/SUCC, real heap addresses.  Sizes that are
       already multiples of 8 have no padding and no such words. */
    print_header("[2] requested node size  (recursion depth 32)"
                 "  — sizes not a multiple of 8 carry unzeroed padding");
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        char label[16];
        size_t padded = (sizes[i] + 7u) & ~7u;
        g_node_bytes = sizes[i];
        snprintf(label, sizeof label, "%zu+%zu", sizes[i], padded - sizes[i]);
        one_run(label, 6, 20, 3, 32);
    }
    g_node_bytes = 16;

    /* ---- 3. mm_calloc against mm_malloc ----
       mm_calloc zeroes the requested bytes; mm_malloc zeroes nothing.  A
       recycled block therefore arrives holding its predecessor's data,
       including the PRED/SUCC free-list pointers the allocator wrote into
       the first two payload words while it sat on the free list.  The
       workload initialises the fields it uses either way, so what differs
       is only the payload past them. */
    printf("\n  [3] mm_calloc vs mm_malloc  (recursion depth 32)\n");
    printf("      calloc zeroes the requested bytes; malloc zeroes nothing,\n");
    printf("      so stale words survive into the part the program never writes\n");
    printf("\n  %6s %8s %8s %8s %10s   %8s %8s %10s\n",
           "size", "c:marked", "c:false", "c:ret%", "", "m:marked", "m:false", "m:ret%");
    printf("  ------ -------- -------- -------- ----------   -------- -------- ----------\n");
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        size_t cm, cf, mm_, mf;
        double cp, mp;

        g_node_bytes = sizes[i];

        g_use_malloc = 0;
        cp = quiet_run(6, 20, 3, 32, &cm, &cf);
        g_use_malloc = 1;
        mp = quiet_run(6, 20, 3, 32, &mm_, &mf);
        g_use_malloc = 0;

        printf("  %6zu %8zu %8zu %7.1f%% %10s   %8zu %8zu %9.1f%%\n",
               sizes[i], cm, cf, cp, "", mm_, mf, mp);
    }
    g_node_bytes = 16;

    /* ---- 4. where the accepted candidates came from ---- */
    printf("\n  [4] attribution for a depth-32 run\n");
    one_run("d=32", 6, 20, 3, 32);
    region_table();

    /* ---- 5. cost ----
       Two separate scalings.  Marking walks the reachable graph, so it
       tracks the LIVE set.  Building the block table and sweeping both
       walk the implicit list, so they track the WHOLE heap.  Varying one
       at a time tells them apart. */
    printf("\n  [5a] cost against live-set size  (garbage fixed at 20 trees)\n");
    printf("       best of 3 — a single wall-clock reading spreads by over 2x\n\n");
    printf("  %6s %8s %9s %9s %10s %12s\n",
           "depth", "live", "total", "heap KiB", "collect ms", "us/block");
    printf("  ------ -------- --------- --------- ---------- ------------\n");
    for (i = 0; i < sizeof live_depths / sizeof live_depths[0]; i++)
    {
        size_t total = 0, heap;
        double ms = timed_run(live_depths[i], 20, 3, 8, 3, &total);

        heap = (size_t)((char *)gc_heap_hi() - (char *)gc_heap_lo() + 1);
        printf("  %6d %8zu %9zu %9.1f %10.3f %12.3f\n",
               live_depths[i], g_live_nodes, total, (double)heap / 1024.0, ms,
               total ? 1000.0 * ms / (double)total : 0.0);
    }

    printf("\n  [5b] cost against heap size  (live fixed at depth 6 = 127 nodes)\n\n");
    printf("  %6s %8s %9s %9s %10s %12s\n",
           "trees", "live", "total", "heap KiB", "collect ms", "us/block");
    printf("  ------ -------- --------- --------- ---------- ------------\n");
    for (i = 0; i < sizeof tree_counts / sizeof tree_counts[0]; i++)
    {
        size_t total = 0, heap;
        double ms = timed_run(6, tree_counts[i], 5, 8, 3, &total);

        heap = (size_t)((char *)gc_heap_hi() - (char *)gc_heap_lo() + 1);
        printf("  %6d %8zu %9zu %9.1f %10.3f %12.3f\n",
               tree_counts[i], g_live_nodes, total, (double)heap / 1024.0, ms,
               total ? 1000.0 * ms / (double)total : 0.0);
    }

    /* ---- 6. do arbitrary integers pass the pointer test? ---- */
    printf("\n  [6] pseudo-random 32-bit words on the stack, no real addresses\n\n");
    printf("  %6s %9s %12s %14s %8s\n",
           "words", "heap KiB", "heap % of 4G", "expected hits", "actual");
    printf("  ------ --------- ------------ -------------- --------\n");
    reset_heap();
    plant_live(6);
    for (i = 1024; i <= 8192; i *= 2)
        noise_run(i);

    printf("\n  var         the variable for that table\n");
    printf("  live        nodes reachable from the global root (ground truth)\n");
    printf("  garbage     allocated blocks that are not in the live tree\n");
    printf("  marked      blocks the collector decided were reachable\n");
    printf("  false       marked - live: garbage retained anyway\n");
    printf("  false-ret%%  false / garbage\n");
    return 0;
}
