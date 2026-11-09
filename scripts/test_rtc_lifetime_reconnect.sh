#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
for public in 0 1; do
 for mode in plain sanitized; do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -D_POSIX_C_SOURCE=200809L -DX4_STREAM_PROFILE=2 -DX4_RECEIVE_DIAG_HOST_TEST
  -DX4_PUBLIC="$public"
  -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include")
 if [[ $mode == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
 "${CC:-gcc}" "${flags[@]}" tests/rtc_lifetime_reconnect_test.c src/streaming/stream_profile.c src/streaming/stream_startup.c \
  src/streaming/keyframe_limiter.c src/auth/json.c src/streaming/rtc_receive_diag.c -Wl,--gc-sections -o "$out/lifetime-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/lifetime-$mode" "$out/result.json"
 python3 - "$out/result.json" <<'PY'
import json,pathlib,re,sys
text=pathlib.Path(sys.argv[1]).read_text();data=json.loads(text)
assert data['reconnects']==1
assert data['reconnection']==dict(attempts=2,omitted=0,records=[
 dict(cause='manual',completed=True,recovered=False,first_video_ms=0),
 dict(cause='no_video',completed=True,recovered=True,first_video_ms=114)])
assert not re.search(r'\b(?:\d{1,3}\.){3}\d{1,3}\b',text)
assert all(key not in text for key in ('candidate:','token','password','accessKey'))
print('PASS actual public/Lab exporter: cumulative reconnect totals and bounded fixed causes/timings, no addresses or credentials')
PY
 done
done
