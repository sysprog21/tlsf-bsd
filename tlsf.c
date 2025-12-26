/*
 * Copyright (c) 2006-2016, Matthew Conte
 * Copyright (c) 2017-2020, Daniel Mendler
 * All rights reserved.
 * Use of this source code is governed by a BSD-style license.
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
#define SL_SHIFT 4
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

typedef struct tlsf_block {
    /* Points to the previous block.
     * This field is only valid if the previous block is free and is actually
     * stored at the end of the previous block.
     */
    struct tlsf_block *prev;

    /* Size and block bits */
    size_t header;

    /* Next and previous free blocks.
     * These fields are only valid if the corresponding block is free.
     */
    struct tlsf_block *next_free, *prev_free;
} tlsf_block_t;

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

INLINE uint32_t bitmap_ffs(uint32_t x)
{
    uint32_t i = (uint32_t) __builtin_ffs((int32_t) x);
    ASSERT(i, "no set bit found");
    return i - 1U;
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
    ASSERT(!(align & (align - 1)), "must align to a power of two");
    return (((x - 1) | (align - 1)) + 1);
}

INLINE char *align_ptr(char *p, size_t align)
{
    return (char *) align_up((size_t) p, align);
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

INLINE void block_set_free(tlsf_block_t *block, bool free)
{
    ASSERT(block_is_free(block) != free, "block free bit unchanged");
    block->header =
        free ? block->header | BLOCK_BIT_FREE : block->header & ~BLOCK_BIT_FREE;
    block_set_prev_free(block_link_next(block), free);
}

/* Adjust allocation size to be aligned, and no smaller than internal minimum.
 */
INLINE size_t adjust_size(size_t size, size_t align)
{
    size = align_up(size, align);
    return size < BLOCK_SIZE_MIN ? BLOCK_SIZE_MIN : size;
}

/* Round up to the next block size */
INLINE size_t round_block_size(size_t size)
{
    size_t t = ((size_t) 1 << (log2floor(size) - SL_SHIFT)) - 1;
    return size >= BLOCK_SIZE_SMALL ? (size + t) & ~t : size;
}

INLINE void mapping(size_t size, uint32_t *fl, uint32_t *sl)
{
    if (size < BLOCK_SIZE_SMALL) {
        /* Store small blocks in first list. */
        *fl = 0;
        *sl = (uint32_t) size / (BLOCK_SIZE_SMALL / SL_COUNT);
    } else {
        uint32_t t = log2floor(size);
        *sl = (uint32_t) (size >> (t - SL_SHIFT)) ^ SL_COUNT;
        *fl = t - FL_SHIFT + 1;
    }
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

/* Remove a free block from the free list. */
INLINE void remove_free_block(tlsf_t *t,
                              tlsf_block_t *block,
                              uint32_t fl,
                              uint32_t sl)
{
    ASSERT(fl < FL_COUNT, "wrong first level");
    ASSERT(sl < SL_COUNT, "wrong second level");

    tlsf_block_t *prev = block->prev_free;
    tlsf_block_t *next = block->next_free;
    if (next)
        next->prev_free = prev;
    if (prev)
        prev->next_free = next;

    /* If this block is the head of the free list, set new head. */
    if (t->block[fl][sl] == block) {
        t->block[fl][sl] = next;

        /* If the new head is null, clear the bitmap. */
        if (!next) {
            t->sl[fl] &= ~(1U << sl);

            /* If the second bitmap is now empty, clear the fl bitmap. */
            if (!t->sl[fl])
                t->fl &= ~(1U << fl);
        }
    }
}

/* Insert a free block into the free block list and mark the bitmaps. */
INLINE void insert_free_block(tlsf_t *t,
                              tlsf_block_t *block,
                              uint32_t fl,
                              uint32_t sl)
{
    tlsf_block_t *current = t->block[fl][sl];
    ASSERT(block, "cannot insert a null entry into the free list");
    block->next_free = current;
    block->prev_free = 0;
    if (current)
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
    if (!block_can_split(block, size))
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
    if (!block_can_split(block, size))
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
    size_t req_size =
        (t->size ? t->size + BLOCK_OVERHEAD : 2 * BLOCK_OVERHEAD) + size;
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

    if (aligned_size < 2 * BLOCK_OVERHEAD)
        return 0;

    /* Get current pool information */
    void *current_pool_start =
        tlsf_resize(t, t->size); /* Ensure current pool is available */
    if (!current_pool_start)
        return 0;

    char *current_pool_end = (char *) current_pool_start + t->size;

    /* Only support coalescing if the new memory is immediately adjacent to the
     * current pool
     */
    if (start != current_pool_end)
        return 0;

    /* Update the pool size first to include the new memory */
    size_t old_size = t->size;
    size_t new_total_size = t->size + aligned_size;

    /* Try to resize the pool to include the new memory */
    void *resized_pool = tlsf_resize(t, new_total_size);
    if (!resized_pool)
        return 0;

    /* Update our pool size */
    t->size = new_total_size;

    /* Find the current sentinel block */
    tlsf_block_t *old_sentinel =
        to_block((char *) resized_pool + old_size - 2 * BLOCK_OVERHEAD);
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

    /* Calculate the new free block size */
    size_t new_free_size =
        aligned_size + BLOCK_OVERHEAD; /* Include old sentinel space */
    tlsf_block_t *new_free_block;

    if (last_block) {
        /* Merge with the existing free block */
        new_free_size += block_size(last_block) + BLOCK_OVERHEAD;
        new_free_block = last_block;
    } else {
        /* Convert the old sentinel into the start of the new free block */
        new_free_block = old_sentinel;
    }

    /* Set up the new free block header */
    new_free_block->header = new_free_size | BLOCK_BIT_FREE;

    /* Set up proper linking for the new free block */
    if (!last_block && old_size > 2 * BLOCK_OVERHEAD) {
        /* There's a previous block, find it by scanning backwards */
        char *scan_start = (char *) old_sentinel - BLOCK_OVERHEAD;
        char *pool_start = (char *) resized_pool;

        /* Simple backward scan to find the previous block */
        for (char *scan_ptr = scan_start; scan_ptr >= pool_start;
             scan_ptr -= ALIGN_SIZE) {
            tlsf_block_t *candidate = to_block(scan_ptr - BLOCK_OVERHEAD);
            if ((char *) candidate >= pool_start &&
                (char *) candidate + BLOCK_OVERHEAD + block_size(candidate) ==
                    (char *) old_sentinel) {
                new_free_block->prev = candidate;
                block_set_prev_free(new_free_block, block_is_free(candidate));
                break;
            }
        }
    }

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
    tlsf_block_t *block = block_find_free(t, &size);
    if (UNLIKELY(!block))
        return NULL;
    return block_use(t, block, size);
}

void *tlsf_aalloc(tlsf_t *t, size_t align, size_t size)
{
    size_t adjust = adjust_size(size, ALIGN_SIZE);

    if (UNLIKELY(
            !size ||
            ((align | size) & (align - 1)) /* align!=2**x, size!=n*align */ ||
            align > TLSF_MAX_SIZE || sizeof(tlsf_block_t) > TLSF_MAX_SIZE ||
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

    if (UNLIKELY(!block_size(block_next(block))))
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

    /* Do we need to expand to the next block? */
    if (size > avail) {
        /* If the next block is used or too small, we must relocate and copy. */
        tlsf_block_t *next = block_next(block);
        if (!block_is_free(next) ||
            size > avail + block_size(next) + BLOCK_OVERHEAD) {
            void *dst = tlsf_malloc(t, size);
            if (dst) {
                memcpy(dst, mem, avail);
                tlsf_free(t, mem);
            }
            return dst;
        }

        block_merge_next(t, block);
        block_set_prev_free(block_next(block), false);
    }

    /* Trim the resulting block and return the original pointer. */
    block_rtrim_used(t, block, size);
    return mem;
}

size_t tlsf_append_pool(tlsf_t *t, void *mem, size_t size)
{
    if (UNLIKELY(!t || !mem || !size))
        return 0;

    return arena_append_pool(t, mem, size);
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
 * Check for duplicate blocks in free lists (double-free detection).
 * Uses hash-set approach for O(n) complexity.
 *
 * Note: Uses stack allocation for reentrancy safety across multiple instances.
 * Table size of 2048 supports heaps with up to ~1400 free blocks (70% load).
 * For heaps exceeding this, the check is silently skipped.
 */
#define DUP_CHECK_TABLE_SIZE 2048
#define DUP_CHECK_MAX_LOAD 1433 /* ~70% of 2048 */
static void check_no_duplicates(tlsf_t *t, size_t free_count)
{
    /* Skip check if too many free blocks - would exceed hash table capacity */
    if (free_count > DUP_CHECK_MAX_LOAD)
        return;

    /* Stack allocation for reentrancy - 16KB on 64-bit systems */
    tlsf_block_t *seen[DUP_CHECK_TABLE_SIZE];

    /* Clear the table */
    memset(seen, 0, sizeof(seen));

    for (uint32_t i = 0; i < FL_COUNT; ++i) {
        for (uint32_t j = 0; j < SL_COUNT; ++j) {
            tlsf_block_t *block = t->block[i][j];
            while (block) {
                /* Hash the pointer using golden ratio for better distribution
                 */
                size_t hash = (((size_t) block >> 4) * 2654435769UL) %
                              DUP_CHECK_TABLE_SIZE;
                size_t orig_hash = hash;

                /* Linear probe to find empty slot or duplicate */
                while (seen[hash]) {
                    CHECK(seen[hash] != block,
                          "duplicate block in free list (double-free?)");
                    hash = (hash + 1) % DUP_CHECK_TABLE_SIZE;
                    CHECK(hash != orig_hash, "hash table unexpectedly full");
                }

                seen[hash] = block;
                block = block->next_free;
            }
        }
    }
}
#undef DUP_CHECK_MAX_LOAD
#undef DUP_CHECK_TABLE_SIZE

void tlsf_check(tlsf_t *t)
{
    size_t free_count = 0;

    for (uint32_t i = 0; i < FL_COUNT; ++i) {
        for (uint32_t j = 0; j < SL_COUNT; ++j) {
            size_t fl_map = t->fl & (1U << i), sl_list = t->sl[i],
                   sl_map = sl_list & (1U << j);
            tlsf_block_t *block = t->block[i][j];

            /* Check that first- and second-level lists agree. */
            if (!fl_map)
                CHECK(!sl_map, "second-level map must be null");

            if (!sl_map) {
                CHECK(!block, "block list must be null");
                continue;
            }

            /* Check that there is at least one free block. */
            CHECK(sl_list, "no free blocks in second-level map");

            while (block) {
                uint32_t fl, sl;
                CHECK(block_is_free(block), "block should be free");
                CHECK(!block_is_prev_free(block),
                      "blocks should have coalesced");
                CHECK(!block_is_free(block_next(block)),
                      "blocks should have coalesced");
                CHECK(block_is_prev_free(block_next(block)),
                      "block should be free");
                CHECK(block_size(block) >= BLOCK_SIZE_MIN,
                      "block not minimum size");

                mapping(block_size(block), &fl, &sl);
                CHECK(fl == i && sl == j, "block size indexed in wrong list");
                block = block->next_free;
                free_count++;
            }
        }
    }

    /* Check for duplicates in free lists (double-free detection) */
    check_no_duplicates(t, free_count);
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
     * Get arena start via tlsf_resize with current size.
     * Contract: tlsf_resize(t, t->size) must return current arena pointer
     * without reallocation or side effects.
     */
    void *arena_start = tlsf_resize(t, t->size);
    if (!arena_start)
        return -1;

    tlsf_block_t *block = to_block(arena_start);

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
