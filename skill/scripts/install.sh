#!/usr/bin/env bash
# install.sh — clone and build cppfunctrace in a scratch location.
#
# Usage:
#   install.sh [install_dir]
#
# On success prints three paths on stdout (shell-evalable):
#   export CPPFT_LIB_DIR=...
#   export CPPFT_INCLUDE_DIR=...
#   export CPPFT_BIN_DIR=...
#
# The build has no runtime dependencies beyond libc, libdl, libpthread
# and a working cc. Safe to re-run; skips the clone if the tree is
# already present.

set -euo pipefail

INSTALL_DIR=${1:-${CPPFT_HOME:-$HOME/.cache/cppfunctrace}}
REPO=${CPPFT_REPO:-https://github.com/thomasdullien/cppfunctrace.git}
REV=${CPPFT_REV:-main}

mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

if [ ! -d src ] && [ ! -d .git ]; then
    git clone --depth 1 --branch "$REV" "$REPO" src
fi

if [ ! -d src ] && [ -d .git ]; then
    # User pointed INSTALL_DIR at an existing checkout
    src_dir=$(pwd)
else
    src_dir="$INSTALL_DIR/src"
fi

cd "$src_dir"
make -s all >&2

lib_dir="$src_dir/build"
inc_dir="$src_dir/include"
bin_dir="$src_dir/build"

printf 'export CPPFT_HOME=%q\n'        "$INSTALL_DIR"
printf 'export CPPFT_LIB_DIR=%q\n'     "$lib_dir"
printf 'export CPPFT_INCLUDE_DIR=%q\n' "$inc_dir"
printf 'export CPPFT_BIN_DIR=%q\n'     "$bin_dir"
printf 'export PATH=%q:${PATH}\n'      "$bin_dir"
