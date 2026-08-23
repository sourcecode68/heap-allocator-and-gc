/* mm.c — Explicit free-list dynamic memory allocator
 *
 * DESCRIPTION
 *   This file implements a general purpose dynamic storage allocator
 *   built on top of the mem_sbrk / mem_heap_* primitives provided by
 *   memlib.  The design follows the classic boundary tag scheme described
 *   extended with an explicit doubly linked free list for O(1) free block discovery.
 *
 * BLOCK STRUCTURE
 *   Every block free or allocated is bracketed by a one word (4-byte)
 *   header and a one word footer.  Both words encode the same information:
 *   the total block size (including the header and footer) in the upper
 *   29 bits, and an allocation flag in bit 0 (1 = allocated, 0 = free).
 *
 *   Allocated block:
 *     ┌─────────┬──────────────────────────────┬─────────┐
 *     │ hdr (4) │           payload            │ ftr (4) │
 *     └─────────┴──────────────────────────────┴─────────┘
 *               ^bp   (pointer returned to caller)
 *
 *   Free block (minimum size 16 bytes):
 *     ┌─────────┬──────────┬──────────┬─────────────────┬─────────┐
 *     │ hdr (4) │ pred (4) │ succ (4) │    (padding)    │ ftr (4) │
 *     └─────────┴──────────┴──────────┴─────────────────┴─────────┘
 *               ^bp
 *
 *   The predecessor (PRED) and successor (SUCC) pointer words occupy
 *   the first two payload words of every free block and are used
 *   exclusively to link the explicit free list.  They are meaningless
 *   while a block is allocated.
 *
 * HEAP LAYOUT
 *   The heap begins with a four-word preamble laid down by mm_init:
 *
 *     +──────────────+──────────────+──────────────+──── ... ──────────+
 *     │ pad (4)      │ pro hdr (4)  │ pro ftr (4)  │  Epilogue(4)   │
 *     │              │   [8 | 1]    │   [8 | 1]    │    [0 | 1]     │
 *     +──────────────+──────────────+──────────────+──── ... ──────────+
 *                                   ^heap_listp
 *
 *   The prologue is an 8-byte allocated block whose sole purpose is to
 *   give every regular block a non-NULL physical predecessor, eliminating
 *   edge cases in PREV_BLKP and coalesce.  The heap ends with a zero-size
 *   allocated epilogue header [0 | 1] that terminates heap-traversal loops.
 *
 * FREE LIST POLICY
 *   Free blocks are linked in an unsorted, doubly-linked list head at
 *   free_listp.  New entries are always prepended to the head (LIFO
 *   discipline).  Allocation searches the list from the head using a
 *   first-fit strategy.  Coalescing is performed eagerly: whenever a
 *   block is freed or a split remainder is created, adjacent free blocks
 *   are merged immediately.
 *
 * ALIGNMENT
 *   All payload pointers are aligned to DSIZE (8) bytes.  The minimum
 *   allocatable block size is MIN_BLKSIZE (16 bytes): 4 (header) +
 *   4 (predecessor) + 4 (successor) + 4 (footer).
 *
 * PERFORMANCE NOTES
 *   - malloc:   O(free blocks) first-fit search over free list, O(1) insertion.
 *   - free:     O(1) list insertion + O(1) boundary-tag coalescing.
 *   - realloc:  avoids copying when the block can shrink in place or can
 *               absorb a free physical successor.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "memlib.h"

/* ──────────────────────── Basic constants ──────────────────────── */

/* WSIZE — size in bytes of a single word, a block header, or a block footer.
   All size fields and pointer slots inside free blocks are one word wide.  */
#define WSIZE 4 /* Word / header / footer size (bytes) */

/* DSIZE — size of a double word (two consecutive WSIZE words).
   Used as the basic alignment unit: every payload pointer returned by
   mm_malloc is aligned to a multiple of DSIZE.  */
#define DSIZE 8 /* Double word size (bytes) */

/* CHUNKSIZE — the default number of bytes by which the heap is extended
   when no free block is large enough to satisfy a request.  Chosen as a
   power of two so that mem_sbrk requests align naturally with typical
   OS page sizes.  */
#define CHUNKSIZE (1 << 12) /* Extend heap by this amount (bytes)  */

