/* MemGuard self-test suite.
 *
 * Plain-C test harness: each test is a function; TEST_ASSERT records
 * failures instead of aborting, so one broken test doesn't hide the
 * rest. Run `make test` (or bin\test_memguard.exe) — exit code 0
 * means everything passed.
 */
#include <stdio.h>
#include <string.h>
#include "../src/memguard.h"

static int g_failures = 0;
static int g_checks   = 0;

#define TEST_ASSERT(cond) do {                                        \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
        g_failures++;                                                 \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                 \
} while (0)

static void test_alloc_and_free(void) {
    printf("test_alloc_and_free\n");
    mg_reset();

    char *p = mg_malloc(100);
    TEST_ASSERT(p != NULL);
    memset(p, 0xAB, 100);          /* memory must be writable */

    mg_stats_t s;
    mg_get_stats(&s);
    TEST_ASSERT(s.bytes_in_use == 100);
    TEST_ASSERT(s.live_blocks == 1);

    mg_free(p);
    mg_get_stats(&s);
    TEST_ASSERT(s.bytes_in_use == 0);
    TEST_ASSERT(s.live_blocks == 0);
    TEST_ASSERT(s.errors_detected == 0);
}

static void test_alignment(void) {
    printf("test_alignment\n");
    mg_reset();
    for (int i = 1; i <= 5; i++) {
        void *p = mg_malloc((size_t)i * 7);   /* odd sizes on purpose */
        TEST_ASSERT(((size_t)p % 16) == 0);   /* payload 16-byte aligned */
    }
}

static void test_calloc_zeroes(void) {
    printf("test_calloc_zeroes\n");
    mg_reset();
    unsigned char *p = mg_calloc(64, 4);
    int all_zero = 1;
    for (int i = 0; i < 256; i++) if (p[i] != 0) all_zero = 0;
    TEST_ASSERT(all_zero);
    mg_free(p);
}

static void test_realloc_preserves_data(void) {
    printf("test_realloc_preserves_data\n");
    mg_reset();
    char *p = mg_malloc(16);
    strcpy(p, "keep me");
    p = mg_realloc(p, 4096);
    TEST_ASSERT(p != NULL);
    TEST_ASSERT(strcmp(p, "keep me") == 0);
    mg_free(p);
}

static void test_leak_detection(void) {
    printf("test_leak_detection\n");
    mg_reset();
    void *a = mg_malloc(10);
    void *b = mg_malloc(20);
    void *c = mg_malloc(30);
    mg_free(b);
    (void)a; (void)c;
    TEST_ASSERT(mg_report_leaks() == 2);
    mg_free(a);
    mg_free(c);
    TEST_ASSERT(mg_report_leaks() == 0);
}

static void test_double_free_detected(void) {
    printf("test_double_free_detected  (errors below are expected)\n");
    mg_reset();
    void *p = mg_malloc(8);
    mg_free(p);
    mg_free(p);                    /* the bug */

    mg_stats_t s;
    mg_get_stats(&s);
    TEST_ASSERT(s.errors_detected == 1);
    TEST_ASSERT(s.total_frees == 1);   /* second free was refused */
}

static void test_overflow_detected(void) {
    printf("test_overflow_detected  (errors below are expected)\n");
    mg_reset();
    char *p = mg_malloc(8);
    memset(p, 'x', 12);            /* 4 bytes past the end */
    TEST_ASSERT(mg_check_all() == 1);
}

static void test_invalid_free_detected(void) {
    printf("test_invalid_free_detected  (errors below are expected)\n");
    mg_reset();
    int stack_var = 7;
    mg_free(&stack_var);
    mg_stats_t s;
    mg_get_stats(&s);
    TEST_ASSERT(s.errors_detected == 1);
}

static void test_coalescing(void) {
    printf("test_coalescing\n");
    mg_reset();

    mg_stats_t before;
    mg_get_stats(&before);
    size_t pristine_largest = before.largest_free;

    void *a = mg_malloc(1000);
    void *b = mg_malloc(1000);
    void *c = mg_malloc(1000);

    /* free in an order that exercises both merge directions */
    mg_free(a);
    mg_free(c);
    mg_free(b);   /* b must merge with BOTH neighbours */

    mg_stats_t after;
    mg_get_stats(&after);
    TEST_ASSERT(after.free_blocks == 1);
    TEST_ASSERT(after.largest_free == pristine_largest);
}

static void test_fragmentation_then_recovery(void) {
    printf("test_fragmentation_then_recovery\n");
    mg_reset();

    /* 16 x 60 KB fills nearly the whole 1 MB pool, so the free space
     * afterwards exists only as the holes we punch, not as one big
     * untouched tail region */
    void *blocks[16];
    for (int i = 0; i < 16; i++) blocks[i] = mg_malloc(60 * 1024);

    /* free alternating blocks -> lots of holes */
    for (int i = 0; i < 16; i += 2) mg_free(blocks[i]);

    /* total free is large, but no hole fits 300 KB */
    void *big = mg_malloc(300 * 1024);
    TEST_ASSERT(big == NULL);

    for (int i = 1; i < 16; i += 2) mg_free(blocks[i]);

    big = mg_malloc(300 * 1024);   /* coalesced -> now it fits */
    TEST_ASSERT(big != NULL);
    mg_free(big);
}

static void test_best_fit_policy(void) {
    printf("test_best_fit_policy\n");
    mg_reset();

    /* carve the heap so there is a small hole and a big hole */
    void *small_hole = mg_malloc(64);
    void *wall1      = mg_malloc(64);
    void *big_hole   = mg_malloc(4096);
    void *wall2      = mg_malloc(64);
    mg_free(small_hole);
    mg_free(big_hole);

    /* best fit should place a 60-byte request in the SMALL hole,
     * i.e. exactly where small_hole used to be */
    mg_set_fit(MG_FIT_BEST);
    void *p = mg_malloc(60);
    TEST_ASSERT(p == small_hole);

    mg_free(p);
    mg_free(wall1);
    mg_free(wall2);
    mg_set_fit(MG_FIT_FIRST);
}

static void test_out_of_memory(void) {
    printf("test_out_of_memory  (error below is expected)\n");
    mg_reset();
    void *huge = mg_malloc(2u * 1024u * 1024u);   /* bigger than the pool */
    TEST_ASSERT(huge == NULL);
    mg_stats_t s;
    mg_get_stats(&s);
    TEST_ASSERT(s.failed_allocs == 1);
}

int main(void) {
    printf("==== MemGuard self-tests ====\n\n");

    test_alloc_and_free();
    test_alignment();
    test_calloc_zeroes();
    test_realloc_preserves_data();
    test_leak_detection();
    test_double_free_detected();
    test_overflow_detected();
    test_invalid_free_detected();
    test_coalescing();
    test_fragmentation_then_recovery();
    test_best_fit_policy();
    test_out_of_memory();

    printf("\n==== %d checks, %d failure(s) ====\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
