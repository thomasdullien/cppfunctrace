#!/usr/bin/env bash
# trace.sh — run a command under cppfunctrace and produce a Perfetto trace.
#
# Usage:
#   trace.sh [--out DIR] -- <cmd> [args...]
#
# Requires CPPFT_LIB_DIR and CPPFT_BIN_DIR in the environment (export
# via skill/scripts/install.sh). The target binary must already have
# been built with -finstrument-functions and linked against
# libcppfunctrace — see skill/scripts/enable-build.sh for build-system
# integration helpers.
#
# Outputs:
#   <out>/raw/<pid>*.ftrc           — raw binary traces
#   <out>/trace.perfetto-trace      — merged Perfetto protobuf
#   <out>/stdout.log, stderr.log    — captured output

set -euo pipefail

OUT_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --out)   OUT_DIR="$2"; shift 2 ;;
        --)      shift; break ;;
        -h|--help)
            sed -n '2,16p' "$0"; exit 0 ;;
        *)       break ;;
    esac
done

if [ $# -eq 0 ]; then
    echo "trace.sh: no command provided" >&2
    exit 2
fi

if [ -z "${CPPFT_LIB_DIR:-}" ] || [ -z "${CPPFT_BIN_DIR:-}" ]; then
    echo "trace.sh: CPPFT_LIB_DIR / CPPFT_BIN_DIR not set — run install.sh first" >&2
    exit 2
fi

if [ -z "$OUT_DIR" ]; then
    OUT_DIR="$(mktemp -d -t cppft-trace-XXXXXX)"
fi
mkdir -p "$OUT_DIR/raw"

# Make the lib findable by the dynamic loader regardless of rpath.
export LD_LIBRARY_PATH="$CPPFT_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CPPFUNCTRACE_OUTPUT_DIR="$OUT_DIR/raw"

# Reasonable defaults for an unknown workload. Users can override by
# setting the env var before invoking the script.
: "${CPPFUNCTRACE_BUFFER_SIZE:=268435456}"     # 256 MiB
: "${CPPFUNCTRACE_ROLLOVER:=2147483648}"       # 2 GiB per file, then rotate
export CPPFUNCTRACE_BUFFER_SIZE CPPFUNCTRACE_ROLLOVER

echo "==> tracing: $*" >&2
echo "    output : $OUT_DIR" >&2
echo "    buffer : $CPPFUNCTRACE_BUFFER_SIZE bytes / rollover $CPPFUNCTRACE_ROLLOVER" >&2

set +e
"$@" >"$OUT_DIR/stdout.log" 2>"$OUT_DIR/stderr.log"
status=$?
set -e

ftrc_files=( "$OUT_DIR"/raw/*.ftrc )
if [ ! -e "${ftrc_files[0]}" ]; then
    echo "trace.sh: no .ftrc files produced — was the binary built with -finstrument-functions?" >&2
    exit 3
fi

"$CPPFT_BIN_DIR/ftrc2perfetto" \
    -o "$OUT_DIR/trace.perfetto-trace" \
    "${ftrc_files[@]}" >&2

echo "==> wrote $OUT_DIR/trace.perfetto-trace (exit=$status)" >&2
printf '%s\n' "$OUT_DIR/trace.perfetto-trace"
exit $status
