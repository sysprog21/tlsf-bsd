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

    /* Allocate random sizes up to the cap threshold.
     * Track them in an array.
     */
    int64_t rest = (int64_t) spacelen * (rand() % 6 + 1);
    unsigned i = 0;
    while (rest > 0) {
        size_t len = ((size_t) rand() % cap) + 1;
        if (rand() % 2 == 0) {
            p[i] = tlsf_malloc(t, len);
        } else {
            size_t align = 1U << (rand() % 20);
            if (cap < align)
                align = 0;
            else
                len = align * (((size_t) rand() % (cap / align)) + 1);
            p[i] = !align || !len ? tlsf_malloc(t, len)
                                  : tlsf_aalloc(t, align, len);
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

        tlsf_check(t);

        /* Fill with magic (only when testing up to 1MB). */
        uint8_t *data = (uint8_t *) p[i];
        if (spacelen <= 1024 * 1024)
            memset(data, 0, len);
        data[0] = 0xa5;

        if (i++ == maxitems)
            break;
    }

    /* Randomly deallocate the memory blocks until all of them are freed.
     * The free space should match the free space after initialisation.
     */
    for (unsigned n = i; n;) {
        size_t target = (size_t) rand() % i;
        if (p[target] == NULL)
            continue;
        uint8_t *data = (uint8_t *) p[target];
        assert(data[0] == 0xa5);
        tlsf_free(t, p[target]);
        p[target] = NULL;
        n--;

        tlsf_check(t);
    }

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

    /*
     * Cap test size to fit within test pool limits.
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

/*
 * Test internal fragmentation by allocating various sizes and measuring
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

    /*
     * Validate SL subdivision improvement:
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

int main(void)
{
    PAGE = (size_t) sysconf(_SC_PAGESIZE);
    /*
     * Virtual address space reservation for testing.
     * 64-bit: 1GB is sufficient and safe
     * 32-bit: 128MB to avoid VA space exhaustion (user space is 2-3GB)
     */
#if __SIZE_WIDTH__ == 64 || defined(__LP64__) || defined(_LP64)
    MAX_PAGES = ((size_t) 1 << 30) / PAGE; /* 1 GB */
#else
    MAX_PAGES = ((size_t) 128 << 20) / PAGE; /* 128 MB */
#endif
    tlsf_t t = TLSF_INIT;
    srand((unsigned int) time(0));

    /* Run existing tests */
    large_size_test(&t);
    random_sizes_test(&t);

    /* Run pool append test */
    append_pool_test(&t);

    /* Run fragmentation validation test */
    fragmentation_test(&t);

    puts("OK!");
    return 0;
}
