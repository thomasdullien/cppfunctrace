/*
 * cppfunctrace — public C API.
 *
 * Link your program with -lcppfunctrace and compile with
 * -finstrument-functions (or -finstrument-functions-after-inlining) to
 * get a .ftrc file under $CPPFUNCTRACE_OUTPUT_DIR (default /tmp/cppfunctrace).
 *
 * The library auto-starts via a constructor on load and auto-flushes on
 * exit. Use cppfunctrace_start()/cppfunctrace_stop() to bound the traced
 * region programmatically.
 *
 * Environment variables:
 *   CPPFUNCTRACE_OUTPUT_DIR   output directory (default /tmp/cppfunctrace)
 *   CPPFUNCTRACE_BUFFER_SIZE  bytes per buffer (default 256 MiB)
 *   CPPFUNCTRACE_ROLLOVER     file-rollover threshold in bytes (0 = off)
 *   CPPFUNCTRACE_DISABLE      if set and non-empty, tracing never starts
 *   CPPFUNCTRACE_DEFER        if set and non-empty, do NOT auto-start on
 *                             library load — wait for cppfunctrace_start()
 *   CPPFUNCTRACE_TRACE_CHILDREN  if set and non-empty, fork children
 *                             rebuild tracer state on first post-fork
 *                             hook and write their own <child_pid>.ftrc
 *                             (default: children silently stop tracing)
 */

#ifndef CPPFUNCTRACE_H
#define CPPFUNCTRACE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Begin/resume recording events. Safe to call multiple times. */
void cppfunctrace_start(void) __attribute__((no_instrument_function));

/* Stop recording and flush all pending buffers synchronously. */
void cppfunctrace_stop(void) __attribute__((no_instrument_function));

/* Return the path of the currently-active .ftrc file (NUL-terminated,
 * internal storage — do not free). Returns NULL if tracing never started. */
const char* cppfunctrace_output_path(void)
    __attribute__((no_instrument_function));

#ifdef __cplusplus
}
#endif

#endif /* CPPFUNCTRACE_H */
