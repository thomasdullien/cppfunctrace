/*
 * ftrc2perfetto — convert .ftrc binary traces to Perfetto's native
 * protobuf format (.perfetto-trace), readable directly by
 * ui.perfetto.dev, trace_processor, and anything else that consumes
 * the perfetto.protos.Trace schema.
 *
 * No libprotobuf dependency — we hand-emit the wire format. Only the
 * fields actually needed to render per-thread slice tracks are encoded:
 *   Trace { repeated TracePacket packet = 1; }
 *   TracePacket.timestamp                  (8,  varint)
 *   TracePacket.timestamp_clock_id         (58, varint)
 *   TracePacket.track_event                (11, msg)
 *   TracePacket.trusted_packet_sequence_id (10, varint)
 *   TracePacket.interned_data              (12, msg)
 *   TracePacket.sequence_flags             (13, varint)
 *   TracePacket.track_descriptor           (60, msg)
 *   TrackDescriptor.uuid                   (1,  varint)
 *   TrackDescriptor.process                (3,  msg)
 *   TrackDescriptor.thread                 (4,  msg)
 *   TrackDescriptor.parent_uuid            (5,  varint)
 *   ProcessDescriptor.pid                  (1,  varint)
 *   ProcessDescriptor.process_name         (6,  string)
 *   ThreadDescriptor.pid                   (1,  varint)
 *   ThreadDescriptor.tid                   (2,  varint)
 *   ThreadDescriptor.thread_name           (5,  string)
 *   TrackEvent.name_iid                    (10, varint)
 *   TrackEvent.type                        (9,  enum)
 *   TrackEvent.track_uuid                  (11, varint)
 *   InternedData.event_names               (2,  repeated msg)
 *   EventName.iid                          (1,  varint)
 *   EventName.name                         (2,  string)
 *
 * CLOCK_MONOTONIC is encoded via BUILTIN_CLOCK_MONOTONIC = 3.
 */

#define _GNU_SOURCE
#include "libftrc.h"
#include "symresolve.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Small growable byte buffer ───────────────────────────────────── */

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} pbuf;

static void pbuf_init(pbuf* b)    { b->data = NULL; b->len = 0; b->cap = 0; }
static void pbuf_clear(pbuf* b)   { b->len = 0; }
static void pbuf_free(pbuf* b)    { free(b->data); pbuf_init(b); }

static void
pbuf_grow(pbuf* b, size_t need)
{
    if (b->cap >= need) return;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    uint8_t* nd = (uint8_t*)realloc(b->data, ncap);
    if (!nd) { fprintf(stderr, "ftrc2perfetto: OOM\n"); exit(1); }
    b->data = nd;
    b->cap = ncap;
}

static inline void
pbuf_byte(pbuf* b, uint8_t c)
{
    pbuf_grow(b, b->len + 1);
    b->data[b->len++] = c;
}

