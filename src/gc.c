/* gc.c — conservative mark & sweep garbage collector.
 *
 * M1 — the pointer test.  Builds an address-sorted table of every
 * allocated block so that an arbitrary address can be resolved back to
 * the block containing it.  Nothing is marked and nothing is freed yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> /* uintptr_t, for aligning scan candidates */
#include "memlib.h"
#include "gc.h"
#include "mm.h"

/* ══════════════════════════════════════════════════════════════════
 *  Boundary-tag layout
 *
 *  The allocator keeps HDRP / GET_SIZE / GET_ALLOC as file-private
 *  macros inside src/mm_explicit.c, and that file is frozen, so they
 *  cannot be exported.  What follows is gc.c's own copy of the same
 *  layout.  This is duplication, and it is deliberate.
 *
 *  Reads use GET; the single write the collector makes is the mark bit,
 *  via PUT below.
 * ══════════════════════════════════════════════════════════════════ */

#define WSIZE 4 /* header / footer size (bytes)        */
#define DSIZE 8 /* header + footer overhead per block  */

#define GET(p) (*(unsigned int *)(p))
#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp) ((char *)(bp) - WSIZE)
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))

/* ── The mark bit ──────────────────────────────────────────────────
 *
 * PUT is the collector's only write into the heap.  Everything else
 * here reads.
 *
 * Bit 1 of the header is the mark bit.  It is free because every block
 * size is a multiple of 8, which forces the low three bits of the size
 * field to zero, and bit 0 is already spoken for as the allocated flag.
 *
 * Setting it is invisible to the allocator: GET_SIZE masks off ~0x7 and
 * GET_ALLOC masks off 0x1, so neither ever sees bit 1.  It is NOT
 * invisible to checkblock, which compares the raw header and footer
 * words — which is why gc_collect clears every mark before returning.
 * The two copies disagree only inside a collection, and nothing can
 * look at the heap during one.
 */
#define PUT(p, val) (*(unsigned int *)(p) = (val))
#define GET_MARK(p) ((GET(p) & 0x2) != 0)
#define SET_MARK(p) PUT(p, GET(p) | 0x2)
#define CLR_MARK(p) PUT(p, GET(p) & ~0x2)

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
/* gc_lookup carries the work; gc_isPtr is the public face of it.  The
   split exists because marking needs the block's payload SIZE to know
   how far to scan, and gc_isPtr's signature only hands back the start. */
static struct gc_block *gc_lookup(char *cand)
{
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
        return e;

    return NULL;
}
void *gc_isPtr(void *candidate)
{
    struct gc_block *e = gc_lookup((char *)candidate);
    return e ? e->payload : NULL;
}
/*══════════════════════════════════════════════════════════════════
 * Marking
 *
 *  Reachability is computed with an explicit worklist, never recursion.
 *  A chain of N blocks would otherwise cost N stack frames, and nothing
 *  bounds N but the size of the heap.
 * ══════════════════════════════════════════════════════════════════ */

/* Blocks that are marked but whose payloads have not been scanned yet.
   The list holds table entries rather than bare addresses so that each
   block's payload size travels with it.

   Storage comes from libc malloc, for the same two reasons as the table:
   allocating out of the heap under collection would disturb the snapshot
   just taken, and an array of block addresses sitting in .bss would be
   scanned as a root region and mark the entire heap reachable.

   It cannot overflow.  gc_push pushes only a block whose mark bit was
   clear, and sets the bit before pushing, so a block enters at most once
   and the list never exceeds gc_table_len entries. */
static struct gc_block **gc_worklist;
static size_t gc_work_top;

/* gc_push - mark a block and queue its payload for scanning.
 *
 * Setting the bit BEFORE the push is what bounds the worklist, and it is
 * equally what terminates a cyclic structure: if block A holds a pointer
 * to B and B holds one back to A, the second visit finds the bit already
 * set and goes no further. */
static void gc_push(struct gc_block *e)
{
    char *hdr = HDRP(e->payload);

    if (GET_MARK(hdr))
        return;

    SET_MARK(hdr);

    gc_worklist[gc_work_top++] = e;
}

