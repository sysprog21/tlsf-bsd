/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Pool sizing limits derived from the build configuration.
 *
 * A dynamic arena can never exceed 2^_TLSF_FL_MAX bytes, and tlsf_pool_init()
 * rejects a static pool whose single initial free block would exceed
 * BLOCK_SIZE_MAX (2^(_TLSF_FL_MAX - 1)). The default build puts these limits
 * far above anything the tests ask for, but -DTLSF_MAX_POOL_BITS=N brings them
 * down sharply. Tests size their pools through the macros here so a reduced
 * TLSF_MAX_POOL_BITS is actually exercised instead of tripping an assertion.
 */

#pragma once

#include <stddef.h>

#include "tlsf.h"

/* The suite has been validated down to TLSF_MAX_POOL_BITS=18. Below that,
 * fixtures in fragmentation_test() and elsewhere still carry sizes derived from
 * the default configuration and start failing on allocation. The library itself
 * supports smaller values; only these tests do not. Fail at compile time with
 * an explanation rather than mid-run on an opaque assertion.
 */
#if _TLSF_FL_MAX < 18
#error \
    "tests require TLSF_MAX_POOL_BITS >= 18 (more fixtures need parameterizing below that)"
#endif

/* Largest pool size the current configuration comfortably accepts. The factor
 * of four below 2^_TLSF_FL_MAX leaves headroom for block headers, the sentinel,
 * and the second live block that some tests hold.
 */
#define TLSF_TEST_POOL_MAX ((size_t) 1 << (_TLSF_FL_MAX - 2))

/* Per-allocation cost: the minimum block payload plus its header. Mirrors
 * BLOCK_SIZE_MIN + BLOCK_OVERHEAD in src/tlsf.c, which tests cannot see.
 */
#define TLSF_TEST_BLOCK_COST \
    (sizeof(struct tlsf_block) - sizeof(struct tlsf_block *) + sizeof(size_t))

/* The minimum remainder tlsf leaves when it trims a block, mirroring the
 * library default at the top of src/tlsf.c. The macro is internal there, so a
 * fixture that has to know whether a trim is possible at all must recompute it.
 * An override reaches both translation units through CPPFLAGS, so honor it when
 * present rather than assuming the default.
 */
#ifdef TLSF_SPLIT_THRESHOLD
#define TLSF_TEST_SPLIT_THRESHOLD ((size_t) (TLSF_SPLIT_THRESHOLD))
#else
#define TLSF_TEST_SPLIT_THRESHOLD \
    (sizeof(struct tlsf_block) - sizeof(struct tlsf_block *))
#endif

/* Ceiling on what a fixture may ask the host for in one go. Several tests are
 * only meaningful near a configuration limit that scales with
 * TLSF_MAX_POOL_BITS, which at the default is far past any allocation that
 * would succeed. They compare their requirement against this and skip with a
 * reason instead of failing, so a default build stays quiet and a reduced-
 * ceiling build actually exercises them.
 */
#define TLSF_TEST_BACKABLE_MAX ((size_t) 64 << 20)

/* Largest block a single allocation can ever occupy: the biggest request plus
 * its header. src/tlsf.c static-asserts this equals BLOCK_SIZE_MAX. A free
 * block may legitimately exceed it after coalescing, which is exactly what the
 * fixtures that use this constant are there to pin down.
 */
#define TLSF_TEST_ALLOC_BOUND (TLSF_MAX_SIZE + sizeof(size_t))

/* Clamp a desired pool size to what this configuration accepts. Usable as an
 * array bound: both operands are integer constant expressions.
 */
#define TLSF_TEST_POOL_CLAMP(want)                          \
    ((size_t) (want) < TLSF_TEST_POOL_MAX ? (size_t) (want) \
                                          : TLSF_TEST_POOL_MAX)