/* MIN_BLKSIZE — the smallest valid block size.  A free block must hold
   a header (WSIZE), a predecessor pointer (WSIZE), a successor pointer
   (WSIZE), and a footer (WSIZE), for a total of 2 * DSIZE bytes.
   Allocated blocks may be as small as MIN_BLKSIZE as well, since that
   is the minimum size produced by the size-rounding in mm_malloc.  */
#define MIN_BLKSIZE (2 * DSIZE) /* Minimum block size = 16 bytes       */

/* MAX — evaluate to the larger of X and Y.
   Both arguments may be evaluated more than once; avoid side effects.  */
#define MAX(x, y) ((x) > (y) ? (x) : (y))

/* ──────────────────────── Header / footer word encoding ──────────── */

/* PACK — combine a block size and an allocation flag into a single header
   or footer word.  SIZE must already be aligned and must have its low
   three bits clear; ALLOC must be 0 or 1.  */
#define PACK(size, alloc) ((size) | (alloc))

/* ──────────────────────── Raw memory access ─────────────────────── */

/* GET — read an unsigned 32-bit word from address P.
   P need not be DSIZE-aligned; WSIZE alignment is assumed.  */
#define GET(p) (*(unsigned int *)(p))

/* PUT — write the unsigned 32-bit value VAL to address P.  */
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/* ──────────────────────── Header / footer field access ──────────── */

/* GET_SIZE — extract the block size (in bytes) from the header or footer
   word at P.  The low three bits are masked off because they carry flags;
   only bit 0 (the allocation bit) is currently used.  */
#define GET_SIZE(p) (GET(p) & ~0x7) // min block size is 16 which means last 3 bits are 0 even 8 ensures it

/* GET_ALLOC — extract the allocation flag from the header or footer word
   at P.  Returns 1 if the block is allocated, 0 if it is free.  */
#define GET_ALLOC(p) (GET(p) & 0x1)

/* ──────────────────────── Block boundary macros ─────────────────── */

/* HDRP — compute the address of the header word of the block whose
   payload starts at BP.  The header immediately precedes the payload.  */
#define HDRP(bp) ((char *)(bp) - WSIZE)

/* FTRP — compute the address of the footer word of the block whose
   payload starts at BP.  The footer is the last word of the block,
   positioned GET_SIZE(HDRP(bp)) - DSIZE bytes past BP.  */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* ──────────────────────── Heap-level block navigation ───────────── */

/* NEXT_BLKP — compute the payload address of the block that immediately
   follows BP in the heap (physical successor).  */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))

/* PREV_BLKP — compute the payload address of the block that immediately
   precedes BP in the heap (physical predecessor).  The size of the
   predecessor is read from its footer, which sits in the word just
   before BP's header.  */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))

/* ──────────────────────── Free-list pointer macros ──────────────── */

/*
 * Free blocks store two 4-byte pointers in their first two payload words.
 * PRED and SUCC yield the *address* of those pointer slots; GET_PRED and
 * GET_SUCC dereference the slots to obtain the pointed-to block address.
 *
 *   PRED(bp)     — address of the predecessor-pointer slot in block BP.
 *                  Holds the address of the previous block in the free list,
 *                  or NULL if BP is the list head.
 *   SUCC(bp)     — address of the successor-pointer slot in block BP.
 *                  Holds the address of the next block in the free list,
 *                  or NULL if BP is the list tail.
 *   GET_PRED(bp) — dereference PRED(bp), returning a (char *).
 *   GET_SUCC(bp) — dereference SUCC(bp), returning a (char *).
 */
#define PRED(bp) ((char *)(bp))
#define SUCC(bp) ((char *)(bp) + WSIZE)
#define GET_PRED(bp) ((char *)GET(PRED(bp)))
#define GET_SUCC(bp) ((char *)GET(SUCC(bp)))

/* ──────────────────────── Globals ──────────────────────── */

/* heap_listp — payload address of the prologue block.
   Serves as the fixed starting point for heap traversal; never modified
   after mm_init completes.  */
static char *heap_listp; /* Always points to prologue block payload  */

/* free_listp — head of the explicit free list, or NULL when the list is
   empty.  Updated by insert_free and remove_free; reads during find_fit.
   All list traversals begin here.  */
