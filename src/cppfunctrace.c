/*
 * cppfunctrace — core tracer. Hooks __cyg_profile_func_enter/exit that
 * GCC/clang emit for code compiled with -finstrument-functions, records
 * fixed-size binary events into mmap'd double-buffers, and flushes them
 * asynchronously via a writer thread.
 *
 * Binary output matches pyfasttrace's .ftrc v3 format so downstream
 * tooling (libftrc, ftrc2perfetto, perf-viz-merge) can be reused.
 *
 * Every function in this TU is annotated `no_instrument_function`, and
 * the whole library is compiled with -fno-instrument-functions for
 * defence in depth.
 */

#define _GNU_SOURCE
#include "cppfunctrace.h"
#include "cppfunctrace_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NO_INST __attribute__((no_instrument_function))

/* ── Global singleton ─────────────────────────────────────────────── */

static CppFuncTracer g_tracer;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

/* Thread-local state. Using GCC __thread (initial-exec model) for the
 * fastest TLS access; this requires the library to be loaded at program
 * start, which is guaranteed by -lcppfunctrace on the link line. */
static __thread struct ThreadStack tl_stack;
static __thread int tl_stack_init = 0;
static __thread int tl_in_hook = 0;        /* reentrancy guard */
static __thread pid_t tl_kernel_tid = 0;
static __thread uint32_t tl_call_count = 0;
static __thread int tl_name_stable = 0;   /* 1 once thread has its final name */

/* Module map — snapshot of loaded ELF objects keyed by VMA range.
 * Built once at init and refreshed on lookup miss (handles dlopen). */
struct ModInfo {
    uintptr_t base;
    uintptr_t end;
    char*     path;     /* heap-owned */
};

static struct ModInfo* g_modmap = NULL;
static size_t          g_modmap_count = 0;
static size_t          g_modmap_cap = 0;
static char            g_exe_path[PATH_MAX];  /* readlink /proc/self/exe */

static inline int64_t NO_INST ft_now_ns(void);
static inline int64_t
ft_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

static inline pid_t NO_INST ft_gettid(void);
static inline pid_t
ft_gettid(void)
{
    if (tl_kernel_tid == 0) {
        tl_kernel_tid = (pid_t)syscall(SYS_gettid);
    }
    return tl_kernel_tid;
}

/* ── Forward declarations ─────────────────────────────────────────── */

static void ft_init_once(void) NO_INST;
static void ft_real_init(void) NO_INST;
static int  ft_flush_buffer(int sync) NO_INST;
static void ft_make_output_path(char* buf, size_t buflen) NO_INST;
static uint32_t ft_intern_fp_slow(void* fn) NO_INST;
static uint8_t  ft_register_thread(void) NO_INST;
static void ft_resolve_symbol(void* fn, char* out, size_t outlen) NO_INST;
static void ft_capture_process_name(char* out, size_t outlen) NO_INST;
static void ft_capture_thread_name(char* out, size_t outlen) NO_INST;
static void* ft_writer_thread(void* arg) NO_INST;
static void ft_emergency_flush(void) NO_INST;
static void ft_atexit_handler(void) NO_INST;
static void ft_signal_handler(int signo, siginfo_t* info, void* ucontext) NO_INST;
static void ft_install_handlers(void) NO_INST;
static void ft_rebuild_modmap(void) NO_INST;

/* ── Initialisation ───────────────────────────────────────────────── */

static struct sigaction g_old_sigterm;
static struct sigaction g_old_sigabrt;
static struct sigaction g_old_sigsegv;
static struct sigaction g_old_sigint;

static void NO_INST
ft_make_output_path(char* buf, size_t buflen)
{
    if (g_tracer.rollover_threshold > 0) {
        snprintf(buf, buflen, "%s/%d_%04u.ftrc",
                 g_tracer.output_dir, (int)g_tracer.pid, g_tracer.file_seq);
    } else {
        snprintf(buf, buflen, "%s/%d.ftrc",
                 g_tracer.output_dir, (int)g_tracer.pid);
    }
}

