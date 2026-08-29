# Heap Allocator and Garbage Collector

Two projects in one repository, built in sequence.

**Part 1 — a dynamic memory allocator** written in C, implementing the
full `malloc` / `free` / `realloc` / `calloc` API without using libc's
allocator. Two strategies are implemented and compared: an **implicit
free list** and an **explicit doubly-linked free list**.

**Part 2 — a conservative mark-and-sweep garbage collector** built on
top of that allocator, which it is not permitted to modify. It finds
roots in `.data`, `.bss`, the stack and the CPU registers, and reclaims
what nothing points at. Target is **Linux i386** (`-m32`,
single-threaded) — pointers are 4 bytes, which makes conservative
scanning measurably harder than the 64-bit case.

Both are learning exercises in systems programming and heap internals.

---

## Allocator Design

### Block layout

Every block carries a 4-byte **header** and 4-byte **footer**, each
encoding the block size and an allocated bit packed into one word:

```
[ HDR (4B) | payload ... | FTR (4B) ]
           ^bp (pointer returned to caller)
```

Size is always a multiple of 8, so the bottom 3 bits are free the
lowest bit stores the allocated flag. This is called a **boundary
tag** and enables O(1) navigation to both the next and previous
physical blocks without a separate data structure.

### Coalescing

On every `free`, the allocator checks both physical neighbours and
merges in one of four cases:

```
prev alloc, next alloc  → no merge
prev alloc, next free   → merge with next
prev free,  next alloc  → merge with prev
prev free,  next free   → merge both
```

All four cases are O(1) because the footer of the previous block
is always at a fixed offset behind the current header.

### Heap backing

`memlib.c` backs the heap with `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`
instead of `malloc`, giving the allocator a contiguous virtual
address region completely isolated from libc's own heap. `mem_sbrk`
is a thin pointer-advance over this region, modelling the real Unix
`sbrk(2)` syscall.

---

## Allocator Strategies

### Implicit Free List

`find_fit` walks every block in the heap sequentially from the
prologue, checking the alloc bit of each header. First-fit policy.

| Operation | Complexity         |
|-----------|--------------------|
| malloc    | O(all blocks)      |
| free      | O(1)               |
| coalesce  | O(1)               |

### Explicit Free List

Free blocks are linked into a doubly-linked list via `PRED` and
`SUCC` pointers stored inside the payload area of each free block.
`find_fit` walks only free blocks. LIFO insertion — freed blocks are
prepended to the list head.

Minimum block size is 16 bytes (header + PRED + SUCC + footer).

| Operation | Complexity         |
|-----------|--------------------|
| malloc    | O(free blocks)     |
| free      | O(1)               |
| coalesce  | O(1)               |

The known limitation of both designs: no size classes. A single
unsorted list means `find_fit` degrades under fragmentation.
Size segregated bins (as used in glibc) would reduce `malloc` to
O(1) in practice,that is the natural next step.

---

## Garbage Collector

A **conservative** collector: it cannot tell a real pointer from an
integer that happens to look like one, so it treats any 4-byte word
that could be a heap address as if it were. Nothing about the program
needs to cooperate — no type information, no registration, no write
barriers.

It is **purely additive**. `memlib.c`, both allocators and the public
API in `mm.h` are unmodified; the collector lives entirely in
`gc.h` / `gc.c` and reads the allocator's boundary tags from outside.

### The pointer test

The core question is: given an arbitrary word, does it point inside an
allocated block, and where does that block start?

The heap's bytes do not self-identify — reading four bytes behind a
candidate and calling it a header fails, because ordinary program data
can look exactly like a valid header. The integer `25` is `0x00000019`,
which masks to a 24-byte allocated block.

So the collector walks the implicit list once per collection to build
an **address-sorted table** of allocated blocks, then answers by binary
search: find the first entry starting strictly above the candidate,
step back one, and range-check. That resolves interior pointers, and
the same range check rejects free blocks for free — a candidate inside
a free block searches back to the previous *allocated* block and falls
outside it. That matters, because a free block's first two payload
words hold `PRED`/`SUCC`, which are real heap addresses.

| Operation | Complexity |
|-----------|------------|
| build table | O(all blocks) |
| pointer test | O(log allocated blocks) |
| mark | O(reachable words) |
| sweep | O(all blocks) |

### Root set

| Region | Delimited by |
|--------|--------------|
| `.data` | `__data_start` / `_edata` linker symbols |
| `.bss` | `__bss_start` / `_end` linker symbols |
| stack | a local in `gc_collect` up to `__builtin_frame_address(0)` in `main` |
| registers | `setjmp` spills the callee-saved registers; the `jmp_buf` is scanned |

The stack and register regions are complementary rather than redundant
in principle. If an intermediate frame wanted a callee-saved register it
had to spill the caller's value into its own frame, and the stack scan
finds it; if nobody touched it, the value is still in the register and
the `jmp_buf` catches it.

