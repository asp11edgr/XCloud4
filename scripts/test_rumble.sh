#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Host-only parser/mailbox checks explicitly requested for vibration work.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-rumble-tests.XXXXXX")
cleanup() { rm -f -- "$build_dir"/*; rmdir -- "$build_dir"; }
trap cleanup EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -DX4_RUMBLE_TEST_HOOKS
    -I"$repo_dir/src/input" "$repo_dir/tests/rumble_test.c" "$repo_dir/src/input/rumble.c")
"$compiler" --version
"$compiler" "${common[@]}" -o "$build_dir/plain"
"$build_dir/plain"
if "$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$build_dir/asan" >"$build_dir/asan-build.txt" 2>&1; then
    cat "$build_dir/asan-build.txt"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/asan"
else
    cat "$build_dir/asan-build.txt"
    echo "UNAVAILABLE ASan/UBSan build; not a sanitizer pass"
fi
if "$compiler" "${common[@]}" -fsanitize=thread -fno-omit-frame-pointer \
    -o "$build_dir/tsan" >"$build_dir/tsan-build.txt" 2>&1; then
    cat "$build_dir/tsan-build.txt"
    if TSAN_OPTIONS=halt_on_error=1 "$build_dir/tsan" >"$build_dir/tsan-run.txt" 2>&1; then
        cat "$build_dir/tsan-run.txt"
    else
        result=$?; cat "$build_dir/tsan-run.txt"
        if grep -Eq 'FATAL: ThreadSanitizer: (unexpected memory mapping|unsupported)' "$build_dir/tsan-run.txt"; then
            echo "UNAVAILABLE TSan runtime; not a race-free result"
        else exit "$result"; fi
    fi
else
    cat "$build_dir/tsan-build.txt"
    echo "UNAVAILABLE TSan build; not a sanitizer pass"
fi
