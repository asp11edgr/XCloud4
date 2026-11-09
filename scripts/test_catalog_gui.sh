#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
ui_test_dir=$(mktemp -d)
trap 'rm -rf -- "$ui_test_dir"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror ${X4_UI_TEST_FLAGS:-} -Itests/host_ui \
    tests/catalog_gui_capture.c tests/catalog_background_stub.c src/ui/catalog_gui.c src/ui/apple_ui.c src/ui/net_hint.c src/ui/lang.c src/ui/font.c src/ui/catalog_browser.c \
    src/ui/recent_games.c src/ui/access_country.c src/ui/product_metadata.c src/ui/image_area.c src/auth/json.c src/auth/public_image_policy.c third_party/qrcodegen/qrcodegen.c \
    -lm -o "$ui_test_dir/gui"
mkdir -p "$2"
"$ui_test_dir/gui" "$1" "$2"
