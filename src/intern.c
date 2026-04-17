/*
 * Intern table and string table for cppfunctrace.
 *
 * Design: open-addressing hash table keyed by a void* function pointer.
 * - Lookups are lock-free: they atomically load the `key` slot with acquire
 *   ordering; if the slot is NULL → not present, if it matches → return
 *   func_id. Entries are never mutated or deleted once inserted, so a
 *   concurrent insert that publishes `key` last (release store) is safe.
 * - Inserts are serialised via a mutex in CppFuncTracer.cold_mutex. This
 *   path is rare — one insert per unique function pointer over the life
 *   of the process.
 *
 * The table does NOT grow. We allocate enough capacity up front to cover
 * any realistic C/C++ binary (configurable; default 1 M entries → 16 MiB).
 */

#define _GNU_SOURCE
#include "cppfunctrace_internal.h"

#include <stdlib.h>
#include <string.h>

#define NO_INST __attribute__((no_instrument_function))

static inline uint32_t hash_ptr(void* p, uint32_t mask) NO_INST;
static inline uint32_t
hash_ptr(void* p, uint32_t mask)
{
    /* Fibonacci-ish mix — function pointers alias strongly in their low
     * bits because of alignment, so the upper bits carry the entropy. */
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (uint32_t)v & mask;
}

int NO_INST
ft_intern_init(struct InternTable* t, uint32_t initial_capacity)
{
    uint32_t cap = 1;
    while (cap < initial_capacity) cap <<= 1;
    if (cap < 1024) cap = 1024;

    t->entries = (struct InternEntry*)calloc(cap, sizeof(struct InternEntry));
    if (!t->entries) return -1;
    t->capacity = cap;
    atomic_store_explicit(&t->count, 0, memory_order_relaxed);
    return 0;
}

void NO_INST
ft_intern_free(struct InternTable* t)
{
    free(t->entries);
    t->entries = NULL;
    t->capacity = 0;
    atomic_store_explicit(&t->count, 0, memory_order_relaxed);
}

uint32_t NO_INST
ft_intern_lookup(struct InternTable* t, void* key)
{
    uint32_t mask = t->capacity - 1;
    uint32_t idx = hash_ptr(key, mask);

    for (;;) {
        struct InternEntry* e = &t->entries[idx];
        void* k = atomic_load_explicit(&e->key, memory_order_acquire);
        if (k == NULL) return 0;
        if (k == key) return e->func_id;
        idx = (idx + 1) & mask;
    }
}

int NO_INST
ft_intern_insert_locked(struct InternTable* t, void* key, uint32_t func_id)
{
    /* Caller holds cold_mutex. The table is sized so it never fills; we
     * assert on > 75% load as a sanity check. */
    uint32_t count = atomic_load_explicit(&t->count, memory_order_relaxed);
    if (count * 4 >= t->capacity * 3) {
        /* Table is effectively full — ignore new symbols. Returning -1
         * tells the caller to emit events with func_id=0 ("<unknown>"). */
        return -1;
    }

    uint32_t mask = t->capacity - 1;
    uint32_t idx = hash_ptr(key, mask);
    for (;;) {
        struct InternEntry* e = &t->entries[idx];
        void* k = atomic_load_explicit(&e->key, memory_order_relaxed);
        if (k == key) return 0;                 /* raced with another writer */
        if (k == NULL) {
            e->func_id = func_id;
            /* Publish the key last with release ordering so readers that
             * load key with acquire see func_id correctly. */
            atomic_store_explicit(&e->key, key, memory_order_release);
            atomic_fetch_add_explicit(&t->count, 1, memory_order_relaxed);
            return 0;
        }
        idx = (idx + 1) & mask;
    }
}

/* ── String table ─────────────────────────────────────────────────── */

#define ST_INITIAL_CAP (64 * 1024)

int NO_INST
ft_string_table_init(struct StringTable* st)
{
    st->len = 0;
    st->cap = ST_INITIAL_CAP;
    st->data = (char*)malloc(st->cap);
    return st->data ? 0 : -1;
}

void NO_INST
ft_string_table_free(struct StringTable* st)
{
    free(st->data);
    st->data = NULL;
    st->len = st->cap = 0;
}

int NO_INST
ft_string_table_append(struct StringTable* st, const char* s, uint32_t len)
{
    size_t need = st->len + 4 + len;
    if (need > st->cap) {
        size_t new_cap = st->cap;
        while (new_cap < need) new_cap *= 2;
        char* nd = (char*)realloc(st->data, new_cap);
        if (!nd) return -1;
        st->data = nd;
        st->cap = new_cap;
    }
    memcpy(st->data + st->len, &len, 4);
    memcpy(st->data + st->len + 4, s, len);
    st->len += 4 + len;
    return 0;
}
