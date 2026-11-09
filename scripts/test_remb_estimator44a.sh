#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-remb-observation.XXXXXX")
trap 'rm -f -- "$work/estimator" "$work/observer"; rmdir -- "$work"' EXIT
cc=${CC:-clang}
common=(-std=c11 -O2 -g -Wall -Wextra -Werror -I"$root/src/streaming")
run() {
    local label=$1; shift
    printf 'REMB_ESTIMATOR_OBSERVER %s\n' "$label"
    if [[ "$label" != tsan ]]; then
        "$cc" "${common[@]}" "$@" "$root/tests/remb_estimator44a_test.c" -o "$work/estimator"
        "$work/estimator"
    fi
    "$cc" "${common[@]}" "$@" -D_DEFAULT_SOURCE -DX4_PUBLIC=0 -DX4_REMB_HOST_TEST \
        -DX4_RECEIVE_DIAG_HOST_TEST -I"$root/src/media" -pthread \
        "$root/tests/remb_receive44a_test.c" "$root/src/streaming/rtc_receive_diag.c" -o "$work/observer"
    "$work/observer"
}
run plain
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    run asan-ubsan -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
TSAN_OPTIONS=halt_on_error=1 run tsan -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie
