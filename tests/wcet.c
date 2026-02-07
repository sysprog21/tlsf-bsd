/*
 * Copyright (c) 2016 National Cheng Kung University, Taiwan.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style license.
 *
 * WCET (Worst-Case Execution Time) measurement for TLSF allocator.
 *
 * Measures per-operation latency under pathological scenarios to bound
 * the O(1) constant of TLSF's malloc and free operations.
 *
 * Two worst-case scenarios (from TLSF-WCET):
 *   malloc: Small request from a pool with one huge free block.
 *           Full bitmap search + split + remainder insertion.
 *   free:   Block sandwiched between two free neighbors.
 *           Two merges + two list removals + one insertion.
 *
 * Two best-case baselines for comparison:
 *   malloc: Exact bin hit, no split required.
 *   free:   No merge possible (used neighbors on both sides).
 *
 * Timing: rdtsc on x86-64, cntvct_el0 on ARM64, clock_gettime fallback.
 * Addresses TLSF-WCET limitations: clock() resolution, single-size
 * testing, no optimization-level variation.
 */

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#include "tlsf.h"

/* --- Timing primitives --- */

typedef uint64_t tick_t;

/*
 * Platform-specific cycle/tick counter.
 *
 * x86-64: lfence + rdtsc gives cycle-accurate measurement with ~5 cycle
 * overhead.  lfence serializes preceding instructions without the full
 * cost of cpuid (~100 cycles).
 *
 * ARM64 (Linux): cntvct_el0 reads the generic timer counter.  Not true
 * CPU cycles, but a fixed-frequency counter suitable for latency
 * measurement.  isb ensures instruction stream synchronization.
 * Resolution depends on counter frequency (typically 25-100 MHz,
 * i.e. 10-40 ns granularity); subtle regressions may be invisible.
 * For cycle-accurate ARM measurements, enable userspace PMU access
 * to PMCCNTR_EL0 via perf_event or a kernel module.
 *
 * macOS: mach_absolute_time() with timebase conversion to nanoseconds.
 * Works on both Intel and Apple Silicon.
 *
 * Fallback: clock_gettime(CLOCK_MONOTONIC) gives nanosecond resolution,
 * a major improvement over TLSF-WCET's clock() (millisecond resolution).
 */
#if defined(__x86_64__) || defined(__i386__)
#define TICK_UNIT "cycles"

static inline tick_t read_tick(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtsc" : "=a"(lo), "=d"(hi));
    return ((tick_t) hi << 32) | lo;
}

#elif defined(__aarch64__) && !defined(__APPLE__)
#define TICK_UNIT "ticks"

static inline tick_t read_tick(void)
{
    tick_t val;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(val));
    return val;
}

#elif defined(__APPLE__)
#define TICK_UNIT "ns"

static mach_timebase_info_data_t tb_info;

static inline tick_t read_tick(void)
{
    if (tb_info.denom == 0)
        mach_timebase_info(&tb_info);
    uint64_t ticks = mach_absolute_time();
    if (tb_info.numer <= tb_info.denom)
        return ticks / tb_info.denom * tb_info.numer;
#ifdef __SIZEOF_INT128__
    return (uint64_t) (((__uint128_t) ticks * tb_info.numer) / tb_info.denom);
#else
    return ticks * tb_info.numer / tb_info.denom;
#endif
}

#else
#define TICK_UNIT "ns"

static inline tick_t read_tick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (tick_t) ts.tv_sec * 1000000000ULL + (tick_t) ts.tv_nsec;
}
#endif

/* --- Statistics --- */

static int cmp_tick(const void *a, const void *b)
{
    tick_t va = *(const tick_t *) a;
    tick_t vb = *(const tick_t *) b;
    return (va > vb) - (va < vb);
}

typedef struct {
    tick_t min, max;
    tick_t p50, p90, p99, p999;
    double mean, stddev;
} latency_stats_t;

static void compute_latency_stats(tick_t *samples, size_t n, latency_stats_t *s)
{
    if (!n) {
        memset(s, 0, sizeof(*s));
        return;
    }

    qsort(samples, n, sizeof(tick_t), cmp_tick);

    s->min = samples[0];
    s->max = samples[n - 1];
    s->p50 = samples[n / 2];

    size_t p90_idx = (size_t) ((double) n * 0.90);
    size_t p99_idx = (size_t) ((double) n * 0.99);
    size_t p999_idx = (size_t) ((double) n * 0.999);
    if (p90_idx >= n)
        p90_idx = n - 1;
    if (p99_idx >= n)
        p99_idx = n - 1;
    if (p999_idx >= n)
        p999_idx = n - 1;

    s->p90 = samples[p90_idx];
    s->p99 = samples[p99_idx];
    s->p999 = samples[p999_idx];

    double sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (double) samples[i];
    s->mean = sum / (double) n;

    double var = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double) samples[i] - s->mean;
        var += d * d;
    }
    s->stddev = n > 1 ? sqrt(var / (double) (n - 1)) : 0;
}

