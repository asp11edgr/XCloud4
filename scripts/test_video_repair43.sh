#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${X4_REPAIR43_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z "${X4_REPAIR43_TEST_OUTPUT:-}" ]]; then trap 'rm -rf -- "$out"' EXIT; fi
mkdir -p "$out"
mode=${1:-all}
for variant in plain sanitized tsan; do
 if [[ $mode != all && $mode != "$variant" ]]; then continue; fi
 flags=()
 if [[ $variant == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
 if [[ $variant == tsan ]]; then flags=(-fsanitize=thread -fno-omit-frame-pointer -no-pie); fi
 "${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
   src/streaming/video_repair43.c tests/test_video_repair43.c -o "$out/repair-$variant"
 echo "REPAIR43_VARIANT=$variant"
 "$out/repair-$variant"
done
