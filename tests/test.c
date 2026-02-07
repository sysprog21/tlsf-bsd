/*
 * Copyright (c) 2016 National Cheng Kung University, Taiwan.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style license.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "tlsf.h"

static size_t PAGE;
static size_t MAX_PAGES;
static size_t curr_pages = 0;
static void *start_addr = 0;

void *tlsf_resize(tlsf_t *t, size_t req_size)
{
    (void) t;

    if (!start_addr)
        start_addr = mmap(0, MAX_PAGES * PAGE, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);

    size_t req_pages = (req_size + PAGE - 1) / PAGE;
    if (req_pages > MAX_PAGES)
        return 0;

    if (req_pages != curr_pages) {
        if (req_pages < curr_pages)
            madvise((char *) start_addr + PAGE * req_pages,
                    (size_t) (curr_pages - req_pages) * PAGE, MADV_DONTNEED);
        curr_pages = req_pages;
    }

    return start_addr;
}

static void random_test(tlsf_t *t, size_t spacelen, const size_t cap)
{
    const size_t maxitems = 2 * spacelen;

    void **p = (void **) malloc(maxitems * sizeof(void *));
    assert(p);

    /* Throttle tlsf_check() frequency to avoid O(n^2) overhead.
     * Per-operation checking is fine for small pools (< 256 items).
     * For large pools, check every N operations where N scales with pool
     * size, bounding total check work to ~256 full heap walks per phase.
     */
    size_t check_stride = maxitems > 256 ? (maxitems + 255) / 256 : 1;

    /* Allocate random sizes up to the cap threshold.
     * Track them in an array.
     */
    int64_t rest = (int64_t) spacelen * (rand() % 6 + 1);
    unsigned i = 0;
    while (rest > 0 && i < maxitems) {
        size_t len = ((size_t) rand() % cap) + 1;
        if (rand() % 2 == 0) {
            p[i] = tlsf_malloc(t, len);
        } else {
            size_t align = 1U << (rand() % 20);
            if (cap < align)
                align = 0;
            p[i] = !align ? tlsf_malloc(t, len) : tlsf_aalloc(t, align, len);
            if (align)
                assert(!((size_t) p[i] % align));
        }
        assert(p[i]);
        rest -= (int64_t) len;

        if (rand() % 10 == 0) {
            len = ((size_t) rand() % cap) + 1;
            p[i] = tlsf_realloc(t, p[i], len);
            assert(p[i]);
        }

        if (i % check_stride == 0)
            tlsf_check(t);

        /* Fill with magic (only when testing up to 1MB). */
        uint8_t *data = (uint8_t *) p[i];
        if (spacelen <= 1024 * 1024)
            memset(data, 0, len);
        data[0] = 0xa5;

        i++;
    }

    /* Final consistency check after all allocations. */
    tlsf_check(t);

    /* Randomly deallocate the memory blocks until all of them are freed.
     * The free space should match the free space after initialisation.
     */
    size_t freed = 0;
    for (unsigned n = i; n;) {
        size_t target = (size_t) rand() % i;
        if (p[target] == NULL)
            continue;

        uint8_t *data = (uint8_t *) p[target];
        assert(data[0] == 0xa5);
        tlsf_free(t, p[target]);
        p[target] = NULL;
        n--;

        if (++freed % check_stride == 0)
            tlsf_check(t);
    }

    /* Final consistency check after all deallocations. */
    tlsf_check(t);

    free(p);
}

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

static void random_sizes_test(tlsf_t *t)
{
    const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 1024 * 1024};

    printf("Random allocation test: ");
    for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
        unsigned n = 1024;

        while (n--)
            random_test(t, sizes[i], (size_t) rand() % sizes[i] + 1);
        printf(".");
        fflush(stdout);
    }
    printf(" done\n");
}

static void large_alloc(tlsf_t *t, size_t s)
{
    for (size_t d = 0; d < 100 && d < s; ++d) {
        void *p = tlsf_malloc(t, s - d);
        assert(p);

        void *q = tlsf_malloc(t, s - d);
        assert(q);
        tlsf_free(t, q);

        q = tlsf_malloc(t, s - d);
        assert(q);
        tlsf_free(t, q);

        tlsf_free(t, p);
        tlsf_check(t);
    }
}

