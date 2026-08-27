/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

/* Bit-scan intrinsic selection. Toolchains that are neither GCC-family nor MSVC
 * (IAR, Green Hills, TI, ARM Compiler 5) fall through to the portable
 * implementations below, which are fixed-step and therefore still O(1). Define
 * TLSF_NO_INTRINSICS to force that path; CI uses it to test it.
 */
#ifndef TLSF_NO_INTRINSICS
#if defined(__GNUC__) || defined(__MINGW32__) || defined(__MINGW64__) || \
    defined(__clang__)
#define TLSF_BUILTIN_BITSCAN 1
#elif defined(_MSC_VER)
#define TLSF_MSVC_BITSCAN 1
#endif
#endif

#include "tlsf.h"

/* Used only from ASSERT(), which compiles out when assertions are disabled. */
#ifndef MAYBE_UNUSED
#if defined(__GNUC__) || defined(__clang__)
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif
#endif

#ifndef UNLIKELY
#if defined(__GNUC__) || defined(__MINGW32__) || defined(__MINGW64__) || \
    defined(__clang__)
#define UNLIKELY(x) __builtin_expect(!!(x), false)
#else
#define UNLIKELY(x) (!!(x))
#endif
#endif

/* All allocation sizes and addresses are aligned. */
#define ALIGN_SIZE ((size_t) 1 << ALIGN_SHIFT)
#if _TLSF_SIZE_WIDTH == 64
#define ALIGN_SHIFT 3
#else
#define ALIGN_SHIFT 2
#endif

/* First level (FL) and second level (SL) counts */
#define SL_SHIFT 5
#define SL_COUNT (1U << SL_SHIFT)
#define FL_MAX _TLSF_FL_MAX
#define FL_SHIFT (SL_SHIFT + ALIGN_SHIFT)
#define FL_COUNT (FL_MAX - FL_SHIFT + 1)

/* Block status bits are stored in the least significant bits (LSB) of the size
 * field.
 */
#define BLOCK_BIT_FREE ((size_t) 1)
#define BLOCK_BIT_PREV_FREE ((size_t) 2)
#define BLOCK_BITS (BLOCK_BIT_FREE | BLOCK_BIT_PREV_FREE)

/* A free block must be large enough to store its header minus the size of the
 * prev field.
 */
#define BLOCK_OVERHEAD (sizeof(size_t))
#define BLOCK_SIZE_MIN (sizeof(tlsf_block_t) - sizeof(tlsf_block_t *))
#define BLOCK_SIZE_MAX ((size_t) 1 << (FL_MAX - 1))
#define BLOCK_SIZE_SMALL ((size_t) 1 << FL_SHIFT)

/* Minimum remainder size for trimming. Raising this above BLOCK_SIZE_MIN avoids
 * creating tiny free blocks that waste metadata overhead relative to their
 * usable payload, trading internal fragmentation for fewer unusable fragments.
 * Default: BLOCK_SIZE_MIN (current behavior).
 */
#ifndef TLSF_SPLIT_THRESHOLD
#define TLSF_SPLIT_THRESHOLD BLOCK_SIZE_MIN
#endif

#ifndef ASSERT
#ifdef TLSF_ENABLE_ASSERT
#include <assert.h>
#define ASSERT(cond, msg) assert((cond) && msg)
#else
#define ASSERT(cond, msg)
#endif
#endif

/* ASan shadow poisoning: teach AddressSanitizer about TLSF's internal pool
 * layout so it can detect UAF and overflow within custom pools. Auto-detected;
 * zero overhead when ASan is not active.
 */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define ASAN_POISON(addr, size) __asan_poison_memory_region((addr), (size))
#define ASAN_UNPOISON(addr, size) __asan_unpoison_memory_region((addr), (size))
#else
#define ASAN_POISON(addr, size) ((void) (addr), (void) (size))
#define ASAN_UNPOISON(addr, size) ((void) (addr), (void) (size))
#endif

/* Fill-pattern poisoning: memset payload with 0xAA on alloc and 0xFF on free to
 * catch use-after-free and uninitialized reads on bare-metal targets where
 * sanitizers are unavailable. Gated by -DTLSF_ENABLE_POISON; zero overhead when
 * not defined.
 */
#ifdef TLSF_ENABLE_POISON
#define POISON_FILL(addr, val, size) memset((addr), (val), (size))
#else
#define POISON_FILL(addr, val, size) \
    ((void) (addr), (void) (val), (void) (size))
#endif

/* Metadata bytes embedded within a free block's payload:
 *   - next_free + prev_free at the start (2 pointers)
 *   - next block's prev at the end (1 pointer)
 * Fill/poison must skip these regions to avoid corrupting TLSF metadata. For
 * minimum-size blocks the safe region is empty.
 */
#define BLOCK_PAYLOAD_OVERHEAD (sizeof(struct tlsf_block *) * 3)

/* Forcing always_inline costs 168 bytes of .text over plain 'static inline' at
 * -O2 (6236 vs 6068, clang/arm64), because the compiler already inlines these
 * helpers on its own. Median malloc_worst latency is identical at 42 ticks
 * either way; the tails are scheduler noise. Override with -DINLINE="static
 * inline" if a target's icache budget says otherwise.
 */
#ifndef INLINE
#if defined(__GNUC__) || defined(__MINGW32__) || defined(__MINGW64__) || \
    defined(__clang__)
#define INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define INLINE static __forceinline
#else
#define INLINE static inline
#endif
#endif

typedef struct tlsf_block tlsf_block_t;

#define BLOCK_HEADER_OFFSET (sizeof(tlsf_block_t *))

/* The control structure's canonical empty state. This deliberately covers only
 * bin metadata; physical block-chain invariants are established by pool_build()
 * and will be layered on top of this predicate.
 */
/*@
  predicate tlsf_aligned_header{L}(tlsf_block_t *block) =
    \valid_read(block) && block->header % ALIGN_SIZE <= BLOCK_BITS;

  predicate tlsf_empty_bins{L}(tlsf_t *t) =
    t->fl == 0 &&
    (\forall integer i; 0 <= i < FL_COUNT ==> t->sl[i] == 0) &&
    (\forall integer i, j;
       0 <= i < FL_COUNT && 0 <= j < SL_COUNT ==>
         t->block[i][j] == &t->block_null);

  predicate tlsf_next_header_span{L}(tlsf_block_t *block) =
    tlsf_aligned_header(block) &&
    block->header - block->header % ALIGN_SIZE >= BLOCK_OVERHEAD &&
    \valid((char *)block +
           (0 .. BLOCK_HEADER_OFFSET +
                 block->header - block->header % ALIGN_SIZE -
                 BLOCK_OVERHEAD + sizeof(tlsf_block_t) - 1));
*/

TLSF_STATIC_ASSERT(sizeof(size_t) == 4 || sizeof(size_t) == 8,
                   "size_t must be 32 or 64 bit");
TLSF_STATIC_ASSERT(sizeof(size_t) == sizeof(void *),
                   "size_t must equal pointer size");
TLSF_STATIC_ASSERT(offsetof(tlsf_block_t, header) == BLOCK_HEADER_OFFSET,
                   "unexpected block header offset");
/* SL_COUNT is cast because it is an unsigned int and BLOCK_SIZE_SMALL is a
 * size_t: without it the division widens a 32-bit shift result to 64 bits,
 * which is what MSVC reports as C4334, and CI promotes that to an error.
 */
TLSF_STATIC_ASSERT(ALIGN_SIZE == BLOCK_SIZE_SMALL / (size_t) SL_COUNT,
                   "sizes are not properly set");
TLSF_STATIC_ASSERT(BLOCK_SIZE_MIN < BLOCK_SIZE_SMALL,
                   "min allocation size is wrong");
TLSF_STATIC_ASSERT(BLOCK_SIZE_MAX == TLSF_MAX_SIZE + BLOCK_OVERHEAD,
                   "max allocation size is wrong");
TLSF_STATIC_ASSERT(FL_COUNT <= 32, "index too large");
TLSF_STATIC_ASSERT(SL_COUNT <= 32, "index too large");
TLSF_STATIC_ASSERT(FL_COUNT == _TLSF_FL_COUNT, "invalid level configuration");
TLSF_STATIC_ASSERT(SL_COUNT == _TLSF_SL_COUNT, "invalid level configuration");
TLSF_STATIC_ASSERT(TLSF_SPLIT_THRESHOLD >= BLOCK_SIZE_MIN,
                   "split threshold must be at least minimum block size");

/* Without an upper bound, a documented-but-absurd threshold wraps
 * BLOCK_OVERHEAD + TLSF_SPLIT_THRESHOLD + size in block_can_trim() and makes it
 * accept blocks it must not.
 */
TLSF_STATIC_ASSERT(TLSF_SPLIT_THRESHOLD <= BLOCK_SIZE_MAX,
                   "split threshold must not overflow block size arithmetic");

