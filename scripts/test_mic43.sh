#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
opus_include=${X4_MIC_OPUS_INCLUDE:-${X4_WEBRTC_ROOT:-$HOME/.local/share/xcloud4/webrtc-clean-17}/prefix/include}
for public in 0 1; do
 "${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -DX4_MIC_CAPTURE_TEST -DX4_PUBLIC="$public" ${X4_TEST_CFLAGS:-} \
  -I"$opus_include" ${X4_MIC_OPUS_CFLAGS:-} tests/mic_capture43_test.c -pthread -Wl,--gc-sections \
  ${X4_MIC_OPUS_ARCHIVE:-} -lm -o "$out/mic-$public"
 "$out/mic-$public"
done
