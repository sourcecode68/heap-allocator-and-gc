/* gc.c — conservative mark & sweep garbage collector.
 *
 * M1 — the pointer test.  Builds an address-sorted table of every
 * allocated block so that an arbitrary address can be resolved back to
 * the block containing it.  Nothing is marked and nothing is freed yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include "memlib.h"
#include "gc.h"

/* ══════════════════════════════════════════════════════════════════
 *  Boundary-tag layout
 *
 *  The allocator keeps HDRP / GET_SIZE / GET_ALLOC as file-private
 *  macros inside src/mm_explicit.c, and that file is frozen, so they
 *  cannot be exported.  What follows is gc.c's own copy of the same
 *  layout.  This is duplication, and it is deliberate.
 *
 *  Every use below only ever READS the heap.  The collector writes
 *  nothing until M2 claims the mark bit.
 * ══════════════════════════════════════════════════════════════════ */

#define WSIZE 4 /* header / footer size (bytes)        */
#define DSIZE 8 /* header + footer overhead per block  */

#define GET(p) (*(unsigned int *)(p))
#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp) ((char *)(bp) - WSIZE)
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))

/* ══════════════════════════════════════════════════════════════════
 *  Heap bounds
 * ══════════════════════════════════════════════════════════════════ */

/* gc_heap_start — first byte of the heap, cached by gc_init.
 *
 * Caching this is safe.  mem_init assigns memlib's mem_heap exactly once
 * and nothing afterwards ever changes it.
 *
 * Note there is deliberately NO cached high bound to match.  memlib's
 * mem_brk moves upward every time the allocator extends the heap, which
 * happens on any mm_malloc that cannot find a fit and inside mm_realloc's
 * grow-in-place path.  A stored high bound would go stale the moment the
 * program allocated past the first chunk, and a stale high bound makes the
 * collector reject pointers to blocks that are genuinely live.  gc_heap_hi
 * reads it live instead.
 */
static char *gc_heap_start; // first byte of heap needs +8 adjustment this points to the absolute starting of the heap

void gc_init(void)
{
    gc_heap_start = (char *)mem_heap_lo();
    gc_free_table();
}

void *gc_heap_lo(void)
{
    return gc_heap_start;
}

void *gc_heap_hi(void)
{
    return mem_heap_hi(); /* live read; this is mem_brk - 1 */
}

/* ══════════════════════════════════════════════════════════════════
 *  The block table
 *
 *  One entry per allocated block, in increasing address order.  It is
 *  scratch state, not a persistent index: built by a heap walk, read
 *  many times, then thrown away.  It is never updated in place, because
 *  the program cannot allocate or free while the collector is running.
 * ══════════════════════════════════════════════════════════════════ */

struct gc_block
{
    char *payload; /* address mm_malloc handed to the program   */
    size_t psize;  /* usable payload bytes (block size - DSIZE) */
};

static struct gc_block *gc_table;
static size_t gc_table_len;

/* gc_prologue - payload address of the prologue block.
 *
 * mm_init lays down a four-word preamble: alignment padding, prologue
 * header, prologue footer, epilogue header.  The prologue's payload
 * address is therefore the heap base plus two words.  The allocator
 * tracks the same address in heap_listp, but that is static to
 * mm_explicit.c, so gc.c recomputes it from the heap base.
 *
 * This is the one place gc.c assumes a specific preamble layout.  Both
 * allocators in this repo share it.  */
static char *gc_prologue(void)
{
    return (char *)gc_heap_lo() + DSIZE;
}

void gc_free_table(void)
{
    free(gc_table);
    gc_table = NULL;
    gc_table_len = 0;
}

size_t gc_table_count(void)
{
    return gc_table_len;
}