TLSF_STATIC_ASSERT(_TLSF_FL_COUNT >= 1,
                   "TLSF_MAX_POOL_BITS too small for this architecture");
TLSF_STATIC_ASSERT(FL_MAX < _TLSF_SIZE_WIDTH,
                   "TLSF_MAX_POOL_BITS must be less than pointer width");

/* Default (weak) implementation of tlsf_resize. Users of tlsf_pool_init() need
 * not provide their own. Users of the dynamic growth API must provide a strong
 * definition; without one, dynamic allocations via TLSF_INIT will return NULL.
 *
 * Note: __attribute__((weak)) requires GCC or Clang. On compilers without weak
 * symbol support, users must always define tlsf_resize.
 */
#if defined(__GNUC__) || defined(__MINGW32__) || defined(__MINGW64__) || \
    defined(__clang__)
__attribute__((weak)) void *tlsf_resize(tlsf_t *t, size_t size)
{
    (void) t;
    (void) size;
    return NULL;
}
#elif defined(_MSC_VER)
void *tlsf_resize_default(tlsf_t *t, size_t size)
{
    (void) t;
    (void) size;
    return NULL;
}

/* The linker directive names the symbol as a string, and macros do not expand
 * inside a string literal, so it cannot spell 'tlsf_resize' directly: the ABI
 * guard in tlsf.h renames that to a configuration-suffixed symbol and the
 * fallback would bind to a name nothing defines. Stringify the expansion
 * instead, and build the pragma through __pragma so the whole directive can
 * come from a macro. The fallback target keeps its plain name because it is
 * defined here rather than declared in the public header.
 */
#define TLSF_STRINGIFY_(x) #x
#define TLSF_STRINGIFY(x) TLSF_STRINGIFY_(x)
#define TLSF_LINKER_COMMENT_(x) __pragma(comment(linker, x))
#if defined(_M_IX86)
#define TLSF_RESIZE_ALTERNATENAME \
    "/alternatename:_" TLSF_STRINGIFY(tlsf_resize) "=_tlsf_resize_default"
#else
#define TLSF_RESIZE_ALTERNATENAME \
    "/alternatename:" TLSF_STRINGIFY(tlsf_resize) "=tlsf_resize_default"
#endif
TLSF_LINKER_COMMENT_(TLSF_RESIZE_ALTERNATENAME)
#undef TLSF_RESIZE_ALTERNATENAME
#undef TLSF_LINKER_COMMENT_
#undef TLSF_STRINGIFY
#undef TLSF_STRINGIFY_
#endif

/*@
  requires x != 0;
  assigns \nothing;
  ensures \result < 32;
*/
INLINE uint32_t bitmap_ffs(uint32_t x)
{
    ASSERT(x, "no set bit found");
#if defined(TLSF_BUILTIN_BITSCAN)
    return (uint32_t) __builtin_ctz(x);
#elif defined(TLSF_MSVC_BITSCAN)
    unsigned long index;
    return _BitScanForward(&index, x) ? (uint32_t) index : 0;
#else
    /* Isolate the lowest set bit, then accumulate its index with five mask
     * tests. Each mask covers the positions whose index has that bit set.
     */
    uint32_t lsb = x & (~x + 1U);
    uint32_t n = 0;
    if (lsb & 0xFFFF0000U)
        n += 16;
    if (lsb & 0xFF00FF00U)
        n += 8;
    if (lsb & 0xF0F0F0F0U)
        n += 4;
    if (lsb & 0xCCCCCCCCU)
        n += 2;
    if (lsb & 0xAAAAAAAAU)
        n += 1;
    return n;
#endif
}

/*@
  requires x > 0;
  assigns \nothing;
  ensures \result < _TLSF_SIZE_WIDTH;
*/
INLINE uint32_t log2floor(size_t x)
{
    ASSERT(x > 0, "log2 of zero");
#if defined(TLSF_BUILTIN_BITSCAN)
#if _TLSF_SIZE_WIDTH == 64
    return (uint32_t) (63 - (uint32_t) __builtin_clzll((unsigned long long) x));
#else
    return (uint32_t) (31 - (uint32_t) __builtin_clzl((unsigned long) x));
#endif
#elif defined(TLSF_MSVC_BITSCAN)
    /* Zero-initialized: the intrinsic leaves index untouched when x is 0, and
     * ASSERT is compiled out in release builds.
     */
    unsigned long index = 0;
#if _TLSF_SIZE_WIDTH == 64
    _BitScanReverse64(&index, (unsigned __int64) x);
#else
    _BitScanReverse(&index, (unsigned long) x);
#endif
    return (uint32_t) index;
#else
    /* Fixed-step binary search over the word. */
    uint32_t n = 0;
#if _TLSF_SIZE_WIDTH == 64
    if (x >> 32) {
        x >>= 32;
        n += 32;
    }
#endif
    if (x >> 16) {
        x >>= 16;
        n += 16;
    }
    if (x >> 8) {
        x >>= 8;
        n += 8;
    }
    if (x >> 4) {
        x >>= 4;
        n += 4;
    }
    if (x >> 2) {
        x >>= 2;
        n += 2;
    }
    if (x >> 1)
        n += 1;
    return n;
#endif
}

/*@
  requires tlsf_aligned_header(block);
  assigns \nothing;
  ensures \result == block->header - block->header % ALIGN_SIZE;
  ensures \result % ALIGN_SIZE == 0;
  ensures \result <= block->header;
  ensures block->header - \result <= BLOCK_BITS;
*/
INLINE size_t block_size(const tlsf_block_t *block)
{
    return block->header - block->header % ALIGN_SIZE;
}

/*@
  requires \valid(block);
  requires tlsf_aligned_header(block);
  requires size % ALIGN_SIZE == 0;
  requires size <= SIZE_MAX - BLOCK_BITS;
  assigns block->header;
  ensures block->header == size + \old(block->header) % ALIGN_SIZE;
  ensures block->header - block->header % ALIGN_SIZE == size;
  ensures tlsf_aligned_header(block);
*/
INLINE void block_set_size(tlsf_block_t *block, size_t size)
{
    ASSERT(!(size % ALIGN_SIZE), "invalid size");
    block->header = size + block->header % ALIGN_SIZE;
}

/*@
  requires \valid(block);
  requires tlsf_aligned_header(block);
  requires size % ALIGN_SIZE == 0;
  requires size <= SIZE_MAX - block->header;
  assigns block->header;
  ensures block->header == \old(block->header) + size;
  ensures tlsf_aligned_header(block);
*/
INLINE void block_add_size(tlsf_block_t *block, size_t size)
{
    block->header += size;
}

/*@
  requires \valid(block);
  requires \valid(rest);
  requires \separated(block, rest);
  requires tlsf_aligned_header(block);
  requires size % ALIGN_SIZE == 0;
  requires rest_size % ALIGN_SIZE == 0;
  requires size <= SIZE_MAX - BLOCK_BITS;
  assigns block->header, rest->header;
  ensures rest->header == rest_size;
  ensures block->header - block->header % ALIGN_SIZE == size;
  ensures tlsf_aligned_header(block);
  ensures tlsf_aligned_header(rest);
*/
INLINE void block_split_headers(tlsf_block_t *block,
                                tlsf_block_t *rest,
                                size_t size,
                                size_t rest_size)
{
    rest->header = rest_size;
    block_set_size(block, size);
}

/*@
  requires \valid_read(block);
  assigns \nothing;
  ensures \result <==> block->header % 2 != 0;
*/
INLINE bool block_is_free(const tlsf_block_t *block)
{
    return block->header % 2 != 0;
}

/*@
  requires \valid_read(block);
  assigns \nothing;
  ensures \result <==> (block->header / BLOCK_BIT_PREV_FREE) % 2 != 0;
*/
INLINE bool block_is_prev_free(const tlsf_block_t *block)
{
    return (block->header / BLOCK_BIT_PREV_FREE) % 2 != 0;
}

/*@
  requires \valid(block);
  requires tlsf_aligned_header(block);
  assigns block->header;
  ensures tlsf_aligned_header(block);
  ensures block->header - block->header % ALIGN_SIZE ==
            \old(block->header) - \old(block->header) % ALIGN_SIZE;
  ensures free ==> (block->header / BLOCK_BIT_PREV_FREE) % 2 != 0;
  ensures !free ==> (block->header / BLOCK_BIT_PREV_FREE) % 2 == 0;
*/
INLINE void block_set_prev_free(tlsf_block_t *block, bool free)
{
    size_t flags = block->header % ALIGN_SIZE;
    block->header = block->header - flags + flags % BLOCK_BIT_PREV_FREE +
                    (free ? BLOCK_BIT_PREV_FREE : 0);
}

/*@
  requires align > 0;
  requires (align & (align - 1)) == 0;
  assigns \nothing;
*/
INLINE size_t align_up(size_t x, size_t align)
{
    ASSERT(align, "alignment must be non-zero");
    ASSERT(!(align & (align - 1)), "must align to a power of two");
    return (((x - 1) | (align - 1)) + 1);
}

