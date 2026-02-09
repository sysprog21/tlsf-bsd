/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

/* Inhibit C++ name-mangling for tlsf functions */
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>
#include <stdint.h>

/*
 * Second-level subdivisions: 32 bins per first-level class.
 * Max internal fragmentation bounded by 1/SL_COUNT = 3.125% (was 6.25%
 * with 16 bins). Control structure size increases ~2x for the block
 * pointer array.
 */
#define _TLSF_SL_COUNT 32

/*
 * Configurable maximum pool size: define TLSF_MAX_POOL_BITS to clamp
 * the first-level index, reducing the tlsf_t control structure size.
 * Pool cannot exceed 2^TLSF_MAX_POOL_BITS bytes.
 * E.g. -DTLSF_MAX_POOL_BITS=20 for a 1MB-max pool.
 */
#ifdef TLSF_MAX_POOL_BITS
#define _TLSF_FL_MAX TLSF_MAX_POOL_BITS
#else
#if __SIZE_WIDTH__ == 64
#define _TLSF_FL_MAX 39
#else
#define _TLSF_FL_MAX 31
#endif
#endif

/* FL_SHIFT = log2(SL_COUNT) + log2(ALIGN_SIZE) */
#if __SIZE_WIDTH__ == 64
#define _TLSF_FL_SHIFT 8
#else
#define _TLSF_FL_SHIFT 7
#endif
#define _TLSF_FL_COUNT (_TLSF_FL_MAX - _TLSF_FL_SHIFT + 1)
#define TLSF_MAX_SIZE (((size_t) 1 << (_TLSF_FL_MAX - 1)) - sizeof(size_t))
#define TLSF_INIT ((tlsf_t) {.size = 0})

/*
 * Block header structure.
 *
 * prev:      Pointer to the previous physical block.  Only valid when the
 *            previous block is free; physically stored at the tail of that
 *            block's payload.
 * header:    Size (upper bits) | status bits (lower 2 bits).
 * next_free: Next block in the same free list (only valid when free).
 * prev_free: Previous block in the same free list (only valid when free).
 */
struct tlsf_block {
    struct tlsf_block *prev;
    size_t header;
    struct tlsf_block *next_free, *prev_free;
};

typedef struct {
    uint32_t fl, sl[_TLSF_FL_COUNT];
    void *arena; /* Pool base address; non-NULL for fixed pools */
    size_t size;
    struct tlsf_block *block[_TLSF_FL_COUNT][_TLSF_SL_COUNT];
    struct tlsf_block block_null; /* Free-list sentinel (absorbs writes) */
} tlsf_t;

/**
 * Callback to grow or query the memory arena (dynamic pools only).
 * Users of tlsf_pool_init() need not provide this function.
 * A weak default returning NULL is provided in tlsf.c; dynamic pool
 * users MUST override it, otherwise allocations will silently fail.
 */
void *tlsf_resize(tlsf_t *, size_t);

/**
 * Allocate memory with a specified alignment.
 *
 * @param t     The TLSF allocator instance
 * @param align Alignment in bytes; must be a non-zero power of two
 * @param size  Requested allocation size in bytes; need not be a multiple of
 *              @align (follows POSIX posix_memalign semantics; C11
 *              aligned_alloc required size % align == 0 but C23 and
 *              common implementations dropped that constraint)
 * @return Pointer to at least @size bytes aligned to @align, or NULL on
 *         failure.  A zero @size request returns a unique minimum-sized
 *         allocation (consistent with tlsf_malloc).
 */
void *tlsf_aalloc(tlsf_t *, size_t align, size_t size);

/**
 * Append a memory block to an existing pool, potentially coalescing with
 * the last block if it's free. Returns the number of bytes actually used
 * from the memory block for pool expansion.
 *
 * @param tlsf The TLSF allocator instance
 * @param mem Pointer to the memory block to append
 * @param size Size of the memory block in bytes
 * @return Number of bytes used from the memory block, 0 on failure
 */
size_t tlsf_append_pool(tlsf_t *tlsf, void *mem, size_t size);

/**
 * Initialize the allocator with a fixed-size memory pool.
 * The pool will not auto-grow via tlsf_resize(); when the pool is
 * exhausted, allocations return NULL.  Callers may still extend the
 * pool explicitly via tlsf_append_pool() with adjacent memory.
 * This avoids the need to implement tlsf_resize().
 *
 * Multiple independent instances are supported by initializing separate
 * tlsf_t structures with their own memory regions.
 *
 * @param t     The TLSF allocator instance (will be zero-initialized)
 * @param mem   Pointer to the memory region to use as the pool
 * @param bytes Total size of the memory region in bytes
 * @return      Usable bytes in the pool, or 0 on failure
 */
size_t tlsf_pool_init(tlsf_t *t, void *mem, size_t bytes);

/**
 * Reset a static pool to its initial state, discarding all allocations.
 * Bounded-time bulk deallocation: clears bitmaps, recreates a single
 * free block.  Cost is O(FL_COUNT * SL_COUNT) for the bin reset, which
 * is fixed at compile time.
 *
 * Only valid for pools created with tlsf_pool_init().
 * Does nothing for dynamic pools or uninitialized instances.
 *
 * WARNING: All pointers previously returned by tlsf_malloc/aalloc/realloc
 * become invalid after reset.  Passing stale pointers to tlsf_free or
 * tlsf_realloc causes undefined behavior (silent metadata corruption in
 * release builds, assertion failure in debug builds).
 *
 * @param t The TLSF allocator instance
 */
void tlsf_pool_reset(tlsf_t *t);

/**
 * Allocate memory from the pool.
 *
 * @param t    The TLSF allocator instance
 * @param size Requested allocation size in bytes.  A zero @size request
 *             returns a unique minimum-sized allocation (POSIX-compatible
 *             behavior), not NULL.
 * @return Pointer to at least @size bytes, aligned to ALIGN_SIZE (8 on
 *         64-bit, 4 on 32-bit), or NULL on failure.
 */
void *tlsf_malloc(tlsf_t *, size_t size);
void *tlsf_realloc(tlsf_t *, void *, size_t);

/**
 * Releases the previously allocated memory, given the pointer.
 */
void tlsf_free(tlsf_t *, void *);

#ifdef TLSF_ENABLE_CHECK
void tlsf_check(tlsf_t *);
#else
static inline void tlsf_check(tlsf_t *t)
{
    (void) t;
}
#endif

/**
 * Heap statistics structure for monitoring allocator state.
 */
typedef struct {
    size_t total_free;   /* Total free bytes available */
    size_t largest_free; /* Largest contiguous free block */
    size_t total_used;   /* Total bytes in allocated blocks */
    size_t block_count;  /* Total number of blocks (free + used) */
    size_t free_count;   /* Number of free blocks (fragmentation indicator) */
    size_t overhead;     /* Metadata overhead bytes */
} tlsf_stats_t;

/**
 * Collect heap statistics by walking all blocks.
 * @param t The TLSF allocator instance
 * @param stats Output structure to fill with statistics
 * @return 0 on success, -1 if t or stats is NULL
 */
int tlsf_get_stats(tlsf_t *t, tlsf_stats_t *stats);

#ifdef __cplusplus
}
#endif
