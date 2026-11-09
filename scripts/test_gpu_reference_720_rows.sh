#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_output=${X4_GPU_REFERENCE_TEST_OUTPUT:-$(mktemp -d)}
mkdir -p "$task_output"
for variant in sse2 scalar sse2-sanitized scalar-sanitized public;do
    flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread -DX4_VIDEO_PROBE=0 -DX4_STREAM_PROFILE=1)
    if [[ "$variant" == scalar* ]];then flags+=(-DX4_PIPELINE_SCALAR=1);fi
    if [[ "$variant" == *sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
    if [[ "$variant" == public ]];then flags+=(-DX4_PUBLIC=1);else flags+=(-DX4_PUBLIC=0);fi
    "${CC:-clang}" "${flags[@]}" tests/test_gpu_reference_720_rows.c src/video/video_pipeline.c -o "$task_output/$variant"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 90s "$task_output/$variant"
    if [[ "$variant" == public ]];then
        nm "$task_output/$variant" > "$task_output/$variant.symbols"
        grep -q 'x4_video_pipeline_gpu_reference_720_rows' "$task_output/$variant.symbols"
        if grep -q 'x4_video_pipeline_gpu_copy_rows' "$task_output/$variant.symbols";then exit 1;fi
    fi
done
