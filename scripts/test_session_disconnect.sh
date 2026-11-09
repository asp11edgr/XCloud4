#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
compile_flags=()
for flag in ${X4_TEST_CFLAGS:-}; do
 [[ $flag == -no-pie || $flag == -Wl,* ]] || compile_flags+=("$flag")
done
for public in 0 1; do
"${CC:-clang}" -std=c23 -DX4_PUBLIC="$public" -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
 -DX4_STREAM_PROFILE=2 -DX4_RECEIVE_DIAG_HOST_TEST -DX4_T41_LIFETIME_INTEGRATION \
 -Dx4_rtc_snapshot=t41_rtc_snapshot -Dx4_rtc_get_selected_candidate_pair=t41_rtc_unused_candidate_pair \
 -DsceKernelGetProcessTime=t41_rtc_clock -pthread -ffunction-sections -fdata-sections \
 -Itests/host_stubs -I"$rtc_include" "${compile_flags[@]}" -c tests/rtc_lifetime_reconnect_test.c -o "$build_dir/rtc.o"
"${CC:-clang}" -std=c23 -DX4_PUBLIC="$public" -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
 -DX4_STREAM_PROFILE=0 -DX4_STREAM_OS='"Orbis"' -DX4_STREAM_WAIT_HOST=1 \
 -DX4_HTTP_TEST -DX4_T41_LIFETIME_INTEGRATION -pthread -ffunction-sections -fdata-sections -Itests/host_stubs ${X4_TEST_CFLAGS:-} \
 tests/session_disconnect_test.c tests/session_state_http_test.c src/auth/json.c src/auth/http_wire.c src/ui/lang.c \
 src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c "$build_dir/rtc.o" \
 -Wl,--gc-sections -o "$build_dir/disconnect"
"$build_dir/disconnect"
done
