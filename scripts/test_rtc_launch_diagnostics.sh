#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
for public in 0 1; do
 "${CC:-clang}" -std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE=2 -DX4_PUBLIC="$public" \
  -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include" ${X4_TEST_CFLAGS:-} \
  tests/rtc_launch_diagnostics_test.c src/streaming/stream_profile.c src/streaming/stream_startup.c \
  src/streaming/keyframe_limiter.c src/auth/json.c -Wl,--gc-sections -o "$out/launch-logs"
 "$out/launch-logs"
done