static char *free_listp; /* Head of the explicit free list (or NULL) */

/* ──────────────────────── Prototypes ──────────────────────── */
void checkheap(int verbose);
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void insert_free(void *bp);
static void remove_free(void *bp);
static void printblock(void *bp);
static void checkblock(void *bp);

/* ══════════════════════════════════════════════════════════════════
 *  Free-list helpers
 * ══════════════════════════════════════════════════════════════════ */

/* insert_free (bp)
 *
 * Prepend the free block at BP to the head of the explicit free list
 * (LIFO insertion order).  After this call:
 *
 *   - BP's SUCC slot points to the old list head (free_listp).
 *   - BP's PRED slot is set to NULL, marking it as the new head.
 *   - If the old head was non-NULL, its PRED slot is updated to BP.
 *   - free_listp is advanced to BP.
 *
 * BP must already have its header and footer written as a free block
 * before insert_free is called.  The function does not inspect or
 * modify the block's size or allocation bits.  */
static void insert_free(void *bp)
{
    /* bp -> old head */
    PUT(SUCC(bp), (unsigned int)free_listp);
    /* bp has no predecessor */
    PUT(PRED(bp), (unsigned int)NULL);

    if (free_listp != NULL)
        PUT(PRED(free_listp), (unsigned int)bp);

    free_listp = bp;
}

/* remove_free (bp)
 *
 * Unlink the free block at BP from the explicit free list, splicing its
 * predecessor and successor together.  Four pointer words may be modified:
 *
 *   - If BP has a predecessor, that block's SUCC slot is set to BP's
 *     successor.  Otherwise (BP is the head), free_listp is updated to
 *     BP's successor.
 *   - If BP has a successor, that block's PRED slot is set to BP's
 *     predecessor.
 *
 * BP's own PRED and SUCC slots are left intact; callers that reassign
 * or deallocate BP after this call need not clear them.  Calling
 * remove_free on a block that is not currently in the free list
 * produces undefined behaviour.  */
static void remove_free(void *bp)
{
    char *pred = GET_PRED(bp);
    char *succ = GET_SUCC(bp);

    if (pred != NULL)
        PUT(SUCC(pred), (unsigned int)succ); /* pred->next = succ */
    else
        free_listp = succ; /* bp was the head   */

    if (succ != NULL)
        PUT(PRED(succ), (unsigned int)pred); /* succ->prev = pred  */
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_init
 * ══════════════════════════════════════════════════════════════════ */

/* mm_init (void)
 *
 * Initialize the memory allocator.  Must be called exactly once before
 * any other mm_* function is invoked.  Performs the following steps:
 *
 *  1. Resets free_listp to NULL (empty free list).
 *  2. Allocates four words from mem_sbrk to form the initial heap
 *     preamble: an alignment padding word, a prologue header, a
 *     prologue footer, and an epilogue header.
 *  3. Advances heap_listp to point at the prologue block's payload.
 *  4. Extends the heap by CHUNKSIZE bytes to create an initial free
 *     block large enough for typical early allocations.
 *
 * Returns  0 on success.
 * Returns -1 if mem_sbrk fails at either step 2 or step 4.  */
int mm_init(void)
{
    free_listp = NULL;

    /* Allocate: padding + prologue hdr + prologue ftr + epilogue hdr */
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
        return -1;

    PUT(heap_listp, 0);                            /* Alignment padding  */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /* Prologue header    */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /* Prologue footer    */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));     /* Epilogue header    */
    heap_listp += (2 * WSIZE);                     /* bp of prologue     */

    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
        return -1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  extend_heap
 * ══════════════════════════════════════════════════════════════════ */

/* extend_heap (words)
 *
 * Grow the heap by WORDS words, rounding up to the nearest even number
 * of words to maintain double-word alignment.  The newly acquired memory
 * is formatted as a single free block and prepended to the free list;
 * coalesce is then called to merge it with any adjacent free block that
 * immediately precedes it in the heap (which can happen when the previous
 * epilogue was immediately preceded by a free block).
 *
 * The new epilogue header [0 | 1] is written immediately after the new
 * free block, replacing the old epilogue.
 *
 * WORDS  — requested growth in words; rounded up to the next even value
 *           so the resulting block size is a multiple of DSIZE.
 *
 * Returns a pointer to the coalesced free block on success.
 * Returns NULL if mem_sbrk fails.  */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* Round up to maintain double-word alignment */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    if ((long)(bp = mem_sbrk(size)) == -1)
        return NULL;

    /* New free block */
    PUT(HDRP(bp), PACK(size, 0));         /* Free block header  */
    PUT(FTRP(bp), PACK(size, 0));         /* Free block footer  */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* New epilogue       */

    /* Insert into free list before coalescing */
    insert_free(bp);
    return coalesce(bp);
}