static inline void
pbuf_bytes(pbuf* b, const void* src, size_t n)
{
    pbuf_grow(b, b->len + n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

/* Standard protobuf varint. */
static void
pbuf_varint(pbuf* b, uint64_t v)
{
    while (v >= 0x80) { pbuf_byte(b, (uint8_t)(v | 0x80)); v >>= 7; }
    pbuf_byte(b, (uint8_t)v);
}

/* (field_number << 3) | wire_type */
static inline void
pbuf_tag(pbuf* b, uint32_t field, uint32_t wire_type)
{
    pbuf_varint(b, (uint64_t)((field << 3) | wire_type));
}

/* Encode `field` as a varint of `value`. */
static void
pb_varint_field(pbuf* b, uint32_t field, uint64_t value)
{
    pbuf_tag(b, field, 0);
    pbuf_varint(b, value);
}

/* Encode `field` as a length-delimited bytes/string. */
static void
pb_bytes_field(pbuf* b, uint32_t field, const void* src, size_t n)
{
    pbuf_tag(b, field, 2);
    pbuf_varint(b, (uint64_t)n);
    pbuf_bytes(b, src, n);
}

static inline void
pb_string_field(pbuf* b, uint32_t field, const char* s)
{
    pb_bytes_field(b, field, s, strlen(s));
}

/* Encode `field` as a length-delimited embedded message — takes the
 * already-serialised message bytes from `msg`. */
static inline void
pb_msg_field(pbuf* out, uint32_t field, const pbuf* msg)
{
    pb_bytes_field(out, field, msg->data, msg->len);
}

/* ── Name interning ───────────────────────────────────────────────── */
/*
 * To keep output size sane we intern TrackEvent names and reference them
 * by `name_iid`. Each time a new name is encountered we attach it to the
 * TracePacket's `interned_data.event_names` field. Perfetto's reader
 * remembers interned entries across packets as long as every packet
 * since the clear carries `SEQ_NEEDS_INCREMENTAL_STATE`.
 */

typedef struct {
    char*    name;       /* heap-owned, NUL-terminated */
    uint32_t len;
    uint64_t iid;
    uint64_t hash;
} NameEntry;

typedef struct {
    NameEntry* entries;   /* open-addressing; iid==0 = empty */
    size_t     capacity;  /* power of two */
    size_t     count;
} NameTable;

static uint64_t
fnv1a(const char* s, uint32_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 0x100000001b3ULL;
    }
    return h ? h : 1;
}

static void
name_table_init(NameTable* t)
{
    t->capacity = 4096;
    t->count = 0;
    t->entries = (NameEntry*)calloc(t->capacity, sizeof(NameEntry));
    if (!t->entries) { fprintf(stderr, "ftrc2perfetto: OOM\n"); exit(1); }
}

static void
name_table_free(NameTable* t)
{
    for (size_t i = 0; i < t->capacity; i++) free(t->entries[i].name);
    free(t->entries);
    t->entries = NULL;
    t->capacity = t->count = 0;
}

static void name_table_grow(NameTable* t);

/* Lookup or insert; sets *is_new=1 if inserted. Returns iid. */
static uint64_t
name_intern(NameTable* t, const char* s, uint32_t len, int* is_new)
{
    if (t->count * 2 >= t->capacity) name_table_grow(t);

    uint64_t h = fnv1a(s, len);
    size_t mask = t->capacity - 1;
    size_t idx = (size_t)h & mask;
    for (;;) {
        NameEntry* e = &t->entries[idx];
        if (e->iid == 0) {
            e->name = (char*)malloc(len + 1);
            if (!e->name) { fprintf(stderr, "OOM\n"); exit(1); }
            memcpy(e->name, s, len);
            e->name[len] = 0;
            e->len = len;
            e->hash = h;
            e->iid = ++t->count;    /* iids start at 1 */
            *is_new = 1;
            return e->iid;
        }
        if (e->hash == h && e->len == len && memcmp(e->name, s, len) == 0) {
            *is_new = 0;
            return e->iid;
        }
        idx = (idx + 1) & mask;
    }
}

static void
name_table_grow(NameTable* t)
{
    size_t old_cap = t->capacity;
    NameEntry* old = t->entries;
    t->capacity = old_cap * 2;
    t->entries = (NameEntry*)calloc(t->capacity, sizeof(NameEntry));
    if (!t->entries) { fprintf(stderr, "OOM\n"); exit(1); }
    size_t mask = t->capacity - 1;
    for (size_t i = 0; i < old_cap; i++) {
        NameEntry* e = &old[i];
        if (e->iid == 0) continue;
        size_t idx = (size_t)e->hash & mask;
        while (t->entries[idx].iid) idx = (idx + 1) & mask;
        t->entries[idx] = *e;
    }
    free(old);
}

/* ── Track descriptor bookkeeping ─────────────────────────────────── */
/*
 * Every unique (pid, tid) pair gets one TrackDescriptor. We emit the
 * process descriptor the first time we see a pid, then a thread
 * descriptor the first time we see each (pid, tid).
 */

typedef struct {
    uint32_t pid;
    uint64_t tid;
    uint64_t track_uuid;
} TrackKey;

typedef struct {
    TrackKey* tracks;
    size_t    count;
    size_t    cap;
} TrackSet;

static int
track_set_find(TrackSet* s, uint32_t pid, uint64_t tid, uint64_t* uuid_out)
{
    for (size_t i = 0; i < s->count; i++) {
        if (s->tracks[i].pid == pid && s->tracks[i].tid == tid) {
            if (uuid_out) *uuid_out = s->tracks[i].track_uuid;
            return 1;
        }
    }
    return 0;
}

static uint64_t
track_set_add(TrackSet* s, uint32_t pid, uint64_t tid, uint64_t uuid)
{
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->tracks = (TrackKey*)realloc(s->tracks, s->cap * sizeof(TrackKey));
        if (!s->tracks) { fprintf(stderr, "OOM\n"); exit(1); }
    }
    s->tracks[s->count].pid = pid;
    s->tracks[s->count].tid = tid;
    s->tracks[s->count].track_uuid = uuid;
    s->count++;
    return uuid;
}

