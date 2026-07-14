/* Demo 5 — fragmentation made visible.
 *
 * Allocate many blocks, free every second one, and look at the heap
 * map: plenty of free bytes in total, but scattered in small holes,
 * so a big allocation fails. Then free the rest and watch coalescing
 * merge the holes back into one big region.
 */
#include <stdio.h>
#include "../src/memguard.h"

#define N 32

int main(void) {
    void *blocks[N];

    printf("=== MemGuard demo: fragmentation & coalescing ===\n");

    printf("\n1) allocating %d blocks of 30 KB each (fills the 1 MB heap)...\n", N);
    for (int i = 0; i < N; i++) blocks[i] = mg_malloc(30 * 1024);
    mg_heap_map();

    printf("\n2) freeing every SECOND block (classic fragmentation)...\n");
    for (int i = 0; i < N; i += 2) { mg_free(blocks[i]); blocks[i] = NULL; }
    mg_heap_map();
    mg_print_stats();

    printf("\n3) trying to allocate one 100 KB block...\n");
    void *big = mg_malloc(100 * 1024);
    if (big == NULL)
        printf("   -> FAILED, even though total free space is much "
               "larger than 100 KB.\n"
               "   That is external fragmentation: the space exists "
               "but no single hole is big enough.\n");
    else
        mg_free(big);

    printf("\n4) freeing the remaining blocks — coalescing merges "
           "neighbours...\n");
    for (int i = 1; i < N; i += 2) mg_free(blocks[i]);
    mg_heap_map();
    mg_print_stats();

    printf("\n5) retrying the 100 KB allocation...\n");
    big = mg_malloc(100 * 1024);
    printf("   -> %s\n", big ? "SUCCESS — the holes were merged back "
                               "into one big free region." : "failed?!");
    mg_free(big);

    mg_report_leaks();
    return 0;
}
