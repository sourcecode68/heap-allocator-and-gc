/* main_gc.c — test driver for the garbage collector.
 *
 * M0 — prove the collector links against the explicit allocator and can
 *      see where the heap lives.
 * M1 — the allocated-block table, and the pointer test built on it.
 */

#include <stdio.h>
#include <assert.h>
#include <stdint.h> /* uintptr_t, for hiding addresses from the collector */
#include "memlib.h"
#include "mm.h"
#include "gc.h"

#define NBLOCKS 10
#define CHAIN_LEN 5
#define GARBAGE_LEN 3
#define NODE_BYTES 16 /* a multiple of 8, so the payload has no padding */

/* g_chain - the global root the chain hangs from.
 *
 * It lands in .bss, one of the regions gc_collect scans.  Through M3 this
 * was the ONLY way to keep a block alive, because the stack was not a root
 * region; from M4 on, locals count too. */
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

/* ── Remembering an address without keeping it reachable ────────────
 *
 * From M4 on, the stack is a root region, so a test cannot simply keep a
 * pointer in a local and then assert the block was reclaimed — the local
 * itself is what keeps it alive.  Storing the address as an integer does
 * not help either: a conservative collector sees a bit pattern, and the
 * bit pattern is identical.
 *
 * XOR-ing with a constant changes the bits into something that fails the
 * pointer test, and the original comes back on demand.  Standard trick
 * for exactly this situation. */
#define HIDE(p) ((uintptr_t)(p) ^ 0xA5A5A5A5u)
#define UNHIDE(x) ((void *)((x) ^ 0xA5A5A5A5u))

/* Addresses of blocks the test expects to be UNREACHABLE.  Hidden, so
   these globals do not themselves keep the blocks alive. */
static uintptr_t g_hidden[GARBAGE_LEN];
static uintptr_t g_orphan;

/* make_garbage - allocate blocks and let every reference to them die with
   this frame.  Nothing in main ever holds one. */
/* ── Why every helper below is noinline ────────────────────────────
 *
 * These functions exist to OWN A STACK FRAME THAT DIES.  Each one puts a
 * heap address into its locals and then returns, so that clobber_stack can
 * overwrite the slot afterwards and the collector stops seeing it.
 *
 * That only works if the call is real.  They are static and called once or
 * twice, which is exactly the shape GCC inlines at -O2 — and an inlined
 * helper's locals land in the CALLER's frame, which is still live and still
 * scanned.  Measured: without noinline the test fails 15/15 at -O2 while
 * passing 40/40 at -O0, because a garbage block's address was sitting in
 * main's frame and the collector correctly refused to free it.
 *
 * The collector was right in that case; the test's isolation had silently
 * stopped existing.  noinline makes the call boundary load-bearing rather
 * than incidental.
 */
__attribute__((noinline)) static void make_garbage(void)
{
    size_t k;
    printf("garbage blocks (no reference):");
    for (k = 0; k < GARBAGE_LEN; k++)
    {
        void *p = mm_calloc(1, NODE_BYTES);
        assert(p != NULL);
        g_hidden[k] = HIDE(p);
        printf(" %p", p);
    }
    printf("\n");
    /* Printed from in here, not from main.  Passing these addresses to
       printf in main would put them in main's frame as call arguments,
       and main's frame is a root region — the act of displaying them
       would keep them alive.  This frame is about to die and be
       clobbered, so materialising them here is safe. */
}

/* make_orphan - allocate one block whose only reference is this
   function's local, then return.  M4's second done-when case. */
__attribute__((noinline)) static void make_orphan(void)
{
    void *p = mm_calloc(1, NODE_BYTES);
    assert(p != NULL);
    g_orphan = HIDE(p);
    printf("allocated %p inside a function that is about to return\n", p);
}

/* orphan_alive - is the orphaned block still in the heap?
 *
 * This runs in its own frame on purpose.  Answering the question means
 * un-hiding the address, and that value lands in the caller's frame as an
 * argument and a temporary.  Asked from main, the check itself would keep
 * the block alive on the NEXT collection — observing the thing changes it.
 * Here the evidence dies with this frame, and clobber_stack can erase it. */