/* ── Stable UUID derivation ───────────────────────────────────────── */
/*
 * Perfetto requires track_uuid values to be globally unique within a
 * trace. Deriving them from (pid, tid) via a splittable 64-bit mix keeps
 * output deterministic and collision-free for any reasonable workload.
 */
static uint64_t
mix64(uint64_t a, uint64_t b)
{
    uint64_t v = a * 0x9e3779b97f4a7c15ULL + b;
    v ^= v >> 30; v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27; v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
    return v ? v : 1;
}

static uint64_t process_uuid(uint32_t pid)                 { return mix64(pid, 1); }
static uint64_t thread_uuid (uint32_t pid, uint64_t tid)  { return mix64((uint64_t)pid << 32 | 0xA5, tid); }

/* ── Perfetto proto field numbers ─────────────────────────────────── */

#define FLD_TP_TIMESTAMP            8
#define FLD_TP_TRACK_EVENT          11
#define FLD_TP_INTERNED_DATA        12
#define FLD_TP_TRUSTED_SEQ_ID       10
#define FLD_TP_SEQ_FLAGS            13
#define FLD_TP_TIMESTAMP_CLOCK_ID   58
#define FLD_TP_TRACK_DESCRIPTOR     60

#define FLD_TD_UUID                 1
#define FLD_TD_NAME                 2
#define FLD_TD_PROCESS              3
#define FLD_TD_THREAD               4
#define FLD_TD_PARENT_UUID          5

#define FLD_PD_PID                  1
#define FLD_PD_PROCESS_NAME         6

#define FLD_TH_PID                  1
#define FLD_TH_TID                  2
#define FLD_TH_THREAD_NAME          5

#define FLD_TE_CATEGORY_IIDS        3
#define FLD_TE_TYPE                 9
#define FLD_TE_NAME_IID             10
#define FLD_TE_TRACK_UUID           11

#define FLD_ID_EVENT_NAMES          2

#define FLD_EN_IID                  1
#define FLD_EN_NAME                 2

#define TE_TYPE_SLICE_BEGIN         1
#define TE_TYPE_SLICE_END           2

#define SEQ_INCREMENTAL_CLEARED     1
#define SEQ_NEEDS_INCREMENTAL_STATE 2

#define BUILTIN_CLOCK_MONOTONIC     3

#define TRUSTED_SEQ_ID              1

/* ── Output writer ────────────────────────────────────────────────── */

typedef struct {
    FILE*         out;
    uint64_t      packets;
    pbuf          tmp_packet;
    pbuf          tmp_track_event;
    pbuf          tmp_track_descriptor;
    pbuf          tmp_process;
    pbuf          tmp_thread;
    pbuf          tmp_interned;
    pbuf          tmp_event_name;
    NameTable     names;
    TrackSet      tracks;
    int           first_packet_written;
    /* Process descriptors we've already emitted, to dedupe */
    uint32_t*     known_pids;
    size_t        known_pids_count;
    size_t        known_pids_cap;
    sym_resolver* resolver;    /* turns "@<mod>+0x<off>" into real names */
} Writer;

