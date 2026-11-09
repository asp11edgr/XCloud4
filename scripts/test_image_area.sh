#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
    tests/image_area_test.c src/ui/image_area.c -lm -o "$build_dir/image-area-test"
"$build_dir/image-area-test"
