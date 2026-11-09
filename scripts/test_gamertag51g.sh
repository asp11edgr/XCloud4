#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
build=${1:?test output directory}
mkdir -p -- "$build"
build=$(realpath "$build")
cc=${CC:-cc}
read -r -a extra <<< "${X4_TEST_CFLAGS:-}"
for public in 0 1; do
    for mode in plain sanitized tsan; do
        flags=()
        if [[ $mode == sanitized ]]; then
            flags=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -fno-pie -no-pie)
        elif [[ $mode == tsan ]]; then
            flags=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie)
        fi
        prefix="$build/gamertag-$public-$mode"
        "$cc" -std=c23 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
            -Wno-misleading-indentation -pthread -ffunction-sections -fdata-sections \
            "${extra[@]}" "${flags[@]}" -DX4_STREAM_PROFILE=0 -DX4_STREAM_OS='"Orbis"' -DX4_VIDEO_PROBE=0 \
            -DX4_PUBLIC="$public" -DX4_HTTP_TEST -DX4_TLS_HOST -DX4_XBOX_GAMERTAG_TEST \
            -Itests/host_stubs tests/gamertag51g_test.c src/auth/http_client.c src/auth/http_wire.c \
            src/auth/cloud_time.c src/auth/json.c src/auth/public_image_policy.c src/ui/access_country.c \
            -Wl,--gc-sections -o "$prefix"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            TSAN_OPTIONS=halt_on_error=1 timeout 45s "$prefix" >"$prefix.log" 2>&1
        if grep -Eq 'fixture-|ube51g' "$prefix.log"; then exit 1; fi
        if ((public)); then
            if grep -q 'llegada_ms' "$prefix.log"; then exit 1; fi
        else
            test "$(grep -c '^XCloud4: XSTS xboxlive.com llegada_ms=0 claims=\[' "$prefix.log")" = 2
            grep -Fxq 'XCloud4: XSTS xboxlive.com llegada_ms=0 claims=[gtg,xid]' "$prefix.log"
            grep -Fxq 'XCloud4: XSTS xboxlive.com llegada_ms=0 claims=[mgt,xid]' "$prefix.log"
        fi
        printf 'T51G_GAMERTAG_TEST edition=%s mode=%s\n' "$public" "$mode"
        cat "$prefix.log"
    done
done