/* Bytes that must be added to 'p' to reach the next 'align' boundary.
 *
 * Callers that cannot yet prove the boundary lies inside their buffer need this
 * offset on its own: forming the aligned pointer first would run past the end
 * of an undersized object before the bounds check can reject it.
 *
 * Note: uintptr_t is the canonical type for pointer-to-integer round-trips.
 * 'align' is a power of two, so subtracting from it is congruent to negating
 * modulo 'align' and the mask below discards the difference. Writing it that
 * way also keeps unary minus off an unsigned operand, which MSVC reports as
 * C4146.
 */
/*@
  requires align > 0;
  requires (align & (align - 1)) == 0;
  assigns \nothing;
*/
INLINE size_t align_offset(const char *p, size_t align)
{
    ASSERT(align, "alignment must be non-zero");
    ASSERT(!(align & (align - 1)), "must align to a power of two");
    return (size_t) (align - (uintptr_t) p) & (align - 1);
}

/* Align pointer while preserving pointer provenance.
 *
 * The naive approach '(char *) align_up((size_t) p, align)' loses provenance
 * because the integer-to-pointer cast creates a pointer with no derivation
 * history. This causes issues with Miri, UBSan, and strict aliasing analysis.
 * Adding the offset to 'p' keeps the result derived from 'p'.
 *
 * The caller must own a full alignment window at 'p'. Forming a pointer past
 * the end of the object is undefined even when it is never dereferenced, and
 * the boundary can sit up to 'align - 1' bytes ahead. A caller that cannot
 * promise that window yet wants align_offset() instead, which yields the
 * distance as a number so the span can be bounds-checked before any pointer is
 * built. That is why tlsf_pool_init() uses align_offset().
 */
/*@
  requires align > 0;
  requires (align & (align - 1)) == 0;
  requires \valid_read((char *) p + (0 .. align - 1));
  assigns \result \from p, align;
*/
INLINE char *align_ptr(char *p, size_t align)
{
    return p + align_offset(p, align);
}

/*@ assigns \result \from block; */
INLINE char *block_payload(tlsf_block_t *block)
{
    return (char *) block + BLOCK_HEADER_OFFSET + BLOCK_OVERHEAD;
}

/*@ assigns \result \from ptr; */
INLINE tlsf_block_t *to_block(void *ptr)
{
    tlsf_block_t *block = (tlsf_block_t *) ptr;
    ASSERT(block_payload(block) == align_ptr(block_payload(block), ALIGN_SIZE),
           "block not aligned properly");
    return block;
}

/*@
  requires \valid((char *)ptr - BLOCK_HEADER_OFFSET - BLOCK_OVERHEAD +
                  (0 .. sizeof(size_t) - 1));
  assigns \result \from ptr;
*/
INLINE tlsf_block_t *block_from_payload(void *ptr)
{
    return to_block((char *) ptr - offsetof(tlsf_block_t, header) -
                    BLOCK_OVERHEAD);
}

/* Poison the safe region of a free block's payload.
 *
 * The safe region excludes live TLSF metadata embedded in the payload
 * (free-list pointers at the start, next block's prev at the end). Must
 * unpoison the full payload first: after block_absorb merges two blocks, the
 * old safe region may carry stale ASan shadow.
 */
/*@
  requires tlsf_aligned_header(block);
  assigns *((char *)block + BLOCK_HEADER_OFFSET + BLOCK_OVERHEAD +
            (0 .. block->header - block->header % ALIGN_SIZE - 1));
*/
INLINE void block_poison_free(tlsf_block_t *block)
{
    size_t bsize = block_size(block);
    ASAN_UNPOISON(block_payload(block), bsize);
    if (bsize > BLOCK_PAYLOAD_OVERHEAD) {
        char *safe = block_payload(block) + sizeof(tlsf_block_t *) * 2;
        size_t safe_len = bsize - BLOCK_PAYLOAD_OVERHEAD;
        POISON_FILL(safe, 0xFF, safe_len);
        ASAN_POISON(safe, safe_len);
    }
}

/* Return location of previous block. */
/*@
  requires tlsf_aligned_header(block);
  requires (block->header / BLOCK_BIT_PREV_FREE) % 2 != 0;
  assigns \result \from block->prev;
  ensures \result == block->prev;
*/
INLINE tlsf_block_t *block_prev(const tlsf_block_t *block)
{
    ASSERT(block_is_prev_free(block), "previous block must be free");
    return block->prev;
}

/* Return location of next existing block. */
/*@
  requires tlsf_next_header_span(block);
  requires tlsf_aligned_header(block);
  requires block->header - block->header % ALIGN_SIZE >= BLOCK_OVERHEAD;
  assigns \result \from block, block->header;
*/
INLINE tlsf_block_t *block_next(tlsf_block_t *block)
{
    size_t size = block_size(block);
    ASSERT(size >= BLOCK_OVERHEAD, "block is last");
    tlsf_block_t *next = to_block(block_payload(block) + size - BLOCK_OVERHEAD);
    return next;
}

/*@
  requires \valid(next);
  assigns next->prev \from block;
  ensures next->prev == block;
*/
INLINE void block_link(tlsf_block_t *block, tlsf_block_t *next)
{
    next->prev = block;
}

/*@
  requires \valid(prev);
  requires \valid(next);
  requires prev == next || \separated(prev, next);
  requires tlsf_aligned_header(prev);
  requires size % ALIGN_SIZE == 0;
  requires size <= SIZE_MAX - prev->header;
  assigns prev->header \from prev->header, size;
  assigns next->prev \from prev;
  assigns \result \from prev;
  ensures \result == prev;
  ensures prev->header == \old(prev->header) + size;
  ensures next->prev == prev;
  ensures tlsf_aligned_header(prev);
*/
INLINE tlsf_block_t *block_absorb_at(tlsf_block_t *prev,
                                     tlsf_block_t *next,
                                     size_t size)
{
    block_add_size(prev, size);
    block_link(prev, next);
    return prev;
}

/* Link a new block with its neighbor, return the neighbor. */
INLINE tlsf_block_t *block_link_next(tlsf_block_t *block)
{
    tlsf_block_t *next = block_next(block);
    block_link(block, next);
    return next;
}

/*@
  requires tlsf_aligned_header(block);
  requires size <= SIZE_MAX - sizeof(tlsf_block_t);
  assigns \nothing;
  ensures \result ==> block->header >= sizeof(tlsf_block_t) + size;
*/
MAYBE_UNUSED INLINE bool block_can_split(const tlsf_block_t *block, size_t size)
{
    return block_size(block) >= sizeof(tlsf_block_t) + size;
}

/* When trimming, require the remainder to be at least TLSF_SPLIT_THRESHOLD to
 * avoid creating tiny free blocks that waste metadata overhead.
 */
/*@
  requires tlsf_aligned_header(block);
  assigns \nothing;
  ensures \result ==>
            block->header >= BLOCK_OVERHEAD + TLSF_SPLIT_THRESHOLD + size;
*/
INLINE bool block_can_trim(const tlsf_block_t *block, size_t size)
{
    /* Subtraction form so the comparison cannot wrap for any 'size', even if
     * the TLSF_SPLIT_THRESHOLD bound above is later relaxed. min_total is a
     * compile-time constant the static assert keeps well below SIZE_MAX.
     */
    size_t bsize = block_size(block);
    const size_t min_total = BLOCK_OVERHEAD + TLSF_SPLIT_THRESHOLD;
    return bsize >= min_total && size <= bsize - min_total;
}

/* The flag nibble is header % ALIGN_SIZE: bit 0 is FREE, bit 1 is PREV_FREE.
 * Both setters strip the nibble, keep the bit they do not own, and re-add
 * theirs, so the size part is untouched. Compilers fold this back to the same
 * single and/orr pair as the equivalent bit twiddling.
 */
/*@
  requires \valid(block);
  requires tlsf_aligned_header(block);
  assigns block->header;
  ensures tlsf_aligned_header(block);
  ensures block->header - block->header % ALIGN_SIZE ==
            \old(block->header) - \old(block->header) % ALIGN_SIZE;
  ensures free ==> block->header % 2 != 0;
  ensures !free ==> block->header % 2 == 0;
*/
INLINE void block_set_free_bit(tlsf_block_t *block, bool free)
{
    size_t flags = block->header % ALIGN_SIZE;
    block->header = block->header - flags +
                    flags / BLOCK_BIT_PREV_FREE * BLOCK_BIT_PREV_FREE +
                    (free ? BLOCK_BIT_FREE : 0);
}

