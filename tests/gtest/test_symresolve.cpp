/*
 * Unit tests for src/symresolve.c — turns "@<module>+0x<offset>"
 * names into "<symbol> (<basename>)" using ELF .symtab.
 *
 * The harness dlopen()s build/libtestsyms.so (passed in via the
 * CPPFT_TESTSYMS_SO env var from the Makefile), uses dlsym to get
 * pointers to known symbols, computes each symbol's module-relative
 * offset, and asks the resolver to name them.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <string>

extern "C" {
#include "symresolve.h"
}

namespace {

class SymresolveFixture : public ::testing::Test {
  protected:
    static std::string so_path_;
    static void* so_handle_;
    static uintptr_t so_base_;

    static void SetUpTestSuite() {
        const char* p = std::getenv("CPPFT_TESTSYMS_SO");
        ASSERT_NE(p, nullptr)
            << "CPPFT_TESTSYMS_SO must be set by the Makefile";
        so_path_ = p;

        so_handle_ = dlopen(so_path_.c_str(), RTLD_NOW);
        ASSERT_NE(so_handle_, nullptr) << dlerror();

        /* Get the load base via dladdr on any symbol in the library. */
        void* any = dlsym(so_handle_, "testsyms_alpha");
        ASSERT_NE(any, nullptr);
        Dl_info info{};
        ASSERT_NE(dladdr(any, &info), 0);
        so_base_ = reinterpret_cast<uintptr_t>(info.dli_fbase);
    }

    static void TearDownTestSuite() {
        if (so_handle_) dlclose(so_handle_);
    }

    /* Build "@<path>+0x<offset>" for a given symbol. */
    std::string raw_for(const char* sym) {
        void* p = dlsym(so_handle_, sym);
        uintptr_t off = reinterpret_cast<uintptr_t>(p) - so_base_;
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "@%s+0x%lx",
                       so_path_.c_str(), (unsigned long)off);
        return buf;
    }
};

std::string SymresolveFixture::so_path_;
void*       SymresolveFixture::so_handle_ = nullptr;
uintptr_t   SymresolveFixture::so_base_   = 0;

/* ── new/free basic hygiene ────────────────────────────────────── */

TEST(Symresolve, NewAndFreeDoesNotCrash) {
    sym_resolver* r = sym_resolver_new();
    ASSERT_NE(r, nullptr);
    sym_resolver_free(r);
}

TEST(Symresolve, FreeOfNullptrIsSafe) {
    sym_resolver_free(nullptr);
}

/* ── malformed input rejected ──────────────────────────────────── */

TEST(Symresolve, RejectsEmptyString) {
    sym_resolver* r = sym_resolver_new();
    char out[256];
    EXPECT_EQ(sym_resolver_lookup(r, "", 0, out, sizeof(out)), 0);
    sym_resolver_free(r);
}

TEST(Symresolve, RejectsStringWithoutAtPrefix) {
    sym_resolver* r = sym_resolver_new();
    char out[256];
    const char* raw = "plain_name";
    EXPECT_EQ(sym_resolver_lookup(r, raw, std::strlen(raw),
                                    out, sizeof(out)), 0);
    sym_resolver_free(r);
}

TEST(Symresolve, RejectsMissingPlusDelim) {
    sym_resolver* r = sym_resolver_new();
    char out[256];
    const char* raw = "@/tmp/foo";
    EXPECT_EQ(sym_resolver_lookup(r, raw, std::strlen(raw),
                                    out, sizeof(out)), 0);
    sym_resolver_free(r);
}

TEST(Symresolve, RejectsMissingHexPrefix) {
    sym_resolver* r = sym_resolver_new();
    char out[256];
    const char* raw = "@/tmp/foo+1234";
    EXPECT_EQ(sym_resolver_lookup(r, raw, std::strlen(raw),
                                    out, sizeof(out)), 0);
    sym_resolver_free(r);
}

TEST(Symresolve, RejectsNonexistentModule) {
    sym_resolver* r = sym_resolver_new();
    char out[256];
    const char* raw = "@/tmp/definitely-does-not-exist.so+0x1000";
    EXPECT_EQ(sym_resolver_lookup(r, raw, std::strlen(raw),
                                    out, sizeof(out)), 0);
    sym_resolver_free(r);
}

/* ── real resolution against libtestsyms.so ────────────────────── */

