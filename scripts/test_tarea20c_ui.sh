#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
cc=${CC:-clang}
flags=(-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror
    -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0)
# Linux must supply its actual mmap flags and time types. The FreeBSD SDK's
# libc headers are only a fallback for Orbis-specific declarations.
includes=(-idirafter "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
    -I. -Isrc -Ithird_party -Itests/host_stubs)
for public in 1 0;do
    edition=public;[[ $public == 0 ]]&&edition=lab
    for hooks in 0 1;do
    # X4_OPENORBIS is deliberately absent: actual loading clock uses host's
    # monotonic clock. No native function or renderer is replaced by a fake.
    # shellcheck disable=SC2086
    "$cc" "${flags[@]}" ${X4_UI_TEST_FLAGS:-} "${includes[@]}" -DX4_PUBLIC="$public" -DX4_UI_SURFACE_TEST_HOOKS="$hooks" \
        tests/test_tarea20c_ui.c tests/catalog_background_stub.c tests/ui_surface_test_trap.c src/ui/ui_surface.c \
        src/ui/screen.c src/ui/apple_ui.c src/ui/canvas.c src/ui/font.c src/ui/lang.c \
        src/ui/net_hint.c src/ui/picture_settings.c src/ui/server_settings.c \
        src/ui/catalog_gui.c src/ui/catalog_browser.c src/ui/recent_games.c src/ui/loading_screen.c src/ui/ui2_theme.c \
        src/ui/product_metadata.c src/ui/image_area.c src/auth/public_image_policy.c src/auth/json.c src/auth/release_check.c \
        src/ui/stream_settings.c src/streaming/stream_profile.c src/input/touch_menu.c \
        third_party/qrcodegen/qrcodegen.c -pthread -lm -o "$build_dir/ui-$edition-$hooks"
    destination=()
    if [[ -n ${1:-} && $hooks == 0 ]];then mkdir -p "$1/$edition";destination=("$1/$edition");fi
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:handle_segv=0 UBSAN_OPTIONS=halt_on_error=1 \
        "$build_dir/ui-$edition-$hooks" "${destination[@]}"
    done
done
