#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/x4-auth-language.XXXXXXXX")
trap 'rm -r -- "$build_dir"' EXIT
python3 -B tests/auth_stage_translation_source_test.py
for public in 0 1; do
  "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_PUBLIC="$public" \
    tests/auth_stage_translation_test.c -o "$build_dir/language-$public"
  "$build_dir/language-$public"
  if [ "${X4_TEST_SANITIZERS:-1}" = 1 ]; then
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -DX4_PUBLIC="$public" \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      tests/auth_stage_translation_test.c -o "$build_dir/language-$public-san"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 "$build_dir/language-$public-san"
  fi
done
