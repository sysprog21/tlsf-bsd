/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

/* Optional C++17 adapter exposing the allocator through
 * 'std::pmr::memory_resource', the interface most modern C++ consumers accept.
 * Nothing else in the project includes this header, so the library keeps its
 * C11 baseline; only a translation unit that includes it needs C++17.
 *
 * The thread-safe wrapper has its own adapter in tlsf_thread_pmr.hpp, split off
 * for the reason tlsf.h and tlsf_thread.h are two files: including the thread
 * header drags in '<pthread.h>' or '<windows.h>', and on a target with no lock
 * backend it does not compile at all until the caller defines TLSF_LOCK_T. A
 * single-threaded consumer of this adapter should not have to pay either.
 *
 * The adapter is a non-owning view over a pool the caller already built. The
 * pool, the allocator instance and the resource must all outlive every
 * allocation made through them, because a 'std::pmr' container holds the
 * resource pointer and calls back into it at destruction.
 *
 * It adds no locking, so it is unsynchronized exactly as the C allocator is.
 *
 * Exceptions are optional. Where they are off, exhaustion terminates rather
 * than throwing, for the reason '_TLSF_PMR_EXCEPTIONS' gives below. The two
 * modes must not be mixed in one binary: 'do_allocate()' is an inline virtual,
 * so its two bodies mangle the same and the linker keeps whichever copy it saw
 * first. That is not a hole these adapters open, since the standard library's
 * own headers are conditioned on the same macro, but a caller linking a
 * '-fno-exceptions' object into an exception-enabled program should know
 * exhaustion may terminate it.
 */

/* MSVC leaves '__cplusplus' at 199711L unless '/Zc:__cplusplus' is passed, and
 * '/std:c++17' does not imply it. Reading that macro alone would reject a
 * compiler that supports everything below, on the flag a user reaches first.
 * '_MSVC_LANG' always carries the real value, and only a compiler in MSVC's
 * compatibility mode defines it at all, clang-cl included, where it is the
 * value to read for the same reason. Testing it first costs nothing elsewhere.
 */
#if defined(_MSVC_LANG)
#define _TLSF_PMR_LANG _MSVC_LANG
#elif defined(__cplusplus)
#define _TLSF_PMR_LANG __cplusplus
#else
#define _TLSF_PMR_LANG 0L
#endif

#if _TLSF_PMR_LANG < 201703L
#error tlsf_pmr.hpp needs C++17. The library itself stays C11 and C++11.
#endif

#undef _TLSF_PMR_LANG

/* Exceptions are not required. 'do_allocate()' may not return null, so with
 * them off the only conforming outcome left is not to return at all:
 * std::terminate(), whose default handler aborts and which std::set_terminate()
 * lets a caller redirect. That is a process kill, not an error report, so a
 * caller for which exhaustion is a recoverable condition should reach
 * tlsf_aalloc() directly rather than through PMR.
 *
 * Written once here because tlsf_thread_pmr.hpp and tests/test_pmr.cpp need the
 * same answer and two spellings of one feature test drift apart. Left defined,
 * unlike '_TLSF_PMR_LANG' above, for exactly that reason.
 */
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define _TLSF_PMR_EXCEPTIONS 1
#else
#define _TLSF_PMR_EXCEPTIONS 0
#endif

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <new>

#include "tlsf.h"

/* The namespace name, spelled through a macro because clang-format rewrites the
 * closing comment of a namespace whose name is a macro invocation and then
 * rejects what it wrote, so the file never converges under the formatter. A
 * plain identifier is stable. Reusing '_TLSF_ABI' rather than restating the
 * knob list is deliberate: a knob added to tlsf.h reaches this file for free.
 */
#define _TLSF_PMR_NS _TLSF_ABI(abi)

namespace tlsf
{

/* What a full pool does, named once so the two adapters cannot answer it
 * differently. Both call it and neither spells the branch itself.
 *
 * Outside the configuration-tagged namespace below on purpose: the exceptions
 * mode is not a TLSF configuration knob, and this body is byte-identical under
 * every one of them, so tagging it would suggest a dependency that is not
 * there.
 */
[[noreturn]] inline void pmr_exhausted()
{
#if _TLSF_PMR_EXCEPTIONS
    throw std::bad_alloc();
#else
    std::terminate();
#endif
}

/* The inline namespace below carries the same configuration suffix the C
 * headers paste onto their symbol names, for the same reason and against a hole
 * the C++ side would otherwise open in that guard.
 *
 * A class defined in a header has weak definitions for its vtable and its
 * inline virtuals, and their mangled names say nothing about the configuration
 * the translation unit was compiled with. Two units that disagree about, say,
 * TLSF_MAX_POOL_BITS emit the same mangled 'do_allocate' twice, and a linker
 * that discards one COMDAT group discards that copy's reference to the suffixed
 * C symbol along with it. The undefined reference that would have failed the
 * link disappears, and one unit's 'tlsf_t' is then read at the other's offsets.
 * Suffixing the namespace makes the two configurations' classes distinct types,
 * so neither definition can stand in for the other.
 *
 * Callers still write 'tlsf::pmr_resource'; an inline namespace is transparent
 * to name lookup. Only a hand-written forward declaration of the class would
 * notice, and that fails to compile rather than mislinking.
 *
 * tlsf_thread_pmr.hpp tags its own class with the wider thread suffix. The two
 * are separate because 'tlsf_thread_t' also moves with the arena count, the
 * cache line and the lock backend, and tagging 'pmr_resource' with those would
 * reject builds differing only in a knob it does not touch.
 */
inline namespace _TLSF_PMR_NS
{

/* Adapter over a single 'tlsf_t'. Every allocation goes through tlsf_aalloc(),
 * since 'std::pmr' always states the alignment it needs and tlsf_malloc() only
 * promises the natural one.
 */
class pmr_resource final : public std::pmr::memory_resource
{
public:
    explicit pmr_resource(tlsf_t &pool) noexcept : pool_(&pool) {}

    pmr_resource(const pmr_resource &) = delete;
    pmr_resource &operator=(const pmr_resource &) = delete;

private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        void *p = tlsf_aalloc(pool_, alignment, bytes);
        if (!p)
            pmr_exhausted();
        return p;
    }

    /* The byte count and alignment are redundant here: TLSF stores the block
     * size in the header ahead of the payload, so it needs neither.
     */
    void do_deallocate(void *p, std::size_t, std::size_t) override
    {
        tlsf_free(pool_, p);
    }

    /* Object identity, the answer the standard's own pool resources give.
     *
     * Two 'pmr_resource' objects over one 'tlsf_t' are interchangeable in fact,
     * since tlsf_free() takes the pool and not the wrapper, so a strict reading
     * of do_is_equal() would have that pair answer true. Comparing pools means
     * recovering the other resource's type through a dynamic_cast, which the
     * standard's note on this function contemplates and which needs RTTI. These
     * headers compile under -fno-rtti, the mode a good deal of embedded C++
     * builds in, and the conservative answer costs only a container's chance to
     * steal a buffer on move assignment or swap rather than moving elements one
     * at a time. Nothing is freed through the wrong pool either way.
     */
    bool do_is_equal(
        const std::pmr::memory_resource &other) const noexcept override
    {
        return this == &other;
    }

    tlsf_t *pool_;
};

} /* namespace _TLSF_PMR_NS */

} /* namespace tlsf */