static void large_size_test(tlsf_t *t)
{
    printf("Large allocation test: ");
    fflush(stdout);

    /* Cap test size to fit within test pool limits.
     * 64-bit: up to 256MB, 32-bit: up to 32MB (pool is 128MB)
     */
#if __SIZE_WIDTH__ == 64 || defined(__LP64__) || defined(_LP64)
    size_t max_test = (size_t) 1 << 28; /* 256 MB */
#else
    size_t max_test = (size_t) 1 << 25; /* 32 MB */
#endif
    if (max_test > TLSF_MAX_SIZE)
        max_test = TLSF_MAX_SIZE;

    size_t s = 1;
    while (s <= max_test) {
        large_alloc(t, s);
        s *= 2;
    }
    printf(".");
    fflush(stdout);

    s = max_test;
    while (s > 0) {
        large_alloc(t, s);
        s /= 2;
    }
    printf(". done\n");
}

static void append_pool_test(tlsf_t *t)
{
    printf("Pool append test: ");
    fflush(stdout);

    /* Simple test: Initial allocation */
    void *ptr1 = tlsf_malloc(t, 1000);
    assert(ptr1);

    size_t initial_size = t->size;

    /* Try to append adjacent memory */
    void *append_addr = (char *) start_addr + initial_size;
    size_t appended = tlsf_append_pool(t, append_addr, 4096);

    if (appended > 0) {
        /* Test large allocation from expanded pool */
        void *large_ptr = tlsf_malloc(t, 3000);
        if (large_ptr)
            tlsf_free(t, large_ptr);
    }

    /* Test non-adjacent append (should fail) */
    char separate_memory[2048];
    size_t non_adjacent =
        tlsf_append_pool(t, separate_memory, sizeof(separate_memory));
    assert(non_adjacent == 0);

    tlsf_free(t, ptr1);
    tlsf_check(t);
    printf("done\n");
}

/* Test internal fragmentation by allocating various sizes and measuring
 * the overhead. With SL=32, max internal fragmentation should be ~3.125%
 * (1/32) compared to ~6.25% (1/16) with SL=16.
 */
static void fragmentation_test(tlsf_t *t)
{
    printf("Internal fragmentation test:\n");

    /*
     * Split into "small" (affected by min block size) and "large" (where
     * SL subdivision is the primary factor). BLOCK_SIZE_SMALL is 256 on
     * 64-bit with SL=32.
     */
    const size_t small_sizes[] = {17, 31, 33, 47, 63, 65, 95, 127};
    const size_t large_sizes[] = {
        257,  400,  511,  513,   800,   1000,  1500,  2000,  3000,
        4000, 5000, 7000, 10000, 15000, 20000, 30000, 50000, 100000,
    };

    double small_total = 0.0, large_total = 0.0, large_max = 0.0;
    size_t large_worst = 0;
    size_t small_count = sizeof(small_sizes) / sizeof(small_sizes[0]);
    size_t large_count = sizeof(large_sizes) / sizeof(large_sizes[0]);

    /* Test small sizes (high overhead expected due to min block size) */
    for (size_t i = 0; i < small_count; i++) {
        tlsf_stats_t before, after;
        tlsf_get_stats(t, &before);
        void *ptr = tlsf_malloc(t, small_sizes[i]);
        assert(ptr);
        tlsf_get_stats(t, &after);
        size_t actual = after.total_used - before.total_used;
        small_total += 100.0 * (double) (actual - small_sizes[i]) /
                       (double) small_sizes[i];
        tlsf_free(t, ptr);
    }

    /* Test large sizes (SL subdivision is the limiting factor) */
    for (size_t i = 0; i < large_count; i++) {
        tlsf_stats_t before, after;
        tlsf_get_stats(t, &before);
        void *ptr = tlsf_malloc(t, large_sizes[i]);
        assert(ptr);
        tlsf_get_stats(t, &after);
        size_t actual = after.total_used - before.total_used;
        double pct = 100.0 * (double) (actual - large_sizes[i]) /
                     (double) large_sizes[i];
        large_total += pct;
        if (pct > large_max) {
            large_max = pct;
            large_worst = large_sizes[i];
        }
        tlsf_free(t, ptr);
    }

    double small_avg = small_total / (double) small_count;
    double large_avg = large_total / (double) large_count;

    printf("  SL subdivisions: %u\n", _TLSF_SL_COUNT);
    printf("  Small sizes (<256B) avg overhead: %.2f%%\n", small_avg);
    printf("  Large sizes (>=256B) avg overhead: %.2f%%\n", large_avg);
    printf("  Large sizes max overhead: %.2f%% (size=%zu)\n", large_max,
           large_worst);

    /* Validate SL subdivision improvement:
     * - SL=32: theoretical max 1/32 = 3.125%, allow < 5% for alignment
     * - SL=16: theoretical max 1/16 = 6.25%, allow < 8%
     */
    if (_TLSF_SL_COUNT == 32) {
        assert(large_max < 5.0 && "large size max overhead exceeds 5%");
        assert(large_avg < 3.0 && "large size avg overhead exceeds 3%");
        printf("  [PASS] SL=32 validated: max<5%%, avg<3%%\n");
    } else if (_TLSF_SL_COUNT == 16) {
        assert(large_max < 8.0 && "large size max overhead exceeds 8%");
        assert(large_avg < 5.0 && "large size avg overhead exceeds 5%");
        printf("  [PASS] SL=16 validated: max<8%%, avg<5%%\n");
    }

    tlsf_check(t);
    printf("done\n");
}

