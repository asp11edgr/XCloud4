#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-home-nav46.XXXXXX")
trap 'rm -f -- "$out"/*; rmdir -- "$out"' EXIT

for edition in 0 1; do
    "${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
        -DX4_PUBLIC="$edition" -Itests/host_stubs -isystem "$OO_PS4_TOOLCHAIN/include" \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        tests/ui46_home_nav_test.c src/ui/screen.c -o "$out/home-nav-$edition"
    "$out/home-nav-$edition"
done
