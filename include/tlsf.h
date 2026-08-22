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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IMPORTANT: the configuration macros below (TLSF_MAX_POOL_BITS in particular)
 * change the layout and size of tlsf_t, which callers allocate themselves.
 * Every translation unit in a build -- src/tlsf.c and every consumer -- must
 * see identical definitions. A mismatch is not diagnosed: the struct silently
 * differs in size (8376 vs 3440 bytes for the default vs TLSF_MAX_POOL_BITS=20
 * on 64-bit) and accesses land at the wrong offsets. Define them in the build
 * system, never in a single .c file.
 *
 * TLSF_ENABLE_CHECK is a special case, and it is only half-safe. tlsf_check()
 * is an extern function when it is defined and a static inline no-op when it is
 * not, so a mismatch behaves differently depending on which side has it:
 *
 *   consumer has it, tlsf.c does not -> undefined reference at link time
 *   tlsf.c has it, consumer does not -> links fine, and every tlsf_check()
 *                                       call in the consumer silently becomes
 *                                       a no-op
 *
 * The second direction produces no diagnostic at all: the heap checking a
 * caller believes is enabled simply is not running. Set it uniformly.
 */

/* Second-level subdivisions: 32 bins per first-level class. Max internal
 * fragmentation bounded by 1/SL_COUNT = 3.125% (was 6.25% with 16 bins).
 * Control structure size increases ~2x for the block pointer array.
 */
#define _TLSF_SL_COUNT 32

#ifndef _TLSF_SIZE_WIDTH
#if defined(_MSC_VER)
#if defined(_WIN64)
#define _TLSF_SIZE_WIDTH 64
#else
#define _TLSF_SIZE_WIDTH 32
#endif
#elif defined(__GNUC__) || defined(__MINGW32__) || defined(__MINGW64__) || \
    defined(__clang__)
#define _TLSF_SIZE_WIDTH __SIZE_WIDTH__
#else
#if INTPTR_MAX == INT64_MAX
#define _TLSF_SIZE_WIDTH 64
#elif INTPTR_MAX == INT32_MAX
#define _TLSF_SIZE_WIDTH 32
#else
#error No support for non 32 or 64 bit systems
#endif
#endif
#endif

/* Configurable maximum pool size: define TLSF_MAX_POOL_BITS to clamp the
 * first-level index, reducing the tlsf_t control structure size. Pool cannot
 * exceed 2^TLSF_MAX_POOL_BITS bytes. E.g. -DTLSF_MAX_POOL_BITS=20 for a 1MB-max
 * pool.
 */
#ifdef TLSF_MAX_POOL_BITS
#define _TLSF_FL_MAX TLSF_MAX_POOL_BITS
#else
#if _TLSF_SIZE_WIDTH == 64
#define _TLSF_FL_MAX 39
#else
#define _TLSF_FL_MAX 31
#endif
#endif

/* FL_SHIFT = log2(SL_COUNT) + log2(ALIGN_SIZE) */
#if _TLSF_SIZE_WIDTH == 64
#define _TLSF_FL_SHIFT 8
#else
#define _TLSF_FL_SHIFT 7
#endif
#define _TLSF_FL_COUNT (_TLSF_FL_MAX - _TLSF_FL_SHIFT + 1)
#define TLSF_MAX_SIZE (((size_t) 1 << (_TLSF_FL_MAX - 1)) - sizeof(size_t))

/* Largest region tlsf_pool_init() accepts, for an ALIGN_SIZE-aligned pointer.
 * The pool's single initial free block cannot exceed 2^(_TLSF_FL_MAX - 1)
 * bytes, and the pool also carries that block's header and a sentinel.
 *
 * A region is accepted when its size, rounded down to the alignment, is at most
 * this; a few trailing bytes past it are tolerated and ignored. Note this is
 * about half of 2^_TLSF_FL_MAX, which bounds a *dynamic* arena instead.
 */
#define TLSF_MAX_POOL_BYTES \
    (((size_t) 1 << (_TLSF_FL_MAX - 1)) + 2 * sizeof(size_t))
#define TLSF_INIT ((tlsf_t) {.size = 0})

/* TLSF_INIT_STATIC is need to be used instead of TLSF_INIT for initializing
 * static objects for cross platform compatibility
 */
