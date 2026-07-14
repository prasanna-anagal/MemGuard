/* Demo 2 — leak detection: forget to free, and MemGuard names the
 * exact file and line where the leaked memory was allocated. */
#include <stdio.h>
#include <string.h>
#include "../src/memguard.h"

static char *make_greeting(const char *who) {
    char *buf = mg_malloc(64);              /* <-- this one gets leaked */
    snprintf(buf, 64, "hello, %s", who);
    return buf;
}

int main(void) {
    printf("=== MemGuard demo: leak detection ===\n");

    char *g1 = make_greeting("world");
    char *g2 = make_greeting("interviewer");
    int  *scratch = mg_malloc(256);         /* <-- also leaked */

    printf("%s / %s\n", g1, g2);
    (void)scratch;

    mg_free(g1);   /* we free only one of the three allocations */

    /* In a real program you would call this at exit. */
    int leaks = mg_report_leaks();
    printf("\nMemGuard found %d leak(s) and told us exactly where "
           "each was allocated.\n", leaks);
    return 0;
}
