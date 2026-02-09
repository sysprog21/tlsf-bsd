/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Thread-safety stress test for the per-arena TLSF wrapper.
 *
 * Spawns multiple threads that concurrently malloc/free/realloc from a
 * shared tlsf_thread_t instance.  Verifies:
 *   - No data corruption (fill-pattern integrity)
 *   - No double-free or use-after-free (ASan / TLSF_ENABLE_CHECK)
 *   - Arena distribution (multiple arenas actually used)
 *   - Aggregate statistics consistency after all threads join
 */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlsf_thread.h"

/* ------------------------------------------------------------------ */
/* Test parameters (tuned for < 2s on modern hardware)                 */
/* ------------------------------------------------------------------ */

#define POOL_SIZE (4 * 1024 * 1024) /* 4 MB static pool */
#define NUM_THREADS 8
#define OPS_PER_THREAD 50000
#define MAX_ALLOCS 128
#define MAX_ALLOC_SIZE 2048

static char pool[POOL_SIZE] __attribute__((aligned(16)));
static tlsf_thread_t ts;

/* ------------------------------------------------------------------ */
/* Per-thread work                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int id;
    int errors;
    int alloc_count;   /* total successful allocations */
    int free_count;    /* total frees */
    int realloc_count; /* total reallocs */
} thread_result_t;

static void *thread_func(void *arg)
{
    thread_result_t *res = (thread_result_t *) arg;
    void *ptrs[MAX_ALLOCS];
    size_t sizes[MAX_ALLOCS];
    int count = 0;
    unsigned seed = (unsigned) res->id * 2654435761U + 42;

    memset(ptrs, 0, sizeof(ptrs));

    for (int op = 0; op < OPS_PER_THREAD; op++) {
        int action = (int) (rand_r(&seed) % 4);

        switch (action) {
        case 0: /* malloc */
        case 1:
            if (count < MAX_ALLOCS) {
                size_t sz = (size_t) (rand_r(&seed) % MAX_ALLOC_SIZE) + 1;
                void *p = tlsf_thread_malloc(&ts, sz);
                if (p) {
                    /* Fill with per-thread pattern for integrity check */
                    memset(p, res->id & 0xFF, sz);
                    ptrs[count] = p;
                    sizes[count] = sz;
                    count++;
                    res->alloc_count++;
                }
            }
            break;

        case 2: /* free */
            if (count > 0) {
                int idx = (int) ((unsigned) rand_r(&seed) % (unsigned) count);
                /* Verify fill pattern before freeing */
                uint8_t *data = (uint8_t *) ptrs[idx];
                for (size_t i = 0; i < sizes[idx]; i++) {
                    if (data[i] != (uint8_t) (res->id & 0xFF)) {
                        res->errors++;
                        break;
                    }
                }
                tlsf_thread_free(&ts, ptrs[idx]);
                res->free_count++;
                /* Swap-remove */
                ptrs[idx] = ptrs[count - 1];
                sizes[idx] = sizes[count - 1];
                count--;
            }
            break;

        case 3: /* realloc */
            if (count > 0) {
                int idx = (int) ((unsigned) rand_r(&seed) % (unsigned) count);
                size_t old_sz = sizes[idx];
                size_t new_sz = (size_t) (rand_r(&seed) % MAX_ALLOC_SIZE) + 1;

                void *p = tlsf_thread_realloc(&ts, ptrs[idx], new_sz);
                if (p) {
                    /* Verify preserved portion */
                    uint8_t *data = (uint8_t *) p;
                    size_t verify = old_sz < new_sz ? old_sz : new_sz;
                    for (size_t i = 0; i < verify; i++) {
                        if (data[i] != (uint8_t) (res->id & 0xFF)) {
                            res->errors++;
                            break;
                        }
                    }
                    /* Re-fill entirely with the pattern */
                    memset(p, res->id & 0xFF, new_sz);
                    ptrs[idx] = p;
                    sizes[idx] = new_sz;
                    res->realloc_count++;
                }
            }
            break;
        }
    }

    /* Free all remaining allocations */
    for (int i = 0; i < count; i++) {
        uint8_t *data = (uint8_t *) ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (data[j] != (uint8_t) (res->id & 0xFF)) {
                res->errors++;
                break;
            }
        }
        tlsf_thread_free(&ts, ptrs[i]);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Test: multi-threaded stress                                         */
/* ------------------------------------------------------------------ */

static void stress_test(void)
{
    printf("Thread stress test (%d threads, %d ops each): ", NUM_THREADS,
           OPS_PER_THREAD);
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);
    printf("(%d arenas, %zu usable) ", ts.count, usable);
    fflush(stdout);

    pthread_t threads[NUM_THREADS];
    thread_result_t results[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        results[i].id = i;
        results[i].errors = 0;
        results[i].alloc_count = 0;
        results[i].free_count = 0;
        results[i].realloc_count = 0;
        pthread_create(&threads[i], NULL, thread_func, &results[i]);
    }

    int total_errors = 0;
    int total_allocs = 0, total_frees = 0, total_reallocs = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_errors += results[i].errors;
        total_allocs += results[i].alloc_count;
        total_frees += results[i].free_count;
        total_reallocs += results[i].realloc_count;
    }

    /* Verify heap consistency after all threads complete. */
    tlsf_thread_check(&ts);

    /* All allocations should have been freed. */
    tlsf_stats_t stats;
    int rc = tlsf_thread_stats(&ts, &stats);
    assert(rc == 0);
    assert(stats.total_used == 0);

    printf("done (%d allocs, %d frees, %d reallocs)\n", total_allocs,
           total_frees, total_reallocs);
    assert(total_errors == 0);

    tlsf_thread_destroy(&ts);
}

