/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Thread-safe TLSF wrapper with fine-grained per-arena locking.
 *
 * Instead of a single coarse mutex around the entire allocator, the pool is
 * split into TLSF_ARENA_COUNT independent sub-pools (arenas), each with its own
 * lock. An allocation prefers the arena at mix(TLSF_THREAD_HINT()) modulo the
 * live arena count, so allocations from different threads usually hit different
 * locks. Nothing guarantees distinct arenas: hints can collide, and an arena
 * that is locked or full is skipped for another. Where TLSF_THREAD_HINT()
 * cannot identify a thread it is the constant 0, which funnels every thread to
 * arena 0 and forfeits the whole benefit; see the lock abstraction below.
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
 * (FreeRTOS semaphore, Zephyr k_mutex, bare-metal spinlock, etc.). Default:
 * POSIX pthread_mutex_t.
 */

#pragma once

#include "tlsf.h"

#include <stddef.h>
#include <stdint.h>

/* Lock abstraction
 *
 * Override ALL six lock macros together before including this header. When
 * providing custom locks, also define TLSF_THREAD_HINT() to return a
 * thread-specific unsigned integer for arena selection. Without it the hint
 * falls back to the constant 0, every thread selects arena 0, and the per-arena
 * locking degenerates to a single lock.
 *
 * TLSF_LOCK_INIT must evaluate to an int: 0 on success, non-zero on failure.
 * tlsf_thread_init() checks it and aborts initialization if a lock cannot be
 * created. The other five macros are used as statements or as a boolean
 * (TLSF_LOCK_TRY) and their values are otherwise ignored.
 *
 * Example (FreeRTOS):
 *   #define TLSF_LOCK_T           SemaphoreHandle_t
 *   #define TLSF_LOCK_INIT(l)     ((*(l) = xSemaphoreCreateMutex()) ? 0 : -1)
 *   #define TLSF_LOCK_DESTROY(l)  vSemaphoreDelete(*(l))
 *   #define TLSF_LOCK_ACQUIRE(l)  xSemaphoreTake(*(l), portMAX_DELAY)
 *   #define TLSF_LOCK_RELEASE(l)  xSemaphoreGive(*(l))
 *   #define TLSF_LOCK_TRY(l)      (xSemaphoreTake(*(l),0)==pdTRUE)
 *   #define TLSF_THREAD_HINT()    ((unsigned)uxTaskGetTaskNumber(NULL))
 *   #include "tlsf_thread.h"
 */

#ifndef TLSF_LOCK_T
/* Define TLSF_C11_THREADS to select the C11 '<threads.h>' backend. It reaches
 * the public symbol names, so it belongs in the build system and has to be seen
 * by every translation unit or by none, exactly as tlsf.h asks of its own
 * knobs.
 *
 * The arms below answer one question, "is there a '<threads.h>' to use", by
 * whatever means the dialect offers, and they have to answer it alike or the
 * two languages end up on different layouts.
 *
 * __STDC_NO_THREADS__ is therefore tested once, ahead of the split, and not
 * inside any arm. The name says C, the practice does not: clang defines it in
 * C++ mode, and defines it for its MSVC targets in both languages. An arm that
 * skipped it would answer differently from an arm that did not, which is how
 * clang-cl came to pick C11 threads from C and the native lock from C++ off the
 * same flag.
 *
 * After that, C++ has no __STDC_VERSION__ to read and no compiler switch to
 * demand, so the header's own presence is the evidence. __has_include is tested
 * for before it is used, and the toolset version answers where it is missing.
 * That fallback should never fire: GCC and clang offer the probe in every mode,
 * and MSVC has offered it since VS 2017 15.3, well below the version this arm
 * would then have to accept. It stays because the cost is two lines and the
 * failure it covers is the one this change exists to remove, a C++ unit quietly
 * on the native lock beside a C unit on 'mtx_t' off one flag. C11 threads ship
 * in the VC runtime, not the Windows SDK, and arrived in VS 2022 17.8 together
 * with 'vcruntime140_threads.dll'; that is where 1938 comes from. C reaches the
 * same number through its own arm, which can also say what is wrong, an error
 * no C++ build could have acted on.
 *
 * The generic C arm probes as well, for the mirror-image case: a C11 compiler
 * that promises threads through __STDC_VERSION__ against a libc that ships no
 * header. Taking its word there would fail on the include below while the C++
 * unit beside it quietly took the native lock. Where the probe is missing the
 * promise is all there is, which is where this arm started.
 *
 * That leaves g++ against a C library that sets __STDC_NO_THREADS__ while still
 * shipping the header, where C declines and C++ accepts. The flag is opt-in,
 * and the ABI suffix turns the disagreement into a link error rather than a
 * corrupt 'tlsf_thread_t'.
 */
