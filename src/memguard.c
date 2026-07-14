/*
 * MemGuard implementation.
 *
 * Design overview
 * ---------------
 * The heap is one static byte array (MG_POOL_SIZE). Every block of
 * memory — allocated or free — starts with a header (mg_block_t). All
 * blocks are linked in one doubly-linked list ordered by address, so
 * two neighbours in the list are also physical neighbours in memory.
 * That makes coalescing trivial: when a block is freed, look at
 * list-prev and list-next; if either is free, merge.
 *
 * Layout of one block:
 *
 *   +------------------+--------------------------+-------------+
 *   | mg_block_t hdr   | payload (user's bytes)   | rear canary |
 *   +------------------+--------------------------+-------------+
 *   ^ header->magic tells us if the block is allocated or free
 *   ^ header ends with a front canary field
 *                                                  ^ 4 bytes of
 *                                                    0xDEADBEEF
 *
 * The user only ever sees a pointer to the payload. mg_free() walks
 * back sizeof(mg_block_t) bytes to find the header and validates it.
 *
 * A buffer overflow tramples the rear canary of its own block (or the
 * header of the next block) — both are detected. A buffer underflow
 * tramples the front canary. A double free finds MAGIC_FREE where
 * MAGIC_ALLOC should be. A wild free finds neither magic value.
 */
#include "memguard.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

#ifndef MG_POOL_SIZE
#define MG_POOL_SIZE (1024u * 1024u)   /* 1 MiB heap */
#endif

#define MG_ALIGN        16u            /* every payload is 16-byte aligned */
#define MG_MAGIC_ALLOC  0xA110CA7Eu    /* "ALLOCATE" — block is live       */
#define MG_MAGIC_FREE   0xF2EE0000u    /* "FREE"     — block is free       */
#define MG_CANARY       0xDEADBEEFu    /* guard value around payloads      */
#define MG_REAR_BYTES   4u             /* rear canary size in bytes        */

/* ------------------------------------------------------------------ */
/* internal types                                                      */
/* ------------------------------------------------------------------ */

typedef struct mg_block {
    uint32_t         magic;        /* MG_MAGIC_ALLOC or MG_MAGIC_FREE     */
    size_t           region;       /* total bytes this block owns,
                                      header + payload + canary + padding */
    size_t           size;         /* payload bytes the user asked for    */
    const char      *file;         /* allocation site: file name          */
    int              line;         /* allocation site: line number        */
    size_t           seq;          /* allocation sequence number          */
    struct mg_block *prev;         /* previous block by address           */
    struct mg_block *next;         /* next block by address               */
    uint32_t         front_canary; /* guards against underflow            */
} mg_block_t;

/* ------------------------------------------------------------------ */
/* allocator state                                                     */
/* ------------------------------------------------------------------ */

/* Header bytes reserved in front of every payload — sizeof(mg_block_t)
 * rounded up to MG_ALIGN so that payloads stay 16-byte aligned. */
#define MG_HDR (((sizeof(mg_block_t)) + MG_ALIGN - 1u) & ~(size_t)(MG_ALIGN - 1u))

/* This array IS the heap. It lives in the program's data segment. */
static uint8_t g_pool[MG_POOL_SIZE] __attribute__((aligned(16)));

static mg_block_t *g_head        = NULL;  /* first block in the pool  */
static int         g_initialized = 0;
static mg_fit_t    g_fit         = MG_FIT_FIRST;

static size_t g_bytes_in_use = 0, g_peak_in_use = 0;
static size_t g_total_allocs = 0, g_total_frees = 0;
static size_t g_errors = 0, g_failed_allocs = 0;
static size_t g_seq = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static size_t align_up(size_t n, size_t a) {
    return (n + a - 1) & ~(a - 1);
}

/* Total bytes a block needs to hold `payload` user bytes. */
static size_t region_for(size_t payload) {
    return align_up(MG_HDR + payload + MG_REAR_BYTES, MG_ALIGN);
}

/* Biggest payload a region of `region` bytes can serve. */
static size_t capacity_of(size_t region) {
    return region - MG_HDR - MG_REAR_BYTES;
}

static uint8_t *payload_of(mg_block_t *b) {
    return (uint8_t *)b + MG_HDR;
}

static mg_block_t *header_of(void *payload) {
    return (mg_block_t *)((uint8_t *)payload - MG_HDR);
}