/* gc_mark_range (lo, hi)
 *
 * Treat every aligned word in [LO, HI) as a candidate pointer and mark
 * whatever it resolves to.  Used for both root regions and block
 * payloads: the collector cannot tell those apart, and does not need to.
 *
 * Candidates step 4 bytes at a time and are aligned to 4, because a
 * pointer is 4 bytes on i386 and the compiler will not place one at an
 * unaligned address.  Note that 4 is the POINTER alignment, not the
 * 8-byte alignment of block payloads; conflating the two is how half the
 * roots get missed. */
static void gc_mark_range(char *lo, char *hi)
{
    char *p = (char *)(((uintptr_t)lo + 3u) & ~(uintptr_t)3u); /* align up */
    // p is always an address never a value we get value by dereferencing
    for (; p + WSIZE <= hi; p += WSIZE)
    {
        // so now i have to read 4B value starting from the address p and treat is as an adddress to a character
        struct gc_block *e = gc_lookup((char *)GET(p));
        // struct gc_block*e=gc_lookup(*(char**)p); treat p as holding a pointer to a char * so dereffing gives me the 4 byte pointer to char * note that p still holds an address

        if (e != NULL)
        {
            gc_push(e);
        }
    }
}
/* gc_drain - scan queued payloads until nothing is left to scan.
 *
 * Each payload scanned may queue more blocks, so this runs until the
 * worklist empties rather than for any fixed number of rounds. */
static void gc_drain(void)
{
    while (gc_work_top > 0)
    {
        struct gc_block *e = gc_worklist[--gc_work_top];
        gc_mark_range(e->payload, e->payload + e->psize);
    }
}

/* The separate clear pass that lived here through M2 is gone: gc_sweep
   now does both of its jobs.  Survivors get an explicit CLR_MARK, and
   freed blocks clear themselves, because mm_free writes
   PACK(GET_SIZE(...), 0) and GET_SIZE masks off ~0x7.  M6's mark-only
   mode will want something like it back. */

/* ══════════════════════════════════════════════════════════════════
 *  Collection
 * ══════════════════════════════════════════════════════════════════ */
/* The root set, M2 edition: the executable's own .data and .bss.
 *
 * These four symbols are placed by the GNU linker; no object file
 * defines them.  They are declared as ARRAYS deliberately — a linker
 * boundary symbol has no storage of its own, so what you want is the
 * address the name sits at, and an array name decays to exactly that.
 * Declaring them `extern char *__data_start` instead would read the four
 * bytes stored AT that address and scan a garbage range, silently.
 *
 * Scanned as two ranges rather than one merged span: .data and .bss are
 * adjacent in this binary, but that is not guaranteed.
 *
 * The stack and the registers are not roots yet.  M4 and M5.
 */
extern char __data_start[], _edata[]; /* .data */
extern char __bss_start[], _end[];    /* .bss  */

/* Statistics from the most recent collection.
 *
 * gc_report cannot count mark bits for itself, because gc_collect clears
 * every mark before returning.  So the counting happens inside the
 * collection — after draining, before clearing — and the numbers are
 * parked here for gc_report to print afterwards.
 *
 * These live in .bss, which the NEXT collection scans as a root region.
 * They are small counts, far below the heap's address range, so
 * gc_lookup's bounds check rejects them.  Worth being aware of rather
 * than lucky about: anything parked in .bss becomes a root candidate. */
static size_t gc_marked_blocks;
static size_t gc_marked_bytes;
static size_t gc_total_blocks;
static size_t gc_total_bytes;

/* gc_record_stats - tally marked against allocated while the bits are
   still set.  Payload bytes rather than block bytes: the boundary tags
   are overhead the program never sees. */
static void gc_record_stats(void)
{
    size_t i;

    gc_marked_blocks = 0;
    gc_marked_bytes = 0;
    gc_total_blocks = gc_table_len;
    gc_total_bytes = 0;

    for (i = 0; i < gc_table_len; i++)
    {
        gc_total_bytes += gc_table[i].psize;
        if (GET_MARK(HDRP(gc_table[i].payload)))
        {
            gc_marked_blocks++;
            gc_marked_bytes += gc_table[i].psize;
        }
    }
}