static void NO_INST
ft_capture_process_name(char* out, size_t outlen)
{
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) { out[0] = 0; return; }
    ssize_t n = read(fd, out, outlen - 1);
    close(fd);
    if (n <= 0) { out[0] = 0; return; }
    out[n] = 0;
    /* Strip trailing newline */
    for (ssize_t i = n - 1; i >= 0 && (out[i] == '\n' || out[i] == '\r'); i--)
        out[i] = 0;
}

static void NO_INST
ft_capture_thread_name(char* out, size_t outlen)
{
    /* pthread_getname_np writes up to 16 bytes incl NUL on Linux; it's
     * safe to call on any thread. Fall back to /proc if it fails. */
    if (pthread_getname_np(pthread_self(), out, outlen) == 0 && out[0]) {
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int)ft_gettid());
    int fd = open(path, O_RDONLY);
    if (fd < 0) { out[0] = 0; return; }
    ssize_t n = read(fd, out, outlen - 1);
    close(fd);
    if (n <= 0) { out[0] = 0; return; }
    out[n] = 0;
    for (ssize_t i = n - 1; i >= 0 && (out[i] == '\n' || out[i] == '\r'); i--)
        out[i] = 0;
}

static void NO_INST
ft_real_init(void)
{
    /* Check CPPFUNCTRACE_DISABLE early. */
    const char* disable = getenv("CPPFUNCTRACE_DISABLE");
    if (disable && disable[0]) {
        atomic_store_explicit(&g_tracer.initialised, 1, memory_order_release);
        return;
    }

    const char* out_dir = getenv("CPPFUNCTRACE_OUTPUT_DIR");
    if (!out_dir || !out_dir[0]) out_dir = "/tmp/cppfunctrace";
    g_tracer.output_dir = strdup(out_dir);
    mkdir(g_tracer.output_dir, 0755);

    size_t buffer_size = 256 * 1024 * 1024;   /* 256 MiB */
    const char* env_buf = getenv("CPPFUNCTRACE_BUFFER_SIZE");
    if (env_buf && env_buf[0]) {
        long long v = atoll(env_buf);
        if (v >= (long long)(8 * 1024 * 1024)) buffer_size = (size_t)v;
    }

    size_t rollover = 0;
    const char* env_roll = getenv("CPPFUNCTRACE_ROLLOVER");
    if (env_roll && env_roll[0]) {
        long long v = atoll(env_roll);
        if (v > 0) rollover = (size_t)v;
    }

    /* Layout: header + string-table reserve + events. Reserve 32 MiB for
     * strings — enough for ~300k functions at ~100 chars each. */
    size_t st_reserve = 32 * 1024 * 1024;
    size_t events_start = sizeof(struct BufferHeader) + st_reserve;
    events_start = (events_start + 7) & ~(size_t)7;

    if (buffer_size < events_start + 1024) {
        buffer_size = events_start + 16 * 1024 * 1024;
    }

    g_tracer.buffer_size = buffer_size;
    g_tracer.events_start = events_start;
    g_tracer.rollover_threshold = rollover;
    g_tracer.cumulative_bytes = 0;
    g_tracer.file_seq = 0;
    g_tracer.active_buf = 0;
    atomic_store_explicit(&g_tracer.write_offset, events_start,
                          memory_order_relaxed);
    atomic_store_explicit(&g_tracer.flush_in_progress, 0,
                          memory_order_relaxed);

    for (int i = 0; i < FT_NUM_BUFFERS; i++) {
        g_tracer.buffers[i] = mmap(NULL, buffer_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (g_tracer.buffers[i] == MAP_FAILED) {
            fprintf(stderr, "cppfunctrace: mmap %zu bytes failed: %s\n",
                    buffer_size, strerror(errno));
            atomic_store_explicit(&g_tracer.initialised, 1,
                                  memory_order_release);
            return;
        }
    }

    g_tracer.base_ts_ns = ft_now_ns();
    g_tracer.pid = getpid();
    ft_capture_process_name(g_tracer.process_name,
                            sizeof(g_tracer.process_name));

    /* Intern table: default 1 M slots = 16 MiB. Covers LLVM, ffmpeg, etc. */
    uint32_t intern_cap = 1u << 20;
    const char* env_ic = getenv("CPPFUNCTRACE_INTERN_CAPACITY");
    if (env_ic && env_ic[0]) {
        long long v = atoll(env_ic);
        if (v >= 4096) intern_cap = (uint32_t)v;
    }
    if (ft_intern_init(&g_tracer.intern, intern_cap) < 0 ||
        ft_string_table_init(&g_tracer.strings) < 0) {
        fprintf(stderr, "cppfunctrace: intern/string init failed\n");
        atomic_store_explicit(&g_tracer.initialised, 1,
                              memory_order_release);
        return;
    }
    pthread_mutex_init(&g_tracer.cold_mutex, NULL);

    /* Writer thread */
    sem_init(&g_tracer.flush_avail, 0, 0);
    sem_init(&g_tracer.free_bufs, 0, FT_NUM_BUFFERS - 1);
    g_tracer.flush_head = 0;
    g_tracer.flush_tail = 0;
    g_tracer.writer_stop = 0;
    if (pthread_create(&g_tracer.writer_thread, NULL,
                       ft_writer_thread, NULL) == 0) {
        pthread_setname_np(g_tracer.writer_thread, "cppft-writer");
        g_tracer.writer_started = 1;
    } else {
        fprintf(stderr, "cppfunctrace: writer thread start failed\n");
    }

    /* Capture the main executable's real path — dl_iterate_phdr returns
     * "" for the executable and we want the actual filesystem path so
     * that ftrc2perfetto can open it to resolve symbols later. */
    ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
    if (n > 0) g_exe_path[n] = 0; else g_exe_path[0] = 0;

    /* Initial module snapshot. Will be rebuilt on demand if an address
     * fails to resolve (e.g. after a dlopen). */
    ft_rebuild_modmap();

    ft_install_handlers();
    atexit(ft_atexit_handler);

    atomic_store_explicit(&g_tracer.initialised, 1, memory_order_release);

    /* Respect CPPFUNCTRACE_DEFER — if set, user calls start() explicitly. */
    const char* defer = getenv("CPPFUNCTRACE_DEFER");
    if (!defer || !defer[0]) {
        atomic_store_explicit(&g_tracer.collecting, 1, memory_order_release);
    }
}

static void NO_INST
ft_init_once(void)
{
    ft_real_init();
}

/* ── Symbol resolution ────────────────────────────────────────────── */

/* Callback for dl_iterate_phdr: extend the module map. */
static int NO_INST
ft_dl_cb(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size; (void)data;

    /* Compute the VMA range this module occupies. */
    uintptr_t base = (uintptr_t)info->dlpi_addr;
    uintptr_t max_end = base;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD) {
            uintptr_t end = base + ph->p_vaddr + ph->p_memsz;
            if (end > max_end) max_end = end;
        }
    }
    if (max_end == base) return 0;  /* skip vDSO-like empty objects */

    /* Grow the map */
    if (g_modmap_count == g_modmap_cap) {
        size_t ncap = g_modmap_cap ? g_modmap_cap * 2 : 32;
        struct ModInfo* nm = (struct ModInfo*)realloc(
            g_modmap, ncap * sizeof(struct ModInfo));
        if (!nm) return 1;
        g_modmap = nm; g_modmap_cap = ncap;
    }

    const char* name = info->dlpi_name;
    if (!name || !name[0]) name = g_exe_path[0] ? g_exe_path : "/proc/self/exe";

    g_modmap[g_modmap_count].base = base;
    g_modmap[g_modmap_count].end  = max_end;
    g_modmap[g_modmap_count].path = strdup(name);
    g_modmap_count++;
    return 0;
}

