#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=${X4_GPU_QUALITY_TEST_OUTPUT:-$(mktemp -d)}
mkdir -p "$out"
python3 -B tests/gpu_quality_shader_test.py --output "$out/ir"
printf '%s\n' 'PASS strict quality shader ABI/ELF, immutable R3 hashes and actual LLVM/CPU reference equivalence'
