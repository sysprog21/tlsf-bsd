/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <string.h>

#include "tlsf.h"

#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), false)
#endif

/* All allocation sizes and addresses are aligned. */
#define ALIGN_SIZE ((size_t) 1 << ALIGN_SHIFT)
#if __SIZE_WIDTH__ == 64
#define ALIGN_SHIFT 3
#else
#define ALIGN_SHIFT 2
#endif

/* First level (FL) and second level (SL) counts */
#define SL_SHIFT 5
#define SL_COUNT (1U << SL_SHIFT)
#define FL_MAX _TLSF_FL_MAX
#define FL_SHIFT (SL_SHIFT + ALIGN_SHIFT)
#define FL_COUNT (FL_MAX - FL_SHIFT + 1)

/* Block status bits are stored in the least significant bits (LSB) of the
 * size field.
 */
#define BLOCK_BIT_FREE ((size_t) 1)
#define BLOCK_BIT_PREV_FREE ((size_t) 2)
#define BLOCK_BITS (BLOCK_BIT_FREE | BLOCK_BIT_PREV_FREE)

/* A free block must be large enough to store its header minus the size of the
 * prev field.
 */
#define BLOCK_OVERHEAD (sizeof(size_t))
#define BLOCK_SIZE_MIN (sizeof(tlsf_block_t) - sizeof(tlsf_block_t *))
#define BLOCK_SIZE_MAX ((size_t) 1 << (FL_MAX - 1))
#define BLOCK_SIZE_SMALL ((size_t) 1 << FL_SHIFT)

/* Minimum remainder size for trimming. Raising this above BLOCK_SIZE_MIN
 * avoids creating tiny free blocks that waste metadata overhead relative
 * to their usable payload, trading internal fragmentation for fewer
 * unusable fragments. Default: BLOCK_SIZE_MIN (current behavior).
 */
#ifndef TLSF_SPLIT_THRESHOLD
#define TLSF_SPLIT_THRESHOLD BLOCK_SIZE_MIN
#endif

#ifndef ASSERT
#ifdef TLSF_ENABLE_ASSERT
#include <assert.h>
#define ASSERT(cond, msg) assert((cond) && msg)
#else
#define ASSERT(cond, msg)
#endif
#endif

#ifndef INLINE
#define INLINE static inline __attribute__((always_inline))
#endif

typedef struct tlsf_block tlsf_block_t;

_Static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8,
               "size_t must be 32 or 64 bit");
_Static_assert(sizeof(size_t) == sizeof(void *),
               "size_t must equal pointer size");
_Static_assert(ALIGN_SIZE == BLOCK_SIZE_SMALL / SL_COUNT,
               "sizes are not properly set");
_Static_assert(BLOCK_SIZE_MIN < BLOCK_SIZE_SMALL,
               "min allocation size is wrong");
_Static_assert(BLOCK_SIZE_MAX == TLSF_MAX_SIZE + BLOCK_OVERHEAD,
               "max allocation size is wrong");
_Static_assert(FL_COUNT <= 32, "index too large");
_Static_assert(SL_COUNT <= 32, "index too large");
_Static_assert(FL_COUNT == _TLSF_FL_COUNT, "invalid level configuration");
_Static_assert(SL_COUNT == _TLSF_SL_COUNT, "invalid level configuration");
_Static_assert(TLSF_SPLIT_THRESHOLD >= BLOCK_SIZE_MIN,
               "split threshold must be at least minimum block size");
_Static_assert(_TLSF_FL_COUNT >= 1,
               "TLSF_MAX_POOL_BITS too small for this architecture");
_Static_assert(FL_MAX < __SIZE_WIDTH__,
               "TLSF_MAX_POOL_BITS must be less than pointer width");

/*
 * Default (weak) implementation of tlsf_resize.
 * Users of tlsf_pool_init() need not provide their own.
 * Users of the dynamic growth API must provide a strong definition;
 * without one, dynamic allocations via TLSF_INIT will return NULL.
 *
 * Note: __attribute__((weak)) requires GCC or Clang.  On compilers
 * without weak symbol support, users must always define tlsf_resize.
 */
__attribute__((weak)) void *tlsf_resize(tlsf_t *t, size_t size)
{
    (void) t;
    (void) size;
    return NULL;
}

INLINE uint32_t bitmap_ffs(uint32_t x)
{
    ASSERT(x, "no set bit found");
    return (uint32_t) __builtin_ctz(x);
}

INLINE uint32_t log2floor(size_t x)
{
    ASSERT(x > 0, "log2 of zero");
#if __SIZE_WIDTH__ == 64
    return (uint32_t) (63 - (uint32_t) __builtin_clzll((unsigned long long) x));
#else
    return (uint32_t) (31 - (uint32_t) __builtin_clzl((unsigned long) x));
#endif
}

INLINE size_t block_size(const tlsf_block_t *block)
{
    return block->header & ~BLOCK_BITS;
}

INLINE void block_set_size(tlsf_block_t *block, size_t size)
{
    ASSERT(!(size % ALIGN_SIZE), "invalid size");
    block->header = size | (block->header & BLOCK_BITS);
}

INLINE bool block_is_free(const tlsf_block_t *block)
{
    return !!(block->header & BLOCK_BIT_FREE);
}

INLINE bool block_is_prev_free(const tlsf_block_t *block)
{
    return !!(block->header & BLOCK_BIT_PREV_FREE);
}