/* Test backward expansion optimization in realloc.
 *
 * When growing an allocation and the next block is unavailable,
 * realloc should try expanding into the previous free block,
 * moving data with memmove instead of malloc+memcpy+free.
 */
static void realloc_backward_test(tlsf_t *t)
{
    printf("Realloc backward expansion test: ");
    fflush(stdout);

    /* Test 1: Simple backward expansion
     * Allocate A, B, C in sequence, free A, then grow B.
     * B should expand backward into A's space.
     */
    {
        const size_t size_a = 512;
        const size_t size_b = 256;
        const size_t size_c = 128;

        void *a = tlsf_malloc(t, size_a);
        void *b = tlsf_malloc(t, size_b);
        void *c = tlsf_malloc(t, size_c);
        assert(a && b && c);

        /* Fill B with pattern to verify data integrity after move */
        memset(b, 0xAB, size_b);

        /* Free A to create a free block before B */
        tlsf_free(t, a);
        tlsf_check(t);

        /* Grow B beyond its current size. Next block (C) is used,
         * so backward expansion should be triggered.
         */
        size_t new_size = size_a + size_b - 32; /* Fits in prev+current */
        void *new_b = tlsf_realloc(t, b, new_size);
        assert(new_b);
        tlsf_check(t);

        /* Verify data integrity (first size_b bytes should be 0xAB) */
        uint8_t *data = (uint8_t *) new_b;
        for (size_t i = 0; i < size_b; i++)
            assert(data[i] == 0xAB);

        /* The new pointer should be at A's original location (backward) */
        assert(new_b == a);

        tlsf_free(t, new_b);
        tlsf_free(t, c);
        tlsf_check(t);
    }
    printf(".");
    fflush(stdout);

    /* Test 2: Backward + forward expansion (both neighbors free)
     * Allocate A, B, C, D, free A and C, then grow B.
     * B should merge with both A and C.
     */
    {
        const size_t size_a = 512;
        const size_t size_b = 256;
        const size_t size_c = 512;
        const size_t size_d = 128;

        void *a = tlsf_malloc(t, size_a);
        void *b = tlsf_malloc(t, size_b);
        void *c = tlsf_malloc(t, size_c);
        void *d = tlsf_malloc(t, size_d);
        assert(a && b && c && d);

        /* Fill B with pattern */
        memset(b, 0xCD, size_b);

        /* Free both A and C */
        tlsf_free(t, a);
        tlsf_free(t, c);
        tlsf_check(t);

        /* Request size that needs both prev and next */
        size_t new_size = size_a + size_b + size_c - 64;
        void *new_b = tlsf_realloc(t, b, new_size);
        assert(new_b);
        tlsf_check(t);

        /* Verify data integrity */
        uint8_t *data = (uint8_t *) new_b;
        for (size_t i = 0; i < size_b; i++)
            assert(data[i] == 0xCD);

        /* Pointer should be at A's location */
        assert(new_b == a);

        tlsf_free(t, new_b);
        tlsf_free(t, d);
        tlsf_check(t);
    }
    printf(".");
    fflush(stdout);

    /* Test 3: Verify forward expansion is still preferred over backward
     * (no data movement needed for forward expansion)
     */
    {
        const size_t size_a = 256;
        const size_t size_b = 256;
        const size_t size_c = 512;
        const size_t size_d = 128; /* Keep D to prevent arena_shrink on C */

        void *a = tlsf_malloc(t, size_a);
        void *b = tlsf_malloc(t, size_b);
        void *c = tlsf_malloc(t, size_c);
        void *d = tlsf_malloc(t, size_d);
        assert(a && b && c && d);

        memset(b, 0xEF, size_b);

        /* Free both A and C (D keeps C from being shrunk away) */
        tlsf_free(t, a);
        tlsf_free(t, c);
        tlsf_check(t);

        /* Request size that fits in current + next (forward) */
        size_t new_size = size_b + size_c - 64;
        void *new_b = tlsf_realloc(t, b, new_size);
        assert(new_b);
        tlsf_check(t);

        /* Verify data integrity */
        uint8_t *data = (uint8_t *) new_b;
        for (size_t i = 0; i < size_b; i++)
            assert(data[i] == 0xEF);

        /* Forward expansion: pointer should remain at B's location */
        assert(new_b == b);

        tlsf_free(t, new_b);
        tlsf_free(t, d);
        tlsf_check(t);
    }
    printf(".");
    fflush(stdout);

    /* Test 4: Shrink then grow with backward expansion */
    {
        const size_t size_a = 1024, size_b = 512;

        void *a = tlsf_malloc(t, size_a);
        void *b = tlsf_malloc(t, size_b);
        assert(a && b);

        memset(b, 0x77, size_b);
        tlsf_free(t, a);
        tlsf_check(t);

        /* First shrink B */
        void *shrunk = tlsf_realloc(t, b, 128);
        assert(shrunk == b); /* Shrink in place */

        /* Verify data in shrunk size */
        uint8_t *data = (uint8_t *) shrunk;
        for (size_t i = 0; i < 128; i++)
            assert(data[i] == 0x77);

        /* Now grow it backward */
        void *grown = tlsf_realloc(t, shrunk, size_a + 128);
        assert(grown);
        assert(grown == a); /* Should expand backward */
        tlsf_check(t);

        /* Verify data preserved */
        data = (uint8_t *) grown;
        for (size_t i = 0; i < 128; i++)
            assert(data[i] == 0x77);

        tlsf_free(t, grown);
        tlsf_check(t);
    }
    printf(". done\n");
}

