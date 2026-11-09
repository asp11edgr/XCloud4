#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
umask 077
cd "$(dirname "$0")/.."
: "${X4_HOST_MBEDTLS_PREFIX:?Point to the isolated Linux-host Mbed TLS prefix}"
: "${X4_UPDATE_TEST_OUTPUT:?Use a new test directory within the T51 VM workspace}"
prefix=$X4_HOST_MBEDTLS_PREFIX
build=$X4_UPDATE_TEST_OUTPUT
[[ ! -e "$build" ]]
mkdir -p "$build"
python3 -B tests/update_manifest51_fixtures.py "$build/fixtures" "$@"
test_args=("$build/fixtures")
if [[ $# -eq 2 && "$1" == --real-pkg ]]; then test_args+=("$2"); fi
libraries=("$prefix/lib/libmbedcrypto.a")
for name in everest p256m; do
    if [[ -f "$prefix/lib/lib$name.a" ]]; then libraries+=("$prefix/lib/lib$name.a"); fi
done
for mode in plain sanitized; do
    flags=()
    if [[ "$mode" == sanitized ]]; then
        flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
    fi
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca \
        -I"$prefix/include" "${flags[@]}" tests/update_manifest51_test.c \
        src/update/update_manifest.c src/update/pkg_header.c src/update/update_io51.c src/auth/json.c \
        -Wl,--start-group "${libraries[@]}" -Wl,--end-group -pthread -o "$build/$mode"
    printf 'Modo %s\n' "$mode"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build/$mode" "${test_args[@]}"
done
