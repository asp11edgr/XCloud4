#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Explicit one-shot integrated real-host proof. Only the root operator runs this.
set -euo pipefail
ulimit -c 0
[[ ${X4_ALLOW_REAL_TLS_POC:-} == 1 ]] || { echo 'Real-host gate is not enabled' >&2; exit 2; }
cd "$(dirname "$0")/.."
W=$(cd .. && pwd)
PREFIX=${X4_HOST_MBEDTLS_PREFIX:-"$W/host-prefix"}
ENTROPY=${X4_HOST_ENTROPY_SOURCE:-"$W/tools/entropy_getrandom.c"}
BUILD=${X4_TLS_POC_OUTPUT_DIR:-"$W/tmp/tls-http-poc"}
[[ -f "$ENTROPY" && -f "$PREFIX/lib/libmbedtls.a" && ! -e "$BUILD" ]]
mkdir -p "$BUILD"
# Link the final production HTTP code, suppressing only its text logs in this
# host object. The driver reports metadata, never URL/header/body contents.
COMMON=(-std=c23 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror
  -DX4_HTTP_TEST -DX4_PUBLIC=0 -pthread -fPIC -Itests/host_stubs -I"$PREFIX/include")
"${CC:-cc}" "${COMMON[@]}" -Dprintf=poc_quiet_printf -c src/auth/http_client.c -o "$BUILD/http_client.o"
"${CC:-cc}" "${COMMON[@]}" -Dprintf=poc_quiet_printf -c src/auth/tls_conn.c -o "$BUILD/tls_conn.o"
"${CC:-cc}" "${COMMON[@]}" tests/tls_http_poc.c "$BUILD/http_client.o" "$BUILD/tls_conn.o" \
  src/auth/http_wire.c src/auth/ca_bundle.c src/auth/public_image_policy.c src/ui/access_country.c src/ui/lang.c "$ENTROPY" \
  "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a" \
  -Wl,--wrap=x4_tls_http_open,--wrap=x4_tls_http_close \
  -Wl,--wrap=x4_tls_conn_connect,--wrap=x4_tls_conn_close \
  -pthread -o "$BUILD/tls-http-poc"
# No retry loop here. Within this process every approved host has <=2 attempts
# including the RSA+CBC control handshake. Preserve BUILD as a one-shot guard.
"$BUILD/tls-http-poc" "${X4_TLS_POC_RUN_ARG:---approved-integrated-hosts}"
