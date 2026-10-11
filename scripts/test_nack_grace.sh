#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-clang}; temp=$(mktemp -d)
trap 'rm -f -- "$temp"/*; rmdir -- "$temp"' EXIT
args=(-std=c11 -O1 -g -Wall -Wextra -Werror -I"$repo/src/media"
    "$repo/src/media/nack_grace.c" "$repo/tests/test_nack_grace.c")
"$cc" "${args[@]}" -o "$temp/test"; "$temp/test"
if "$cc" "${args[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -o "$temp/san" >"$temp/san-build.txt" 2>&1; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$temp/san"
else
    cat "$temp/san-build.txt"
    echo "UNAVAILABLE ASan/UBSan build; not a sanitizer pass"
fi
