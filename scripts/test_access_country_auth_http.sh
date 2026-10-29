#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
export X4_SESSION_FIXTURE_SETTINGS_PATH="$build_dir/server-settings.bin"
compatibility_flags=""
case "$("${CC:-cc}" --version)" in *[Cc]lang*) compatibility_flags="-Wno-c23-extensions" ;; esac
for profile in 0 1; do
 for mode in plain sanitized; do
  sanitizer_flags=""
  if [[ $mode == sanitized ]]; then sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie"; fi
  "${CC:-cc}" -std=gnu11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror -include strings.h ${X4_TEST_CFLAGS:-} \
   $compatibility_flags -DX4_STREAM_PROFILE="$profile" -DX4_STREAM_WAIT_HOST=1 -ffunction-sections -fdata-sections \
   -Itests/host_stubs $sanitizer_flags tests/access_country_auth_http_test.c src/auth/http_client.c \
   src/auth/ca_bundle.c src/auth/json.c src/auth/xbox_recent.c src/streaming/stream_profile.c \
   src/ui/lang.c src/ui/server_settings.c -Wl,--gc-sections -o "$build_dir/access-http-$profile-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/access-http-$profile-$mode"
 done
done