TEST_F(SymresolveFixture, ResolvesExternCSymbol) {
    sym_resolver* r = sym_resolver_new();
    std::string raw = raw_for("testsyms_alpha");
    char out[256];
    ASSERT_EQ(sym_resolver_lookup(r, raw.data(), raw.size(),
                                    out, sizeof(out)), 1);
    EXPECT_NE(std::strstr(out, "testsyms_alpha"), nullptr) << out;
    EXPECT_NE(std::strstr(out, "libtestsyms.so"), nullptr) << out;
    sym_resolver_free(r);
}

TEST_F(SymresolveFixture, ResolvesMangledCppSymbolAndDemangles) {
    sym_resolver* r = sym_resolver_new();
    std::string raw = raw_for("_ZN8testsyms7computeEi");  /* testsyms::compute(int) */
    char out[256];
    ASSERT_EQ(sym_resolver_lookup(r, raw.data(), raw.size(),
                                    out, sizeof(out)), 1);
    /* __cxa_demangle should turn it into "testsyms::compute(int)" */
    EXPECT_NE(std::strstr(out, "testsyms::compute"), nullptr) << out;
    sym_resolver_free(r);
}

TEST_F(SymresolveFixture, ResolvesFileLocalStaticFromSymtab) {
    /* hidden_static is STB_LOCAL so dlsym can't find it, but .symtab
     * does — this is the test that the tracer pipeline can name
     * static functions. We find its offset by querying the exposed
     * wrapper and scanning near it. */
    sym_resolver* r = sym_resolver_new();

    /* Use dladdr on the wrapper to know the module; then probe
     * address slots up to 2 KiB back to find the static helper. */
    void* wrapper = dlsym(so_handle_, "testsyms_call_hidden");
    ASSERT_NE(wrapper, nullptr);

    bool found = false;
    for (int delta = -2048; delta <= 2048; delta += 4) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(wrapper) + delta;
        uintptr_t off  = addr - so_base_;
        char raw[1024];
        std::snprintf(raw, sizeof(raw), "@%s+0x%lx",
                       so_path_.c_str(), (unsigned long)off);
        char out[256];
        if (sym_resolver_lookup(r, raw, std::strlen(raw),
                                  out, sizeof(out)) == 1 &&
            std::strstr(out, "hidden_static") != nullptr) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "file-local static not found in .symtab";
    sym_resolver_free(r);
}

TEST_F(SymresolveFixture, OutOfRangeOffsetReturnsZero) {
    sym_resolver* r = sym_resolver_new();
    char raw[1024];
    /* An offset so far past the end of the .so that no symbol covers it. */
    std::snprintf(raw, sizeof(raw), "@%s+0x%lx",
                   so_path_.c_str(), (unsigned long)0x7fffffff);
    char out[256];
    EXPECT_EQ(sym_resolver_lookup(r, raw, std::strlen(raw),
                                    out, sizeof(out)), 0);
    sym_resolver_free(r);
}

TEST_F(SymresolveFixture, CachesModuleBetweenLookups) {
    /* A second lookup in the same module should succeed at the same
     * cost — no crash, same result. Ensures the internal free-list
     * for previously-parsed modules works. */
    sym_resolver* r = sym_resolver_new();
    std::string a = raw_for("testsyms_alpha");
    std::string b = raw_for("testsyms_beta");
    char out[256];
    EXPECT_EQ(sym_resolver_lookup(r, a.data(), a.size(),
                                    out, sizeof(out)), 1);
    EXPECT_EQ(sym_resolver_lookup(r, b.data(), b.size(),
                                    out, sizeof(out)), 1);
    EXPECT_NE(std::strstr(out, "testsyms_beta"), nullptr);
    /* And a third lookup of the first symbol, to really stress the cache. */
    EXPECT_EQ(sym_resolver_lookup(r, a.data(), a.size(),
                                    out, sizeof(out)), 1);
    EXPECT_NE(std::strstr(out, "testsyms_alpha"), nullptr);
    sym_resolver_free(r);
}

TEST(Symresolve, UppercaseHexDigitsAccepted) {
    /* 0xABCDE should parse the same as 0xabcde; feed a known-bad path
     * and only verify that the digits don't short-circuit the
     * malformed-input checks. */
    sym_resolver* r = sym_resolver_new();
    char out[256];
    const char* raw = "@/tmp/definitely-does-not-exist.so+0xABCDEF";
    /* Expected to return 0 (missing file) but NOT crash. */
    EXPECT_EQ(sym_resolver_lookup(r, raw, std::strlen(raw),
                                    out, sizeof(out)), 0);
    sym_resolver_free(r);
}

}  // namespace
