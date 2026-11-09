#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/x4-t33-roster.XXXXXXXX")
flags=()
if [[ -n ${X4_TEST_CFLAGS:-} ]]; then read -r -a flags <<< "$X4_TEST_CFLAGS"; fi
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -DX4_PUBLIC=0 -DX4_RUMBLE=1 \
    -I"$root/tests/controller_roster_stubs" -I"$root/tests/host_stubs" "${flags[@]}" \
    "$root/tests/controller_roster33_test.c" "$root/src/input/rumble.c" -o "$out/test"
"$out/test"
