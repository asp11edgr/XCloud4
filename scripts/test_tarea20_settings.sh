#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
    -DX4_PUBLIC=1 -Itests/host_stubs \
    tests/test_tarea20_settings.c src/ui/picture_settings.c src/ui/server_settings.c src/ui/access_country.c \
    src/ui/apple_ui.c src/ui/canvas.c src/ui/font.c src/ui/lang.c \
    src/streaming/stream_profile.c third_party/qrcodegen/qrcodegen.c \
    -Wl,--wrap=x4_text_px -Wl,--wrap=x4_text_ellipsis_px \
    -lpthread -lm -o "$build_dir/settings"
if [[ -n "${1:-}" ]]; then mkdir -p "$1"; fi
"$build_dir/settings" "$@"