### Mark bit

Bit 1 of the existing block header. It is free because every block size
is a multiple of 8, so the low three bits of the size field are zero,
and bit 0 is already the allocated flag. `GET_SIZE` masks `~0x7` and
`GET_ALLOC` masks `0x1`, so the allocator never sees it.

The allocator's own `checkblock` compares the raw header and footer
words, though, and a mark in the header alone makes them differ. Rather
than weaken that check or write the bit twice, `gc_collect` clears every
mark before returning — the two copies disagree only *inside* a
collection, where nothing can observe the heap.

### Sweeping under a coalescing allocator

`mm_free` coalesces eagerly, so a sweep that saves the successor address
before freeing still breaks: if that successor was already free,
`coalesce` absorbs it and the saved address becomes interior to a merged
block, where the word read as a header is payload data.

The sweep therefore skips forward past the whole run of free blocks to
the next block carrying the allocated flag, and only then calls
`mm_free`. Coalescing can absorb free neighbours but never an allocated
one, so that address survives. The epilogue terminates the scan for the
same reason — it is `PACK(0, 1)`.

### Marking

An explicit worklist, never recursion — a chain of N blocks would
otherwise cost N stack frames with nothing bounding N but the heap.
`gc_push` sets the mark bit *before* pushing, which does two jobs: it
bounds the worklist to one entry per block, and it terminates traversal
of cyclic structures.

---

## Project Structure

```
heap-allocator/
├── include/
│   ├── mm.h
│   ├── memlib.h
│   └── gc.h
├── lib/
│   └── memlib.c          ← mmap-backed heap simulation
├── src/
│   ├── mm_implicit.c     ← implicit allocator
│   ├── mm_explicit.c     ← explicit allocator
│   ├── gc.c              ← conservative mark & sweep collector
│   ├── main_implicit.c   ← correctness tests (implicit)
│   ├── main_explicit.c   ← correctness tests (explicit)
│   ├── main_gc.c         ← correctness tests (collector)
│   ├── bench_gc.c        ← false-retention and cost benchmark
│   └── bench_mark1_throughput.c
├── build/
├── bin/
└── Makefile
```

---

## Build and Run

```bash
# Build and run implicit allocator tests
make run-implicit

# Build and run explicit allocator tests
make run-explicit

#To run both
make run-all

# Build and run collector tests
make run-gc

# Benchmark: explicit allocator vs glibc
make run-bench

# Benchmark: collector false retention and cost
make run-bench-gc
```

Everything builds `-m32`. On a 64-bit host that needs `gcc-multilib`
and `libc6-dev-i386`.

---

## Correctness Tests

### Allocator tests

Both allocators pass a shared test suite covering:

- 8-byte payload alignment
- Block splitting and remainder insertion
- All four coalesce cases
- `realloc` — shrink in place, absorb adjacent free block,
  epilogue extension, fallback copy with data preservation
- `calloc` zero-initialisation and multiplication overflow
- LIFO free-list ordering (explicit only)
- heap checker: physical heap walk via `NEXT_BLKP`
  cross-validated against free-list traversal via `SUCC` pointers

### Collector tests

`make run-gc` runs a driver covering the pointer test, marking,
sweeping, and each root region:

- Fresh heap yields an empty table — the prologue carries the
  allocated flag but must never be recorded
- Pointer test against a block's start, its interior, its last payload
  byte, one byte before, one byte past the end, and a stack address —
  plus **exhaustive** sweeps of all 200 offsets in one payload and all
  8 bytes of a footer+header gap
- A pointer into a freed block returns NULL, so free blocks are
  rejected despite holding real addresses in `PRED`/`SUCC`
- 5 of 8 blocks reachable from one global; the 3 unreferenced blocks
  are reclaimed and their space is reusable
- The chain still walks 5 links afterwards, so marking corrupts no
  payload data
- A **cyclic** chain still reports 5 and terminates — without
  mark-before-push this hangs rather than failing an assertion
- A block held only in a local survives; a block from a returned
  function is *eventually* reclaimed, and the test reports whether it
  took one collection or two
- The allocator's own `checkheap` is silent afterwards, proving marks
  did not outlive the collection

Verified at 25/25 runs at both `-O0` and `-O2`, with and without ASLR,
clean under `-Wall -Wextra -Wshadow -Wpointer-arith` and `-std=c11`.

**Ablation** is used to confirm the code is load-bearing rather than
merely present: deleting the stack scan alone passes (the register
buffer covers it), deleting the register scan alone passes (the stack
covers it), and deleting **both** fails at either optimisation level.

---

## Collector Measurements