/* ══════════════════════════════════════════════════════════════════
 *  coalesce
 *
 *  bp must already be in the free list (inserted before this call).
 *  Adjacent free neighbours are removed from the list, the blocks
 *  are merged, and the merged block takes bp's list slot (or the
 *  neighbour's if bp was not the head).
 * ══════════════════════════════════════════════════════════════════ */

/* coalesce (bp)
 *
 * Merge the free block at BP with any physically adjacent free blocks,
 * maintaining the explicit free list invariants throughout.
 *
 * PRECONDITION: BP must have been inserted into the free list (via
 * insert_free) before coalesce is called.  BP's header and footer must
 * reflect its current size and free status.
 *
 * The function examines the allocation bits of BP's physical predecessor
 * (via its footer) and physical successor (via its header) and handles
 * four cases:
 *
 *   Case 1 — Both neighbours are allocated.
 *     No merging is possible.  BP is returned unchanged.
 *
 *   Case 2 — Only the physical successor is free.
 *     The successor is removed from the free list; BP is also removed
 *     and then re-inserted so it becomes the new list head.  BP's header
 *     and the successor's footer are rewritten with the combined size.
 *
 *   Case 3 — Only the physical predecessor is free.
 *     The predecessor and BP are both removed from the free list; the
 *     predecessor is then re-inserted as the new head.  The predecessor's
 *     header and BP's footer are rewritten with the combined size.
 *     The predecessor's address is returned.
 *
 *   Case 4 — Both neighbours are free.
 *     The predecessor, successor, and BP are all removed.  The predecessor
 *     is re-inserted as the new head.  The predecessor's header and
 *     successor's footer are rewritten with the combined size.
 *     The predecessor's address is returned.
 *
 * In all cases the block that remains in (or re-enters) the free list
 * is moved to the head, consistent with the LIFO discipline.
 *
 * Returns a pointer to the payload of the merged free block.  */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    /* Case 1 — both neighbours allocated, nothing to merge */
    if (prev_alloc && next_alloc)
        return bp;

    /* Case 2 — next block is free */
    if (prev_alloc && !next_alloc)
    {
        remove_free(NEXT_BLKP(bp)); /* remove next from list       */
        remove_free(bp);            /* remove bp   from list       */
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0)); /* FTRP now reaches new footer */
        insert_free(bp);              /* re-insert merged block      */
        return bp;
    }

    /* Case 3 — previous block is free */
    if (!prev_alloc && next_alloc)
    {
        remove_free(PREV_BLKP(bp)); /* remove prev from list */
        remove_free(bp);            /* remove bp   from list */
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free(bp);
        return bp;
    }

    /* Case 4 — both neighbours are free */
    if (!prev_alloc && !next_alloc)
    {
        remove_free(PREV_BLKP(bp));
        remove_free(NEXT_BLKP(bp));
        remove_free(bp);
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
                GET_SIZE(FTRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free(bp);
    }

    return bp;
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_free
 * ══════════════════════════════════════════════════════════════════ */

/* mm_free (bp)
 *
 * Release the allocated block whose payload starts at BP, making it
 * available for future allocation requests.  Follows these steps:
 *
 *  1. Silently ignores NULL; freeing a NULL pointer is well-defined.
 *  2. When compiled with -DDEBUG, checks the allocation bit to detect
 *     a double-free; if the block is already free a diagnostic is printed
 *     to stderr and the function returns without further action.
 *  3. Clears the allocation bit in the block's header and footer.
 *  4. Prepends the block to the free list via insert_free.
 *  5. Merges with any adjacent free blocks via coalesce.
 *
 * BP must be a pointer that was previously returned by mm_malloc,
 * mm_realloc, or mm_calloc and has not already been freed.  Passing
 * any other pointer produces undefined behaviour.  */
void mm_free(void *bp)
{
    if (bp == NULL)
        return;

#ifdef DEBUG
    if (!GET_ALLOC(HDRP(bp)))
    {
        fprintf(stderr,
                "Double free detected: %p\n",
                bp);
        return;
    }
#endif
    size_t size = GET_SIZE(HDRP(bp));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    insert_free(bp); /* add to free list BEFORE coalescing */
    coalesce(bp);
}

/* ══════════════════════════════════════════════════════════════════
 *  find_fit  (first-fit over the explicit free list)
 * ══════════════════════════════════════════════════════════════════ */

/* find_fit (asize)
 *
 * Search the explicit free list for the first block whose size is at
 * least ASIZE bytes.  Traversal starts at free_listp (the most recently
 * freed or coalesced block) and follows SUCC pointers until a suitable
 * block is found or the end of the list is reached.
 *
 * The first-fit policy tends to produce good utilisation for workloads
 * with a mix of small and large requests, because large free blocks near
 * the tail are preserved for future large allocations.
 * First fit also benefits for the temporal cache locality of recently freed blocks, which are recently freed.
 *
 *
 * ASIZE — the adjusted (aligned) number of bytes required, including
 *          the header and footer overhead.
 *
 * Returns a pointer to the payload of the first sufficiently large free
 * block, or NULL if no such block exists in the free list.  */
static void *find_fit(size_t asize)
{
    for (char *bp = free_listp; bp != NULL; bp = GET_SUCC(bp))
    {
        if (GET_SIZE(HDRP(bp)) >= asize)
            return bp;
    }
    return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 *  place
 * ══════════════════════════════════════════════════════════════════ */

/* place (bp, asize)
 *
 * Mark the free block at BP as allocated and split it if the remainder
 * is large enough to form a valid free block.  BP must currently reside
 * in the free list; it is removed unconditionally at the start.
 *
 * Splitting behaviour:
 *   If the difference between the current block size CSIZE and the
 *   requested size ASIZE is at least MIN_BLKSIZE, the block is split:
 *
 *     [  ASIZE bytes (allocated)  |  CSIZE−ASIZE bytes (free)  ]
 *
 *   The front portion is marked allocated (header + footer written with
 *   ASIZE and alloc=1).  The remainder is formatted as a new free block
 *   (header + footer written with CSIZE−ASIZE and alloc=0) and prepended
 *   to the free list via insert_free.  No coalescing is attempted on the
 *   remainder because the block being placed was already the result of a
 *   search; its physical successor cannot be free (it was already checked
 *   by find_fit or would have been coalesced earlier).
 *
 *   If (CSIZE − ASIZE) < MIN_BLKSIZE, the entire block is used as-is to
 *   avoid creating an unusably small fragment.  Internal fragmentation of
 *   up to MIN_BLKSIZE − 1 bytes may result.
 *
 * BP     — payload address of a free block located by find_fit.
 * ASIZE  — the adjusted (aligned) allocation size in bytes, including
 *           the header and footer overhead.  */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    int cansplit = (csize - asize) >= MIN_BLKSIZE;

    remove_free(bp); /* bp is leaving the free list */

    if (cansplit)
    {
        /* Allocate front portion */
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        /* Remainder becomes a new free block */
        void *remainder = NEXT_BLKP(bp);
        PUT(HDRP(remainder), PACK(csize - asize, 0));
        PUT(FTRP(remainder), PACK(csize - asize, 0));
        insert_free(remainder);
    }
    else
    {
        /* Use the whole block */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_malloc
 * ══════════════════════════════════════════════════════════════════ */

/* mm_malloc (size)
 *
 * Allocate a block of at least SIZE bytes of usable payload and return
 * a pointer to its first payload byte.  The returned pointer is aligned
 * to DSIZE (8) bytes.  The block is not initialised; its contents are
 * indeterminate.
 *
 * Size adjustment:
 *   The requested SIZE is rounded up to the nearest multiple of DSIZE
 *   after adding DSIZE bytes of header/footer overhead, subject to a
 *   minimum of MIN_BLKSIZE bytes total.  Formally:
 *
 *     asize = MIN_BLKSIZE                       if size ≤ DSIZE
 *           = DSIZE * ⌈(size + DSIZE) / DSIZE⌉  otherwise
 *
 * Allocation strategy:
 *  1. If find_fit succeeds, place is called on the found block and the
 *     payload address is returned.
 *  2. If the free list holds no suitable block, the heap is grown by
 *     MAX(asize, CHUNKSIZE) bytes, and place is called on the new block.
 *     Using CHUNKSIZE as the minimum growth amount amortises the cost of
 *     repeated mem_sbrk calls over many small allocations.
 *
 * SIZE — the number of payload bytes requested by the caller.  A value
 *         of 0 is defined to return NULL.
 *
 * Returns a pointer to the allocated payload on success.
 * Returns NULL if SIZE is 0 or if the heap cannot be extended.  */
void *mm_malloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Adjust to meet alignment and overhead requirements */
    size_t asize = (size <= DSIZE) ? MIN_BLKSIZE
                                   : DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);

    char *bp;
    if ((bp = find_fit(asize)) != NULL)
    {
        place(bp, asize);
        return bp;
    }

    /* No fit — extend the heap */
    size_t extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL)
        return NULL;
    place(bp, asize);
    return bp;
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_realloc
 * ══════════════════════════════════════════════════════════════════ */

/* mm_realloc (bp, size)
 *
 * Change the size of the allocated block at BP to at least SIZE bytes,
 * returning a pointer to the (possibly relocated) block.  The contents
 * of the block are preserved up to the minimum of the old and new sizes.
 *
 * Special cases defined by POSIX:
 *   - If SIZE is 0, the block is freed and NULL is returned.
 *   - If BP is NULL, the call is equivalent to mm_malloc(SIZE).
 *   - Calling realloc on a free block is an error; a diagnostic is
 *     printed to stderr and NULL is returned.
 *
 * Optimised in-place strategies (to avoid unnecessary copying):
 *
 *   [Same size]
 *     If the adjusted new size equals the current block size, BP is
 *     returned immediately with no changes.
 *
 *   Case 1 — Shrink:
 *     If the adjusted new size is smaller than the current size and the
 *     leftover region is at least MIN_BLKSIZE bytes, the block is split
 *     in place: the front portion stays allocated; the remainder is
 *     formatted as a free block, inserted into the free list, and
 *     coalesced with its physical successor if that successor is also
 *     free.  No data movement occurs.
 *
 *   Case 2 — Absorb free successor:
 *     If the physical successor of BP is free and the combined size of
 *     BP and its successor is sufficient to satisfy the new size, the
 *     successor is absorbed: it is removed from the free list and BP's
 *     header/footer are rewritten.  If any space remains after fitting
 *     the new size, that space is split off as a new free block.
 *     No data movement occurs.
 *
 *   Case 3 — Grow at heap boundary:
 *     If BP's physical successor is the epilogue (nextsize == 0), the
 *     heap is extended by exactly (newsize − oldsize) bytes in place.
 *     BP's header/footer and the new epilogue are rewritten.
 *     No data movement occurs.
 *
 *   Case 4 — Fallback copy:
 *     When none of the above strategies apply, a new block of the
 *     requested size is obtained via mm_malloc, the payload is copied
 *     with memcpy (up to oldsize − DSIZE usable bytes), and the old
 *     block is released with mm_free.  The new address is returned.
 *
 * BP    — payload pointer to a previously allocated block, or NULL.
 * SIZE  — the desired new payload size in bytes.
 *
 * Returns a pointer to the reallocated block on success (which may differ
 * from BP if Case 4 was used).
 * Returns NULL if SIZE is 0 or if allocation fails in Case 4.  */
void *mm_realloc(void *bp, size_t size)
{
    if (size == 0)
    {
        mm_free(bp);
        return NULL;
    }
    if (bp == NULL)
    {
        return mm_malloc(size);
    }
    if (!GET_ALLOC(HDRP(bp)))
    {
        fprintf(stderr,
                "Error: realloc called on free block %p\n",
                bp);
        return NULL;
    }

    size_t oldsize = GET_SIZE(HDRP(bp));
    size_t newsize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
    // Case 3:
    if (newsize == oldsize)
        return bp;

    /* Case 1 — shrink (or same size) */
    if (newsize < oldsize)
    {
        if (oldsize - newsize >= MIN_BLKSIZE)
        {
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *next = NEXT_BLKP(bp);
            PUT(HDRP(next), PACK(oldsize - newsize, 0));
            PUT(FTRP(next), PACK(oldsize - newsize, 0));
            insert_free(next);
            coalesce(next);
        }
        return bp;
    }
    // newsize > oldsize
    void *next = NEXT_BLKP(bp);
    size_t nextsize = GET_SIZE(HDRP(next));
    size_t combined = oldsize + nextsize;

    /* Case 2 — absorb free next block */
    if (!GET_ALLOC(HDRP(next)) && combined >= newsize)
    {
        remove_free(next);
        if (combined - newsize >= MIN_BLKSIZE)
        {
            PUT(HDRP(bp), PACK(newsize, 1));
            PUT(FTRP(bp), PACK(newsize, 1));
            void *remainder = NEXT_BLKP(bp);
            PUT(HDRP(remainder), PACK(combined - newsize, 0));
            PUT(FTRP(remainder), PACK(combined - newsize, 0));
            insert_free(remainder);
            coalesce(remainder); // no need just for checking
        }
        else
        {
            PUT(HDRP(bp), PACK(combined, 1));
            PUT(FTRP(bp), PACK(combined, 1));
        }
        return bp;
    }

    /* Case 3 — next is epilogue, extend heap in-place */
    if (nextsize == 0)
    {
        size_t extendsize = newsize - oldsize;
        if ((long)mem_sbrk(extendsize) == -1)
            return NULL;
        PUT(HDRP(bp), PACK(newsize, 1));
        PUT(FTRP(bp), PACK(newsize, 1));
        PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));
        return bp;
    }

    /* Case 4 — fall back: allocate new, copy, free old */
    void *newbp = mm_malloc(size);
    if (newbp == NULL)
        return NULL;
    memcpy(newbp, bp, oldsize - DSIZE);
    mm_free(bp);
    return newbp;
}

