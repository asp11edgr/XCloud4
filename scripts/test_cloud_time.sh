#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(mktemp -d "${TMPDIR:-/tmp}/x4-cloud-time.XXXXXXXX")
trap 'rm -rf "$dir"' EXIT
for mode in plain sanitized; do
    flags=()
    if [[ $mode = sanitized ]]; then
        flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie)
    fi
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror "${flags[@]}" -Isrc \
        tests/cloud_time_test.c src/auth/cloud_time.c src/auth/json.c -o "$dir/$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$dir/$mode"
done
