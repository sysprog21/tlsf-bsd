/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * tlsf-bsd is freely redistributable under the BSD License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Negative-path coverage for tlsf_check().
 *
 * Every other call of it in the suite expects it to pass, which proves only
 * that it accepts valid states: gutting the body to '(void) t;' left both
 * tests/test.c and tests/test_thread.c green.
 *
 * Each case below corrupts one state so that exactly one CHECK() site in
 * 'src/tlsf.c' fires, and carries the diagnostic it must produce. Matching the
 * message is what makes the count meaningful: a corruption that trips some
 * earlier CHECK still aborts, so a runner that only looked for an abort would
 * report coverage this file does not have.
 *
 * The sites left without a case are unreachable rather than omitted. They are
 * named here with the reason:
 *
 *   block size not aligned         tautological. block_size() returns
 *                                  'header - header % ALIGN_SIZE', so what it
 *                                  returns is aligned whatever the header
 *                                  holds. This is also why a case cannot reach
 *                                  the alignment entries below it.
 *   block pointer not aligned      a block's address is the arena base plus
 *   payload not aligned            the sizes walked so far, all of them
 *                                  aligned by the line above, so an unaligned
 *                                  one needs an unaligned arena, which is
 *                                  rejected before the walk starts.
 *   free block below minimum size  phase 1 walks every block, including this
 *   free block has free            one, and rejects an undersized or
 *     predecessor                  uncoalesced block before phase 2 reaches
 *   free block has free successor  the bin lists.
 *   next block doesn't know this
 *     block is free
 *   free list next pointer         tautological. 'list_block' was assigned
 *     incorrect                    'list_prev->next_free' at the bottom of the
 *                                  previous iteration and nothing writes to
 *                                  the list in between, so the comparison
 *                                  cannot fail.
 *
 * CHECK() aborts, so each case needs its own process; the Makefile runner
 * drives them by index and asserts both the abort and the diagnostic. Case -1
 * corrupts nothing and must exit cleanly, or a harness broken for its own
 * reasons would look like success on every case.
 *
 * That abort also keeps these cases out of 'make coverage': gcov flushes its
 * counters at exit and abort() does not run that, so every CHECK failure edge
 * reads as never taken however many are exercised here. The count this program
 * prints is the measure; the coverage report is not.
 */

#include <stdio.h>
#include <stdlib.h>

#include "tlsf.h"

#include "pool_limits.h"

/* Mirror of the block encoding in 'src/tlsf.c'. The struct is public, the
 * constants that interpret its header word are not, so they are restated here
 * and checked against a live pool by mirror_selfcheck() before any case runs. A
 * silently wrong mirror would corrupt something other than what the case names,
 * which is the one way this file could report coverage it lacks.
 */
#define BLK_FREE ((size_t) 1)
#define BLK_PREV_FREE ((size_t) 2)
#define BLK_BITS (BLK_FREE | BLK_PREV_FREE)
#define BLK_OVERHEAD (sizeof(size_t))
#define BLK_ALIGN (sizeof(size_t))
#define BLK_SIZE_MIN (sizeof(struct tlsf_block) - sizeof(struct tlsf_block *))

/* The size occupies everything above the alignment shift, which is a wider
 * field than the status bits sit in. That gap is why a misaligned block size
 * cannot be expressed at all.
 */
static size_t blk_size(const struct tlsf_block *b)
{
    return b->header - b->header % BLK_ALIGN;
}

static bool blk_is_free(const struct tlsf_block *b)
{
    return (b->header & BLK_FREE) != 0;
}

static bool blk_is_prev_free(const struct tlsf_block *b)
{
    return (b->header & BLK_PREV_FREE) != 0;
}

static struct tlsf_block *blk_next(struct tlsf_block *b)
{
    char *payload = (char *) b + sizeof(struct tlsf_block *) + BLK_OVERHEAD;
    return (struct tlsf_block *) (payload + blk_size(b) - BLK_OVERHEAD);
}

/* The first block starts one header word below the arena: a block's 'prev'
 * field precedes its header, and for the first block that field lies outside
 * the arena.
 */
static struct tlsf_block *blk_first(const tlsf_t *t)
{
    return (struct tlsf_block *) ((char *) t->arena - BLK_OVERHEAD);
}

static void die(const char *what)
{
    fprintf(stderr, "check_negative: %s\n", what);
    exit(2);
}

