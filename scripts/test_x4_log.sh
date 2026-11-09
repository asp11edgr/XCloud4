#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
output=$(mkdir -- "$1" && cd "$1" && pwd)
ulimit -c 0

common=(-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror
        -DX4_LOG_TEST -fno-builtin -pthread -Isrc)
wrap=()
for name in printf vprintf puts putchar perror fprintf vfprintf fputs fputc fwrite putc _IO_putc write; do
    wrap+=("-Wl,--wrap=$name")
done
for mode in plain asan-ubsan tsan; do
    sanitizer=()
    compiler=${CC:-cc}
    case "$mode" in
        asan-ubsan)
            sanitizer=(-fsanitize=address,undefined -fno-sanitize-recover=all
                       -fno-omit-frame-pointer -fno-pie -no-pie)
            ;;
        tsan)
            compiler=${CC_TSAN:-$compiler}
            sanitizer=(-fsanitize=thread -fno-omit-frame-pointer)
            ;;
    esac
    echo "x4_log host $mode"
    "$compiler" "${common[@]}" "${sanitizer[@]}" tests/x4_log_test.c src/core/x4_log.c \
        "${wrap[@]}" -o "$output/$mode"
    ASAN_OPTIONS="halt_on_error=1:log_path=$output/asan-report" \
        UBSAN_OPTIONS="halt_on_error=1:log_path=$output/ubsan-report" \
        TSAN_OPTIONS="halt_on_error=1:log_path=$output/tsan-report" \
        "$output/$mode" "$output/$mode.stdout" "$output/$mode.stderr"
done
