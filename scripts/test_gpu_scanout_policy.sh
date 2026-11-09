#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_tmp=$(mktemp -d)
trap 'rm -rf -- "$task_tmp"' EXIT
for variant in sse2 scalar public sanitized-sse2 sanitized-scalar;do
    flags=();public=0
    if [[ "$variant" == scalar || "$variant" == sanitized-scalar ]];then flags+=(-DX4_PIPELINE_SCALAR=1);fi
    if [[ "$variant" == public ]];then public=1;fi
    if [[ "$variant" == sanitized-* ]];then
        if [[ "${X4_TEST_SANITIZERS:-1}" != 1 ]];then continue;fi
        flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
    fi
    "${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror \
        -DX4_VIDEO_PROBE=0 -DX4_STREAM_PROFILE=1 -DX4_PUBLIC="$public" \
        "${flags[@]}" ${X4_TEST_CFLAGS:-} tests/test_gpu_scanout_policy.c src/video/video_pipeline.c \
        -o "$task_tmp/$variant"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$task_tmp/$variant"
done
printf '%s\n' 'PASS scanout state and real compact CPU verification rows: SSE2/scalar, Public, ASan/UBSan'