static int
pids_contains(Writer* w, uint32_t pid)
{
    for (size_t i = 0; i < w->known_pids_count; i++)
        if (w->known_pids[i] == pid) return 1;
    return 0;
}

static void
pids_add(Writer* w, uint32_t pid)
{
    if (w->known_pids_count == w->known_pids_cap) {
        w->known_pids_cap = w->known_pids_cap ? w->known_pids_cap * 2 : 16;
        w->known_pids = realloc(w->known_pids,
                                w->known_pids_cap * sizeof(uint32_t));
        if (!w->known_pids) { fprintf(stderr, "OOM\n"); exit(1); }
    }
    w->known_pids[w->known_pids_count++] = pid;
}

/* Emit the packet sitting in w->tmp_packet as Trace.packet field #1. */
static void
writer_flush_packet(Writer* w)
{
    /* Trace.packet field: tag=(1<<3|2), varint(len), bytes. */
    uint8_t framing[16];
    size_t  fl = 0;
    framing[fl++] = (1 << 3) | 2;
    uint64_t len = w->tmp_packet.len;
    while (len >= 0x80) { framing[fl++] = (uint8_t)(len | 0x80); len >>= 7; }
    framing[fl++] = (uint8_t)len;

    fwrite(framing, 1, fl, w->out);
    fwrite(w->tmp_packet.data, 1, w->tmp_packet.len, w->out);
    w->packets++;
    pbuf_clear(&w->tmp_packet);
}

/* Set the sequence_flags field. First packet uses CLEARED, rest use
 * NEEDS_INCREMENTAL_STATE so interned names stay valid. */
static void
writer_begin_packet(Writer* w, pbuf* p)
{
    pbuf_clear(p);
    pb_varint_field(p, FLD_TP_TRUSTED_SEQ_ID, TRUSTED_SEQ_ID);
    pb_varint_field(p, FLD_TP_SEQ_FLAGS,
                    w->first_packet_written ? SEQ_NEEDS_INCREMENTAL_STATE
                                             : SEQ_INCREMENTAL_CLEARED);
    w->first_packet_written = 1;
}

/* Emit a TrackDescriptor packet for a process. */
static void
writer_emit_process(Writer* w, uint32_t pid, const char* pname)
{
    if (pids_contains(w, pid)) return;
    pids_add(w, pid);

    pbuf* proc = &w->tmp_process;
    pbuf* td   = &w->tmp_track_descriptor;
    pbuf* p    = &w->tmp_packet;
    pbuf_clear(proc); pbuf_clear(td);

    pb_varint_field(proc, FLD_PD_PID, pid);
    if (pname && pname[0]) pb_string_field(proc, FLD_PD_PROCESS_NAME, pname);

    pb_varint_field(td, FLD_TD_UUID, process_uuid(pid));
    pb_msg_field   (td, FLD_TD_PROCESS, proc);
    /* Also populate .name so the UI shows something even if the process
     * descriptor is elided in a narrow view. */
    if (pname && pname[0]) pb_string_field(td, FLD_TD_NAME, pname);

    writer_begin_packet(w, p);
    pb_msg_field(p, FLD_TP_TRACK_DESCRIPTOR, td);
    writer_flush_packet(w);
}

