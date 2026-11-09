#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-t26-controller.XXXXXX")
printf 'T26_CONTROLLER_TEST_ARTIFACTS %s\n' "$build"
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc-clean-17/mbedtls}
test -f "$dep/library/sha256.c" && test -f "$dep/library/platform_util.c"
mkdir -p "$build/stubs/orbis"
cat > "$build/stubs/orbis/Pad.h" <<'EOF'
/* HOST parsing-only shape. No controller/SDK ABI is exercised by this test.
 * The real UI entry point remains unreferenced and is removed by section GC. */
#pragma once
typedef struct { unsigned unused; } OrbisPadData;
#define ORBIS_PAD_BUTTON_OPTIONS 0u
#define ORBIS_PAD_BUTTON_CIRCLE 0u
EOF
extra=();if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";fi
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -pthread
  -D_POSIX_C_SOURCE=200809L -I"$build/stubs" -Itests/host_stubs -I"$dep/include" -I src
  -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_CAPACITY_1080=1
  -DX4_RECEIVER_NACK=1 -DX4_NACK_GRACE_MS=45 -DX4_RUMBLE=0 -DX4_PUBLIC=0
  -DX4_PROBE_STACK_HOST=1 -DX4_STREAM_WAIT_HOST=1 -DX4_T26_STORE_TESTING=1
  -ffunction-sections -fdata-sections)
sources=(src/video/probe_t26_store.c src/video/probe_t26_state.c
  src/video/probe_t26_result.c src/video/probe_t26_plan.c src/video/video_capacity.c
  src/auth/json.c "$dep/library/sha256.c" "$dep/library/platform_util.c")
run_set(){
  local name=$1;shift
  local flags=("$@") compile=() objects=()
  local directory="$build/$name"
  mkdir "$directory" "$directory/fixtures"
  for option in "${flags[@]}";do case "$option" in -no-pie|-pie|-Wl,*) ;; *) compile+=("$option");;esac;done
  "${CC:-clang}" "${common[@]}" "${compile[@]}" -UNDEBUG -Wno-unused-function \
    -c tests/probe_t26_controller_test.c -o "$directory/harness.o"
  objects+=("$directory/harness.o")
  local number=0
  for source in "${sources[@]}";do
    "${CC:-clang}" "${common[@]}" "${compile[@]}" -UNDEBUG -c "$source" -o "$directory/module-$number.o"
    objects+=("$directory/module-$number.o");number=$((number+1))
  done
  "${CC:-clang}" "${objects[@]}" "${flags[@]}" -pthread -Wl,--gc-sections -Wl,--wrap=fsync -lm -o "$directory/test"
  ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
  UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$directory/test" "$directory/fixtures" | tee "$directory/test.log"
}
if [[ ${#extra[@]} -gt 0 ]];then run_set inherited "${extra[@]}";else
  run_set plain
  if [[ ${X4_TEST_SANITIZERS:-1} == 1 ]];then
    run_set sanitized -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie
  else printf '%s\n' 'NOT RUN T26 controller helpers ASan/UBSan (X4_TEST_SANITIZERS=0)';fi
fi
printf '%s\n' 'PASS actual T26 controller recovery/summary helpers; no duplicated protocol or native/UI execution'
