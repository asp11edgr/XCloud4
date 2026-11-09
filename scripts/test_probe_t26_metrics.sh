#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_output=${X4_T26_METRICS_TEST_OUTPUT:-$(mktemp -d /tmp/x4-t26-metrics-XXXXXX)}
if [[ -z ${X4_T26_METRICS_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$task_output"' EXIT;fi
mkdir -p "$task_output"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca
    -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1)
if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";flags+=("${extra[@]}");fi
"${CC:-cc}" "${flags[@]}" tests/probe_t26_metrics_test.c src/video/probe_t26_metrics.c \
    -lm -o "$task_output/probe-t26-metrics"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30s "$task_output/probe-t26-metrics" "$task_output/metrics-fixture.json"
python3 -B - "$task_output/metrics-fixture.json" <<'PY'
import json,math,pathlib,sys
def reject(value): raise ValueError('Nonfinite JSON '+value)
v=json.loads(pathlib.Path(sys.argv[1]).read_text(),parse_constant=reject)
assert v['empty'] is None
assert v['rank']==dict(n=1024,p50_us=512,p95_us=973,p99_us=1014,max_us=1024,mean_us=512.5)
assert v['huge']['n']==1024 and v['huge']['max_us']==2**64-1
assert all(v['huge'][p]==2**64-1 for p in ('p50_us','p95_us','p99_us'))
assert math.isfinite(v['huge']['mean_us']) and v['huge']['mean_us']==float(2**64-1)
print('PASS independent JSON stats reader: null/nearest-rank/finite/full uint64')
PY
for pair in 0-0 0-1 1-0;do
 probe=${pair%-*};bench=${pair#*-}
 "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_VIDEO_PROBE="$probe" -DX4_PROBE_T26="$bench" \
    -c src/video/probe_t26_metrics.c -o "$task_output/off-$pair.o"
 if "${NM:-nm}" --defined-only "$task_output/off-$pair.o" | grep -Eq ' [TtBbDdRr] ';then
  printf 'FAIL T26 metrics runtime symbol with probe=%s bench=%s\n' "$probe" "$bench";exit 1
 fi
done
printf '%s\n' 'PASS T26 exact statistics, delayed-output inference, corruption rejection and disabled gates'
