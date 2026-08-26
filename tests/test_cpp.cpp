/* SPDX-License-Identifier: BSD-3-Clause */

/* Compiling this file is most of the test: the public headers have to parse as
 * C++, and the C objects have to link against a C++ translation unit. What runs
 * afterwards only confirms the headers agree with the library they were built
 * against, so it must not lean on assert(), which NDEBUG deletes along with any
 * side effect inside it.
 */

#include <stdio.h>

#include "tlsf.h"
#include "tlsf_thread.h"

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                  \
        }                                                              \
    } while (0)

/* TLSF_INIT_STATIC is a separate macro because MSVC's C mode needs its own
 * spelling for a static initializer. Everywhere else it aliases TLSF_INIT.
 *
 * 'constexpr' is what holds the C++ form to constant initialization. Namespace
 * scope alone proves nothing: C++ accepts dynamic initialization there without
 * a diagnostic, so 'static_instance' below would compile either way.
 */
constexpr tlsf_t constant_instance = TLSF_INIT_STATIC;
static tlsf_t static_instance = TLSF_INIT_STATIC;

static int core_test(void)
{
    alignas(16) unsigned char memory[16 * 1024];
    tlsf_t tlsf = TLSF_INIT;

    CHECK(tlsf.size == 0);
    CHECK(static_instance.size == 0);
    CHECK(constant_instance.size == 0);
    CHECK(tlsf_pool_init(&tlsf, memory, sizeof(memory)) > 0);

    void *ptr = tlsf_malloc(&tlsf, 64);
    CHECK(ptr);
    CHECK(tlsf_usable_size(ptr) >= 64);
    tlsf_free(&tlsf, ptr);
    tlsf_check(&tlsf);
    return 0;
}

static int thread_test(void)
{
    alignas(TLSF_CACHELINE_SIZE) unsigned char memory[64 * 1024];
    tlsf_thread_t tlsf;

    /* Nothing to release when this fails: tlsf_thread_init() destroys the locks
     * it had already created and zeroes the instance.
     */
    CHECK(tlsf_thread_init(&tlsf, memory, sizeof(memory)) > 0);

    void *ptr = tlsf_thread_malloc(&tlsf, 64);
    if (ptr) {
        tlsf_thread_free(&tlsf, ptr);
        tlsf_thread_check(&tlsf);
    }
    tlsf_thread_destroy(&tlsf);
    CHECK(ptr);
    return 0;
}

int main()
{
    /* Not '||': short-circuiting would skip thread_test() whenever
     * core_test() fails, hiding a second broken area behind the first.
     */
    int failed = core_test();
    failed |= thread_test();
    return failed;
}
