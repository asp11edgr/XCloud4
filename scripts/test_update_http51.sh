#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
PREFIX=${X4_HOST_MBEDTLS_PREFIX:?}
entropy=()
if [[ -n ${X4_HOST_ENTROPY_SOURCE:-} ]]; then entropy=("$X4_HOST_ENTROPY_SOURCE"); fi
libraries=("$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a")
for library in everest p256m; do
    if [[ -f $PREFIX/lib/lib$library.a ]]; then libraries+=("$PREFIX/lib/lib$library.a"); fi
done
BUILD=${1:?build output directory}
BASE=${2:?verified baseline source directory}
mode=${3:-all}
mkdir -p -- "$BUILD"
common=(-std=c23 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror -pthread)
sanitize=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -no-pie)
if [[ $mode == all || $mode == network ]]; then
    for edition in 0 1; do
        for flavor in plain sanitized; do
            flags=()
            if [[ $flavor == sanitized ]]; then flags=("${sanitize[@]}"); fi
            "${CC:-cc}" "${common[@]}" ${X4_TEST_CFLAGS:-} -DX4_HTTP_TEST -DX4_TLS_HOST -DX4_PUBLIC="$edition" -fPIC \
                -Itests/host_stubs -I"$PREFIX/include" "${flags[@]}" \
                tests/update_http51_test.c src/auth/http_client.c src/auth/tls_conn.c \
                src/auth/http_wire.c src/auth/ca_bundle.c src/auth/public_image_policy.c \
                src/ui/access_country.c src/ui/lang.c "${entropy[@]}" \
                "${libraries[@]}" \
                -Wl,--wrap=sigaction,--wrap=x4_tls_http_open,--wrap=x4_tls_conn_connect,--wrap=x4_tls_conn_write \
                -o "$BUILD/network-$edition-$flavor"
            echo "UPDATE51_LOCAL edition=$edition mode=$flavor"
            ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
                "$BUILD/network-$edition-$flavor"
        done
    done
    "$BUILD/network-1-plain" --live
fi
if [[ $mode == all || $mode == parity ]]; then
    for edition in 0 1; do
        for tree in "$BASE" .; do
            version=base
            if [[ $tree == . ]]; then version=update; fi
            "${CC:-cc}" "${common[@]}" ${X4_TEST_CFLAGS:-} -DX4_HTTP_TEST -DX4_TLS_HOST -DX4_PUBLIC="$edition" \
                -Wno-misleading-indentation -Itests/host_stubs \
                tests/http_parity51_test.c "$tree/src/auth/http_client.c" "$tree/src/auth/http_wire.c" \
                "$tree/src/auth/public_image_policy.c" "$tree/src/ui/access_country.c" \
                -Wl,--wrap=x4_tls_conn_write -o "$BUILD/parity-$edition-$version"
        done
        python3 - "$BUILD" "$edition" <<'PY'
import hashlib
import pathlib
import subprocess
import sys
out, edition = pathlib.Path(sys.argv[1]), sys.argv[2]
before, after = [subprocess.check_output([out / f"parity-{edition}-{version}"])
 \
                for version in ("base", "update")]
assert before and before == after, "An existing request changed"
print(f"UPDATE51_EXISTING_WIRE edition={edition} requests=27 bytes={len(after)} "
      f"sha256={hashlib.sha256(after).hexdigest()} identical=true")
PY
    done
fi
