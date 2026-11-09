#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
W=$(cd .. && pwd)
PREFIX=${X4_HOST_MBEDTLS_PREFIX:-"$W/host-prefix"}
ENTROPY=${X4_HOST_ENTROPY_SOURCE:-"$W/host-mbedtls-support/entropy_getrandom.c"}
[[ -f "$PREFIX/lib/libmbedtls.a" && -f "$PREFIX/lib/libmbedx509.a" && -f "$PREFIX/lib/libmbedcrypto.a" && -f "$ENTROPY" ]]
BUILD=${X4_TLS_TEST_OUTPUT_DIR:-"$W/tmp/tls-conn-tests"}
[[ ! -e "$BUILD" ]] || { echo 'Preserve prior TLS test output; choose a new output directory' >&2; exit 2; }
mkdir -p "$BUILD"
modes=(plain)
if [[ ${X4_TEST_SANITIZERS:-1} == 1 ]]; then modes+=(sanitized); fi
for mode in "${modes[@]}"; do
  flags=()
  if [[ $mode == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
  "${CC:-cc}" -std=gnu11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
    -DX4_HTTP_TEST -DX4_TLS_HOST -DX4_PUBLIC=0 -pthread -fPIC \
    -I"$PREFIX/include" "${flags[@]}" ${X4_TEST_CFLAGS:-} \
    tests/tls_conn_test.c src/auth/tls_conn.c src/auth/ca_bundle.c "$ENTROPY" \
    "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a" \
    -pthread -Wl,--wrap=sigaction -o "$BUILD/tls-conn-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$BUILD/tls-conn-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$BUILD/tls-conn-$mode" --sigaction-fail
done
