#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in plain sanitized public;do
    flags=();public=0
    if [[ "$variant" == public ]];then public=1;fi
    if [[ "$variant" == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
    "${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
        -DX4_VIDEO_PROBE=0 -DX4_STREAM_PROFILE=1 -DX4_PUBLIC="$public" \
        "${flags[@]}" tests/test_gpu_readback_rows.c src/video/video_pipeline.c -o "$task_tmp/$variant"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$task_tmp/$variant"
    if [[ "$variant" == public ]];then
        if nm "$task_tmp/$variant" | grep -q 'x4_video_pipeline_gpu_copy_rows';then exit 1;fi
    else
        objdump -d "$task_tmp/$variant" > "$task_tmp/$variant.asm"
        grep -q 'movntdqa' "$task_tmp/$variant.asm"
    fi
done
printf '%s\n' 'PASS 16-byte readback instruction, functional/bounds tests, ASan/UBSan and Public exclusion'
