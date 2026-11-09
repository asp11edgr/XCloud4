#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CC:-clang}
build=$(mktemp -d /tmp/x4-t26-pipeline.XXXXXX)
echo "T26_PIPELINE_TEST_ARTIFACTS $build"
# Reuse the existing native/VideoOut/GPU substitutes. Adapt only the temporary
# native double to own all five advertised depth-two output slots. The legacy
# source stays unchanged and its renamed main keeps the existing link contract.
python3 -B - "$repo" "$build" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]);out=pathlib.Path(sys.argv[2])
runtime=(root/'tests/stream_pipeline_runtime_test.c').read_text()
adaptations={
    'typedef struct { uint8_t *pixels[4]; } Native;':
        'enum { T26_MOCK_OUTPUTS=5 };\ntypedef struct { uint8_t *pixels[T26_MOCK_OUTPUTS]; } Native;',
    'for(unsigned i=0;i<4;++i){n->pixels[i]=malloc':
        'for(unsigned i=0;i<T26_MOCK_OUTPUTS;++i){n->pixels[i]=malloc',
    'slot<4 && bytes==8':
        'slot<v->stream_info.output_count && slot<T26_MOCK_OUTPUTS && bytes==8',
    '    for(unsigned i=0;i<4;++i)free(n->pixels[i]);\n    free(n);v->state=NULL;':
        '    for(unsigned i=0;i<T26_MOCK_OUTPUTS;++i)free(n->pixels[i]);\n    free(n);v->state=NULL;',
}
for old,new in adaptations.items():
    assert runtime.count(old)==1,('native-double adaptation anchor',old)
    runtime=runtime.replace(old,new,1)
(out/'stream_pipeline_runtime_test.c').write_text(runtime)
source=(root/'tests/stream_gpu_integration_test.c').read_text()
needle='int main(int argc,char **argv)'
assert source.count(needle)==1
source=source.replace(needle,'int x4_t26_unused_gpu_main(int argc,char **argv)')
(out/'combined.c').write_text(source+'\n'+(root/'tests/probe_t26_pipeline_test.c').read_text())
PY
extra=(); if [[ -n ${X4_TEST_CFLAGS:-} ]]; then read -r -a extra <<< "$X4_TEST_CFLAGS"; fi
flags=(-std=gnu2x -O1 -g -ffp-contract=off -Wall -Wextra -Werror -pthread
    -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1 -DX4_STREAM_PIPELINE_TEST_HOOKS=1
    -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_PUBLIC=0
    -I"$repo/tests" -I"$repo/tests/video_capacity_stubs" -I"$repo/tests/host_stubs")
"$compiler" "${flags[@]}" "${extra[@]}" "$build/combined.c" \
    "$repo/src/video/gpu_quality_ref.c" "$repo/src/video/video_pipeline.c" \
    "$repo/src/video/stream_cpu.c" "$repo/src/video/stream_wait.c" \
    "$repo/src/video/stream_au_queue.c" "$repo/src/video/stream_diagnostics.c" \
    "$repo/src/video/stream_sps.c" "$repo/src/video/stream_color_policy.c" \
    "$repo/src/streaming/stream_profile.c" "$repo/src/streaming/stream_startup.c" \
    "$repo/src/streaming/keyframe_limiter.c" "$repo/src/video/probe_t26_plan.c" \
    -o "$build/pipeline-test"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} timeout 60s "$build/pipeline-test" "$build"
