/*
 * Test helper shared library with a handful of symbols that the
 * symresolve tests query by offset. Compiled into
 * build/libtestsyms.so.
 *
 * Mix of:
 *   - extern "C" (no C++ mangling)
 *   - namespaced C++ (mangled — exercises __cxa_demangle round-trip)
 *   - file-local static (should appear in .symtab but NOT .dynsym)
 */

#include <cstdint>

extern "C" {
int testsyms_alpha(int x)                   { return x + 1; }
int testsyms_beta (int x)                   { return x * 2; }
}

namespace testsyms {
int compute(int x) { return x * x + 1; }
}  // namespace testsyms

static int hidden_static(int x)             { return x - 1; }

/* Ensure hidden_static is actually emitted: call it from an
 * externally-visible function so the compiler can't elide it. */
extern "C" int testsyms_call_hidden(int x)  { return hidden_static(x); }
