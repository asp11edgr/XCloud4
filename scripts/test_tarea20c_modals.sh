#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
for public in 0 1; do
    "${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
        ${X4_TEST_CFLAGS:-} -DX4_PUBLIC="$public" -Itests/host_stubs \
        tests/test_tarea20c_modals.c tests/catalog_background_stub.c src/ui/picture_settings.c src/ui/server_settings.c \
        src/ui/catalog_gui.c src/ui/catalog_browser.c src/ui/recent_games.c src/ui/net_hint.c \
        src/ui/apple_ui.c src/ui/canvas.c src/ui/font.c src/ui/lang.c \
        src/streaming/stream_profile.c third_party/qrcodegen/qrcodegen.c \
        -Wl,--wrap=x4_apple_glass_panel -Wl,--wrap=x4_apple_fill_round_rect \
        -lpthread -lm -o "$build_dir/modals-$public"
    if [[ -n "${1:-}" ]]; then
        mkdir -p "$1/public-$public"
        "$build_dir/modals-$public" "$1/public-$public"
    else
        "$build_dir/modals-$public"
    fi
done
