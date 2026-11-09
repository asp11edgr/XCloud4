#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
cc=${CC:-clang}
build=${1:?Provide a build directory inside this task workspace}
mkdir -p -- "$build"
for mode in plain sanitized; do
  flags=()
  if [[ $mode == sanitized ]]; then flags=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
  "$cc" -std=c23 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections \
    -DX4_OPENORBIS -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -DX4_VIDEO_CAPACITY_1080=1 \
    -DX4_INGRESS_HOST_TEST -DX4_RUMBLE=0 -DX4_NACK_GRACE_MS=45 -DX4_STREAM_WAIT_HOST=1 \
    -D_POSIX_C_SOURCE=200809L -Itests/host_stubs -Itests/audio_stubs "${flags[@]}" \
    tests/live_media_epoch_test.c src/media/video_ingress.c src/media/rtp_reorder.c \
    src/media/nack_grace.c src/media/nack_sequence_binding.c src/streaming/nack_grace_inbox.c \
    src/video/stream_au_queue.c src/streaming/keyframe_limiter.c src/video/stream_wait.c \
    -Wl,--gc-sections -o "$build/media-epoch-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$build/media-epoch-$mode"
done