/*@
  requires \valid(block);
  requires \valid(next);
  requires tlsf_aligned_header(block);
  requires tlsf_aligned_header(next);
  requires \separated(block, next);
  assigns block->header, next->header;
  ensures free ==> block->header % 2 != 0;
  ensures !free ==> block->header % 2 == 0;
  ensures free ==> (next->header / BLOCK_BIT_PREV_FREE) % 2 != 0;
  ensures !free ==> (next->header / BLOCK_BIT_PREV_FREE) % 2 == 0;
*/
INLINE void block_set_free_at(tlsf_block_t *block,
                              tlsf_block_t *next,
                              bool free)
{
    block_set_free_bit(block, free);
    block_set_prev_free(next, free);
}

INLINE void block_set_free(tlsf_block_t *block, bool free)
{
    ASSERT(block_is_free(block) != free, "block free bit unchanged");
    block_set_free_at(block, block_link_next(block), free);
}

/* Adjust allocation size to be aligned, and no smaller than internal minimum.
 * Check bounds BEFORE alignment to prevent integer overflow. align_up()
 * computes (((x-1) | (align-1)) + 1), which wraps to 0 when x is near SIZE_MAX,
 * bypassing subsequent TLSF_MAX_SIZE checks.
 */
/*@
  requires align > 0;
  requires (align & (align - 1)) == 0;
  assigns \nothing;
  ensures size > TLSF_MAX_SIZE ==> \result == size;
*/
INLINE size_t adjust_size(size_t size, size_t align)
{
    if (UNLIKELY(size > TLSF_MAX_SIZE))
        return size; /* Preserve huge value to fail caller's bounds check */
    size = align_up(size, align);
    return size < BLOCK_SIZE_MIN ? BLOCK_SIZE_MIN : size;
}

/* Round up to the next block size. Branch-free: for small sizes (<
 * BLOCK_SIZE_SMALL), the rounding mask is zero, producing an identity. For
 * large sizes, it rounds up to the next second-level bin boundary.
 */
/*@
  requires size > 0;
  requires size <= TLSF_MAX_SIZE;
  requires size % ALIGN_SIZE == 0;
  assigns \nothing;
*/
INLINE size_t round_block_size(size_t size)
{
    uint32_t lg = log2floor(size);
    size_t is_large = (size_t) (lg >= (uint32_t) FL_SHIFT);

    /* Clamp shift to valid range; garbage value is harmless when is_large=0
     * because shifting zero by any valid amount yields zero.
     */
    uint32_t shift =
        (lg - (uint32_t) SL_SHIFT) & ((uint32_t) (_TLSF_SIZE_WIDTH - 1));
    size_t round = is_large << shift;
    /* Large: (1 << shift) - 1 = SL rounding mask.  Small: 0 - 0 = 0. */
    size_t t = round - is_large;
    return (size + t) & ~t;
}

/* Map size to first-level (fl) and second-level (sl) bin indices. Branch-free:
 * bitmask selection handles small sizes (linear binning in fl=0) and large
 * sizes (logarithmic binning) without a conditional branch. Beneficial on
 * in-order cores (e.g., Cortex-M) where branch misprediction stalls the
 * pipeline.
 */
/*@
  requires size > 0;
  requires size < ((size_t) 1 << FL_MAX);
  requires \valid(fl);
  requires \valid(sl);
  requires \separated(fl, sl);
  assigns *fl, *sl;
*/
INLINE void mapping(size_t size, uint32_t *fl, uint32_t *sl)
{
    uint32_t t = log2floor(size);

    /* All-ones mask when size is in the linear range (< BLOCK_SIZE_SMALL),
     * all-zeros when in the logarithmic range. Subtracting from zero rather
     * than negating keeps unary minus off an unsigned operand, which MSVC
     * reports as C4146.
     */
    uint32_t small = 0u - (uint32_t) (t < (uint32_t) FL_SHIFT);

    /* FL: 0 for small sizes, (t - FL_SHIFT + 1) for large sizes. The wrapping
     * subtraction when t < FL_SHIFT is harmless because ~small masks it to
     * zero.
     */
    *fl = ~small & (t - (uint32_t) FL_SHIFT + 1);

    /* SL: linear index for small, logarithmic for large. Clamp the shift to
     * avoid undefined behavior when t < SL_SHIFT; the garbage result is masked
     * out by 'small'.
     */
    uint32_t shift =
        (t - (uint32_t) SL_SHIFT) & ((uint32_t) (_TLSF_SIZE_WIDTH - 1));
    uint32_t sl_large = (uint32_t) (size >> shift) ^ SL_COUNT;
    uint32_t sl_small = (uint32_t) (size >> ALIGN_SHIFT);
    *sl = (~small & sl_large) | (small & sl_small);

    ASSERT(*fl < FL_COUNT, "wrong first level");
    ASSERT(*sl < SL_COUNT, "wrong second level");
}

/* The preconditions below mirror the runtime asserts and are what discharge the
 * shift and array-index obligations on 't->sl[*fl]' and '~0U << *sl'.
 *
 * Deliberately kept out of WP_FUNCTIONS: every goal but one proves, and the
 * holdout is the bitmap_ffs precondition on 'sl_map = t->sl[*fl]'. Proving it
 * needs the coherence invariant "a set bit in t->fl implies a nonzero
 * t->sl[i]", which in turn needs a postcondition relating bitmap_ffs to the bit
 * it found. bitmap_ffs is __builtin_ctz here, and Alt-Ergo does not discharge
 * that bit-level link. Add this function to WP_FUNCTIONS only together with
 * that invariant.
 */
/*@
  requires \valid(t);
  requires \valid(fl) && \valid(sl);
  requires \separated(fl, sl);
  requires *fl < FL_COUNT;
  requires *sl < SL_COUNT;
  assigns *fl, *sl;
*/
INLINE tlsf_block_t *block_find_suitable(tlsf_t *t, uint32_t *fl, uint32_t *sl)
{
    ASSERT(*fl < FL_COUNT, "wrong first level");
    ASSERT(*sl < SL_COUNT, "wrong second level");

    /* Search for a block in the list associated with the given fl/sl index. */
    uint32_t sl_map = t->sl[*fl] & (~0U << *sl);
    if (!sl_map) {
        /* No block exists. Search in the next largest first-level list. */
        uint32_t fl_map = t->fl & ((*fl + 1 >= 32) ? 0U : (~0U << (*fl + 1)));

        /* No free blocks available, memory has been exhausted. */
        if (UNLIKELY(!fl_map))
            return NULL;

        *fl = bitmap_ffs(fl_map);
        ASSERT(*fl < FL_COUNT, "wrong first level");

        sl_map = t->sl[*fl];
        ASSERT(sl_map, "second level bitmap is null");
    }

    *sl = bitmap_ffs(sl_map);
    ASSERT(*sl < SL_COUNT, "wrong second level");

    return t->block[*fl][*sl];
}

/*@
  requires \valid(t);
  requires fl < FL_COUNT;
  requires sl < SL_COUNT;
  assigns t->block[fl][sl] \from block;
  ensures t->block[fl][sl] == block;
*/
INLINE void bin_set_head(tlsf_t *t,
                         uint32_t fl,
                         uint32_t sl,
                         tlsf_block_t *block)
{
    t->block[fl][sl] = block;
}

/* prev and next may be the same block (a bin holding one entry, or the sentinel
 * absorbing writes). prev_free and next_free are distinct fields, so the two
 * stores never alias and the aliased case needs no special handling: it writes
 * the block's own two links back to itself, which is what an empty list means
 * here.
 */
/*@
  requires \valid(prev);
  requires \valid(next);
  requires prev == next || \separated(prev, next);
  assigns next->prev_free \from prev;
  assigns prev->next_free \from next;
  ensures next->prev_free == prev;
  ensures prev->next_free == next;
*/
INLINE void free_list_unlink(tlsf_block_t *prev, tlsf_block_t *next)
{
    next->prev_free = prev;
    prev->next_free = next;
}

/* Remove a free block from the free list. Unconditional writes: prev/next may
 * be &t->block_null (sentinel), in which case the writes are harmless.
 */
/*@
  requires \valid(t);
  requires \valid(block);
  requires fl < FL_COUNT;
  requires sl < SL_COUNT;
  requires \valid(block->prev_free);
  requires \valid(block->next_free);
  requires block->prev_free == block->next_free ||
          \separated(block->prev_free, block->next_free);
  assigns block->prev_free->next_free, block->next_free->prev_free;
  assigns t->block[fl][sl], t->sl[fl], t->fl;
*/
INLINE void remove_free_block(tlsf_t *t,
                              tlsf_block_t *block,
                              uint32_t fl,
                              uint32_t sl)
{
    ASSERT(fl < FL_COUNT, "wrong first level");
    ASSERT(sl < SL_COUNT, "wrong second level");

    tlsf_block_t *prev = block->prev_free;
    tlsf_block_t *next = block->next_free;
    free_list_unlink(prev, next);

    /* If this block is the head of the free list, set new head. */
    if (t->block[fl][sl] == block) {
        bin_set_head(t, fl, sl, next);

        /* If the new head is the sentinel, the bin is empty. */
        if (next == &t->block_null) {
            t->sl[fl] &= ~(1U << sl);

            /* If the second bitmap is now empty, clear the fl bitmap. */
            if (!t->sl[fl])
                t->fl &= ~(1U << fl);
        }
    }
}

