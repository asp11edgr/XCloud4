#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Only the stream_settings_test requested by expansion 4; no catalog suite.
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-script-settings30.XXXXXXXX")
trap 'rm -r -- "$build"' EXIT
sdk="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
"${CC:-clang}" -std=c23 -O1 -g -Wall -Wextra -Werror \
    -DX4_PUBLIC=0 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 \
    -Itests/host_stubs -idirafter "$sdk" \
    tests/stream_settings_test.c tests/script_settings_ui_stubs.c src/ui/stream_settings.c \
    src/ui/canvas.c src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c \
    src/video/stream_color_policy.c -o "$build/settings"
"$build/settings" assets/fonts/noto_sans.x4font
