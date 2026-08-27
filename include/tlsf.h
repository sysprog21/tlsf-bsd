/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Inhibit C++ name-mangling for tlsf functions. Opened after the includes, for
 * the reason tlsf_thread.h gives at its own block: a system header compiled
 * inside a language linkage it never declared is not the library's to promise.
 */
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* IMPORTANT: the configuration macros below (TLSF_MAX_POOL_BITS in particular)
 * change the layout and size of tlsf_t, which callers allocate themselves.
 * Every translation unit in a build (src/tlsf.c and every consumer) must see
 * identical definitions. A mismatch is not diagnosed: the struct silently
 * differs in size (8376 vs 3440 bytes for the default vs TLSF_MAX_POOL_BITS=20
 * on 64-bit) and accesses land at the wrong offsets. Define them in the build
 * system, never in a single .c file.
 *
 * TLSF_ENABLE_CHECK is a special case, and it is only half-safe. tlsf_check()
 * is an extern function when it is defined and a static inline no-op when it is
 * not, so a mismatch behaves differently depending on which side has it:
 *
 *   consumer has it, tlsf.c does not -> undefined reference at link time
 *   tlsf.c has it, consumer does not -> links fine, and every tlsf_check()
 *                                       call in the consumer silently becomes
 *                                       a no-op
 *
 * The second direction produces no diagnostic at all: the heap checking a
 * caller believes is enabled simply is not running. Set it uniformly.
 */

/* Second-level subdivisions: 32 bins per first-level class. Max internal
 * fragmentation bounded by 1/SL_COUNT = 3.125% (was 6.25% with 16 bins).
 * Control structure size increases ~2x for the block pointer array.
 */
#define _TLSF_SL_COUNT 32

#ifndef _TLSF_SIZE_WIDTH
#if defined(_MSC_VER)
#if defined(_WIN64)
#define _TLSF_SIZE_WIDTH 64
#else
#define _TLSF_SIZE_WIDTH 32
#endif
#elif defined(__GNUC__) || defined(__MINGW32__) || defined(__MINGW64__) || \
    defined(__clang__)
#define _TLSF_SIZE_WIDTH __SIZE_WIDTH__
#else
#if INTPTR_MAX == INT64_MAX
#define _TLSF_SIZE_WIDTH 64
#elif INTPTR_MAX == INT32_MAX
#define _TLSF_SIZE_WIDTH 32
#else
#error No support for non 32 or 64 bit systems
#endif
#endif
#endif

/* Configurable maximum pool size: define TLSF_MAX_POOL_BITS to clamp the
 * first-level index, which shrinks the tlsf_t control structure. On 64-bit,
 * -DTLSF_MAX_POOL_BITS=20 takes tlsf_t from 8376 down to 3440 bytes.
 *
 * The clamp bounds a dynamic arena at 2^TLSF_MAX_POOL_BITS bytes. It does not
 * bound a fixed pool at the same figure: tlsf_pool_init() accepts only about
 * half that, because the pool's single initial free block is itself capped at
 * 2^(_TLSF_FL_MAX - 1). For an ALIGN_SIZE-aligned pointer at
 * TLSF_MAX_POOL_BITS=20 the largest accepted region is 524304 bytes, not 1 MB.
 * An unaligned pointer may carry up to ALIGN_SIZE-1 further bytes that are
 * skipped for alignment. TLSF_MAX_POOL_BYTES below states the limit.
 *
 * Give it a bare decimal literal. The ABI guard below pastes the value into the
 * public symbol names, so a parenthesised or computed form such as (20) or (10
 * + 10) does not form a valid identifier and fails to compile. The same applies
 * to TLSF_ARENA_COUNT and TLSF_CACHELINE_SIZE in tlsf_thread.h.
 */
#ifdef TLSF_MAX_POOL_BITS
#define _TLSF_FL_MAX TLSF_MAX_POOL_BITS
#else
#if _TLSF_SIZE_WIDTH == 64
#define _TLSF_FL_MAX 39
#else
#define _TLSF_FL_MAX 31
#endif
#endif

/* FL_SHIFT = log2(SL_COUNT) + log2(ALIGN_SIZE) */
#if _TLSF_SIZE_WIDTH == 64
#define _TLSF_FL_SHIFT 8
#else
#define _TLSF_FL_SHIFT 7
#endif
#define _TLSF_FL_COUNT (_TLSF_FL_MAX - _TLSF_FL_SHIFT + 1)
#define TLSF_MAX_SIZE (((size_t) 1 << (_TLSF_FL_MAX - 1)) - sizeof(size_t))