#if defined(TLSF_C11_THREADS) && !defined(__STDC_NO_THREADS__)
#if defined(__cplusplus)
#if defined(__has_include)
#if __has_include(<threads.h>)
#define _TLSF_USE_C11_THREADS 1
#endif
#elif defined(_MSC_VER) && (_MSC_VER >= 1938)
#define _TLSF_USE_C11_THREADS 1
#endif
#elif defined(_MSC_VER)
#if (_MSC_VER < 1938)
#error Incompatible Visual C++ version. Requires VS 2022 17.8+ for C11 threads support.
#elif !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 201112L)
#error MSVC /std:c11 compiler switch is missing! Please enable C11 standard or higher in project properties.
#else
#define _TLSF_USE_C11_THREADS 1
#endif
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#if defined(__has_include)
#if __has_include(<threads.h>)
#define _TLSF_USE_C11_THREADS 1
#endif
#else
#define _TLSF_USE_C11_THREADS 1
#endif
#endif
#endif

#if defined(_WIN32) || defined(WIN32) || defined(__WIN32__) || defined(_WIN64)
#define TLSF_THREAD_WIN
#elif defined(__unix__) || defined(__APPLE__) || defined(__posix) || \
    defined(__FreeBSD__) || defined(__linux__) || defined(__linux)
#define TLSF_THREAD_POSIX
#endif

#if defined(_TLSF_USE_C11_THREADS)
#include <threads.h>
#elif defined(TLSF_THREAD_POSIX)
#include <pthread.h>
#elif defined(TLSF_THREAD_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0600)
#define TLSF_THREAD_WIN_SRWLOCK
#elif defined(__clang__) && (__clang_major__ >= 10)
#define TLSF_THREAD_WIN_SRWLOCK
#elif defined(_MSC_VER) && (_MSC_VER >= 1700)
#define TLSF_THREAD_WIN_SRWLOCK
#elif defined(__GNUC__) && (__GNUC__ >= 8) && \
    (defined(__MINGW32__) || defined(__MINGW64__))
#define TLSF_THREAD_WIN_SRWLOCK
#else
#define TLSF_THREAD_WIN_CRSECTION
#endif
#endif

#if defined(_TLSF_USE_C11_THREADS)
#define TLSF_LOCK_T mtx_t
/* C11 does not require thrd_success to be 0, so normalize it. */
#define TLSF_LOCK_INIT(l) (mtx_init((l), mtx_plain) == thrd_success ? 0 : -1)
#define TLSF_LOCK_DESTROY(l) mtx_destroy((l))
#define TLSF_LOCK_ACQUIRE(l) mtx_lock((l))
#define TLSF_LOCK_RELEASE(l) mtx_unlock((l))
#define TLSF_LOCK_TRY(l) (mtx_trylock((l)) == thrd_success)
#elif defined(TLSF_THREAD_WIN_SRWLOCK)
#define TLSF_LOCK_T SRWLOCK
#define TLSF_LOCK_INIT(l) (InitializeSRWLock((l)), 0)
/* SRWLOCK is just a pointer - no need to destroy it */
#define TLSF_LOCK_DESTROY(l)
#define TLSF_LOCK_ACQUIRE(l) AcquireSRWLockExclusive((l))
#define TLSF_LOCK_RELEASE(l) ReleaseSRWLockExclusive((l))
#define TLSF_LOCK_TRY(l) (TryAcquireSRWLockExclusive((l)) != 0)
#elif defined(TLSF_THREAD_WIN_CRSECTION)
#define TLSF_LOCK_T CRITICAL_SECTION
#define TLSF_LOCK_INIT(l) (InitializeCriticalSection((l)), 0)
#define TLSF_LOCK_DESTROY(l) DeleteCriticalSection((l))
#define TLSF_LOCK_ACQUIRE(l) EnterCriticalSection((l))
#define TLSF_LOCK_RELEASE(l) LeaveCriticalSection((l))
#define TLSF_LOCK_TRY(l) (TryEnterCriticalSection((l)) != 0)
#elif defined(TLSF_THREAD_POSIX)
#define TLSF_LOCK_T pthread_mutex_t
#define TLSF_LOCK_INIT(l) pthread_mutex_init((l), NULL)
#define TLSF_LOCK_DESTROY(l) pthread_mutex_destroy((l))
#define TLSF_LOCK_ACQUIRE(l) pthread_mutex_lock((l))
#define TLSF_LOCK_RELEASE(l) pthread_mutex_unlock((l))
#define TLSF_LOCK_TRY(l) (pthread_mutex_trylock((l)) == 0)
#endif

