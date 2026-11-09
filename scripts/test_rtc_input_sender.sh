#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/x4-t33b-rtc.XXXXXXXX")
trap 'rm -rf -- "$out"' EXIT
rtc_include="${X4_RTC_INCLUDE:-${X4_WEBRTC_PREFIX:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc}/prefix}/include}"
test -f "$rtc_include/rtc/rtc.h"
common=(-std=gnu2x -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE=1 -DX4_RUMBLE=1
 -ffunction-sections -fdata-sections -Itests/host_stubs -I"$rtc_include")
if [[ ${1:-} == --bench && $# == 2 ]]; then
 base=$(realpath -- "$2")
 test -f "$base/src/streaming/rtc_transport.c"
 for variant in old new; do
  flags=("${common[@]}" -O2 -DX4_PUBLIC=1)
  tree=.
  if [[ $variant == old ]]; then
   flags+=("-DX4_T37_TRANSPORT_SOURCE=\"$base/src/streaming/rtc_transport.c\"")
   tree=$base
  fi
  "${CC:-gcc}" "${flags[@]}" tests/rtc_input_sender_bench.c "$tree/src/streaming/stream_profile.c" \
   "$tree/src/input/rumble.c" -Wl,--gc-sections -o "$out/bench-$variant"
 done
 cases=(old:1 new:1 new:1:off new:4)
 for run in 0 1 2; do
  for ((i=0;i<${#cases[@]};++i)); do
   IFS=: read -r variant pads option <<<"${cases[(i+run)%${#cases[@]}]}"
   "$out/bench-$variant" "$pads" ${option:+"$option"}
  done
 done
 exit 0
fi
modes=(plain sanitized)
if [[ ${1:-} == --tsan && $# == 1 ]]; then modes=(tsan)
elif [[ $# != 0 ]]; then exit 2
fi
for mode in "${modes[@]}"; do
 flags=("${common[@]}" -O1)
 if [[ $mode == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
 elif [[ $mode == tsan ]]; then flags+=(-fsanitize=thread -fno-omit-frame-pointer -no-pie)
 fi
 "${CC:-gcc}" "${flags[@]}" tests/rtc_input_sender_test.c src/streaming/stream_profile.c src/input/rumble.c \
  -Wl,--gc-sections -o "$out/input-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  TSAN_OPTIONS=halt_on_error=1 "$out/input-$mode"
done
