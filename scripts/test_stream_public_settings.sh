#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_PUBLIC=1 \
    -Itests/host_stubs src/streaming/stream_profile.c src/ui/lang.c src/ui/picture_settings.c src/ui/server_settings.c src/ui/access_country.c src/ui/canvas.c src/ui/font.c src/ui/apple_ui.c third_party/qrcodegen/qrcodegen.c tests/test_stream_public_settings.c \
    -lpthread -lm -o "$build_dir/public_test"
"$build_dir/public_test"

"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_PUBLIC=0 \
    -Itests/host_stubs src/streaming/stream_profile.c src/ui/lang.c tests/test_stream_public_settings.c \
    -o "$build_dir/lab_test"
"$build_dir/lab_test"
