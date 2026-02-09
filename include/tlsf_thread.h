/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * Thread-safe TLSF wrapper with fine-grained per-arena locking.
 *
 * Instead of a single coarse mutex around the entire allocator, the pool
 * is split into TLSF_ARENA_COUNT independent sub-pools (arenas), each
 * with its own lock.  Threads are mapped to arenas by a hash of their
 * thread identifier, so concurrent allocations from different threads
 * typically hit different locks with zero contention.
 *
 * Thread-safety contract (same as POSIX malloc/free):
 * - Different threads may call any API function concurrently.
 * - Concurrent operations on the SAME pointer are undefined behavior.
 *   Each live pointer must be owned by exactly one thread at a time;
 *   the owner may free or realloc it, but no other thread may simultaneously
 *   free, realloc, or read/write that pointer.
 * - init, destroy, and reset are not thread-safe with respect to other API
 *   calls on the same tlsf_thread_t instance.  Callers must ensure
 *   quiescence (no concurrent alloc/free/realloc) before calling them.
 *
 * Lock primitives are configurable: define TLSF_LOCK_T and the associated
 * macros BEFORE including this header to use a platform-specific primitive
 * (FreeRTOS semaphore, Zephyr k_mutex, bare-metal spinlock, etc.).
 * Default: POSIX pthread_mutex_t.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "tlsf.h"

#include <stddef.h>
#include <stdint.h>

/* Lock abstraction
 *
 * Override ALL six lock macros together before including this header.
 * When providing custom locks, also define TLSF_THREAD_HINT() to
 * return a thread-specific unsigned integer for arena selection.
 *
 * Example (FreeRTOS):
 *   #define TLSF_LOCK_T           SemaphoreHandle_t
 *   #define TLSF_LOCK_INIT(l)     (*(l) = xSemaphoreCreateMutex())
 *   #define TLSF_LOCK_DESTROY(l)  vSemaphoreDelete(*(l))
 *   #define TLSF_LOCK_ACQUIRE(l)  xSemaphoreTake(*(l), portMAX_DELAY)
 *   #define TLSF_LOCK_RELEASE(l)  xSemaphoreGive(*(l))
 *   #define TLSF_LOCK_TRY(l)      (xSemaphoreTake(*(l),0)==pdTRUE)
 *   #define TLSF_THREAD_HINT()    ((unsigned)uxTaskGetTaskNumber(NULL))
 *   #include "tlsf_thread.h"
 */

#ifndef TLSF_LOCK_T

#include <pthread.h>

#define TLSF_LOCK_T pthread_mutex_t
#define TLSF_LOCK_INIT(l) pthread_mutex_init((l), NULL)
#define TLSF_LOCK_DESTROY(l) pthread_mutex_destroy((l))
#define TLSF_LOCK_ACQUIRE(l) pthread_mutex_lock((l))
#define TLSF_LOCK_RELEASE(l) pthread_mutex_unlock((l))
#define TLSF_LOCK_TRY(l) (pthread_mutex_trylock((l)) == 0)

#ifndef TLSF_THREAD_HINT
/* Fold upper bits into lower 32 to retain entropy on 64-bit systems. */
#define TLSF_THREAD_HINT()                    \
    ((unsigned) ((uintptr_t) pthread_self() ^ \
                 ((uintptr_t) pthread_self() >> 16)))
#endif

#endif /* TLSF_LOCK_T */

/* Fallback thread hint for custom locks without a custom hint. */
#ifndef TLSF_THREAD_HINT
#define TLSF_THREAD_HINT() 0U
#endif

/*
 * Number of independent arenas.  Each arena has its own lock and TLSF
 * pool, so N arenas support up to N contention-free concurrent
 * allocations.
 *
 * Trade-offs:
 *   More arenas  -> lower contention, but memory is partitioned (one
 *                   arena can exhaust while others have space).
 *   Fewer arenas -> better memory utilization, higher contention.
 *
 * Must be >= 1.  Power of two recommended for efficient hash mapping.
 */
#ifndef TLSF_ARENA_COUNT
#define TLSF_ARENA_COUNT 4
#endif

_Static_assert(TLSF_ARENA_COUNT >= 1, "TLSF_ARENA_COUNT must be >= 1");

/*
 * Align each arena to a cache line to prevent false sharing between
 * arenas that would otherwise sit on the same line.  64 bytes is the
 * common L1 cache line size on x86-64 and ARMv8.
 */
#ifndef TLSF_CACHELINE_SIZE
#define TLSF_CACHELINE_SIZE 64
#endif

_Static_assert((TLSF_CACHELINE_SIZE & (TLSF_CACHELINE_SIZE - 1)) == 0,
               "TLSF_CACHELINE_SIZE must be a power of two");

typedef struct {
    tlsf_t pool;
    TLSF_LOCK_T lock;
    void *base;      /* Arena memory base (for pointer ownership) */
    size_t capacity; /* Arena memory size in bytes */
} __attribute__((aligned(TLSF_CACHELINE_SIZE))) tlsf_arena_t;

typedef struct {
    tlsf_arena_t arenas[TLSF_ARENA_COUNT];
    int count; /* Initialized arena count (<= TLSF_ARENA_COUNT) */
} tlsf_thread_t;

/**
 * Initialize from a contiguous memory region, splitting it into up to
 * TLSF_ARENA_COUNT independent sub-pools.  The arena count may be
 * reduced if the region is too small to support all arenas.
 *
 * @param ts    Thread-safe allocator instance
 * @param mem   Memory region
 * @param bytes Size of the memory region
 * @return Total usable bytes across all arenas, or 0 on failure
 */
size_t tlsf_thread_init(tlsf_thread_t *ts, void *mem, size_t bytes);

/**
 * Destroy: release lock resources.  Does not free the memory region
 * passed to tlsf_thread_init (caller retains ownership).
 */
void tlsf_thread_destroy(tlsf_thread_t *ts);

/**
 * Thread-safe malloc.  Tries the calling thread's preferred arena
 * first, then falls back to other arenas via non-blocking try-lock,
 * then blocking acquire.
 */
void *tlsf_thread_malloc(tlsf_thread_t *ts, size_t size);

/**
 * Thread-safe aligned allocation.
 */
void *tlsf_thread_aalloc(tlsf_thread_t *ts, size_t align, size_t size);

/**
 * Thread-safe realloc.  Attempts in-place realloc within the owning
 * arena first; falls back to cross-arena malloc + memcpy + free.
 */
void *tlsf_thread_realloc(tlsf_thread_t *ts, void *ptr, size_t size);

/**
 * Thread-safe free.  Finds the owning arena automatically via
 * pointer-range lookup (O(TLSF_ARENA_COUNT), effectively O(1)).
 */
void tlsf_thread_free(tlsf_thread_t *ts, void *ptr);

/**
 * Heap consistency check across all arenas.
 * Acquires each arena lock in order during the check.
 */
void tlsf_thread_check(tlsf_thread_t *ts);

/**
 * Aggregate statistics across all arenas.
 * largest_free reports the single largest free block in any arena.
 */
int tlsf_thread_stats(tlsf_thread_t *ts, tlsf_stats_t *stats);

/**
 * Reset all arenas to initial state (bounded time).
 * All outstanding pointers become invalid.
 */
void tlsf_thread_reset(tlsf_thread_t *ts);

#ifdef __cplusplus
}
#endif