/* Test static (fixed-size) pool initialization and usage.
 * Exercises tlsf_pool_init() without requiring tlsf_resize().
 */
static void static_pool_test(void)
{
    printf("Static pool test: ");
    fflush(stdout);

    /* Test 1: Basic init, alloc, free */
    {
        static char pool[1024 * 1024]; /* 1 MB */
        tlsf_t t;
        size_t usable = tlsf_pool_init(&t, pool, sizeof(pool));
        assert(usable > 0);

        void *p = tlsf_malloc(&t, 100);
        assert(p);
        assert((char *) p >= pool && (char *) p < pool + sizeof(pool));

        tlsf_free(&t, p);
        tlsf_check(&t);
    }
    printf(".");
    fflush(stdout);

    /* Test 2: Pool exhaustion returns NULL */
    {
        static char pool[4096];
        tlsf_t t;
        size_t usable = tlsf_pool_init(&t, pool, sizeof(pool));
        assert(usable > 0);

        void *ptrs[256];
        int count = 0;
        for (int i = 0; i < 256; i++) {
            ptrs[i] = tlsf_malloc(&t, 64);
            if (!ptrs[i])
                break;
            count++;
        }
        assert(count > 0);
        assert(count < 256);

        for (int i = 0; i < count; i++)
            tlsf_free(&t, ptrs[i]);
        tlsf_check(&t);
    }
    printf(".");
    fflush(stdout);

    /* Test 3: Multiple independent instances (no globals needed) */
    {
        static char pool_a[8192];
        static char pool_b[8192];
        tlsf_t ta, tb;
        size_t ua = tlsf_pool_init(&ta, pool_a, sizeof(pool_a));
        size_t ub = tlsf_pool_init(&tb, pool_b, sizeof(pool_b));
        assert(ua > 0 && ub > 0);

        void *pa = tlsf_malloc(&ta, 1000);
        void *pb = tlsf_malloc(&tb, 2000);
        assert(pa && pb);

        assert((char *) pa >= pool_a && (char *) pa < pool_a + sizeof(pool_a));
        assert((char *) pb >= pool_b && (char *) pb < pool_b + sizeof(pool_b));

        tlsf_free(&tb, pb);
        tlsf_free(&ta, pa);
        tlsf_check(&ta);
        tlsf_check(&tb);
    }
    printf(".");
    fflush(stdout);

    /* Test 4: Realloc within static pool */
    {
        static char pool[32768];
        tlsf_t t;
        tlsf_pool_init(&t, pool, sizeof(pool));

        void *p = tlsf_malloc(&t, 100);
        assert(p);
        memset(p, 0xAA, 100);

        void *p2 = tlsf_realloc(&t, p, 500);
        assert(p2);
        uint8_t *data = (uint8_t *) p2;
        for (int i = 0; i < 100; i++)
            assert(data[i] == 0xAA);

        void *p3 = tlsf_realloc(&t, p2, 50);
        assert(p3);

        tlsf_free(&t, p3);
        tlsf_check(&t);
    }
    printf(".");
    fflush(stdout);

    /* Test 5: Aligned allocation within static pool */
    {
        static char pool[65536];
        tlsf_t t;
        tlsf_pool_init(&t, pool, sizeof(pool));

        void *p = tlsf_aalloc(&t, 256, 256);
        assert(p);
        assert(((size_t) p % 256) == 0);

        void *q = tlsf_aalloc(&t, 4096, 4096);
        assert(q);
        assert(((size_t) q % 4096) == 0);

        tlsf_free(&t, p);
        tlsf_free(&t, q);
        tlsf_check(&t);
    }
    printf(".");
    fflush(stdout);

    /* Test 6: Pool too small */
    {
        char tiny[8];
        tlsf_t t;
        size_t usable = tlsf_pool_init(&t, tiny, sizeof(tiny));
        assert(usable == 0);
    }
    printf(".");
    fflush(stdout);

    /* Test 7: Stats on static pool */
    {
        static char pool[16384];
        tlsf_t t;
        tlsf_pool_init(&t, pool, sizeof(pool));

        tlsf_stats_t stats;
        int rc = tlsf_get_stats(&t, &stats);
        assert(rc == 0);
        assert(stats.total_free > 0);
        assert(stats.free_count == 1);

        void *p = tlsf_malloc(&t, 100);
        assert(p);
        rc = tlsf_get_stats(&t, &stats);
        assert(rc == 0);
        assert(stats.total_used > 0);

        tlsf_free(&t, p);
        tlsf_check(&t);
    }
    printf(".");
    fflush(stdout);

    /* Test 8: Append pool extends a static pool */
    {
        static char combined[8192];
        tlsf_t t;

        /* Initialize with first half */
        size_t half = 4096;
        size_t usable = tlsf_pool_init(&t, combined, half);
        assert(usable > 0);

        void *p1 = tlsf_malloc(&t, 1000);
        assert(p1);

        /* Append second half (adjacent by construction) */
        size_t appended = tlsf_append_pool(&t, combined + half, half);
        assert(appended > 0);

        /* Allocate from the expanded pool */
        void *p2 = tlsf_malloc(&t, 3000);
        assert(p2);

        /* Non-adjacent memory should fail */
        char separate[512];
        size_t bad = tlsf_append_pool(&t, separate, sizeof(separate));
        assert(bad == 0);

        tlsf_free(&t, p1);
        tlsf_free(&t, p2);
        tlsf_check(&t);
    }
    printf(". done\n");
}

