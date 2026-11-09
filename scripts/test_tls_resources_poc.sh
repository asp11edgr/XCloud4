#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Only plain is a footprint/32KiB benchmark. Separate ASan/UBSan coverage uses
# an explicit 512KiB client stack and never claims that stack/footprint result.
# Both runs are localhost-only and keep generated test keys solely in RAM.
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")/.."
W=$(cd .. && pwd)
PREFIX=${X4_HOST_MBEDTLS_PREFIX:-"$W/host-prefix"}
ENTROPY=${X4_HOST_ENTROPY_SOURCE:-"$W/host-mbedtls-support/entropy_getrandom.c"}
[[ -f "$PREFIX/lib/libmbedtls.a" && -f "$PREFIX/lib/libmbedx509.a" && -f "$PREFIX/lib/libmbedcrypto.a" && -f "$ENTROPY" ]]
BUILD=${X4_TLS_RESOURCE_OUTPUT_DIR:-"$W/tmp/tls-resources-poc"}
[[ ! -e "$BUILD" ]] || { echo 'Preserve prior resource output; choose a new output directory' >&2; exit 2; }
mkdir -p "$BUILD"
for mode in plain sanitized;do
  flags=()
  if [[ $mode == sanitized ]];then
    flags=(-DX4_TLS_RESOURCES_SANITIZED -fsanitize=address,undefined -fno-omit-frame-pointer)
  fi
  binary="$BUILD/tls-resources-poc-$mode"
  "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca \
    -DX4_HTTP_TEST -DX4_TLS_HOST -DX4_PUBLIC=0 -pthread -fno-pie -no-pie \
    -ffunction-sections -fdata-sections -I"$PREFIX/include" "${flags[@]}" \
    tests/tls_resources_poc.c src/auth/tls_conn.c src/auth/ca_bundle.c "$ENTROPY" \
    "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" "$PREFIX/lib/libmbedcrypto.a" \
    -Wl,--gc-sections -Wl,--wrap=malloc,--wrap=calloc,--wrap=free -pthread -o "$binary"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$binary" | tee "$BUILD/resources-$mode.txt"
  # The single 896-byte exception is not accepted by size alone: identify
  # the allocation's actual code origin with the current executable symbols.
  mapfile -t psa_sites < <(awk '$1=="TLS_RESOURCE_PSA_BUCKET_SITE" {print $2}' "$BUILD/resources-$mode.txt" | sort -u)
  [[ ${#psa_sites[@]} == 1 ]] || { echo 'FAIL: expected exactly one PSA lazy bucket code site' >&2; exit 1; }
  symbol=$("${ADDR2LINE:-addr2line}" -f -C -e "$binary" "${psa_sites[0]}")
  printf 'PSA_BUCKET_ORIGIN %s %s\n%s\n' "$mode" "${psa_sites[0]}" "$symbol"
  [[ ${symbol%%$'\n'*} == psa_reserve_free_key_slot ]] || { echo 'FAIL: retained allocation has an unexpected owner' >&2; exit 1; }
  while read -r site;do
    printf 'CODE_SITE %s %s\n' "$mode" "$site"
    "${ADDR2LINE:-addr2line}" -f -C -e "$binary" "$site"
  done < <(awk '$1=="TLS_RESOURCE_CODE_SITE" {print $2}' "$BUILD/resources-$mode.txt" | sort -u)
  printf 'PASS %s: PSA lazy bucket origin confirmed; final global shutdown freed it\n' "$mode"
done