/* Rebuild the module map from scratch — caller must hold cold_mutex. */
static void NO_INST
ft_rebuild_modmap(void)
{
    for (size_t i = 0; i < g_modmap_count; i++) free(g_modmap[i].path);
    g_modmap_count = 0;
    dl_iterate_phdr(ft_dl_cb, NULL);
}

/* Format "@<module>+0x<offset>" into `out`. Records only the raw
 * address in module-relative form so `ftrc2perfetto` can resolve it
 * later using the ELF `.symtab` + DWARF info of the on-disk binary.
 * This keeps the hot/cold path *dramatically* cheaper than doing
 * dladdr + demangle per unique function. */
static void NO_INST
ft_resolve_symbol(void* fn, char* out, size_t outlen)
{
    uintptr_t addr = (uintptr_t)fn;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (size_t i = 0; i < g_modmap_count; i++) {
            if (addr >= g_modmap[i].base && addr < g_modmap[i].end) {
                uintptr_t off = addr - g_modmap[i].base;
                snprintf(out, outlen, "@%s+0x%lx",
                         g_modmap[i].path, (unsigned long)off);
                return;
            }
        }
        /* Miss — may have dlopened a new object since the last rebuild. */
        ft_rebuild_modmap();
    }
    snprintf(out, outlen, "@?+0x%lx", (unsigned long)addr);
}

