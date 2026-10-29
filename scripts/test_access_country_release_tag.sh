#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for public in 0 1; do
 for mode in plain sanitized; do
  sanitizer_flags=""
  if [[ $mode == sanitized ]]; then sanitizer_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie"; fi
  "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} -DX4_PUBLIC="$public" \
   $sanitizer_flags tests/access_country_release_tag_test.c src/auth/json.c -pthread -o "$build_dir/release-tag-$public-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/release-tag-$public-$mode"
 done
done
