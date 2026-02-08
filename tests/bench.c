/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * Benchmark methodology:
 * - Multiple iterations for statistical significance
 * - Warmup phase before measurements
 * - Report median and percentiles (not just mean)
 * - High-resolution timing (mach_absolute_time on macOS, clock_gettime on
 *   Linux)
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <sched.h>
#include <time.h>
#endif

#include "tlsf.h"

static tlsf_t t = TLSF_INIT;

/* Fast xorshift32 PRNG - avoids rand() overhead and mutex in hot loop */
static uint32_t xorshift_state = 1;

static inline uint32_t xorshift32(void)
{
    uint32_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift_state = x;
    return x;
}

/* High-resolution timing abstraction */
#ifdef __APPLE__
static mach_timebase_info_data_t timebase_info;

static inline uint64_t get_time_ns(void)
{
    if (timebase_info.denom == 0)
        mach_timebase_info(&timebase_info);
    /* Avoid overflow: divide first when numer <= denom (common case),
     * otherwise use __uint128_t for intermediate result */
    uint64_t ticks = mach_absolute_time();
    if (timebase_info.numer <= timebase_info.denom)
        return ticks / timebase_info.denom * timebase_info.numer;
#ifdef __SIZEOF_INT128__
    return (uint64_t) (((__uint128_t) ticks * timebase_info.numer) /
                       timebase_info.denom);
#else
    /* Fallback: accept potential precision loss */
    return ticks * timebase_info.numer / timebase_info.denom;
#endif
}
#else
static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}
#endif

/* Comparison function for qsort */
static int compare_double(const void *a, const void *b)
{
    double da = *(const double *) a, db = *(const double *) b;
    return (da > db) - (da < db);
}

/* Statistical analysis structure */
typedef struct {
    double min, max;
    double mean;
    double median;
    double p5;  /* 5th percentile */
    double p95; /* 95th percentile */
    double stddev;
} stats_t;

/* Compute statistics from sorted array */
static void compute_stats(double *samples, size_t n, stats_t *stats)
{
    /* Initialize to safe defaults */
    memset(stats, 0, sizeof(*stats));

    if (n == 0)
        return;

    qsort(samples, n, sizeof(double), compare_double);

    stats->min = samples[0];
    stats->max = samples[n - 1];

    /* Median: average of two middle values for even n */
    if (n % 2 == 0 && n >= 2)
        stats->median = (samples[n / 2 - 1] + samples[n / 2]) / 2.0;
    else
        stats->median = samples[n / 2];

    /* Safe percentile indexing with bounds check */
    size_t p5_idx = (size_t) (n * 0.05);
    size_t p95_idx = (size_t) (n * 0.95);
    if (p5_idx >= n)
        p5_idx = 0;
    if (p95_idx >= n)
        p95_idx = n - 1;
    stats->p5 = samples[p5_idx];
    stats->p95 = samples[p95_idx];

    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += samples[i];
    stats->mean = sum / (double) n;

    double variance = 0.0;
    for (size_t i = 0; i < n; i++) {
        double diff = samples[i] - stats->mean;
        variance += diff * diff;
    }
    stats->stddev = n > 1 ? sqrt(variance / (double) (n - 1)) : 0.0;
}

static void usage(const char *name)
{
    printf(
        "TLSF memory allocator benchmark with statistical analysis.\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  -s size|min:max  Block size or range (default: 512)\n"
        "  -l loops         Operations per iteration (default: 1000000)\n"
        "  -n num-blocks    Number of concurrent blocks (default: 10000)\n"
        "  -i iterations    Number of benchmark iterations (default: 50)\n"
        "  -w warmup        Warmup iterations before measuring (default: 5)\n"
        "  -c               Clear allocated memory (memset to 0)\n"
        "  -q               Quiet mode (machine-readable output only)\n"
        "  -h               Show this help\n\n"
        "Benchmark Methodology:\n"
        "  - Runs warmup iterations to stabilize caches/TLB\n"
        "  - Reports median, min, max, p5, p95, stddev\n"
        "  - Uses high-resolution monotonic clock\n\n"
        "Example:\n"
        "  %s -s 64:4096 -l 100000 -i 50 -w 10\n",
        name, name);
    exit(-1);
}

/* Parse an integer argument (rejects negative values). */
static size_t parse_int_arg(const char *arg, const char *exe_name)
{
    char *endptr;
    errno = 0;
    long ret = strtol(arg, &endptr, 0);
    if (errno || ret < 0 || endptr == arg || *endptr != '\0') {
        fprintf(stderr, "Invalid argument: %s\n", arg);
        usage(exe_name);
    }
    return (size_t) ret;
}

/* Parse a size argument, which is either an integer or two integers separated
 * by a colon, denoting a range.
 */
