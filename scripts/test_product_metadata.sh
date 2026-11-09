#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} \
    tests/product_metadata_test.c src/ui/product_metadata.c src/ui/image_area.c src/auth/json.c \
    src/auth/public_image_policy.c -lm -o "$build_dir/product-metadata-test"
"$build_dir/product-metadata-test"
