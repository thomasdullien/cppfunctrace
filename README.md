# cppfunctrace

Low-overhead function-level tracing for C and C++ programs built with
`-finstrument-functions`. Writes [pyfasttrace](https://github.com/thomasdullien/pyfasttrace)-compatible
`.ftrc` files and converts them to Perfetto's native protobuf format
for viewing in [ui.perfetto.dev](https://ui.perfetto.dev) or analysis
with [`trace_processor`](https://perfetto.dev/docs/analysis/trace-processor).

Designed to handle real multi-threaded workloads: validated on a
re-built FFmpeg transcoding a 10-frame video (200 k slices,
1.5 k unique functions, 10 threads — `enc0:0:mpeg4`, `vf#0:0`,
`dec0:0:rawvideo`, …).

## Build

```bash
make                    # build/libcppfunctrace.{so,a}, build/ftrc2perfetto
sudo make install       # → /usr/local (override with PREFIX=)
make test               # end-to-end smoke tests against the bundled binaries
make gtest              # unit + integration tests (fetches googletest locally)
```

## Use

Compile the target with `-finstrument-functions` and link the
library. No `-rdynamic` is needed — the tracer records raw
`@<module>+0x<offset>` names via `dl_iterate_phdr` and
`ftrc2perfetto` resolves them offline from each module's `.symtab`,
so both global and file-local static functions are named in the
final trace.

```bash
cc -O2 -g -finstrument-functions -o myprog src/*.c -lcppfunctrace
CPPFUNCTRACE_OUTPUT_DIR=/tmp/tr ./myprog
ftrc2perfetto -o trace.perfetto-trace /tmp/tr/*.ftrc
```

Load `trace.perfetto-trace` in <https://ui.perfetto.dev>.

### Environment variables

| Variable                       | Default              | Meaning                                                |
|--------------------------------|----------------------|--------------------------------------------------------|
| `CPPFUNCTRACE_OUTPUT_DIR`      | `/tmp/cppfunctrace`  | directory for `.ftrc` files                            |
| `CPPFUNCTRACE_BUFFER_SIZE`     | 256 MiB              | bytes per buffer (×4 in the ring)                      |
| `CPPFUNCTRACE_ROLLOVER`        | 0 (off)              | cumulative bytes before starting a new file            |
| `CPPFUNCTRACE_INTERN_CAPACITY` | 1 M slots            | cap on unique functions                                |
| `CPPFUNCTRACE_DEFER`           | unset                | don't auto-start; require explicit `cppfunctrace_start()` |
| `CPPFUNCTRACE_DISABLE`         | unset                | disable tracing for this run                           |

### Programmatic API

```c
#include <cppfunctrace.h>
cppfunctrace_stop();   /* pause */
...
cppfunctrace_start();  /* resume */
```

Combined with `CPPFUNCTRACE_DEFER=1`, you pay the hook overhead only
inside the region of interest.

### Excluding code from instrumentation

```c
void not_traced(void) __attribute__((no_instrument_function));
```

For a whole translation unit, compile it with `-fno-instrument-functions`.
The tracer library itself is built that way so it can never recurse
through its own hooks.

## Design

- **Hot path** (~50–100 ns/event): atomic `fetch_add` on the write
  offset + 12-byte store. No malloc, no lock, no syscall — even
  symbol resolution is deferred.
- **Cold path** (first-seen function pointer): under a single mutex,
  consult a cached module map (built from `dl_iterate_phdr`) and
  format a raw name `@<module>+0x<offset>`. Dladdr, ELF parsing and
  `__cxa_demangle` all run offline in `ftrc2perfetto` instead.
- **Flush**: a dedicated writer thread drains completed buffers to
  disk. If it falls behind, the hot path drops events rather than
  serialising through a mutex.
- **Rollover**: optional file rotation keeps individual files small
  for long-running workloads.
- **Crash safety**: `SIGTERM` / `SIGABRT` / `SIGSEGV` / `SIGINT` and
  `atexit` trigger an async-signal-safe emergency flush that writes
  the active buffer with `write(2)` alone.

## Binary format

Byte-identical to pyfasttrace `.ftrc` v3 — see
[pyfasttrace's PLAN.md](https://github.com/thomasdullien/pyfasttrace/blob/main/PLAN.md)
for the full layout:

- 12-byte packed events (`ts_delta_us`, `func_id`, `tid_idx`, `flags`)
- `CLOCK_MONOTONIC` timestamps (directly correlatable with
  `perf record -k CLOCK_MONOTONIC`)
- Linux kernel TIDs via `gettid()`
- Per-chunk string table, growing monotonically across chunks

`src/libftrc.[ch]` is vendored from pyfasttrace so `ftrc2perfetto`
reads files produced by either tracer.

## License

MIT, matching pyfasttrace.
