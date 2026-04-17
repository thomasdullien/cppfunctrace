/*
 * Unit tests for src/intern.c — the pointer-keyed intern table and
 * the growable string table used by the tracer.
 *
 * Written in C++ because gtest needs it; the API under test is C.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

extern "C" {
#include "cppfunctrace_internal.h"
}

namespace {

/* ── ft_intern_init / free ─────────────────────────────────────── */

TEST(Intern, InitRoundsUpCapacityToPowerOfTwo) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 100), 0);
    EXPECT_EQ(t.capacity, 1024u);  // clamped to min 1024
    EXPECT_EQ(t.count.load(), 0u);
    ft_intern_free(&t);

    ASSERT_EQ(ft_intern_init(&t, 5000), 0);
    EXPECT_EQ(t.capacity, 8192u);  // next pow2 above 5000
    ft_intern_free(&t);
}

TEST(Intern, FreeNullsOutFields) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 0), 0);
    ft_intern_free(&t);
    EXPECT_EQ(t.entries, nullptr);
    EXPECT_EQ(t.capacity, 0u);
    EXPECT_EQ(t.count.load(), 0u);
}

/* ── ft_intern_lookup on empty / unknown ───────────────────────── */

TEST(Intern, LookupOnEmptyReturnsZero) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 0), 0);
    EXPECT_EQ(ft_intern_lookup(&t, (void*)0xdeadbeef), 0u);
    ft_intern_free(&t);
}

/* ── Insert then lookup ────────────────────────────────────────── */

TEST(Intern, InsertThenLookupReturnsFuncId) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 0), 0);
    void* key = (void*)0x1000;
    ASSERT_EQ(ft_intern_insert_locked(&t, key, 42), 0);
    EXPECT_EQ(ft_intern_lookup(&t, key), 42u);
    EXPECT_EQ(t.count.load(), 1u);
    ft_intern_free(&t);
}

TEST(Intern, DuplicateInsertIsIdempotent) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 0), 0);
    void* key = (void*)0xaabbccdd;
    ASSERT_EQ(ft_intern_insert_locked(&t, key, 7), 0);
    ASSERT_EQ(ft_intern_insert_locked(&t, key, 99), 0);  // silently ignored
    EXPECT_EQ(ft_intern_lookup(&t, key), 7u);            // first wins
    EXPECT_EQ(t.count.load(), 1u);
    ft_intern_free(&t);
}

TEST(Intern, ManyDistinctKeysRoundTrip) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 8192), 0);
    constexpr uint32_t N = 4000;
    for (uint32_t i = 0; i < N; ++i) {
        void* key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x400000 + i * 8));
        ASSERT_EQ(ft_intern_insert_locked(&t, key, i + 1), 0);
    }
    for (uint32_t i = 0; i < N; ++i) {
        void* key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x400000 + i * 8));
        EXPECT_EQ(ft_intern_lookup(&t, key), i + 1) << "i=" << i;
    }
    EXPECT_EQ(t.count.load(), N);
    ft_intern_free(&t);
}

TEST(Intern, LookupOfUnknownKeyReturnsZeroEvenAfterInserts) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 0), 0);
    for (uint32_t i = 0; i < 10; ++i) {
        ft_intern_insert_locked(&t,
            reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + i)), i + 1);
    }
    EXPECT_EQ(ft_intern_lookup(&t, (void*)0x9999), 0u);
    ft_intern_free(&t);
}

/* ── Capacity limit (75% load factor) ──────────────────────────── */

TEST(Intern, CapacityLimitRejectsFurtherInserts) {
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 0), 0);
    const uint32_t cap = t.capacity;
    const uint32_t limit = cap * 3 / 4;   /* mirrors implementation */

    for (uint32_t i = 0; i < limit; ++i) {
        void* key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x100000 + i));
        ASSERT_EQ(ft_intern_insert_locked(&t, key, i + 1), 0);
    }
    /* One more should fail cleanly. */
    EXPECT_EQ(
        ft_intern_insert_locked(&t, (void*)0xdeadbeef, limit + 1),
        -1);
    ft_intern_free(&t);
}

/* ── Concurrent reads + serialised writes ──────────────────────── */