/*@
  requires \valid(block);
  requires \valid(current);
  requires \valid(sentinel);
  requires \separated(block, current);
  requires \separated(block, sentinel);
  assigns block->next_free \from current;
  assigns block->prev_free \from sentinel;
  assigns current->prev_free \from block;
  ensures block->next_free == current;
  ensures block->prev_free == sentinel;
  ensures current->prev_free == block;
*/
INLINE void free_list_link(tlsf_block_t *block,
                           tlsf_block_t *current,
                           tlsf_block_t *sentinel)
{
    block->next_free = current;
    block->prev_free = sentinel;
    current->prev_free = block;
}

/* Insert a free block into the free block list and mark the bitmaps.
 * Unconditional write: current may be &t->block_null (sentinel), in which case
 * the write to current->prev_free is harmless.
 */
/*@
  requires \valid(t);
  requires \valid(block);
  requires fl < FL_COUNT;
  requires sl < SL_COUNT;
  requires \valid(t->block[fl][sl]);
  requires \separated(block, t->block[fl][sl]);
  requires \separated(block, &t->block_null);
  assigns block->next_free, block->prev_free;
  assigns t->block[fl][sl]->prev_free;
  assigns t->block[fl][sl], t->fl, t->sl[fl];
*/
INLINE void insert_free_block(tlsf_t *t,
                              tlsf_block_t *block,
                              uint32_t fl,
                              uint32_t sl)
{
    tlsf_block_t *current = t->block[fl][sl];
    ASSERT(block, "cannot insert a null entry into the free list");
    free_list_link(block, current, &t->block_null);
    bin_set_head(t, fl, sl, block);
    t->fl |= 1U << fl;
    t->sl[fl] |= 1U << sl;
}

/* Remove a given block from the free list. */
INLINE void block_remove(tlsf_t *t, tlsf_block_t *block)
{
    uint32_t fl, sl;
    mapping(block_size(block), &fl, &sl);
    remove_free_block(t, block, fl, sl);
}

/* Insert a given block into the free list. */
INLINE void block_insert(tlsf_t *t, tlsf_block_t *block)
{
    uint32_t fl, sl;
    mapping(block_size(block), &fl, &sl);
    insert_free_block(t, block, fl, sl);
}

/* Split a block into two, the second of which is free. */
INLINE tlsf_block_t *block_split(tlsf_block_t *block, size_t size)
{
    tlsf_block_t *rest = to_block(block_payload(block) + size - BLOCK_OVERHEAD);
    size_t rest_size = block_size(block) - (size + BLOCK_OVERHEAD);
    ASSERT(block_size(block) == rest_size + size + BLOCK_OVERHEAD,
           "rest block size is wrong");
    ASSERT(rest_size >= BLOCK_SIZE_MIN, "block split with invalid size");
    block_split_headers(block, rest, size, rest_size);
    ASSERT(!(rest_size % ALIGN_SIZE), "invalid block size");
    block_set_free(rest, true);

    block_poison_free(rest);

    return rest;
}

/* Absorb a free block's storage into an adjacent previous free block. 'block'
 * is not const: its successor is resolved through it, and that successor is
 * then written. Taking it const would only move a const-discarding cast inside.
 *
 * Resolving next from 'block' before growing 'prev' (rather than from 'prev'
 * after, as the size arithmetic would also allow) keeps the successor lookup
 * off a header that is mid-update. The two agree only because a block's
 * successor sits at 'block + BLOCK_HEADER_OFFSET + block_size(block)' and
 * BLOCK_HEADER_OFFSET == BLOCK_OVERHEAD, which the static assert above pins.
 */
INLINE tlsf_block_t *block_absorb(tlsf_block_t *prev, tlsf_block_t *block)
{
    ASSERT(block_size(prev), "previous block can't be last");
    size_t size = block_size(block) + BLOCK_OVERHEAD;
    tlsf_block_t *next = block_next(block);
    return block_absorb_at(prev, next, size);
}

/* Merge a just-freed block with an adjacent previous free block. */
INLINE tlsf_block_t *block_merge_prev(tlsf_t *t, tlsf_block_t *block)
{
    if (block_is_prev_free(block)) {
        tlsf_block_t *prev = block_prev(block);
        ASSERT(prev, "prev block can't be null");
        ASSERT(block_is_free(prev),
               "prev block is not free though marked as such");
        block_remove(t, prev);
        block = block_absorb(prev, block);
    }
    return block;
}

/* Merge a just-freed block with an adjacent free block. */
INLINE tlsf_block_t *block_merge_next(tlsf_t *t, tlsf_block_t *block)
{
    tlsf_block_t *next = block_next(block);
    ASSERT(next, "next block can't be null");
    if (block_is_free(next)) {
        ASSERT(block_size(block), "previous block can't be last");
        block_remove(t, next);
        block = block_absorb(block, next);
    }
    return block;
}

/* Trim any trailing block space off the end of a block, return to pool. */
INLINE void block_rtrim_free(tlsf_t *t, tlsf_block_t *block, size_t size)
{
    ASSERT(block_is_free(block), "block must be free");
    if (!block_can_trim(block, size))
        return;
    tlsf_block_t *rest = block_split(block, size);
    block_link_next(block);
    block_set_prev_free(rest, true);
    block_insert(t, rest);
}

/* Trim any trailing block space off the end of a used block, return to pool. */
INLINE void block_rtrim_used(tlsf_t *t, tlsf_block_t *block, size_t size)
{
    ASSERT(!block_is_free(block), "block must be used");
    if (!block_can_trim(block, size))
        return;
    tlsf_block_t *rest = block_split(block, size);
    block_set_prev_free(rest, false);
    rest = block_merge_next(t, rest);
    block_insert(t, rest);
}

INLINE tlsf_block_t *block_ltrim_free(tlsf_t *t,
                                      tlsf_block_t *block,
                                      size_t size)
{
    ASSERT(block_is_free(block), "block must be free");
    ASSERT(block_can_split(block, size), "block is too small");
    tlsf_block_t *rest = block_split(block, size - BLOCK_OVERHEAD);
    block_set_prev_free(rest, true);
    block_link_next(block);
    block_insert(t, block);
    block_poison_free(block);
    return rest;
}

INLINE void *block_use(tlsf_t *t, tlsf_block_t *block, size_t size)
{
    /* Unpoison before trimming -- block_split writes into the payload. */
    ASAN_UNPOISON(block_payload(block), block_size(block));
    block_rtrim_free(t, block, size);
    block_set_free(block, false);
    POISON_FILL(block_payload(block), 0xAA, block_size(block));

    return block_payload(block);
}

/*@
  requires tlsf_aligned_header(block);
  assigns \nothing;
*/
INLINE void check_sentinel(tlsf_block_t *block)
{
    (void) block;
    ASSERT(!block_size(block), "sentinel should be last");
    ASSERT(!block_is_free(block), "sentinel block should not be free");
}

/* Point every bin at the free-list sentinel and clear the bitmaps, so that
 * insert/remove can write unconditionally without a NULL check. Cost is a fixed
 * O(FL_COUNT * SL_COUNT) regardless of how much is allocated.
 */
/*@
  requires \valid(t);
  assigns t->fl, t->sl[0 .. FL_COUNT - 1];
  assigns t->block[0 .. FL_COUNT - 1][0 .. SL_COUNT - 1] \from t;
  ensures tlsf_empty_bins(t);
  ensures t->fl == 0;
  ensures \forall integer i; 0 <= i < FL_COUNT ==> t->sl[i] == 0;
  ensures \forall integer i, j;
            0 <= i < FL_COUNT && 0 <= j < SL_COUNT ==>
              t->block[i][j] == &t->block_null;
*/
static void bins_reset(tlsf_t *t)
{
    t->fl = 0;
    /*@
      loop invariant 0 <= i <= FL_COUNT;
      loop invariant \forall integer k; 0 <= k < i ==> t->sl[k] == 0;
      loop assigns i, t->sl[0 .. FL_COUNT - 1];
      loop variant FL_COUNT - i;
    */
    for (uint32_t i = 0; i < FL_COUNT; i++)
        t->sl[i] = 0;
    /*@
      loop invariant 0 <= i <= FL_COUNT;
      loop invariant t->fl == 0;
      loop invariant \forall integer k; 0 <= k < FL_COUNT ==> t->sl[k] == 0;
      loop invariant \forall integer k, j;
        0 <= k < i && 0 <= j < SL_COUNT ==>
          t->block[k][j] == &t->block_null;
      loop assigns i, t->block[0 .. FL_COUNT - 1][0 .. SL_COUNT - 1];
      loop variant FL_COUNT - i;
    */
    for (uint32_t i = 0; i < FL_COUNT; i++)
        /*@
          loop invariant 0 <= j <= SL_COUNT;
          loop invariant \forall integer k; 0 <= k < j ==>
            t->block[i][k] == &t->block_null;
          loop assigns j, t->block[i][0 .. SL_COUNT - 1];
          loop variant SL_COUNT - j;
        */
        for (uint32_t j = 0; j < SL_COUNT; j++)
            t->block[i][j] = &t->block_null;
}

