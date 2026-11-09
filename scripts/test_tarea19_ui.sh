#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

"${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror -DX4_PUBLIC=1 ${X4_TEST_CFLAGS:-} \
    -ffunction-sections -fdata-sections \
    -isystem "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include" \
    -I. -Isrc -Ithird_party -Itests/host_stubs \
    tests/test_tarea19_ui.c tests/catalog_background_stub.c \
    src/ui/apple_ui.c src/ui/screen.c src/ui/catalog_gui.c src/ui/catalog_browser.c \
    src/ui/recent_games.c src/ui/product_metadata.c src/ui/image_area.c src/ui/canvas.c src/ui/font.c \
    src/ui/lang.c src/ui/net_hint.c src/ui/access_country.c src/ui/server_settings.c src/ui/picture_settings.c \
    src/streaming/stream_profile.c src/auth/release_check.c src/auth/json.c \
    src/auth/public_image_policy.c third_party/qrcodegen/qrcodegen.c \
    -Wl,--gc-sections -lpthread -lm -o "$build_dir/test_tarea19_ui"

"$build_dir/test_tarea19_ui"