/* Fold upper bits into lower 32 to retain entropy on 64-bit systems. */
#ifndef TLSF_THREAD_HINT
#if defined(_TLSF_USE_C11_THREADS)
#if defined(TLSF_THREAD_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define TLSF_THREAD_HINT() ((unsigned) GetCurrentThreadId())
#elif defined(TLSF_THREAD_POSIX)
#include <pthread.h>
#define TLSF_THREAD_HINT()                    \
    ((unsigned) ((uintptr_t) pthread_self() ^ \
                 ((uintptr_t) pthread_self() >> 16)))
#else
#define TLSF_THREAD_HINT()                    \
    ((unsigned) ((uintptr_t) thrd_current() ^ \
                 ((uintptr_t) thrd_current() >> 16)))
#endif
#elif defined(TLSF_THREAD_POSIX)
#define TLSF_THREAD_HINT()                    \
    ((unsigned) ((uintptr_t) pthread_self() ^ \
                 ((uintptr_t) pthread_self() >> 16)))
#elif defined(TLSF_THREAD_WIN)
#define TLSF_THREAD_HINT() ((unsigned) GetCurrentThreadId())
#else
#define TLSF_THREAD_HINT() 0U
#endif
#endif

#endif /* TLSF_LOCK_T */

/* Fallback thread hint for custom locks without a custom hint. */
#ifndef TLSF_THREAD_HINT
#define TLSF_THREAD_HINT() 0U
#endif

#if defined(_MSC_VER)
#define TLSF_MSVC_ALIGN(x) __declspec(align(x))
#define TLSF_GCC_ALIGN(x)
#elif defined(__GNUC__) || defined(__clang__)
#define TLSF_MSVC_ALIGN(x)
#define TLSF_GCC_ALIGN(x) __attribute__((aligned(x)))
#else
#define TLSF_MSVC_ALIGN(x)
#define TLSF_GCC_ALIGN(x)
#endif

/* Number of independent arenas. Each arena has its own lock and TLSF pool, so N
 * arenas support up to N contention-free concurrent allocations.
 *
 * Trade-offs:
 *   More arenas  -> lower contention, but memory is partitioned (one
 *                   arena can exhaust while others have space).
 *   Fewer arenas -> better memory utilization, higher contention.
 *
 * Must be >= 1. Any value works: arena selection takes a modulo of the live
 * arena count, so a power of two buys nothing here.
 */
#ifndef TLSF_ARENA_COUNT
#define TLSF_ARENA_COUNT 4
#endif

TLSF_STATIC_ASSERT(TLSF_ARENA_COUNT >= 1, "TLSF_ARENA_COUNT must be >= 1");

/* Align each arena to a cache line to prevent false sharing between arenas that
 * would otherwise sit on the same line. 64 bytes is the common L1 cache line
 * size on x86-64 and ARMv8.
 */
#ifndef TLSF_CACHELINE_SIZE
#define TLSF_CACHELINE_SIZE 64
#endif

TLSF_STATIC_ASSERT((TLSF_CACHELINE_SIZE & (TLSF_CACHELINE_SIZE - 1)) == 0,
                   "TLSF_CACHELINE_SIZE must be a power of two");

/* Which lock backend was selected, not which macro the caller passed. Every arm
 * of the selection above lands on a different lock type, and two units that
 * disagree about which one then disagree about 'base' and 'capacity', which sit
 * behind the lock inside 'tlsf_arena_t'. Do not look for the damage in the
 * arena stride. The cache-line padding absorbs a lock-size change until it
 * crosses a whole line, so 'SRWLOCK' at one pointer and 'CRITICAL_SECTION' at
 * 40 bytes give a byte-identical 'sizeof(tlsf_thread_t)'. Equal sizes are the
 * ordinary case, not the exception, which is why the choice is encoded whole
 * rather than measured or reduced to a C11-or-not flag.
 *
 * A boolean would miss the pair that needs it most. Nothing in the build system
 * picks between 'SRWLOCK' and 'CRITICAL_SECTION': the header decides from
 * '_WIN32_WINNT' and the compiler version, which a caller can set per
 * translation unit, and a mismatched pair would then link cleanly, write the
 * two fields at different offsets, and pass storage prepared by
 * 'InitializeSRWLock' to 'EnterCriticalSection'.
 *
 * Keying off TLSF_C11_THREADS instead would both miss a real difference and
 * invent one where the flag changes nothing, since the same flag reaches
 * translation units whose toolchain has no '<threads.h>' to select. A
 * caller-supplied TLSF_LOCK_T skips the whole selection, leaving this zero,
 * which is why that case stays a caller obligation.
 */