static void parse_size_arg(const char *arg,
                           const char *exe_name,
                           size_t *blk_min,
                           size_t *blk_max)
{
    char *endptr;
    *blk_min = (size_t) strtol(arg, &endptr, 0);

    if (errno)
        usage(exe_name);

    if (endptr && *endptr == ':') {
        *blk_max = (size_t) strtol(endptr + 1, NULL, 0);
        if (errno)
            usage(exe_name);
    } else {
        *blk_max = *blk_min; /* Single value: min == max */
    }

    if (*blk_min > *blk_max)
        usage(exe_name);
}

/* Get a random block size between blk_min and blk_max. */
static size_t get_random_block_size(size_t blk_min, size_t blk_max)
{
    if (blk_max > blk_min)
        return blk_min + ((size_t) xorshift32() % (blk_max - blk_min));
    return blk_min;
}

/* Reset allocator state for clean iteration */
static void reset_allocator(void **blk_array, size_t num_blks)
{
    for (size_t i = 0; i < num_blks; i++) {
        if (blk_array[i]) {
            tlsf_free(&t, blk_array[i]);
            blk_array[i] = NULL;
        }
    }
}

/* Run one benchmark iteration, return elapsed time in seconds */
static double run_alloc_benchmark(size_t loops,
                                  size_t blk_min,
                                  size_t blk_max,
                                  void **blk_array,
                                  size_t num_blks,
                                  bool clear)
{
    uint64_t start = get_time_ns();

    for (size_t i = 0; i < loops; i++) {
        size_t next_idx = (size_t) xorshift32() % num_blks;
        size_t blk_size = get_random_block_size(blk_min, blk_max);

        if (blk_array[next_idx]) {
            if (xorshift32() % 10 == 0) {
                /* 10% chance: realloc - preserve original on failure */
                void *new_ptr = tlsf_realloc(&t, blk_array[next_idx], blk_size);
                if (new_ptr)
                    blk_array[next_idx] = new_ptr;
                /* else: keep original allocation */
            } else {
                /* 90% chance: free + malloc */
                tlsf_free(&t, blk_array[next_idx]);
                blk_array[next_idx] = tlsf_malloc(&t, blk_size);
            }
        } else {
            blk_array[next_idx] = tlsf_malloc(&t, blk_size);
        }
        if (clear && blk_array[next_idx])
            memset(blk_array[next_idx], 0, blk_size);
    }

    uint64_t end = get_time_ns();

    /* Clean up for next iteration */
    reset_allocator(blk_array, num_blks);

    return (double) (end - start) / 1e9;
}

static size_t max_size;
static void *mem = 0;

void *tlsf_resize(tlsf_t *_t, size_t req_size)
{
    (void) _t;
    return req_size <= max_size ? mem : 0;
}