INLINE void block_set_prev_free(tlsf_block_t *block, bool free)
{
    block->header = free ? block->header | BLOCK_BIT_PREV_FREE
                         : block->header & ~BLOCK_BIT_PREV_FREE;
}

INLINE size_t align_up(size_t x, size_t align)
{
    ASSERT(align, "alignment must be non-zero");
    ASSERT(!(align & (align - 1)), "must align to a power of two");
    return (((x - 1) | (align - 1)) + 1);
}

/*
 * Align pointer while preserving pointer provenance.
 *
 * The naive approach '(char *) align_up((size_t) p, align)' loses provenance
 * because the integer-to-pointer cast creates a pointer with no derivation
 * history. This causes issues with Miri, UBSan, and strict aliasing analysis.
 *
 * Instead, we compute the alignment offset and use pointer arithmetic:
 *   p + (aligned_addr - addr)
 * This preserves provenance because the result is derived from `p`.
 *
 * Note: uintptr_t is the canonical type for pointer-to-integer round-trips.
 */
INLINE char *align_ptr(char *p, size_t align)
{
    uintptr_t addr = (uintptr_t) p;
    uintptr_t aligned_addr = align_up(addr, align);
    return p + (aligned_addr - addr);
}

INLINE char *block_payload(tlsf_block_t *block)
{
    return (char *) block + offsetof(tlsf_block_t, header) + BLOCK_OVERHEAD;
}

INLINE tlsf_block_t *to_block(void *ptr)
{
    tlsf_block_t *block = (tlsf_block_t *) ptr;
    ASSERT(block_payload(block) == align_ptr(block_payload(block), ALIGN_SIZE),
           "block not aligned properly");
    return block;
}

INLINE tlsf_block_t *block_from_payload(void *ptr)
{
    return to_block((char *) ptr - offsetof(tlsf_block_t, header) -
                    BLOCK_OVERHEAD);
}

/* Return location of previous block. */
INLINE tlsf_block_t *block_prev(const tlsf_block_t *block)
{
    ASSERT(block_is_prev_free(block), "previous block must be free");
    return block->prev;
}

/* Return location of next existing block. */
INLINE tlsf_block_t *block_next(tlsf_block_t *block)
{
    tlsf_block_t *next =
        to_block(block_payload(block) + block_size(block) - BLOCK_OVERHEAD);
    ASSERT(block_size(block), "block is last");
    return next;
}

/* Link a new block with its neighbor, return the neighbor. */
INLINE tlsf_block_t *block_link_next(tlsf_block_t *block)
{
    tlsf_block_t *next = block_next(block);
    next->prev = block;
    return next;
}

INLINE bool block_can_split(tlsf_block_t *block, size_t size)
{
    return block_size(block) >= sizeof(tlsf_block_t) + size;
}

/* When trimming, require the remainder to be at least TLSF_SPLIT_THRESHOLD
 * to avoid creating tiny free blocks that waste metadata overhead.
 */
INLINE bool block_can_trim(tlsf_block_t *block, size_t size)
{
    return block_size(block) >= BLOCK_OVERHEAD + TLSF_SPLIT_THRESHOLD + size;
}

INLINE void block_set_free(tlsf_block_t *block, bool free)
{
    ASSERT(block_is_free(block) != free, "block free bit unchanged");
    block->header =
        free ? block->header | BLOCK_BIT_FREE : block->header & ~BLOCK_BIT_FREE;
    block_set_prev_free(block_link_next(block), free);
}

/* Adjust allocation size to be aligned, and no smaller than internal minimum.
 * Check bounds BEFORE alignment to prevent integer overflow.
 * align_up() computes (((x-1) | (align-1)) + 1), which wraps to 0 when
 * x is near SIZE_MAX, bypassing subsequent TLSF_MAX_SIZE checks.
 */
INLINE size_t adjust_size(size_t size, size_t align)
{
    if (UNLIKELY(size > TLSF_MAX_SIZE))
        return size; /* Preserve huge value to fail caller's bounds check */
    size = align_up(size, align);
    return size < BLOCK_SIZE_MIN ? BLOCK_SIZE_MIN : size;
}

/* Round up to the next block size.
 * Branch-free: for small sizes (< BLOCK_SIZE_SMALL), the rounding mask
 * is zero, producing an identity.  For large sizes, it rounds up to
 * the next second-level bin boundary.
 */
INLINE size_t round_block_size(size_t size)
{
    uint32_t lg = log2floor(size);
    size_t is_large = (size_t) (lg >= (uint32_t) FL_SHIFT);
    /* Clamp shift to valid range; garbage value is harmless when is_large=0
     * because shifting zero by any valid amount yields zero.
     */
    uint32_t shift =
        (lg - (uint32_t) SL_SHIFT) & ((uint32_t) (__SIZE_WIDTH__ - 1));
    size_t round = is_large << shift;
    /* Large: (1 << shift) - 1 = SL rounding mask.  Small: 0 - 0 = 0. */
    size_t t = round - is_large;
    return (size + t) & ~t;
}

/* Map size to first-level (fl) and second-level (sl) bin indices.
 * Branch-free: bitmask selection handles small sizes (linear binning
 * in fl=0) and large sizes (logarithmic binning) without a conditional
 * branch.  Beneficial on in-order cores (e.g., Cortex-M) where branch
 * misprediction stalls the pipeline.
 */
