#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
cc=${CC:-clang}
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror -DX4_STREAM_PROFILE=1)
includes=(-isystem "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include" -I. -Isrc -Ithird_party -Itests/host_stubs)
# shellcheck disable=SC2086
"$cc" "${flags[@]}" ${X4_UI_TEST_FLAGS:-} "${includes[@]}" \
    tests/test_tarea20c_rocket.c src/ui/loading_screen.c src/ui/ui2_theme.c src/ui/apple_ui.c \
    src/ui/font.c src/ui/lang.c src/auth/public_image_policy.c \
    third_party/qrcodegen/qrcodegen.c -pthread -lm -o "$build_dir/rocket"
if [[ -n ${1:-} ]];then mkdir -p "$1";"$build_dir/rocket" "$1";else "$build_dir/rocket";fi