`make run-bench-gc`. False retention is *marked* minus *genuinely
live* — and the collector cannot supply the second term, since a
collector that could identify exactly what is reachable would not be
conservative. So the workload is built with its live set known by
construction: a full binary tree of depth *d* has exactly 2^(d+1)−1
nodes and nothing else is reachable.

### Retention against stack depth

| Recursion depth | Live | Garbage | Falsely retained | False retention |
|---|---|---|---|---|
| 0 | 127 | 301 | 4 | 1.3% |
| 8 | 127 | 309 | 9 | 2.9% |
| 32 | 127 | 333 | 33 | 9.9% |
| 64 | 127 | 365 | 65 | 17.8% |
| 128 | 127 | 429 | 129 | 30.1% |

From depth 8 onward the retained count equals the number of recursion
temporaries **exactly** — one per frame. The 300 tree-garbage blocks,
built in a helper that returned, are always reclaimed. So retention is
not diffuse: it is precisely the set of objects a live stack frame
still mentions, whether or not the program will ever use them again.

### Retention against allocator zeroing

| Node size | `mm_calloc` | `mm_malloc` |
|---|---|---|
| 16 | 9.9% | 9.9% |
| 24 | 9.9% | 15.3% |
| 64 | 9.9% | **17.4%** |
| 100 | 15.9% | 15.9% |
| 104 | 9.9% | 12.9% |

`mm_malloc` zeroes nothing, so a recycled block arrives holding its
predecessor's data — including the free-list pointers the allocator
wrote into the first two payload words. At 16 bytes the program's own
two writes land exactly on them and erase them; at 64 bytes fourteen
words survive untouched. The 100-byte row shows the same leak through
padding: `mm_calloc` zeroes the *request*, not the padded payload.

### Do arbitrary integers look like pointers?

On i386 a single 4-byte word is a complete pointer, so any `int` is a
candidate by itself. The magnitude is arithmetic — a candidate is
accepted only if it lands in the mapped heap:

```
P(hit) = 794,624 bytes / 2^32 = 1.85e-4   (0.0185% of the address space)
expected from 8192 random words = 1.52
observed = 0
```

Consistent with the model rather than a confirmation of it, but the
order of magnitude is the point: chance hits are a handful, not
hundreds. Retention here is dominated by stale references to real
objects, not by integers that coincidentally look like pointers.

### Cost

| Phase | Per block | Scales with |
|---|---|---|
| marking | 0.086 µs | the live set |
| table build + sweep | 0.016 µs | the whole heap |

33,079 blocks collected in 2.83 ms. Both phases linear. The ~5× gap is
the pointer test: the sweep reads one header and tests a bit, while
marking runs a binary search for every payload word.

**That is the measured case for a page-indexed pointer test** — Boehm's
collector uses size-segregated pages plus a sparse page table to make
the lookup O(1) arithmetic instead of a search. It needs a co-designed
allocator, which is exactly what this project did not have.

### Limits

Single-threaded. Not generational, incremental, or compacting. Heaps
under 1 MB, so cache behaviour at larger scale is untested. Retention
figures are compiler-dependent — `-O2` reuses stack slots and keeps
more in registers, giving roughly half the retention of `-O0` at the
same depth.

---

## Allocator Benchmark

**Workload:** 1M allocations of random sizes (8–1024 bytes),
free every other block, reallocate into freed slots at 64 bytes,
free all.

### Per-phase results

| Phase | Operation | Explicit | glibc |
|-------|-----------|----------|-------|
| 1 | alloc 1M blocks | 16.2 M ops/s | 11.7 M ops/s |
| 2 | free 500k blocks | 47.0 M ops/s | 35.3 M ops/s |
| 3 | alloc 500k reuse | 0.01 M ops/s | 12.7 M ops/s |
| 4 | free 1M blocks | 29.5 M ops/s | 13.9 M ops/s |
| **Total** | | **0.03 M ops/s** | **14.2 M ops/s** |

### Why Phase 1 and 2 beat glibc

The explicit allocator has lower per call overhead for small
working sets. Phase 1 hits a nearly-empty free list (one large
block from `mm_init`) and finds a fit immediately. Phase 2's
`mm_free` is a pure O(1) prepend — no coalescing fires because
every freed block has an allocated neighbour on both sides.
glibc pays thread-safety and bin-management overhead even in
single-threaded use.

### Why Phase 3 collapses

After Phase 2, the free list holds 500,000 nodes. Each `malloc(64)`
in Phase 3 calls `find_fit`, which walks the list from the head.
With no size classes, it cannot jump directly to a 64-byte block —
it must scan on average ~250,000 nodes per call.
Total: 500k × 250k ≈ 125 billion pointer dereferences.

glibc avoids this entirely with size-segregated bins: a `malloc(64)`
goes directly to the 64-byte free list in O(1).

**This benchmark makes the cost of an unsorted free list under
fragmentation concretely visible, and motivates size class design.**

---

## Author

Piyush Khanna