void gc_stats(size_t *marked_blocks, size_t *marked_bytes,
              size_t *total_blocks, size_t *total_bytes)
{
    if (marked_blocks)
        *marked_blocks = gc_marked_blocks;
    if (marked_bytes)
        *marked_bytes = gc_marked_bytes;
    if (total_blocks)
        *total_blocks = gc_total_blocks;
    if (total_bytes)
        *total_bytes = gc_total_bytes;
}

void gc_report(void)
{
    printf("gc: reachable   %zu/%zu blocks, %zu/%zu payload bytes\n",
           gc_marked_blocks, gc_total_blocks,
           gc_marked_bytes, gc_total_bytes);
    printf("gc: reclaimed   %zu blocks, %zu payload bytes\n",
           gc_total_blocks - gc_marked_blocks,
           gc_total_bytes - gc_marked_bytes);
}

/* Defined below gc_collect, so it needs a prototype here.  Kept static:
   sweeping alone would free every block in the heap. */
static void gc_sweep(void);

/* gc_collect - one full mark and sweep.
 *
 * The phase order is forced, not chosen:
 *
 *   1. Snapshot the heap.  Everything downstream reads this table, and
 *      it stays valid only because nothing else can run during a
 *      collection — single-threaded, no signal handlers.
 *   2. Size the worklist from that snapshot.  It cannot overflow: a
 *      block is pushed only when its mark bit was clear, and the bit is
 *      set before the push, so each block enters at most once.
 *   3. Scan the roots, marking and queueing whatever the globals point
 *      at directly.
 *   4. Drain.  This is what makes reachability transitive; without it
 *      only the first block of a chain is ever marked.
 *   5. Count, while the bits are still set — the sweep destroys them.
 *   6. Sweep: free every unmarked block, clear the mark on the rest.  By
 *      the time this returns no header carries a mark, so header and
 *      footer agree again and checkheap is meaningful.
 *
 * The root set is .data and .bss only.  A block reachable solely from a
 * local variable WILL be reclaimed while it is still live.  That is not a
 * bug in the sweep; it is the missing half of the root set, and it is
 * what M4 and M5 exist to fix.
 */
void gc_collect(void)
{
    size_t n = gc_build_table();

    /* 0 means no usable table — an empty heap, or a failed allocation.
       Both mean the same thing here: do not proceed.  Checking it first
       also keeps the malloc below from ever seeing a size of zero, which
       may legitimately return NULL and would look like a failure that
       never actually happened. */
    if (n == 0)
        return;

    gc_worklist = malloc(n * sizeof(*gc_worklist));
    if (gc_worklist == NULL)
    {
        gc_free_table(); /* release the snapshot we are abandoning */
        return;
    }
    /* Reset explicitly rather than trusting the previous drain to have
       finished: a collection that bailed out early leaves this dirty. */
    gc_work_top = 0;

    gc_mark_range(__data_start, _edata);
    gc_mark_range(__bss_start, _end);
    gc_drain();

    gc_record_stats();
    gc_sweep();

    free(gc_worklist);
    gc_worklist = NULL;
    gc_work_top = 0;
    gc_free_table();
}

//
static void gc_sweep(void)
{
    char *bp = gc_prologue();
    for (bp = NEXT_BLKP(bp); GET_SIZE(HDRP(bp)) > 0;)
    {
        if (GET_ALLOC(HDRP(bp)))
        {
            if (!GET_MARK(HDRP(bp)))
            {
                char *temp = NEXT_BLKP(bp);
                while (!GET_ALLOC(HDRP(temp)))
                    temp = NEXT_BLKP(temp);
                mm_free(bp);
                bp = temp;
            }
            else
            {
                CLR_MARK(HDRP(bp));
                bp = NEXT_BLKP(bp);
            }
        }
        else
            bp = NEXT_BLKP(bp);
    }
}