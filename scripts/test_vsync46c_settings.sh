#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
sdk_include=${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
    -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs -idirafter "$sdk_include")
shared=(src/streaming/stream_profile.c src/ui/canvas.c src/ui/font.c src/ui/lang.c)
public=(tests/test_picture_settings.c src/ui/apple_ui.c third_party/qrcodegen/qrcodegen.c
    src/ui/picture_settings.c src/ui/server_settings.c src/ui/access_country.c)
lab=(tests/stream_settings_test.c tests/script_settings_ui_stubs.c src/ui/stream_settings.c
    src/video/stream_color_policy.c)
paths=("-DX4_LAB_RECOVERY_SETTINGS_PATH=\"$build_dir/recovery.bin\""
       "-DX4_LAB_GPU_SETTINGS_PATH=\"$build_dir/gpu.bin\"")

for variant in plain sanitized; do
    extra=()
    if [[ $variant == sanitized ]]; then
        extra=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
    fi
    "${CC:-clang}" "${flags[@]}" "${extra[@]}" -DX4_PUBLIC=1 "${shared[@]}" "${public[@]}" -pthread -lm -o "$build_dir/public"
    "${CC:-clang}" "${flags[@]}" "${extra[@]}" -DX4_PUBLIC=0 "${paths[@]}" "${shared[@]}" "${lab[@]}" -pthread -lm -o "$build_dir/lab"
    (
        cd "$build_dir"
        export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
        ./public --vsync-only
        ./lab --vsync-only
    )
    printf 'PASS T46c VSync settings %s\n' "$variant"
done
