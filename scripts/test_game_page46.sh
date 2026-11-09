#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-game-page46.XXXXXX")
trap 'rm -f -- "$out"/*; rmdir -- "$out"' EXIT

for edition in 0 1; do
    "${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
        -DX4_PUBLIC="$edition" -I. -Itests/host_stubs -ffunction-sections -fdata-sections \
        tests/test_game_page46.c src/ui/game_page.c src/ui/apple_ui.c src/ui/canvas.c \
        src/ui/font.c src/ui/lang.c src/ui/product_metadata.c src/ui/image_area.c \
        src/auth/public_image_policy.c src/auth/json.c third_party/qrcodegen/qrcodegen.c \
        -Wl,--gc-sections -lm -o "$out/game-page-$edition"
    "$out/game-page-$edition"
done