static uint32_t NO_INST
ft_intern_fp_slow(void* fn)
{
    pthread_mutex_lock(&g_tracer.cold_mutex);

    /* Double-check under the lock — another thread may have inserted. */
    uint32_t existing = ft_intern_lookup(&g_tracer.intern, fn);
    if (existing) {
        pthread_mutex_unlock(&g_tracer.cold_mutex);
        return existing;
    }

    char name[512];
    ft_resolve_symbol(fn, name, sizeof(name));
    uint32_t len = (uint32_t)strnlen(name, sizeof(name) - 1);

    /* Append to string table first so func_id N maps to string N. */
    if (ft_string_table_append(&g_tracer.strings, name, len) < 0) {
        pthread_mutex_unlock(&g_tracer.cold_mutex);
        return 0;
    }

    uint32_t count = atomic_load_explicit(&g_tracer.intern.count,
                                          memory_order_relaxed);
    uint32_t func_id = count + 1;
    if (func_id > FT_MAX_FUNC_ID ||
        ft_intern_insert_locked(&g_tracer.intern, fn, func_id) < 0) {
        pthread_mutex_unlock(&g_tracer.cold_mutex);
        return 0;
    }

    pthread_mutex_unlock(&g_tracer.cold_mutex);
    return func_id;
}

/* ── Thread registration ──────────────────────────────────────────── */

static uint8_t NO_INST
ft_register_thread(void)
{
    pthread_mutex_lock(&g_tracer.cold_mutex);
    uint16_t idx = atomic_load_explicit(&g_tracer.thread_map.count,
                                         memory_order_relaxed);
    if (idx >= FT_MAX_THREADS) {
        /* Overflow — lump everything past 255 into slot 255 */
        pthread_mutex_unlock(&g_tracer.cold_mutex);
        return (uint8_t)(FT_MAX_THREADS - 1);
    }
    struct ThreadMapEntry* e = &g_tracer.thread_map.entries[idx];
    e->os_tid = (uint64_t)ft_gettid();
    e->tid_idx = (uint8_t)idx;
    e->stack = &tl_stack;
    ft_capture_thread_name(e->name, sizeof(e->name));
    atomic_store_explicit(&g_tracer.thread_map.count, idx + 1,
                          memory_order_release);
    pthread_mutex_unlock(&g_tracer.cold_mutex);
    return (uint8_t)idx;
}

/* ── Hot path ─────────────────────────────────────────────────────── */