/* The rear canary sits immediately after the user's last byte. It may
 * be unaligned, so it is written and read with memcpy. */
static void write_rear_canary(mg_block_t *b) {
    uint32_t c = MG_CANARY;
    memcpy(payload_of(b) + b->size, &c, MG_REAR_BYTES);
}

static int rear_canary_ok(const mg_block_t *b) {
    uint32_t c = 0;
    memcpy(&c, (const uint8_t *)b + MG_HDR + b->size, MG_REAR_BYTES);
    return c == MG_CANARY;
}

static void mg_init(void) {
    if (g_initialized) return;
    g_head = (mg_block_t *)g_pool;
    g_head->magic  = MG_MAGIC_FREE;
    g_head->region = MG_POOL_SIZE;
    g_head->size   = 0;
    g_head->file   = NULL;
    g_head->line   = 0;
    g_head->seq    = 0;
    g_head->prev   = NULL;
    g_head->next   = NULL;
    g_head->front_canary = MG_CANARY;
    g_initialized = 1;
}

static void report_error(const char *what, const void *ptr,
                         const char *file, int line,
                         const mg_block_t *b) {
    g_errors++;
    fprintf(stderr, "\n[MemGuard] *** %s ***\n", what);
    fprintf(stderr, "[MemGuard]   pointer  : %p\n", ptr);
    fprintf(stderr, "[MemGuard]   detected : %s:%d\n", file ? file : "?", line);
    if (b && (b->magic == MG_MAGIC_ALLOC || b->magic == MG_MAGIC_FREE) && b->file) {
        fprintf(stderr, "[MemGuard]   block    : %zu bytes, allocated at %s:%d (alloc #%zu)\n",
                b->size, b->file, b->line, b->seq);
    }
}

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

/* Find a free block whose region can serve `payload` bytes,
 * honouring the configured fit policy. */
static mg_block_t *find_free(size_t payload) {
    size_t need = region_for(payload);
    mg_block_t *best = NULL;

    for (mg_block_t *b = g_head; b != NULL; b = b->next) {
        if (b->magic != MG_MAGIC_FREE || b->region < need) continue;
        if (g_fit == MG_FIT_FIRST) return b;
        if (best == NULL || b->region < best->region) best = b;
    }
    return best;
}

/* If the chosen free block is much bigger than needed, split it in
 * two: the front part becomes the allocation, the tail stays free. */
static void split_block(mg_block_t *b, size_t payload) {
    size_t need = region_for(payload);
    size_t rest = b->region - need;

    /* Only split if the remainder can hold a header plus a minimally
     * useful payload; otherwise keep the whole region (internal
     * fragmentation, but no unusable slivers). */
    if (rest < region_for(MG_ALIGN)) return;

    mg_block_t *tail = (mg_block_t *)((uint8_t *)b + need);
    tail->magic  = MG_MAGIC_FREE;
    tail->region = rest;
    tail->size   = 0;
    tail->file   = NULL;
    tail->line   = 0;
    tail->seq    = 0;
    tail->front_canary = MG_CANARY;

    tail->prev = b;
    tail->next = b->next;
    if (b->next) b->next->prev = tail;
    b->next   = tail;
    b->region = need;
}

void *mg_malloc_at(size_t size, const char *file, int line) {
    mg_init();
    if (size == 0) size = 1;   /* keep every allocation addressable */

    mg_block_t *b = find_free(size);
    if (b == NULL) {
        g_failed_allocs++;
        fprintf(stderr, "[MemGuard] out of memory: %zu bytes requested at %s:%d\n",
                size, file, line);
        return NULL;
    }

    split_block(b, size);

    b->magic = MG_MAGIC_ALLOC;
    b->size  = size;
    b->file  = file;
    b->line  = line;
    b->seq   = ++g_seq;
    b->front_canary = MG_CANARY;
    write_rear_canary(b);

    g_total_allocs++;
    g_bytes_in_use += size;
    if (g_bytes_in_use > g_peak_in_use) g_peak_in_use = g_bytes_in_use;

    return payload_of(b);
}

