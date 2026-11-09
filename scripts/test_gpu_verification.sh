#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${X4_GPU_VERIFY_TEST_OUTPUT:-$(mktemp -d)}
mkdir -p "$out"
read -r -a cc <<< "${X4_GPU_HOST_CC:-${CC:-clang}}"
read -r -a extra <<< "${X4_TEST_CFLAGS:-}"
python3 - "$out" "${cc[@]}" <<'PY'
import pathlib,re,subprocess,sys,hashlib,json
out=pathlib.Path(sys.argv[1]);cc=sys.argv[2:]
source=pathlib.Path('src/video/gpu/verify/verify_rows.ll')
target=subprocess.check_output(cc+['-dumpmachine'],text=True).strip()
assert re.fullmatch(r'x86_64-[A-Za-z0-9_.-]+',target)
text=source.read_text().replace('target triple = "amdgcn--"','target triple = "'+target+'"')
text=text.replace('define amdgpu_cs void @main(','define void @x4_gpu_verify_rows_host(')
text=text.replace(' inreg ',' ').replace(' immarg','')
text=text.replace('@llvm.amdgcn.raw.buffer.load.i32','@x4_verify_raw_load')
text=text.replace('@llvm.amdgcn.raw.buffer.store.i32','@x4_verify_raw_store')
text=text.replace(') #0 {',') {').replace('attributes #0 = { "amdgpu-flat-work-group-size"="64,64" }','')
(out/'verify-host.ll').write_text(text)
(out/'scope.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
 'host_ir_sha256':hashlib.sha256(text.encode()).hexdigest(),'scope':'Actual new LLVM sparse-copy offsets executed on x86; no native GPU/timing evidence'},indent=2)+'\n')
PY
"${cc[@]}" -O2 -g -x ir -c "$out/verify-host.ll" -o "$out/verify-host.o"
for public in 0 1;do
  for scalar in 0 1;do
    scalar_flags=();if [[ $scalar == 1 ]];then scalar_flags=(-DX4_PIPELINE_SCALAR=1);fi
    "${cc[@]}" -std=gnu2x -O2 -g -Wall -Wextra -Werror -pthread -ffp-contract=off \
      -DX4_PUBLIC="$public" -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 "${scalar_flags[@]}" \
      -Itests/video_capacity_stubs -Itests/host_stubs "${extra[@]}" \
      tests/gpu_verification_snapshot_test.c src/video/gpu_quality_ref.c src/video/video_pipeline.c \
      src/video/video_capacity.c "$out/verify-host.o" -o "$out/verify-$public-$scalar"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/verify-$public-$scalar"
  done
done
