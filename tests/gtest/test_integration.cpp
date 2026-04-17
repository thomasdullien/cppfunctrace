/*
 * End-to-end integration tests that build on the existing
 * tests/test_simple.c and tests/test_threaded.c workloads.
 *
 * For each workload we:
 *   1. Run the pre-built binary with a scratch output directory.
 *   2. Open every .ftrc file with libftrc.
 *   3. Aggregate per-function / per-thread event counts and stack
 *      balance data.
 *   4. Assert on exact counts derived from the workload (for fib(n)
 *      the number of calls is deterministic) and on structural
 *      invariants (matched BEGIN/END, no negative durations, thread
 *      names include "worker-N" for the threaded run).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "libftrc.h"
#include "symresolve.h"
}

namespace {

struct AggregatedTrace {
    std::map<std::string, uint64_t> by_name;       /* name → complete count   */
    std::map<uint64_t, uint64_t>    by_tid;        /* os_tid → complete count */
    std::vector<std::string>        thread_names;
    uint64_t                        complete_events = 0;
    uint64_t                        neg_durations  = 0;
    uint64_t                        raw_events     = 0;
};

/* List all files under dir with the given extension. */
static std::vector<std::string>
find_files(const std::string& dir, const std::string& ext)
{
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name.size() >= ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            out.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    return out;
}

/* Remove "<path>" (non-recursive directory contents). */
static void
clear_dir(const std::string& dir)
{
    mkdir(dir.c_str(), 0755);
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string path = dir + "/" + de->d_name;
        unlink(path.c_str());
    }
    closedir(d);
}

/* Run a binary with CPPFUNCTRACE_OUTPUT_DIR set; return exit status. */
static int
run_workload(const std::string& exe, const std::string& out_dir)
{
    std::string cmd =
        "CPPFUNCTRACE_OUTPUT_DIR=" + out_dir + " " + exe + " >/dev/null 2>&1";
    return std::system(cmd.c_str());
}

static AggregatedTrace
aggregate(const std::string& trace_dir)
{
    AggregatedTrace a;
    auto files = find_files(trace_dir, ".ftrc");

    /* Tracer stores raw "@<module>+0x<offset>" names — resolve them
     * here so assertions can refer to plain symbol names. */
    sym_resolver* resolver = sym_resolver_new();

    for (const auto& f : files) {
        ftrc_reader* r = ftrc_open(f.c_str());
        if (!r) continue;
        ftrc_event ev;
        while (ftrc_next(r, &ev) == 0) {
            if (ev.type == FTRC_EVENT_METADATA) {
                if (ev.name_len == 11 &&
                    std::memcmp(ev.name, "thread_name", 11) == 0 &&
                    ev.meta_value && ev.meta_value[0]) {
                    a.thread_names.emplace_back(
                        ev.meta_value, ev.meta_value + ev.meta_value_len);
                }
                continue;
            }
            a.complete_events++;
            if (static_cast<int64_t>(ev.dur_us) < 0) a.neg_durations++;

            std::string name;
            char resolved[1024];
            if (resolver && ev.name_len >= 2 && ev.name[0] == '@' &&
                sym_resolver_lookup(resolver, ev.name, ev.name_len,
                                      resolved, sizeof(resolved))) {
                name = resolved;
            } else {
                name.assign(ev.name, ev.name + ev.name_len);
            }
            a.by_name[name]++;
            a.by_tid[ev.tid]++;
        }
        a.raw_events += ftrc_raw_event_count(r);
        ftrc_close(r);
    }
    sym_resolver_free(resolver);
    return a;
}

/* Look up by keyword substring (matches "@<mod>+0x..." raw names). */
static uint64_t
sum_matching(const std::map<std::string, uint64_t>& m, const char* needle)
{
    uint64_t s = 0;
    for (const auto& kv : m) {
        if (kv.first.find(needle) != std::string::npos) s += kv.second;
    }
    return s;
}

static std::string
build_dir()
{
    const char* p = std::getenv("CPPFT_BUILD_DIR");
    return p ? std::string(p) : std::string("build");
}

