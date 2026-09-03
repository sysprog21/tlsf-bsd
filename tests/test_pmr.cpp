/*
 * Exercises the optional std::pmr adapters, tlsf_pmr.hpp and
 * tlsf_thread_pmr.hpp.
 *
 * Kept apart from tests/test_cpp.cpp, which pins the public headers to C++11.
 * The adapters need C++17 for <memory_resource>, and that requirement must not
 * leak into the baseline the library itself promises.
 */

#include <cstdint>
#include <cstdio>
#include <memory_resource>
#include <new>
#include <thread>
#include <vector>

#include "tlsf_pmr.hpp"
#include "tlsf_thread_pmr.hpp"

#include "pool_limits.h"

/* Without exceptions the only outcome left to check is the process dying, and
 * that needs a child to die in.
 */
#if !_TLSF_PMR_EXCEPTIONS && !defined(_WIN32)
#include <csignal>

#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                  \
        }                                                              \
    } while (0)

/* One element per cache line, so a container of these asks the resource for an
 * alignment stricter than the allocator's natural one.
 */
struct alignas(64) cacheline {
    std::uint64_t value;
};

/* Both fixtures are clamped to what the build configuration can hold, the way
 * every other test in this directory sizes its pool.
 *
 * The threaded fixture is sized per arena and multiplied up, not carved out of
 * a fixed total. Memory is partitioned, so what matters is that one arena can
 * serve the largest request the test makes of it, and dividing a fixed total by
 * TLSF_ARENA_COUNT fails that at a high count: at 64 arenas a 256 KiB total
 * leaves 4 KiB each, which is less than one finished vector.
 *
 * The floor is derived rather than picked, and derived for the worst growth
 * policy rather than the local one. A fill_vector() run grows a vector to 64
 * 'cacheline' elements, and its last growth holds the old buffer while
 * allocating the new one. Doubling, which libstdc++ and libc++ use, reaches 64
 * exactly and peaks at 32 + 64 = 96 elements live. MSVC grows by 1.5, so its
 * capacities run 63 then 94 and it peaks at 157. Both workers can land on one
 * arena, taking that to 314, and every block carries a header on top. 512
 * elements' worth covers it with room for those headers and the pool's own
 * sentinel.
 *
 * Sizing this from the local growth factor would have passed everywhere it was
 * run and failed only in the MSVC job, which is the one place this test cannot
 * be reproduced from a POSIX host.
 */
#define CORE_BYTES TLSF_TEST_POOL_CLAMP(64 * 1024)

#define THREAD_ARENA_BYTES (512 * sizeof(cacheline))
#define THREAD_BYTES \
    (TLSF_TEST_POOL_CLAMP(THREAD_ARENA_BYTES) * TLSF_ARENA_COUNT)

alignas(64) static unsigned char core_memory[CORE_BYTES];
alignas(TLSF_CACHELINE_SIZE) static unsigned char thread_memory[THREAD_BYTES];

static bool is_aligned(const void *p, std::size_t align)
{
    return (reinterpret_cast<std::uintptr_t>(p) & (align - 1)) == 0;
}

/* A request no pool can serve must be reported, never answered with a null
 * pointer, which is what every std::pmr consumer expects. How it is reported is
 * what the build decides, so the mode picks a body here once and every caller
 * asks the same question.
 *
 * With exceptions, catch the std::bad_alloc. The block is returned on the path
 * that must not be taken, so a regression shows up as a failed check and not as
 * a leak.
 *
 * Without them the pass condition is that the child never comes back:
 * 'do_allocate()' may not return null, so std::terminate() aborts through its
 * default handler, and any other outcome, a clean exit above all, means the
 * null leaked out to the caller. The child's stderr goes to /dev/null because
 * the runtime prints a line there before aborting, and an expected failure that
 * looks like a crash in the log is how a real one gets missed.
 */
#if _TLSF_PMR_EXCEPTIONS || !defined(_WIN32)
#define HAVE_EXHAUSTION_CHECK 1
static bool fails_on_exhaustion(std::pmr::memory_resource *res,
                                std::size_t bytes)
{
#if _TLSF_PMR_EXCEPTIONS
    try {
        void *p = res->allocate(bytes, 8);
        res->deallocate(p, bytes, 8);
    } catch (const std::bad_alloc &) {
        return true;
    }
    return false;
#else
    pid_t child = fork();
    if (child == 0) {
        if (!freopen("/dev/null", "w", stderr))
            _exit(2);
        void *p = res->allocate(bytes, 8);
        res->deallocate(p, bytes, 8);
        _exit(0);
    }
    if (child < 0)
        return false;

    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
#endif
}
#else
/* Windows with exceptions off: neither arm is available, since there is no
 * throw to catch and no fork to die in. Nothing else in the file is skipped.
 */
#define HAVE_EXHAUSTION_CHECK 0
#endif

/* Grow past several reallocations, so the resource sees allocate and deallocate
 * of differing sizes rather than a single fresh block.
 */
static int fill_vector(std::pmr::memory_resource *res)
{
    std::pmr::vector<cacheline> v(res);
    for (std::uint64_t i = 0; i < 64; ++i) {
        v.push_back(cacheline{i});
        CHECK(is_aligned(v.data(), alignof(cacheline)));
    }
    for (std::uint64_t i = 0; i < 64; ++i)
        CHECK(v[i].value == i);
    return 0;
}

