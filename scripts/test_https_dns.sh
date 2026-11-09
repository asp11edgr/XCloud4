#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
output=$(mkdir -- "$1" && cd "$1" && pwd)
# Compile the production HTTPS block unchanged. The preceding RTC adapter
# expects the native BSD sockaddr layout and cannot use Linux headers.
awk '/^static pthread_once_t http_dns_once = PTHREAD_ONCE_INIT;/ { dns = 1 } dns' \
    src/streaming/rtc_net.c > "$output/rtc_net_https_under_test.inc"
test -s "$output/rtc_net_https_under_test.inc"
common=(-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror -pthread -I"$output")
"${CC:-cc}" "${common[@]}" tests/https_dns_test.c -o "$output/https_dns_plain"
echo 'HTTPS DNS plain'
"$output/https_dns_plain"
"${CC:-cc}" "${common[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -fno-pie -no-pie tests/https_dns_test.c -o "$output/https_dns_san"
echo 'HTTPS DNS ASan/UBSan'
"$output/https_dns_san"
