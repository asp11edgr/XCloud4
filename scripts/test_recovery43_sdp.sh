#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${X4_EDGE43_REFERENCE:?provide sanitized negotiation-only reference}"
out=$(mktemp -d "${TMPDIR:-/tmp}/x4-sdp43.XXXXXX")
trap 'rm -rf -- "$out"' EXIT
for kind in plain sanitized; do
 flags=();if [[ $kind == sanitized ]];then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
 "${CC:-clang}" -std=gnu2x -O1 -g -Wall -Wextra -Werror "${flags[@]}" -DX4_PUBLIC=0 \
  src/streaming/recovery43_sdp.c tests/recovery43_sdp_test.c -o "$out/$kind"
 "$out/$kind" "$X4_EDGE43_REFERENCE"
done