void *mg_calloc_at(size_t count, size_t size, const char *file, int line) {
    /* guard against count*size overflowing size_t */
    if (size != 0 && count > (size_t)-1 / size) {
        g_failed_allocs++;
        fprintf(stderr, "[MemGuard] calloc overflow: %zu * %zu at %s:%d\n",
                count, size, file, line);
        return NULL;
    }
    size_t total = count * size;
    void *p = mg_malloc_at(total, file, line);
    if (p) memset(p, 0, total);
    return p;
}

/* ------------------------------------------------------------------ */
/* freeing                                                             */
/* ------------------------------------------------------------------ */

/* Validate that `ptr` really is a live MemGuard payload pointer.
 * Returns the header on success, NULL after reporting the problem. */
static mg_block_t *validate_for_free(void *ptr, const char *file, int line) {
    uint8_t *p = (uint8_t *)ptr;

    if (p < g_pool + MG_HDR || p >= g_pool + MG_POOL_SIZE) {
        report_error("invalid free: pointer is not from this heap",
                     ptr, file, line, NULL);
        return NULL;
    }

    mg_block_t *b = header_of(ptr);

    if (b->magic == MG_MAGIC_FREE) {
        report_error("DOUBLE FREE detected", ptr, file, line, b);
        return NULL;
    }
    if (b->magic != MG_MAGIC_ALLOC) {
        report_error("invalid free: pointer does not point to a block start",
                     ptr, file, line, NULL);
        return NULL;
    }
    if (b->front_canary != MG_CANARY) {
        report_error("HEAP UNDERFLOW detected (front canary destroyed)",
                     ptr, file, line, b);
        /* still free it — the header linkage is intact */
    }
    if (!rear_canary_ok(b)) {
        report_error("HEAP BUFFER OVERFLOW detected (rear canary destroyed)",
                     ptr, file, line, b);
    }
    return b;
}

/* Merge b with its next neighbour (which must be free). */
static void merge_with_next(mg_block_t *b) {
    mg_block_t *n = b->next;
    b->region += n->region;
    b->next = n->next;
    if (n->next) n->next->prev = b;
}

void mg_free_at(void *ptr, const char *file, int line) {
    mg_init();
    if (ptr == NULL) return;   /* free(NULL) is a no-op, like the standard */

    mg_block_t *b = validate_for_free(ptr, file, line);
    if (b == NULL) return;

    g_total_frees++;
    g_bytes_in_use -= b->size;

    b->magic = MG_MAGIC_FREE;
    /* b->size is left intact so a later double-free report can still
     * say how big the block was and where it came from */

    /* coalesce with physical neighbours to fight fragmentation */
    if (b->next && b->next->magic == MG_MAGIC_FREE) merge_with_next(b);
    if (b->prev && b->prev->magic == MG_MAGIC_FREE) merge_with_next(b->prev);
}

void *mg_realloc_at(void *ptr, size_t size, const char *file, int line) {
    mg_init();
    if (ptr == NULL) return mg_malloc_at(size, file, line);
    if (size == 0) { mg_free_at(ptr, file, line); return NULL; }

    mg_block_t *b = header_of(ptr);
    if (b->magic != MG_MAGIC_ALLOC) {
        report_error("realloc on invalid pointer", ptr, file, line, NULL);
        return NULL;
    }

    /* if the current region can already hold it, just adjust in place */
    if (capacity_of(b->region) >= size) {
        g_bytes_in_use += size - b->size;
        if (g_bytes_in_use > g_peak_in_use) g_peak_in_use = g_bytes_in_use;
        b->size = size;
        write_rear_canary(b);
        return ptr;
    }

    void *fresh = mg_malloc_at(size, file, line);
    if (fresh == NULL) return NULL;
    memcpy(fresh, ptr, b->size < size ? b->size : size);
    mg_free_at(ptr, file, line);
    return fresh;
}

/* ------------------------------------------------------------------ */
/* policy                                                              */
/* ------------------------------------------------------------------ */

void mg_set_fit(mg_fit_t policy) { g_fit = policy; }

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

int mg_report_leaks(void) {
    mg_init();
    int leaks = 0;
    size_t bytes = 0;

    printf("\n================ MemGuard leak report ================\n");
    for (mg_block_t *b = g_head; b != NULL; b = b->next) {
        if (b->magic != MG_MAGIC_ALLOC) continue;
        leaks++;
        bytes += b->size;
        printf("  LEAK #%d: %6zu bytes  allocated at %s:%d  (alloc #%zu)\n",
               leaks, b->size, b->file ? b->file : "?", b->line, b->seq);
    }
    if (leaks == 0)
        printf("  no leaks — every allocation was freed. \n");
    else
        printf("  TOTAL: %d leaked block(s), %zu bytes\n", leaks, bytes);
    printf("======================================================\n");
    return leaks;
}

