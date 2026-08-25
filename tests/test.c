/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(WIN32) || defined(__WIN32__) || defined(_WIN64)
#include <stddef.h>
#include <time.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif


#include "tlsf.h"

#include "pool_limits.h"

static size_t PAGE;
static size_t MAX_PAGES;
static size_t curr_pages = 0;
static void *start_addr = 0;

static inline size_t get_page_size(void)
{
#if defined(_WIN32) || defined(WIN32) || defined(__WIN32__) || defined(_WIN64)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t) si.dwPageSize;
#else
    long page_size = sysconf(_SC_PAGESIZE);

    /* Zero is as unusable as a negative return, since PAGE is a divisor below.
     * Report with fputs rather than perror: sysconf may return -1 for an
     * indeterminate limit without setting errno.
     */
    if (page_size <= 0) {
        fputs("sysconf(_SC_PAGESIZE) returned an unusable page size\n", stderr);
        exit(EXIT_FAILURE);
    }
    return (size_t) page_size;
#endif
}

/* Size the virtual address space window that tlsf_resize() hands out. Called
 * from tlsf_resize() itself so PAGE can never be read before it is set. 64-bit:
 * 1 GB is sufficient and safe. 32-bit: 128 MB to avoid VA space exhaustion
 * (user space is 2-3 GB).
 */
static void page_init(void)
{
    if (PAGE)
        return;
    PAGE = get_page_size();
#if _TLSF_SIZE_WIDTH == 64 || defined(__LP64__) || defined(_LP64)
    MAX_PAGES = ((size_t) 1 << 30) / PAGE;
#else
    MAX_PAGES = ((size_t) 128 << 20) / PAGE;
#endif
}

/* Platform primitives behind tlsf_resize(). Each does one job: reserve the
 * address window, hand physical pages back, and make pages writable before the
 * allocator touches them. Keeping them this small is what lets the resize
 * driver below exist in one copy instead of two that must be kept in step.
 *
 * pages_reserve() reports failure as NULL on both platforms, so the POSIX
 * MAP_FAILED sentinel never escapes it and cannot be mistaken for an arena.
 */
#if defined(_WIN32) || defined(WIN32) || defined(__WIN32__) || defined(_WIN64)
/* VirtualAlloc with MEM_RESERVE is the analogue of mmap with MAP_NORESERVE. */
static void *pages_reserve(size_t bytes)
{
    void *addr = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
    if (!addr)
        fprintf(stderr, "VirtualAlloc reserve failed: %lu\n", GetLastError());
    return addr;
}

/* MEM_DECOMMIT is the analogue of madvise(MADV_DONTNEED): the address stays
 * reserved, the physical pages go back. Best-effort, see pages_release() use.
 */
static void pages_release(void *addr, size_t bytes)
{
    if (!VirtualFree(addr, bytes, MEM_DECOMMIT))
        fprintf(stderr, "VirtualFree decommit failed: %lu\n", GetLastError());
}

