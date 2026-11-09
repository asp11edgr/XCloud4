#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-t26-startup.XXXXXX")
printf 'T26_STARTUP_TEST_ARTIFACTS %s\n' "$build"
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc-clean-17/mbedtls}
test -f "$dep/include/mbedtls/sha256.h"
mkdir -p "$build/stubs/orbis"
cat > "$build/stubs/orbis/Pad.h" <<'EOF'
/* Host startup double only; not a native PS4 Pad ABI. */
#pragma once
typedef struct { unsigned unused; } OrbisPadData;
#define ORBIS_PAD_BUTTON_OPTIONS 1u
#define ORBIS_PAD_BUTTON_CIRCLE 2u
EOF
extra=();if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";fi
common=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca -pthread
  -D_POSIX_C_SOURCE=200809L -I"$build/stubs" -Itests/video_capacity_stubs -Itests/host_stubs -I"$dep/include" -I src
  -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_CAPACITY_1080=1
  -DX4_RECEIVER_NACK=1 -DX4_NACK_GRACE_MS=45 -DX4_RUMBLE=0 -DX4_PUBLIC=0
  -DX4_PROBE_STACK_HOST=1 -DX4_STREAM_WAIT_HOST=1 -ffunction-sections -fdata-sections)
if [[ -n ${X4_T26_STARTUP_SOURCE_DIR:-} ]];then
  test -f "$X4_T26_STARTUP_SOURCE_DIR/src/video/probe_t26_controller.c"
  common+=("-DX4_T26_CONTROLLER_SOURCE=\"$X4_T26_STARTUP_SOURCE_DIR/src/video/probe_t26_controller.c\"")
  common+=("-DX4_T26_STACK_HEADER=\"$X4_T26_STARTUP_SOURCE_DIR/src/video/probe_stack.h\"")
fi
run_set(){
  local name=$1;shift
  local flags=("$@") compile=() objects=()
  local directory="$build/$name";mkdir "$directory"
  for option in "${flags[@]}";do case "$option" in -no-pie|-pie|-Wl,*) ;; *) compile+=("$option");;esac;done
  "${CC:-clang}" "${common[@]}" "${compile[@]}" -UNDEBUG -Wno-unused-function \
    -c tests/probe_t26_startup_test.c -o "$directory/harness.o"
  "${CC:-clang}" "${common[@]}" "${compile[@]}" -UNDEBUG \
    -c src/video/display.c -o "$directory/display.o"
  "${CC:-clang}" "$directory/harness.o" "$directory/display.o" "${flags[@]}" -pthread -Wl,--gc-sections -o "$directory/test"
  ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
  UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$directory/test" | tee "$directory/test.log"
}
if [[ ${#extra[@]} -gt 0 ]];then run_set inherited "${extra[@]}";else
  run_set plain
  if [[ ${X4_TEST_SANITIZERS:-1} == 1 ]];then
    run_set sanitized -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie
  else printf '%s\n' 'NOT RUN internal startup ASan/UBSan loop (external runner owns sanitized case)';fi
fi