static inline void NO_INST
ft_record(void* fn, uint8_t flags)
{
    /* Initialisation happens on first hook call. pthread_once is cheap
     * after the first invocation (single atomic load). */
    pthread_once(&g_init_once, ft_init_once);
    if (!atomic_load_explicit(&g_tracer.collecting, memory_order_acquire))
        return;

    /* Fork detection: if the app forked, silently stop in the child. */
    if (getpid() != g_tracer.pid) {
        atomic_store_explicit(&g_tracer.collecting, 0, memory_order_release);
        return;
    }

    /* Reentrancy guard: if our own code (malloc during intern etc.) hits
     * an instrumented callback, drop it. */
    if (tl_in_hook) return;
    tl_in_hook = 1;

    if (!tl_stack_init) {
        tl_stack.depth = 0;
        tl_stack.registered = 0;
        tl_stack_init = 1;
    }
    if (!tl_stack.registered) {
        tl_stack.tid_idx = ft_register_thread();
        tl_stack.registered = 1;
    }

    /* Threads often rename themselves shortly AFTER entering a function
     * (e.g. worker() → pthread_setname_np(...) → real work). So the
     * name captured at registration is typically the process-wide comm.
     * Poll PR_GET_NAME cheaply every 256 calls until we see a name that
     * differs from the process name, then stop. */
    if (!tl_name_stable && ((++tl_call_count & 255) == 0)) {
        char cur[16];
        if (prctl(PR_GET_NAME, (unsigned long)cur, 0, 0, 0) == 0) {
            cur[15] = 0;
            struct ThreadMapEntry* e =
                &g_tracer.thread_map.entries[tl_stack.tid_idx];
            if (strncmp(e->name, cur, sizeof(cur)) != 0) {
                pthread_mutex_lock(&g_tracer.cold_mutex);
                memset(e->name, 0, FT_THREAD_NAME_LEN);
                memcpy(e->name, cur, strnlen(cur, sizeof(cur)));
                pthread_mutex_unlock(&g_tracer.cold_mutex);
            }
            if (strcmp(cur, g_tracer.process_name) != 0) tl_name_stable = 1;
        }
    }

    uint32_t fid = ft_intern_lookup(&g_tracer.intern, fn);
    if (fid == 0) {
        fid = ft_intern_fp_slow(fn);
        if (fid == 0) { tl_in_hook = 0; return; }
    }

    int64_t now = ft_now_ns();
    uint32_t delta_us = (uint32_t)((now - g_tracer.base_ts_ns) / 1000);

    size_t offset = atomic_fetch_add_explicit(
        &g_tracer.write_offset, sizeof(struct BinaryEvent),
        memory_order_relaxed);

    if (offset + sizeof(struct BinaryEvent) > g_tracer.buffer_size) {
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &g_tracer.flush_in_progress, &expected, 1,
                memory_order_acq_rel, memory_order_acquire)) {
            ft_flush_buffer(0);
            atomic_store_explicit(&g_tracer.flush_in_progress, 0,
                                  memory_order_release);
        } else {
            while (atomic_load_explicit(&g_tracer.flush_in_progress,
                                        memory_order_acquire))
                ;
        }
        offset = atomic_fetch_add_explicit(
            &g_tracer.write_offset, sizeof(struct BinaryEvent),
            memory_order_relaxed);
        if (offset + sizeof(struct BinaryEvent) > g_tracer.buffer_size) {
            /* Writer hasn't caught up — drop to avoid blocking the hot
             * path. Losing a sprinkling of events is far better than
             * deadlocking user code. */
            tl_in_hook = 0;
            return;
        }
        /* Recompute delta relative to the (new) base_ts_ns. */
        delta_us = (uint32_t)((now - g_tracer.base_ts_ns) / 1000);
    }

    struct BinaryEvent* ev = (struct BinaryEvent*)
        ((char*)g_tracer.buffers[g_tracer.active_buf] + offset);
    ev->ts_delta_us = delta_us;
    ev->func_id = fid;
    ev->tid_idx = tl_stack.tid_idx;
    ev->flags = (uint8_t)(flags | FT_FLAG_C_FUNCTION);
    ev->_pad = 0;

    if (!(flags & FT_FLAG_EXIT)) {
        if (tl_stack.depth < FT_MAX_STACK_DEPTH) {
            tl_stack.func_ids[tl_stack.depth] = fid;
        }
        tl_stack.depth++;
    } else {
        if (tl_stack.depth > 0) tl_stack.depth--;
    }
    tl_in_hook = 0;
}

/* ── GCC/clang -finstrument-functions entry points ────────────────── */
/* These must NOT have the no_instrument_function attribute, but the
 * whole TU is compiled with -fno-instrument-functions. */

void __cyg_profile_func_enter(void* this_fn, void* call_site)
    __attribute__((visibility("default")));
void __cyg_profile_func_exit (void* this_fn, void* call_site)
    __attribute__((visibility("default")));

void
__cyg_profile_func_enter(void* this_fn, void* call_site)
{
    (void)call_site;
    ft_record(this_fn, 0);
}

void
__cyg_profile_func_exit(void* this_fn, void* call_site)
{
    (void)call_site;
    ft_record(this_fn, FT_FLAG_EXIT);
}

