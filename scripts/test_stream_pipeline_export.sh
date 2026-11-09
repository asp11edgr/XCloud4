#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Focused exporter test only; no scheduler/timing/native/GPU runs.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
base=${X4_EXPORT_TEST_OUTPUT:-$(mktemp -d)}
if [[ -z ${X4_EXPORT_TEST_OUTPUT:-} ]]; then trap 'rm -rf -- "$base"' EXIT; fi
cc=${CC:-cc}
read -r -a test_flags <<< "${X4_TEST_CFLAGS:-}"
cd "$root"
for public in 0 1; do
    out="$base/public-$public";mkdir -p "$out"
    "$cc" -std=gnu2x -O1 -g -Wall -Wextra -Wno-unused-function -pthread \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1 -DX4_STREAM_PROFILE=1 \
        -DX4_VIDEO_PROBE=0 -DX4_PUBLIC="$public" -DX4_RECEIVE_DIAG_HOST_TEST -DX4_REMB_HOST_TEST \
        -DX4_NET_HUD_TEST=1 \
        "${test_flags[@]}" \
        -Itests/video_capacity_stubs -Itests/host_stubs \
        tests/stream_pipeline_export_privacy_test.c src/video/stream_cpu.c \
        src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_diagnostics.c src/video/stream_sps.c \
        src/streaming/stream_profile.c src/streaming/stream_startup.c \
        src/streaming/keyframe_limiter.c src/streaming/rtc_receive_diag.c src/ui/net_hud.c \
        -o "$out/exporter"
    "$out/exporter" "$out"
done
python3 tests/stream_pipeline_export_test.py "$base" privacy
