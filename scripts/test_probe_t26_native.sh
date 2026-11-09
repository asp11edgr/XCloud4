#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
build=$(mktemp -d /tmp/x4-t26-native.XXXXXX)
echo "T26_NATIVE_TEST_ARTIFACTS $build"
# Only instrument host SDK/decoder doubles in a temporary copy. The included
# ../src/video/live_h264.c remains the actual unchanged product source, and
# the old harness on disk remains byte-for-byte unchanged.
python3 -B - "$repo" "$build" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]);out=pathlib.Path(sys.argv[2])
fixture=(root/'tests/probe_native_mock_test.c').read_text()
replacements={
'''static int32_t query_compute(OrbisVideodec2ComputeMemoryInfo *m)
{ m->cpuGpuMemorySize = 16384; return 0; }''':'''static unsigned t26_compute_queries,t26_queue_allocates,t26_queue_releases;
static OrbisVideodec2ComputeConfigInfo t26_queue_config;
static int32_t query_compute(OrbisVideodec2ComputeMemoryInfo *m)
{ ++t26_compute_queries; CHECK(m&&m->thisSize==sizeof(*m)); m->cpuGpuMemorySize=16384; return 0; }''',
'''{ (void)c; (void)m; *out = (void *)(uintptr_t)0x11; return 0; }''':'''{ ++t26_queue_allocates; CHECK(c&&m&&out&&m->cpuGpuMemorySize==16384&&m->pCpuGpuMemory);
  t26_queue_config=*c; *out=(void *)(uintptr_t)0x11; return 0; }''',
'''static int32_t release_queue(void *q) { (void)q; return 0; }''':'''static int32_t release_queue(void *q)
{ ++t26_queue_releases; CHECK(q==(void *)(uintptr_t)0x11); return 0; }''',
}
for old,new in replacements.items():
    assert fixture.count(old)==1,old
    fixture=fixture.replace(old,new)
(out/'probe_native_mock_test.c').write_text(fixture)
(out/'driver.c').write_bytes((root/'tests/probe_t26_native_test.c').read_bytes())
PY
extra=(); if [[ -n ${X4_TEST_CFLAGS:-} ]]; then read -r -a extra <<< "$X4_TEST_CFLAGS"; fi
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections
    -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_VIDEO_CAPACITY_1080=1
    -DX4_STREAM_PROFILE=1 -DX4_PROBE_STACK_HOST=1 -DX4_PUBLIC=0
    -I"$repo/tests" -I"$repo/tests/video_capacity_stubs")
"$compiler" "${flags[@]}" "${extra[@]}" "$build/driver.c" \
    "$repo/src/video/video_capacity.c" "$repo/src/video/video_pipeline.c" \
    "$repo/src/video/stream_sps.c" "$repo/src/video/probe_stack.c" "$repo/src/video/probe_contract.c" \
    "$repo/src/video/stream_cpu.c" "$repo/src/video/decoder_depth.c" \
    "$repo/src/video/probe_t26_plan.c" -Wl,--gc-sections -o "$build/native-test"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} timeout 90s "$build/native-test" | tee "$build/native.log"
