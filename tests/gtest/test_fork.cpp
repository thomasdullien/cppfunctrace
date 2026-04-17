/*
 * Fork-safety integration test.
 *
 * Runs tests/test_fork.c in three modes (child exits normally, child
 * raises SIGTERM, child raises SIGABRT). In every mode the inherited
 * signal/atexit handlers in the child must NOT write anything into
 * the parent's .ftrc file — that would be bug #1 from the fork
 * pitfalls analysis.
 *
 * Expectations:
 *   - exactly one .ftrc file exists (the parent's <parent_pid>.ftrc)
 *   - it contains events from both `before_fork` AND `after_fork`
 *     (proves the parent kept tracing after the fork and its buffer
 *     wasn't overwritten or truncated by the child)
 *   - no negative durations, no stale file from the child's PID
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "libftrc.h"
#include "symresolve.h"
}

namespace {

static std::string build_dir() {
    const char* p = std::getenv("CPPFT_BUILD_DIR");
    return p ? std::string(p) : std::string("build");
}

static std::vector<std::string>
find_ftrc(const std::string& dir)
{
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name.size() >= 5 &&
            name.compare(name.size() - 5, 5, ".ftrc") == 0) {
            out.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    return out;
}

static void clear_dir(const std::string& dir) {
    mkdir(dir.c_str(), 0755);
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        std::string p = dir + "/" + de->d_name;
        unlink(p.c_str());
    }
    closedir(d);
}

struct Counts {
    std::map<std::string, uint64_t> by_name;
    uint64_t complete = 0;
    uint64_t raw      = 0;
    uint64_t neg_dur  = 0;
};

/* Count distinct FTRC chunks in the file by scanning for the magic.
 * libftrc silently dedups overlapping chunks, so the event counts
 * alone can't tell whether the child wrote a spurious chunk — but
 * an extra chunk header is visible at the byte level. */
static uint64_t count_chunks(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);
    std::vector<char> buf(size);
    size_t n = std::fread(buf.data(), 1, size, f);
    std::fclose(f);

    uint64_t chunks = 0;
    /* FT_MAGIC = "FTRC" little-endian = 0x43525446 */
    const char want[4] = {'F', 'T', 'R', 'C'};
    for (size_t i = 0; i + 4 <= n; i++) {
        if (std::memcmp(buf.data() + i, want, 4) == 0) chunks++;
    }
    return chunks;
}

static Counts aggregate(const std::string& path) {
    Counts c;
    ftrc_reader* r = ftrc_open(path.c_str());
    if (!r) return c;
    sym_resolver* sr = sym_resolver_new();
    ftrc_event ev;
    while (ftrc_next(r, &ev) == 0) {
        if (ev.type != FTRC_EVENT_COMPLETE) continue;
        c.complete++;
        if (static_cast<int64_t>(ev.dur_us) < 0) c.neg_dur++;
        char out[512];
        std::string name;
        if (ev.name[0] == '@' &&
            sym_resolver_lookup(sr, ev.name, ev.name_len,
                                  out, sizeof(out))) {
            name = out;
        } else {
            name.assign(ev.name, ev.name + ev.name_len);
        }
        c.by_name[name]++;
    }
    c.raw = ftrc_raw_event_count(r);
    sym_resolver_free(sr);
    ftrc_close(r);
    return c;
}

static void run_one(const std::string& mode) {
    const std::string dir = build_dir() + "/gtest-traces-fork-" + mode;
    const std::string bin = build_dir() + "/test_fork";
    clear_dir(dir);

    /* Expected parent behaviour: exit 0, child status != 0 for signal modes. */
    std::string cmd =
        "CPPFUNCTRACE_OUTPUT_DIR=" + dir + " " + bin + " " + mode +
        " >/dev/null 2>&1";
    int status = std::system(cmd.c_str());
    ASSERT_EQ(status, 0) << "parent exited non-zero in mode=" << mode;

    auto files = find_ftrc(dir);
    ASSERT_EQ(files.size(), 1u) << "expected exactly one .ftrc (parent's), got "
                                 << files.size() << " in mode=" << mode;

    Counts c = aggregate(files[0]);
    EXPECT_GT(c.complete, 0u) << mode;
    EXPECT_EQ(c.neg_dur, 0u)  << mode;

    /* Parent ran before_fork() 100x and after_fork() 50x. With trace
     * enabled both should appear. If the child had corrupted the
     * parent's buffer, after_fork would be missing. */
    uint64_t before = 0, after = 0;
    for (const auto& kv : c.by_name) {
        if (kv.first.find("before_fork") != std::string::npos) before += kv.second;
        if (kv.first.find("after_fork")  != std::string::npos) after  += kv.second;
    }
    EXPECT_EQ(before, 100u) << "mode=" << mode;
    EXPECT_EQ(after,  50u)  << "mode=" << mode;

    /* The real bug: a forked child's inherited signal/atexit handlers
     * would append a chunk to the parent's file containing the
     * pre-fork buffer contents. libftrc's dedup hides this at the
     * event-count level but the extra chunk header is visible in the
     * raw file. Expect exactly one chunk (the parent's final flush). */
    uint64_t chunks = count_chunks(files[0]);
    EXPECT_EQ(chunks, 1u) << "unexpected extra chunk from child in mode=" << mode;

    /* And the raw pre-dedup event count should match 2x complete
     * (every event is one entry + one exit). With the bug, the
     * duplicated chunk roughly doubles raw events for before_fork. */
    EXPECT_EQ(c.raw, 2 * c.complete) << "mode=" << mode;
}

TEST(Fork, NormalExitDoesNotCorruptParentTrace)    { run_one("exit");    }
TEST(Fork, SigtermInChildDoesNotCorruptParentTrace) { run_one("sigterm"); }
TEST(Fork, SigabrtInChildDoesNotCorruptParentTrace) { run_one("sigabrt"); }

}  // namespace
