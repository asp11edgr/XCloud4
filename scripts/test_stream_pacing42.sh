#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${X4_PACING42_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z "${X4_PACING42_TEST_OUTPUT:-}" ]]; then trap 'rm -rf -- "$out"' EXIT; fi
mkdir -p "$out"
for public in 0 1; do
 "${CC:-clang}" -std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_PUBLIC="$public" \
  ${X4_TEST_CFLAGS:-} tests/stream_pacing42_test.c -o "$out/pacing-$public"
 "$out/pacing-$public" "$out/pacing-$public.json"
 python3 tests/stream_pacing42_analysis_test.py "$out/pacing-$public.json"
done
