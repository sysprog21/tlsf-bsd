# tlsf-bsd: Two-Level Segregated Fit Memory Allocator

An O(1) constant-time memory allocator for real-time and embedded systems,
derived from the BSD-licensed implementation by
[Matthew Conte](https://github.com/mattconte/tlsf) and based on the
[TLSF specification](http://www.gii.upv.es/tlsf/main/docs.html).

TLSF provides bounded worst-case allocation and deallocation with low fragmentation.
The algorithm guarantees O(1) time complexity for all operations regardless of allocation pattern or heap state,
a property required by hard real-time systems where unbounded latency is unacceptable.

This implementation was written to the specification of the document,
therefore no GPL restrictions apply.

## Features

* O(1) cost for `malloc`, `free`, `realloc`, `aligned_alloc`
* One word overhead per allocation
* 32 second-level subdivisions per first-level class
  (~3.125% max internal fragmentation for large allocations)
* Immediate coalescing on free (no deferred work)
* Two pool modes: dynamic (auto-growing via `tlsf_resize`) and static
  (fixed-size via `tlsf_pool_init`)
* Pool extension via `tlsf_append_pool` (coalesces adjacent memory)
* Realloc with in-place expansion (forward, backward, and combined)
* Heap statistics and 4-phase consistency checking
* WCET measurement infrastructure with cycle-accurate timing
* Branch-free size-to-bin mapping
* ~500 lines of core allocator code
* Minimal libc: only `stddef.h`, `stdbool.h`, `stdint.h`, `string.h`
* Not thread-safe by design; callers provide external synchronization

## Build and Test

```shell
make all          # Build test, bench, and wcet executables -> build/
make check        # Run all tests with heap debugging
make bench        # Full throughput benchmark (50 iterations)
make bench-quick  # Quick benchmark for development
make wcet         # WCET measurement (10000 iterations)
make wcet-quick   # Quick WCET check
make clean        # Remove build artifacts
```

Compile flags used by default:
```
-std=gnu11 -g -O2 -Wall -Wextra -DTLSF_ENABLE_ASSERT -DTLSF_ENABLE_CHECK
```

## API

### Core Allocation

```c
#include "tlsf.h"

/* Dynamic pool (auto-growing): user must define tlsf_resize() */
tlsf_t t = TLSF_INIT;
void *p = tlsf_malloc(&t, 256);
void *q = tlsf_aalloc(&t, 64, 256);   /* 64-byte aligned */
p = tlsf_realloc(&t, p, 512);
tlsf_free(&t, p);
tlsf_free(&t, q);

/* Static pool (fixed-size): no tlsf_resize() needed */
char pool[1 << 20];
tlsf_t s;
size_t usable = tlsf_pool_init(&s, pool, sizeof(pool));
void *r = tlsf_malloc(&s, 100);
tlsf_free(&s, r);
```

### Functions

| Function | Description |
|----------|-------------|
| `tlsf_malloc(t, size)` | Allocate `size` bytes. Zero `size` returns a unique minimum-sized block. |
| `tlsf_free(t, ptr)` | Free a previously allocated block. NULL is a no-op. |
| `tlsf_realloc(t, ptr, size)` | Resize allocation. Tries in-place expansion before relocating. |
| `tlsf_aalloc(t, align, size)` | Allocate with alignment. `align` must be a power of two. |
| `tlsf_pool_init(t, mem, bytes)` | Initialize a fixed-size pool. Returns usable bytes, 0 on failure. |
| `tlsf_append_pool(t, mem, size)` | Extend pool with adjacent memory. Returns bytes used, 0 on failure. |
| `tlsf_resize(t, size)` | Platform callback for dynamic pool growth (weak symbol). |
| `tlsf_check(t)` | Validate heap consistency (requires `TLSF_ENABLE_CHECK`). |
| `tlsf_get_stats(t, stats)` | Collect heap statistics (free/used bytes, block counts, overhead). |

### Compile Flags

| Flag | Effect |
|------|--------|
| `TLSF_ENABLE_ASSERT` | Enable runtime assertions in allocator internals |
| `TLSF_ENABLE_CHECK` | Enable `tlsf_check()` heap consistency validation |
| `TLSF_MAX_POOL_BITS` | Clamp FL index to reduce `tlsf_t` size. Pool max becomes `2^N` bytes. E.g. `-DTLSF_MAX_POOL_BITS=20` for 1 MB |
| `TLSF_SPLIT_THRESHOLD` | Minimum remainder size (bytes) to split off when trimming. Default: `BLOCK_SIZE_MIN` (16 on 64-bit) |

## Design

### Segregated Free Lists

Traditional allocators maintain a single free list and search it on every allocation,
resulting in O(n) cost in the number of free blocks.
TLSF instead _segregates_ free blocks into size-classified bins.
Each bin holds a linked list of blocks whose sizes fall within that bin's range.
To allocate, TLSF looks up the bin for the requested size and pops a block from it.
No searching, no traversal: the bin lookup _is_ the allocation.

The key question is how to organize the bins so that (a) lookup is O(1),
(b) internal fragmentation stays low, and (c) the control structure fits in a few kilobytes.

### Two-Level Indexing

A single level of power-of-2 bins would give O(1) lookup but up to 50% internal fragmentation
(a 65-byte request lands in the 128-byte bin).
Hundreds of linear bins would reduce fragmentation but make the bitmap too large for O(1) scanning.

TLSF splits the difference with two levels:
* First level (FL): power-of-2 size classes.
  FL index `i` covers sizes `[2^i, 2^(i+1))`.
  This gives logarithmic coverage: 32 classes span all sizes on 64-bit systems.
* Second level (SL): each FL class is subdivided into 32 equal linear bins.
  Within the range `[2^i, 2^(i+1))`, each SL bin covers a span of `2^i / 32`.

The result: 32 x 32 = 1024 bins total, with worst-case internal fragmentation bounded by 1/32 = 3.125% for large allocations.
Small sizes (below 256 bytes on 64-bit) use a flat linear mapping where every aligned size gets its own SL bin,
so fragmentation for small allocations is zero.

### Bitmap-Driven O(1) Lookup

Each level is tracked by a bitmap: a single `uint32_t` for FL,
and one `uint32_t` per FL class for SL.
A set bit means "at least one free block exists in this bin."

To find a suitable block:
1. Map the requested size to FL/SL indices (arithmetic, no branches).
2. Mask the SL bitmap at that FL index to find the first bin at or above the target SL using one `ffs` (find-first-set) instruction.
3. If no SL bit is set, mask the FL bitmap to find the next larger FL class using another `ffs`.
4. Read the head of the free list at `block[fl][sl]`.

The entire search is two bitmap scans and an array dereference.
`ffs` compiles to a single hardware instruction on all modern architectures
(`bsf`/`tzcnt` on x86, `clz` on ARM), making the lookup genuinely O(1):
not amortized, not expected-case, but worst-case constant time.

![TLSF Data Structure for Free Blocks](assets/data-structure.png)

### Block Layout

Each block in the pool has a minimal header:

```
 ┌─────────────────────────────────────────────────┐
 │ prev (pointer to previous block, if it is free) │  ← boundary tag
 ├─────────────────────────────────────────────────┤
 │ header: size | free_bit | prev_free_bit         │  ← 1 word overhead
 ├─────────────────────────────────────────────────┤
 │ next_free (only when block is free)             │
 │ prev_free (only when block is free)             │
 ├─────────────────────────────────────────────────┤
 │ ... payload ...                                 │
 └─────────────────────────────────────────────────┘
```

The `header` field stores the block size with two status bits packed into the least significant bits.
This works because all sizes are aligned to `ALIGN_SIZE` (8 bytes on 64-bit),
so the low bits are always zero, providing free storage for metadata.
Bit 0 indicates whether this block is free; bit 1 indicates whether the physically previous block is free.

The `prev` pointer is a _boundary tag_:
it is stored at the end of the previous block (overlapping with this block's struct layout),
and is only valid when the previous block is free.
This enables O(1) backward coalescing without walking the block chain.

The `next_free`/`prev_free` pointers exist only in free blocks,
forming the doubly-linked free list within each bin.
When a block is allocated, these fields become part of the user payload,
so no space is wasted.
The net overhead per allocation is exactly one word (`header`).

### Allocation

1. Round the requested size up to the next SL bin boundary
   (`round_block_size`), ensuring the block found will be at least as
   large as requested.
2. Map the rounded size to FL/SL indices (`mapping`) using bit
   manipulation. This is branch-free on this implementation,
   beneficial on in-order cores like Arm Cortex-M where branch misprediction stalls the pipeline.
3. Search the SL bitmap at that FL index for a set bit (`ffs`).
   If none, search the FL bitmap for the next larger class.
4. Pop the head block from the free list at `block[fl][sl]`.
5. If the block is larger than needed by at least `TLSF_SPLIT_THRESHOLD`
   (default `BLOCK_SIZE_MIN`), split it: the front becomes the allocation,
   the remainder is inserted back into the appropriate bin.

Worst case: small request from a pool with one huge free block.
Full bitmap scan + split + remainder insertion, yet still O(1).

### Deallocation

1. Mark the block as free (set bit 0 in `header`).
2. Check bit 1 (`prev_free`): if set, merge with the previous block by
   following the `prev` boundary tag for O(1) backward coalescing.
3. Check the next block's free bit: if set, merge forward.
4. Insert the (possibly merged) block into the appropriate free list and update the FL/SL bitmaps.

Coalescing is _immediate_: every free produces the largest possible contiguous block.
There is no deferred coalescing pass, no periodic compaction, and no garbage collection.
This eliminates latency spikes from batch reclamation, a critical property for hard real-time systems.

Worst case: block sandwiched between two free neighbors.
Two merges + two list removals + one insertion, yet still O(1).

### Sentinel Blocks

Each pool ends with a zero-size _sentinel_ block.
The sentinel terminates the physical block chain: `block_next()` never walks past it,
and `block_size() == 0` signals "last block." Sentinels are never inserted into the free list.

When `tlsf_append_pool` extends a pool with adjacent memory,
the old sentinel is repurposed into a regular free block and merged with its neighbors.
A new sentinel is placed at the end of the appended region.
This allows pools to grow without leaving dead gaps at boundaries.

### Reallocation

Four-phase strategy to minimize data movement:

1. Forward expansion: if the next physical block is free and large enough,
   absorb it with zero copy since the payload does not move.
2. Backward expansion: if the previous block is free and the combined size suffices,
   absorb it and `memmove` the payload backward.
3. Combined: merge prev + current + next when neither alone is enough but together they satisfy the request.
4. Fallback: `malloc` a new block, `memcpy`, `free` the old one.

Phases 1-3 avoid heap fragmentation by reusing adjacent space.
Phase 2 uses `memmove` (not `memcpy`) because source and destination overlap.

### Pool Modes

Dynamic pools grow on demand via a user-provided `tlsf_resize()`
callback.
Static pools (`tlsf_pool_init`) use a fixed memory region and never call `tlsf_resize`.
Both can be extended with `tlsf_append_pool` if adjacent memory is available.

The `tlsf_resize` function is declared as a weak symbol: static pool users need not define it at all.
Dynamic pool users must provide a strong definition
(typically backed by `mmap` or a platform-specific memory source);
without one, allocations silently return NULL.

Multiple independent allocator instances are supported by initializing separate `tlsf_t` structures with their own memory regions.

### Constants

| Constant | 64-bit | 32-bit | Notes |
|----------|--------|--------|-------|
| `TLSF_MAX_SIZE` | ~274 GB | ~2 GB | Reduced by `TLSF_MAX_POOL_BITS` |
| FL classes | 32 | 25 | `_TLSF_FL_MAX - _TLSF_FL_SHIFT + 1` |
| Alignment | 8 bytes | 4 bytes | |
| Min block | 16 bytes | 12 bytes | |
| Block overhead | 8 bytes | 4 bytes | |
| SL subdivisions | 32 | 32 | |

## WCET Measurement

The `tests/wcet.c` tool measures per-operation latency under pathological scenarios to bound TLSF's O(1) constant:

```shell
build/wcet -i 10000 -w 1000          # Standard measurement
build/wcet -i 10000 -C               # Cold-cache mode
build/wcet -i 10000 -c               # CSV output
build/wcet -i 10000 -r samples.csv   # Raw samples for plotting
```

Timing uses `rdtsc` (x86-64), `cntvct_el0` (ARM64), or `mach_absolute_time` (macOS).
Reports min, p50, p90, p99, p99.9, max, mean, and stddev.

## Reference

M. Masmano, I. Ripoll, A. Crespo, and J. Real.
TLSF: a new dynamic memory allocator for real-time systems.
In Proc. ECRTS (2004), IEEE Computer Society, pp. 79-86.

## Related Projects

* [tlsf-pmr](https://github.com/LiemDQ/tlsf-pmr): C++17 PMR allocator using TLSF

## Licensing

TLSF-BSD is freely redistributable under the 3-clause BSD License.
Use of this source code is governed by a BSD-style license that can be found
in the [LICENSE](LICENSE) file.