/* ══════════════════════════════════════════════════════════════════
 *  mm_calloc
 * ══════════════════════════════════════════════════════════════════ */

/* mm_calloc (nmemb, size)
 *
 * Allocate a zero-initialised array of NMEMB elements of SIZE bytes each.
 * The total allocation is NMEMB * SIZE bytes, subject to the same
 * alignment and overhead rounding applied by mm_malloc.
 *
 * The function explicitly checks for integer overflow in the product
 * NMEMB * SIZE: if (totalsize / size != nmemb) the multiplication must
 * have wrapped, and NULL is returned to prevent a heap underflow.
 *
 * After a successful mm_malloc call, the entire payload region is zeroed
 * with memset.  The size passed to memset is the *logical* requested size
 * (NMEMB * SIZE), not the padded block size, so any internal-fragmentation
 * padding bytes at the end of the block are left uninitialised.
 *
 * NMEMB — number of array elements.  Returns NULL if 0.
 * SIZE  — size in bytes of each element.  Returns NULL if 0.
 *
 * Returns a pointer to the zeroed allocation on success.
 * Returns NULL if either argument is 0, if the product overflows, or if
 * mm_malloc fails.  */
void *mm_calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return NULL;

    size_t totalsize = nmemb * size;
    if (size != 0 && totalsize / size != nmemb) /* overflow check */
        return NULL;

    void *bp = mm_malloc(totalsize);
    if (bp == NULL)
        return NULL;

    memset(bp, 0, totalsize);
    return bp;
}

