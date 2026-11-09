#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-rtc-recovery43.XXXXXX")
trap 'rm -f -- "$out"/*; rmdir -- "$out"' EXIT
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE=1
    -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include")
for public in 0 1; do
    for mode in plain asan; do
        extra=()
        if [[ $mode == asan ]]; then extra=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie); fi
        "${CC:-clang}" "${flags[@]}" "${extra[@]}" -DX4_PUBLIC="$public" tests/rtc_recovery43_transport_test.c \
            src/streaming/recovery43_sdp.c src/streaming/video_repair43.c src/streaming/stream_profile.c src/streaming/stream_sdp_bandwidth.c \
            -Wl,--gc-sections -o "$out/$public-$mode"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/$public-$mode"
    done
done
