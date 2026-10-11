#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Actual shared reorder and grace helpers; synthetic clocks/worker admission.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-grace-reorder.XXXXXX")
cleanup() { rm -f -- "$build_dir"/*; rmdir -- "$build_dir"; }
trap cleanup EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -I"$repo_dir/src/media"
    "$repo_dir/tests/test_nack_grace_reorder.c" "$repo_dir/src/media/nack_grace.c"
    "$repo_dir/src/media/rtp_reorder.c")
"$compiler" --version
"$compiler" "${common[@]}" -o "$build_dir/plain"
"$build_dir/plain"
printf 'int main(void) { return 0; }\n' >"$build_dir/sanitizer-probe.c"
if "$compiler" "$build_dir/sanitizer-probe.c" -fsanitize=address,undefined \
    -o "$build_dir/probe" >"$build_dir/probe-build.txt" 2>&1; then
    "$build_dir/probe"
    "$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -o "$build_dir/asan"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/asan"
else
    cat "$build_dir/probe-build.txt"
    echo 'UNAVAILABLE ASan/UBSan toolchain; not a sanitizer pass'
fi