#if defined(_MSC_VER)
#define TLSF_INIT_STATIC {.size = 0}
#else
#define TLSF_INIT_STATIC ((tlsf_t) {.size = 0})
#endif

#ifndef TLSF_STATIC_ASSERT
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define TLSF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#elif defined(__clang__)
#if __has_extension(c_static_assert) || __has_feature(c_static_assert)
#define TLSF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#elif defined(__GNUC__) &&                                        \
    ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)) && \
    !defined(__STRICT_ANSI__)
#define TLSF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define TLSF_SASSERT_GLUE(a, b) a##b
#define TLSF_SASSERT_JOIN(a, b) TLSF_SASSERT_GLUE(a, b)
#define TLSF_STATIC_ASSERT(cond, msg)                             \
    typedef char TLSF_SASSERT_JOIN(STATIC_ASSERT_FAILED_AT_LINE_, \
                                   __LINE__)[(cond) ? 1 : -1]
#endif
#endif

/* Block header structure.
 *
 * prev: Pointer to the previous physical block. Only valid when the
 *            previous block is free; physically stored at the tail of that
 *            block's payload.
 * header: Size (upper bits) | status bits (lower 2 bits). next_free: Next block
 * in the same free list (only valid when free). prev_free: Previous block in
 * the same free list (only valid when free).
 */
struct tlsf_block {
    struct tlsf_block *prev;
    size_t header;
    struct tlsf_block *next_free, *prev_free;
};

typedef struct {
    uint32_t fl, sl[_TLSF_FL_COUNT];

    /* Fixed pool: memory is caller-owned (tlsf_pool_init) and the arena can
     * neither grow nor shrink. Orthogonal to `arena`, which is always the
     * current base address once the pool has any memory.
     */
    bool fixed;
    void *arena; /* Pool base address; NULL until a dynamic pool first grows */
    size_t size;
    struct tlsf_block *block[_TLSF_FL_COUNT][_TLSF_SL_COUNT];
    struct tlsf_block block_null; /* Free-list sentinel (absorbs writes) */
} tlsf_t;

/* Bytes of block metadata preceding a payload pointer: the prev field and the
 * header word. This is the one place the layout is written down for the public
 * contracts below; src/tlsf.c static-asserts that it matches the real block.
 */
#define _TLSF_PAYLOAD_OFFSET (sizeof(struct tlsf_block *) + sizeof(size_t))

/* A pointer previously returned by tlsf_malloc/aalloc/realloc. Callers must
 * keep the block metadata that precedes the payload intact, because free and
 * realloc step backwards onto it before touching the payload itself.
 */
/*@
  predicate tlsf_allocated{L}(void *ptr) =
    \valid((char *)ptr) &&
    \valid(((char *)ptr - _TLSF_PAYLOAD_OFFSET) +
           (0 .. _TLSF_PAYLOAD_OFFSET - 1));
*/

/**
 * Callback to grow or query the memory arena (dynamic pools only). Users of
 * tlsf_pool_init() need not provide this function. A weak default returning
 * NULL is provided in tlsf.c; dynamic pool users MUST override it, otherwise
 * allocations will silently fail.
 */
/*@
  requires \valid(t);
  requires size <= ((size_t)1 << _TLSF_FL_MAX);
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_resize(tlsf_t *t, size_t size);

/**
 * Allocate memory with a specified alignment.
 *
 * @param t The TLSF allocator instance
 * @param align Alignment in bytes; must be a non-zero power of two
 * @param size Requested allocation size in bytes; need not be a multiple of
 *              @align (follows POSIX posix_memalign semantics; C11
 *              aligned_alloc required size % align == 0 but C23 and
 *              common implementations dropped that constraint)
 * @return Pointer to at least @size bytes aligned to @align, or NULL on
 *         failure.  A zero @size request returns a unique minimum-sized
 *         allocation (consistent with tlsf_malloc).
 */
/*@
  requires \valid(t);
  ensures (align == 0 || (align & (align - 1)) != 0) ==> \result == \null;
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_aalloc(tlsf_t *t, size_t align, size_t size);

/**
 * Append a memory block to an existing pool, potentially coalescing with the
 * last block if it's free.
 *
 * Returns the number of bytes actually used from the memory block for pool
 * expansion.
 *
 * @tlsf : The TLSF allocator instance
 * @mem : Pointer to the memory block to append
 * @size : Size of the memory block in bytes
 *
 * Return Number of bytes used from the memory block, 0 on failure
 */
