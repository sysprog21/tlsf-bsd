/*
 * SPDX-License-Identifier: BSD-3-Clause
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
#if __SIZE_WIDTH__ == 64
#define _TLSF_FL_COUNT 32
#define _TLSF_FL_MAX 39
#else
#define _TLSF_FL_COUNT 25
#define _TLSF_FL_MAX 31
#endif
#define TLSF_MAX_SIZE (((size_t) 1 << (_TLSF_FL_MAX - 1)) - sizeof(size_t))
#define TLSF_INIT ((tlsf_t) {.size = 0})

typedef struct {
    uint32_t fl, sl[_TLSF_FL_COUNT];
    struct tlsf_block *block[_TLSF_FL_COUNT][_TLSF_SL_COUNT];
    size_t size;
    void *arena; /* Pool base address; non-NULL for fixed (static) pools */
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
