#!/usr/bin/env bash
# enable-build.sh — emit the compile / link flags a target project
# needs to pick up cppfunctrace instrumentation.
#
# Usage:
#   enable-build.sh [--cflags|--ldflags|--libs|--env|--cmake|--autoconf]
#
# Meant to be sourced or eval'd by the agent when editing a build
# system. Requires CPPFT_LIB_DIR / CPPFT_INCLUDE_DIR / CPPFT_BIN_DIR
# in the environment.

set -euo pipefail

if [ -z "${CPPFT_LIB_DIR:-}" ] || [ -z "${CPPFT_INCLUDE_DIR:-}" ]; then
    echo "enable-build.sh: run install.sh first to set CPPFT_LIB_DIR / CPPFT_INCLUDE_DIR" >&2
    exit 2
fi

what=${1:-}

cflags() {
    # -O0 / -fno-inline give the most legible trace; tune if perf matters.
    printf -- '-finstrument-functions -g -I%s' "$CPPFT_INCLUDE_DIR"
}

ldflags() {
    printf -- '-L%s -Wl,-rpath,%s' "$CPPFT_LIB_DIR" "$CPPFT_LIB_DIR"
}

libs() {
    printf -- '-lcppfunctrace -lpthread -ldl'
}

case "$what" in
    ""|--env)
        echo "# Evaluate these in your shell before building:"
        echo "export CPPFT_CFLAGS=\"$(cflags)\""
        echo "export CPPFT_LDFLAGS=\"$(ldflags)\""
        echo "export CPPFT_LIBS=\"$(libs)\""
        echo ""
        echo "# then, for a Makefile project:"
        echo "make CFLAGS=\"\$CPPFT_CFLAGS \$CFLAGS\" \\"
        echo "     CXXFLAGS=\"\$CPPFT_CFLAGS \$CXXFLAGS\" \\"
        echo "     LDFLAGS=\"\$CPPFT_LDFLAGS \$LDFLAGS\" \\"
        echo "     LDLIBS=\"\$CPPFT_LIBS \$LDLIBS\""
        ;;
    --cflags)   cflags; echo ;;
    --ldflags)  ldflags; echo ;;
    --libs)     libs; echo ;;
    --cmake)
        cat <<EOF
# Add to your top-level CMakeLists.txt (near the final target):
target_compile_options(<target> PRIVATE -finstrument-functions -g)
target_include_directories(<target> PRIVATE $CPPFT_INCLUDE_DIR)
target_link_directories(<target> PRIVATE $CPPFT_LIB_DIR)
target_link_libraries(<target> PRIVATE cppfunctrace pthread dl)
set_target_properties(<target> PROPERTIES INSTALL_RPATH "$CPPFT_LIB_DIR")
EOF
        ;;
    --autoconf)
        cat <<EOF
# For a configure-script based project (ffmpeg-style):
./configure \\
    --extra-cflags="$(cflags)" \\
    --extra-ldflags="$(ldflags)" \\
    --extra-libs="$(libs)"
EOF
        ;;
    -h|--help)
        sed -n '2,12p' "$0"
        ;;
    *)
        echo "enable-build.sh: unknown option $what" >&2
        exit 2 ;;
esac
