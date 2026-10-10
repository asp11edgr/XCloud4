#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Explicitly requested host-only queue verification; never launches XCloud4.
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
command -v "$compiler" >/dev/null || { echo "FAIL compiler unavailable: $compiler" >&2; exit 2; }
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-ingress-tests.XXXXXX")
cleanup() {
    rm -f -- "$build_dir"/*
    rmdir -- "$build_dir"
}
trap cleanup EXIT
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
    -DX4_INGRESS_HOST_TEST -DX4_INGRESS_TEST_HOOKS -I"$repo_dir/src/media" \
    "$repo_dir/tests/video_ingress_test.c" "$repo_dir/src/media/video_ingress.c")
echo "Host ingress compiler: $compiler"
"$compiler" --version
for mode in baseline candidate; do
    mode_flags=()
    if [[ "$mode" == baseline ]]; then mode_flags=(-DX4_INGRESS_BASELINE); fi
    echo "BUILD/RUN $mode host ingress"
    "$compiler" "${common[@]}" "${mode_flags[@]}" -o "$build_dir/$mode-plain"
    "$build_dir/$mode-plain"

    if "$compiler" "${common[@]}" "${mode_flags[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -o "$build_dir/$mode-asan-ubsan" >"$build_dir/$mode-asan-build.txt" 2>&1; then
        echo "RUN $mode ASan + UBSan"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$build_dir/$mode-asan-ubsan"
    else
        cat "$build_dir/$mode-asan-build.txt"
        echo "UNAVAILABLE $mode ASan/UBSan host build; not a sanitizer pass"
    fi

    if "$compiler" "${common[@]}" "${mode_flags[@]}" -fsanitize=thread -fno-omit-frame-pointer \
        -o "$build_dir/$mode-tsan" >"$build_dir/$mode-tsan-build.txt" 2>&1; then
        echo "RUN $mode TSan"
        if TSAN_OPTIONS=halt_on_error=1 "$build_dir/$mode-tsan" >"$build_dir/$mode-tsan-run.txt" 2>&1; then
            cat "$build_dir/$mode-tsan-run.txt"
        else
            status=$?
            cat "$build_dir/$mode-tsan-run.txt"
            if grep -Eq 'FATAL: ThreadSanitizer: (unexpected memory mapping|unsupported)' "$build_dir/$mode-tsan-run.txt"; then
                echo "UNAVAILABLE $mode TSan runtime; not a race-free result"
            else
                echo "FAIL $mode TSan execution: $status" >&2
                exit "$status"
            fi
        fi
    else
        cat "$build_dir/$mode-tsan-build.txt"
        echo "UNAVAILABLE $mode TSan host build; not a sanitizer pass"
    fi
done