/* '_TLSF_SIZE_WIDTH' and every configuration macro that feeds '_TLSF_FL_MAX'
 * change the layout of 'tlsf_t', which callers allocate themselves. A
 * translation unit that disagrees with the library allocates the wrong size, so
 * the first call writes past the end of the caller's object. Nothing diagnoses
 * it: the link succeeds and the corruption surfaces later, somewhere else.
 *
 * Fold these values into the public symbol names so a disagreement becomes an
 * undefined reference that names the value each side used. Source is unchanged,
 * callers still write tlsf_malloc(); only the emitted symbol carries the
 * suffix, which is what makes the mismatch visible to the linker.
 *
 * The cost is that the knobs must be bare decimal literals, since a token paste
 * is what carries them into the name. That is narrower than the arithmetic the
 * preprocessor would otherwise accept, and it is the one place this guard asks
 * something of callers.
 *
 * The paste is one flat '##' chain: _TLSF_ABI_EVAL expands the value macros,
 * _TLSF_ABI_PASTE glues them, and an argument next to '##' is not expanded, so
 * the levels cannot collapse. Never nest one paste inside another. MSVC's
 * traditional and '/Zc:preprocessor' rescans spell that differently, and
 * '/std:c11' turns the latter on while the C++ modes leave it opt-in, so a
 * mixed C and C++ build gets one name in two spellings and a link that fails on
 * a configuration that matches.
 *
 * Frama-C analyses one translation unit at a time, so it has no mismatch to
 * catch, and the suffixed names would invalidate the '-wp-fct' list in the
 * Makefile, which still names 'tlsf_pool_reset'. WP skips a name it cannot
 * resolve and the proved-goals count still balances, so that failure is silent.
 * Leave its view of the names alone.
 */
#ifndef __FRAMAC__
#define _TLSF_ABI_PASTE(name, wid, flm) name##_w##wid##_fl##flm
#define _TLSF_ABI_EVAL(name, wid, flm) _TLSF_ABI_PASTE(name, wid, flm)
#define _TLSF_ABI(name) _TLSF_ABI_EVAL(name, _TLSF_SIZE_WIDTH, _TLSF_FL_MAX)

#define tlsf_resize _TLSF_ABI(tlsf_resize)
#define tlsf_aalloc _TLSF_ABI(tlsf_aalloc)
#define tlsf_append_pool _TLSF_ABI(tlsf_append_pool)
#define tlsf_pool_init _TLSF_ABI(tlsf_pool_init)
#define tlsf_pool_reset _TLSF_ABI(tlsf_pool_reset)
#define tlsf_malloc _TLSF_ABI(tlsf_malloc)
#define tlsf_realloc _TLSF_ABI(tlsf_realloc)
#define tlsf_free _TLSF_ABI(tlsf_free)
#define tlsf_usable_size _TLSF_ABI(tlsf_usable_size)
#define tlsf_check _TLSF_ABI(tlsf_check)
#define tlsf_get_stats _TLSF_ABI(tlsf_get_stats)
#endif

/* Largest region tlsf_pool_init() accepts, for an ALIGN_SIZE-aligned pointer.
 * The pool's single initial free block cannot exceed 2^(_TLSF_FL_MAX - 1)
 * bytes, and the pool also carries that block's header and a sentinel.
 *
 * A region is accepted when its size, rounded down to the alignment, is at most
 * this; a few trailing bytes past it are tolerated and ignored. Note this is
 * about half of 2^_TLSF_FL_MAX, which bounds a *dynamic* arena instead.
 */
#define TLSF_MAX_POOL_BYTES \
    (((size_t) 1 << (_TLSF_FL_MAX - 1)) + 2 * sizeof(size_t))

/* An allocator with no arena yet. The first tlsf_malloc() grows one through
 * tlsf_resize(), so an instance initialized this way needs that callback.
 */
#if defined(__cplusplus)
/* A compound literal is not C++ at all. Value initialization zeroes every
 * member instead, and because tlsf_t is an aggregate with no user-provided
 * constructor it is a constant expression, so this also serves objects with
 * static storage duration.
 */
#define TLSF_INIT tlsf_t()
#else
#define TLSF_INIT ((tlsf_t) {.size = 0})
#endif