static bool pages_commit(void *addr, size_t bytes)
{
    if (VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE))
        return true;
    fprintf(stderr, "VirtualAlloc commit failed: %lu\n", GetLastError());
    return false;
}
#else
static void *pages_reserve(size_t bytes)
{
    void *addr = mmap(0, bytes, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return addr;
}

static void pages_release(void *addr, size_t bytes)
{
    if (madvise(addr, bytes, MADV_DONTNEED) != 0)
        perror("madvise");
}

static bool pages_commit(void *addr, size_t bytes)
{
    /* Nothing to do: the MAP_NORESERVE mapping is already writable and the
     * kernel backs each page on first touch.
     */
    (void) addr;
    (void) bytes;
    return true;
}
#endif

void *tlsf_resize(tlsf_t *t, size_t req_size)
{
    (void) t;
    page_init();

    if (!start_addr) {
        start_addr = pages_reserve(MAX_PAGES * PAGE);
        if (!start_addr)
            return NULL;
    }

    size_t req_pages = (req_size + PAGE - 1) / PAGE;
    if (req_pages > MAX_PAGES)
        return NULL;

    if (req_pages != curr_pages) {
        if (req_pages < curr_pages) {
            /* Best-effort: a failed release leaves the pages resident but does
             * not affect allocator state, so curr_pages advances either way.
             */
            pages_release((char *) start_addr + PAGE * req_pages,
                          (curr_pages - req_pages) * PAGE);
        } else if (!pages_commit(start_addr, req_pages * PAGE)) {
            return NULL;
        }
        curr_pages = req_pages;
    }

    return start_addr;
}

static void random_test(tlsf_t *t, size_t spacelen, const size_t cap)
{
    const size_t maxitems = 2 * spacelen;

    void **p = (void **) malloc(maxitems * sizeof(void *));
    assert(p);

    /* Throttle tlsf_check() frequency to avoid O(n^2) overhead. Per-operation
     * checking is fine for small pools (< 256 items). For large pools, check
     * every N operations where N scales with pool size, bounding total check
     * work to ~256 full heap walks per phase.
     */
    size_t check_stride = maxitems > 256 ? (maxitems + 255) / 256 : 1;

    /* Allocate random sizes up to the cap threshold. Track them in an array. */
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

        /* Fill with magic (only when testing up to 1MB). The fill runs over the
         * usable size rather than the requested length, so it doubles as the
         * check on tlsf_usable_size(): under-reporting trips the assert, and
         * over-reporting corrupts a neighbour that tlsf_check() then sees.
         */
        uint8_t *data = (uint8_t *) p[i];
        size_t usable = tlsf_usable_size(data);
        assert(usable >= len);
        if (spacelen <= 1024 * 1024)
            memset(data, 0, usable);
        data[0] = 0xa5;

        i++;
    }

    /* Final consistency check after all allocations. */
    tlsf_check(t);

    /* Randomly deallocate the memory blocks until all of them are freed. The
     * free space should match the free space after initialisation.
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

#if defined(_MSC_VER)
static int msvc_large_rand(void)
{
    /* 1 billion random number for MSVC */
    return (rand() << 15) | rand();
}
#endif

static void random_sizes_test(tlsf_t *t)
{
    const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 1024 * 1024};

    /* random_test() creates up to 2 * spacelen live allocations, each costing
     * at least TLSF_TEST_BLOCK_COST bytes, and the arena can never exceed
     * 2^_TLSF_FL_MAX bytes. The item count dominates the 6 * spacelen payload
     * bound, so size from that; halve again for fragmentation headroom. Skip
     * entries a reduced TLSF_MAX_POOL_BITS cannot serve rather than asserting.
     */
    const size_t space_cap =
        ((size_t) 1 << _TLSF_FL_MAX) / (4 * TLSF_TEST_BLOCK_COST);

    printf("Random allocation test: ");
    for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
        unsigned n = 1024;

        if (sizes[i] > space_cap) {
            printf("(skip %zu: pool limit) ", sizes[i]);
            continue;
        }

        while (n--)
#if defined(_MSC_VER)
            random_test(t, sizes[i], (size_t) msvc_large_rand() % sizes[i] + 1);
#else
            random_test(t, sizes[i], (size_t) rand() % sizes[i] + 1);
#endif
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

    /* Cap test size to fit within test pool limits. 64-bit: up to 256MB,
     * 32-bit: up to 32MB (pool is 128MB)
     */
#if _TLSF_SIZE_WIDTH == 64 || defined(__LP64__) || defined(_LP64)
    size_t max_test = (size_t) 1 << 28; /* 256 MB */
#else
    size_t max_test = (size_t) 1 << 25; /* 32 MB */
#endif
    if (max_test > TLSF_MAX_SIZE)
        max_test = TLSF_MAX_SIZE;

    /* large_alloc() keeps two blocks of this size live at once, and the arena
     * can never exceed 2^_TLSF_FL_MAX bytes. Leave room for both plus metadata
     * so the test tracks a reduced TLSF_MAX_POOL_BITS instead of assuming the
     * default configuration.
     */
    size_t pool_cap = TLSF_TEST_POOL_MAX;
    if (max_test > pool_cap)
        max_test = pool_cap;

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

/* Test internal fragmentation by allocating various sizes and measuring the
 * overhead. With SL=32, max internal fragmentation should be ~3.125% (1/32)
 * compared to ~6.25% (1/16) with SL=16.
 */
