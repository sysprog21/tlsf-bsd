/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Fuzz target: replay an input buffer as a stream of allocator operations.
 *
 * The randomized tests in test.c draw from rand() and so explore one shape of
 * history per seed. A coverage-guided fuzzer instead steers the operation
 * stream toward branches nothing has taken yet, which is what reaches the
 * split, merge and trim corners that only appear after a specific sequence of
 * sizes. tlsf_check() is the oracle: every invariant it knows about is
 * evaluated against whatever state the input produced.
 *
 * The payload of every live block carries a tag, verified before the block is
 * handed back. That catches the failure this allocator would otherwise hide,
 * two live allocations overlapping, which no heap invariant can see because
 * the metadata stays perfectly consistent. The tag is derived from the slot
 * index, so no two blocks that are live at the same moment can carry the same
 * one, which is the property the check rests on.
 *
 * Two entry points, one body. Built with -fsanitize=fuzzer the file provides
 * LLVMFuzzerTestOneInput and libFuzzer drives it; built any other way it gets a
 * main() that replays a deterministic pseudo-random corpus, so 'make check'
 * exercises the same code on every toolchain.
 */

#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlsf.h"

#include "pool_limits.h"

/* One eighth of the pool per request keeps several blocks live at once, which
 * is what makes coalescing and splitting reachable.
 */
#define FUZZ_POOL_BYTES TLSF_TEST_POOL_CLAMP(256 * 1024)
#define FUZZ_MAX_REQUEST (FUZZ_POOL_BYTES / 8)
#define FUZZ_SLOTS 32

/* Four bytes drive one operation: what to do, which slot to do it to, and a
 * 16-bit size. Anything shorter than one record ends the run.
 */
#define FUZZ_RECORD 4

static char fuzz_pool[FUZZ_POOL_BYTES];

typedef struct {
    void *ptr;
    size_t size;
    uint8_t tag;
} fuzz_slot_t;

static void fuzz_verify(const fuzz_slot_t *slot)
{
    const unsigned char *bytes = (const unsigned char *) slot->ptr;
    for (size_t i = 0; i < slot->size; i++)
        assert(bytes[i] == slot->tag);
}

/* Take ownership of a fresh block: stamp the payload, then record what was
 * stamped. The three fields have to move together or the next verify reads the
 * wrong length or the wrong tag, which is why no call site sets them by hand.
 */
static void fuzz_adopt(fuzz_slot_t *slot, void *p, size_t size, uint8_t tag)
{
    memset(p, tag, size);
    slot->ptr = p;
    slot->size = size;
    slot->tag = tag;
}

static void fuzz_release(tlsf_t *t, fuzz_slot_t *slot)
{
    if (!slot->ptr)
        return;
    fuzz_verify(slot);
    tlsf_free(t, slot->ptr);
    slot->ptr = NULL;
    slot->size = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    tlsf_t t;
    if (!tlsf_pool_init(&t, fuzz_pool, sizeof(fuzz_pool)))
        return 0;

    fuzz_slot_t slots[FUZZ_SLOTS];
    memset(slots, 0, sizeof(slots));

    for (size_t i = 0; i + FUZZ_RECORD <= size; i += FUZZ_RECORD) {
        unsigned index = data[i + 1] % FUZZ_SLOTS;
        fuzz_slot_t *slot = &slots[index];
        size_t raw = (size_t) data[i + 2] | ((size_t) data[i + 3] << 8);
        size_t want = raw % FUZZ_MAX_REQUEST + 1;

        /* One tag per slot, distinct from the poison patterns the allocator
         * writes, so a mismatch names the block that trampled this one.
         */
        uint8_t tag = (uint8_t) (index + 1);

        switch (data[i] % 6) {
        case 0:
        case 1: { /* allocate, twice as likely as any other operation */
            fuzz_release(&t, slot);
            void *p = tlsf_malloc(&t, want);
            if (!p)
                break;
            assert(tlsf_usable_size(p) >= want);
            fuzz_adopt(slot, p, want, tag);
            break;
        }
        case 2: /* free */
            fuzz_release(&t, slot);
            break;
        case 3: { /* resize, keeping whatever the old payload held */
            if (!slot->ptr) {
                fuzz_release(&t, slot);
                break;
            }
            fuzz_verify(slot);
            void *p = tlsf_realloc(&t, slot->ptr, want);
            if (!p) {
                /* A failed realloc must leave the original intact. */
                fuzz_verify(slot);
                break;
            }
            size_t kept = slot->size < want ? slot->size : want;
            const unsigned char *bytes = (const unsigned char *) p;
            for (size_t k = 0; k < kept; k++)
                assert(bytes[k] == slot->tag);
            fuzz_adopt(slot, p, want, tag);
            break;
        }
        case 4: { /* aligned allocate */
            fuzz_release(&t, slot);
            size_t align = (size_t) 1 << (index % 13);
            void *p = tlsf_aalloc(&t, align, want);
            if (!p)
                break;
            assert((size_t) p % align == 0);
            fuzz_adopt(slot, p, want, tag);
            break;
        }
        default: /* audit the heap mid-stream, not only at the end */
            tlsf_check(&t);
            break;
        }
    }

    for (size_t i = 0; i < FUZZ_SLOTS; i++)
        fuzz_release(&t, &slots[i]);

    tlsf_check(&t);

    /* An empty pool must report one free block holding everything, which is the
     * state tlsf_pool_init() built. Anything else means an operation stranded
     * memory that free could not reclaim.
     */
    tlsf_stats_t stats;
    assert(tlsf_get_stats(&t, &stats) == 0);
    assert(stats.total_used == 0);
    assert(stats.free_count == 1);

    return 0;
}

#ifndef TLSF_FUZZ_NO_MAIN
/* Deterministic stand-in for libFuzzer, so the target is exercised by the
 * ordinary test run on toolchains that have no libFuzzer. xorshift32 rather
 * than rand(), whose sequence varies between libc implementations: the same
 * seed has to replay the same operations everywhere for a failure here to be
 * reproducible at all. TLSF_FUZZ_SEED overrides the default, which is printed
 * either way.
 */
static uint32_t fuzz_rand(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

#define FUZZ_DRIVER_ROUNDS 512
#define FUZZ_DRIVER_MAX_INPUT 1024

int main(void)
{
    uint8_t input[FUZZ_DRIVER_MAX_INPUT];
    const char *seed_env = getenv("TLSF_FUZZ_SEED");
    uint32_t seed =
        seed_env ? (uint32_t) strtoul(seed_env, NULL, 0) : 0x9e3779b9u;
    uint32_t used = seed ? seed : 1u; /* xorshift32 cannot leave zero */
    uint32_t state = used;

    /* Before the rounds, not after. An assertion inside the target aborts the
     * process, and a seed printed at the end is a seed printed only when it
     * was not needed.
     */
    printf("Fuzz target replay: seed 0x%08x (set TLSF_FUZZ_SEED), ", used);
    fflush(stdout);

    for (unsigned round = 0; round < FUZZ_DRIVER_ROUNDS; round++) {
        size_t len = fuzz_rand(&state) % sizeof(input);
        for (size_t i = 0; i < len; i++)
            input[i] = (uint8_t) fuzz_rand(&state);
        LLVMFuzzerTestOneInput(input, len);
    }

    printf("%d rounds, pool %zu bytes, done\n", FUZZ_DRIVER_ROUNDS,
           (size_t) FUZZ_POOL_BYTES);
    return 0;
}
#endif
