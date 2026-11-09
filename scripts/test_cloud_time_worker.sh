#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
cc=${CC:-clang}
mode=${1:-plain}
case "$mode" in
    plain) flags=() ;;
    sanitized) flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie) ;;
    tsan) flags=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie) ;;
    *) exit 2 ;;
esac
common=(-std=gnu2x -D_POSIX_C_SOURCE=200809L -DX4_STREAM_PROFILE=0 -DX4_STREAM_OS='"Orbis"'
    -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections
    -Itests/host_stubs -pthread -Wl,--gc-sections)
run_private() {
    local log=$1 rc=0
    shift
    "$@" >"$log" 2>&1 || rc=$?
    if grep -Eq 'fixture[A-Za-z]+Private|2468101214' "$log"; then
        printf '%s\n' 'FAIL quota test exposed a synthetic private value'
        return 1
    fi
    if ((rc)); then cat "$log"; fi
    return "$rc"
}
for public in 0 1; do
    "$cc" "${common[@]}" "${flags[@]}" -DX4_PUBLIC="$public" \
        tests/cloud_time_worker_test.c src/auth/cloud_time.c src/auth/json.c -o "$out/worker"
    run_private "$out/worker.log" env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 "$out/worker"
    if ((public)); then
        if grep -q 'tiempo nube' "$out/worker.log"; then exit 1; fi
    else
        test "$(grep -c 'tiempo nube estructura' "$out/worker.log")" = 1
        test "$(grep -c 'tiempo nube restante_s' "$out/worker.log")" = 5
        if grep -q 'restante_s=1203' "$out/worker.log"; then exit 1; fi
    fi
    cat "$out/worker.log"
    if [[ $mode == tsan ]]; then continue; fi
    "$cc" "${common[@]}" "${flags[@]}" -DX4_PUBLIC="$public" \
        tests/cloud_time_http_test.c src/auth/http_client.c src/auth/http_wire.c src/auth/ca_bundle.c \
        src/auth/cloud_time.c src/auth/json.c -o "$out/http"
    run_private "$out/http.log" env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1 "$out/http"
    cat "$out/http.log"
done
