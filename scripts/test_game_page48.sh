#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-game-page48.XXXXXX")
trap 'rm -f -- "$out"/*; rmdir -- "$out"' EXIT
read -r -a test_flags <<< "${X4_TEST_CFLAGS:-}"

for edition in 0 1; do
    "${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror "${test_flags[@]}" \
        -DX4_PUBLIC="$edition" -I. -Itests/host_stubs -ffunction-sections -fdata-sections \
        tests/test_game_page48.c src/ui/game_page.c src/ui/favorites.c src/ui/apple_ui.c \
        src/ui/ui2_theme.c src/ui/canvas.c src/ui/font.c src/ui/lang.c src/ui/product_metadata.c \
        src/ui/image_area.c src/auth/public_image_policy.c src/auth/json.c third_party/qrcodegen/qrcodegen.c \
        -Wl,--gc-sections -lm -o "$out/game-page-$edition"
    "$out/game-page-$edition" "$out/favorites-$edition.bin"
done