/* --- Cache control ---
 *
 * Cold-cache mode (-C): stride through a large buffer before each timed
 * operation to evict the tlsf_t control structure (~7 KB) and heap block
 * headers from L1/L2 cache.  This simulates the real-time worst case
 * where an allocation happens in an interrupt handler after a long idle
 * period.
 *
 * Without -C, measurements reflect hot-cache behavior: the bitmap and
 * block headers sit in L1/L2 from the preceding setup code.  Hot-cache
 * numbers isolate algorithmic cost; cold-cache numbers bound the true
 * system-level WCET including memory latency.
 */

#define THRASH_SIZE (64U << 20) /* 64 MB -- exceeds typical L2 and most L3 */

static char *thrash_buf;

static void cache_thrash(void)
{
    if (!thrash_buf)
        return;
    /* Read every cache line to evict pool data from cache hierarchy.
     * Volatile pointer prevents compiler from optimizing away reads.
     */
    volatile const char *p = (volatile const char *) thrash_buf;
    for (size_t i = 0; i < THRASH_SIZE; i += 64)
        (void) p[i];
}

/* --- Scenario measurement functions ---
 *
 * Each function sets up the pathological heap state, then measures the
 * target operation in a tight loop.  The pool is re-initialized per
 * iteration to ensure consistent starting conditions.
 *
 * Static pools (tlsf_pool_init) are used exclusively: no tlsf_resize
 * callback, no arena growth during measurement.
 */

/*
 * malloc worst case: Pool has a single huge free block, request is small.
 *
 * This forces the longest bitmap search path: the requested size maps
 * to a low FL/SL bin, but the only available block is in a high FL bin.
 * block_find_suitable must scan the SL bitmap (all zeros), then the FL
 * bitmap to find the distant block.  After extraction, the block is
 * split and the remainder is inserted into its bin.
 *
 * Full path: adjust_size -> round_block_size -> mapping ->
 *   block_find_suitable (full bitmap search) -> remove_free_block ->
 *   block_use -> block_rtrim_free (split + insert) -> block_set_free
 */
static void measure_malloc_worst(char *pool,
                                 size_t pool_size,
                                 size_t alloc_size,
                                 size_t iterations,
                                 size_t warmup,
                                 tick_t *samples)
{
    tlsf_t t;

    for (size_t i = 0; i < warmup; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        void *p = tlsf_malloc(&t, alloc_size);
        assert(p);
        tlsf_free(&t, p);
    }

    for (size_t i = 0; i < iterations; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        cache_thrash();

        tick_t start = read_tick();
        void *p = tlsf_malloc(&t, alloc_size);
        tick_t end = read_tick();

        assert(p);
        (void) p;
        samples[i] = end - start;
    }
}

/*
 * malloc best case: A block exists in the exact bin for the request.
 *
 * Setup: allocate a block of the target size, then allocate a separator
 * (prevents coalescing), then free the target block.  The freed block
 * lands in its size-class bin.  The subsequent measured malloc hits the
 * exact bin immediately -- no bitmap scanning beyond the target SL.
 * The block size matches the bin's minimum, so no split occurs.
 *
 * Short path: adjust_size -> round_block_size -> mapping ->
 *   block_find_suitable (immediate hit) -> remove_free_block ->
 *   block_use (no split) -> block_set_free
 */
static void measure_malloc_best(char *pool,
                                size_t pool_size,
                                size_t alloc_size,
                                size_t iterations,
                                size_t warmup,
                                tick_t *samples)
{
    tlsf_t t;

    for (size_t i = 0; i < warmup; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        void *a = tlsf_malloc(&t, alloc_size);
        void *sep = tlsf_malloc(&t, 1);
        assert(a && sep);
        tlsf_free(&t, a);
        void *b = tlsf_malloc(&t, alloc_size);
        assert(b);
        (void) b;
    }

    for (size_t i = 0; i < iterations; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        void *a = tlsf_malloc(&t, alloc_size);
        void *sep = tlsf_malloc(&t, 1);
        assert(a && sep);
        (void) sep;
        tlsf_free(&t, a);
        cache_thrash();

        tick_t start = read_tick();
        void *b = tlsf_malloc(&t, alloc_size);
        tick_t end = read_tick();

        assert(b);
        (void) b;
        samples[i] = end - start;
    }
}