/* Walk to the zero-size block that terminates the chain. */
static struct tlsf_block *blk_sentinel(const tlsf_t *t)
{
    struct tlsf_block *b = blk_first(t);
    while (blk_size(b))
        b = blk_next(b);
    return b;
}

/* A free block whose physical successor is a real block rather than the
 * sentinel, so a case can corrupt the pair.
 */
static struct tlsf_block *blk_free_with_successor(const tlsf_t *t)
{
    struct tlsf_block *b = blk_first(t);
    while (blk_size(b)) {
        struct tlsf_block *next = blk_next(b);
        if (blk_is_free(b) && blk_size(next))
            return b;
        b = blk_next(b);
    }
    die("fixture has no free block with a successor");
    return NULL;
}

/* Lowest first-level index with a clear bit. */
static uint32_t first_clear_fl(const tlsf_t *t)
{
    uint32_t i = 0;
    while (i < _TLSF_FL_COUNT && (t->fl & (1U << i)))
        i++;
    if (i >= _TLSF_FL_COUNT)
        die("no clear FL bit to set");
    return i;
}

/* The list head of the 'n'th non-empty bin. Returned by address because most
 * cases rewrite the head rather than read it.
 */
static struct tlsf_block **nth_bin_head(tlsf_t *t, size_t n)
{
    for (uint32_t i = 0; i < _TLSF_FL_COUNT; i++) {
        for (uint32_t j = 0; j < _TLSF_SL_COUNT; j++) {
            if (t->block[i][j] != &t->block_null && !n--)
                return &t->block[i][j];
        }
    }
    die("fixture left too few non-empty bins");
    return NULL;
}

/* Non-null and not the free-list sentinel, which is all the bitmap cases below
 * need: tlsf_check() compares the pointer and aborts without following it.
 */
static struct tlsf_block decoy;

/* Occupy one bin several times over and leave a large tail, so the free-list
 * cases have both a multi-block bin and a second non-empty bin to work with.
 * The kept blocks vary in size only to spread the holes; the dropped ones are
 * all one size on purpose, because equal sizes land in one bin and unequal ones
 * would not.
 */
static void seed(tlsf_t *t)
{
    void *live[8], *dead[8];

    for (size_t i = 0; i < 8; i++) {
        live[i] = tlsf_malloc(t, 32 + i * 64);
        dead[i] = tlsf_malloc(t, 96);
        if (!live[i] || !dead[i])
            die("seed allocation failed");
    }

    /* Allocated alternately, so freeing one array leaves holes the survivors
     * keep from merging.
     */
    for (size_t i = 0; i < 8; i++)
        tlsf_free(t, dead[i]);

    tlsf_check(t); /* the fixture itself must be valid */
}

/* Confirm the header mirror above agrees with the allocator, so a case that
 * flips a bit flips the bit it names.
 */
static void mirror_selfcheck(const tlsf_t *t)
{
    size_t total = 0, seen_free = 0;
    struct tlsf_block *b = blk_first(t);

    while (blk_size(b)) {
        if (blk_size(b) < BLK_SIZE_MIN || blk_size(b) % BLK_ALIGN)
            die("mirror disagrees on block size encoding");

        /* Nothing but the status bits may live under the mask, or the flag
         * constants above name the wrong bits.
         */
        if (b->header % BLK_ALIGN > BLK_BITS)
            die("mirror disagrees on the status bits");
        seen_free += blk_is_free(b);
        total += blk_size(b) + BLK_OVERHEAD;
        b = blk_next(b);
    }
    total += BLK_OVERHEAD;

    /* Reaching the sentinel with the right total means the size mask, the
     * overhead and the payload offset are all right.
     */
    if (total != t->size)
        die("mirror disagrees on the block chain layout");
    if (!seen_free)
        die("mirror disagrees on the free bit");

    /* seed() left holes between live blocks, so a free block must have a used
     * predecessor whose successor bit says so.
     */
    b = blk_free_with_successor(t);
    if (!blk_is_prev_free(blk_next(b)))
        die("mirror disagrees on the prev-free bit");
}

static void corrupt_null_allocator(tlsf_t *t)
{
    (void) t;
    tlsf_check(NULL);
}

static void corrupt_arena_null(tlsf_t *t)
{
    t->arena = NULL;
}

static void corrupt_arena_unaligned(tlsf_t *t)
{
    t->arena = (char *) t->arena + 1;
}

static void corrupt_block_undersized(tlsf_t *t)
{
    struct tlsf_block *b = blk_first(t);
    b->header = (b->header & BLK_BITS) | BLK_ALIGN;
}

