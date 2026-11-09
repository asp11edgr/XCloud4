#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-remb44c.XXXXXX")
trap 'rm -f -- "$out"/*; rmdir -- "$out"' EXIT
vendor=${X4_LIBDATACHANNEL_SOURCE:?Set X4_LIBDATACHANNEL_SOURCE to the pinned libdatachannel source}
cc=${CC:-clang}
flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -DX4_PUBLIC=1 -DX4_STREAM_PROFILE=1 -DX4_REMB_HOST_TEST -DX4_RECEIVE_DIAG_HOST_TEST
    -Itests/host_stubs -I"$vendor/include" -Isrc/streaming)

run() {
    local label=$1; shift
    printf 'REMB44C public %s\n' "$label"
    "$cc" "${flags[@]}" "$@" tests/remb_public_transport44c_test.c src/streaming/rtc_receive_diag.c \
        -Wl,--gc-sections -o "$out/transport"
    "$out/transport"
    "$cc" "${flags[@]}" "$@" tests/remb_estimator44a_test.c -o "$out/estimator"
    "$out/estimator"
}
run plain
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    run asan-ubsan -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
bash scripts/test_remb_wire.sh --public
bash scripts/test_stream_pipeline_export.sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    X4_TEST_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie" \
    bash scripts/test_stream_pipeline_export.sh