#if defined(_TLSF_USE_C11_THREADS)
#define _TLSF_THREAD_BACKEND 1
#elif defined(TLSF_THREAD_WIN_SRWLOCK)
#define _TLSF_THREAD_BACKEND 2
#elif defined(TLSF_THREAD_WIN_CRSECTION)
#define _TLSF_THREAD_BACKEND 3
#elif defined(TLSF_THREAD_POSIX)
#define _TLSF_THREAD_BACKEND 4
#else
#define _TLSF_THREAD_BACKEND 0
#endif

/* Same hazard as the core allocator, and the same remedy: 'tlsf_thread_t' is
 * caller-allocated and its layout moves with TLSF_ARENA_COUNT,
 * TLSF_CACHELINE_SIZE and the embedded 'tlsf_t', so a translation unit that
 * disagrees about any of them corrupts the caller's object on the first call.
 * The core ABI suffix plus the thread-specific knobs, including the selected
 * lock backend, turn that into a link error. See the matching block in tlsf.h.
 *
 * A custom TLSF_LOCK_T also moves the layout and cannot be encoded in a token,
 * so it stays a caller obligation: define it in one place every translation
 * unit sees, the way the lock macros already have to be.
 *
 * One flat '##' chain fed by a single expansion layer, for the reason tlsf.h
 * gives. The core half of the suffix is spelled again here rather than wrapped
 * around _TLSF_ABI, because wrapping is the nesting that broke. Keep the two
 * spellings in step; check-abi-guard.sh fails the thread target on a core knob
 * if they drift. Parameters are not named for the separator letters they sit
 * beside, or 'a##a' would paste the arena count to itself and drop the
 * separator.
 *
 * Guarded on __FRAMAC__ like tlsf.h: relaxing one alone would suffix the
 * wrapper's names while leaving the core's plain, or the reverse.
 */
#ifndef __FRAMAC__
#define _TLSF_THREAD_ABI_PASTE(name, wid, flm, arena, cache, bk) \
    name##_w##wid##_fl##flm##a##arena##c##cache##t##bk
#define _TLSF_THREAD_ABI_EVAL(name, wid, flm, arena, cache, bk) \
    _TLSF_THREAD_ABI_PASTE(name, wid, flm, arena, cache, bk)
#define _TLSF_THREAD_ABI(name)                                   \
    _TLSF_THREAD_ABI_EVAL(name, _TLSF_SIZE_WIDTH, _TLSF_FL_MAX,  \
                          TLSF_ARENA_COUNT, TLSF_CACHELINE_SIZE, \
                          _TLSF_THREAD_BACKEND)

#define tlsf_thread_init _TLSF_THREAD_ABI(tlsf_thread_init)
#define tlsf_thread_destroy _TLSF_THREAD_ABI(tlsf_thread_destroy)
#define tlsf_thread_malloc _TLSF_THREAD_ABI(tlsf_thread_malloc)
#define tlsf_thread_aalloc _TLSF_THREAD_ABI(tlsf_thread_aalloc)
#define tlsf_thread_realloc _TLSF_THREAD_ABI(tlsf_thread_realloc)
#define tlsf_thread_free _TLSF_THREAD_ABI(tlsf_thread_free)
#define tlsf_thread_check _TLSF_THREAD_ABI(tlsf_thread_check)
#define tlsf_thread_stats _TLSF_THREAD_ABI(tlsf_thread_stats)
#define tlsf_thread_reset _TLSF_THREAD_ABI(tlsf_thread_reset)
#endif

/* Everything above is includes, macros and static assertions, none of which
 * needs C linkage, and one of the includes is '<threads.h>' or '<windows.h>'.
 * Opening the block here rather than at the top of the file keeps those system
 * headers out of it. A C++ unit only started reaching '<threads.h>' when the
 * selection above grew its C++ arm, so this is the one placement that does not
 * rest on a system header tolerating a language linkage it never asked for.
 */
