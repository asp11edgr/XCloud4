#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
: "${X4_MIC_OPUS_ARCHIVE:?Set X4_MIC_OPUS_ARCHIVE to a host libopus 1.5.2 archive}"
rtc_include="${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include"
[[ -f $rtc_include/rtc/rtc.h && -f $rtc_include/opus/opus.h && -f $X4_MIC_OPUS_ARCHIVE ]]
mode=${1:-plain}
case "$mode" in
    plain) extra=() ;;
    sanitized) extra=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie) ;;
    tsan) extra=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie) ;;
    *) exit 2 ;;
esac
out=$(mktemp -d)
trap 'rm -r -- "$out"' EXIT
common=(-std=gnu2x -O2 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -D_POSIX_C_SOURCE=200809L -Itests/host_stubs -I"$rtc_include" -Wl,--gc-sections)
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1
for public in 0 1; do
    echo "T34 mode=$mode public=$public"
    "${CC:-cc}" "${common[@]}" "${extra[@]}" -DX4_PUBLIC="$public" -DX4_MIC_CAPTURE_TEST \
        tests/mic_capture34_test.c src/audio/mic_capture.c "$X4_MIC_OPUS_ARCHIVE" -lm -o "$out/capture"
    "$out/capture"
    "${CC:-cc}" "${common[@]}" "${extra[@]}" -DX4_PUBLIC="$public" -DX4_RECEIVE_DIAG_HOST_TEST \
        -DX4_MIC_HOST_TEST tests/mic_rtp34_test.c src/audio/mic_rtp.c src/streaming/rtc_receive_diag.c -o "$out/rtp"
    "$out/rtp"
    if [[ $mode == tsan ]]; then continue; fi
    "${CC:-cc}" "${common[@]}" "${extra[@]}" -DX4_PUBLIC="$public" -DX4_RECEIVE_DIAG_HOST_TEST \
        -DX4_STREAM_PROFILE=2 tests/mic_rtp34_transport_test.c src/audio/mic_rtp.c -o "$out/transport"
    "$out/transport"
done
