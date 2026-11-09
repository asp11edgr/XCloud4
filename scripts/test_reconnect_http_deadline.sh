#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
"${CC:-clang}" -std=c23 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
 -DX4_STREAM_PROFILE=0 -DX4_STREAM_OS='"Orbis"' -DX4_STREAM_WAIT_HOST=1 \
 -DX4_HTTP_TEST -pthread -ffunction-sections -fdata-sections -Itests/host_stubs ${X4_TEST_CFLAGS:-} \
 tests/reconnect_http_deadline_test.c src/auth/json.c src/auth/http_wire.c -Wl,--gc-sections -o "$tmp/reconnect-http"
"$tmp/reconnect-http"