/*@
  requires \valid(tlsf);
  requires mem != \null ==> \valid((char *)mem);
  ensures \result <= size;
*/
size_t tlsf_append_pool(tlsf_t *tlsf, void *mem, size_t size);

/**
 * Initialize the allocator with a fixed-size memory pool. The pool will not
 * auto-grow via tlsf_resize(); when the pool is exhausted, allocations return
 * NULL. Callers may still extend the pool explicitly via tlsf_append_pool()
 * with adjacent memory. This avoids the need to implement tlsf_resize().
 *
 * Multiple independent instances are supported by initializing separate tlsf_t
 * structures with their own memory regions.
 *
 * @t : The TLSF allocator instance (will be zero-initialized)
 * @mem : Pointer to the memory region to use as the pool
 * @bytes : Total size of the memory region in bytes
 *
 * Return Usable bytes in the pool, or 0 on failure
 */
/*@
  requires \valid(t);
  requires mem != \null ==> \valid((char *)mem);
  ensures \result == 0 || t->fixed;
*/
size_t tlsf_pool_init(tlsf_t *t, void *mem, size_t bytes);

/**
 * Reset a static pool to its initial state, discarding all allocations.
 * Bounded-time bulk deallocation: clears bitmaps, recreates a single free
 * block. Cost is O(FL_COUNT * SL_COUNT) for the bin reset, which is fixed at
 * compile time.
 *
 * Only valid for pools created with tlsf_pool_init(). Does nothing for dynamic
 * pools or uninitialized instances.
 *
 * WARNING: All pointers previously returned by tlsf_malloc/aalloc/realloc
 * become invalid after reset. Passing stale pointers to tlsf_free or
 * tlsf_realloc causes undefined behavior (silent metadata corruption in release
 * builds, assertion failure in debug builds).
 *
 * @t : The TLSF allocator instance
 */
/*@ requires \valid(t); */
void tlsf_pool_reset(tlsf_t *t);

/**
 * Allocate memory from the pool.
 *
 * @param t The TLSF allocator instance
 * @param size Requested allocation size in bytes. A zero @size request
 *             returns a unique minimum-sized allocation (POSIX-compatible
 *             behavior), not NULL.
 * @return Pointer to at least @size bytes, aligned to ALIGN_SIZE (8 on
 *         64-bit, 4 on 32-bit), or NULL on failure.
 */
/*@
  requires \valid(t);
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_malloc(tlsf_t *t, size_t size);

/*@
  requires \valid(t);
  requires mem != \null ==> tlsf_allocated(mem);
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_realloc(tlsf_t *t, void *mem, size_t size);

/**
 * Releases the previously allocated memory, given the pointer.
 */
/*@
  requires \valid(t);
  requires mem != \null ==> tlsf_allocated(mem);
*/
void tlsf_free(tlsf_t *t, void *mem);

/**
 * Return the usable size of an existing allocation. The usable size may exceed
 * the originally requested size due to alignment rounding and bin-class
 * quantization. Equivalent to POSIX malloc_usable_size().
 *
 * @ptr : Pointer previously returned by tlsf_malloc/aalloc/realloc. Behavior is
 * undefined if ptr has been freed.
 *
 * Return Usable payload bytes, or 0 if ptr is NULL
 */
/*@
  requires ptr != \null ==> tlsf_allocated(ptr);
  ensures ptr == \null ==> \result == 0;
*/
size_t tlsf_usable_size(void *ptr);

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
 * @t : The TLSF allocator instance
 * @stats : Output structure to fill with statistics
 *
 * Return 0 on success, -1 if t or stats is NULL
 */
/*@
  behavior invalid:
    assumes t == \null || stats == \null;
    assigns \nothing;
    ensures \result == -1;
  behavior valid:
    assumes t != \null && stats != \null;
    requires \valid(t) && \valid(stats);
    ensures \result == 0 || \result == -1;
  complete behaviors;
  disjoint behaviors;
*/
int tlsf_get_stats(tlsf_t *t, tlsf_stats_t *stats);

#ifdef __cplusplus
}
#endif