/* Lay out a pool as a single free block of 'free_size' payload bytes followed
 * by the terminating zero-size sentinel. 'start' is the first payload byte; the
 * block header sits one word below it, and that block's prev field lies outside
 * the pool and is never read.
 */
static void pool_build(tlsf_t *t, char *start, size_t free_size)
{
    tlsf_block_t *block = to_block(start - BLOCK_OVERHEAD);
    block->header = free_size | BLOCK_BIT_FREE;
    block_insert(t, block);

    tlsf_block_t *sentinel = block_link_next(block);
    sentinel->header = BLOCK_BIT_PREV_FREE;
    check_sentinel(sentinel);

    block_poison_free(block);
}

static bool arena_grow(tlsf_t *t, size_t size)
{
    /* Fixed pools cannot grow. */
    if (t->fixed)
        return false;

    /* First use of a dynamic pool: point all empty-bin pointers at the sentinel
     * so that insert/remove can write unconditionally.
     */
    if (!t->size)
        bins_reset(t);

    size_t req_size =
        (t->size ? t->size + BLOCK_OVERHEAD : 2 * BLOCK_OVERHEAD) + size;

    /* Pool cannot exceed the maximum addressable range for the configured
     * first-level index. With reduced TLSF_MAX_POOL_BITS, this prevents merged
     * blocks from overflowing the mapping function.
     */
    if (UNLIKELY(req_size > (size_t) 1 << FL_MAX))
        return false;

    void *addr = tlsf_resize(t, req_size);
    if (!addr)
        return false;
    ASSERT((size_t) addr % ALIGN_SIZE == 0, "wrong heap alignment address");

    /* Cache the base so later reads (tlsf_check, tlsf_get_stats, pool append)
     * never have to call back into tlsf_resize just to learn the address.
     */
    t->arena = addr;

    /* Clear stale ASan shadow in the growth region: prior arena_shrink cycles
     * may have left poisoned shadow bytes that were never cleared.
     */
    ASAN_UNPOISON((char *) addr + t->size, req_size - t->size);
    tlsf_block_t *block =
        to_block(t->size ? (char *) addr + t->size - 2 * BLOCK_OVERHEAD
                         : (char *) addr - BLOCK_OVERHEAD);
    if (!t->size)
        block->header = 0;
    check_sentinel(block);
    block->header |= size | BLOCK_BIT_FREE;
    block = block_merge_prev(t, block);
    block_insert(t, block);
    tlsf_block_t *sentinel = block_link_next(block);
    sentinel->header = BLOCK_BIT_PREV_FREE;
    t->size = req_size;
    check_sentinel(sentinel);

    block_poison_free(block);
    return true;
}

static size_t arena_append_pool(tlsf_t *t, void *mem, size_t size)
{
    if (!t->size || !mem || size < 2 * BLOCK_OVERHEAD ||
        size >= (size_t) 1 << FL_MAX)
        return 0;

    /* Align memory block boundaries */
    const char *start = align_ptr((char *) mem, ALIGN_SIZE);
    const char *end = (char *) mem + size;
    size_t aligned_size = (size_t) (end - start) & ~(ALIGN_SIZE - 1);

    /* For fixed pools, the new sentinel must fit within the appended region
     * itself, since there is no backend to provide extra bytes.
     */
    if (t->fixed) {
        if (aligned_size <= BLOCK_OVERHEAD)
            return 0;
        aligned_size -= BLOCK_OVERHEAD;
    }

    if (aligned_size < 2 * BLOCK_OVERHEAD)
        return 0;

    /* Get current pool information */
    void *current_pool_start = t->arena;
    if (!current_pool_start)
        return 0;

    const char *current_pool_end = (char *) current_pool_start + t->size;

    /* Only support coalescing if the new memory is immediately adjacent to the
     * current pool
     */
    if (start != current_pool_end)
        return 0;

    /* Update the pool size first to include the new memory. We need
     * aligned_size for payload + BLOCK_OVERHEAD for new sentinel.
     */
    size_t old_size = t->size;

    /* Check before adding, so the total cannot wrap on 32-bit targets. */
    if (UNLIKELY(t->size > ((size_t) 1 << FL_MAX) - BLOCK_OVERHEAD ||
                 aligned_size >
                     ((size_t) 1 << FL_MAX) - BLOCK_OVERHEAD - t->size))
        return 0;
    size_t new_total_size = t->size + aligned_size + BLOCK_OVERHEAD;

    /* For dynamic pools, request the backend to extend. For fixed pools, the
     * caller provides adjacent memory directly.
     */
    if (!t->fixed) {
        void *resized = tlsf_resize(t, new_total_size);
        if (!resized)
            return 0;
        current_pool_start = resized;
        t->arena = resized;

        /* Clear stale ASan shadow in the extension region. */
        ASAN_UNPOISON((char *) resized + old_size, new_total_size - old_size);
    } else {
        ASAN_UNPOISON(mem, size);
    }

    /* Update our pool size */
    t->size = new_total_size;

    /* Find the current sentinel block */
    tlsf_block_t *old_sentinel =
        to_block((char *) current_pool_start + old_size - 2 * BLOCK_OVERHEAD);
    check_sentinel(old_sentinel);

    /* Check if the block before the sentinel is free */
    tlsf_block_t *last_block = NULL;
    if (block_is_prev_free(old_sentinel)) {
        last_block = block_prev(old_sentinel);
        ASSERT(last_block && block_is_free(last_block),
               "last block should be free");
        /* Remove the last free block from lists since we'll recreate it */
        block_remove(t, last_block);
    }

    /* Calculate the new free block size. The old sentinel header becomes the
     * new block's header (not payload). Payload is just the appended memory.
     */
    size_t new_free_size = aligned_size;
    tlsf_block_t *new_free_block;

    if (last_block) {
        /* Merge with the existing free block. Absorb: last_block payload + old
         * sentinel header + new memory.
         */
        new_free_size += block_size(last_block) + BLOCK_OVERHEAD;
        new_free_block = last_block;
    } else {
        /* Convert the old sentinel into the start of the new free block */
        new_free_block = old_sentinel;
    }

    /* Set up the new free block header */
    new_free_block->header = new_free_size | BLOCK_BIT_FREE;

    /* When !last_block, the previous block is allocated (otherwise
     * block_is_prev_free(old_sentinel) would have been true and we would have
     * taken the last_block path). BLOCK_BIT_PREV_FREE is already clear from the
     * header assignment above.
     *
     * Do NOT write new_free_block->prev: it physically overlaps with the
     * previous allocated block's payload tail (by TLSF block layout, the next
     * block's prev field sits in the last sizeof(void *) bytes of the current
     * block's payload). The prev field is only read through block_prev(), which
     * asserts block_is_prev_free() first.
     */

    /* Insert the new free block into the appropriate list */
    block_insert(t, new_free_block);

    /* Create a new sentinel at the end */
    tlsf_block_t *new_sentinel = block_link_next(new_free_block);
    new_sentinel->header = BLOCK_BIT_PREV_FREE;
    check_sentinel(new_sentinel);

    block_poison_free(new_free_block);
    return aligned_size;
}

static void arena_shrink(tlsf_t *t, tlsf_block_t *block)
{
    check_sentinel(block_next(block));
    size_t size = block_size(block);
    ASSERT(t->size + BLOCK_OVERHEAD >= size, "invalid heap size before shrink");
    t->size = t->size - size - BLOCK_OVERHEAD;
    if (t->size == BLOCK_OVERHEAD)
        t->size = 0;
    void *addr = tlsf_resize(t, t->size);
    if (!t->size) {
        /* Pool fully released; the next allocation grows it from scratch. */
        t->arena = NULL;
    } else {
        /* Keep the previous base if the backend declined to shrink. */
        if (addr)
            t->arena = addr;
        block->header = 0;
        check_sentinel(block);
    }
}

INLINE tlsf_block_t *block_find_free(tlsf_t *t, size_t *size)
{
    *size = round_block_size(*size);
    uint32_t fl, sl;
    mapping(*size, &fl, &sl);
    tlsf_block_t *block = block_find_suitable(t, &fl, &sl);
    if (UNLIKELY(!block)) {
        if (!arena_grow(t, *size))
            return NULL;
        block = block_find_suitable(t, &fl, &sl);
        ASSERT(block, "no block found");
    }

    /* *size stays at the rounded request. round_block_size() above already put
     * it exactly on a bin boundary, which is what bin-boundary reuse requires:
     * a freed block lands in the bin a same-size request will search.
     *
     * Do NOT substitute mapping_size(fl, sl) here. block_find_suitable() may
     * return a block from a LARGER bin than the request maps to, and inflating
     * the allocation to that bin's minimum hands out the whole block: a fresh 1
     * MB pool then served two 1 KB allocations instead of ~1000.
     */
    ASSERT(block_size(block) >= *size, "insufficient block size");
    remove_free_block(t, block, fl, sl);
    return block;
}