/*
 * Allocate three adjacent blocks of the requested size from a static pool.
 *
 * TLSF's block_find_free updates the request size to mapping_size(found_bin),
 * which can be far larger than the original request when the pool has a
 * single huge free block (e.g., requesting 1024 from a 4MB pool allocates
 * ~4MB due to bin-minimum inflation).  This prevents multiple allocations
 * from a fresh pool.
 *
 * Workaround: allocate each block at the inflated size, then immediately
 * realloc down to the target size.  Realloc's trim path (block_rtrim_used)
 * splits the oversized block, returning the excess to the free list.
 * The excess merges with any adjacent free block, making space for the
 * next allocation.  After three malloc+realloc cycles, we have three
 * adjacent blocks of the correct size.
 */
static void alloc_three_blocks(tlsf_t *t,
                               size_t alloc_size,
                               void **a,
                               void **b,
                               void **c)
{
    *a = tlsf_malloc(t, alloc_size);
    assert(*a);
    *a = tlsf_realloc(t, *a, alloc_size);
    assert(*a);

    *b = tlsf_malloc(t, alloc_size);
    assert(*b);
    *b = tlsf_realloc(t, *b, alloc_size);
    assert(*b);

    *c = tlsf_malloc(t, alloc_size);
    assert(*c);
    *c = tlsf_realloc(t, *c, alloc_size);
    assert(*c);
}

/*
 * free worst case: Block sandwiched between two free neighbors.
 *
 * Setup: allocate three adjacent blocks A, B, C using the
 * alloc+realloc trick (see alloc_three_blocks), then free A
 * (left neighbor becomes free) and free C (right neighbor merges
 * with the pool remainder, creating a large free block).
 * Now B has a free block on each side.
 *
 * Freeing B triggers the maximum-work path:
 *   1. block_merge_prev: remove A from free list, absorb A into B
 *   2. block_merge_next: remove C+remainder from free list, absorb
 *   3. block_insert: insert the fully merged block
 *
 * This is 2 list removals + 2 absorbs + 1 insertion = worst case.
 */
static void measure_free_worst(char *pool,
                               size_t pool_size,
                               size_t alloc_size,
                               size_t iterations,
                               size_t warmup,
                               tick_t *samples)
{
    tlsf_t t;
    void *a, *b, *c;

    for (size_t i = 0; i < warmup; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        alloc_three_blocks(&t, alloc_size, &a, &b, &c);
        tlsf_free(&t, a);
        tlsf_free(&t, c);
        tlsf_free(&t, b);
    }

    for (size_t i = 0; i < iterations; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        alloc_three_blocks(&t, alloc_size, &a, &b, &c);
        tlsf_free(&t, a);
        tlsf_free(&t, c);
        cache_thrash();

        tick_t start = read_tick();
        tlsf_free(&t, b);
        tick_t end = read_tick();

        samples[i] = end - start;
    }
}

/*
 * free best case: Block between two used neighbors (no merge).
 *
 * Setup: allocate three adjacent blocks A, B, C (using the
 * alloc+realloc trick).  All remain allocated.  Freeing B finds used
 * blocks on both sides, so block_merge_prev and block_merge_next both
 * skip.  Only a single list insertion occurs.
 *
 * Minimal path: block_from_payload -> block_set_free -> block_link_next
 *   -> block_merge_prev (skip) -> block_merge_next (skip) ->
 *   block_insert
 */
static void measure_free_best(char *pool,
                              size_t pool_size,
                              size_t alloc_size,
                              size_t iterations,
                              size_t warmup,
                              tick_t *samples)
{
    tlsf_t t;
    void *a, *b, *c;

    for (size_t i = 0; i < warmup; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        alloc_three_blocks(&t, alloc_size, &a, &b, &c);
        tlsf_free(&t, b);
    }

    for (size_t i = 0; i < iterations; i++) {
        tlsf_pool_init(&t, pool, pool_size);
        alloc_three_blocks(&t, alloc_size, &a, &b, &c);
        cache_thrash();

        tick_t start = read_tick();
        tlsf_free(&t, b);
        tick_t end = read_tick();

        samples[i] = end - start;
    }
}