void mg_get_stats(mg_stats_t *out) {
    mg_init();
    memset(out, 0, sizeof(*out));
    out->pool_size     = MG_POOL_SIZE;
    out->bytes_in_use  = g_bytes_in_use;
    out->peak_in_use   = g_peak_in_use;
    out->total_allocs  = g_total_allocs;
    out->total_frees   = g_total_frees;
    out->errors_detected = g_errors;
    out->failed_allocs = g_failed_allocs;

    for (mg_block_t *b = g_head; b != NULL; b = b->next) {
        if (b->magic == MG_MAGIC_ALLOC) {
            out->live_blocks++;
        } else {
            size_t cap = capacity_of(b->region);
            out->free_blocks++;
            out->total_free += cap;
            if (cap > out->largest_free) out->largest_free = cap;
        }
    }
    if (out->total_free > 0)
        out->fragmentation_pct =
            100.0 * (1.0 - (double)out->largest_free / (double)out->total_free);
}

void mg_print_stats(void) {
    mg_stats_t s;
    mg_get_stats(&s);
    printf("\n---------------- MemGuard heap stats ----------------\n");
    printf("  pool size        : %zu bytes\n", s.pool_size);
    printf("  in use           : %zu bytes across %zu block(s)\n",
           s.bytes_in_use, s.live_blocks);
    printf("  peak usage       : %zu bytes\n", s.peak_in_use);
    printf("  free             : %zu bytes across %zu block(s)\n",
           s.total_free, s.free_blocks);
    printf("  largest free     : %zu bytes\n", s.largest_free);
    printf("  fragmentation    : %.1f%%\n", s.fragmentation_pct);
    printf("  lifetime allocs  : %zu   frees: %zu   failed: %zu\n",
           s.total_allocs, s.total_frees, s.failed_allocs);
    printf("  errors detected  : %zu\n", s.errors_detected);
    printf("------------------------------------------------------\n");
}

void mg_heap_map(void) {
    mg_init();
    /* one character per MG_POOL_SIZE/64 bytes: # = used, . = free */
    const int WIDTH = 64;
    const size_t bytes_per_cell = MG_POOL_SIZE / WIDTH;

    char map[65];
    for (int i = 0; i < WIDTH; i++) map[i] = '.';
    map[WIDTH] = '\0';

    for (mg_block_t *b = g_head; b != NULL; b = b->next) {
        if (b->magic != MG_MAGIC_ALLOC) continue;
        size_t start = (size_t)((uint8_t *)b - g_pool);
        size_t end   = start + b->region;
        for (size_t off = start; off < end; off += bytes_per_cell) {
            size_t cell = off / bytes_per_cell;
            if (cell < (size_t)WIDTH) map[cell] = '#';
        }
    }
    printf("\nheap map  [# = allocated, . = free]  (%zu bytes per char)\n",
           bytes_per_cell);
    printf("  |%s|\n", map);
}

int mg_check_all(void) {
    mg_init();
    int problems = 0;
    for (mg_block_t *b = g_head; b != NULL; b = b->next) {
        if (b->magic == MG_MAGIC_FREE) continue;
        if (b->magic != MG_MAGIC_ALLOC) {
            report_error("heap corruption: block header destroyed",
                         b, "mg_check_all", 0, NULL);
            problems++;
            break;   /* linkage can no longer be trusted */
        }
        if (b->front_canary != MG_CANARY) {
            report_error("underflow found during heap check",
                         payload_of(b), "mg_check_all", 0, b);
            problems++;
        }
        if (!rear_canary_ok(b)) {
            report_error("overflow found during heap check",
                         payload_of(b), "mg_check_all", 0, b);
            problems++;
        }
    }
    return problems;
}

void mg_reset(void) {
    g_initialized = 0;
    g_bytes_in_use = g_peak_in_use = 0;
    g_total_allocs = g_total_frees = 0;
    g_errors = g_failed_allocs = 0;
    g_seq = 0;
    g_fit = MG_FIT_FIRST;
    memset(g_pool, 0, sizeof(g_pool));
    mg_init();
}