void *tlsf_malloc(tlsf_t *t, size_t size)
{
    size = adjust_size(size, ALIGN_SIZE);
    if (UNLIKELY(size > TLSF_MAX_SIZE))
        return NULL;

    /* Fast path: small sizes (FL=0) use linear SL mapping directly. FL=0 bins
     * are spaced at ALIGN_SIZE granularity, so we can skip log2floor,
     * round_block_size, and mapping entirely.
     */
    if (size < BLOCK_SIZE_SMALL) {
        uint32_t sl = (uint32_t) (size >> ALIGN_SHIFT);
        uint32_t sl_map = t->sl[0] & (~0U << sl);
        if (sl_map) {
            uint32_t found_sl = bitmap_ffs(sl_map);

            /* Keep 'size' at the request. bitmap_ffs may land on a bin above
             * the one the request maps to, and inflating the allocation to that
             * bin's size hands out the whole block instead of trimming it,
             * which is the defect block_find_free() documents below. The
             * request is already ALIGN_SIZE-aligned and at least
             * BLOCK_SIZE_MIN, and FL=0 bins are ALIGN_SIZE-spaced, so it is
             * already on a bin boundary: a trimmed block still lands in the bin
             * a same-size request will search, and an untrimmable one keeps its
             * own size and maps to its own bin.
             */
            tlsf_block_t *block = t->block[0][found_sl];
            remove_free_block(t, block, 0, found_sl);
            return block_use(t, block, size);
        }
        /* Fall through: search larger FL classes via generic path */
    }

    tlsf_block_t *block = block_find_free(t, &size);
    if (UNLIKELY(!block))
        return NULL;
    return block_use(t, block, size);
}

void *tlsf_aalloc(tlsf_t *t, size_t align, size_t size)
{
    size_t adjust = adjust_size(size, ALIGN_SIZE);

    if (UNLIKELY(
            !align || (align & (align - 1)) /* align must be power of two */
            || align > TLSF_MAX_SIZE || sizeof(tlsf_block_t) > TLSF_MAX_SIZE ||
            adjust > TLSF_MAX_SIZE - align -
                         sizeof(tlsf_block_t) /* size is too large */))
        return NULL;

    if (align <= ALIGN_SIZE)
        return tlsf_malloc(t, size);

    size_t asize =
        adjust_size(adjust + align - 1 + sizeof(tlsf_block_t), align);
    tlsf_block_t *block = block_find_free(t, &asize);
    if (UNLIKELY(!block))
        return NULL;

    ASAN_UNPOISON(block_payload(block), block_size(block));

    const char *mem =
        align_ptr(block_payload(block) + sizeof(tlsf_block_t), align);
    block = block_ltrim_free(t, block, (size_t) (mem - block_payload(block)));
    return block_use(t, block, adjust);
}

void tlsf_free(tlsf_t *t, void *mem)
{
    if (UNLIKELY(!mem))
        return;

    tlsf_block_t *block = block_from_payload(mem);
    ASSERT(!block_is_free(block), "block already marked as free");

    block_set_free(block, true);
    block = block_merge_prev(t, block);
    block = block_merge_next(t, block);

    block_poison_free(block);

    if (UNLIKELY(!block_size(block_next(block))) && !t->fixed)
        arena_shrink(t, block);
    else
        block_insert(t, block);
}

size_t tlsf_usable_size(void *ptr)
{
    if (UNLIKELY(!ptr))
        return 0;
    const tlsf_block_t *block = block_from_payload(ptr);
    ASSERT(!block_is_free(block), "block must be allocated");
    return block_size(block);
}

void *tlsf_realloc(tlsf_t *t, void *mem, size_t size)
{
    /* Zero-size requests are treated as free. */
    if (UNLIKELY(mem && !size)) {
        tlsf_free(t, mem);
        return NULL;
    }

    /* Null-pointer requests are treated as malloc. */
    if (UNLIKELY(!mem))
        return tlsf_malloc(t, size);

    tlsf_block_t *block = block_from_payload(mem);
    size_t avail = block_size(block);
    size = adjust_size(size, ALIGN_SIZE);
    if (UNLIKELY(size > TLSF_MAX_SIZE))
        return NULL;

    ASSERT(!block_is_free(block), "block already marked as free");

    /* Do we need to expand? */
    if (size > avail) {
        tlsf_block_t *next = block_next(block);
        bool next_free = block_is_free(next);
        size_t next_size = next_free ? block_size(next) + BLOCK_OVERHEAD : 0;

        /* Try forward expansion first (no data movement required). */
        if (next_free && size <= avail + next_size) {
            block_merge_next(t, block);
            ASAN_UNPOISON(block_payload(block), block_size(block));
            block_set_prev_free(block_next(block), false);
        }
        /* Try backward expansion (requires memmove). */
        else if (block_is_prev_free(block)) {
            tlsf_block_t *prev = block_prev(block);
            size_t prev_size = block_size(prev);
            size_t combined = prev_size + avail + BLOCK_OVERHEAD;

            /* Can also merge with next if it's free. */
            if (next_free)
                combined += next_size;

            if (size <= combined) {
                /* Remove prev from free list. */
                block_remove(t, prev);

                ASAN_UNPOISON(block_payload(prev), prev_size);

                /* Move data to prev's payload area (regions may overlap). */
                memmove(block_payload(prev), mem, avail);

                /* Merge prev + current: update size, preserve prev's prev_free
                 * bit. Result is a used block (not free).
                 */
                size_t new_size = prev_size + avail + BLOCK_OVERHEAD;
                prev->header = new_size | (prev->header & BLOCK_BIT_PREV_FREE);
                block_link_next(prev);

                /* Also merge next if it's free. */
                if (next_free) {
                    block_remove(t, next);
                    ASAN_UNPOISON(block_payload(next), block_size(next));
                    prev->header += block_size(next) + BLOCK_OVERHEAD;
                    block_link_next(prev);
                }

                /* Update next block's prev_free status (we're now used). */
                block_set_prev_free(block_next(prev), false);

                /* Switch to the merged block. */
                block = prev;
                mem = block_payload(block);
            } else {
                /* Combined space still insufficient, must relocate. */
                void *dst = tlsf_malloc(t, size);
                if (dst) {
                    memcpy(dst, mem, avail);
                    tlsf_free(t, mem);
                }
                return dst;
            }
        } else {
            /* No in-place expansion possible, must relocate. */
            void *dst = tlsf_malloc(t, size);
            if (dst) {
                memcpy(dst, mem, avail);
                tlsf_free(t, mem);
            }
            return dst;
        }
    }

    /* Trim the resulting block and return the pointer. */
    block_rtrim_used(t, block, size);
    return mem;
}

size_t tlsf_append_pool(tlsf_t *t, void *mem, size_t size)
{
    if (UNLIKELY(!t || !mem || !size))
        return 0;

    return arena_append_pool(t, mem, size);
}

size_t tlsf_pool_init(tlsf_t *t, void *mem, size_t bytes)
{
    if (!t || !mem)
        return 0;

    size_t adj = align_offset((char *) mem, ALIGN_SIZE);
    if (bytes <= adj)
        return 0;
    char *start = (char *) mem + adj;

    /* Compute usable pool size (aligned down) */
    size_t pool_bytes = (bytes - adj) & ~(ALIGN_SIZE - 1);
    if (pool_bytes < 2 * BLOCK_OVERHEAD + BLOCK_SIZE_MIN)
        return 0;

    size_t free_size = pool_bytes - 2 * BLOCK_OVERHEAD;
    free_size &= ~(ALIGN_SIZE - 1);
    if (free_size < BLOCK_SIZE_MIN || free_size > BLOCK_SIZE_MAX)
        return 0;

    /* Clear any stale ASan shadow in the provided memory. Deferred until every
     * argument check has passed so a rejected re-init leaves the poisoning of
     * an existing arena, and with it use-after-free detection, intact.
     */
    ASAN_UNPOISON(mem, bytes);

    /* Zero-initialize the control structure, then point every bin at the
     * sentinel so that free-list insert/remove can write unconditionally.
     */
    memset(t, 0, sizeof(*t));
    bins_reset(t);

    /* Mark as a fixed-size, caller-owned pool */
    t->fixed = true;
    t->arena = start;

    pool_build(t, start, free_size);
    t->size = free_size + 2 * BLOCK_OVERHEAD;

    return free_size;
}

void tlsf_pool_reset(tlsf_t *t)
{
    if (!t || !t->fixed)
        return;

    /* Unpoison the entire pool for ASan. */
    ASAN_UNPOISON(t->arena, t->size);

    bins_reset(t);

    /* Reconstruct the single free block spanning the entire pool. */
    pool_build(t, (char *) t->arena, t->size - 2 * BLOCK_OVERHEAD);
}

