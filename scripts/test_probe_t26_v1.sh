#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-t26-v1.XXXXXX")
# Preserve the fresh executables/logs for the task evidence; never clean work.
printf 'T26_V1_TEST_ARTIFACTS %s\n' "$build"
compiler=${CC:-clang}
extra=(); if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";fi
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca
  -D_POSIX_C_SOURCE=200809L -I src -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1)

run_set() {
  local label=$1;shift
  local options=("$@")
  "$compiler" "${common[@]}" "${options[@]}" -UNDEBUG \
    tests/probe_t26_v1_abi_test.c -o "$build/$label-v1-abi"
  "$compiler" "${common[@]}" "${options[@]}" -UNDEBUG \
    tests/probe_t26_v2_abi_test.c -o "$build/$label-v2-abi"
  "$compiler" "${common[@]}" "${options[@]}" -UNDEBUG \
    tests/probe_t26_v1_test.c src/video/probe_t26_v1.c -o "$build/$label-v1-backend"
  for name in v1-abi v2-abi v1-backend;do
    ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
    UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} \
      "$build/$label-$name" | tee "$build/$label-$name.log"
  done
  printf 'PASS T26 v1/v2 ABI and injected v1 backend: %s\n' "$label"
}

if [[ ${#extra[@]} -gt 0 ]];then
  # The task driver supplies plain/sanitized flags in separate invocations.
  printf 'T26_V1_TEST_FLAGS inherited X4_TEST_CFLAGS\n'
  run_set inherited "${extra[@]}"
else
  run_set plain
  if [[ "${X4_TEST_SANITIZERS:-1}" == 1 ]];then
    run_set sanitized -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie
  else
    printf '%s\n' 'NOT RUN T26 v1 ASan/UBSan (X4_TEST_SANITIZERS=0)'
  fi
fi

cat > "$build/guard.c" <<'EOF'
#include "video/probe_t26_v1.h"
#include "video/probe_t26_v2_abi_check.h"
#ifdef X4_T26_V1_CODEC_AVC
#error T26 v1 ABI constants escaped their feature guards
#endif
#ifdef X4_T26_V2_ABI_CHECKED
#error T26 v2 ABI assertions escaped their feature guards
#endif
int main(void) { return 0; }
EOF
for pair in 0-0 0-1 1-0;do
  off_flags=(-std=c11 -O1 -Wall -Wextra -Werror -I src
    -DX4_VIDEO_PROBE="${pair%-*}" -DX4_PROBE_T26="${pair#*-}")
  "$compiler" "${off_flags[@]}" "$build/guard.c" src/video/probe_t26_v1.c -o "$build/off-$pair"
  "$compiler" "${off_flags[@]}" tests/probe_t26_v2_abi_test.c -o "$build/v2-guard-$pair"
  "$build/v2-guard-$pair"
  if "${NM:-nm}" -g --defined-only "$build/off-$pair" | grep -E 'x4_t26_v1';then
    printf '%s\n' 'FAIL T26 v1 backend symbol while disabled';exit 1
  fi
  if strings "$build/off-$pair" | grep -E 'sceVideodecQueryResourceInfo|sceVideodecCreateDecoder|leCAscipfFY|qkgRiwHyheU';then
    printf '%s\n' 'FAIL T26 v1 native ABI text while disabled';exit 1
  fi
done
printf '%s\n' 'PASS T26 v1/v2 feature guards: no disabled backend/ABI symbols or native symbol text'
