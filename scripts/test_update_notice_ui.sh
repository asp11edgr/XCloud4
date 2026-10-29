#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf -- "$build_dir"' EXIT
for public in 1 0;do
    edition=public;[[ $public == 0 ]]&&edition=lab
    "${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
        ${X4_TEST_CFLAGS:-} -DX4_PUBLIC="$public" -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -Itests/host_stubs \
        tests/test_update_notice_ui.c src/ui/apple_ui.c src/ui/access_country.c src/ui/font.c src/ui/lang.c \
        src/ui/stream_settings.c src/ui/picture_settings.c src/ui/server_settings.c src/ui/canvas.c src/streaming/stream_profile.c \
        third_party/qrcodegen/qrcodegen.c -Wl,--wrap=x4_text_px -Wl,--wrap=x4_text_ellipsis_px \
        -pthread -lm -o "$build_dir/update-notice-$edition"
    destination=()
    if [[ -n ${1:-} ]];then mkdir -p "$1/$edition";destination=("$1/$edition");fi
    "$build_dir/update-notice-$edition" "${destination[@]}"
done