static void corrupt_block_oversized(tlsf_t *t)
{
    struct tlsf_block *b = blk_first(t);
    b->header = (b->header & BLK_BITS) | ((size_t) 1 << _TLSF_FL_MAX);
}

static void corrupt_prev_free_bit(tlsf_t *t)
{
    /* The first block has no predecessor to disagree with, so use the one after
     * it.
     */
    struct tlsf_block *b = blk_next(blk_first(t));
    b->header ^= BLK_PREV_FREE;
}

static void corrupt_prev_pointer(tlsf_t *t)
{
    /* Only consulted where the predecessor is free, so corrupt the successor of
     * a free block. Its own address is a valid pointer and the wrong value.
     */
    struct tlsf_block *b = blk_next(blk_free_with_successor(t));
    b->prev = b;
}

static void corrupt_consecutive_free(tlsf_t *t)
{
    struct tlsf_block *b = blk_next(blk_free_with_successor(t));
    b->header |= BLK_FREE;
}

static void corrupt_sentinel_free(tlsf_t *t)
{
    blk_sentinel(t)->header |= BLK_FREE;
}

static void corrupt_sentinel_prev_free_bit(tlsf_t *t)
{
    blk_sentinel(t)->header ^= BLK_PREV_FREE;
}

static void corrupt_sentinel_prev_pointer(tlsf_t *t)
{
    struct tlsf_block *s = blk_sentinel(t);
    if (!blk_is_prev_free(s))
        die("fixture does not end in a free block");
    s->prev = s;
}

static void corrupt_size_sum(tlsf_t *t)
{
    /* One alignment unit breaks the size reconciliation and leaves every
     * pointer valid, so the walk reaches it.
     */
    t->size += BLK_ALIGN;
}

static void corrupt_sl_without_fl(tlsf_t *t)
{
    t->sl[first_clear_fl(t)] = 1;
}

static void corrupt_bin_without_fl(tlsf_t *t)
{
    t->block[first_clear_fl(t)][0] = &decoy;
}

static void corrupt_fl_without_sl(tlsf_t *t)
{
    t->fl |= 1U << first_clear_fl(t);
}

static void corrupt_bin_without_sl(tlsf_t *t)
{
    for (uint32_t i = 0; i < _TLSF_FL_COUNT; i++) {
        if (!(t->fl & (1U << i)))
            continue;
        for (uint32_t j = 0; j < _TLSF_SL_COUNT; j++) {
            if (!(t->sl[i] & (1U << j))) {
                t->block[i][j] = &decoy;
                return;
            }
        }
    }
    die("no clear SL bit in an occupied first level");
}

static void corrupt_bin_empty_with_sl(tlsf_t *t)
{
    *nth_bin_head(t, 0) = &t->block_null;
}

static void corrupt_listed_block_not_free(tlsf_t *t)
{
    /* Clear the successor's prev-free bit too, or phase 1 rejects the pair
     * before phase 2 walks the bin.
     */
    struct tlsf_block *b = *nth_bin_head(t, 0);
    b->header &= ~BLK_FREE;
    blk_next(b)->header &= ~BLK_PREV_FREE;
}

static void corrupt_block_in_wrong_bin(tlsf_t *t)
{
    struct tlsf_block **a = nth_bin_head(t, 0);
    struct tlsf_block **b = nth_bin_head(t, 1);
    struct tlsf_block *tmp = *a;

    /* Two bins of different sizes, so each head now maps to the other bin. */
    *a = *b;
    *b = tmp;
}

static void corrupt_free_list_prev(tlsf_t *t)
{
    /* A list head must point back at the sentinel. Point it at itself: a valid
     * address, wrong value.
     */
    struct tlsf_block *b = *nth_bin_head(t, 0);
    b->prev_free = b;
}

static void corrupt_free_list_cycle(tlsf_t *t)
{
    struct tlsf_block *b = *nth_bin_head(t, 0);
    b->next_free = b;
}

static void corrupt_free_count(tlsf_t *t)
{
    /* Drop the head off its list without touching the physical chain, so the
     * two walks count different numbers of free blocks. seed() fills this bin
     * with more than one block, so it stays non-empty and its bitmap bits stay
     * honest.
     */
    struct tlsf_block **head = nth_bin_head(t, 0);
    struct tlsf_block *second = (*head)->next_free;

    if (second == &t->block_null)
        die("fixture bin holds only one block");
    second->prev_free = &t->block_null;
    *head = second;
}

