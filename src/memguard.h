/*
 * MemGuard — an instrumented memory allocator for C.
 *
 * A from-scratch implementation of malloc/free/calloc/realloc over a
 * fixed memory pool, with the debugging instrumentation of tools like
 * Valgrind/AddressSanitizer built in:
 *
 *   - leak detection with the file:line that allocated each block
 *   - canary bytes that catch heap buffer overflows
 *   - double-free and invalid-free detection
 *   - fragmentation statistics and an ASCII heap map
 *
 * Usage: include this header and call mg_malloc/mg_free instead of
 * malloc/free. The macros capture __FILE__/__LINE__ automatically.
 */
#ifndef MEMGUARD_H
#define MEMGUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- allocation API (use the macros, not the _at functions) ---- */

void *mg_malloc_at (size_t size, const char *file, int line);
void *mg_calloc_at (size_t count, size_t size, const char *file, int line);
void *mg_realloc_at(void *ptr, size_t size, const char *file, int line);
void  mg_free_at   (void *ptr, const char *file, int line);

#define mg_malloc(size)       mg_malloc_at((size), __FILE__, __LINE__)
#define mg_calloc(n, size)    mg_calloc_at((n), (size), __FILE__, __LINE__)
#define mg_realloc(ptr, size) mg_realloc_at((ptr), (size), __FILE__, __LINE__)
#define mg_free(ptr)          mg_free_at((ptr), __FILE__, __LINE__)

/* ---- allocation policy ---- */

typedef enum {
    MG_FIT_FIRST = 0,   /* first free block that is big enough (default) */
    MG_FIT_BEST  = 1    /* smallest free block that is big enough        */
} mg_fit_t;

void mg_set_fit(mg_fit_t policy);

/* ---- diagnostics ---- */

typedef struct {
    size_t pool_size;          /* total heap size in bytes               */
    size_t bytes_in_use;       /* payload bytes currently allocated      */
    size_t peak_in_use;        /* high-water mark of bytes_in_use        */
    size_t total_allocs;       /* lifetime count of successful allocs    */
    size_t total_frees;        /* lifetime count of successful frees     */
    size_t live_blocks;        /* blocks currently allocated             */
    size_t free_blocks;        /* free blocks in the free structure      */
    size_t largest_free;       /* biggest single free payload available  */
    size_t total_free;         /* sum of all free payload bytes          */
    double fragmentation_pct;  /* 100 * (1 - largest_free / total_free)  */
    size_t errors_detected;    /* overflows, double frees, bad frees     */
    size_t failed_allocs;      /* out-of-memory returns                  */
} mg_stats_t;

/* Print every block still allocated (a leak report), with the file and
 * line that allocated it. Returns the number of leaked blocks. */
int  mg_report_leaks(void);

/* Fill a stats struct / print a formatted stats table. */
void mg_get_stats(mg_stats_t *out);
void mg_print_stats(void);

/* Print an ASCII map of the heap: which regions are used vs free. */
void mg_heap_map(void);

/* Sweep every block and verify its canaries and headers are intact.
 * Reports corruption to stderr. Returns the number of problems found. */
int  mg_check_all(void);

/* Reset the allocator to a pristine empty heap (used by tests). */
void mg_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMGUARD_H */