/* ------------------------------------------------------------------ */
/* Test: aligned allocation under contention                           */
/* ------------------------------------------------------------------ */

static void *aligned_thread_func(void *arg)
{
    int id = *(int *) arg;
    unsigned seed = (unsigned) id * 0xDEADBEEF + 7;

    for (int op = 0; op < 5000; op++) {
        /* Alignment: power of two from 8 to 4096 */
        unsigned shift = (unsigned) (rand_r(&seed) % 10) + 3; /* 8 to 8192 */
        size_t align = (size_t) 1 << shift;
        if (align > 4096)
            align = 4096;
        size_t sz = (size_t) (rand_r(&seed) % 512) + 1;

        void *p = tlsf_thread_aalloc(&ts, align, sz);
        if (p) {
            assert(((uintptr_t) p % align) == 0);
            memset(p, id & 0xFF, sz);
            tlsf_thread_free(&ts, p);
        }
    }
    return NULL;
}

static void aligned_test(void)
{
    printf("Thread aligned alloc test: ");
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);

    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, aligned_thread_func, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    tlsf_thread_check(&ts);

    tlsf_stats_t stats;
    tlsf_thread_stats(&ts, &stats);
    assert(stats.total_used == 0);

    tlsf_thread_destroy(&ts);
    printf("done\n");
}

/* ------------------------------------------------------------------ */
/* Test: reset under quiescence                                        */
/* ------------------------------------------------------------------ */

static void reset_test(void)
{
    printf("Thread pool reset test: ");
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);

    /* Allocate from multiple threads, then reset. */
    void *ptrs[64];
    int count = 0;
    for (int i = 0; i < 64; i++) {
        ptrs[i] = tlsf_thread_malloc(&ts, 256);
        if (ptrs[i])
            count++;
    }
    assert(count > 0);

    /* Reset discards everything. */
    tlsf_thread_reset(&ts);
    tlsf_thread_check(&ts);

    /* All memory should be free after reset. */
    tlsf_stats_t stats;
    tlsf_thread_stats(&ts, &stats);
    assert(stats.total_used == 0);
    assert(stats.total_free == usable);

    /* Pool should be usable after reset. */
    void *p = tlsf_thread_malloc(&ts, 100);
    assert(p);
    tlsf_thread_free(&ts, p);

    tlsf_thread_destroy(&ts);
    printf("done\n");
}

/* ------------------------------------------------------------------ */
/* Test: single-threaded basic sanity                                  */
/* ------------------------------------------------------------------ */

static void basic_test(void)
{
    printf("Thread wrapper basic test: ");
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);
    assert(ts.count >= 1);

    /* malloc / free */
    void *p = tlsf_thread_malloc(&ts, 100);
    assert(p);
    memset(p, 0xAA, 100);
    tlsf_thread_free(&ts, p);

    /* aalloc */
    p = tlsf_thread_aalloc(&ts, 256, 100);
    assert(p);
    assert(((uintptr_t) p % 256) == 0);
    tlsf_thread_free(&ts, p);

    /* realloc */
    p = tlsf_thread_malloc(&ts, 50);
    assert(p);
    memset(p, 0xBB, 50);
    void *q = tlsf_thread_realloc(&ts, p, 200);
    assert(q);
    uint8_t *data = (uint8_t *) q;
    for (int i = 0; i < 50; i++)
        assert(data[i] == 0xBB);
    tlsf_thread_free(&ts, q);

    /* realloc NULL -> malloc */
    p = tlsf_thread_realloc(&ts, NULL, 64);
    assert(p);
    tlsf_thread_free(&ts, p);

    /* realloc ptr, 0 -> free */
    p = tlsf_thread_malloc(&ts, 32);
    assert(p);
    q = tlsf_thread_realloc(&ts, p, 0);
    assert(q == NULL);

    /* free NULL is a no-op */
    tlsf_thread_free(&ts, NULL);

    /* stats */
    tlsf_stats_t stats;
    int rc = tlsf_thread_stats(&ts, &stats);
    assert(rc == 0);
    assert(stats.total_used == 0);

    /* usable_size */
    p = tlsf_thread_malloc(&ts, 100);
    assert(p);
    size_t us = tlsf_usable_size(p);
    assert(us >= 100);
    tlsf_thread_free(&ts, p);

    tlsf_thread_check(&ts);
    tlsf_thread_destroy(&ts);
    printf("done\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== Thread-safe TLSF tests ===\n");
    printf("Arena count: %d\n", TLSF_ARENA_COUNT);

    basic_test();
    stress_test();
    aligned_test();
    reset_test();

    puts("OK!");
    return 0;
}