/* The same value for objects with static storage duration. Only MSVC's C mode
 * needs a different spelling, because it rejects a compound literal as a static
 * initializer; everywhere else, including C++, the two macros cannot drift.
 */
#if defined(_MSC_VER) && !defined(__cplusplus)
#define TLSF_INIT_STATIC {.size = 0}
#else
#define TLSF_INIT_STATIC TLSF_INIT
#endif

#ifndef TLSF_STATIC_ASSERT
#if defined(__cplusplus) && (__cplusplus >= 201103L)
#define TLSF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define TLSF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#elif !defined(__cplusplus) && defined(__clang__)
#if __has_extension(c_static_assert) || __has_feature(c_static_assert)
#define TLSF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#elif !defined(__cplusplus) && defined(__GNUC__) &&               \
    ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)) && \
    !defined(__STRICT_ANSI__)
#define TLSF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif

/* Everything the ladder above did not claim lands here: pre-C11 compilers,
 * pre-C++11 compilers, and a clang whose nested __has_extension test came out
 * false. That last case is why the fallback cannot be an '#else' arm of the
 * ladder; an '#elif' that a nested '#if' declines to fill would otherwise leave
 * the macro undefined.
 */
#ifndef TLSF_STATIC_ASSERT
#define TLSF_SASSERT_GLUE(a, b) a##b
#define TLSF_SASSERT_JOIN(a, b) TLSF_SASSERT_GLUE(a, b)
#define TLSF_STATIC_ASSERT(cond, msg)                             \
    typedef char TLSF_SASSERT_JOIN(STATIC_ASSERT_FAILED_AT_LINE_, \
                                   __LINE__)[(cond) ? 1 : -1]
#endif

/* Block header structure.
 *
 * @prev : Pointer to the previous physical block. Only valid when the previous
 *         block is free; physically stored at the tail of that block's payload.
 * @header : Size (upper bits) | status bits (lower 2 bits).
 * @next_free : Next block in the same free list (only valid when free).
 * @prev_free : Previous block in the same free list (only valid when free).
 */
struct tlsf_block {
    struct tlsf_block *prev;
    size_t header;
    struct tlsf_block *next_free, *prev_free;
};

typedef struct {
    /* First-level bitmap, plus one second-level bitmap per first-level class. A
     * set bit means that bin holds at least one free block, which is what makes
     * the search for a fit O(1).
     */
    uint32_t fl, sl[_TLSF_FL_COUNT];

    /* Fixed pool: memory is caller-owned (tlsf_pool_init) and the allocator
     * never calls tlsf_resize(), so the arena never auto-grows and never
     * shrinks. It can still be extended deliberately, by tlsf_append_pool()
     * with adjacent memory. Orthogonal to @arena, which is always the current
     * base address once the pool has any memory.
     */
    bool fixed;

    /* Pool base address. Set by tlsf_pool_init() for a fixed pool; NULL for a
     * dynamic pool until its first growth.
     */
    void *arena;
    size_t size; /* Arena bytes owned, sentinels included; 0 when there is no
                  * pool yet
                  */
    struct tlsf_block *block[_TLSF_FL_COUNT][_TLSF_SL_COUNT];
    struct tlsf_block block_null; /* Free-list sentinel (absorbs writes) */
} tlsf_t;

/* The header word immediately precedes a payload pointer. Its validity is the
 * memory-safety condition shared by free and realloc; allocation ownership is a
 * semantic caller obligation, documented on those functions.
 */
/*@
  predicate tlsf_payload_header{L}(void *ptr) =
    \valid((char *)ptr) &&
    \valid(((char *)ptr - sizeof(size_t)) + (0 .. sizeof(size_t) - 1));
*/

/**
 * Callback to grow or query the memory arena (dynamic pools only). Users of
 * tlsf_pool_init() need not provide this function. A weak default returning
 * NULL is provided in tlsf.c; dynamic pool users MUST override it, otherwise
 * allocations will silently fail.
 *
 * The first successful resize establishes the arena base. While the arena is
 * live, every later successful nonzero resize must return that same base and
 * preserve the surviving prefix at the same offsets. Returning NULL must leave
 * the old mapping intact. A resize to zero releases the arena and ends its
 * lifetime; the next nonzero resize starts over and may establish a different
 * base. The callback must not mutate @t; allocator state is owned by the
 * allocator.
 */