INLINE void mapping(size_t size, uint32_t *fl, uint32_t *sl)
{
    uint32_t t = log2floor(size);
    /* All-ones mask when size is in the linear range (< BLOCK_SIZE_SMALL),
     * all-zeros when in the logarithmic range.
     */
    uint32_t small = -(uint32_t) (t < (uint32_t) FL_SHIFT);

    /* FL: 0 for small sizes, (t - FL_SHIFT + 1) for large sizes.
     * The wrapping subtraction when t < FL_SHIFT is harmless because
     * ~small masks it to zero.
     */
    *fl = ~small & (t - (uint32_t) FL_SHIFT + 1);

    /* SL: linear index for small, logarithmic for large.
     * Clamp the shift to avoid undefined behavior when t < SL_SHIFT;
     * the garbage result is masked out by `small`.
     */
    uint32_t shift =
        (t - (uint32_t) SL_SHIFT) & ((uint32_t) (__SIZE_WIDTH__ - 1));
    uint32_t sl_large = (uint32_t) (size >> shift) ^ SL_COUNT;
    uint32_t sl_small = (uint32_t) (size >> ALIGN_SHIFT);
    *sl = (~small & sl_large) | (small & sl_small);

    ASSERT(*fl < FL_COUNT, "wrong first level");
    ASSERT(*sl < SL_COUNT, "wrong second level");
}

/* Calculate the minimum block size for a given FL/SL bin */
INLINE size_t mapping_size(uint32_t fl, uint32_t sl)
{
    if (fl == 0)
        return sl * (BLOCK_SIZE_SMALL / SL_COUNT);

    size_t size = (size_t) 1 << (fl + FL_SHIFT - 1);
    return size + (sl * (size >> SL_SHIFT));
}

INLINE tlsf_block_t *block_find_suitable(tlsf_t *t, uint32_t *fl, uint32_t *sl)
{
    ASSERT(*fl < FL_COUNT, "wrong first level");
    ASSERT(*sl < SL_COUNT, "wrong second level");

    /* Search for a block in the list associated with the given fl/sl index. */
    uint32_t sl_map = t->sl[*fl] & (~0U << *sl);
    if (!sl_map) {
        /* No block exists. Search in the next largest first-level list. */
        uint32_t fl_map = t->fl & ((*fl + 1 >= 32) ? 0U : (~0U << (*fl + 1)));

        /* No free blocks available, memory has been exhausted. */
        if (UNLIKELY(!fl_map))
            return NULL;

        *fl = bitmap_ffs(fl_map);
        ASSERT(*fl < FL_COUNT, "wrong first level");

        sl_map = t->sl[*fl];
        ASSERT(sl_map, "second level bitmap is null");
    }

    *sl = bitmap_ffs(sl_map);
    ASSERT(*sl < SL_COUNT, "wrong second level");

    return t->block[*fl][*sl];
}

/* Remove a free block from the free list.
 * Unconditional writes: prev/next may be &t->block_null (sentinel),
 * in which case the writes are harmless.
 */
INLINE void remove_free_block(tlsf_t *t,
                              tlsf_block_t *block,
                              uint32_t fl,
                              uint32_t sl)
{
    ASSERT(fl < FL_COUNT, "wrong first level");
    ASSERT(sl < SL_COUNT, "wrong second level");

    tlsf_block_t *prev = block->prev_free;
    tlsf_block_t *next = block->next_free;
    next->prev_free = prev;
    prev->next_free = next;

    /* If this block is the head of the free list, set new head. */
    if (t->block[fl][sl] == block) {
        t->block[fl][sl] = next;

        /* If the new head is the sentinel, the bin is empty. */
        if (next == &t->block_null) {
            t->sl[fl] &= ~(1U << sl);

            /* If the second bitmap is now empty, clear the fl bitmap. */
            if (!t->sl[fl])
                t->fl &= ~(1U << fl);
        }
    }
}

/* Insert a free block into the free block list and mark the bitmaps.
 * Unconditional write: current may be &t->block_null (sentinel),
 * in which case the write to current->prev_free is harmless.
 */
INLINE void insert_free_block(tlsf_t *t,
                              tlsf_block_t *block,
                              uint32_t fl,
                              uint32_t sl)
{
    tlsf_block_t *current = t->block[fl][sl];
    ASSERT(block, "cannot insert a null entry into the free list");
    block->next_free = current;
    block->prev_free = &t->block_null;
    current->prev_free = block;
    t->block[fl][sl] = block;
    t->fl |= 1U << fl;
    t->sl[fl] |= 1U << sl;
}

/* Remove a given block from the free list. */
INLINE void block_remove(tlsf_t *t, tlsf_block_t *block)
{
    uint32_t fl, sl;
    mapping(block_size(block), &fl, &sl);
    remove_free_block(t, block, fl, sl);
}

/* Insert a given block into the free list. */
INLINE void block_insert(tlsf_t *t, tlsf_block_t *block)
{
    uint32_t fl, sl;
    mapping(block_size(block), &fl, &sl);
    insert_free_block(t, block, fl, sl);
}

/* Split a block into two, the second of which is free. */
INLINE tlsf_block_t *block_split(tlsf_block_t *block, size_t size)
{
    tlsf_block_t *rest = to_block(block_payload(block) + size - BLOCK_OVERHEAD);
    size_t rest_size = block_size(block) - (size + BLOCK_OVERHEAD);
    ASSERT(block_size(block) == rest_size + size + BLOCK_OVERHEAD,
           "rest block size is wrong");
    ASSERT(rest_size >= BLOCK_SIZE_MIN, "block split with invalid size");
    rest->header = rest_size;
    ASSERT(!(rest_size % ALIGN_SIZE), "invalid block size");
    block_set_free(rest, true);
    block_set_size(block, size);
    return rest;
}