__attribute__((noinline)) static int orphan_alive(void)
{
    int r;
    gc_build_table();
    r = (gc_isPtr(UNHIDE(g_orphan)) != NULL);
    gc_free_table();
    return r;
}

/* clobber_stack - overwrite the stack region a returned frame occupied.
 *
 * Returning does not erase a frame; its words sit there until something
 * reuses that memory.  Until then a dead local still holds a heap address
 * that the collector will find and honour — a stale stack slot.  This is
 * why M4's criterion says a block is reclaimed "eventually" rather than
 * on the next collection.  volatile so the writes are not optimised away. */
__attribute__((noinline)) static void clobber_stack(void)
{
    volatile char buf[768];
    size_t k;
    for (k = 0; k < sizeof buf; k++)
        buf[k] = 0;
}

/* run_m1_m3_setup - the M0/M1 tests, then free everything they allocated.
 *
 * Deliberately a separate function rather than inline in main.  From M4 on
 * the stack is a root region, and a block-scoped local does NOT release its
 * stack slot at the closing brace — the value sits there for the rest of
 * the enclosing function.  Run inline, this section leaves a dozen dead
 * pointers to freed M1 blocks scattered through main's frame.  The blocks
 * allocated later are carved out of that same memory and land on the same
 * addresses, so those dead locals mark them reachable and every count
 * afterwards is wrong.
 *
 * Giving them their own frame means they die on return, and clobber_stack
 * can then overwrite what they left behind.  clobber_stack cannot help with
 * main's own locals, because main's frame is still live.
 */
static void run_m1_m3_setup(void)
{
    const size_t sizes[NBLOCKS] = {1, 8, 16, 17, 24, 32, 40, 64, 100, 200};
    /* volatile: the clearing loop at the end of this function writes NULL
       over every element and nothing reads them afterwards, so at -O2 GCC
       discards those stores as dead — leaving ten live heap addresses on
       the stack for the rest of the program.  Measured: without volatile
       the test fails 15/15 at -O2 because blocks[5]'s address is still on
       the stack when a later block is allocated at that same address. */
    void *volatile blocks[NBLOCKS];
    size_t n, i;
    int local = 0; /* a stack object, for the stack-address case */

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

    /* Clear the array, now that the stack is a root region.  It still holds
       ten addresses of freed memory, and the blocks allocated next are
       carved out of exactly that memory — so those stale values would land
       inside them and mark them reachable.  The collector would be right
       and the test would be measuring nothing.  See the volatile above:
       these stores are dead code as far as the optimiser can tell. */
    for (i = 0; i < NBLOCKS; i++)
        blocks[i] = NULL;

    assert(gc_build_table() == 0);
    gc_free_table();
    printf("heap holds no allocated blocks\n");
}

/* test_local_survives - M4's first done-when case, in its own frame.
 *
 * Its own frame, because when this returns `kept` is a dead local holding
 * the address of a block that is about to be freed and its memory reused.
 * Left in main's frame that stale value would retain whatever gets
 * allocated there next — which is exactly what the following test then
 * tries to prove is unreachable. */
__attribute__((noinline)) static void test_local_survives(void)
{
    void *kept = mm_calloc(1, NODE_BYTES);
    assert(kept != NULL);
    printf("allocated %p, referenced only by a local\n\n", kept);

    gc_collect();

    /* gc_collect releases the table on its way out, and gc_isPtr answers
       NULL for everything when there is no table.  Rebuild before asking. */
    gc_build_table();
    expect("held only in a local, after a collection", gc_isPtr(kept), kept);
    gc_free_table();
    mm_free(kept);
}

