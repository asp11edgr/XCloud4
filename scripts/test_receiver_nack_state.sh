#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Host-only state verification explicitly requested by the owner; no network.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-receiver-nack-tests.XXXXXX")
cleanup() {
    rm -f -- "$build_dir"/*
    rmdir -- "$build_dir"
}
trap cleanup EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror \
    -I"$repo_dir/src/streaming" "$repo_dir/tests/receiver_nack_state_test.c" \
    "$repo_dir/src/streaming/receiver_nack_state.c")
echo "Host receiver NACK state compiler: $compiler"
"$compiler" --version
"$compiler" "${common[@]}" -o "$build_dir/state-plain"
"$build_dir/state-plain"
if "$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$build_dir/state-asan-ubsan" >"$build_dir/asan-build.txt" 2>&1; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/state-asan-ubsan"
else
    cat "$build_dir/asan-build.txt"
    echo "UNAVAILABLE ASan/UBSan host build; not a sanitizer pass"
fi