/* Absorb a free block's storage into an adjacent previous free block. */
INLINE tlsf_block_t *block_absorb(tlsf_block_t *prev, tlsf_block_t *block)
{
    ASSERT(block_size(prev), "previous block can't be last");
    /* Note: Leaves flags untouched. */
    prev->header += block_size(block) + BLOCK_OVERHEAD;
    block_link_next(prev);
    return prev;
}

/* Merge a just-freed block with an adjacent previous free block. */
INLINE tlsf_block_t *block_merge_prev(tlsf_t *t, tlsf_block_t *block)
{
    if (block_is_prev_free(block)) {
        tlsf_block_t *prev = block_prev(block);
        ASSERT(prev, "prev block can't be null");
        ASSERT(block_is_free(prev),
               "prev block is not free though marked as such");
        block_remove(t, prev);
        block = block_absorb(prev, block);
    }
    return block;
}

/* Merge a just-freed block with an adjacent free block. */
INLINE tlsf_block_t *block_merge_next(tlsf_t *t, tlsf_block_t *block)
{
    tlsf_block_t *next = block_next(block);
    ASSERT(next, "next block can't be null");
    if (block_is_free(next)) {
        ASSERT(block_size(block), "previous block can't be last");
        block_remove(t, next);
        block = block_absorb(block, next);
    }
    return block;
}

/* Trim any trailing block space off the end of a block, return to pool. */
INLINE void block_rtrim_free(tlsf_t *t, tlsf_block_t *block, size_t size)
{
    ASSERT(block_is_free(block), "block must be free");
    if (!block_can_trim(block, size))
        return;
    tlsf_block_t *rest = block_split(block, size);
    block_link_next(block);
    block_set_prev_free(rest, true);
    block_insert(t, rest);
}

/* Trim any trailing block space off the end of a used block, return to pool. */
INLINE void block_rtrim_used(tlsf_t *t, tlsf_block_t *block, size_t size)
{
    ASSERT(!block_is_free(block), "block must be used");
    if (!block_can_trim(block, size))
        return;
    tlsf_block_t *rest = block_split(block, size);
    block_set_prev_free(rest, false);
    rest = block_merge_next(t, rest);
    block_insert(t, rest);
}

INLINE tlsf_block_t *block_ltrim_free(tlsf_t *t,
                                      tlsf_block_t *block,
                                      size_t size)
{
    ASSERT(block_is_free(block), "block must be free");
    ASSERT(block_can_split(block, size), "block is too small");
    tlsf_block_t *rest = block_split(block, size - BLOCK_OVERHEAD);
    block_set_prev_free(rest, true);
    block_link_next(block);
    block_insert(t, block);
    return rest;
}

INLINE void *block_use(tlsf_t *t, tlsf_block_t *block, size_t size)
{
    block_rtrim_free(t, block, size);
    block_set_free(block, false);
    return block_payload(block);
}

INLINE void check_sentinel(tlsf_block_t *block)
{
    (void) block;
    ASSERT(!block_size(block), "sentinel should be last");
    ASSERT(!block_is_free(block), "sentinel block should not be free");
}

static bool arena_grow(tlsf_t *t, size_t size)
{
    /* Static pools cannot grow. */
    if (t->arena)
        return false;

    /* First use of a dynamic pool: point all empty-bin pointers at the
     * sentinel so that insert/remove can write unconditionally.
     */
    if (!t->size) {
        for (uint32_t i = 0; i < FL_COUNT; i++)
            for (uint32_t j = 0; j < SL_COUNT; j++)
                t->block[i][j] = &t->block_null;
    }

    size_t req_size =
        (t->size ? t->size + BLOCK_OVERHEAD : 2 * BLOCK_OVERHEAD) + size;

    /* Pool cannot exceed the maximum addressable range for the configured
     * first-level index.  With reduced TLSF_MAX_POOL_BITS, this prevents
     * merged blocks from overflowing the mapping function.
     */
    if (UNLIKELY(req_size > (size_t) 1 << FL_MAX))
        return false;

    void *addr = tlsf_resize(t, req_size);
    if (!addr)
        return false;
    ASSERT((size_t) addr % ALIGN_SIZE == 0, "wrong heap alignment address");
    tlsf_block_t *block =
        to_block(t->size ? (char *) addr + t->size - 2 * BLOCK_OVERHEAD
                         : (char *) addr - BLOCK_OVERHEAD);
    if (!t->size)
        block->header = 0;
    check_sentinel(block);
    block->header |= size | BLOCK_BIT_FREE;
    block = block_merge_prev(t, block);
    block_insert(t, block);
    tlsf_block_t *sentinel = block_link_next(block);
    sentinel->header = BLOCK_BIT_PREV_FREE;
    t->size = req_size;
    check_sentinel(sentinel);
    return true;
}