/* Test zero-size and alignment edge cases.
 * Validates consistent behavior between tlsf_malloc and tlsf_aalloc.
 */
static void zero_size_align_test(tlsf_t *t)
{
    printf("Zero-size and alignment semantics test: ");
    fflush(stdout);

    /* Test 1: tlsf_malloc(t, 0) returns a valid, unique pointer */
    {
        void *p = tlsf_malloc(t, 0);
        assert(p);
        void *q = tlsf_malloc(t, 0);
        assert(q);
        assert(p != q); /* Each zero-size alloc is unique */
        tlsf_free(t, p);
        tlsf_free(t, q);
        tlsf_check(t);
    }
    printf(".");
    fflush(stdout);

    /* Test 2: tlsf_aalloc(t, align, 0) returns a valid aligned pointer
     * (was returning NULL before the fix)
     */
    {
        size_t aligns[] = {8, 16, 32, 64, 128, 256, 512, 1024, 4096};
        for (size_t i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
            void *p = tlsf_aalloc(t, aligns[i], 0);
            assert(p);
            assert(((size_t) p % aligns[i]) == 0);
            tlsf_free(t, p);
        }
        tlsf_check(t);
    }
    printf(".");
    fflush(stdout);

    /* Test 3: tlsf_aalloc no longer requires size to be a multiple of align
     * (POSIX posix_memalign semantics: size need not be n*align)
     */
    {
        /* size=100 is not a multiple of align=64 */
        void *p = tlsf_aalloc(t, 64, 100);
        assert(p);
        assert(((size_t) p % 64) == 0);
        memset(p, 0xAA, 100); /* Usable for at least 100 bytes */
        tlsf_free(t, p);

        /* size=7 is not a multiple of align=16 */
        p = tlsf_aalloc(t, 16, 7);
        assert(p);
        assert(((size_t) p % 16) == 0);
        tlsf_free(t, p);

        /* size=1000 is not a multiple of align=256 */
        p = tlsf_aalloc(t, 256, 1000);
        assert(p);
        assert(((size_t) p % 256) == 0);
        memset(p, 0xBB, 1000);
        tlsf_free(t, p);

        tlsf_check(t);
    }
    printf(".");
    fflush(stdout);

    /* Test 4: Invalid alignment rejected (not power of two, zero) */
    {
        assert(tlsf_aalloc(t, 0, 100) == NULL);
        assert(tlsf_aalloc(t, 3, 100) == NULL);
        assert(tlsf_aalloc(t, 5, 100) == NULL);
        assert(tlsf_aalloc(t, 6, 100) == NULL);
        assert(tlsf_aalloc(t, 7, 100) == NULL);
        assert(tlsf_aalloc(t, 9, 100) == NULL);
    }
    printf(".");
    fflush(stdout);

    /* Test 5: Size that IS a multiple of align still works (regression) */
    {
        void *p = tlsf_aalloc(t, 64, 128);
        assert(p);
        assert(((size_t) p % 64) == 0);
        tlsf_free(t, p);

        p = tlsf_aalloc(t, 256, 512);
        assert(p);
        assert(((size_t) p % 256) == 0);
        tlsf_free(t, p);

        tlsf_check(t);
    }
    printf(". done\n");
}