/* ── Writer thread ────────────────────────────────────────────────── */

static void* NO_INST
ft_writer_thread(void* arg)
{
    (void)arg;
    for (;;) {
        sem_wait(&g_tracer.flush_avail);
        if (g_tracer.writer_stop) break;

        struct FlushRequest* req = &g_tracer.flush_queue[g_tracer.flush_tail];
        g_tracer.flush_tail = (g_tracer.flush_tail + 1) % FT_NUM_BUFFERS;

        int fd = open(req->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            size_t done = 0;
            while (done < req->bytes) {
                ssize_t n = write(fd,
                    (char*)g_tracer.buffers[req->buf_index] + done,
                    req->bytes - done);
                if (n <= 0) break;
                done += n;
            }
            close(fd);
        } else {
            fprintf(stderr, "cppfunctrace: cannot open %s: %s\n",
                    req->path, strerror(errno));
        }
        sem_post(&g_tracer.free_bufs);
    }
    return NULL;
}

/* ── Flush ────────────────────────────────────────────────────────── */

static int NO_INST
ft_flush_buffer(int sync)
{
    int buf = g_tracer.active_buf;
    size_t bytes = atomic_load_explicit(&g_tracer.write_offset,
                                         memory_order_relaxed);
    uint32_t num_events = 0;
    if (bytes > g_tracer.events_start) {
        num_events = (uint32_t)((bytes - g_tracer.events_start)
                                / sizeof(struct BinaryEvent));
    }
    if (num_events == 0) return 0;

    /* Hold cold_mutex while copying string / thread state so that
     * a concurrent insert doesn't tear the tables mid-copy. */
    pthread_mutex_lock(&g_tracer.cold_mutex);

    /* Refresh thread names from /proc/self/task/<tid>/comm — threads
     * frequently rename themselves after the first traced call, so the
     * value we cached at registration is often "<binary-name>" rather
     * than the intended worker label. */
    int nthreads = (int)atomic_load_explicit(&g_tracer.thread_map.count,
                                              memory_order_relaxed);
    for (int i = 0; i < nthreads; i++) {
        uint64_t tid = g_tracer.thread_map.entries[i].os_tid;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%llu/comm",
                 (unsigned long long)tid);
        int tfd = open(path, O_RDONLY);
        if (tfd >= 0) {
            char tmp[FT_THREAD_NAME_LEN];
            ssize_t n = read(tfd, tmp, sizeof(tmp) - 1);
            close(tfd);
            if (n > 0) {
                tmp[n] = 0;
                for (ssize_t k = n - 1; k >= 0 &&
                        (tmp[k] == '\n' || tmp[k] == '\r'); k--) tmp[k] = 0;
                if (tmp[0])
                    memcpy(g_tracer.thread_map.entries[i].name, tmp,
                           FT_THREAD_NAME_LEN);
            }
        }
    }

    struct BufferHeader* hdr = (struct BufferHeader*)g_tracer.buffers[buf];
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = FT_MAGIC;
    hdr->version = FT_VERSION;
    hdr->pid = (uint32_t)g_tracer.pid;
    hdr->num_strings = atomic_load_explicit(&g_tracer.intern.count,
                                             memory_order_relaxed);
    hdr->base_ts_ns = g_tracer.base_ts_ns;
    hdr->num_events = num_events;
    hdr->num_threads = (uint8_t)nthreads;
    hdr->string_table_offset = (uint32_t)sizeof(struct BufferHeader);
    hdr->events_offset = (uint32_t)g_tracer.events_start;

    for (int i = 0; i < hdr->num_threads; i++) {
        hdr->thread_table[i] = g_tracer.thread_map.entries[i].os_tid;
        memcpy(hdr->thread_names[i], g_tracer.thread_map.entries[i].name,
               FT_THREAD_NAME_LEN);
    }
    memcpy(hdr->process_name, g_tracer.process_name, FT_THREAD_NAME_LEN);

    size_t st_space = g_tracer.events_start - sizeof(struct BufferHeader);
    size_t st_len = g_tracer.strings.len < st_space
                  ? g_tracer.strings.len : st_space;
    memcpy((char*)g_tracer.buffers[buf] + sizeof(struct BufferHeader),
           g_tracer.strings.data, st_len);

    pthread_mutex_unlock(&g_tracer.cold_mutex);

    /* Swap to the next buffer before starting I/O so recording can
     * continue without waiting on disk. */
    if (!sync) {
        sem_wait(&g_tracer.free_bufs);
    }
    g_tracer.active_buf = (g_tracer.active_buf + 1) % FT_NUM_BUFFERS;
    g_tracer.base_ts_ns = ft_now_ns();
    atomic_store_explicit(&g_tracer.write_offset, g_tracer.events_start,
                          memory_order_relaxed);

    if (sync) {
        char path[512];
        ft_make_output_path(path, sizeof(path));
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            size_t done = 0;
            while (done < bytes) {
                ssize_t n = write(fd, (char*)g_tracer.buffers[buf] + done,
                                  bytes - done);
                if (n <= 0) break;
                done += n;
            }
            close(fd);
        }
        g_tracer.cumulative_bytes += bytes;
        return 0;
    }

    struct FlushRequest* req = &g_tracer.flush_queue[g_tracer.flush_head];
    req->buf_index = buf;
    req->bytes = bytes;
    ft_make_output_path(req->path, sizeof(req->path));
    g_tracer.flush_head = (g_tracer.flush_head + 1) % FT_NUM_BUFFERS;
    sem_post(&g_tracer.flush_avail);

    g_tracer.cumulative_bytes += bytes;

    if (g_tracer.rollover_threshold > 0 &&
        g_tracer.cumulative_bytes >= g_tracer.rollover_threshold) {
        /* Drain in-flight writes so the next chunk lands in the new file. */
        for (int i = 0; i < FT_NUM_BUFFERS - 1; i++) sem_wait(&g_tracer.free_bufs);
        for (int i = 0; i < FT_NUM_BUFFERS - 1; i++) sem_post(&g_tracer.free_bufs);
        g_tracer.file_seq++;
        g_tracer.cumulative_bytes = 0;
    }

    return 0;
}