/*@
  requires \valid(t);
  requires size <= ((size_t)1 << _TLSF_FL_MAX);
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_resize(tlsf_t *t, size_t size);

/**
 * Allocate memory with a specified alignment.
 *
 * @t : The TLSF allocator instance
 * @align : Alignment in bytes; must be a non-zero power of two
 * @size : Requested allocation size in bytes; need not be a multiple of @align
 *         (follows POSIX posix_memalign semantics; C11 aligned_alloc required
 *         size % align == 0 but C23 and common implementations dropped that
 *         constraint)
 *
 * Return Pointer to at least @size bytes aligned to @align, or NULL on failure.
 * A zero @size request returns a unique minimum-sized allocation (consistent
 * with tlsf_malloc).
 */
/*@
  requires \valid(t);
  ensures (align == 0 || (align & (align - 1)) != 0) ==> \result == \null;
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_aalloc(tlsf_t *t, size_t align, size_t size);

/**
 * Extend an existing pool with more memory, coalescing with the pool's last
 * block when that block is free. Works for fixed and dynamic pools alike.
 *
 * @mem, rounded up to ALIGN_SIZE, must equal the current pool end. A region
 * that does not abut the pool is rejected, and that is the usual reason for a 0
 * return. Bytes lost to that rounding and to the new sentinel are not handed
 * back, so the return value can be less than @size.
 *
 * @t : The TLSF allocator instance
 * @mem : Pointer to the memory block to append
 * @size : Size of the memory block in bytes
 *
 * Return Number of bytes used from the memory block, 0 on failure
 */
/*@
  requires \valid(t);
  requires mem != \null && size != 0 ==>
    \valid(((char *)mem) + (0 .. size - 1));
  ensures \result <= size;
*/
size_t tlsf_append_pool(tlsf_t *t, void *mem, size_t size);

/**
 * Initialize the allocator with a fixed-size memory pool. The pool will not
 * auto-grow via tlsf_resize(); when the pool is exhausted, allocations return
 * NULL. Callers may still extend the pool explicitly via tlsf_append_pool()
 * with adjacent memory. This avoids the need to implement tlsf_resize().
 *
 * Multiple independent instances are supported by initializing separate tlsf_t
 * structures with their own memory regions.
 *
 * On success @t is zero-initialized before the pool is laid out. On failure @t
 * is left exactly as it was, so a rejected re-init cannot destroy a live arena;
 * a caller that ignores the return value therefore inherits whatever @t already
 * held, including indeterminate bytes for a fresh automatic object.
 *
 * @t : The TLSF allocator instance
 * @mem : Pointer to the memory region to use as the pool
 * @bytes : Total size of the memory region in bytes
 *
 * Return Usable bytes in the pool, or 0 on failure
 */
/*@
  requires \valid(t);
  requires mem != \null && bytes != 0 ==>
    \valid(((char *)mem) + (0 .. bytes - 1));
  ensures \result == 0 || t->fixed;
*/
size_t tlsf_pool_init(tlsf_t *t, void *mem, size_t bytes);

/**
 * Reset a static pool to its initial state, discarding all allocations.
 * Bounded-time bulk deallocation: clears bitmaps, recreates a single free
 * block. Cost is O(FL_COUNT * SL_COUNT) for the bin reset, which is fixed at
 * compile time.
 *
 * Only valid for pools created with tlsf_pool_init(). Does nothing for dynamic
 * pools or uninitialized instances.
 *
 * WARNING: All pointers previously returned by tlsf_malloc/aalloc/realloc
 * become invalid after reset. Passing stale pointers to tlsf_free or
 * tlsf_realloc causes undefined behavior (silent metadata corruption in release
 * builds, assertion failure in debug builds).
 *
 * @t : The TLSF allocator instance
 */
/*@ requires \valid(t); */
void tlsf_pool_reset(tlsf_t *t);

/**
 * Allocate memory from the pool.
 *
 * @t : The TLSF allocator instance
 * @size : Requested allocation size in bytes. A zero @size request returns a
 *         unique minimum-sized allocation (POSIX-compatible behavior), not
 *         NULL.
 *
 * Return Pointer to at least @size bytes, aligned to ALIGN_SIZE (8 on 64-bit, 4
 * on 32-bit), or NULL on failure.
 */
