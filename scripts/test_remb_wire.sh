#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vendor=${X4_LIBDATACHANNEL_SOURCE:?Set X4_LIBDATACHANNEL_SOURCE to the pinned libdatachannel source}
plog=${X4_PLOG_INCLUDE:-$vendor/deps/plog/include}
cc=${CC:-clang}
cxx=${CXX:-clang++}
public=0
if [[ ${1:-} == --public ]]; then
    public=1
    shift
fi
if (( $# )); then
    printf 'usage: %s [--public]\n' "$0" >&2
    exit 2
fi
test "$(git -C "$vendor" rev-parse HEAD)" = bdc5ff28e9d3b863144c94a677ecf5bf043aaf15
work=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-remb-wire.XXXXXX")
trap 'rm -f -- "$work"/*; rmdir -- "$work"' EXIT
cp -- "$repo/src/streaming/rtc_receive_diag.h" "$work/x4_rtc_receive_diag.h"

common=(-O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -I"$repo/src/streaming")
cpp=(-std=c++17 -DX4_OPENORBIS -DX4_PUBLIC="$public" -DX4_RECOVERY43=0 -DRTC_ENABLE_MEDIA=1
    -DRTC_ENABLE_WEBSOCKET=0 -I"$work" -I"$vendor/include" -I"$vendor/include/rtc"
    -I"$vendor/src" -I"$plog")
sources=("$repo/tests/remb_wire_test.cpp" "$repo/src/streaming/rtc_receiver_nack.cpp"
    "$vendor/src/mediahandler.cpp" "$vendor/src/message.cpp" "$vendor/src/rtp.cpp"
    "$vendor/src/rtcpreceivingsession.cpp" "$vendor/src/description.cpp" "$vendor/src/impl/utils.cpp")

run() {
    local label=$1; shift
    printf 'REMB_WIRE public=%s %s\n' "$public" "$label"
    "$cc" -std=c11 "${common[@]}" "$@" -c "$repo/src/streaming/receiver_nack_state.c" -o "$work/$label.o"
    "$cxx" "${common[@]}" "${cpp[@]}" "$@" "${sources[@]}" "$work/$label.o" \
        -Wl,--gc-sections -no-pie -o "$work/$label"
    "$work/$label"
}
run plain
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    run asan-ubsan -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie
if (( ! public )); then
    TSAN_OPTIONS=halt_on_error=1 \
        run tsan -fsanitize=thread -fno-omit-frame-pointer -fno-pie
fi