static size_t arena_append_pool(tlsf_t *t, void *mem, size_t size)
{
    if (!t->size || !mem || size < 2 * BLOCK_OVERHEAD)
        return 0;

    /* Align memory block boundaries */
    char *start = align_ptr((char *) mem, ALIGN_SIZE);
    char *end = (char *) mem + size;
    size_t aligned_size = (size_t) (end - start) & ~(ALIGN_SIZE - 1);

    /* For static pools, the new sentinel must fit within the appended
     * region itself, since there is no backend to provide extra bytes.
     */
    if (t->arena) {
        if (aligned_size <= BLOCK_OVERHEAD)
            return 0;
        aligned_size -= BLOCK_OVERHEAD;
    }

    if (aligned_size < 2 * BLOCK_OVERHEAD)
        return 0;

    /* Get current pool information */
    void *current_pool_start = t->arena ? t->arena : tlsf_resize(t, t->size);
    if (!current_pool_start)
        return 0;

    char *current_pool_end = (char *) current_pool_start + t->size;

    /* Only support coalescing if the new memory is immediately adjacent to the
     * current pool
     */
    if (start != current_pool_end)
        return 0;

    /* Update the pool size first to include the new memory.
     * We need aligned_size for payload + BLOCK_OVERHEAD for new sentinel.
     */
    size_t old_size = t->size;
    size_t new_total_size = t->size + aligned_size + BLOCK_OVERHEAD;

    /* Reject if expanded pool would exceed the maximum addressable range. */
    if (UNLIKELY(new_total_size > (size_t) 1 << FL_MAX))
        return 0;

    /* For dynamic pools, request the backend to extend.
     * For static pools, the caller provides adjacent memory directly.
     */
    if (!t->arena) {
        void *resized = tlsf_resize(t, new_total_size);
        if (!resized)
            return 0;
        current_pool_start = resized;
    }

    /* Update our pool size */
    t->size = new_total_size;

    /* Find the current sentinel block */
    tlsf_block_t *old_sentinel =
        to_block((char *) current_pool_start + old_size - 2 * BLOCK_OVERHEAD);
    check_sentinel(old_sentinel);

    /* Check if the block before the sentinel is free */
    tlsf_block_t *last_block = NULL;
    if (block_is_prev_free(old_sentinel)) {
        last_block = block_prev(old_sentinel);
        ASSERT(last_block && block_is_free(last_block),
               "last block should be free");
        /* Remove the last free block from lists since we'll recreate it */
        block_remove(t, last_block);
    }

    /* Calculate the new free block size.
     * The old sentinel header becomes the new block's header (not payload).
     * Payload is just the appended memory.
     */
    size_t new_free_size = aligned_size;
    tlsf_block_t *new_free_block;

    if (last_block) {
        /* Merge with the existing free block.
         * Absorb: last_block payload + old sentinel header + new memory.
         */
        new_free_size += block_size(last_block) + BLOCK_OVERHEAD;
        new_free_block = last_block;
    } else {
        /* Convert the old sentinel into the start of the new free block */
        new_free_block = old_sentinel;
    }

    /* Set up the new free block header */
    new_free_block->header = new_free_size | BLOCK_BIT_FREE;

    /* When !last_block, the previous block is allocated (otherwise
     * block_is_prev_free(old_sentinel) would have been true and we would
     * have taken the last_block path).  BLOCK_BIT_PREV_FREE is already
     * clear from the header assignment above.
     *
     * Do NOT write new_free_block->prev: it physically overlaps with the
     * previous allocated block's payload tail (by TLSF block layout, the
     * next block's prev field sits in the last sizeof(void *) bytes of
     * the current block's payload).  The prev field is only read through
     * block_prev(), which asserts block_is_prev_free() first.
     */

    /* Insert the new free block into the appropriate list */
    block_insert(t, new_free_block);

    /* Create a new sentinel at the end */
    tlsf_block_t *new_sentinel = block_link_next(new_free_block);
    new_sentinel->header = BLOCK_BIT_PREV_FREE;
    check_sentinel(new_sentinel);

    return aligned_size;
}

static void arena_shrink(tlsf_t *t, tlsf_block_t *block)
{
    check_sentinel(block_next(block));
    size_t size = block_size(block);
    ASSERT(t->size + BLOCK_OVERHEAD >= size, "invalid heap size before shrink");
    t->size = t->size - size - BLOCK_OVERHEAD;
    if (t->size == BLOCK_OVERHEAD)
        t->size = 0;
    tlsf_resize(t, t->size);
    if (t->size) {
        block->header = 0;
        check_sentinel(block);
    }
}

INLINE tlsf_block_t *block_find_free(tlsf_t *t, size_t *size)
{
    *size = round_block_size(*size);
    uint32_t fl, sl;
    mapping(*size, &fl, &sl);
    tlsf_block_t *block = block_find_suitable(t, &fl, &sl);
    if (UNLIKELY(!block)) {
        if (!arena_grow(t, *size))
            return NULL;
        block = block_find_suitable(t, &fl, &sl);
        ASSERT(block, "no block found");
    }

    /* Update size to match the FL/SL bin that was actually used.
     * This ensures that when the block is freed, it will be placed in the same
     * bin it was allocated from.
     */
    *size = mapping_size(fl, sl);
    ASSERT(block_size(block) >= *size, "insufficient block size");
    remove_free_block(t, block, fl, sl);
    return block;
}

void *tlsf_malloc(tlsf_t *t, size_t size)
{
    size = adjust_size(size, ALIGN_SIZE);
    if (UNLIKELY(size > TLSF_MAX_SIZE))
        return NULL;

    /* Fast path: small sizes (FL=0) use linear SL mapping directly.
     * FL=0 bins are spaced at ALIGN_SIZE granularity, so we can skip
     * log2floor, round_block_size, and mapping entirely.
     */
    if (size < BLOCK_SIZE_SMALL) {
        uint32_t sl = (uint32_t) (size >> ALIGN_SHIFT);
        uint32_t sl_map = t->sl[0] & (~0U << sl);
        if (sl_map) {
            uint32_t found_sl = bitmap_ffs(sl_map);
            /* Use the bin's minimum size so mapping(block_size) returns
             * the same bin on free.
             */
            size = (size_t) found_sl << ALIGN_SHIFT;
            tlsf_block_t *block = t->block[0][found_sl];
            remove_free_block(t, block, 0, found_sl);
            return block_use(t, block, size);
        }
        /* Fall through: search larger FL classes via generic path */
    }

    tlsf_block_t *block = block_find_free(t, &size);
    if (UNLIKELY(!block))
        return NULL;
    return block_use(t, block, size);
}