/* Distinct resources over one pool never compare equal, which is what a
 * container consults before assuming it may free another's blocks.
 *
 * Through is_equal(), which forwards to do_is_equal() unconditionally, rather
 * than operator==. The latter is specified as '&a == &b || a.is_equal(b)', so
 * a same-object comparison short-circuits before the class is reached and a
 * distinct-object comparison is satisfied by any override that answers false.
 * Between them those two cases cannot catch a do_is_equal() stuck at false, and
 * an earlier version of this test using operator== stayed green with both
 * bodies replaced by 'return false'. Through is_equal() that same mutation
 * fails the first check below.
 */
static int run_rounds(std::pmr::memory_resource *res)
{
    for (int round = 0; round < 32; ++round)
        if (fill_vector(res) != 0)
            return 1;
    return 0;
}

/* An exception escaping a thread's entry point is a terminate(), which reports
 * the failure as a crash instead of the file and line CHECK exists to print.
 *
 * A whole body per mode rather than a 'try' opened in one preprocessor branch
 * and closed in another: the latter balances its braces in no configuration a
 * reader can follow from top to bottom.
 */
static int run_rounds_guarded(std::pmr::memory_resource *res)
{
#if _TLSF_PMR_EXCEPTIONS
    try {
        return run_rounds(res);
    } catch (const std::bad_alloc &) {
        return 1;
    }
#else
    return run_rounds(res);
#endif
}

static int check_identity(std::pmr::memory_resource &res,
                          std::pmr::memory_resource &other)
{
    CHECK(res.is_equal(res));
    CHECK(!res.is_equal(other));
    return 0;
}

/* Both instances are static for the reason tests/test_thread.c makes its own
 * static: 'tlsf_t' is several kilobytes and 'tlsf_thread_t' is that again per
 * arena, so a stack copy grows with TLSF_ARENA_COUNT and TLSF_CACHELINE_SIZE
 * until it no longer fits a modest main stack.
 */
static tlsf_t core_pool = TLSF_INIT;
static tlsf_thread_t thread_pool;

/* The floor above is reasoned from a growth factor this toolchain may not use,
 * so pin it with an allocation instead of trusting the reasoning: both workers'
 * worst-case peaks must be servable at once. Without this the MSVC job would be
 * the first to find a pool too small, and it is the one job that cannot be
 * reproduced from a POSIX host.
 */
/* Peak elements live in one arena during a fill_vector() growth, taken from the
 * 1.5 factor rather than the local one, and the two workers' share of it.
 */
#define GROWTH_PEAK 157

/* Through the C API rather than the resource, because this asks about pool
 * capacity and nothing else. tlsf_thread_aalloc() reports failure with a null
 * in every build, so the check does not inherit the resource's failure policy
 * and go missing wherever that policy cannot be observed.
 */
static int check_peak_fits(tlsf_thread_t *pool)
{
    const std::size_t peak = GROWTH_PEAK * sizeof(cacheline);
    void *a = tlsf_thread_aalloc(pool, alignof(cacheline), peak);
    void *b = tlsf_thread_aalloc(pool, alignof(cacheline), peak);

    bool both = a && b;
    if (a)
        tlsf_thread_free(pool, a);
    if (b)
        tlsf_thread_free(pool, b);
    CHECK(both);
    return 0;
}

static int core_test(void)
{
    tlsf_t &pool = core_pool;
    CHECK(tlsf_pool_init(&pool, core_memory, sizeof(core_memory)) > 0);

    tlsf::pmr_resource res(pool);
    CHECK(fill_vector(&res) == 0);
    tlsf_check(&pool);

    tlsf::pmr_resource other(pool);
    CHECK(check_identity(res, other) == 0);

#if HAVE_EXHAUSTION_CHECK
    CHECK(fails_on_exhaustion(&res, sizeof(core_memory) * 2));
#endif
    tlsf_check(&pool);
    return 0;
}

static int thread_test(void)
{
    tlsf_thread_t &pool = thread_pool;
    CHECK(tlsf_thread_init(&pool, thread_memory, sizeof(thread_memory)) > 0);

    CHECK(check_peak_fits(&pool) == 0);

    tlsf::pmr_thread_resource res(pool);

    int failed[2] = {0, 0};
    std::thread workers[2];
    for (int i = 0; i < 2; ++i) {
        workers[i] = std::thread(
            [&res, &failed, i] { failed[i] = run_rounds_guarded(&res); });
    }
    for (std::thread &w : workers)
        w.join();
    CHECK(!failed[0] && !failed[1]);
    tlsf_thread_check(&pool);

    /* The partition failure, which is the one a PMR user is most likely to be
     * surprised by: tlsf_thread.h promises allocation fails when no single
     * arena can serve the request, even where the arenas together hold far more
     * than it asks for. A request past the largest arena but inside the total
     * is exactly that gap. Self-skips at TLSF_ARENA_COUNT=1, where the two
     * figures are equal and the gap does not exist.
     */
#if HAVE_EXHAUSTION_CHECK
    tlsf_stats_t stats;
    CHECK(tlsf_thread_stats(&pool, &stats) == 0);
    if (stats.total_free > stats.largest_free)
        CHECK(fails_on_exhaustion(&res, stats.largest_free + 1));

    CHECK(fails_on_exhaustion(&res, sizeof(thread_memory) * 2));
#endif

    tlsf::pmr_thread_resource other(pool);
    CHECK(check_identity(res, other) == 0);

    tlsf_thread_check(&pool);
    tlsf_thread_destroy(&pool);
    return 0;
}

int main()
{
    int failed = core_test();
    failed |= thread_test();
    if (!failed)
        printf("test_pmr: ok\n");
    return failed;
}