/* ══════════════════════════════════════════════════════════════════
 *  Heap checker
 * ══════════════════════════════════════════════════════════════════ */

/* printblock (bp)
 *
 * Print a one-line diagnostic summary of the block at BP to stdout.
 * For allocated blocks the format is:
 *
 *   <addr>: header:[<size>:<a|f>] footer:[<size>:<a|f>]
 *
 * For free blocks the predecessor and successor free-list pointers are
 * included:
 *
 *   <addr>: header:[<size>:f] pred:<pred> succ:<succ> footer:[<size>:f]
 *
 * If the header size is 0 (the epilogue sentinel), "EOL" is printed.
 * This function is intended for debugging and is called by checkheap.  */
static void printblock(void *bp)
{
    size_t hsize = GET_SIZE(HDRP(bp));
    size_t halloc = GET_ALLOC(HDRP(bp));
    size_t fsize = GET_SIZE(FTRP(bp));
    size_t falloc = GET_ALLOC(FTRP(bp));

    if (hsize == 0)
    {
        printf("%p: EOL\n", bp);
        return;
    }

    if (!halloc)
        printf("%p: header:[%zu:%c] pred:%p succ:%p footer:[%zu:%c]\n",
               bp, hsize, (halloc ? 'a' : 'f'),
               (void *)GET_PRED(bp), (void *)GET_SUCC(bp),
               fsize, (falloc ? 'a' : 'f'));
    else
        printf("%p: header:[%zu:%c] footer:[%zu:%c]\n",
               bp, hsize, (halloc ? 'a' : 'f'),
               fsize, (falloc ? 'a' : 'f'));
}