/* --- Configuration --- */

static const size_t test_sizes[] = {16, 64, 256, 1024, 4096};
#define NUM_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

typedef void (*measure_fn)(char *, size_t, size_t, size_t, size_t, tick_t *);

typedef struct {
    const char *name;
    const char *desc;
    measure_fn measure;
} scenario_t;

static const scenario_t scenarios[] = {
    {"malloc_worst", "small alloc from single huge block",
     measure_malloc_worst},
    {"malloc_best", "exact bin hit, no split", measure_malloc_best},
    {"free_worst", "sandwiched between two free blocks", measure_free_worst},
    {"free_best", "no merge (used neighbors)", measure_free_best},
};
#define NUM_SCENARIOS (sizeof(scenarios) / sizeof(scenarios[0]))

/* --- Argument parsing --- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "TLSF WCET (Worst-Case Execution Time) measurement.\n\n"
            "Measures per-operation latency under pathological scenarios\n"
            "to bound the O(1) constant of TLSF malloc/free.\n\n"
            "Usage: %s [options]\n\n"
            "Options:\n"
            "  -i N       Measured iterations per scenario (default: 10000)\n"
            "  -w N       Warmup iterations (default: 1000)\n"
            "  -p SIZE    Pool size in bytes (default: 4194304)\n"
            "  -c         CSV output (machine-readable summary)\n"
            "  -r FILE    Write raw samples to FILE (for plotting)\n"
            "  -C         Cold-cache mode (64 MB thrash between iterations)\n"
            "  -h         Show this help\n\n"
            "Scenarios:\n",
            prog);

    for (size_t i = 0; i < NUM_SCENARIOS; i++)
        fprintf(stderr, "  %-14s %s\n", scenarios[i].name, scenarios[i].desc);

    fprintf(stderr,
            "\nTimer: %s\n\n"
            "Example:\n"
            "  %s -i 10000 -c                    # CSV summary\n"
            "  %s -i 10000 -r samples.csv         # raw data for plotting\n"
            "  %s -i 100 -w 10                    # quick validation\n",
            TICK_UNIT, prog, prog, prog);
    exit(1);
}

static size_t parse_size_arg(const char *arg, const char *name)
{
    char *end;
    unsigned long val = strtoul(arg, &end, 0);
    if (*end != '\0' || end == arg) {
        fprintf(stderr, "Invalid %s: %s\n", name, arg);
        exit(1);
    }
    return (size_t) val;
}

/* --- Main --- */

#define DEFAULT_POOL_SIZE ((size_t) 4 << 20) /* 4 MB */

