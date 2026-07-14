/* Demo 1 — basic use: allocate, use, free, and inspect the heap. */
#include <stdio.h>
#include <string.h>
#include "../src/memguard.h"

int main(void) {
    printf("=== MemGuard demo: basic allocation ===\n");

    char *name = mg_malloc(32);
    int  *nums = mg_calloc(10, sizeof(int));

    strcpy(name, "Toshiba");
    for (int i = 0; i < 10; i++) nums[i] = i * i;

    printf("name = %s, nums[9] = %d\n", name, nums[9]);

    /* grow the array — realloc keeps the old contents */
    nums = mg_realloc(nums, 20 * sizeof(int));
    printf("after realloc, nums[9] is still %d\n", nums[9]);

    mg_print_stats();
    mg_heap_map();

    mg_free(name);
    mg_free(nums);

    printf("\nafter freeing everything:\n");
    mg_print_stats();
    mg_report_leaks();
    return 0;
}