/* Emit a TrackDescriptor packet for a thread, if not already emitted. */
static void
writer_emit_thread(Writer* w, uint32_t pid, uint64_t tid, const char* tname)
{
    uint64_t uuid;
    if (track_set_find(&w->tracks, pid, tid, &uuid)) return;
    uuid = thread_uuid(pid, tid);
    track_set_add(&w->tracks, pid, tid, uuid);

    pbuf* thr = &w->tmp_thread;
    pbuf* td  = &w->tmp_track_descriptor;
    pbuf* p   = &w->tmp_packet;
    pbuf_clear(thr); pbuf_clear(td);

    pb_varint_field(thr, FLD_TH_PID, pid);
    pb_varint_field(thr, FLD_TH_TID, (uint64_t)tid);
    if (tname && tname[0]) pb_string_field(thr, FLD_TH_THREAD_NAME, tname);

    pb_varint_field(td, FLD_TD_UUID, uuid);
    pb_varint_field(td, FLD_TD_PARENT_UUID, process_uuid(pid));
    pb_msg_field   (td, FLD_TD_THREAD, thr);
    if (tname && tname[0]) pb_string_field(td, FLD_TD_NAME, tname);

    writer_begin_packet(w, p);
    pb_msg_field(p, FLD_TP_TRACK_DESCRIPTOR, td);
    writer_flush_packet(w);
}

/* Encode one SLICE_BEGIN + SLICE_END pair for an FTRC complete event. */
static void
writer_emit_slice(Writer* w, const ftrc_event* ev)
{
    uint64_t uuid;
    if (!track_set_find(&w->tracks, ev->pid, ev->tid, &uuid)) {
        uuid = thread_uuid(ev->pid, ev->tid);
        track_set_add(&w->tracks, ev->pid, ev->tid, uuid);
    }

    /* Dress up raw "@<module>+0x<offset>" names recorded by the tracer.
     * On a miss, fall back to the raw string (still usable for debugging
     * when the binary is not present on this host). */
    const char* disp     = ev->name;
    uint32_t    disp_len = ev->name_len;
    char        resolved[1024];
    if (w->resolver && ev->name_len >= 2 && ev->name[0] == '@' &&
        sym_resolver_lookup(w->resolver, ev->name, ev->name_len,
                             resolved, sizeof(resolved))) {
        disp     = resolved;
        disp_len = (uint32_t)strnlen(resolved, sizeof(resolved));
    }

    int is_new = 0;
    uint64_t iid = name_intern(&w->names, disp, disp_len, &is_new);

    /* BEGIN packet */
    pbuf* te = &w->tmp_track_event;
    pbuf* p  = &w->tmp_packet;
    pbuf_clear(te);

    pb_varint_field(te, FLD_TE_TYPE, TE_TYPE_SLICE_BEGIN);
    pb_varint_field(te, FLD_TE_TRACK_UUID, uuid);
    pb_varint_field(te, FLD_TE_NAME_IID, iid);

    writer_begin_packet(w, p);

    /* Perfetto timestamps are uint64 nanoseconds. ftrc ts is in microseconds
     * as a double — multiply and round. We don't set timestamp_clock_id:
     * all our events share one monotonic reference, so letting the trace
     * processor use its default clock (BOOTTIME) still renders correct
     * relative timings without requiring a ClockSnapshot packet. */
    uint64_t ts_ns = (uint64_t)(ev->ts_us * 1000.0 + 0.5);
    pb_varint_field(p, FLD_TP_TIMESTAMP, ts_ns);
    pb_msg_field   (p, FLD_TP_TRACK_EVENT, te);

    if (is_new) {
        pbuf* en = &w->tmp_event_name;
        pbuf* id = &w->tmp_interned;
        pbuf_clear(en); pbuf_clear(id);
        pb_varint_field(en, FLD_EN_IID, iid);
        pb_bytes_field (en, FLD_EN_NAME, disp, disp_len);
        pb_msg_field   (id, FLD_ID_EVENT_NAMES, en);
        pb_msg_field   (p,  FLD_TP_INTERNED_DATA, id);
    }

    writer_flush_packet(w);

    /* END packet */
    pbuf_clear(te);
    pb_varint_field(te, FLD_TE_TYPE, TE_TYPE_SLICE_END);
    pb_varint_field(te, FLD_TE_TRACK_UUID, uuid);

    writer_begin_packet(w, p);
    uint64_t end_ns = (uint64_t)((ev->ts_us + ev->dur_us) * 1000.0 + 0.5);
    pb_varint_field(p, FLD_TP_TIMESTAMP, end_ns);
    pb_msg_field   (p, FLD_TP_TRACK_EVENT, te);
    writer_flush_packet(w);
}