int main(void)
{
    PAGE = (size_t) sysconf(_SC_PAGESIZE);
    /* Virtual address space reservation for testing.
     * 64-bit: 1GB is sufficient and safe
     * 32-bit: 128MB to avoid VA space exhaustion (user space is 2-3GB)
     */
#if __SIZE_WIDTH__ == 64 || defined(__LP64__) || defined(_LP64)
    MAX_PAGES = ((size_t) 1 << 30) / PAGE; /* 1 GB */
#else
    MAX_PAGES = ((size_t) 128 << 20) / PAGE; /* 128 MB */
#endif
    tlsf_t t = TLSF_INIT;

    const char *seed_env = getenv("TLSF_TEST_SEED");
    unsigned int seed = seed_env ? (unsigned int) strtoul(seed_env, NULL, 0)
                                 : (unsigned int) time(0);
    printf("Random seed: %u (set TLSF_TEST_SEED to reproduce)\n", seed);
    srand(seed);

    /* Run existing tests */
    large_size_test(&t);
    random_sizes_test(&t);

    /* Run pool append test */
    append_pool_test(&t);

    /* Run backward expansion test */
    realloc_backward_test(&t);

    /* Run fragmentation validation test */
    fragmentation_test(&t);

    /* Run zero-size and alignment semantics test */
    zero_size_align_test(&t);

    /* Run static pool test */
    static_pool_test();

    puts("OK!");
    return 0;
}
