// include/gc.h
#ifndef GC_H
#define GC_H

#include <stddef.h>

/*
 * Conservative mark & sweep garbage collector for the explicit allocator.
 *
 * M1 — the pointer test.  gc_build_table snapshots every allocated block
 * in the heap; gc_isPtr answers "does this value point into one, and if
 * so where does that block begin?"  Nothing is marked and nothing is
 * freed yet.
 */

/* gc_init - record the heap bounds.
   Must be called AFTER mem_init and mm_init: the heap does not exist until
   they have run, and mem_heap_lo would hand back an uninitialised pointer. */
void gc_init(void);

/* gc_collect - run a collection.  Empty until M2. */
void gc_collect(void);

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
   Implemented in step 3 of M1. */
void *gc_isPtr(void *candidate);

#endif
