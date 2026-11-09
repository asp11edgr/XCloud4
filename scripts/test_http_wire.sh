#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/x4-http-wire.XXXXXXXX")
trap 'rm -r -- "$tmp_dir"' EXIT
flags=(-std=c23 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca)
if [[ ${X4_TEST_SANITIZERS:-0} == 1 ]];then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CC:-clang}" "${flags[@]}" tests/http_wire_test.c src/auth/http_wire.c -o "$tmp_dir/http_wire_test"
"$tmp_dir/http_wire_test"
