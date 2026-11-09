#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
W=$(cd .. && pwd)
PREFIX=${X4_HOST_MBEDTLS_PREFIX:-"$W/host-prefix"}
ENTROPY=${X4_HOST_ENTROPY_SOURCE:-"$W/host-mbedtls-support/entropy_getrandom.c"}
BUILD=${1:-"$W/tmp/public-http-local"}
[[ -f "$PREFIX/lib/libmbedtls.a" && -f "$PREFIX/lib/libmbedx509.a" && -f "$PREFIX/lib/libmbedcrypto.a" && -f "$ENTROPY" ]]
mkdir -- "$BUILD"
for mode in plain sanitized; do
    flags=()
    if [[ $mode == sanitized ]]; then
        flags=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -no-pie)
    fi
    "${CC:-cc}" -std=c23 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
        -DX4_HTTP_TEST -DX4_TLS_HOST -DX4_PUBLIC=0 -pthread -fPIC \
        -Itests/host_stubs -I"$PREFIX/include" "${flags[@]}" \
        tests/public_http_local_test.c src/auth/http_client.c src/auth/tls_conn.c \
        src/auth/http_wire.c src/auth/ca_bundle.c src/auth/public_image_policy.c \
        src/ui/access_country.c src/ui/lang.c "$ENTROPY" \
        "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a" \
        -Wl,--wrap=sigaction,--wrap=x4_tls_http_open,--wrap=x4_tls_conn_connect,--wrap=x4_tls_conn_read \
        -o "$BUILD/$mode"
    echo "public HTTP localhost $mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$BUILD/$mode"
done