/* checkblock (bp)
 *
 * Perform basic consistency checks on the block at BP, printing a
 * diagnostic message to stdout for each detected violation:
 *
 *   - Alignment: the payload address must be a multiple of DSIZE (8).
 *     A misaligned block indicates corruption or a bug in place/extend_heap.
 *
 *   - Header/footer consistency: the raw header word must equal the raw
 *     footer word.  A mismatch indicates that one of the two boundary tags
 *     was not updated atomically with the other, or that a write overflowed
 *     into an adjacent field.
 *
 * This function does not return an error code; all violations are reported
 * by side-effect (printf).  It is called by checkheap for every block
 * in the heap.  */
static void checkblock(void *bp)
{
    if ((size_t)bp % 8)
        printf("Error: %p is not doubleword aligned\n", bp);
    if (GET(HDRP(bp)) != GET(FTRP(bp)))
        printf("Error: header does not match footer at %p\n", bp);
}

/* checkheap (verbose)
 *
 * Scan the entire heap and the explicit free list, reporting any
 * structural inconsistencies to stdout.  This function is intended for
 * use during debugging; it should not be called in production code
 * because it traverses the entire heap.
 *
 * Checks performed:
 *
 *   Prologue:
 *     The prologue block at heap_listp must have size DSIZE and
 *     allocation bit 1.  checkblock is invoked to verify alignment
 *     and header/footer consistency.
 *
 *   All blocks (heap traversal):
 *     Starting from the prologue and following NEXT_BLKP until the
 *     epilogue (size 0), each block is passed to checkblock for
 *     alignment and consistency checks.  If VERBOSE is non-zero,
 *     printblock is called for each block as well.
 *
 *   Epilogue:
 *     The final block reached by heap traversal must have size 0 and
 *     allocation bit 1.
 *
 *   Free list integrity:
 *     Every block reachable via free_listp -> GET_SUCC chain must have
 *     its allocation bit clear.  An allocated block on the free list
 *     indicates a missing remove_free call (e.g., after place).
 *     If VERBOSE is non-zero, each free-list block is printed with
 *     printblock.
 *
 * VERBOSE — if non-zero, print the payload address of the heap base,
 *            each heap block, the epilogue, and each free-list entry.
 *            Pass 0 to suppress all output (errors are still printed).  */
void checkheap(int verbose)
{
    char *bp = heap_listp;

    if (verbose)
        printf("Heap (%p):\n", heap_listp);

    if ((GET_SIZE(HDRP(heap_listp)) != DSIZE) || !GET_ALLOC(HDRP(heap_listp)))
        printf("Bad prologue header\n");
    checkblock(heap_listp);

    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        if (verbose)
            printblock(bp);
        checkblock(bp);
    }

    if (verbose)
        printblock(bp);
    if ((GET_SIZE(HDRP(bp)) != 0) || !GET_ALLOC(HDRP(bp)))
        printf("Bad epilogue header\n");

    /* Verify every free-list block is actually free */
    if (verbose)
        printf("\nFree list:\n");
    for (bp = free_listp; bp != NULL; bp = GET_SUCC(bp))
    {
        if (verbose)
            printblock(bp);
        if (GET_ALLOC(HDRP(bp)))
            printf("Error: allocated block %p on free list\n", bp);
    }
}