/*@
  requires \valid(t);
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_malloc(tlsf_t *t, size_t size);

/**
 * Resize an existing allocation, preserving its contents up to the smaller of
 * the old and new sizes.
 *
 * Two calls are aliases for other entry points: a NULL @mem allocates, and a
 * zero @size frees @mem and returns NULL. Note the latter differs from C's
 * realloc, where the same call is implementation-defined (C17) or undefined
 * (C23).
 *
 * The block may be grown in place, slid backward onto a free predecessor, or
 * relocated, so the returned pointer need not equal @mem. On failure NULL is
 * returned and @mem is left allocated and intact.
 *
 * @t : The TLSF allocator instance
 * @mem : Pointer from tlsf_malloc/aalloc/realloc, or NULL
 * @size : Requested new size in bytes
 *
 * Return Pointer to the resized allocation, or NULL on failure or zero @size
 */
/*@
  requires \valid(t);
  requires mem != \null ==> tlsf_payload_header(mem);
  ensures \result == \null || \valid(((char *)\result) + (0 .. size - 1));
*/
void *tlsf_realloc(tlsf_t *t, void *mem, size_t size);

/**
 * Release an allocation and coalesce it with any free physical neighbors.
 *
 * A NULL @mem is a no-op. Passing a pointer that was already freed, or one that
 * did not come from this allocator, is undefined: it corrupts metadata silently
 * in release builds and trips an assertion in debug builds.
 *
 * @t : The TLSF allocator instance
 * @mem : Pointer from tlsf_malloc/aalloc/realloc, or NULL
 */
/*@
  requires \valid(t);
  requires mem != \null ==> tlsf_payload_header(mem);
*/
void tlsf_free(tlsf_t *t, void *mem);

/**
 * Return the usable size of an existing allocation. The usable size may exceed
 * the originally requested size due to alignment rounding and bin-class
 * quantization. The analogue of glibc's malloc_usable_size(); POSIX specifies
 * no such function.
 *
 * @ptr : Pointer previously returned by tlsf_malloc/aalloc/realloc. Behavior is
 *        undefined if ptr has been freed.
 *
 * Return Usable payload bytes, or 0 if ptr is NULL
 */
/*@
  requires ptr != \null ==> tlsf_payload_header(ptr);
  ensures ptr == \null ==> \result == 0;
*/
size_t tlsf_usable_size(void *ptr);

/**
 * Walk the whole arena and verify its invariants: block linkage, size and
 * alignment, sentinels, and agreement between the bitmaps and the free lists.
 *
 * A violation it reaches prints to stderr and calls abort(); corruption severe
 * enough to derail the walk itself may fault first. It is a debugging aid, not
 * an error-reporting API, and it is linear in the number of blocks. Without
 * TLSF_ENABLE_CHECK it compiles to nothing; see the note at the top of this
 * header about setting that macro uniformly.
 *
 * @t : The TLSF allocator instance
 */
#ifdef TLSF_ENABLE_CHECK
void tlsf_check(tlsf_t *t);
#else
static inline void tlsf_check(tlsf_t *t)
{
    (void) t;
}
#endif

/**
 * Heap statistics structure for monitoring allocator state.
 */
typedef struct {
    size_t total_free;   /* Total free bytes available */
    size_t largest_free; /* Largest contiguous free block */
    size_t total_used;   /* Total bytes in allocated blocks */
    size_t block_count;  /* Total number of blocks (free + used) */
    size_t free_count;   /* Number of free blocks (fragmentation indicator) */
    size_t overhead;     /* Metadata overhead bytes */
} tlsf_stats_t;

/**
 * Collect heap statistics by walking every block, so cost is linear in the
 * block count rather than constant.
 *
 * An allocator with no pool yet reports all-zero statistics and succeeds.
 *
 * @t : The TLSF allocator instance
 * @stats : Output structure to fill with statistics
 *
 * Return 0 on success, -1 if @t or @stats is NULL, or if @t claims a nonzero
 * size but has no arena
 */
/*@
  behavior invalid:
    assumes t == \null || stats == \null;
    assigns \nothing;
    ensures \result == -1;
  behavior valid:
    assumes t != \null && stats != \null;
    requires \valid(t) && \valid(stats);
    ensures \result == 0 || \result == -1;
  complete behaviors;
  disjoint behaviors;
*/
int tlsf_get_stats(tlsf_t *t, tlsf_stats_t *stats);

#ifdef __cplusplus
}
#endif
