# MemGuard â€” an instrumented memory allocator in C

A from-scratch implementation of `malloc` / `free` / `calloc` / `realloc`
over a fixed 1 MB memory pool, with the debugging powers of tools like
**Valgrind** and **AddressSanitizer** built in:

| Feature | What it catches |
|---|---|
| **Leak detection** | every unfreed block, reported with the exact `file:line` that allocated it |
| **Rear canary bytes** | heap buffer **overflows** (writing past the end of an allocation) |
| **Front canary** | heap **underflows** (writing before the start) |
| **Magic-value headers** | **double free** and **wild free** (freeing a non-heap pointer) |
| **Heap statistics** | bytes in use, peak usage, **external fragmentation %** |
| **ASCII heap map** | a visual picture of used vs free memory |
| **Fit policies** | switch between **first-fit** and **best-fit** at runtime |

~550 lines of C99. No dependencies. No platform-specific code.

## Build & run

Needs GCC on PATH (any toolchain works, e.g. w64devkit or MinGW on Windows).

```
build.bat                     (or: make)
bin\test_memguard.exe         run the 27-check self-test suite
bin\demo_basic.exe            allocation / realloc / stats / heap map
bin\demo_leak.exe             leak detection with file:line reporting
bin\demo_overflow.exe         buffer overflow caught by canary
bin\demo_doublefree.exe       double free + wild free detection
bin\demo_fragmentation.exe    fragmentation made visible, then coalesced away
```

## How it works

The heap is one static byte array. Every block â€” allocated or free â€”
begins with a header, and all blocks are linked in **one doubly-linked
list ordered by address**, so list neighbours are also physical
neighbours in memory. That single invariant makes coalescing trivial.

```
 g_pool (1 MB static array)
 +-----------+----------------+----+-----------+---------+----+------------------+
 | header    | payload        | RC | header    | payload | RC | free block ...   |
 | magic=A   | (user's bytes) |    | magic=A   |         |    | magic=F          |
 +-----------+----------------+----+-----------+---------+----+------------------+
   ^ file/line/seq recorded      ^ rear canary (0xDEADBEEF)
   ^ prev/next pointers ---------- address-ordered list ------->
```

- **`mg_malloc(n)`** walks the list for a free block (first- or
  best-fit), **splits** it if the remainder is usable, stamps the
  header with `MAGIC_ALLOC` + `__FILE__`/`__LINE__`, writes the rear
  canary, returns the payload pointer.
- **`mg_free(p)`** steps back from the payload to the header and
  validates everything: pointer in range, magic is `ALLOC` (a `FREE`
  magic here = double free), canaries intact (else overflow/underflow
  report). Then it marks the block free and **coalesces** with either
  physical neighbour that is also free.
- **Alignment**: headers are padded to 16 bytes and all regions are
  16-byte multiples, so every payload is 16-byte aligned (same
  guarantee as the system malloc).

## Known limitations (by design, and I can defend each)

- **Not thread-safe** â€” one global heap, no locking. Fix: one mutex
  around the list, or per-thread arenas like real allocators.
- **O(n) allocation** â€” list walk. Real allocators use segregated
  free lists (bins by size) for O(1) common cases.
- **Fixed pool** â€” no `sbrk`/`VirtualAlloc` growth. Deliberate: a
  fixed pool is exactly how embedded/firmware allocators work.
- Detection happens at `free`/`mg_check_all` time, not the instant of
  the overflow (that needs MMU page tricks Ã  la ASan).

