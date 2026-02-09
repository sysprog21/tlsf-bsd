/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * Thread-safe TLSF wrapper: per-arena fine-grained locking.
 *
 * See include/tlsf_thread.h for the design rationale and API
 * documentation.
 */

#include <string.h>

#include "tlsf_thread.h"

/*
 * Hash the thread hint to select a preferred arena.
 *
 * The mixing function distributes thread IDs that may differ only in
 * their low bits (sequential handles, page-aligned stacks) across all
 * arenas.
 */
static inline int arena_select(const tlsf_thread_t *ts)
{
    unsigned h = TLSF_THREAD_HINT();
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    return (int) (h % (unsigned) ts->count);
}

/*
 * Find which arena owns a pointer by range check.
 * O(TLSF_ARENA_COUNT) -- effectively O(1) for small N.
 * Returns -1 if the pointer is not from any arena.
 */
static inline int arena_find(const tlsf_thread_t *ts, const void *ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    for (int i = 0; i < ts->count; i++) {
        uintptr_t base = (uintptr_t) ts->arenas[i].base;
        if (p >= base && (p - base) < ts->arenas[i].capacity)
            return i;
    }
    return -1;
}

/*
 * Try to allocate from arenas other than `skip`, using non-blocking
 * try-lock first, then blocking acquire.  Returns NULL if all arenas
 * are exhausted.
 */
static void *arena_fallback_malloc(tlsf_thread_t *ts, int skip, size_t size)
{
    void *ptr;

    /* Phase 1: non-blocking scan */
    for (int i = 1; i < ts->count; i++) {
        int idx = (skip + i) % ts->count;
        if (TLSF_LOCK_TRY(&ts->arenas[idx].lock)) {
            ptr = tlsf_malloc(&ts->arenas[idx].pool, size);
            TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);
            if (ptr)
                return ptr;
        }
    }

    /* Phase 2: blocking scan */
    for (int i = 1; i < ts->count; i++) {
        int idx = (skip + i) % ts->count;
        TLSF_LOCK_ACQUIRE(&ts->arenas[idx].lock);
        ptr = tlsf_malloc(&ts->arenas[idx].pool, size);
        TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);
        if (ptr)
            return ptr;
    }

    return NULL;
}

static void *arena_fallback_aalloc(tlsf_thread_t *ts,
                                   int skip,
                                   size_t align,
                                   size_t size)
{
    void *ptr;

    for (int i = 1; i < ts->count; i++) {
        int idx = (skip + i) % ts->count;
        if (TLSF_LOCK_TRY(&ts->arenas[idx].lock)) {
            ptr = tlsf_aalloc(&ts->arenas[idx].pool, align, size);
            TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);
            if (ptr)
                return ptr;
        }
    }

    for (int i = 1; i < ts->count; i++) {
        int idx = (skip + i) % ts->count;
        TLSF_LOCK_ACQUIRE(&ts->arenas[idx].lock);
        ptr = tlsf_aalloc(&ts->arenas[idx].pool, align, size);
        TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);
        if (ptr)
            return ptr;
    }

    return NULL;
}

size_t tlsf_thread_init(tlsf_thread_t *ts, void *mem, size_t bytes)
{
    if (!ts || !mem || !bytes)
        return 0;

    memset(ts, 0, sizeof(*ts));

    /*
     * Determine how many arenas we can fit.  Reduce the count if the
     * per-arena share is too small for a viable TLSF pool.
     */
    int count = TLSF_ARENA_COUNT;
    size_t min_arena = 256;
    while (count > 1 && bytes / (unsigned) count < min_arena)
        count >>= 1;

    size_t per_arena =
        (bytes / (unsigned) count) & (size_t) ~(TLSF_CACHELINE_SIZE - 1);
    size_t total_usable = 0;
    char *base = (char *) mem;

    for (int i = 0; i < count; i++) {
        /* Last arena absorbs any remainder from integer division. */
        size_t chunk =
            (i == count - 1) ? bytes - (size_t) i * per_arena : per_arena;

        ts->arenas[i].base = base + (size_t) i * per_arena;
        ts->arenas[i].capacity = chunk;
        TLSF_LOCK_INIT(&ts->arenas[i].lock);

        size_t usable =
            tlsf_pool_init(&ts->arenas[i].pool, ts->arenas[i].base, chunk);
        if (!usable) {
            /* Cleanup previously initialized arenas. */
            for (int j = 0; j <= i; j++)
                TLSF_LOCK_DESTROY(&ts->arenas[j].lock);
            memset(ts, 0, sizeof(*ts));
            return 0;
        }
        total_usable += usable;
    }

    ts->count = count;
    return total_usable;
}

void tlsf_thread_destroy(tlsf_thread_t *ts)
{
    if (!ts)
        return;
    for (int i = 0; i < ts->count; i++)
        TLSF_LOCK_DESTROY(&ts->arenas[i].lock);
    ts->count = 0;
}

