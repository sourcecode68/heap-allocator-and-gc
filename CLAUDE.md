# Project: conservative mark & sweep garbage collector

Building a conservative garbage collector on top of the finished heap allocator in
this repo. Target: **Linux i386** — the allocator is 32-bit only and everything is
built with `-m32`. Single-threaded, GCC.

**Pointers are 4 bytes.** Scan steps, alignment masks, and every size assumption
follow from that. Most garbage collection material online assumes x86-64; if you
are about to write `8` where a pointer width is meant, or copy a 64-bit example
unchanged, stop and flag it to me instead.

---

## How to work with me

I am learning this material. My understanding matters more than the code.

- **Explain before you write.** Before any new code, describe in plain English what
  it does and why it is needed. Then write it. Never lead with a code block.
- **One new term at a time.** Define every piece of jargon in one sentence on first
  use. Do not use a term from the "not yet introduced" list below until we reach the
  milestone that needs it.
- **Stay in the current milestone.** Implement only the milestone we are on. Do not
  add code for a later one "while we're here," and do not reference its concepts.
- **Small steps.** Stop after roughly 40 lines of new code and let me read it. If a
  change would span more than two files, stop and propose the split first.
- **Make me predict.** Before running a new test, ask what I expect to happen and
  wait for my answer.
- **Answer "why" with mechanism.** Explain what the machine is actually doing.
  Never answer with "that's the convention" or "that's how it's done."
- **Do not refactor my allocator.** The GC is purely additive. If you think existing
  code needs changing, say so and wait — do not change it.
- **Say when you are unsure.** If a detail depends on the ABI, the compiler version,
  or something you have not read in this repo, say so rather than guessing.

---

## Hard constraints

- **Do not modify**: `lib/memlib.c`, `src/mm_implicit.c`, `src/mm_explicit.c`, or the
  public API in `include/mm.h`.
- **New files only**: `include/gc.h`, `src/gc.c`, `src/main_gc.c`.
- Add Makefile targets alongside the existing ones; do not restructure the Makefile.
- Target the **explicit** allocator first. Keep `gc.c` free of assumptions about
  which allocator is linked, where that is cheap to do.
- Single-threaded only. No threads, no signal handlers, no `fork`.

---

## The existing allocator

Verify each of these against the source before relying on it — I wrote this from
memory of the design, not from reading the code.

- The heap is one contiguous `mmap` region created in `lib/memlib.c`. The collector
  needs its low and high bounds; check what `memlib` already exposes before adding
  anything new.
- Blocks use **boundary tags**: a 4-byte header and 4-byte footer, each packing the
  block size together with an allocated flag.
- Sizes are always multiples of 8, so header bits 1 and 2 are unused. Bit 0 is the
  allocated flag. **Use bit 1 as the mark bit.**
- `mm_free` coalesces immediately with both physical neighbours. Any loop that frees
  while walking the heap must compute the successor block *before* calling free —
  coalescing can absorb it or merge the current block backwards.
- The explicit allocator stores `PRED`/`SUCC` pointers inside the payloads of free
  blocks. Those are real heap addresses. The collector must never treat a free block
  as scannable, and the pointer test must return NULL for anything that is not
  inside an *allocated* block.
- Minimum block size is 16 bytes.
- `src/main_implicit.c` and `src/main_explicit.c` contain a heap checker. Extend that
  pattern for GC invariants rather than writing a new checker from scratch.

---

## Consequences of the 32-bit target

- `sizeof(void *) == 4`. Root scanning steps 4 bytes at a time and aligns candidates
  to 4 bytes. Block payloads are still 8-byte aligned — these are different numbers
  and must not be conflated.
- Sizes are still multiples of 8, so header bits 1 and 2 are still free. The mark bit
  plan is unchanged.
- **False positives will be far more common than published 64-bit figures suggest.**
  On i386 a single 4-byte word is a complete pointer, so any `int` in the program is
  by itself a candidate. The heap is also a much larger fraction of a 4 GB address
  space than of a 64-bit one. Treat high retention as expected behaviour to be
  measured, not as a bug to be fixed.
- The callee-saved registers on i386 are `%ebx`, `%esi`, `%edi`, `%ebp` — four, not
  the six of x86-64. `setjmp` still spills them.
- i386 passes arguments on the stack rather than in registers and has only 8
  general-purpose registers, so the compiler spills to memory much more often.
  Pointers reach the stack more readily than on x86-64.