/* run_m3_tests - the whole M3 phase, in its own frame.
 *
 * Every phase of this driver gets its own function now, for one reason:
 * from M4 on, main's frame is a root region that is scanned on every
 * collection, and a stack slot is never released — not at a closing
 * brace, not when a variable goes out of scope.  So any address a phase
 * materialises in main, even transiently as a function argument or a
 * compiler temporary, sits there for the rest of the program and retains
 * whatever is later allocated at that address.
 *
 * That bit this test three separate times: `freed` from M1d, `kept` from
 * the M4 survival case, and the UNHIDE calls in the checks below.  A
 * phase in its own frame leaves its debris where clobber_stack can reach
 * it; a phase inlined into main does not. */
__attribute__((noinline)) static void run_m3_tests(void)
{
    size_t i;

    section("M3: build a 5-block chain plus 3 unreferenced blocks");
    {
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

        /* The garbage blocks are allocated in a helper and never touched
           by main, so no local of main refers to them.  Then clobber the
           stack the helper used, or its dead locals still hold the
           addresses and the collector will honour them. */
        make_garbage();
        clobber_stack();

        printf("chain head (global g_chain) : %p\n", g_chain);

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
        /* These are dangling addresses now.  gc_isPtr only compares the
           values, it never dereferences them. */
        for (i = 0; i < GARBAGE_LEN; i++)
            expect("a reclaimed block's payload",
                   gc_isPtr(UNHIDE(g_hidden[i])), NULL);
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

    /* ═══════════════════════════════════════════════════════════════
     *  M4 — the two done-when cases for stack roots.
     * ═══════════════════════════════════════════════════════════════ */

}

int main(void)
{

    /* main declares nothing.  Every local here lives for the whole program
       and is scanned as a root on every collection, and a stack slot is
       never released — so one stale pointer silently retains a block for
       the rest of the run.  Each phase gets its own frame instead. */

    mem_init();                          /* create the heap                      */
    mm_init();                           /* prologue, epilogue, first free block */
    gc_init(__builtin_frame_address(0)); /* heap bounds + stack bottom          */

    section("M0: heap bounds");
    {
        char *lo = (char *)gc_heap_lo();
        char *hi = (char *)gc_heap_hi();
        printf("heap low       : %p\n", (void *)lo);
        printf("heap high      : %p   (last valid byte, inclusive)\n", (void *)hi);
        printf("heap size      : %ld bytes\n", (long)(hi - lo + 1));
        printf("sizeof(void *) : %zu bytes\n", sizeof(void *));
    }

    run_m1_m3_setup();
    clobber_stack();

    run_m3_tests();
    clobber_stack();

    section("M4: a block held only in a local survives");
    test_local_survives();
    clobber_stack(); /* erase the dead `kept` before the next test */

    section("M4: a block from a returned function is eventually reclaimed");
    {
        int rounds = 1;

        make_orphan(); /* its only reference dies with that frame */

        gc_collect();

        /* The frame is gone but its bytes are not.  A dead local still
           holding the address is a stale stack slot, and the collector
           cannot tell it from a live one, so the block may well survive
           this first collection.  That is why the criterion says
           "eventually" rather than "on the next collection". */
        if (orphan_alive())
        {
            printf("still alive after collection 1 -- a stale stack slot\n");
            printf("still holds the address.  Overwriting that memory...\n");
            clobber_stack();
            gc_collect();
            rounds++;
        }
        else
        {
            printf("already reclaimed on collection 1 -- the slot had been\n");
            printf("reused before the collector looked\n");
        }

        printf("  still reachable? %s   (after %d collection%s)\n",
               orphan_alive() ? "*** YES - FAIL ***" : "no",
               rounds, rounds == 1 ? "" : "s");
        assert(!orphan_alive());
    }

    section("allocator invariants after a collection that freed");
    checkheap(0);
    printf("checkheap(0) reported nothing — survivors had their marks\n");
    printf("cleared, and the freed blocks coalesced cleanly\n");

    printf("\n=== M4 COMPLETE — GLOBALS, HEAP AND STACK ARE ROOTS ===\n");
    printf("\nCaveat: CPU registers are still not roots.  A live heap pointer\n");
    printf("held only in a callee-saved register, with no copy anywhere in\n");
    printf("memory, is invisible to this collector and its block is freed.\n");
    printf("M5 spills them with setjmp and scans the buffer.\n");
    return 0;
}
