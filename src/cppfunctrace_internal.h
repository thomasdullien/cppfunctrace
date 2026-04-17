/*
 * cppfunctrace — high-performance function tracer for C/C++ code built with
 * -finstrument-functions. Produces .ftrc files binary-compatible with
 * pyfasttrace (format v3), so all existing tooling can be reused.
 */

#ifndef CPPFUNCTRACE_INTERNAL_H
#define CPPFUNCTRACE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Binary event format (12 bytes) — MUST MATCH fasttracer.h ──────── */

#define FT_FLAG_EXIT        (1 << 7)
#define FT_FLAG_C_FUNCTION  (1 << 0)   /* set for all C/C++ events        */
#define FT_FLAG_SYNTHETIC   (1 << 1)

#define FT_MAX_FUNC_ID      16777215

struct __attribute__((packed)) BinaryEvent {
    uint32_t ts_delta_us;
    uint32_t func_id;
    uint8_t  tid_idx;
    uint8_t  flags;
    uint16_t _pad;
};

_Static_assert(sizeof(struct BinaryEvent) == 12, "BinaryEvent must be 12 bytes");

/* ── Buffer header (v3) ────────────────────────────────────────────── */

#define FT_MAGIC            0x43525446u   /* "FTRC" little-endian           */
#define FT_VERSION          3u
#define FT_MAX_THREADS      256
#define FT_THREAD_NAME_LEN  64

struct __attribute__((packed)) BufferHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t num_strings;
    int64_t  base_ts_ns;
    uint32_t num_events;
    uint8_t  num_threads;
    uint8_t  _pad1;
    uint16_t _pad2;
    uint64_t thread_table[FT_MAX_THREADS];
    char     thread_names[FT_MAX_THREADS][FT_THREAD_NAME_LEN];
    char     process_name[FT_THREAD_NAME_LEN];
    uint32_t string_table_offset;
    uint32_t events_offset;
};

/* ── Intern table (function pointer → uint32 func_id) ──────────────── */
/*
 * Lock-free reads, mutex-guarded writes. Entries are append-only —
 * never mutated after insert. A reader loading key with acquire order
 * either sees NULL (not inserted yet) or a valid (key, func_id) pair.
 */
struct InternEntry {
    _Atomic(void*) key;      /* function pointer; NULL = empty slot       */
    uint32_t       func_id;  /* 1-based                                   */
};

struct InternTable {
    struct InternEntry* entries;
    uint32_t capacity;       /* power of two                              */
    _Atomic uint32_t count;
};

/* Growable buffer of [uint32_t len][char bytes[len]] — matches the
 * on-disk string table format read by libftrc. */
struct StringTable {
    char*   data;
    size_t  len;
    size_t  cap;
};

/* ── Per-thread state (TLS, lazily initialised) ────────────────────── */

#define FT_MAX_STACK_DEPTH  512

struct ThreadStack {
    uint32_t func_ids[FT_MAX_STACK_DEPTH];
    int      depth;
    uint8_t  tid_idx;
    int      registered;     /* non-zero once thread is in thread_map     */
};

struct ThreadMapEntry {
    uint64_t os_tid;
    uint8_t  tid_idx;
    char     name[FT_THREAD_NAME_LEN];
    struct ThreadStack* stack;  /* NULL until the thread records an event */
};

struct ThreadMap {
    struct ThreadMapEntry entries[FT_MAX_THREADS];
    _Atomic uint16_t count;
};

/* ── Ring of flush buffers + writer thread ─────────────────────────── */

#define FT_NUM_BUFFERS 4

struct FlushRequest {
    int    buf_index;
    size_t bytes;
    char   path[512];
};

typedef struct {
    /* Ring of buffers */
    void*  buffers[FT_NUM_BUFFERS];
    size_t buffer_size;
    int    active_buf;
    _Atomic size_t write_offset;
    size_t events_start;        /* byte offset of first event inside buf  */

    int64_t base_ts_ns;
    pid_t   pid;
    char    process_name[FT_THREAD_NAME_LEN];

    /* Intern + string tables, guarded by cold_mutex on writes */
    struct InternTable  intern;
    struct StringTable  strings;
    pthread_mutex_t     cold_mutex;    /* serialises intern/string inserts */

    /* Thread bookkeeping */
    struct ThreadMap thread_map;

    /* Writer thread / flush queue */
    pthread_t   writer_thread;
    sem_t       flush_avail;
    sem_t       free_bufs;
    struct FlushRequest flush_queue[FT_NUM_BUFFERS];
    int         flush_head;
    int         flush_tail;
    int         writer_stop;
    int         writer_started;
    char*       output_dir;

    /* Rollover */
    size_t      rollover_threshold;
    size_t      cumulative_bytes;
    uint32_t    file_seq;

    /* Flush coordination: one flusher at a time */
    _Atomic int flush_in_progress;

    _Atomic int collecting;
    _Atomic int initialised;
} CppFuncTracer;

/* ── Intern API (intern.c) ─────────────────────────────────────────── */

int  ft_intern_init(struct InternTable* t, uint32_t initial_capacity);
void ft_intern_free(struct InternTable* t);

/* Lookup by function pointer. Returns func_id (>=1) or 0 if not found. */
uint32_t ft_intern_lookup(struct InternTable* t, void* key)
    __attribute__((no_instrument_function));

/* Insert (writer must hold cold_mutex). Returns 0 on success, -1 on OOM. */
int  ft_intern_insert_locked(struct InternTable* t, void* key, uint32_t func_id);

int  ft_string_table_init(struct StringTable* st);
void ft_string_table_free(struct StringTable* st);
int  ft_string_table_append(struct StringTable* st, const char* s, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CPPFUNCTRACE_INTERNAL_H */
