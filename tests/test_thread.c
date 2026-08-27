/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Thread-safety stress test for the per-arena TLSF wrapper.
 *
 * Spawns multiple threads that concurrently malloc/free/realloc from a shared
 * tlsf_thread_t instance. Verifies:
 *   - No data corruption (fill-pattern integrity)
 *   - No double-free or use-after-free (ASan / TLSF_ENABLE_CHECK)
 *   - Arena distribution (multiple arenas actually used)
 *   - Aggregate statistics consistency after all threads join
 *
 * It also carries weak_resize_test(), which belongs to the core allocator
 * rather than the wrapper. build/wcet links the weak tlsf_resize() too, but
 * drives static pools only, so arena_grow() returns before ever calling it.
 * This is the one binary that both links the weak default and reaches it.
 */

/* Assertions are this program's pass/fail signal; see tests/test.c. */
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#if defined(_MSC_VER)
#define _CRT_RAND_S
#include <stdlib.h>
#else
#include <stdlib.h>
#endif
#include <string.h>

#include "tlsf_thread.h"

#include "pool_limits.h"

/* Test parameters (tuned for < 2s on modern hardware) */

#define POOL_SIZE TLSF_TEST_POOL_CLAMP(4 * 1024 * 1024) /* 4 MB static pool */
#define NUM_THREADS 8
#define OPS_PER_THREAD 50000
#define MAX_ALLOCS 128
#define MAX_ALLOC_SIZE 2048

TLSF_MSVC_ALIGN(16) static char pool[POOL_SIZE] TLSF_GCC_ALIGN(16);
static tlsf_thread_t ts;

#if defined(_TLSF_USE_C11_THREADS)
#define TLSF_THREAD_T thrd_t
#define TLSF_CREATE_THREAD(thrd, func, arg) thrd_create(thrd, func, arg)
#define TLSF_JOIN_THREAD(thrd) thrd_join((thrd), NULL)
#define TLSF_THREAD_CONVENTION int
#define TLSF_THREAD_RETURN 0
#elif defined(TLSF_THREAD_POSIX)
#define TLSF_THREAD_T pthread_t
#define TLSF_CREATE_THREAD(thrd, func, arg) \
    pthread_create(thrd, NULL, func, arg)
#define TLSF_JOIN_THREAD(thrd) pthread_join((thrd), NULL)
#define TLSF_THREAD_CONVENTION void *
#define TLSF_THREAD_RETURN NULL
#elif defined(TLSF_THREAD_WIN)
#define TLSF_THREAD_T HANDLE
#define TLSF_CREATE_THREAD(thrd, func, arg)                                   \
    ((*(thrd) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) (func), (arg), \
                             0, NULL)) != NULL                                \
         ? 0                                                                  \
         : -1)
#define TLSF_JOIN_THREAD(thrd) \
    (WaitForSingleObject((thrd), INFINITE), CloseHandle((thrd)), 0)
#define TLSF_THREAD_CONVENTION DWORD WINAPI
#define TLSF_THREAD_RETURN 0
#endif

#if defined(_MSC_VER)
#define TLSF_RAND(x) (rand_s((x)), (unsigned) *(x))
#else
#define TLSF_RAND(x) (rand_r(x))
#endif

/* Per-thread work */

typedef struct {
    int id;
    int errors;
    int alloc_count;   /* total successful allocations */
    int free_count;    /* total frees */
    int realloc_count; /* total reallocs */
} thread_result_t;

