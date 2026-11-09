#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-t26-store.XXXXXX")
# Keep every newly generated file, including failed temporary/partial files.
printf 'T26_STORE_TEST_ARTIFACTS %s\n' "$build"
extra=(); if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";fi
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -I src
  -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1 -DX4_T26_STORE_TESTING=1
  tests/probe_t26_store_test.c src/video/probe_t26_store.c
  src/video/probe_t26_state.c src/video/probe_t26_plan.c src/auth/json.c)
"${CC:-clang}" "${common[@]}" "${extra[@]}" -o "$build/store-test"
mkdir "$build/plain"
"$build/store-test" "$build/plain"
if [[ "${X4_TEST_SANITIZERS:-1}" == 1 ]];then
  "${CC:-clang}" "${common[@]}" -fsanitize=address,undefined \
    -fno-omit-frame-pointer -no-pie -o "$build/store-test-sanitized"
  mkdir "$build/sanitized"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$build/store-test-sanitized" "$build/sanitized"
  printf '%s\n' 'PASS T26 store ASan/UBSan'
else
  printf '%s\n' 'NOT RUN T26 store ASan/UBSan (X4_TEST_SANITIZERS=0)'
fi
for pair in 0-0 0-1 1-0;do
  "${CC:-clang}" -std=c11 -Wall -Wextra -Werror -I src \
    -DX4_VIDEO_PROBE="${pair%-*}" -DX4_PROBE_T26="${pair#*-}" \
    -c src/video/probe_t26_store.c -o "$build/off-$pair.o"
  if nm -g --defined-only "$build/off-$pair.o" | grep -E 'x4_t26_store';then
    printf '%s\n' 'FAIL T26 store runtime symbol while disabled';exit 1
  fi
  if strings "$build/off-$pair.o" | grep -E '/data/xcloud4-probe/t26|progress.json|cell-%s';then
    printf '%s\n' 'FAIL T26 store path text while disabled';exit 1
  fi
done
if "${CC:-clang}" -std=c11 -fsyntax-only -I src -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 \
  -DX4_OPENORBIS=1 -DX4_T26_STORE_TESTING=1 -x c src/video/probe_t26_store.h \
  >"$build/forbidden-native-testing.log" 2>&1;then
  printf '%s\n' 'FAIL host-configurable root allowed in native build';exit 1
fi
grep -F 'Host-configurable store roots must never enter a PS4 build' "$build/forbidden-native-testing.log" >/dev/null
printf '%s\n' 'PASS T26 store real filesystem, durable boundaries, exclusive history and disabled/native guards'
