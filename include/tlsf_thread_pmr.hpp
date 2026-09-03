/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#pragma once

/* Optional C++17 adapter exposing the thread-safe wrapper through
 * 'std::pmr::memory_resource'. Split from tlsf_pmr.hpp so that a consumer of
 * the single-threaded adapter does not pull in '<pthread.h>' or '<windows.h>',
 * and is not stopped outright on a target where tlsf_thread.h has no lock
 * backend to select. Including this header is the same opt-in as including
 * tlsf_thread.h, and needs the same TLSF_LOCK_T obligations.
 *
 * A non-owning view, like its sibling. The pool, the 'tlsf_thread_t' and the
 * resource must all outlive every allocation, because a 'std::pmr' container
 * holds the resource pointer and calls back into it at destruction.
 *
 * It adds no locking of its own. The per-arena locking, arena selection and
 * quiescence contracts are the ones tlsf_thread.h documents, and this wrapper
 * inherits them rather than layering a second policy on top. Per-arena locking
 * lowers contention; it is not a jitter-free guarantee. A hard latency bound
 * still needs a single-owner resource or a platform-specific lock bound.
 */

/* Depends on the core adapter the way tlsf_thread.h depends on tlsf.h, and for
 * the same reason: the wrapper is built on the allocator, not beside it. It
 * also means the C++17 language gate is written once, in the file both include
 * paths reach, instead of twice in two spellings that can drift apart.
 *
 * Ahead of the standard headers so that gate is what a pre-C++17 build reports.
 * A toolchain old enough to lack '<memory_resource>' entirely would otherwise
 * fail on a missing file, naming a header the reader never asked for instead of
 * the language level that is actually wrong.
 */
#include "tlsf_pmr.hpp"

#include <cstddef>
#include <memory_resource>

#include "tlsf_thread.h"

/* Spelled through a macro for the reason tlsf_pmr.hpp gives, and derived from
 * '_TLSF_THREAD_ABI' because 'tlsf_thread_t' moves with the arena count, the
 * cache line and the lock backend on top of the core knobs.
 */
#define _TLSF_PMR_THREAD_NS _TLSF_THREAD_ABI(abi)

namespace tlsf
{

/* Configuration-tagged for the reason tlsf_pmr.hpp sets out at length: a class
 * defined in a header has weak definitions whose mangled names carry no
 * configuration, which is a way around the suffixed C symbol names.
 */
inline namespace _TLSF_PMR_THREAD_NS
{

/* Adapter over a 'tlsf_thread_t'. Two hand-written classes rather than one
 * parameterized on the pool type, because the two must land in differently
 * suffixed inline namespaces and a template's instantiations mangle into the
 * namespace where the template is declared, not where it is used. A shared
 * template would put this class's vtable and inline virtuals under the core
 * suffix, leaving the guard resting on how a compiler spells a function-pointer
 * template argument. Thirty lines is the cheaper side of that trade.
 */
class pmr_thread_resource final : public std::pmr::memory_resource
{
public:
    explicit pmr_thread_resource(tlsf_thread_t &pool) noexcept : pool_(&pool) {}

    pmr_thread_resource(const pmr_thread_resource &) = delete;
    pmr_thread_resource &operator=(const pmr_thread_resource &) = delete;

private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        void *p = tlsf_thread_aalloc(pool_, alignment, bytes);
        if (!p)
            pmr_exhausted();
        return p;
    }

    void do_deallocate(void *p, std::size_t, std::size_t) override
    {
        tlsf_thread_free(pool_, p);
    }

    /* Object identity, with the trade-off tlsf_pmr.hpp sets out: two wrappers
     * over one 'tlsf_thread_t' are interchangeable in fact, and saying so would
     * take a dynamic_cast and therefore RTTI.
     */
    bool do_is_equal(
        const std::pmr::memory_resource &other) const noexcept override
    {
        return this == &other;
    }

    tlsf_thread_t *pool_;
};

} /* namespace _TLSF_PMR_THREAD_NS */

} /* namespace tlsf */
