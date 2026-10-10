#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Requested host-only diagnostics verification; never launches XCloud4.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-monitor-tests.XXXXXX")
cleanup() {
    rm -f -- "$build_dir"/*
    rmdir -- "$build_dir"
}
trap cleanup EXIT
fixture_root=${X4_MONITOR_EXPORT_FIXTURES:-}
if [[ -n "$fixture_root" && ! -d "$fixture_root" ]]; then
    echo "FAIL fixture export directory must already exist" >&2
    exit 2
fi
prepare_fixtures() {
    if [[ -n "$fixture_root" ]]; then
        mkdir -- "$fixture_root/$1"
        export X4_MONITOR_EXPORT_FIXTURES="$fixture_root/$1"
    fi
}
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
    -DX4_MONITOR_HOST_TEST -DX4_MONITOR_TEST_HOOKS \
    -DX4_INGRESS_HOST_TEST -DX4_INGRESS_TEST_HOOKS -I"$repo_dir/src/media" \
    "$repo_dir/tests/progress_monitor_test.c" "$repo_dir/src/media/live_monitor.c" \
    "$repo_dir/src/media/video_ingress.c")
echo "Host progress monitor compiler: $compiler"
compiler_banner=$("$compiler" --version)
echo "$compiler_banner"
echo "BUILD/RUN independent progress monitor host harness"
"$compiler" "${common[@]}" -o "$build_dir/plain"
prepare_fixtures plain
"$build_dir/plain"

if "$compiler" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$build_dir/asan-ubsan" >"$build_dir/asan-build.txt" 2>&1; then
    echo "RUN progress monitor ASan + UBSan"
    prepare_fixtures asan-ubsan
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/asan-ubsan"
else
    cat "$build_dir/asan-build.txt"
    echo "UNAVAILABLE monitor ASan/UBSan build; not a sanitizer pass"
fi

# GCC warns that TSan does not model standalone atomic fences. Keep that
# warning visible and scoped; it must not be mistaken for a native fence proof.
tsan_warning_flags=()
if [[ "${compiler_banner,,}" != *clang* ]]; then
    tsan_warning_flags=(-Wno-error=tsan)
fi
if "$compiler" "${common[@]}" "${tsan_warning_flags[@]}" -fsanitize=thread -fno-omit-frame-pointer \
    -o "$build_dir/tsan" >"$build_dir/tsan-build.txt" 2>&1; then
    cat "$build_dir/tsan-build.txt"
    echo "RUN progress monitor TSan"
    prepare_fixtures tsan
    if TSAN_OPTIONS=halt_on_error=1 "$build_dir/tsan" >"$build_dir/tsan-run.txt" 2>&1; then
        cat "$build_dir/tsan-run.txt"
    else
        status=$?
        cat "$build_dir/tsan-run.txt"
        if grep -Eq 'FATAL: ThreadSanitizer: (unexpected memory mapping|unsupported)' "$build_dir/tsan-run.txt"; then
            echo "UNAVAILABLE monitor TSan runtime; not a race-free result"
        else
            echo "FAIL monitor TSan execution: $status" >&2
            exit "$status"
        fi
    fi
else
    cat "$build_dir/tsan-build.txt"
    echo "UNAVAILABLE monitor TSan build; not a sanitizer pass"
fi