void *tlsf_aalloc(tlsf_t *t, size_t align, size_t size)
{
    size_t adjust = adjust_size(size, ALIGN_SIZE);

    if (UNLIKELY(
            !align || (align & (align - 1)) /* align must be power of two */
            || align > TLSF_MAX_SIZE || sizeof(tlsf_block_t) > TLSF_MAX_SIZE ||
            adjust > TLSF_MAX_SIZE - align -
                         sizeof(tlsf_block_t) /* size is too large */))
        return NULL;

    if (align <= ALIGN_SIZE)
        return tlsf_malloc(t, size);

    size_t asize =
        adjust_size(adjust + align - 1 + sizeof(tlsf_block_t), align);
    tlsf_block_t *block = block_find_free(t, &asize);
    if (UNLIKELY(!block))
        return NULL;

    char *mem = align_ptr(block_payload(block) + sizeof(tlsf_block_t), align);
    block = block_ltrim_free(t, block, (size_t) (mem - block_payload(block)));
    return block_use(t, block, adjust);
}

void tlsf_free(tlsf_t *t, void *mem)
{
    if (UNLIKELY(!mem))
        return;

    tlsf_block_t *block = block_from_payload(mem);
    ASSERT(!block_is_free(block), "block already marked as free");

    block_set_free(block, true);
    block = block_merge_prev(t, block);
    block = block_merge_next(t, block);

    if (UNLIKELY(!block_size(block_next(block))) && !t->arena)
        arena_shrink(t, block);
    else
        block_insert(t, block);
}

void *tlsf_realloc(tlsf_t *t, void *mem, size_t size)
{
    /* Zero-size requests are treated as free. */
    if (UNLIKELY(mem && !size)) {
        tlsf_free(t, mem);
        return NULL;
    }

    /* Null-pointer requests are treated as malloc. */
    if (UNLIKELY(!mem))
        return tlsf_malloc(t, size);

    tlsf_block_t *block = block_from_payload(mem);
    size_t avail = block_size(block);
    size = adjust_size(size, ALIGN_SIZE);
    if (UNLIKELY(size > TLSF_MAX_SIZE))
        return NULL;

    ASSERT(!block_is_free(block), "block already marked as free");

    /* Do we need to expand? */
    if (size > avail) {
        tlsf_block_t *next = block_next(block);
        bool next_free = block_is_free(next);
        size_t next_size = next_free ? block_size(next) + BLOCK_OVERHEAD : 0;

        /* Try forward expansion first (no data movement required). */
        if (next_free && size <= avail + next_size) {
            block_merge_next(t, block);
            block_set_prev_free(block_next(block), false);
        }
        /* Try backward expansion (requires memmove). */
        else if (block_is_prev_free(block)) {
            tlsf_block_t *prev = block_prev(block);
            size_t prev_size = block_size(prev);
            size_t combined = prev_size + avail + BLOCK_OVERHEAD;

            /* Can also merge with next if it's free. */
            if (next_free)
                combined += next_size;

            if (size <= combined) {
                /* Remove prev from free list. */
                block_remove(t, prev);

                /* Move data to prev's payload area (regions may overlap). */
                memmove(block_payload(prev), mem, avail);

                /* Merge prev + current: update size, preserve prev's prev_free
                 * bit. Result is a used block (not free).
                 */
                size_t new_size = prev_size + avail + BLOCK_OVERHEAD;
                prev->header = new_size | (prev->header & BLOCK_BIT_PREV_FREE);
                block_link_next(prev);

                /* Also merge next if it's free. */
                if (next_free) {
                    block_remove(t, next);
                    prev->header += block_size(next) + BLOCK_OVERHEAD;
                    block_link_next(prev);
                }

                /* Update next block's prev_free status (we're now used). */
                block_set_prev_free(block_next(prev), false);

                /* Switch to the merged block. */
                block = prev;
                mem = block_payload(block);
            } else {
                /* Combined space still insufficient, must relocate. */
                void *dst = tlsf_malloc(t, size);
                if (dst) {
                    memcpy(dst, mem, avail);
                    tlsf_free(t, mem);
                }
                return dst;
            }
        } else {
            /* No in-place expansion possible, must relocate. */
            void *dst = tlsf_malloc(t, size);
            if (dst) {
                memcpy(dst, mem, avail);
                tlsf_free(t, mem);
            }
            return dst;
        }
    }

    /* Trim the resulting block and return the pointer. */
    block_rtrim_used(t, block, size);
    return mem;
}

size_t tlsf_append_pool(tlsf_t *t, void *mem, size_t size)
{
    if (UNLIKELY(!t || !mem || !size))
        return 0;

    return arena_append_pool(t, mem, size);
}