TEST(Intern, ReadersSeeConsistentEntriesDuringWrites) {
    /* The insert path uses a release store on the key slot so readers
     * (lock-free) either see NULL or a fully-published entry. We spin
     * up many reader threads against a writer inserting in parallel
     * and verify no reader ever sees a partial entry (non-zero
     * func_id with the wrong key). */
    struct InternTable t{};
    ASSERT_EQ(ft_intern_init(&t, 16384), 0);

    constexpr uint32_t N = 5000;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<uint32_t> reader_mismatches{0};

    auto reader = [&]() {
        while (!start.load(std::memory_order_acquire)) {}
        while (!done.load(std::memory_order_acquire)) {
            for (uint32_t i = 0; i < N; ++i) {
                void* key = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(0x800000 + i * 16));
                uint32_t fid = ft_intern_lookup(&t, key);
                if (fid != 0 && fid != i + 1) {
                    reader_mismatches.fetch_add(1);
                }
            }
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back(reader);

    start.store(true, std::memory_order_release);

    /* Serialised writer — the real library holds cold_mutex. */
    for (uint32_t i = 0; i < N; ++i) {
        void* key = reinterpret_cast<void*>(
            static_cast<uintptr_t>(0x800000 + i * 16));
        ASSERT_EQ(ft_intern_insert_locked(&t, key, i + 1), 0);
    }

    done.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();

    EXPECT_EQ(reader_mismatches.load(), 0u);

    /* Post-run: every key resolves to the expected func_id. */
    for (uint32_t i = 0; i < N; ++i) {
        void* key = reinterpret_cast<void*>(
            static_cast<uintptr_t>(0x800000 + i * 16));
        ASSERT_EQ(ft_intern_lookup(&t, key), i + 1);
    }
    ft_intern_free(&t);
}

/* ── String table ──────────────────────────────────────────────── */

TEST(StringTable, InitFreeEmptyState) {
    struct StringTable st{};
    ASSERT_EQ(ft_string_table_init(&st), 0);
    EXPECT_NE(st.data, nullptr);
    EXPECT_EQ(st.len, 0u);
    EXPECT_GT(st.cap, 0u);
    ft_string_table_free(&st);
    EXPECT_EQ(st.data, nullptr);
    EXPECT_EQ(st.cap, 0u);
}

TEST(StringTable, AppendSingleEntry) {
    struct StringTable st{};
    ASSERT_EQ(ft_string_table_init(&st), 0);

    const char* s = "hello";
    uint32_t l = 5;
    ASSERT_EQ(ft_string_table_append(&st, s, l), 0);
    ASSERT_EQ(st.len, 4u + l);

    /* Format: [uint32 len][bytes] */
    uint32_t got_len = 0;
    std::memcpy(&got_len, st.data, 4);
    EXPECT_EQ(got_len, l);
    EXPECT_EQ(0, std::memcmp(st.data + 4, s, l));
    ft_string_table_free(&st);
}

TEST(StringTable, GrowsAcrossManyAppends) {
    struct StringTable st{};
    ASSERT_EQ(ft_string_table_init(&st), 0);
    const size_t initial_cap = st.cap;

    /* Write ~512 KiB of strings so we definitely trigger realloc. */
    std::string payload(256, 'x');
    constexpr int N = 2000;
    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(ft_string_table_append(&st, payload.data(),
                                          (uint32_t)payload.size()), 0);
    }
    EXPECT_GT(st.cap, initial_cap);
    EXPECT_EQ(st.len, N * (4u + payload.size()));

    /* Spot-check: first entry still readable after many growths. */
    uint32_t first_len;
    std::memcpy(&first_len, st.data, 4);
    EXPECT_EQ(first_len, payload.size());
    EXPECT_EQ(0, std::memcmp(st.data + 4, payload.data(), payload.size()));
    ft_string_table_free(&st);
}

TEST(StringTable, AcceptsZeroLengthEntry) {
    struct StringTable st{};
    ASSERT_EQ(ft_string_table_init(&st), 0);
    ASSERT_EQ(ft_string_table_append(&st, "", 0), 0);
    EXPECT_EQ(st.len, 4u);   /* just the length prefix */
    uint32_t got;
    std::memcpy(&got, st.data, 4);
    EXPECT_EQ(got, 0u);
    ft_string_table_free(&st);
}

TEST(StringTable, PreservesOrderingOfEntries) {
    struct StringTable st{};
    ASSERT_EQ(ft_string_table_init(&st), 0);
    const char* strs[] = { "alpha", "beta", "gamma", "delta" };
    for (const char* s : strs) {
        ASSERT_EQ(ft_string_table_append(&st, s, (uint32_t)std::strlen(s)), 0);
    }
    size_t off = 0;
    for (const char* s : strs) {
        uint32_t L;
        std::memcpy(&L, st.data + off, 4);
        EXPECT_EQ(L, std::strlen(s));
        EXPECT_EQ(0, std::memcmp(st.data + off + 4, s, L));
        off += 4 + L;
    }
    ft_string_table_free(&st);
}

}  // namespace
