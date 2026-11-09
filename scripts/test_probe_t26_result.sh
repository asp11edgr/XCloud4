#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_output=${X4_T26_RESULT_TEST_OUTPUT:-$(mktemp -d /tmp/x4-t26-result-XXXXXX)}
if [[ -z ${X4_T26_RESULT_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$task_output"' EXIT;fi
mkdir -p "$task_output"
dep=${X4_MBEDTLS_SOURCE:-$HOME/.local/share/xcloud4/webrtc-clean-17/mbedtls}
test -f "$dep/library/sha256.c" && test -f "$dep/library/platform_util.c"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca
    -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_CAPACITY_1080=1 -I"$dep/include")
if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";flags+=("${extra[@]}");fi
"${CC:-cc}" "${flags[@]}" tests/probe_t26_result_test.c src/video/probe_t26_result.c \
    src/video/probe_t26_plan.c src/video/video_capacity.c src/auth/json.c \
    "$dep/library/sha256.c" "$dep/library/platform_util.c" -lm -o "$task_output/probe-t26-result"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 90s "$task_output/probe-t26-result" "$task_output/result-fixture.json"
python3 -B - "$task_output/result-fixture.json" <<'PY'
import json,pathlib,sys
def reject(value): raise ValueError('Nonfinite JSON '+value)
v=json.loads(pathlib.Path(sys.argv[1]).read_text(),parse_constant=reject)
assert v['schema']=='xcloud4-t26-cell-v1' and v['config']['depth']==2
assert v['reason']=='ready "NV12" \\ verified'
assert len(v['modes'])==2 and v['modes'][0]['accepted']
assert v['modes'][0]['decode']['p95_us']==9000
assert v['modes'][0]['cpu']['process']==[20_000_000,200.0]
assert v['modes'][0]['hashes'][0]['compared'] is False
assert v['modes'][1]['hashes'][0]['compared'] is True
assert v['modes'][0]['realistic_gpu']==dict(requested=False,gpu_frames=0,cpu_frames=0,reason=None,scope='whole_run_warmup_and_measured')
assert v['modes'][0]['backend']['memory']['checks'][0][7]==-2147352563
assert pathlib.Path(sys.argv[1]).stat().st_size<65536
print('PASS independent real-cell JSON/schema/stats/CPU/hash/memory reader')
PY
for pair in 0-0 0-1 1-0;do
 probe=${pair%-*};bench=${pair#*-}
 "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_VIDEO_PROBE="$probe" -DX4_PROBE_T26="$bench" \
    -c src/video/probe_t26_result.c -o "$task_output/off-$pair.o"
 if "${NM:-nm}" --defined-only "$task_output/off-$pair.o" | grep -Eq ' [TtBbDdRr] ';then
  printf 'FAIL T26 result runtime symbol with probe=%s bench=%s\n' "$probe" "$bench";exit 1
 fi
done
printf '%s\n' 'PASS T26 bounded result JSON, strict resume reader, visible-only real SHA256 and disabled gates'
