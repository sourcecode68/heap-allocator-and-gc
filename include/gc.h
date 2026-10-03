// include/gc.h
#ifndef GC_H
#define GC_H

#include <stddef.h>

/*
 * Conservative mark & sweep garbage collector for the explicit allocator.
 *
 * gc_collect is the collector: it finds roots in .data, .bss, the stack
 * and the spilled callee-saved registers, marks every allocated block they
 * reach, and frees the rest.  Underneath it, gc_build_table snapshots the
 * allocated blocks and gc_isPtr answers "does this value point into one,
 * and if so where does that block begin?" — exposed so they can be tested
 * on their own.  gc_report, gc_stats and the region figures describe the
 * most recent collection.
 */

/* gc_init - record the heap bounds and the bottom of the stack.
 *
 * Must be called AFTER mem_init and mm_init: the heap does not exist until
 * they have run, and mem_heap_lo would hand back an uninitialised pointer.
 *
 * STACK_BOTTOM must be an address ABOVE every local variable in main,
 * because the stack grows downward and the collector scans from its own
 * frame up to this address.  Pass __builtin_frame_address(0) from main:
 *
 *     gc_init(__builtin_frame_address(0));
 *
 * Do NOT pass the address of a local.  Declaration order does not
 * determine address order, and GCC will happily place your first local at
 * the BOTTOM of the frame — leaving every other local above the recorded
 * bound, unscanned, and their blocks freed while still in use.  Nothing
 * diagnoses that; it just corrupts the program later. */
void gc_init(void *stack_bottom);

/* gc_collect - run one full mark and sweep.
 *
 * Scans .data, .bss, the stack and the spilled callee-saved registers for
 * roots, marks everything reachable from them, frees every allocated block
 * that was not reached, and clears the mark on those that were.  No header
 * carries a mark by the time this returns, so header and footer agree
 * again and checkheap is meaningful.
 *
 * Conservative in one direction only.  A block whose address is held
 * anywhere it scans is never freed; but any word that merely looks like
 * such an address — a stale stack slot, an old pointer in unzeroed memory,
 * an integer that happens to match — keeps its block alive too. */
void gc_collect(void);

/* gc_report - print reachable vs. allocated blocks and payload bytes for
   the most recent collection.  Reads figures captured during that
   collection, so it stays meaningful after the marks have been cleared. */
void gc_report(void);

/* ── Per-region attribution (M6) ──────────────────────────────────
 *
 * Where the accepted candidates came from.  A retention percentage on its
 * own is a number; split by region it explains itself. */
#define GC_R_DATA 0
#define GC_R_BSS 1
#define GC_R_STACK 2
#define GC_R_REGS 3
#define GC_R_PAYLOAD 4
#define GC_NREGIONS 5

/* gc_region_label - short name for a region index, for printing. */
const char *gc_region_label(int region);

/* gc_region_stats - words examined and words that passed the pointer test
   in REGION during the most recent collection.  Either pointer may be
   NULL; an out-of-range region writes nothing. */
void gc_region_stats(int region, size_t *words_scanned, size_t *words_accepted);

/* gc_stats - the same figures gc_report prints, for a caller that wants
   to assert on them rather than read them.  Any pointer may be NULL. */
void gc_stats(size_t *marked_blocks, size_t *marked_bytes,
              size_t *total_blocks, size_t *total_bytes);

/* gc_heap_lo - address of the first byte of the heap.
   Fixed for the life of the process. */
void *gc_heap_lo(void);

/* gc_heap_hi - address of the LAST byte of the heap, inclusive.
   NOT one past the end.  This deliberately matches memlib's mem_heap_hi
   convention rather than introducing a second one.  The value moves upward
   as the heap grows. */
void *gc_heap_hi(void);

/* ─────────────────────── M1: the block table ─────────────────────── */

/* gc_build_table - walk the heap and record every allocated block into an
   address-sorted table, discarding any previous one.  The prologue and
   epilogue are excluded; free blocks are excluded.

   Returns the number of blocks recorded.  A return of 0 means there is no
   usable table, whether because the heap holds no allocated blocks or
   because storage could not be obtained.  Callers need not distinguish
   the two: both mean "do not proceed with a collection". */
size_t gc_build_table(void);

/* gc_free_table - release the table.  Safe to call when none exists. */
void gc_free_table(void);

/* gc_table_count - number of entries in the current table. */
size_t gc_table_count(void);

/* gc_table_dump - print the table.  Debugging aid, mirrors checkheap. */
void gc_table_dump(void);

/* gc_isPtr - if CANDIDATE points anywhere inside the payload of an
   allocated block, return that block's payload start; otherwise NULL.
   A pointer to a block's header or footer is NOT inside it.

   PRECONDITION: a block table must be current, i.e. gc_build_table() has
   been called and gc_free_table() has not.  With no table this returns
   NULL for EVERYTHING, which is indistinguishable from an honest "not a
   pointer" answer.  Note gc_collect releases the table on its way out, so
   calling gc_isPtr straight after a collection answers NULL every time.
   Rebuild first.

   It does not build one on demand deliberately: a query that allocates
   behind the caller's back would be a worse trap than this one. */
void *gc_isPtr(void *candidate);

/* Note: there is deliberately no gc_sweep here.  Sweeping is the
   destructive half of a collection and has a precondition — the heap must
   have just been marked.  Called on its own, it would free every block in
   the heap, because none of them would carry a mark.  gc_collect is the
   only entry point; the sweep is internal to gc.c. */

#endif