int main(int argc, char **argv)
{
    size_t iterations = 10000;
    size_t warmup = 1000;
    size_t pool_size = DEFAULT_POOL_SIZE;
    bool csv_mode = false;
    bool cold_cache = false;
    const char *raw_file = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "i:w:p:cr:Ch")) > 0) {
        switch (opt) {
        case 'i':
            iterations = parse_size_arg(optarg, "iterations");
            break;
        case 'w':
            warmup = parse_size_arg(optarg, "warmup");
            break;
        case 'p':
            pool_size = parse_size_arg(optarg, "pool size");
            break;
        case 'c':
            csv_mode = true;
            break;
        case 'r':
            raw_file = optarg;
            break;
        case 'C':
            cold_cache = true;
            break;
        case 'h':
        default:
            usage(argv[0]);
        }
    }

    if (!iterations) {
        fprintf(stderr, "Error: iterations must be > 0\n");
        return 1;
    }
    if (pool_size < 4096) {
        fprintf(stderr, "Error: pool size must be >= 4096\n");
        return 1;
    }

    /* Allocate pool and sample buffer */
    char *pool = (char *) malloc(pool_size);
    tick_t *samples = (tick_t *) malloc(iterations * sizeof(tick_t));
    if (!pool || !samples) {
        fprintf(stderr, "Failed to allocate memory\n");
        free(pool);
        free(samples);
        return 1;
    }

    if (cold_cache) {
        thrash_buf = (char *) malloc(THRASH_SIZE);
        if (!thrash_buf) {
            fprintf(stderr, "Failed to allocate cache thrash buffer\n");
            free(pool);
            free(samples);
            return 1;
        }
        /* Touch all pages to prevent copy-on-write faults during measurement */
        memset(thrash_buf, 0xAA, THRASH_SIZE);
    }

    FILE *raw_fp = NULL;
    if (raw_file) {
        raw_fp = fopen(raw_file, "w");
        if (!raw_fp) {
            fprintf(stderr, "Failed to open %s\n", raw_file);
            free(thrash_buf);
            free(pool);
            free(samples);
            return 1;
        }
        fprintf(raw_fp, "scenario,size,unit,value\n");
    }

    /* Header */
    if (csv_mode) {
        printf(
            "scenario,size,samples,unit,min,p50,p90,p99,p999,max,mean,"
            "stddev\n");
    } else {
        printf("TLSF WCET Analysis\n");
        printf("==================\n");
        printf("Timer:      %s\n", TICK_UNIT);
        printf("Cache:      %s\n", thrash_buf ? "cold (64 MB thrash)" : "hot");
        printf("Pool:       %zu bytes (%.1f MB)\n", pool_size,
               (double) pool_size / (1024.0 * 1024.0));
        printf("Iterations: %zu (warmup: %zu)\n", iterations, warmup);
        printf("Sizes:     ");
        for (size_t s = 0; s < NUM_SIZES; s++)
            printf(" %zu", test_sizes[s]);
        printf(" bytes\n\n");
    }

    /* Run all scenarios */
    for (size_t sc = 0; sc < NUM_SCENARIOS; sc++) {
        if (!csv_mode) {
            printf("--- %s (%s) ---\n", scenarios[sc].name, scenarios[sc].desc);
            printf("  %6s %10s %10s %10s %10s %10s %10s %10s %10s\n", "size",
                   "min", "p50", "p90", "p99", "p99.9", "max", "mean",
                   "stddev");
        }

        for (size_t si = 0; si < NUM_SIZES; si++) {
            size_t sz = test_sizes[si];

            scenarios[sc].measure(pool, pool_size, sz, iterations, warmup,
                                  samples);

            /* Write raw samples before sorting (compute_latency_stats sorts
             * in place) */
            if (raw_fp) {
                for (size_t i = 0; i < iterations; i++)
                    fprintf(raw_fp, "%s,%zu,%s,%" PRIu64 "\n",
                            scenarios[sc].name, sz, TICK_UNIT, samples[i]);
            }

            latency_stats_t st;
            compute_latency_stats(samples, iterations, &st);

            if (csv_mode) {
                printf("%s,%zu,%zu,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.1f,%.1f\n",
                       scenarios[sc].name, sz, iterations, TICK_UNIT, st.min,
                       st.p50, st.p90, st.p99, st.p999, st.max, st.mean,
                       st.stddev);
            } else {
                printf("  %6zu %10" PRIu64 " %10" PRIu64 " %10" PRIu64
                       " %10" PRIu64 " %10" PRIu64 " %10" PRIu64
                       " %10.1f %10.1f\n",
                       sz, st.min, st.p50, st.p90, st.p99, st.p999, st.max,
                       st.mean, st.stddev);
            }
        }

        if (!csv_mode)
            printf("\n");
    }

    /* Summary: worst/best ratios */
    if (!csv_mode) {
        printf("--- worst/best ratio (p99) ---\n");
        printf("  %6s %10s %10s\n", "size", "malloc", "free");

        for (size_t si = 0; si < NUM_SIZES; si++) {
            size_t sz = test_sizes[si];
            latency_stats_t mw, mb, fw, fb;

            /* Re-measure with minimal iterations for ratio computation.
             * The samples array was already overwritten, so reuse it.
             */
            scenarios[0].measure(pool, pool_size, sz, iterations, warmup,
                                 samples);
            compute_latency_stats(samples, iterations, &mw);

            scenarios[1].measure(pool, pool_size, sz, iterations, warmup,
                                 samples);
            compute_latency_stats(samples, iterations, &mb);

            scenarios[2].measure(pool, pool_size, sz, iterations, warmup,
                                 samples);
            compute_latency_stats(samples, iterations, &fw);

            scenarios[3].measure(pool, pool_size, sz, iterations, warmup,
                                 samples);
            compute_latency_stats(samples, iterations, &fb);

            double malloc_ratio =
                mb.p99 ? (double) mw.p99 / (double) mb.p99 : 0;
            double free_ratio = fb.p99 ? (double) fw.p99 / (double) fb.p99 : 0;

            printf("  %6zu %9.2fx %9.2fx\n", sz, malloc_ratio, free_ratio);
        }
        printf("\n");
    }

    if (raw_fp)
        fclose(raw_fp);
    free(thrash_buf);
    free(samples);
    free(pool);

    return 0;
}
