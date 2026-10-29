#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for mode in plain sanitized; do
 sanitizer_flags=""
 if [[ $mode == sanitized ]]; then sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie"; fi
 "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} -DX4_STREAM_PROFILE=1 \
  -Itests/host_stubs $sanitizer_flags tests/access_country_auth_capture_test.c src/auth/json.c \
  src/streaming/stream_profile.c src/streaming/keyframe_limiter.c src/ui/lang.c -pthread \
  -o "$build_dir/access-capture-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/access-capture-$mode"
done
