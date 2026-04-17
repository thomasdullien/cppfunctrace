# cppfunctrace

Function-level tracing for C/C++ programs compiled with
`-finstrument-functions`. Binary output matches pyfasttrace's `.ftrc`
v3 format so downstream tooling (libftrc, perf-viz-merge, etc.) is
reusable. A companion `ftrc2perfetto` tool converts `.ftrc` directly
to Perfetto's native protobuf format — no Chrome Trace JSON in the
middle.

## Coding style

- C11, `_GNU_SOURCE`. The library is built with
  `-fno-instrument-functions`; every tracer-internal function also
  carries `__attribute__((no_instrument_function))` as defence in
  depth. Never introduce a function into `src/cppfunctrace.c`,
  `src/intern.c`, or `src/symresolve.c` without that attribute.
- **No data races.** The hot path runs on every instrumented function
  entry/exit across all threads. Shared state uses `_Atomic` with
  explicit memory ordering; the only lock on the hot path is nothing.
  Insert paths serialise through `CppFuncTracer.cold_mutex`. Do not
  relax ordering "just to see if it's faster" without benchmarking and
  a read of the reasoning in the header.
- Name resolution is **deferred**: the tracer writes raw
  `@<module>+0x<offset>` names; `ftrc2perfetto` resolves them offline
  via each ELF's `.symtab`. Don't push dladdr/demangle back into the
  hot path.

## Build

```bash
make                    # libcppfunctrace.so, .a, ftrc2perfetto in build/
make test               # end-to-end smoke tests (test_simple, test_threaded)
```

## Key files

- `src/cppfunctrace.c`           — tracer core (hooks, double buffer, writer thread, module map)
- `src/cppfunctrace_internal.h`  — binary layout (BufferHeader, BinaryEvent)
- `include/cppfunctrace.h`       — public C API
- `src/intern.c`                 — lock-free lookup / mutex-guarded insert hash table
- `src/libftrc.c`, `libftrc.h`   — vendored reader from pyfasttrace
- `src/symresolve.c`             — offline ELF `.symtab`-based resolver
- `src/ftrc2perfetto.c`          — hand-rolled Perfetto protobuf encoder

## Verified

Validated end-to-end against FFmpeg transcoding RGB → MPEG4 MP4 with
`-finstrument-functions`: 200k slices, 1544 unique functions, 10
threads (`enc0:0:mpeg4`, `vf#0:0`, `dec0:0:rawvideo`, …), load cleanly
into Perfetto's `trace_processor`.
