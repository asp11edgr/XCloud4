#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -Eeuo pipefail
cd "$(dirname "$0")/.."
# FFmpeg is a host tool only. Its synthetic pattern contains no game footage.
FFMPEG="${X4_FFMPEG:-ffmpeg}"
"$FFMPEG" -hide_banner -loglevel warning -y \
    -f lavfi -i 'testsrc2=size=640x368:rate=30' -t 8 -an \
    -c:v libx264 -profile:v baseline -level:v 3.0 -pix_fmt yuv420p -b:v 800k \
    -x264-params 'aud=1:repeat-headers=1:scenecut=0:ref=1:bframes=0:keyint=30' \
    -f h264 assets/sample.h264