/* ── Emergency / atexit flush ─────────────────────────────────────── */

static void NO_INST
ft_emergency_flush(void)
{
    if (!atomic_load_explicit(&g_tracer.initialised, memory_order_acquire))
        return;
    /* Never flush from a forked child — g_tracer.pid holds the parent's
     * PID so the output path would be <parent_pid>.ftrc, corrupting
     * the parent's trace file. Signal handlers and atexit hooks are
     * both inherited across fork, so without this guard a child that
     * dies before any instrumented function ever ran (collecting is
     * still COW-inherited as 1) would append its empty buffer to the
     * parent's file. */
    if (getpid() != g_tracer.pid) {
        atomic_store_explicit(&g_tracer.collecting, 0, memory_order_release);
        return;
    }
    if (!atomic_exchange_explicit(&g_tracer.collecting, 0,
                                  memory_order_acq_rel))
        return;
    /* Best-effort sync flush using only safe libc. */
    int buf = g_tracer.active_buf;
    size_t bytes = atomic_load_explicit(&g_tracer.write_offset,
                                         memory_order_relaxed);
    uint32_t num_events = 0;
    if (bytes > g_tracer.events_start) {
        num_events = (uint32_t)((bytes - g_tracer.events_start)
                                / sizeof(struct BinaryEvent));
    }
    if (num_events == 0) return;

    struct BufferHeader* hdr = (struct BufferHeader*)g_tracer.buffers[buf];
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = FT_MAGIC;
    hdr->version = FT_VERSION;
    hdr->pid = (uint32_t)g_tracer.pid;
    hdr->num_strings = atomic_load_explicit(&g_tracer.intern.count,
                                             memory_order_relaxed);
    hdr->base_ts_ns = g_tracer.base_ts_ns;
    hdr->num_events = num_events;
    hdr->num_threads = (uint8_t)atomic_load_explicit(
        &g_tracer.thread_map.count, memory_order_relaxed);
    hdr->string_table_offset = (uint32_t)sizeof(struct BufferHeader);
    hdr->events_offset = (uint32_t)g_tracer.events_start;
    for (int i = 0; i < hdr->num_threads; i++) {
        hdr->thread_table[i] = g_tracer.thread_map.entries[i].os_tid;
        memcpy(hdr->thread_names[i], g_tracer.thread_map.entries[i].name,
               FT_THREAD_NAME_LEN);
    }
    memcpy(hdr->process_name, g_tracer.process_name, FT_THREAD_NAME_LEN);

    size_t st_space = g_tracer.events_start - sizeof(struct BufferHeader);
    size_t st_len = g_tracer.strings.len < st_space
                  ? g_tracer.strings.len : st_space;
    memcpy((char*)g_tracer.buffers[buf] + sizeof(struct BufferHeader),
           g_tracer.strings.data, st_len);

    char path[512];
    ft_make_output_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = write(fd, (char*)g_tracer.buffers[buf] + done,
                          bytes - done);
        if (n <= 0) break;
        done += n;
    }
    close(fd);
}

