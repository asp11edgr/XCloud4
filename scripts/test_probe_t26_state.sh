#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
task_output=${X4_T26_STATE_TEST_OUTPUT:-$(mktemp -d /tmp/x4-t26-state-XXXXXX)}
if [[ -z ${X4_T26_STATE_TEST_OUTPUT:-} ]];then trap 'rm -rf -- "$task_output"' EXIT;fi
mkdir -p "$task_output"
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -Werror=vla -Werror=alloca
    -DX4_VIDEO_PROBE=1 -DX4_PROBE_T26=1 -DX4_STREAM_PROFILE=1)
if [[ -n ${X4_TEST_CFLAGS:-} ]];then read -r -a extra <<< "$X4_TEST_CFLAGS";flags+=("${extra[@]}");fi
"${CC:-cc}" "${flags[@]}" tests/probe_t26_plan_state_test.c src/video/probe_t26_plan.c \
    src/video/probe_t26_state.c src/video/decoder_depth.c -o "$task_output/probe-t26-state"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30s "$task_output/probe-t26-state" "$task_output/state-fixture.json"
python3 -B - "$task_output/state-fixture.json" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]);v=json.loads(p.read_text())
assert v['schema']=='X4T26_STATE1' and v['next']==0 and v['cell']==43
assert v['deadline_us']-v['started_us']==1200000000-v['budget_used_us']
assert len(v['checksum'])==16 and all(c in '0123456789abcdef' for c in v['checksum'])
assert not v['active'] and not v['v1_passed'] and v['best_priority']==700
print('PASS independent JSON reader; schema, identity, budget and checksum shape')
PY
for pair in 0-0 0-1 1-0;do
 probe=${pair%-*};bench=${pair#*-}
 for unit in plan state;do
  "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DX4_VIDEO_PROBE="$probe" -DX4_PROBE_T26="$bench" \
    -c "src/video/probe_t26_$unit.c" -o "$task_output/off-$pair-$unit.o"
  if "${NM:-nm}" --defined-only "$task_output/off-$pair-$unit.o" | grep -Eq ' [TtBbDdRr] ';then
   printf 'FAIL T26 runtime symbol with probe=%s bench=%s unit=%s\n' "$probe" "$bench" "$unit";exit 1
  fi
 done
done
printf '%s\n' 'PASS T26 plan/state/mutations/recovery/logical ownership; disabled gates contain no runtime symbols'
