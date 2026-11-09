#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Dedicated acceptance for the four new shaders; existing 1080p gates remain.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${X4_GPU_720_TEST_OUTPUT:-$(mktemp -d)}
mkdir -p "$out"
python3 -B tests/gpu_convert_720_shader_test.py --output "$out/ir"
printf '%s\n' 'PASS strict 720p shader validation and bit-exact actual-IR/CPU equivalence'
