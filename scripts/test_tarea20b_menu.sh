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
"$cc" "${flags[@]}" ${X4_UI_TEST_FLAGS:-} "${includes[@]}" -Dx4_text_px=x4_text_px_impl -Dx4_text_ellipsis_px=x4_text_ellipsis_px_impl -c src/ui/font.c -o "$build_dir/font.o"
for public in 1 0; do
    edition=public; [[ $public == 0 ]] && edition=lab
    # shellcheck disable=SC2086
    "$cc" "${flags[@]}" ${X4_UI_TEST_FLAGS:-} "${includes[@]}" -DX4_PUBLIC="$public" \
        tests/test_tarea20b_menu.c src/ui/screen.c src/ui/apple_ui.c src/ui/canvas.c \
        src/ui/lang.c src/ui/net_hint.c src/ui/picture_settings.c src/ui/server_settings.c \
        src/ui/product_metadata.c src/ui/image_area.c src/auth/public_image_policy.c src/auth/json.c \
        src/streaming/stream_profile.c src/auth/release_check.c src/input/touch_menu.c \
        third_party/qrcodegen/qrcodegen.c "$build_dir/font.o" -pthread -lm -o "$build_dir/menu-$edition"
    objdump -d --disassemble=x4_touch_menu_copy_opaque_row "$build_dir/menu-$edition" > "$build_dir/opaque-copy-$edition.asm"
    if [[ -n ${1:-} ]]; then
        mkdir -p "$1/$edition"
        cp "$build_dir/opaque-copy-$edition.asm" "$1/$edition/opaque-copy.asm"
        "$build_dir/menu-$edition" "$1/$edition"
    else
        cat "$build_dir/opaque-copy-$edition.asm"
        "$build_dir/menu-$edition"
    fi
done