- There is no red zone on i386. That x86-64 caveat does not apply.
- Addresses in `/proc/self/maps` are 8 hex digits. Parse into `uintptr_t` with the
  right format specifier; do not copy one from a 64-bit example.
- Building `-m32` on a 64-bit host needs `gcc-multilib` and `libc6-dev-i386`. If the
  build fails on missing headers, say so rather than trying to work around it.
- The binary is PIE, so addresses from nm are link-time offsets, not runtime
addresses. Never hardcode an address; always take the address of a symbol at
runtime. Use `setarch -R` when reproducibility matters during debugging.
---

## Milestones

Do these in order. Each one must be independently testable before moving on.

**M0 — Scaffolding.** Create `gc.h`, `gc.c`, `main_gc.c`, and a `run-gc` make target.
`gc_init()` records the heap bounds; `gc_collect()` is an empty stub.
*Done when:* `make run-gc` prints the heap's low and high addresses and exits 0.

**M1 — The pointer test.** Walk the heap's implicit list to build an address-sorted
table of allocated block starts, then answer "does this value point into an allocated
block, and if so where does that block begin?" by binary search.
*Done when:* a test allocates 10 blocks and gets the right answer for a pointer to a
block's start, to its middle, to one byte before it, to one byte past its end, and
for a stack address.

**M2 — Marking, globals only, nothing freed.** Claim bit 1 of the header as the mark
bit. Scan only the `.data` and `.bss` regions for roots. Traverse with an explicit
worklist, never recursion. Add `gc_report()` printing marked vs. allocated bytes.
*Done when:* a global pointer to a chain of 5 blocks reports 5 marked, with 3
unreferenced blocks left unmarked. Nothing is freed yet. The globals region contains memlib's and the allocator's own pointers
(mem_heap, mem_brk, mem_max_addr, heap_listp, free_listp). These point into
the heap and will pass the pointer test. The prologue block must never be
treated as reachable, and mem_brk holds a one-past-the-end address that will
exercise the isPtr boundary case.

**M3 — Sweeping.** Walk the heap, free unmarked allocated blocks, clear the mark bit
on the rest.
*Done when:* the M2 test now actually reclaims the 3 unreferenced blocks and the
existing heap checker still passes afterwards.

**M4 — Stack roots.** Record the stack bottom in `main`. Scan from the current frame
to that bottom.
*Done when:* a block whose only reference is a local variable survives collection,
and a block referenced only from a function that has already returned is eventually
reclaimed.

**M5 — Register roots.** Spill callee-saved registers with `setjmp` and scan the
buffer.
*Done when:* the full M4 test suite passes with identical results at both `-O0` and
`-O2`.

Note for i386: because arguments go on the stack and register pressure is high, the
`-O2` failure that motivates this milestone may not reproduce. **Do not conclude
register scanning is unnecessary if nothing crashes.** Instead, disassemble a test
function with `objdump -d -M intel` and show me a case where a live heap pointer sits
in `%ebx` across a call with no corresponding store to memory. That is the evidence.

**M6 — Instrumentation.** Add a mark-only mode that reports what it *would* free, a
poison mode that fills reclaimed payloads with `0xDE`, and measurement of bytes
retained vs. bytes genuinely reachable, plus collection time against live-set size.
Also count how many scanned words pass the pointer test, split by which root region
they came from, so the retention number can be attributed rather than just reported.
*Done when:* a bench target prints a false-retention percentage and a per-region
breakdown of how many candidate words were accepted.

---

## Vocabulary ledger

**Already introduced — safe to use freely:**
root set, reachability, conservative collector, mark bit, sweep phase, boundary tag,
allocated flag, implicit free list, explicit free list, coalescing, payload, CPU
register, caller-saved, callee-saved, stack frame, stack growth direction, `.data`,
`.bss`, linker symbol, `/proc/self/maps`, stale stack slot, floating garbage,
interior pointer, worklist, spilling, mutator, ablation, A/B test,
as-if rule, tail call, stack canary.

**Not yet introduced — do not use until its milestone, and define it when you do:**
tricolour marking, grey set, write barrier, read barrier, generational collection,
nursery, remembered set, card table, compaction, semispace, safepoint, precise or
exact collector, weak reference, finalizer, mostly-copying collection, black
allocation.

If you need a term that is on neither list, introduce it explicitly: say it is new,
define it in one sentence, and add it to the first list.