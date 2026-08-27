/* main_gc.c — test driver for the garbage collector.
 *
 * M0 — prove the collector links against the explicit allocator and can
 *      see where the heap lives.
 * M1 — the allocated-block table, and the pointer test built on it.
 */

#include <stdio.h>
#include <assert.h>
#include "memlib.h"
#include "mm.h"
#include "gc.h"

#define NBLOCKS 10
#define CHAIN_LEN 5
#define GARBAGE_LEN 3
#define NODE_BYTES 16 /* a multiple of 8, so the payload has no padding */

/* g_chain - the ONE global root the M2 test introduces.
 *
 * It lands in .bss, which is exactly what gc_collect scans.  Every other
 * pointer the test holds lives in a local, and locals are on the stack,
 * which M2 does not scan.  That asymmetry is the whole experiment. */
static void *g_chain;

static void section(const char *name)
{
    printf("\n========================================\n");
    printf(" %s\n", name);
    printf("========================================\n");
}

/* payload_size - what mm_malloc actually hands out for a request of
   REQUESTED bytes: the request rounded up to the next multiple of 8.
   Recomputed here rather than read back from the collector, so the test
   checks gc_isPtr against an independent expectation. */
static size_t payload_size(size_t requested)
{
    return (requested + 7u) & ~7u;
}

static void expect(const char *what, void *got, void *want)
{
    int ok = (got == want);
    printf("  %-44s -> %-12p %s\n", what, got, ok ? "ok" : "*** FAIL ***");
    assert(ok);
}

