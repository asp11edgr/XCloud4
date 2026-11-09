#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${X4_GPU_TEST_OUTPUT:-$(mktemp -d)}
mkdir -p "$out"
read -r -a cc <<< "${X4_GPU_HOST_CC:-${CC:-clang}}"
read -r -a extra <<< "${X4_TEST_CFLAGS:-}"
"${cc[@]}" -std=gnu2x -O2 -g -Wall -Wextra -Werror -pthread \
  -DX4_PUBLIC=0 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 \
  -Itests/gpu_convert_stubs -Itests/video_capacity_stubs -Itests/host_stubs \
  "${extra[@]}" -ffp-contract=off tests/gpu_convert_state_test.c src/video/gpu_quality_ref.c \
  src/video/video_pipeline.c src/video/video_capacity.c -o "$out/gpu-state"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/gpu-state"
python3 -B tests/gpu_convert_shader_test.py --output "$out/ir"
printf '%s\n' 'PASS GPU conversion ABI/state, strict shader validation and actual-IR/CPU equivalence'