size_t tlsf_pool_init(tlsf_t *t, void *mem, size_t bytes)
{
    if (!t || !mem)
        return 0;

    /* Zero-initialize the control structure, then point every bin at the
     * sentinel so that free-list insert/remove can write unconditionally.
     */
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < FL_COUNT; i++)
        for (uint32_t j = 0; j < SL_COUNT; j++)
            t->block[i][j] = &t->block_null;

    /* Align pool start */
    char *start = align_ptr((char *) mem, ALIGN_SIZE);
    size_t adj = (size_t) (start - (char *) mem);
    if (bytes <= adj)
        return 0;

    /* Compute usable pool size (aligned down) */
    size_t pool_bytes = (bytes - adj) & ~(ALIGN_SIZE - 1);
    if (pool_bytes < 2 * BLOCK_OVERHEAD + BLOCK_SIZE_MIN)
        return 0;

    size_t free_size = pool_bytes - 2 * BLOCK_OVERHEAD;
    free_size &= ~(ALIGN_SIZE - 1);
    if (free_size < BLOCK_SIZE_MIN || free_size > BLOCK_SIZE_MAX)
        return 0;

    /* Mark as static (fixed-size) pool */
    t->arena = start;

    /* Set up the initial free block.
     * The block struct starts at start - BLOCK_OVERHEAD so that
     * block->header aligns with start.  The prev field sits before
     * the arena and is never accessed for the first block.
     */
    tlsf_block_t *block = to_block(start - BLOCK_OVERHEAD);
    block->header = free_size | BLOCK_BIT_FREE;
    block_insert(t, block);

    /* Set up sentinel at the end of the free block */
    tlsf_block_t *sentinel = block_link_next(block);
    sentinel->header = BLOCK_BIT_PREV_FREE;

    t->size = free_size + 2 * BLOCK_OVERHEAD;

    return free_size;
}