#ifdef TLSF_ENABLE_CHECK
#include <stdio.h>
#include <stdlib.h>
#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "TLSF CHECK: %s - %s\n", msg, #cond); \
            abort();                                              \
        }                                                         \
    } while (0)

/**
 * Comprehensive heap consistency check.
 *
 * Validates ALL block invariants by walking the entire heap:
 * 1. Block walk validation (all blocks from pool start to sentinel)
 * 2. Free list validation (bitmap consistency, coalescing, cycle/duplicate
 *    detection via Floyd's algorithm -- O(1) stack usage)
 * 3. Cross-validation (free list count matches block walk count)
 */
void tlsf_check(tlsf_t *t)
{
    CHECK(t, "tlsf_t pointer is null");

    /* Empty pool is valid */
    if (!t->size)
        return;

    /* Get arena start */
    void *arena_start = t->arena;
    CHECK(arena_start, "failed to get arena pointer");
    CHECK((size_t) arena_start % ALIGN_SIZE == 0, "arena not aligned");

    /* Phase 1: Walk ALL blocks from pool start to sentinel This validates the
     * physical block chain integrity
     *
     * The first block is at arena_start - BLOCK_OVERHEAD because the
     * tlsf_block_t structure's prev field precedes the header, but for the
     * first block, the prev field is outside the arena (never accessed).
     */
    tlsf_block_t *block = to_block((char *) arena_start - BLOCK_OVERHEAD);
    tlsf_block_t *prev_block = NULL;
    size_t walk_free_count = 0;
    size_t total_size = 0;
    bool prev_was_free = false;

    while (block_size(block) != 0) {
        size_t bsize = block_size(block);

        /* Size invariants */
        CHECK(bsize >= BLOCK_SIZE_MIN, "block smaller than minimum size");
        CHECK(bsize < (size_t) 1 << FL_MAX, "block exceeds mapping range");
        CHECK(bsize % ALIGN_SIZE == 0, "block size not aligned");

        /* Pointer alignment check */
        CHECK((size_t) block % ALIGN_SIZE == 0, "block pointer not aligned");
        CHECK((size_t) block_payload(block) % ALIGN_SIZE == 0,
              "payload not aligned");

        /* Prev pointer validation */
        if (prev_block) {
            CHECK(block_is_prev_free(block) == prev_was_free,
                  "prev_free bit mismatch with actual previous block state");
            if (prev_was_free) {
                CHECK(block->prev == prev_block,
                      "prev pointer doesn't match previous block");
            }
        }

        if (block_is_free(block)) {
            walk_free_count++;

            /* Coalescing invariant: no two consecutive free blocks */
            CHECK(!prev_was_free,
                  "consecutive free blocks (coalescing failed)");

            /* Free-list membership verified by Phase 2/3 count match */
            prev_was_free = true;
        } else {
            prev_was_free = false;
        }

        total_size += bsize + BLOCK_OVERHEAD;
        prev_block = block;
        block = block_next(block);
    }

    /* Sentinel validation */
    CHECK(block_size(block) == 0, "sentinel has non-zero size");
    CHECK(!block_is_free(block), "sentinel marked as free");
    CHECK(block_is_prev_free(block) == prev_was_free,
          "sentinel prev_free bit mismatch");
    if (prev_was_free && prev_block) {
        CHECK(block->prev == prev_block, "sentinel prev pointer incorrect");
    }

    /* Account for sentinel header */
    total_size += BLOCK_OVERHEAD;
    CHECK(total_size == t->size, "block sizes don't sum to pool size");

    /* Phase 2: Walk free lists and validate bitmap consistency */
    size_t list_free_count = 0;

    for (uint32_t i = 0; i < FL_COUNT; ++i) {
        uint32_t fl_bit = t->fl & (1U << i);
        uint32_t sl_list = t->sl[i];

        /* If FL bit is clear, all SL bits and block pointers must be the
         * sentinel.
         */
        if (!fl_bit) {
            CHECK(sl_list == 0, "SL bitmap non-zero but FL bit is clear");
            for (uint32_t j = 0; j < SL_COUNT; ++j) {
                CHECK(t->block[i][j] == &t->block_null,
                      "block pointer not sentinel but FL bit is clear");
            }
            continue;
        }

        /* FL bit is set, so at least one SL bit must be set */
        CHECK(sl_list != 0, "FL bit set but SL bitmap is empty");

        for (uint32_t j = 0; j < SL_COUNT; ++j) {
            uint32_t sl_bit = sl_list & (1U << j);
            tlsf_block_t *list_block = t->block[i][j];

            if (!sl_bit) {
                CHECK(list_block == &t->block_null,
                      "block pointer not sentinel but SL bit is clear");
                continue;
            }

            /* SL bit is set, so block list must be non-empty */
            CHECK(list_block != &t->block_null,
                  "SL bit set but block list is empty (sentinel)");

            /* Walk the free list for this bin. Floyd's cycle detection runs in
             * parallel: a fast pointer advances two steps per iteration. If a
             * duplicate block creates a cycle, slow and fast will collide in
             * O(n) steps. This replaces the former 16 KB hash-table approach
             * with O(1) stack usage -- critical for embedded/RTOS targets.
             *
             * Cross-bin duplicates are already caught above: Phase 2 validates
             * that each block maps to its bin, so a block cannot appear in two
             * different bins without failing the fl/sl check first.
             */
            const tlsf_block_t *list_prev = &t->block_null;
            const tlsf_block_t *fast = list_block;
            while (list_block != &t->block_null) {
                list_free_count++;

                /* Block must be free */
                CHECK(block_is_free(list_block), "block in free list not free");

                /* Block must be in correct bin */
                uint32_t fl, sl;
                mapping(block_size(list_block), &fl, &sl);
                CHECK(fl == i && sl == j, "block in wrong FL/SL bin");

                /* Size constraints */
                CHECK(block_size(list_block) >= BLOCK_SIZE_MIN,
                      "free block below minimum size");

                /* Coalescing: previous physical block must not be free */
                CHECK(!block_is_prev_free(list_block),
                      "free block has free predecessor (coalescing violated)");

                /* Coalescing: next physical block must not be free */
                tlsf_block_t *next_phys = block_next(list_block);
                CHECK(!block_is_free(next_phys),
                      "free block has free successor (coalescing violated)");

                /* Next block must have prev_free set */
                CHECK(block_is_prev_free(next_phys),
                      "next block doesn't know this block is free");

                /* Free list linkage */
                CHECK(list_block->prev_free == list_prev,
                      "free list prev pointer incorrect");
                if (list_prev != &t->block_null) {
                    CHECK(list_prev->next_free == list_block,
                          "free list next pointer incorrect");
                }

                list_prev = list_block;
                list_block = list_block->next_free;

                /* Floyd's tortoise-and-hare cycle detection */
                if (fast != &t->block_null)
                    fast = fast->next_free;
                if (fast != &t->block_null)
                    fast = fast->next_free;
                CHECK(list_block == &t->block_null || list_block != fast,
                      "cycle in free list (duplicate block / double-free?)");
            }
        }
    }

    /* Phase 3: Cross-validation */
    CHECK(walk_free_count == list_free_count,
          "free block count mismatch between block walk and free list walk");
}
#endif

/**
 * Collect heap statistics by walking all blocks.
 *
 * Statistics semantics:
 * - total_free/total_used: Payload bytes (usable by application)
 * - overhead: Metadata bytes (block headers + sentinel)
 * - block_count: Total blocks including used and free
 * - free_count: Number of free blocks (fragmentation indicator)
 */
int tlsf_get_stats(tlsf_t *t, tlsf_stats_t *stats)
{
    if (!t || !stats)
        return -1;

    stats->total_free = 0;
    stats->largest_free = 0;
    stats->total_used = 0;
    stats->block_count = 0;
    stats->free_count = 0;
    stats->overhead = 0;

    if (!t->size)
        return 0; /* Empty pool */

    /* t->arena is the current base for both fixed and dynamic pools. The first
     * block sits at arena_start - BLOCK_OVERHEAD because the tlsf_block_t
     * structure's prev field precedes the header.
     */
    void *arena_start = t->arena;
    if (!arena_start)
        return -1;

    tlsf_block_t *block = to_block((char *) arena_start - BLOCK_OVERHEAD);

    while (block_size(block) != 0) {
        size_t bsize = block_size(block);
        stats->block_count++;
        stats->overhead += BLOCK_OVERHEAD;

        if (block_is_free(block)) {
            stats->free_count++;
            stats->total_free += bsize;
            if (bsize > stats->largest_free)
                stats->largest_free = bsize;
        } else {
            stats->total_used += bsize;
        }

        block = block_next(block);
    }

    /* Account for sentinel block overhead */
    stats->overhead += BLOCK_OVERHEAD;

    return 0;
}
