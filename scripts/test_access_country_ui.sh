#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror)
includes=(-Itests/host_stubs -idirafter "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include")
sources=(src/ui/access_country.c src/ui/apple_ui.c src/ui/picture_settings.c src/ui/server_settings.c
    src/ui/canvas.c src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c third_party/qrcodegen/qrcodegen.c)
for public in 1 0;do
    edition=public;[[ $public == 0 ]]&&edition=lab
    "${CC:-clang}" "${flags[@]}" ${X4_TEST_CFLAGS:-} "${includes[@]}" -DX4_PUBLIC="$public" \
        tests/test_access_country_ui.c "${sources[@]}" -Wl,--wrap=rename -pthread -lm -o "$build_dir/ui-$edition"
    "$build_dir/ui-$edition"
    if [[ -n ${1:-} ]];then
        mkdir -p "$1/$edition"
        "${CC:-clang}" "${flags[@]}" ${X4_TEST_CFLAGS:-} "${includes[@]}" -DX4_PUBLIC="$public" \
            -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -ffunction-sections -fdata-sections \
            tests/capture_access_country_ui.c "${sources[@]}" src/ui/screen.c src/ui/catalog_gui.c \
            src/ui/catalog_browser.c src/ui/recent_games.c src/ui/net_hint.c src/ui/stream_settings.c src/input/touch_menu.c \
            -Wl,--gc-sections -pthread -lm -o "$build_dir/capture-$edition"
        "$build_dir/capture-$edition" "$1/$edition"
        if [[ -n ${X4_T22_BASELINE_PICTURE:-} ]];then
            mkdir -p "$1/baseline-$edition"
            before=("${sources[@]}");for i in "${!before[@]}";do
                [[ ${before[$i]} == src/ui/picture_settings.c ]]&&before[$i]=$X4_T22_BASELINE_PICTURE
            done
            "${CC:-clang}" "${flags[@]}" ${X4_TEST_CFLAGS:-} "${includes[@]}" -Isrc/ui \
                -DX4_PUBLIC="$public" -DX4_T22_BASELINE_CAPTURE=1 -ffunction-sections -fdata-sections \
                tests/capture_access_country_ui.c "${before[@]}" src/ui/catalog_gui.c src/ui/catalog_browser.c \
                src/ui/recent_games.c src/ui/net_hint.c src/ui/stream_settings.c -Wl,--gc-sections -pthread -lm -o "$build_dir/before-$edition"
            "$build_dir/before-$edition" "$1/baseline-$edition"
        fi
    fi
done