static TLSF_THREAD_CONVENTION thread_func(void *arg)
{
    thread_result_t *res = (thread_result_t *) arg;
    void *ptrs[MAX_ALLOCS];
    size_t sizes[MAX_ALLOCS];
    int count = 0;
    unsigned seed = (unsigned) res->id * 2654435761U + 42;

    memset(ptrs, 0, sizeof(ptrs));

    for (int op = 0; op < OPS_PER_THREAD; op++) {
        int action = (int) (TLSF_RAND(&seed) % 4);

        switch (action) {
        case 0: /* malloc */
        case 1:
            if (count < MAX_ALLOCS) {
                size_t sz = (size_t) (TLSF_RAND(&seed) % MAX_ALLOC_SIZE) + 1;
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
                int idx =
                    (int) ((unsigned) TLSF_RAND(&seed) % (unsigned) count);
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
                int idx =
                    (int) ((unsigned) TLSF_RAND(&seed) % (unsigned) count);
                size_t old_sz = sizes[idx];
                size_t new_sz =
                    (size_t) (TLSF_RAND(&seed) % MAX_ALLOC_SIZE) + 1;

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

    return TLSF_THREAD_RETURN;
}

/* Test: multi-threaded stress */

static void stress_test(void)
{
    printf("Thread stress test (%d threads, %d ops each): ", NUM_THREADS,
           OPS_PER_THREAD);
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);
    printf("(%d arenas, %zu usable) ", ts.count, usable);
    fflush(stdout);

    TLSF_THREAD_T threads[NUM_THREADS];
    thread_result_t results[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        results[i].id = i;
        results[i].errors = 0;
        results[i].alloc_count = 0;
        results[i].free_count = 0;
        results[i].realloc_count = 0;
        TLSF_CREATE_THREAD(&threads[i], thread_func, &results[i]);
    }

    int total_errors = 0;
    int total_allocs = 0, total_frees = 0, total_reallocs = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        TLSF_JOIN_THREAD(threads[i]);
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

/* Test: aligned allocation under contention */

static TLSF_THREAD_CONVENTION aligned_thread_func(void *arg)
{
    int id = *(int *) arg;
    unsigned seed = (unsigned) id * 0xDEADBEEF + 7;

    for (int op = 0; op < 5000; op++) {
        /* Alignment: power of two from 8 to 4096 */
        unsigned shift = (unsigned) (TLSF_RAND(&seed) % 10) + 3; /* 8 to 8192 */
        size_t align = (size_t) 1 << shift;
        if (align > 4096)
            align = 4096;
        size_t sz = (size_t) (TLSF_RAND(&seed) % 512) + 1;

        void *p = tlsf_thread_aalloc(&ts, align, sz);
        if (p) {
            assert(((uintptr_t) p % align) == 0);
            memset(p, id & 0xFF, sz);
            tlsf_thread_free(&ts, p);
        }
    }
    return TLSF_THREAD_RETURN;
}

static void aligned_test(void)
{
    printf("Thread aligned alloc test: ");
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);

    TLSF_THREAD_T threads[NUM_THREADS];
    int ids[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        ids[i] = i;
        TLSF_CREATE_THREAD(&threads[i], aligned_thread_func, &ids[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++)
        TLSF_JOIN_THREAD(threads[i]);

    tlsf_thread_check(&ts);

    tlsf_stats_t stats;
    tlsf_thread_stats(&ts, &stats);
    assert(stats.total_used == 0);

    tlsf_thread_destroy(&ts);
    printf("done\n");
}

/* Test: reset under quiescence */

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

/* Test: single-threaded basic sanity */

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

/* Did any of 'ptrs' come out of this arena? Mirrors the range test in
 * arena_find(), which is file-static in src/tlsf_thread.c and so unreachable
 * from here.
 */
static bool arena_holds_any(const tlsf_arena_t *a, void **ptrs, size_t count)
{
    uintptr_t base = (uintptr_t) a->base;
    for (size_t i = 0; i < count; i++) {
        uintptr_t addr = (uintptr_t) ptrs[i];
        if (addr >= base && addr - base < a->capacity)
            return true;
    }
    return false;
}

/* Test: cross-arena fallback when the preferred arena runs dry.
 *
 * arena_select() hashes the thread id, so one thread keeps landing on the same
 * preferred arena. Draining it pushes every later request through
 * arena_fallback_alloc(), and running all the way to exhaustion drives that
 * function's second, blocking pass too, since a request that no arena can serve
 * visits every lock twice before giving up. Nothing else in this file reaches
 * either pass: the stress test sizes its arenas so the preferred one never
 * empties.
 */
static void fallback_test(void)
{
    printf("Thread cross-arena fallback test: ");
    fflush(stdout);

    size_t usable = tlsf_thread_init(&ts, pool, sizeof(pool));
    assert(usable > 0);

    if (ts.count < 2) {
        tlsf_thread_destroy(&ts);
        printf("skipped (single arena)\n");
        return;
    }

    /* About four blocks per arena, which bounds the pointer table below. */
    size_t chunk = usable / (4 * (size_t) ts.count);
    assert(chunk > TLSF_TEST_BLOCK_COST);

    void *ptrs[4 * TLSF_ARENA_COUNT + 8];
    size_t count = 0;
    void *p;
    while ((p = tlsf_thread_malloc(&ts, chunk)) != NULL) {
        assert(count < sizeof(ptrs) / sizeof(ptrs[0]));
        ptrs[count++] = p;
    }
    assert(count > 0); /* the loop ended when no arena could serve chunk */

    /* Every arena contributed, which only the fallback path can arrange: a
     * single thread asks the same preferred arena every time.
     */
    int served = 0;
    for (int i = 0; i < ts.count; i++)
        served += arena_holds_any(&ts.arenas[i], ptrs, count);
    assert(served == ts.count);

    /* With nothing left anywhere, a growing realloc cannot expand in place and
     * cannot relocate either. It must fail without touching the original.
     */
    memset(ptrs[0], 0x3C, chunk);
    assert(tlsf_thread_realloc(&ts, ptrs[0], chunk * 2) == NULL);
    const unsigned char *kept = (const unsigned char *) ptrs[0];
    for (size_t i = 0; i < chunk; i++)
        assert(kept[i] == 0x3C);

    for (size_t i = 0; i < count; i++)
        tlsf_thread_free(&ts, ptrs[i]);
    tlsf_thread_check(&ts);

    tlsf_stats_t stats;
    tlsf_thread_stats(&ts, &stats);
    assert(stats.total_used == 0);

    printf("%zu blocks across %d arenas, done\n", count, ts.count);
    tlsf_thread_destroy(&ts);
}

/* Test: init paths the fixed 4 MB pool never reaches.
 *
 * A region too small to split TLSF_ARENA_COUNT ways drives the halving loop; a
 * region too small for even one arena drives the rollback that destroys the
 * locks created so far and leaves the instance zeroed. The remaining rollback,
 * the one for a failed lock init, needs TLSF_LOCK_INIT() to fail and has no
 * portable trigger.
 */
static void init_limits_test(void)
{
    printf("Thread init limits test: ");
    fflush(stdout);

    static tlsf_thread_t small;

    /* One word short of what tlsf_pool_init() accepts, so the first arena fails
     * and init unwinds.
     */
    TLSF_MSVC_ALIGN(16)
    static char cramped[TLSF_TEST_BLOCK_COST] TLSF_GCC_ALIGN(16);
    assert(tlsf_thread_init(&small, cramped, sizeof(cramped)) == 0);
    assert(small.count == 0);

    /* A failed instance is inert rather than dangerous. */
    assert(tlsf_thread_malloc(&small, 16) == NULL);
    tlsf_thread_free(&small, NULL);
    tlsf_thread_check(&small);
    tlsf_thread_destroy(&small);

    /* Too small to divide TLSF_ARENA_COUNT ways: the count halves until each
     * share clears the per-arena minimum. Two arenas' worth of the 256-byte
     * per-arena minimum that tlsf_thread_init() enforces, so the count has to
     * halve at least once.
     */
    TLSF_MSVC_ALIGN(16) static char narrow[2 * 256] TLSF_GCC_ALIGN(16);
    size_t usable = tlsf_thread_init(&small, narrow, sizeof(narrow));
    assert(usable > 0);
    assert(small.count >= 1);
#if TLSF_ARENA_COUNT > 2
    assert(small.count < TLSF_ARENA_COUNT);
#endif

    void *p = tlsf_thread_malloc(&small, 32);
    assert(p);
    tlsf_thread_free(&small, p);
    tlsf_thread_check(&small);

    printf("%zu bytes seated %d of %d arenas, done\n", sizeof(narrow),
           small.count, TLSF_ARENA_COUNT);
    tlsf_thread_destroy(&small);
}

/* Test: the weak tlsf_resize() default.
 *
 * This binary supplies no strong definition, so a dynamic arena has no backend
 * to grow from and every allocation must fail rather than hand out memory that
 * was never mapped. tests/test.c and tests/bench.c cannot check this; both
 * define their own tlsf_resize(). tests/wcet.c links the weak one but never
 * reaches it, since it allocates only from static pools.
 */
static void weak_resize_test(void)
{
    printf("Weak tlsf_resize default test: ");
    fflush(stdout);

    /* Zeroed, so not a fixed pool: growth goes through tlsf_resize(). */
    static tlsf_t dynamic;

    assert(tlsf_malloc(&dynamic, 64) == NULL);
    assert(tlsf_aalloc(&dynamic, 256, 64) == NULL);

    tlsf_stats_t stats;
    assert(tlsf_get_stats(&dynamic, &stats) == 0);
    assert(stats.total_used == 0);
    assert(stats.block_count == 0);

    printf("done\n");
}

/* Test: null and foreign arguments across the wrapper API.
 *
 * Same gap as argument_contract_test() in tests/test.c, one layer up: every one
 * of these is a pure rejection path that the functional tests never enter. The
 * foreign-pointer cases matter most, since arena_find() failing to place a
 * pointer is the wrapper's only defense against operating on memory it does not
 * own.
 */
static void null_argument_test(void)
{
    printf("Thread null argument test: ");
    fflush(stdout);

    assert(tlsf_thread_init(&ts, pool, sizeof(pool)) > 0);

    assert(tlsf_thread_init(NULL, pool, sizeof(pool)) == 0);
    assert(tlsf_thread_init(&ts, NULL, sizeof(pool)) == 0);
    assert(tlsf_thread_init(&ts, pool, 0) == 0);

    assert(tlsf_thread_malloc(NULL, 16) == NULL);
    assert(tlsf_thread_aalloc(&ts, 0, 16) == NULL);
    assert(tlsf_thread_realloc(NULL, NULL, 16) == NULL);

    tlsf_thread_free(NULL, NULL);
    tlsf_thread_check(NULL);
    tlsf_thread_reset(NULL);
    tlsf_thread_destroy(NULL);

    tlsf_stats_t stats;
    assert(tlsf_thread_stats(NULL, &stats) == -1);
    assert(tlsf_thread_stats(&ts, NULL) == -1);

    /* A pointer no arena owns is refused rather than acted on. */
    char foreign[64];
    tlsf_thread_free(&ts, foreign);
    assert(tlsf_thread_realloc(&ts, foreign, 32) == NULL);

    /* The instance is unharmed by all of it. */
    tlsf_thread_check(&ts);
    void *p = tlsf_thread_malloc(&ts, 64);
    assert(p);
    tlsf_thread_free(&ts, p);

    tlsf_thread_destroy(&ts);
    printf("done\n");
}

/* Main */

int main(void)
{
    printf("=== Thread-safe TLSF tests ===\n");
    printf("Arena count: %d\n", TLSF_ARENA_COUNT);

    basic_test();
    stress_test();
    aligned_test();
    reset_test();
    fallback_test();
    init_limits_test();
    weak_resize_test();
    null_argument_test();

    puts("OK!");
    return 0;
}
