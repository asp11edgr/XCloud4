#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
"${CC:-clang}" -std=c23 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
 -DX4_STREAM_PROFILE=0 -DX4_STREAM_OS='"Orbis"' -DX4_RUMBLE=0 \
 -ffunction-sections -fdata-sections -Itests/host_stubs ${X4_TEST_CFLAGS:-} \
 tests/restore_retry_test.c src/auth/json.c -Wl,--gc-sections -o "$tmp/restore"
rc=0
"$tmp/restore" >"$tmp/stdout.log" 2>"$tmp/stderr.log" || rc=$?
if grep -Eq 'FAKE_ACCESS_|FAKE_REFRESH_|Bearer ' "$tmp/stdout.log" "$tmp/stderr.log"; then
  echo 'FAIL fake credential in output' >&2; exit 1
fi
cat "$tmp/stdout.log" "$tmp/stderr.log"

exit "$rc"
