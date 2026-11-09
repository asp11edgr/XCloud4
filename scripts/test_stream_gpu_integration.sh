#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
cc=${CC:-clang}
out=${X4_GPU_TEST_OUTPUT:-$(mktemp -d)}
mkdir -p "$out"
flags=(-std=gnu2x -O1 -g -ffp-contract=off -Wall -Wextra -Werror -pthread -DX4_OPENORBIS=1 -DX4_STREAM_WAIT_HOST=1 -DX4_STREAM_PIPELINE_TEST_HOOKS=1 -DX4_GPU_INTEGRATION_TEST=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -DX4_PUBLIC=0 -Itests/video_capacity_stubs -Itests/host_stubs)
if [[ ${X4_TEST_SANITIZE:-0} == 1 ]];then flags+=(-fno-omit-frame-pointer -fsanitize=address,undefined -no-pie);fi
"$cc" "${flags[@]}" tests/stream_gpu_integration_test.c src/video/gpu_quality_ref.c src/video/video_pipeline.c src/video/stream_cpu.c src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_diagnostics.c src/video/stream_sps.c src/video/stream_color_policy.c src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c -o "$out/integration"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s "$out/integration" "$out"
python3 - "$out" <<'PY'
import json,pathlib,sys
rows=list(pathlib.Path(sys.argv[1]).glob('xcloud4-stream-*.json'))
assert len(rows)==9,len(rows)
for path in rows:
 d=json.loads(path.read_text());g=d['gpu_conversion'];v=g['verification'];bg=g['verification_background']
 assert g['requested']=='GPU' and v['tolerance']==1 and v['rows']==16
 assert v['every_gpu_frames']==600 and v['readback_bytes_per_load']==0
 assert bg['priority']==767 and 'snapshot' in bg['scope']
 assert g['prep_us']['n']==g['eop_us']['n']==g['timing_us']['n']
 assert g['timing_us']['total_us']==g['prep_us']['total_us']+g['eop_us']['total_us']
 assert d['status']['retained']==False and d['status']['decoder_close']==0
 assert d['scanout']['unmap']==d['scanout']['release']==0
 if v['checks']:
  assert g['verification_CPU_scratch_bytes']==8847360 and bg['prepared']
  assert v['checked_pixels']==v['checks']*16*1920
  assert bg['priority_result']==0
assert any(json.loads(p.read_text())['gpu_conversion']['verification_background']['failed'] for p in rows)
print('PASS nine real scheduler exports: immutable background snapshots, lease release before CPU verification, mismatch->CPU, late EOP, prepare failures and close')
PY

# The same real pipeline is also compiled as PUBLIC. Lab preference doubles
# abort if reached; a reduced main covers only defaults/FORCE_CPU/pause.
mkdir -p "$out/public"
public_flags=("${flags[@]/-DX4_PUBLIC=0/-DX4_PUBLIC=1}")
"$cc" "${public_flags[@]}" tests/stream_gpu_integration_test.c src/video/gpu_quality_ref.c src/video/video_pipeline.c src/video/stream_cpu.c src/video/stream_wait.c src/video/stream_au_queue.c src/video/stream_diagnostics.c src/video/stream_sps.c src/video/stream_color_policy.c src/streaming/stream_profile.c src/streaming/stream_startup.c src/streaming/keyframe_limiter.c -o "$out/public/integration"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60s "$out/public/integration" "$out/public" | tee "$out/public/runtime.log"
python3 - "$out/public" <<'PY'
import json,pathlib,sys
rows=list(pathlib.Path(sys.argv[1]).glob('xcloud4-stream-*.json'))
assert len(rows)==3,len(rows)
by_case={}
for path in rows:
 text=path.read_text();d=json.loads(text);g=d['gpu_conversion']
 assert d['synchronization']['requested_policy']==0
 assert not d['synchronization']['mailbox_capable'] and d['synchronization']['scanouts']==3
 assert d['synchronization']['display_budget_extension_bytes']==0
 assert not d['status']['retained'] and not d['status']['error']
 # CAS must be absent from GPU features/selection, not unrelated atomic-CAS
 # diagnostic prose elsewhere in the complete streaming report.
 assert 'CAS' not in json.dumps(g,ensure_ascii=False)
 assert g['requested'] in ('GPU','CPU') and g['effective'] in ('GPU','CPU')
 assert 'deband' not in text and 'repeat_previous' not in text
 assert set(g['quality'])=={'requested_dither','effective_dither','dither_frames','reason'}
 for case in (2401,2402,2403):
  if str(case) in path.name:by_case[case]=g
assert by_case[2401]['requested']=='GPU' and by_case[2401]['gpu_frames']==1 and by_case[2401]['quality']['dither_frames']==1
assert by_case[2402]['requested']=='CPU' and by_case[2402]['gpu_frames']==0 and by_case[2402]['cpu_frames']==1
cpu=by_case[2402];bg=cpu['verification_background']
assert bg['priority_result']==bg['last_result']==2147483647
assert all(value==2147483647 for value in bg['native_returns'].values())
assert cpu['quality']['requested_dither'] and not cpu['quality']['effective_dither']
assert cpu['quality']['reason']=='ignored_cpu'
log=(pathlib.Path(sys.argv[1])/'runtime.log').read_text()
assert 'XCloud4 GPU: Dither=0 reason=ignored_cpu' in log
exports=[line for line in log.splitlines() if 'public stream diagnostic export rc=' in line]
assert len(exports)==4 and sum('rc=0' in line for line in exports)==3
assert sum('rc=-' in line for line in exports)==1
assert by_case[2403]['requested']=='GPU' and by_case[2403]['pause_cpu_frames']==1 and by_case[2403]['quality']['dither_frames']==1
print('PASS PUBLIC real scheduler compile and three exports: GPU default, FORCE_CPU, allowed0 CPU/no Dither, immutable options and no Lab getter')
PY