void *tlsf_thread_malloc(tlsf_thread_t *ts, size_t size)
{
    if (!ts->count)
        return NULL;

    int preferred = arena_select(ts);
    void *ptr;

    /* Fast path: thread-preferred arena. */
    TLSF_LOCK_ACQUIRE(&ts->arenas[preferred].lock);
    ptr = tlsf_malloc(&ts->arenas[preferred].pool, size);
    TLSF_LOCK_RELEASE(&ts->arenas[preferred].lock);
    if (ptr)
        return ptr;

    /* Slow path: try remaining arenas. */
    return arena_fallback_malloc(ts, preferred, size);
}

void *tlsf_thread_aalloc(tlsf_thread_t *ts, size_t align, size_t size)
{
    if (!ts->count)
        return NULL;

    int preferred = arena_select(ts);
    void *ptr;

    TLSF_LOCK_ACQUIRE(&ts->arenas[preferred].lock);
    ptr = tlsf_aalloc(&ts->arenas[preferred].pool, align, size);
    TLSF_LOCK_RELEASE(&ts->arenas[preferred].lock);
    if (ptr)
        return ptr;

    return arena_fallback_aalloc(ts, preferred, align, size);
}

void tlsf_thread_free(tlsf_thread_t *ts, void *ptr)
{
    if (!ptr)
        return;

    int idx = arena_find(ts, ptr);
    if (idx < 0)
        return;

    TLSF_LOCK_ACQUIRE(&ts->arenas[idx].lock);
    tlsf_free(&ts->arenas[idx].pool, ptr);
    TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);
}

void *tlsf_thread_realloc(tlsf_thread_t *ts, void *ptr, size_t size)
{
    if (!ptr)
        return tlsf_thread_malloc(ts, size);

    if (!size) {
        tlsf_thread_free(ts, ptr);
        return NULL;
    }

    int idx = arena_find(ts, ptr);
    if (idx < 0)
        return NULL;

    /*
     * Try in-place realloc within the owning arena.  We also grab
     * the old usable size while we hold the lock, in case we need
     * to do a cross-arena relocation afterwards.
     */
    size_t old_size;
    TLSF_LOCK_ACQUIRE(&ts->arenas[idx].lock);
    old_size = tlsf_usable_size(ptr);
    void *new_ptr = tlsf_realloc(&ts->arenas[idx].pool, ptr, size);
    TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);

    if (new_ptr)
        return new_ptr;

    /*
     * In-arena realloc failed (arena exhausted for the new size).
     * The old block is untouched.  Allocate from any arena, copy,
     * then free the original.
     */
    new_ptr = tlsf_thread_malloc(ts, size);
    if (!new_ptr)
        return NULL;

    size_t copy_size = old_size < size ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);

    TLSF_LOCK_ACQUIRE(&ts->arenas[idx].lock);
    tlsf_free(&ts->arenas[idx].pool, ptr);
    TLSF_LOCK_RELEASE(&ts->arenas[idx].lock);

    return new_ptr;
}

void tlsf_thread_check(tlsf_thread_t *ts)
{
    if (!ts)
        return;
    for (int i = 0; i < ts->count; i++) {
        TLSF_LOCK_ACQUIRE(&ts->arenas[i].lock);
        tlsf_check(&ts->arenas[i].pool);
        TLSF_LOCK_RELEASE(&ts->arenas[i].lock);
    }
}

int tlsf_thread_stats(tlsf_thread_t *ts, tlsf_stats_t *stats)
{
    if (!ts || !stats)
        return -1;

    memset(stats, 0, sizeof(*stats));

    for (int i = 0; i < ts->count; i++) {
        tlsf_stats_t arena_stats;
        TLSF_LOCK_ACQUIRE(&ts->arenas[i].lock);
        int rc = tlsf_get_stats(&ts->arenas[i].pool, &arena_stats);
        TLSF_LOCK_RELEASE(&ts->arenas[i].lock);
        if (rc < 0)
            return rc;

        stats->total_free += arena_stats.total_free;
        stats->total_used += arena_stats.total_used;
        stats->block_count += arena_stats.block_count;
        stats->free_count += arena_stats.free_count;
        stats->overhead += arena_stats.overhead;
        if (arena_stats.largest_free > stats->largest_free)
            stats->largest_free = arena_stats.largest_free;
    }

    return 0;
}

void tlsf_thread_reset(tlsf_thread_t *ts)
{
    if (!ts)
        return;
    for (int i = 0; i < ts->count; i++) {
        TLSF_LOCK_ACQUIRE(&ts->arenas[i].lock);
        tlsf_pool_reset(&ts->arenas[i].pool);
        TLSF_LOCK_RELEASE(&ts->arenas[i].lock);
    }
}