/* ── Reference counts for test_simple (see tests/test_simple.c) ──
 *
 * main -> do_work(10):
 *   for i in 0..10:  fib(10 + (i&3))  +  sum_squares(1000)
 *
 * Number of fib invocations for fib(n):
 *   C(n) = 1 + C(n-1) + C(n-2),  C(0)=C(1)=1
 *   C(10)=177, C(11)=287, C(12)=465, C(13)=753
 * (i&3) cycles 0,1,2,3,0,1,2,3,0,1 over the 10 rounds. */
TEST(Integration, SimpleWorkloadMatchesExpectedCounts) {
    const std::string bin = build_dir() + "/test_simple";
    const std::string dir = build_dir() + "/gtest-traces-simple";
    clear_dir(dir);
    ASSERT_EQ(run_workload(bin, dir), 0);

    AggregatedTrace t = aggregate(dir);
    EXPECT_GT(t.complete_events, 0u);
    EXPECT_EQ(t.neg_durations, 0u);

    EXPECT_EQ(sum_matching(t.by_name, "main"),        1u);
    EXPECT_EQ(sum_matching(t.by_name, "do_work"),     1u);
    EXPECT_EQ(sum_matching(t.by_name, "sum_squares"), 10u);

    /* fib counts per round:
     *   0,1,2,3,0,1,2,3,0,1 → 177,287,465,753,177,287,465,753,177,287 = 3828 */
    EXPECT_EQ(sum_matching(t.by_name, "fib"), 3828u);
}

/* ── Reference counts for test_threaded (tests/test_threaded.c) ──
 *
 * 4 worker threads, main also runs nothing extra.
 * Each worker does accumulate(1000), which calls compute() 1000 times.
 *   -> compute: 4000, accumulate: 4, worker: 4, main: 1. */
TEST(Integration, ThreadedWorkloadMatchesExpectedCounts) {
    const std::string bin = build_dir() + "/test_threaded";
    const std::string dir = build_dir() + "/gtest-traces-threaded";
    clear_dir(dir);
    ASSERT_EQ(run_workload(bin, dir), 0);

    AggregatedTrace t = aggregate(dir);
    EXPECT_GT(t.complete_events, 0u);
    EXPECT_EQ(t.neg_durations, 0u);

    EXPECT_EQ(sum_matching(t.by_name, "main"),       1u);
    EXPECT_EQ(sum_matching(t.by_name, "worker"),     4u);
    EXPECT_EQ(sum_matching(t.by_name, "accumulate"), 4u);
    EXPECT_EQ(sum_matching(t.by_name, "compute"),    4000u);

    /* 1 main thread + 4 workers = 5 TIDs with events. */
    EXPECT_EQ(t.by_tid.size(), 5u);
}

TEST(Integration, ThreadedWorkloadNamesWorkers) {
    const std::string bin = build_dir() + "/test_threaded";
    const std::string dir = build_dir() + "/gtest-traces-threaded-names";
    clear_dir(dir);
    ASSERT_EQ(run_workload(bin, dir), 0);

    AggregatedTrace t = aggregate(dir);

    int worker_named = 0;
    for (const auto& n : t.thread_names) {
        if (n.rfind("worker-", 0) == 0) worker_named++;
    }
    /* All 4 workers rename themselves before making enough calls to
     * trigger the PR_GET_NAME poll, so we expect 4 worker-N names. */
    EXPECT_EQ(worker_named, 4);
}

TEST(Integration, BalancedEntryExitPerThread) {
    /* libftrc's pull loop matches entry→exit pairs per (chunk, tid).
     * For an unbalanced trace, the raw_events count would be far
     * larger than 2 * complete_events. We allow a handful of
     * synthetic/rollover events as slack. */
    const std::string bin = build_dir() + "/test_simple";
    const std::string dir = build_dir() + "/gtest-traces-balance";
    clear_dir(dir);
    ASSERT_EQ(run_workload(bin, dir), 0);

    AggregatedTrace t = aggregate(dir);
    EXPECT_NEAR((double)t.raw_events,
                2.0 * (double)t.complete_events,
                32.0);
}

}  // namespace