int main(int argc, char **argv)
{
    size_t blk_min = 512, blk_max = 512, num_blks = 10000;
    size_t loops = 1000000;
    size_t iterations = 50;
    size_t warmup = 5;
    bool clear = false;
    bool quiet = false;
    int opt;

    while ((opt = getopt(argc, argv, "s:l:n:i:w:cqh")) > 0) {
        switch (opt) {
        case 's':
            parse_size_arg(optarg, argv[0], &blk_min, &blk_max);
            break;
        case 'l':
            loops = parse_int_arg(optarg, argv[0]);
            break;
        case 'n':
            num_blks = parse_int_arg(optarg, argv[0]);
            break;
        case 'i':
            iterations = parse_int_arg(optarg, argv[0]);
            break;
        case 'w':
            warmup = parse_int_arg(optarg, argv[0]);
            break;
        case 'c':
            clear = true;
            break;
        case 'q':
            quiet = true;
            break;
        case 'h':
        default:
            usage(argv[0]);
            break;
        }
    }

    /* Validate parameters */
    if (iterations == 0) {
        fprintf(stderr, "Error: iterations (-i) must be > 0\n");
        return 1;
    }
    if (loops == 0) {
        fprintf(stderr, "Error: loops (-l) must be > 0\n");
        return 1;
    }
    if (num_blks == 0) {
        fprintf(stderr, "Error: num-blocks (-n) must be > 0\n");
        return 1;
    }

    /* Allocate pool memory with overflow check */
    if (blk_max > SIZE_MAX / num_blks / 2) {
        fprintf(stderr,
                "Pool size overflow: blk_max=%zu num_blks=%zu would exceed "
                "SIZE_MAX\n",
                blk_max, num_blks);
        return 1;
    }
    max_size = blk_max * num_blks * 2; /* 2x for fragmentation headroom */
    mem = malloc(max_size);
    if (!mem) {
        fprintf(stderr, "Failed to allocate %zu bytes for pool\n", max_size);
        return 1;
    }

    void **blk_array = (void **) calloc(num_blks, sizeof(void *));
    if (!blk_array) {
        fprintf(stderr, "Failed to allocate block array\n");
        free(mem);
        return 1;
    }

    /* Allocate samples array */
    double *samples = (double *) malloc(iterations * sizeof(double));
    if (!samples) {
        fprintf(stderr, "Failed to allocate samples array\n");
        free(blk_array);
        free(mem);
        return 1;
    }

    if (!quiet) {
        printf("TLSF Benchmark Configuration:\n");
        printf("  Block size: %zu - %zu bytes\n", blk_min, blk_max);
        printf("  Operations per iteration: %zu\n", loops);
        printf("  Concurrent blocks: %zu\n", num_blks);
        printf("  Warmup iterations: %zu\n", warmup);
        printf("  Measured iterations: %zu\n", iterations);
        printf("  Pool size: %zu bytes (%.1f MB)\n", max_size,
               (double) max_size / (1024.0 * 1024.0));
        printf("  Clear memory: %s\n\n", clear ? "yes" : "no");
    }

    /* Seed PRNG - print for reproducibility */
    uint32_t seed = (uint32_t) get_time_ns();
    if (seed == 0)
        seed = 1; /* xorshift32 requires non-zero state */
    xorshift_state = seed;

    if (!quiet)
        printf("Random seed: %u (use for reproducibility)\n\n", seed);

    /* Warmup phase - stabilize caches, TLB, branch predictors */
    if (!quiet)
        printf("Warming up (%zu iterations)...\n", warmup);

    for (size_t i = 0; i < warmup; i++) {
        run_alloc_benchmark(loops, blk_min, blk_max, blk_array, num_blks,
                            clear);
    }

    /* Measurement phase */
    if (!quiet)
        printf("Running benchmark (%zu iterations)...\n", iterations);

    for (size_t i = 0; i < iterations; i++) {
        samples[i] = run_alloc_benchmark(loops, blk_min, blk_max, blk_array,
                                         num_blks, clear);
        if (!quiet && (i + 1) % 10 == 0)
            printf("  Completed %zu/%zu iterations\n", i + 1, iterations);
    }

    /* Compute statistics */
    stats_t stats;
    compute_stats(samples, iterations, &stats);

    /* Get memory usage */
    struct rusage usage_info;
    int err = getrusage(RUSAGE_SELF, &usage_info);
    assert(err == 0);

    /* Report results */
    if (quiet) {
        /* Machine-readable format:
         * blk_min:blk_max:loops:iterations:median_us:p5_us:p95_us:stddev_us
         */
        printf("%zu:%zu:%zu:%zu:%.3f:%.3f:%.3f:%.3f\n", blk_min, blk_max, loops,
               iterations, stats.median / (double) loops * 1e6,
               stats.p5 / (double) loops * 1e6,
               stats.p95 / (double) loops * 1e6,
               stats.stddev / (double) loops * 1e6);
    } else {
        printf("\n=== Benchmark Results ===\n");
        printf("Total time per iteration:\n");
        printf("  Min:    %.6f s\n", stats.min);
        printf("  Max:    %.6f s\n", stats.max);
        printf("  Mean:   %.6f s\n", stats.mean);
        printf("  Median: %.6f s\n", stats.median);
        printf("  StdDev: %.6f s\n", stats.stddev);
        printf("  P5:     %.6f s\n", stats.p5);
        printf("  P95:    %.6f s\n", stats.p95);

        printf("\nPer-operation timing (median):\n");
        printf("  %.3f us per malloc/free cycle\n",
               stats.median / (double) loops * 1e6);
        printf("  %.0f ns per malloc/free cycle\n",
               stats.median / (double) loops * 1e9);

        printf("\nThroughput (median):\n");
        printf("  %.0f ops/sec\n", (double) loops / stats.median);

        printf("\nMemory:\n");
#ifdef __APPLE__
        /* macOS: ru_maxrss is in bytes */
        printf("  Peak RSS: %ld KB\n", usage_info.ru_maxrss / 1024);
#else
        /* Linux: ru_maxrss is in kilobytes */
        printf("  Peak RSS: %ld KB\n", usage_info.ru_maxrss);
#endif
        printf("  Pool size: %.1f MB\n", (double) max_size / (1024.0 * 1024.0));

        printf("\nVariability:\n");
        if (stats.mean > 0.0)
            printf("  Coefficient of Variation: %.2f%%\n",
                   (stats.stddev / stats.mean) * 100.0);
        if (stats.median > 0.0)
            printf("  P95/Median ratio: %.2fx\n", stats.p95 / stats.median);
    }

    free(samples);
    free(blk_array);
    free(mem);

    return 0;
}