int main(void)
{
    /* A spread of sizes, several sitting on rounding boundaries, so the
       psize column in the dump actually proves something. */
    const size_t sizes[NBLOCKS] = {1, 8, 16, 17, 24, 32, 40, 64, 100, 200};
    void *blocks[NBLOCKS];
    size_t n, i;
    int local = 0; /* a stack object, for the stack-address case */

    mem_init(); /* create the heap                      */
    mm_init();  /* prologue, epilogue, first free block */
    gc_init();  /* record where all of that lives       */

    section("M0: heap bounds");
    {
        char *lo = (char *)gc_heap_lo();
        char *hi = (char *)gc_heap_hi();
        printf("heap low       : %p\n", (void *)lo);
        printf("heap high      : %p   (last valid byte, inclusive)\n", (void *)hi);
        printf("heap size      : %ld bytes\n", (long)(hi - lo + 1));
        printf("sizeof(void *) : %zu bytes\n", sizeof(void *));
    }

    section("M1a: table on a fresh heap");
    n = gc_build_table();
    gc_table_dump();
    printf("\nthe prologue carries the allocated flag but must not appear\n");
    assert(n == 0);
    assert(gc_table_count() == 0);

    section("M1b: table after 10 allocations");
    for (i = 0; i < NBLOCKS; i++)
    {
        blocks[i] = mm_malloc(sizes[i]);
        assert(blocks[i] != NULL);
    }
    n = gc_build_table();
    gc_table_dump();
    assert(n == NBLOCKS);
    printf("\nheap size now  : %ld bytes\n",
           (long)((char *)gc_heap_hi() - (char *)gc_heap_lo() + 1));

    section("M1c: the pointer test");

    /* ---- the five cases from the M1 done-when criterion ---- */
    {
        char *b = (char *)blocks[4];        /* requested 24 */
        size_t bs = payload_size(sizes[4]); /* payload  24  */

        printf("block [4] at %p, payload %zu bytes\n\n", (void *)b, bs);

        expect("block start", gc_isPtr(b), b);
        expect("interior, middle of payload", gc_isPtr(b + bs / 2), b);
        expect("interior, last byte of payload", gc_isPtr(b + bs - 1), b);
        expect("one byte BEFORE start (own header's last byte)", gc_isPtr(b - 1), NULL);
        expect("one byte PAST payload end (own footer)", gc_isPtr(b + bs), NULL);
        expect("address of a local variable (stack)", gc_isPtr(&local), NULL);
    }

    /* ---- boundary cases that actually bite ---- */
    printf("\n");
    {
        char *hi = (char *)gc_heap_hi();

        expect("NULL", gc_isPtr(NULL), NULL);
        expect("gc_heap_lo (alignment padding word)", gc_isPtr(gc_heap_lo()), NULL);
        expect("gc_heap_hi (inside epilogue header)", gc_isPtr(hi), NULL);
        expect("one past the last heap byte (mem_brk)", gc_isPtr(hi + 1), NULL);
    }

    /* ---- exhaustive sweeps, so an off-by-one anywhere is caught ---- */
    printf("\n");
    {
        char *big = (char *)blocks[9];
        size_t bigsz = payload_size(sizes[9]);
        size_t off;
        int bad = 0;

        for (off = 0; off < bigsz; off++)
            if (gc_isPtr(big + off) != big)
                bad++;
        printf("  %-44s -> %s\n",
               "every offset inside the 200-byte payload",
               bad ? "*** FAIL ***" : "ok, all 200 resolve to the start");
        assert(bad == 0);
    }
    {
        char *end8 = (char *)blocks[8] + payload_size(sizes[8]);
        char *start9 = (char *)blocks[9];
        char *p;
        int bad = 0;

        for (p = end8; p < start9; p++)
            if (gc_isPtr(p) != NULL)
                bad++;
        printf("  %-44s -> %s\n",
               "every byte of the footer+header gap",
               bad ? "*** FAIL ***" : "ok, all rejected");
        printf("      gap is %ld bytes, %p .. %p\n",
               (long)(start9 - end8), (void *)end8, (void *)(start9 - 1));
        assert(bad == 0);
    }

    section("M1d: after freeing blocks 2, 5 and 8");
    {
        char *freed = (char *)blocks[5];

        mm_free(blocks[2]);
        mm_free(blocks[5]);
        mm_free(blocks[8]);

        n = gc_build_table();
        gc_table_dump();
        assert(n == NBLOCKS - 3);

        printf("\n");
        expect("pointer into a NOW-FREED block", gc_isPtr(freed), NULL);
        expect("pointer into a surviving block", gc_isPtr(blocks[6]), blocks[6]);
    }

    section("allocator invariants still hold");
    checkheap(0); /* silent unless something is wrong */
    printf("checkheap(0) reported nothing\n");

    gc_free_table();

    /* ═══════════════════════════════════════════════════════════════
     *  M2 / M3 — a global pointer to a chain of 5, and 3 blocks with no
     *  reference from any root region.  M2 marked them; M3 reclaims the
     *  three that nothing points at.
     *
     *  There was a smoke test here through M2 that ran gc_collect on the
     *  M1 blocks — 7 live blocks that no root referenced — to show that
     *  the collector cannot see the stack.  It was safe only while
     *  nothing was freed.  With a sweep attached it reclaims all 7, and
     *  the reset loop below then double-frees them, which is exactly how
     *  it behaved before this section was removed: a segfault inside
     *  coalesce, reading PREV_BLKP of a block that was already gone.
     *
     *  The demonstration belongs in M6's mark-only mode, which can report
     *  what it would free without doing it.
     * ═══════════════════════════════════════════════════════════════ */

    section("M3: reset the heap");
    /* Free every M1 block that is still allocated — 2, 5 and 8 already
       went in M1d, and freeing them again would be a double free.  The
       counts below only mean what they say on an otherwise empty heap. */
    for (i = 0; i < NBLOCKS; i++)
        if (i != 2 && i != 5 && i != 8)
            mm_free(blocks[i]);
    assert(gc_build_table() == 0);
    gc_free_table();
    printf("heap holds no allocated blocks\n");

    section("M3: build a 5-block chain plus 3 unreferenced blocks");
    {
        void *garbage[GARBAGE_LEN];
        void *prev = NULL;
        size_t mblocks, mbytes, tblocks, tbytes;

        /* mm_calloc, not mm_malloc.  The collector is perfectly happy
           with garbage — a stale word that looks like a pointer just
           retains a block, which is imprecise but never wrong.  It is
           the TEST that cannot tolerate it: asserting an exact count of
           5 means controlling every word the marker will scan.  The
           allocator writes PRED/SUCC into freed payloads, and these
           blocks are carved from memory that was just freed. */
        for (i = 0; i < CHAIN_LEN; i++)
        {
            void *node = mm_calloc(1, NODE_BYTES);
            assert(node != NULL);
            *(void **)node = prev; /* link to the previous node */
            prev = node;
        }
        g_chain = prev; /* only the head is reachable from a root */

        for (i = 0; i < GARBAGE_LEN; i++)
        {
            garbage[i] = mm_calloc(1, NODE_BYTES);
            assert(garbage[i] != NULL);
        }

        printf("chain head (global g_chain) : %p\n", g_chain);
        printf("garbage blocks (locals only): %p %p %p\n",
               garbage[0], garbage[1], garbage[2]);

        section("M3: collect — 5 reachable, 3 reclaimed");

        gc_collect();
        gc_report();
        gc_stats(&mblocks, &mbytes, &tblocks, &tbytes);

        /* These figures describe the heap as it was BEFORE the sweep,
           because gc_record_stats runs while the mark bits still exist.
           So tblocks is still 8 even though only 5 blocks remain. */
        printf("\n");
        assert(tblocks == CHAIN_LEN + GARBAGE_LEN);
        assert(mblocks == CHAIN_LEN);
        assert(mbytes == CHAIN_LEN * NODE_BYTES);
        printf("  before the sweep: %zu allocated, %zu reachable\n",
               tblocks, mblocks);

        /* Rebuilding the table is the only way to see what survived it. */
        assert(gc_build_table() == CHAIN_LEN);
        printf("  after  the sweep:\n");
        gc_table_dump();

        section("M3: the reclaimed blocks are really gone");
        /* garbage[] now holds dangling pointers.  gc_isPtr only compares
           the values, it never dereferences them. */
        for (i = 0; i < GARBAGE_LEN; i++)
            expect("a reclaimed block's payload", gc_isPtr(garbage[i]), NULL);
        expect("the chain head, still allocated", gc_isPtr(g_chain), g_chain);
        gc_free_table();

        section("M3: reclaimed space is reusable");
        /* Distinguishes 'the blocks left the table' from 'the blocks were
           genuinely handed back to the allocator'. */
        {
            void *reused = mm_calloc(1, NODE_BYTES);
            assert(reused != NULL);
            printf("allocated %p out of reclaimed space\n", reused);
            mm_free(reused);
        }
    }

    section("M3: the sweep did not corrupt the chain");
    {
        void *p = g_chain;
        int links = 0;
        while (p != NULL)
        {
            links++;
            p = *(void **)p;
        }
        printf("walked %d links from g_chain\n", links);
        assert(links == CHAIN_LEN);
    }

    section("M3: a cycle must terminate, not hang");
    {
        size_t mblocks;
        void *tail = g_chain;

        /* Walk to the end and point it back at the head.  Without
           mark-before-push in gc_push this does not fail an assertion —
           it loops forever. */
        while (*(void **)tail != NULL)
            tail = *(void **)tail;
        *(void **)tail = g_chain;

        gc_collect();
        gc_stats(&mblocks, NULL, NULL, NULL);
        printf("collected a cyclic chain, %zu blocks reachable\n", mblocks);
        assert(mblocks == CHAIN_LEN);

        *(void **)tail = NULL; /* undo, so checkheap sees a plain chain */
    }

    section("allocator invariants after a collection that freed");
    checkheap(0);
    printf("checkheap(0) reported nothing — survivors had their marks\n");
    printf("cleared, and the freed blocks coalesced cleanly\n");

    printf("\n=== M3 COMPLETE — MARK AND SWEEP WORKS ===\n");
    printf("\nCaveat: the root set is .data and .bss only.  A block reachable\n");
    printf("solely from a local variable is reclaimed while still live.  This\n");
    printf("test passes because it controls every reference it holds; real\n");
    printf("code does not.  M4 adds the stack, M5 the registers.\n");
    return 0;
}