#ifdef __cplusplus
extern "C" {
#endif

TLSF_MSVC_ALIGN(TLSF_CACHELINE_SIZE) typedef struct {
    tlsf_t pool;
    TLSF_LOCK_T lock;

    /* Raw chunk handed to this arena. Ownership lookup tests against this
     * range, not against pool.arena, so it deliberately covers the leading and
     * trailing bytes that tlsf_pool_init() skipped for alignment.
     */
    void *base;
    size_t capacity;
} TLSF_GCC_ALIGN(TLSF_CACHELINE_SIZE) tlsf_arena_t;

typedef struct {
    tlsf_arena_t arenas[TLSF_ARENA_COUNT];

    /* Live arena count, at most TLSF_ARENA_COUNT. Zero before a successful
     * init, after a failed init, and after tlsf_thread_destroy().
     */
    int count;
} tlsf_thread_t;

/**
 * Initialize from a contiguous memory region, splitting it into up to
 * TLSF_ARENA_COUNT independent sub-pools, one lock each.
 *
 * The count is halved while each arena's share would fall below 256 bytes, so
 * the result is TLSF_ARENA_COUNT divided by a power of two, at least 1. That is
 * a heuristic, not a guarantee: a chunk can still be too small for a pool, and
 * then the whole call fails.
 *
 * @ts must be uninitialized or already passed to tlsf_thread_destroy(). This
 * call memsets @ts, so invoking it over live locks leaks them and is undefined.
 *
 * @ts : Thread-safe allocator instance
 * @mem : Memory region, which the caller continues to own
 * @bytes : Size of the memory region
 *
 * Return Total usable bytes across all arenas, or 0 if @ts or @mem is NULL,
 * @bytes is 0, a lock cannot be created, or an arena pool cannot be built
 */
size_t tlsf_thread_init(tlsf_thread_t *ts, void *mem, size_t bytes);

/**
 * Release the lock resources. The memory region given to tlsf_thread_init() is
 * not freed; the caller still owns it.
 *
 * A NULL @ts is a no-op. Requires a quiescent instance. Afterwards the live
 * arena count is zero, so every allocation fails until @ts is initialized
 * again, and a second destroy does nothing.
 */
void tlsf_thread_destroy(tlsf_thread_t *ts);

/**
 * Thread-safe malloc. Tries the calling thread's preferred arena first, then
 * the others by non-blocking try-lock, then by blocking acquire. Because memory
 * is partitioned, this returns NULL only when no arena can satisfy @size, even
 * if the free bytes summed across arenas would have been enough.
 *
 * A NULL @ts returns NULL.
 */
void *tlsf_thread_malloc(tlsf_thread_t *ts, size_t size);

/**
 * Thread-safe aligned allocation. @align must be a non-zero power of two;
 * anything else returns NULL, as does a NULL @ts. Arena search matches
 * tlsf_thread_malloc().
 */
void *tlsf_thread_aalloc(tlsf_thread_t *ts, size_t align, size_t size);

/**
 * Thread-safe realloc. Tries to resize inside the owning arena, then falls back
 * to allocating elsewhere, copying, and freeing the original.
 *
 * A NULL @ptr allocates, and a zero @size frees @ptr and returns NULL. A @ptr
 * outside every arena range returns NULL and frees nothing. As with
 * tlsf_realloc(), a failure leaves @ptr allocated and intact, and the returned
 * pointer need not equal @ptr.
 */
void *tlsf_thread_realloc(tlsf_thread_t *ts, void *ptr, size_t size);

/**
 * Thread-safe free. Locates the owning arena by scanning the arena address
 * ranges, which is linear in the live arena count and so effectively constant
 * for the small counts this is meant for.
 *
 * A NULL @ts or @ptr is a no-op, and a @ptr outside every arena range is
 * ignored. A @ptr that falls inside a range but is not a live allocation is
 * undefined exactly as in tlsf_free(): the range check establishes ownership,
 * not validity.
 */
void tlsf_thread_free(tlsf_thread_t *ts, void *ptr);

/**
 * Heap consistency check across all arenas. Acquires each arena lock in order
 * during the check.
 */
void tlsf_thread_check(tlsf_thread_t *ts);

/**
 * Aggregate statistics across all arenas. largest_free reports the single
 * largest free block in any one arena, which is the largest allocation that can
 * still succeed, not the sum of free space.
 *
 * Not an atomic snapshot: arena locks are taken one at a time, so concurrent
 * allocations in an already-visited arena are not reflected. Quiesce the other
 * threads first if you need an exact figure.
 *
 * Return 0 on success, -1 if @ts or @stats is NULL or if any arena fails
 */
int tlsf_thread_stats(tlsf_thread_t *ts, tlsf_stats_t *stats);

/**
 * Reset every arena to its initial state in bounded time, discarding all
 * allocations at once.
 *
 * A NULL @ts is a no-op. Each arena lock is taken in turn, but that does not
 * make this safe against concurrent use: every pointer handed out so far
 * becomes invalid, so the other threads must already be quiescent.
 */
void tlsf_thread_reset(tlsf_thread_t *ts);

#ifdef __cplusplus
}
#endif
