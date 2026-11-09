#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build=${1:?Provide a build directory inside this task workspace}
mkdir -p -- "$build"
for mode in plain sanitized;do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections
  -DX4_VIDEO_PROBE=0 -DX4_VIDEO_CAPACITY_1080=1 -DX4_STREAM_PROFILE=2 -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1
  -Itests/video_capacity_stubs -Itests/host_stubs)
 if [[ $mode == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
 "${CC:-gcc}" "${flags[@]}" tests/stream_notice_redraw_test.c tests/gpu_pipeline_cpu_only_stubs.c \
  src/video/video_capacity.c src/video/video_pipeline.c src/video/decoder_depth.c src/video/stream_cpu.c src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_sps.c src/video/stream_diagnostics.c src/video/stream_color_policy.c src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c \
  src/media/live_trace.c src/media/live_monitor.c src/media/video_ingress.c \
  -Wl,--gc-sections -o "$build/notice-redraw-$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build/notice-redraw-$mode"
done
