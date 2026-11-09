#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_output=${X4_T26_BACKEND_TEST_OUTPUT:-$(mktemp -d /tmp/x4-t26-backend-XXXXXX)}
if [[ -z ${X4_T26_BACKEND_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$task_output"' EXIT;fi
mkdir -p "$task_output"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca
    -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_CAPACITY_1080=1)
if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";flags+=("${extra[@]}");fi
"${CC:-cc}" "${flags[@]}" -DX4_T26_PLATFORM_HELPERS_TEST=1 tests/probe_t26_platform_test.c \
    src/video/probe_t26_platform.c -o "$task_output/probe-t26-platform"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30s "$task_output/probe-t26-platform"
"${CC:-cc}" "${flags[@]}" -DX4_T26_BACKEND_HELPERS_TEST=1 tests/probe_t26_layout_test.c \
    src/video/probe_t26_backend.c src/video/video_capacity.c -o "$task_output/probe-t26-layout"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30s "$task_output/probe-t26-layout"
"${CC:-cc}" "${flags[@]}" tests/probe_t26_backend_test.c src/video/probe_t26_backend.c \
    src/video/probe_t26_plan.c src/video/video_capacity.c -o "$task_output/probe-t26-backend"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30s "$task_output/probe-t26-backend"
for pair in 0-0 0-1 1-0;do
 probe=${pair%-*};bench=${pair#*-}
 for file in platform backend;do
  "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_VIDEO_PROBE="$probe" -DX4_PROBE_T26="$bench" \
     -c "src/video/probe_t26_$file.c" -o "$task_output/off-$file-$pair.o"
  if "${NM:-nm}" --defined-only "$task_output/off-$file-$pair.o" | grep -Eq ' [TtBbDdRr] ';then
   printf 'FAIL T26 %s runtime symbol with probe=%s bench=%s\n' "$file" "$probe" "$bench";exit 1
  fi
 done
done
printf '%s\n' 'PASS T26 CPU/memory evidence, strict NV12 layout and disabled gates'
