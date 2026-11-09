#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-t26-runner.XXXXXX")
printf 'T26_RUNNER_TEST_ARTIFACTS %s\n' "$build"
extra=();if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";fi
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -I src
  -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1
  -DX4_T26_PLATFORM_HELPERS_TEST=1 -ffunction-sections -fdata-sections
  tests/probe_t26_runner_test.c src/video/probe_t26_runner.c
  src/video/probe_t26_metrics.c src/video/probe_t26_cache.c src/video/probe_t26_platform.c
  -Wl,--gc-sections -lm)
run_set(){
  local name=$1;shift
  "${CC:-clang}" "${common[@]}" "$@" -UNDEBUG -o "$build/$name"
  ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
  UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$build/$name" | tee "$build/$name.log"
}
if [[ ${#extra[@]} -gt 0 ]];then run_set inherited "${extra[@]}";else
  run_set plain
  if [[ ${X4_TEST_SANITIZERS:-1} == 1 ]];then
    run_set sanitized -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie
  else printf '%s\n' 'NOT RUN T26 runner ASan/UBSan (X4_TEST_SANITIZERS=0)';fi
fi
for pair in 0-0 0-1 1-0;do
  "${CC:-clang}" -std=c11 -Wall -Wextra -Werror -I src \
    -DX4_VIDEO_PROBE="${pair%-*}" -DX4_PROBE_T26="${pair#*-}" \
    -c src/video/probe_t26_runner.c -o "$build/off-$pair.o"
  if "${NM:-nm}" -g --defined-only "$build/off-$pair.o" | grep -E 'x4_t26_run_raw';then
    printf '%s\n' 'FAIL T26 runner symbol while disabled';exit 1
  fi
done
printf '%s\n' 'PASS T26 raw runner host checks; no SDK, firmware, real hashing or console execution'