#ifdef TLSF_ENABLE_CHECK
#include <stdio.h>
#include <stdlib.h>
#define CHECK(cond, msg)                                          \
    ({                                                            \
        if (!(cond)) {                                            \
            fprintf(stderr, "TLSF CHECK: %s - %s\n", msg, #cond); \
            abort();                                              \
        }                                                         \
    })

/**
 * Comprehensive heap consistency check.
 *
 * Validates ALL block invariants by walking the entire heap:
 * 1. Block walk validation (all blocks from pool start to sentinel)
 * 2. Free list validation (bitmap consistency, coalescing, cycle/duplicate
 *    detection via Floyd's algorithm -- O(1) stack usage)
 * 3. Cross-validation (free list count matches block walk count)
 */
void tlsf_check(tlsf_t *t)
{
    CHECK(t, "tlsf_t pointer is null");

    /* Empty pool is valid */
    if (!t->size)
        return;

    /* Get arena start */
    void *arena_start = t->arena ? t->arena : tlsf_resize(t, t->size);
    CHECK(arena_start, "failed to get arena pointer");
    CHECK((size_t) arena_start % ALIGN_SIZE == 0, "arena not aligned");

    /*
     * Phase 1: Walk ALL blocks from pool start to sentinel
     * This validates the physical block chain integrity
     *
     * The first block is at arena_start - BLOCK_OVERHEAD because the
     * tlsf_block_t structure's prev field precedes the header, but for
     * the first block, the prev field is outside the arena (never accessed).
     */
    tlsf_block_t *block = to_block((char *) arena_start - BLOCK_OVERHEAD);
    tlsf_block_t *prev_block = NULL;
    size_t walk_free_count = 0;
    size_t total_size = 0;
    bool prev_was_free = false;

    while (block_size(block) != 0) {
        size_t bsize = block_size(block);

        /* Size invariants */
        CHECK(bsize >= BLOCK_SIZE_MIN, "block smaller than minimum size");
        CHECK(bsize <= BLOCK_SIZE_MAX, "block exceeds maximum size");
        CHECK(bsize % ALIGN_SIZE == 0, "block size not aligned");

        /* Pointer alignment check */
        CHECK((size_t) block % ALIGN_SIZE == 0, "block pointer not aligned");
        CHECK((size_t) block_payload(block) % ALIGN_SIZE == 0,
              "payload not aligned");

        /* Prev pointer validation */
        if (prev_block) {
            CHECK(block_is_prev_free(block) == prev_was_free,
                  "prev_free bit mismatch with actual previous block state");
            if (prev_was_free) {
                CHECK(block->prev == prev_block,
                      "prev pointer doesn't match previous block");
            }
        }

        if (block_is_free(block)) {
            walk_free_count++;

            /* Coalescing invariant: no two consecutive free blocks */
            CHECK(!prev_was_free,
                  "consecutive free blocks (coalescing failed)");

            /* Free-list membership verified by Phase 2/3 count match */
            prev_was_free = true;
        } else {
            prev_was_free = false;
        }

        total_size += bsize + BLOCK_OVERHEAD;
        prev_block = block;
        block = block_next(block);
    }

    /* Sentinel validation */
    CHECK(block_size(block) == 0, "sentinel has non-zero size");
    CHECK(!block_is_free(block), "sentinel marked as free");
    CHECK(block_is_prev_free(block) == prev_was_free,
          "sentinel prev_free bit mismatch");
    if (prev_was_free && prev_block) {
        CHECK(block->prev == prev_block, "sentinel prev pointer incorrect");
    }

    /* Account for sentinel header */
    total_size += BLOCK_OVERHEAD;
    CHECK(total_size == t->size, "block sizes don't sum to pool size");

    /*
     * Phase 2: Walk free lists and validate bitmap consistency
     */
    size_t list_free_count = 0;

    for (uint32_t i = 0; i < FL_COUNT; ++i) {
        uint32_t fl_bit = t->fl & (1U << i);
        uint32_t sl_list = t->sl[i];

        /* If FL bit is clear, all SL bits and block pointers must be
         * the sentinel.
         */
        if (!fl_bit) {
            CHECK(sl_list == 0, "SL bitmap non-zero but FL bit is clear");
            for (uint32_t j = 0; j < SL_COUNT; ++j) {
                CHECK(t->block[i][j] == &t->block_null,
                      "block pointer not sentinel but FL bit is clear");
            }
            continue;
        }

        /* FL bit is set, so at least one SL bit must be set */
        CHECK(sl_list != 0, "FL bit set but SL bitmap is empty");

        for (uint32_t j = 0; j < SL_COUNT; ++j) {
            uint32_t sl_bit = sl_list & (1U << j);
            tlsf_block_t *list_block = t->block[i][j];

            if (!sl_bit) {
                CHECK(list_block == &t->block_null,
                      "block pointer not sentinel but SL bit is clear");
                continue;
            }

            /* SL bit is set, so block list must be non-empty */
            CHECK(list_block != &t->block_null,
                  "SL bit set but block list is empty (sentinel)");

            /* Walk the free list for this bin.
             * Floyd's cycle detection runs in parallel: a fast pointer
             * advances two steps per iteration.  If a duplicate block
             * creates a cycle, slow and fast will collide in O(n) steps.
             * This replaces the former 16 KB hash-table approach with
             * O(1) stack usage -- critical for embedded/RTOS targets.
             *
             * Cross-bin duplicates are already caught above: Phase 2
             * validates that each block maps to its bin, so a block
             * cannot appear in two different bins without failing the
             * fl/sl check first.
             */
            tlsf_block_t *list_prev = &t->block_null;
            tlsf_block_t *fast = list_block;
            while (list_block != &t->block_null) {
                list_free_count++;

                /* Block must be free */
                CHECK(block_is_free(list_block), "block in free list not free");

                /* Block must be in correct bin */
                uint32_t fl, sl;
                mapping(block_size(list_block), &fl, &sl);
                CHECK(fl == i && sl == j, "block in wrong FL/SL bin");

                /* Size constraints */
                CHECK(block_size(list_block) >= BLOCK_SIZE_MIN,
                      "free block below minimum size");

                /* Coalescing: previous physical block must not be free */
                CHECK(!block_is_prev_free(list_block),
                      "free block has free predecessor (coalescing violated)");

                /* Coalescing: next physical block must not be free */
                tlsf_block_t *next_phys = block_next(list_block);
                CHECK(!block_is_free(next_phys),
                      "free block has free successor (coalescing violated)");

                /* Next block must have prev_free set */
                CHECK(block_is_prev_free(next_phys),
                      "next block doesn't know this block is free");

                /* Free list linkage */
                CHECK(list_block->prev_free == list_prev,
                      "free list prev pointer incorrect");
                if (list_prev != &t->block_null) {
                    CHECK(list_prev->next_free == list_block,
                          "free list next pointer incorrect");
                }

                list_prev = list_block;
                list_block = list_block->next_free;

                /* Floyd's tortoise-and-hare cycle detection */
                if (fast != &t->block_null)
                    fast = fast->next_free;
                if (fast != &t->block_null)
                    fast = fast->next_free;
                CHECK(list_block == &t->block_null || list_block != fast,
                      "cycle in free list (duplicate block / double-free?)");
            }
        }
    }

    /*
     * Phase 3: Cross-validation
     */
    CHECK(walk_free_count == list_free_count,
          "free block count mismatch between block walk and free list walk");
}
#endif

/**
 * Collect heap statistics by walking all blocks.
 *
 * Note: This function relies on tlsf_resize(t, t->size) being idempotent
 * (returning the current arena address without reallocation). The platform-
 * specific tlsf_resize implementation must honor this contract.
 *
 * Statistics semantics:
 * - total_free/total_used: Payload bytes (usable by application)
 * - overhead: Metadata bytes (block headers + sentinel)
 * - block_count: Total blocks including used and free
 * - free_count: Number of free blocks (fragmentation indicator)
 */
int tlsf_get_stats(tlsf_t *t, tlsf_stats_t *stats)
{
    if (!t || !stats)
        return -1;

    stats->total_free = 0;
    stats->largest_free = 0;
    stats->total_used = 0;
    stats->block_count = 0;
    stats->free_count = 0;
    stats->overhead = 0;

    if (!t->size)
        return 0; /* Empty pool */

    /*
     * Get arena start.  For static pools, use the stored arena pointer.
     * For dynamic pools, query via tlsf_resize (which must return the
     * current arena pointer without reallocation or side effects).
     *
     * The first block is at arena_start - BLOCK_OVERHEAD because the
     * tlsf_block_t structure's prev field precedes the header.
     */
    void *arena_start = t->arena ? t->arena : tlsf_resize(t, t->size);
    if (!arena_start)
        return -1;

    tlsf_block_t *block = to_block((char *) arena_start - BLOCK_OVERHEAD);

    while (block_size(block) != 0) {
        size_t bsize = block_size(block);
        stats->block_count++;
        stats->overhead += BLOCK_OVERHEAD;

        if (block_is_free(block)) {
            stats->free_count++;
            stats->total_free += bsize;
            if (bsize > stats->largest_free)
                stats->largest_free = bsize;
        } else {
            stats->total_used += bsize;
        }

        block = block_next(block);
    }

    /* Account for sentinel block overhead */
    stats->overhead += BLOCK_OVERHEAD;

    return 0;
}