size_t gc_build_table(void)
{
    char *bp;
    size_t n = 0;
    size_t i = 0;

    gc_free_table();

    /* Pass 1 — count the allocated blocks.
       The walk starts one block PAST the prologue, so the prologue is
       never recorded: CLAUDE.md requires it never be treated as
       reachable.  (Its payload size is zero anyway, so it could never
       match an address, but leaving that to arithmetic would hide the
       intent.)  The walk stops at the epilogue, whose size is 0.  */
    for (bp = NEXT_BLKP(gc_prologue()); GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
        if (GET_ALLOC(HDRP(bp)))
            n++;

    if (n == 0)
        return 0;

    /* Storage comes from libc's malloc, NOT from mm_malloc.  Allocating
       out of the heap being catalogued would change the very thing we
       are snapshotting, and could push mem_brk mid-collection.  It also
       keeps the table out of .data/.bss, which M2 scans for roots — a
       table of block addresses sitting in the root set would mark every
       block in the heap as reachable.  */
    gc_table = malloc(n * sizeof *gc_table); // malloc gives payload aligned to max align always so no need to worry
    if (gc_table == NULL)
        return 0;

    /* Pass 2 — record them.  NEXT_BLKP advances by the block size, so
       this visits blocks in strictly increasing address order and the
       table comes out sorted with no sort step.  */
    for (bp = NEXT_BLKP(gc_prologue()); GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
        if (GET_ALLOC(HDRP(bp)) && i < n)
        {
            gc_table[i].payload = bp;
            gc_table[i].psize = GET_SIZE(HDRP(bp)) - DSIZE;
            i++;
        }

    /* Record what was actually written, not what pass 1 predicted.  The
       i < n guard above stops the loop overrunning the table; using i
       here closes the mirror case, where pass 2 finds FEWER blocks than
       pass 1 and the tail would otherwise be claimed but uninitialised. */
    gc_table_len = i;
    return i;
}

void gc_table_dump(void)
{
    size_t i;

    printf("block table: %zu allocated block%s\n",
           gc_table_len, (gc_table_len == 1) ? "" : "s");

    for (i = 0; i < gc_table_len; i++)
    {
        char *p = gc_table[i].payload;
        size_t s = gc_table[i].psize;
        printf("  [%2zu] payload %p  psize %4zu  accepts %p .. %p\n",
               i, (void *)p, s, (void *)p, (void *)(p + s - 1));
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  The pointer test
 * ══════════════════════════════════════════════════════════════════ */

/* gc_isPtr (candidate)
 *
 * Decide whether CANDIDATE — an arbitrary word read out of a root region
 * or out of some block's payload — points anywhere inside the payload of
 * an allocated block, and if so return that block's payload start.
 *
 * Two stages, cheapest first.
 *
 * Stage 1 is a heap bounds check.  Almost every word the collector will
 * ever examine is not a heap address at all, so this rejection carries
 * the common case and the search below never runs on it.  gc_heap_hi is
 * the last valid byte rather than one past the end, hence <=.
 *
 * Stage 2 searches for the first table entry whose payload begins
 * strictly ABOVE the candidate, then steps back one.  Since the table is
 * in increasing address order, that predecessor is the only block that
 * could possibly contain the candidate.  Nothing is being matched
 * exactly here: an interior pointer appears in no entry, which is why
 * the search looks for a bound rather than for equality.
 *
 * The closing range check is doing double duty.  Besides settling
 * interior pointers, it is what rejects free blocks: a candidate inside
 * a free block searches back to whatever allocated block precedes it and
 * then falls outside that block's payload.  Free blocks are excluded by
 * construction rather than by a special case — which matters, because a
 * free block's first two payload words hold the explicit free list's
 * PRED and SUCC, and those are real heap addresses that a conservative
 * collector would otherwise follow.
 *
 * Headers and footers lie outside every block, because entries record
 * the payload size and the accepted range stops short of the footer.
 */
void *gc_isPtr(void *candidate)
{
    char *cand = (char *)candidate;
    struct gc_block *e;
    size_t lo, hi;

    if (gc_table_len == 0)
        return NULL;

    /* Stage 1 — is it even inside the heap? */
    if (cand < (char *)gc_heap_lo() || cand > (char *)gc_heap_hi())
        return NULL;

    /* Stage 2 — first entry starting strictly above cand.
       lo + (hi - lo) / 2 rather than (lo + hi) / 2: the overflow it
       avoids is unreachable in a 4 GB address space, but the habit is
       worth more than the two characters it costs. */
    lo = 0;
    hi = gc_table_len;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (gc_table[mid].payload <= cand)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo == 0)
        return NULL; /* cand sits below every recorded block */

    e = &gc_table[lo - 1];
    if (cand < e->payload + e->psize)
        return e->payload;

    return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 *  Collection
 * ══════════════════════════════════════════════════════════════════ */

void gc_collect(void)
{
    /* Nothing to collect yet.  Marking arrives in M2, sweeping in M3. */
}
