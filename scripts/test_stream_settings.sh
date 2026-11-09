#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
sdk_include="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0
    -Itests/host_stubs -idirafter "$sdk_include" tests/stream_settings_test.c tests/script_settings_ui_stubs.c src/ui/stream_settings.c
    src/ui/canvas.c src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c src/video/stream_color_policy.c)
quality_common=(-std=c11 -O1 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
    -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs -idirafter "$sdk_include"
    tests/stream_settings_catalog_capture.c tests/catalog_background_stub.c tests/script_settings_ui_stubs.c src/ui/catalog_gui.c src/ui/apple_ui.c src/ui/net_hint.c src/ui/stream_settings.c
    src/ui/canvas.c src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c src/ui/catalog_browser.c
    src/ui/recent_games.c src/ui/product_metadata.c src/ui/image_area.c src/ui/access_country.c src/auth/json.c src/auth/public_image_policy.c third_party/qrcodegen/qrcodegen.c -lm)
"${CC:-clang}" "${common[@]}" ${X4_TEST_CFLAGS:-} -o "$build_dir/settings"
"${CC:-clang}" "${quality_common[@]}" ${X4_TEST_CFLAGS:-} -o "$build_dir/quality"
if [[ -n "${X4_TEST_RENDER_DIR:-}" ]]; then
    mkdir -p "$X4_TEST_RENDER_DIR"
    "$build_dir/settings" assets/fonts/noto_sans.x4font "$X4_TEST_RENDER_DIR"
    "$build_dir/quality" assets/fonts/noto_sans.x4font "$X4_TEST_RENDER_DIR"
else
    "$build_dir/settings" assets/fonts/noto_sans.x4font
    "$build_dir/quality" assets/fonts/noto_sans.x4font
fi
if [[ "${X4_TEST_SANITIZERS:-1}" == 1 ]]; then
    "${CC:-clang}" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -o "$build_dir/settings-sanitized"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/settings-sanitized" assets/fonts/noto_sans.x4font
    "${CC:-clang}" "${quality_common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -o "$build_dir/quality-sanitized"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/quality-sanitized" assets/fonts/noto_sans.x4font
fi
python3 tests/stream_settings_source_test.py
