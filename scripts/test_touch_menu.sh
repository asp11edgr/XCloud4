#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/x4-touch-menu.XXXXXXXX")
trap 'rm -r -- "$tmp_dir"' EXIT
sdk="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
flags=(-std=c23 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror
       -ffunction-sections -fdata-sections -DX4_PUBLIC=0 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0
       -DX4_NET_HUD_TEST=1 -Itests/host_stubs -idirafter "$sdk")
if [[ ${X4_TEST_SANITIZERS:-0} == 1 ]];then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-clang}" "${flags[@]}" tests/touch_menu_test.c src/input/touch_menu.c src/ui/net_hud.c \
  src/ui/font.c src/ui/apple_ui.c src/ui/canvas.c src/ui/lang.c src/ui/net_hint.c \
  third_party/qrcodegen/qrcodegen.c -Wl,--gc-sections -pthread -lm -o "$tmp_dir/test_touch_menu"
"$tmp_dir/test_touch_menu" "$tmp_dir/mode.bin"
