/* Demo 3 — buffer overflow detection: write past the end of an
 * allocation and MemGuard's rear canary catches it at free time. */
#include <stdio.h>
#include <string.h>
#include "../src/memguard.h"

int main(void) {
    printf("=== MemGuard demo: heap buffer overflow ===\n");

    char *buf = mg_malloc(16);

    /* 25 bytes into a 16-byte buffer — a classic off-by-a-lot bug.
     * With the system malloc this silently corrupts the heap and
     * crashes somewhere far away, much later. */
    strcpy(buf, "this string is too long!");

    printf("wrote 25 bytes into a 16-byte buffer...\n");

    /* Option A: a heap sweep finds it immediately */
    int problems = mg_check_all();
    printf("mg_check_all() found %d problem(s)\n", problems);

    /* Option B: the free itself also reports it */
    mg_free(buf);

    printf("\nMemGuard caught the overflow AND reported which "
           "allocation was overflowed.\n");
    return 0;
}