/* Pairing the diagnostic with the corruption is what keeps the two in step: a
 * case whose corruption drifts onto another check fails on the message rather
 * than passing on the abort. Each string is the whole message, not a prefix of
 * it, because the runner anchors on the separator that follows: one CHECK
 * message is a suffix of another, so a prefix would match both.
 */
static const struct {
    const char *want;
    void (*corrupt)(tlsf_t *t);
} cases[] = {
    {"tlsf_t pointer is null", corrupt_null_allocator},
    {"failed to get arena pointer", corrupt_arena_null},
    {"arena not aligned", corrupt_arena_unaligned},
    {"block smaller than minimum size", corrupt_block_undersized},
    {"block exceeds mapping range", corrupt_block_oversized},
    {"prev_free bit mismatch with actual previous block state",
     corrupt_prev_free_bit},
    {"prev pointer doesn't match previous block", corrupt_prev_pointer},
    {"consecutive free blocks (coalescing failed)", corrupt_consecutive_free},
    {"sentinel marked as free", corrupt_sentinel_free},
    {"sentinel prev_free bit mismatch", corrupt_sentinel_prev_free_bit},
    {"sentinel prev pointer incorrect", corrupt_sentinel_prev_pointer},
    {"block sizes don't sum to pool size", corrupt_size_sum},
    {"SL bitmap non-zero but FL bit is clear", corrupt_sl_without_fl},
    {"block pointer not sentinel but FL bit is clear", corrupt_bin_without_fl},
    {"FL bit set but SL bitmap is empty", corrupt_fl_without_sl},
    {"block pointer not sentinel but SL bit is clear", corrupt_bin_without_sl},
    {"SL bit set but block list is empty (sentinel)",
     corrupt_bin_empty_with_sl},
    {"block in free list not free", corrupt_listed_block_not_free},
    {"block in wrong FL/SL bin", corrupt_block_in_wrong_bin},
    {"free list prev pointer incorrect", corrupt_free_list_prev},
    {"cycle in free list (duplicate block / double-free?)",
     corrupt_free_list_cycle},
    {"free block count mismatch between block walk and free list walk",
     corrupt_free_count},
};

/* Without TLSF_ENABLE_CHECK there is no rejection to observe, so offer no cases
 * and let the runner skip rather than pass vacuously.
 */
#ifdef TLSF_ENABLE_CHECK
#define CASE_COUNT ((int) (sizeof(cases) / sizeof(cases[0])))
#else
#define CASE_COUNT 0
#endif

static char pool[TLSF_TEST_POOL_CLAMP(64 * 1024)];

static long parse_case(const char *s)
{
    char *end;
    const long which = strtol(s, &end, 10);
    if (*end || end == s) {
        fprintf(stderr, "check_negative: not a case number: %s\n", s);
        exit(2);
    }
    return which;
}

int main(int argc, char **argv)
{
    /* No case index: report how many there are through the exit status, which
     * the shell already guarantees is a small integer, so the runner needs no
     * parsing and no validation of what it parsed.
     */
    if (argc == 1)
        return CASE_COUNT;

    /* The expected diagnostic goes out on its own, before the run that aborts,
     * because the runner has to know what to look for in output it has not
     * received yet.
     */
    if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'e' && !argv[1][2]) {
        const long which = parse_case(argv[2]);
        if (which < 0 || which >= CASE_COUNT) {
            fprintf(stderr, "check_negative: no such case: %s\n", argv[2]);
            return 2;
        }
        printf("%s\n", cases[which].want);
        return 0;
    }

    if (argc != 2) {
        fprintf(stderr, "check_negative: usage: %s [-e] [case]\n", argv[0]);
        return 2;
    }

    const long which = parse_case(argv[1]);
    if (which < -1 || which >= CASE_COUNT) {
        fprintf(stderr, "check_negative: no such case: %s\n", argv[1]);
        return 2;
    }

    tlsf_t t;
    if (!tlsf_pool_init(&t, pool, sizeof(pool)))
        die("pool init failed");
    seed(&t);
    mirror_selfcheck(&t);

    if (which == -1) /* control: seed() already checked the untouched fixture */
        return 0;

    cases[which].corrupt(&t);
    tlsf_check(&t);

    /* Reached only when tlsf_check() accepted a state it must reject. */
    fprintf(stderr, "check_negative: case %ld was not detected\n", which);
    return 1;
}
