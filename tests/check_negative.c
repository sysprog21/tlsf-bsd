/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Negative-path coverage for tlsf_check().
 *
 * Every other call of it in the suite expects it to pass, which proves only
 * that it accepts valid states: gutting the body to '(void) t;' left both
 * tests/test.c and tests/test_thread.c green. One case per walk function, so
 * dropping any of the three fails here.
 *
 *   case 0  null allocator              tlsf_check() itself
 *   case 1  arena size disagrees        check_block_chain()
 *   case 2  bitmap bit claims an
 *           empty bin                   check_free_lists()
 *   case 3  free-list back link points
 *           at the wrong block          check_bin_list()
 *
 * CHECK() aborts, so each case needs its own process; the Makefile runner
 * drives them by index and asserts both the abort and the diagnostic. Case -1
 * corrupts nothing and must exit cleanly, or a harness broken for its own
 * reasons would look like success on every case.
 */

#include <stdio.h>
#include <stdlib.h>

#include "tlsf.h"

#include "pool_limits.h"

/* Without TLSF_ENABLE_CHECK there is no rejection to observe, so report no
 * cases and let the runner skip rather than pass vacuously.
 */
#ifdef TLSF_ENABLE_CHECK
#define CASE_COUNT 4
#else
#define CASE_COUNT 0
#endif

static char pool[TLSF_TEST_POOL_CLAMP(64 * 1024)];

/* Occupy several bins, so the bitmap and free-list cases have targets. */
static void seed(tlsf_t *t)
{
    void *live[8], *dead[8];

    for (size_t i = 0; i < 8; i++) {
        live[i] = tlsf_malloc(t, 32 + i * 64);
        dead[i] = tlsf_malloc(t, 48 + i * 96);
        if (!live[i] || !dead[i]) {
            fprintf(stderr, "check_negative: seed allocation failed\n");
            exit(2);
        }
    }

    /* Allocated alternately, so freeing one array leaves holes the survivors
     * keep from merging.
     */
    for (size_t i = 0; i < 8; i++)
        tlsf_free(t, dead[i]);

    tlsf_check(t); /* the fixture itself must be valid */
}

/* Lowest first-level index with a clear bit, else _TLSF_FL_COUNT. */
static uint32_t first_clear_fl(const tlsf_t *t)
{
    uint32_t i = 0;
    while (i < _TLSF_FL_COUNT && (t->fl & (1U << i)))
        i++;
    return i;
}

/* A block sitting on some free list, or NULL if the fixture left none. */
static struct tlsf_block *any_free_block(tlsf_t *t)
{
    for (uint32_t i = 0; i < _TLSF_FL_COUNT; i++) {
        for (uint32_t j = 0; j < _TLSF_SL_COUNT; j++) {
            if (t->block[i][j] != &t->block_null)
                return t->block[i][j];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    /* No case index: report how many there are through the exit status, which
     * the shell already guarantees is a small integer, so the runner needs no
     * parsing and no validation of what it parsed.
     */
    if (argc != 2)
        return CASE_COUNT;

    char *end;
    const long which = strtol(argv[1], &end, 10);
    if (*end || end == argv[1]) {
        fprintf(stderr, "check_negative: not a case number: %s\n", argv[1]);
        return 2;
    }

    /* Needs no pool, so it runs before one exists. */
    if (which == 0) {
        tlsf_check(NULL);
        fprintf(stderr, "check_negative: case 0 returned\n");
        return 1;
    }

    tlsf_t t;
    if (!tlsf_pool_init(&t, pool, sizeof(pool))) {
        fprintf(stderr, "check_negative: pool init failed\n");
        return 2;
    }
    seed(&t);

    switch (which) {
    case -1: /* control: seed() already checked the untouched fixture */
        return 0;

    case 1:
        /* One alignment unit breaks the size reconciliation and leaves every
         * pointer valid, so the walk reaches it.
         */
        t.size += sizeof(size_t);
        break;

    case 2: {
        /* A first-level bit over an empty second-level bitmap. Bins untouched,
         * so nothing new is dereferenced.
         */
        uint32_t fl = first_clear_fl(&t);
        if (fl >= _TLSF_FL_COUNT) {
            fprintf(stderr, "check_negative: no clear FL bit to set\n");
            return 2;
        }
        t.fl |= 1U << fl;
        break;
    }

    case 3: {
        /* A list head must point back at the sentinel. Point it at itself: a
         * valid address, wrong value.
         */
        struct tlsf_block *block = any_free_block(&t);
        if (!block) {
            fprintf(stderr, "check_negative: fixture left no free block\n");
            return 2;
        }
        block->prev_free = block;
        break;
    }

    default:
        fprintf(stderr, "check_negative: no such case: %s\n", argv[1]);
        return 2;
    }

    tlsf_check(&t);

    /* Reached only when tlsf_check() accepted a state it must reject. */
    fprintf(stderr, "check_negative: case %ld was not detected\n", which);
    return 1;
}
