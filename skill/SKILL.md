---
name: cppfunctrace
description: Capture and analyze function-level traces of C or C++ programs. Use when the user wants to profile a native binary, understand its call structure, find hot functions or long-tail calls, or hand a Perfetto trace back to the user. The skill re-builds the target with `-finstrument-functions`, links `libcppfunctrace`, runs a traced invocation, and converts the binary `.ftrc` output to a native Perfetto protobuf trace analysable via `trace_processor` SQL.
---

# cppfunctrace — function-level tracing for C/C++

This skill wraps [cppfunctrace](https://github.com/thomasdullien/cppfunctrace),
a compile-time tracer that instruments every function entry/exit via
GCC/clang's `-finstrument-functions` and writes a 12-byte binary event
stream. Symbol resolution is deferred to offline conversion, so the
runtime overhead is ~50–100 ns per event and zero malloc/dladdr.

## When to invoke this skill

Trigger on any of:

- "profile this C / C++ program"
- "which function takes the most time in <binary>?"
- "give me a flamegraph / call trace for <binary>"
- "why is this binary slow?"
- "trace this under Perfetto"
- "show me the call structure when <binary> does X"

Do **not** invoke for: managed-runtime profiling (Python, JS, JVM,
.NET), dynamic binary instrumentation of a pre-built third-party
binary the user cannot rebuild, or kernel-level tracing.

## Preconditions

Abort and report to the user if any of these fail:

1. **The target must be rebuildable from source.**
   `-finstrument-functions` is a compile flag; a prebuilt binary
   cannot be traced without recompilation.
2. **A working C/C++ toolchain is on PATH** (`cc` + `make`).
3. **Linux** (uses `CLOCK_MONOTONIC`, `gettid`, `dl_iterate_phdr`,
   `prctl(PR_GET_NAME)`). macOS and Windows are not supported.

## Workflow

### Step 1 — install cppfunctrace

```bash
eval "$(./skill/scripts/install.sh)"
```

The script clones the repo to `$CPPFT_HOME` (default
`~/.cache/cppfunctrace`), runs `make`, and prints `export` lines for
`CPPFT_LIB_DIR`, `CPPFT_INCLUDE_DIR`, `CPPFT_BIN_DIR` and a `PATH`
extension. `eval` them so subsequent steps can find `ftrc2perfetto`
and the shared library. Re-runs are cheap — `make` is a no-op when
nothing changed.

### Step 2 — rebuild the target with instrumentation

Detect the build system and inject the right flags. Helpers:

```bash
./skill/scripts/enable-build.sh --env       # generic shell flags
./skill/scripts/enable-build.sh --cmake     # CMakeLists.txt snippet
./skill/scripts/enable-build.sh --autoconf  # ./configure --extra-cflags=…
```

Build-system cheatsheet (the skill emits these exact fragments):

| System              | How to apply                                        |
|---------------------|-----------------------------------------------------|
| **plain Makefile**  | `make CFLAGS="$CPPFT_CFLAGS $CFLAGS" LDFLAGS="$CPPFT_LDFLAGS $LDFLAGS" LDLIBS="$CPPFT_LIBS $LDLIBS"` |
| **autoconf** (ffmpeg-style) | `./configure --extra-cflags="$CPPFT_CFLAGS" --extra-ldflags="$CPPFT_LDFLAGS" --extra-libs="$CPPFT_LIBS"` |
| **CMake**           | `target_compile_options/..._link_libraries` on the executable target |
| **Bazel**           | `copts`, `linkopts`, `deps` on the target rule     |
| **Meson**           | `c_args`, `link_args`, `dependencies` on the target |

Prefer `-O0 -g` for readable names and line info. If the workload is
too slow, fall back to `-O2 -g -fno-inline-functions` (still traced,
just less noisy) or `-finstrument-functions-after-inlining`.

Rebuild from scratch to make sure every TU is instrumented.

### Step 3 — record a trace

```bash
./skill/scripts/trace.sh --out /tmp/mytrace -- ./target_binary [args...]
```

On success: `/tmp/mytrace/trace.perfetto-trace`, plus raw `.ftrc`
files under `/tmp/mytrace/raw/` if the user wants to re-convert.

Default runtime knobs (override via env var):

- `CPPFUNCTRACE_BUFFER_SIZE=268435456` (256 MiB per buffer, 4 in ring)
- `CPPFUNCTRACE_ROLLOVER=2147483648`   (2 GiB per file, then rotate)
- `CPPFUNCTRACE_OUTPUT_DIR`             (set by the script)
- `CPPFUNCTRACE_DEFER=1`                (don't auto-start; require explicit `cppfunctrace_start()`)
- `CPPFUNCTRACE_DISABLE=1`              (no-op on this invocation)

### Step 4 — analyze

Canned summary (requires `pip install perfetto`):

```bash
./skill/scripts/analyze.py /tmp/mytrace/trace.perfetto-trace
```

Prints: slice count, trace span, top functions by count and time,
per-module breakdown, per-thread breakdown, slowest single calls.
For a custom query:

```bash
./skill/scripts/analyze.py /tmp/mytrace/trace.perfetto-trace \
    --sql "SELECT name, dur FROM slice WHERE name LIKE '%encode%' ORDER BY dur DESC LIMIT 20"
```

For the user to load into <https://ui.perfetto.dev>: hand over
`/tmp/mytrace/trace.perfetto-trace`.

## SQL query library

Ready-to-paste snippets for common analyses. `dur` is nanoseconds,
`ts` is the start timestamp on the trace's default clock.

```sql
-- Self-time per function (exclusive of callees).
WITH children AS (
  SELECT parent_id, SUM(dur) AS child_dur
  FROM slice WHERE parent_id IS NOT NULL
  GROUP BY parent_id
)
SELECT s.name,
       SUM(s.dur - COALESCE(c.child_dur, 0)) AS self_dur,
       COUNT(*) AS calls
FROM slice s LEFT JOIN children c ON c.parent_id = s.id
GROUP BY s.name ORDER BY self_dur DESC LIMIT 20;

-- Deepest call stacks in the trace.
SELECT depth, COUNT(*) AS n FROM slice GROUP BY depth ORDER BY depth DESC LIMIT 10;

-- Functions called from >1 thread (cross-thread hot paths).
SELECT s.name, COUNT(DISTINCT thread.utid) AS threads, COUNT(*) AS calls
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread ON tt.utid = thread.utid
GROUP BY s.name HAVING threads > 1 ORDER BY calls DESC LIMIT 20;

-- Functions that run only on a worker thread (not main).
SELECT DISTINCT s.name FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread ON tt.utid = thread.utid
WHERE thread.name LIKE 'worker%' OR thread.is_main_thread = 0;
```

## Common pitfalls

- **No `.ftrc` produced.** The target wasn't built with
  `-finstrument-functions`, or it was but the library link was
  missing. Re-check Step 2; `ldd <binary>` must show
  `libcppfunctrace.so`.
- **Everything named `@<module>+0x<offset>`.** `ftrc2perfetto`
  couldn't open the module to parse `.symtab` — check that the
  binary and any shared libraries shown in the name are still on
  disk at the path embedded in the trace.
- **Static functions missing from trace.** Static functions *are*
  traced; they just need `.symtab` (not stripped) to be named.
  Don't run `strip` on the binary.
- **First few events look slow.** `pthread_create` and libc init
  both run before `main`; not a bug. Filter out `ts` under the first
  few ms if visualisation is distracting.
- **Huge `.ftrc` files / slow run.** Tracing inflates runtime
  significantly for numerical inner loops. Compile hot TUs with
  `-fno-instrument-functions` to carve them out:
  ```
  CFLAGS="$CPPFT_CFLAGS $CFLAGS"  # global
  noisy.c: CFLAGS+=-fno-instrument-functions
  ```
  or annotate individual functions:
  ```
  int inner(int) __attribute__((no_instrument_function));
  ```
- **Multi-process workloads.** Each child writes its own
  `<pid>.ftrc`; pass them all to `ftrc2perfetto` in one invocation
  so process descriptors deduplicate.
- **Fork detection.** After `fork()` the child silently stops
  tracing. If you need to trace the child, do the work before
  `fork()` or have the child call `cppfunctrace_start()` explicitly.

## What this skill does *not* do

- Stack sampling (this is exhaustive instrumentation; use `perf
  record` for sampling).
- Dynamic instrumentation of unmodifiable binaries (use DynamoRIO /
  Pin / eBPF uprobes for that).
- Source-level attribution with line numbers (emit DWARF and use
  `addr2line` post-hoc on the raw addresses if needed).

## Reference

- Repo:      <https://github.com/thomasdullien/cppfunctrace>
- Sibling:   <https://github.com/thomasdullien/pyfasttrace> (same binary format, Python)
- Perfetto:  <https://ui.perfetto.dev>, <https://perfetto.dev/docs/analysis/trace-processor>