static void NO_INST
ft_atexit_handler(void)
{
    cppfunctrace_stop();
}

static void NO_INST
ft_signal_handler(int signo, siginfo_t* info, void* ucontext)
{
    (void)info;
    (void)ucontext;
    ft_emergency_flush();

    struct sigaction* old = NULL;
    switch (signo) {
    case SIGTERM: old = &g_old_sigterm; break;
    case SIGABRT: old = &g_old_sigabrt; break;
    case SIGSEGV: old = &g_old_sigsegv; break;
    case SIGINT:  old = &g_old_sigint;  break;
    default: break;
    }
    if (old && old->sa_handler != SIG_IGN && old->sa_handler != SIG_DFL) {
        if (old->sa_flags & SA_SIGINFO) {
            old->sa_sigaction(signo, info, ucontext);
        } else {
            old->sa_handler(signo);
        }
    } else {
        signal(signo, SIG_DFL);
        raise(signo);
    }
}

static void NO_INST
ft_install_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ft_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, &g_old_sigterm);
    sigaction(SIGABRT, &sa, &g_old_sigabrt);
    sigaction(SIGSEGV, &sa, &g_old_sigsegv);
    sigaction(SIGINT,  &sa, &g_old_sigint);
}

/* ── Public API ───────────────────────────────────────────────────── */

void NO_INST
cppfunctrace_start(void)
{
    pthread_once(&g_init_once, ft_init_once);
    if (!atomic_load_explicit(&g_tracer.initialised, memory_order_acquire))
        return;
    /* Reset the base timestamp so delta fits in uint32 microseconds even
     * after a long dormant period between init and start. */
    g_tracer.base_ts_ns = ft_now_ns();
    atomic_store_explicit(&g_tracer.collecting, 1, memory_order_release);
}

void NO_INST
cppfunctrace_stop(void)
{
    if (!atomic_load_explicit(&g_tracer.initialised, memory_order_acquire))
        return;
    /* Same guard as ft_emergency_flush: a fork child must never flush
     * the parent's PID-named output file. */
    if (getpid() != g_tracer.pid) {
        atomic_store_explicit(&g_tracer.collecting, 0, memory_order_release);
        return;
    }
    if (!atomic_exchange_explicit(&g_tracer.collecting, 0,
                                  memory_order_acq_rel))
        return;

    /* Final synchronous flush through the normal path. */
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &g_tracer.flush_in_progress, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        ft_flush_buffer(1);
        atomic_store_explicit(&g_tracer.flush_in_progress, 0,
                              memory_order_release);
    }
}

const char* NO_INST
cppfunctrace_output_path(void)
{
    static __thread char buf[512];
    if (!atomic_load_explicit(&g_tracer.initialised, memory_order_acquire))
        return NULL;
    ft_make_output_path(buf, sizeof(buf));
    return buf;
}

/* Library constructor — runs before main() so tracing can start early.
 * We defer the heavy init to first hook invocation (pthread_once), which
 * lets symbol-resolution picks up the fully-populated link map; but we
 * still kick it here so that programs that run only pre-main work get
 * traced too. */
__attribute__((constructor))
static void NO_INST
ft_ctor(void)
{
    pthread_once(&g_init_once, ft_init_once);
}

__attribute__((destructor))
static void NO_INST
ft_dtor(void)
{
    cppfunctrace_stop();
}
