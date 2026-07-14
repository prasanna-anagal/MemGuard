/* Demo 4 — double free and wild free detection. */
#include <stdio.h>
#include "../src/memguard.h"

int main(void) {
    printf("=== MemGuard demo: double free / invalid free ===\n");

    int *p = mg_malloc(sizeof(int) * 4);
    mg_free(p);
    printf("freed p once — fine.\n");

    mg_free(p);       /* double free: with the system allocator this is
                         undefined behaviour; MemGuard reports it and
                         refuses to corrupt the heap */

    int on_the_stack = 42;
    mg_free(&on_the_stack);   /* wild free: not a heap pointer at all */

    mg_stats_t s;
    mg_get_stats(&s);
    printf("\nMemGuard survived both bugs and logged %zu error(s).\n",
           s.errors_detected);
    return 0;
}