/* ── Per-file processing ──────────────────────────────────────────── */

static int
process_file(Writer* w, const char* path, uint64_t* events_out)
{
    ftrc_reader* r = ftrc_open(path);
    if (!r) return -1;

    /* Track which pids have emitted process descriptors via metadata.
     * We also emit thread descriptors lazily on first slice. */
    ftrc_event ev;
    uint64_t   local_slices = 0;

    while (ftrc_next(r, &ev) == 0) {
        if (ev.type == FTRC_EVENT_METADATA) {
            if (ev.name_len == 12 && memcmp(ev.name, "process_name", 12) == 0) {
                /* meta_value is NUL-terminated */
                writer_emit_process(w, ev.pid,
                                     ev.meta_value ? ev.meta_value : "");
            } else if (ev.name_len == 11 && memcmp(ev.name, "thread_name", 11) == 0) {
                /* Ensure process descriptor exists (fall back when the
                 * per-chunk process_name was empty). */
                writer_emit_process(w, ev.pid, "");
                writer_emit_thread(w, ev.pid, ev.tid,
                                    ev.meta_value ? ev.meta_value : "");
            }
        } else if (ev.type == FTRC_EVENT_COMPLETE) {
            writer_emit_process(w, ev.pid, "");
            writer_emit_thread(w, ev.pid, ev.tid, "");
            writer_emit_slice(w, &ev);
            local_slices++;
        }
    }

    fprintf(stderr, "Read %lu slices from %s\n",
            (unsigned long)local_slices, path);
    if (events_out) *events_out += local_slices;
    ftrc_close(r);
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int
main(int argc, char** argv)
{
    const char* output_path = NULL;
    const char** inputs = (const char**)malloc((size_t)argc * sizeof(char*));
    int num_inputs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "Usage: %s [-o output.perfetto-trace] input1.ftrc [...]\n",
                argv[0]);
            free(inputs);
            return 0;
        } else {
            inputs[num_inputs++] = argv[i];
        }
    }

    if (num_inputs == 0) {
        fprintf(stderr,
            "Usage: %s [-o output.perfetto-trace] input1.ftrc [...]\n",
            argv[0]);
        free(inputs);
        return 1;
    }

    FILE* out = output_path ? fopen(output_path, "wb") : stdout;
    if (!out) {
        fprintf(stderr, "Cannot open %s: %s\n", output_path, strerror(errno));
        free(inputs);
        return 1;
    }

    /* Big stdio buffer reduces write syscall count dramatically. */
    char* iobuf = (char*)malloc(4 * 1024 * 1024);
    if (iobuf) setvbuf(out, iobuf, _IOFBF, 4 * 1024 * 1024);

    Writer w;
    memset(&w, 0, sizeof(w));
    w.out = out;
    name_table_init(&w.names);
    w.resolver = sym_resolver_new();

    uint64_t total_slices = 0;
    for (int i = 0; i < num_inputs; i++) {
        if (process_file(&w, inputs[i], &total_slices) < 0) {
            fprintf(stderr, "Error processing %s\n", inputs[i]);
        }
    }

    if (out != stdout) fclose(out);
    free(iobuf);

    fprintf(stderr, "Wrote %lu packets (%lu slices) to %s\n",
            (unsigned long)w.packets, (unsigned long)total_slices,
            output_path ? output_path : "<stdout>");

    pbuf_free(&w.tmp_packet);
    pbuf_free(&w.tmp_track_event);
    pbuf_free(&w.tmp_track_descriptor);
    pbuf_free(&w.tmp_process);
    pbuf_free(&w.tmp_thread);
    pbuf_free(&w.tmp_interned);
    pbuf_free(&w.tmp_event_name);
    name_table_free(&w.names);
    sym_resolver_free(w.resolver);
    free(w.tracks.tracks);
    free(w.known_pids);
    free(inputs);
    return 0;
}