static void fragmentation_test(tlsf_t *t)
{
    printf("Internal fragmentation test:\n");

    /* Split into "small" (affected by min block size) and "large" (where SL
     * subdivision is the primary factor). BLOCK_SIZE_SMALL is 256 on 64-bit
     * with SL=32.
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
 * When growing an allocation and the next block is unavailable, realloc should
 * try expanding into the previous free block, moving data with memmove instead
 * of malloc+memcpy+free.
 */
static void realloc_backward_test(tlsf_t *t)
{
    printf("Realloc backward expansion test: ");
    fflush(stdout);

    /* Test 1: Simple backward expansion Allocate A, B, C in sequence, free A,
     * then grow B. B should expand backward into A's space.
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

        /* Grow B beyond its current size. Next block (C) is used, so backward
         * expansion should be triggered.
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

    /* Test 2: Backward + forward expansion (both neighbors free) Allocate A, B,
     * C, D, free A and C, then grow B. B should merge with both A and C.
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

    /* Test 3: Verify forward expansion is still preferred over backward (no
     * data movement needed for forward expansion)
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

/* Test static (fixed-size) pool initialization and usage. Exercises
 * tlsf_pool_init() without requiring tlsf_resize().
 */
static void static_pool_test(void)
{
    printf("Static pool test: ");
    fflush(stdout);

    /* Test 1: Basic init, alloc, free */
    {
        static char pool[TLSF_TEST_POOL_CLAMP(1024 * 1024)];
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

/* Test zero-size and alignment edge cases. Validates consistent behavior
 * between tlsf_malloc and tlsf_aalloc.
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

    /* Test 2: tlsf_aalloc(t, align, 0) returns a valid aligned pointer (was
     * returning NULL before the fix)
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

/* Pool utilization: a fixed pool must be able to hand out roughly its whole
 * capacity, not just the first block.
 *
 * Regression guard for block_find_free(). When it inflated the allocation to
 * mapping_size() of the bin the block was FOUND in, rather than leaving it at
 * the rounded request, the first malloc from a fresh pool swallowed almost the
 * entire arena: a 1 MB pool served exactly two 1 KB allocations (0.2%
 * utilization) before reporting exhaustion.
 */
static void pool_utilization_test(void)
{
    printf("Pool utilization test: ");
    fflush(stdout);

    static char pool[TLSF_TEST_POOL_CLAMP(256 * 1024)];

    for (size_t req = 64; req <= 8192; req <<= 1) {
        tlsf_t t;
        size_t usable = tlsf_pool_init(&t, pool, sizeof(pool));
        assert(usable > 0);
        if (req > usable / 8)
            break; /* too coarse to say anything useful about this pool */

        size_t handed_out = 0;
        unsigned n = 0;
        while (tlsf_malloc(&t, req)) {
            handed_out += req;
            n++;
        }
        tlsf_check(&t);

        /* Every allocation costs a header and is rounded up to a bin boundary,
         * so exact capacity is not predictable. Anything below half the pool
         * means blocks are being inflated, not merely rounded.
         */
        assert(n > 1);
        assert(handed_out > usable / 2);
        printf(".");
        fflush(stdout);
    }
    printf(" done\n");
}

/* TLSF_MAX_POOL_BYTES must equal what tlsf_pool_init() actually accepts.
 *
 * A _Static_assert cannot establish this. The macro and the internal block
 * constants are definitionally the same expression, so asserting they are equal
 * is a tautology that survives any drift in the acceptance logic itself: a
 * different overhead-word count, a changed alignment round-down, an extra
 * sentinel. Only calling the function pins the published ceiling to reality.
 *
 * The ceiling is 256 GiB in the default configuration, far past what can be
 * backed with memory, so the boundary is exercised under a reduced
 * TLSF_MAX_POOL_BITS. CI covers 20 and 24.
 */
static void small_bin_trim_test(void)
{
    printf("Small-bin trim test: ");
    fflush(stdout);

    /* The FL=0 fast path in tlsf_malloc() may satisfy a request from a bin
     * above the one the request maps to. It must still trim the block to the
     * request. Inflating the allocation to the found bin's size hands out the
     * whole block, which is the defect block_find_free() documents for the
     * generic path; the fast path has reintroduced it once already.
     *
     * Everything here is derived from _TLSF_FL_SHIFT rather than written as a
     * literal. ALIGN_SIZE differs by word size, so the fast path covers sizes
     * below 256 on 64-bit but below 128 on 32-bit, and a hard-coded seed size
     * silently stops testing the fast path on one of them.
     */
    const size_t fast_path_max = (size_t) 1 << _TLSF_FL_SHIFT;
    const size_t seed_req = fast_path_max - fast_path_max / 4;

    static char pool[TLSF_TEST_POOL_CLAMP(64 * 1024)];
    tlsf_t t;
    assert(tlsf_pool_init(&t, pool, sizeof(pool)));

    /* Seed one large FL=0 free block, guarded so it cannot coalesce away. */
    void *big = tlsf_malloc(&t, seed_req);
    void *guard = tlsf_malloc(&t, 64);
    assert(big && guard);
    const size_t seeded = tlsf_usable_size(big);

    /* Fail loudly if the seed did not land in FL=0: the rest of this test
     * proves nothing about the fast path unless it did.
     */
    assert(seeded >= seed_req);
    assert(seeded < fast_path_max);
    tlsf_free(&t, big);
    tlsf_check(&t);

    /* A minimal request must not swallow the seeded block. */
    void *probe = tlsf_malloc(&t, 1);
    assert(probe);
    const size_t got = tlsf_usable_size(probe);
    assert(got < seeded);

    /* The trimmed remainder must still be allocatable. TLSF_TEST_BLOCK_COST
     * over-subtracts, which only makes the request more conservative.
     */
    assert(seeded - got > TLSF_TEST_BLOCK_COST);
    void *rest = tlsf_malloc(&t, seeded - got - TLSF_TEST_BLOCK_COST);
    assert(rest);
    tlsf_check(&t);

    tlsf_free(&t, rest);
    tlsf_free(&t, probe);
    tlsf_free(&t, guard);
    tlsf_check(&t);

    printf("%zu-byte FL=0 bin served a minimal request as %zu bytes, done\n",
           seeded, got);
}

static void pool_ceiling_test(void)
{
    printf("Pool ceiling test: ");
    fflush(stdout);

    if (TLSF_MAX_POOL_BYTES > (size_t) 64 << 20) {
        printf(
            "skipped (ceiling %zu bytes exceeds what we can allocate; "
            "build with -DTLSF_MAX_POOL_BITS=24 or less to cover it)\n",
            (size_t) TLSF_MAX_POOL_BYTES);
        return;
    }

    /* One extra alignment step so the reject case has memory behind it. */
    const size_t align = sizeof(void *);
    char *mem = (char *) malloc(TLSF_MAX_POOL_BYTES + align);
    assert(mem);
    assert((size_t) mem % align == 0); /* else adj shifts the boundary */

    tlsf_t t;
    size_t at = tlsf_pool_init(&t, mem, TLSF_MAX_POOL_BYTES);
    assert(at > 0); /* the published ceiling must be accepted */
    tlsf_check(&t);

    size_t over = tlsf_pool_init(&t, mem, TLSF_MAX_POOL_BYTES + align);
    assert(over == 0); /* and one alignment step past it must not be */

    free(mem);
    printf("%zu accepted, +%zu rejected\n", (size_t) TLSF_MAX_POOL_BYTES,
           align);
}

/* Issue #4: a freed block must land in the bin that a same-size request will
 * search, so free-then-reallocate at the same size reuses the same address.
 * Guards against a future change that searches with the rounded size but stores
 * the block at the unrounded one. The block under test is sandwiched between
 * live allocations so it cannot coalesce and mask the result.
 */
static void reuse_same_address_test(void)
{
    printf("Free/realloc address reuse test: ");
    fflush(stdout);

    static char pool[TLSF_TEST_POOL_CLAMP(256 * 1024)];
    unsigned checked = 0;

    for (size_t sz = 24; sz <= 16384; sz = sz + 1 + sz / 8) {
        tlsf_t t;
        assert(tlsf_pool_init(&t, pool, sizeof(pool)));

        void *guard_lo = tlsf_malloc(&t, 64);
        void *p = tlsf_malloc(&t, sz);
        if (!p)
            break; /* larger sizes will not fit either */
        void *guard_hi = tlsf_malloc(&t, 64);
        assert(guard_lo && guard_hi);

        tlsf_free(&t, p);
        void *again = tlsf_malloc(&t, sz);
        assert(again == p);
        tlsf_check(&t);
        checked++;
    }

    assert(checked > 0);
    printf("%u sizes done\n", checked);
}

/* Test pool reset: O(1) bulk deallocation for static pools. */
static void pool_reset_test(void)
{
    printf("Pool reset test: ");
    fflush(stdout);

    /* Test 1: Basic reset - allocate, reset, verify clean state */
    {
        static char pool[1024 * 64]; /* 64 KB */
        tlsf_t t;
        size_t usable = tlsf_pool_init(&t, pool, sizeof(pool));
        assert(usable > 0);

        /* Fill pool with allocations */
        void *ptrs[100];
        int count = 0;
        for (int i = 0; i < 100; i++) {
            ptrs[i] = tlsf_malloc(&t, 200);
            if (!ptrs[i])
                break;
            memset(ptrs[i], 0xAB, 200);
            count++;
        }
        assert(count > 0);

        /* Reset discards all allocations */
        tlsf_pool_reset(&t);
        tlsf_check(&t);

        /* Pool should be back to initial state: one free block */
        tlsf_stats_t stats;
        int rc = tlsf_get_stats(&t, &stats);
        assert(rc == 0);
        assert(stats.free_count == 1);
        assert(stats.total_used == 0);
        assert(stats.total_free == usable);

        /* Pool should be usable after reset */
        void *p = tlsf_malloc(&t, 100);
        assert(p);
        tlsf_free(&t, p);
        tlsf_check(&t);
    }
    printf(".");
    fflush(stdout);

    /* Test 2: Multiple resets in a loop */
    {
        static char pool[16384];
        tlsf_t t;
        size_t usable = tlsf_pool_init(&t, pool, sizeof(pool));
        assert(usable > 0);

        for (int round = 0; round < 10; round++) {
            void *p = tlsf_malloc(&t, 100 * ((size_t) round + 1));
            assert(p);
            void *q = tlsf_malloc(&t, 50);
            assert(q);
            tlsf_free(&t, p);
            /* q is intentionally leaked - reset discards it */

            tlsf_pool_reset(&t);
            tlsf_check(&t);

            /* Verify clean state each round */
            tlsf_stats_t stats;
            tlsf_get_stats(&t, &stats);
            assert(stats.free_count == 1);
            assert(stats.total_used == 0);
            assert(stats.total_free == usable);
        }
    }
    printf(".");
    fflush(stdout);

    /* Test 3: Reset after append_pool preserves expanded capacity */
    {
        static char combined[8192];
        tlsf_t t;
        size_t half = 4096;
        size_t usable = tlsf_pool_init(&t, combined, half);
        assert(usable > 0);

        /* Append second half (adjacent by construction) */
        size_t appended = tlsf_append_pool(&t, combined + half, half);
        assert(appended > 0);

        /* Record full capacity before allocations */
        tlsf_stats_t full_stats;
        tlsf_get_stats(&t, &full_stats);
        size_t full_free = full_stats.total_free;
        assert(full_free > usable); /* Expanded pool is larger */

        /* Allocate and fragment */
        void *p1 = tlsf_malloc(&t, 100);
        assert(p1);

        /* Reset should restore full expanded capacity */
        tlsf_pool_reset(&t);
        tlsf_check(&t);

        tlsf_stats_t after;
        tlsf_get_stats(&t, &after);
        assert(after.free_count == 1);
        assert(after.total_used == 0);
        assert(after.total_free == full_free);
    }
    printf(".");
    fflush(stdout);

    /* Test 4: Reset on NULL or dynamic pool is a no-op */
    {
        tlsf_pool_reset(NULL);

        tlsf_t t = TLSF_INIT;
        tlsf_pool_reset(&t); /* arena is NULL, no-op */
    }
    printf(". done\n");
}

int main(void)
{
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

    /* Run pool reset test */
    pool_reset_test();

    /* Run pool utilization test */
    pool_utilization_test();

    /* Run free/realloc address reuse test */
    reuse_same_address_test();

    /* Run small-bin trim test */
    small_bin_trim_test();

    /* Run pool ceiling test */
    pool_ceiling_test();

    puts("OK!");
    return 0;
}
