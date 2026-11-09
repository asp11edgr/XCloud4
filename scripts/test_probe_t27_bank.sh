#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-t27-bank.XXXXXX")
printf 'T27_BANK_HOST_ARTIFACTS %s\n' "$build"
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -I src
 -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_PROBE_T27=1 -DX4_STREAM_PROFILE=1 -DX4_T27_STORE_TESTING=1
 tests/probe_t27_bank_test.c src/video/probe_t27_plan.c src/video/probe_t27_state.c src/video/probe_t27_store.c
 src/video/probe_t27_result.c src/video/probe_t26_plan.c src/auth/json.c)
"${CC:-clang}" "${common[@]}" ${X4_TEST_CFLAGS:-} -lm -o "$build/test-bank"
mkdir "$build/plain";"$build/test-bank" "$build/plain"
if [[ ${X4_TEST_SANITIZERS:-1} == 1 ]];then
 "${CC:-clang}" "${common[@]}" -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -lm -o "$build/test-bank-sanitized"
 mkdir "$build/sanitized"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build/test-bank-sanitized" "$build/sanitized"
fi
for public in 0 1;do
 for source in src/video/probe_t27*.c;do
  "${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_PUBLIC="$public" -DX4_VIDEO_PROBE=0 -DX4_PROBE_T26=0 -DX4_PROBE_T27=0 -c "$source" -o "$build/off.o"
  if nm --defined-only "$build/off.o" | grep -Eq ' [TtBbDdRr] ';then printf 'FAIL T27 symbol in disabled %s\n' "$source";exit 1;fi
 done
done
printf '%s\n' 'PASS T27 dedicated bank and disabled guards, plain + ASan/UBSan; no native execution'
