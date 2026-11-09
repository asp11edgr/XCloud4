#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/x4-script-engine.XXXXXX")
trap 'rm -r -- "$tmp_dir"' EXIT

CC="${CC:-clang}"
test_flags=(-DX4_SCRIPT_ENGINE_TEST=1 "-DX4_SCRIPT_TEST_TMPDIR=\"$tmp_dir\"" "-DX4_SCRIPT_PERSIST_PATH=\"$tmp_dir/lab-scripts-v2.bin\"")
test_args=()
if [[ "${X4_SCRIPT_TEST_ONLY:-}" == "histogram" ]]; then
    test_args+=(--histogram-only)
elif [[ -n "${X4_SCRIPT_TEST_ONLY:-}" ]]; then
    echo "Unsupported script-engine test selector" >&2
    exit 1
fi
check_metrics() {
    if [[ "${X4_SCRIPT_TEST_ONLY:-}" == "histogram" ]]; then return; fi
    python3 -B - "$tmp_dir/metrics.json" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]); raw=p.read_text(encoding='utf-8'); value=json.loads(raw)
assert value['active_script'] and value['memory_bytes']>0
assert value['status']=='disabled' and '"quoted"' in value['disabled_reason']
assert 'function on_input' not in raw
print('PASS: actual metrics JSON parsed; source excluded; exported before Lua unload.')
PY
}

OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}"

LUA_SRCS=(
    third_party/lua/lapi.c third_party/lua/lauxlib.c third_party/lua/lbaselib.c
    third_party/lua/lcode.c third_party/lua/lctype.c third_party/lua/ldebug.c
    third_party/lua/ldo.c third_party/lua/ldump.c third_party/lua/lfunc.c
    third_party/lua/lgc.c third_party/lua/llex.c third_party/lua/lmathlib.c
    third_party/lua/lmem.c third_party/lua/lobject.c third_party/lua/lopcodes.c
    third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c
    third_party/lua/lstrlib.c third_party/lua/ltable.c third_party/lua/ltablib.c
    third_party/lua/ltm.c third_party/lua/lundump.c third_party/lua/lutf8lib.c
    third_party/lua/lvm.c third_party/lua/lzio.c
)

echo "=== Compilando y ejecutando test_script_engine (PLAIN) ==="
"$CC" -std=c11 -O2 -Wall -Wextra \
    -Wno-unused-parameter -Wno-sign-compare \
    -idirafter "$OO_PS4_TOOLCHAIN/include" \
    -Ithird_party/lua -Isrc/input -Isrc/core -Isrc/ui \
    "${test_flags[@]}" "${LUA_SRCS[@]}" \
    src/input/controller.c \
    src/input/script_engine.c src/ui/lang.c \
    tests/test_script_engine.c \
    -lm \
    -o "$tmp_dir/test_script_engine"

"$tmp_dir/test_script_engine" "${test_args[@]}"
check_metrics

if [[ "${X4_TEST_ASAN:-1}" == "1" ]]; then
    ASAN_CC="$CC"
    if ! "$ASAN_CC" -fsanitize=address,undefined -x c - -o /dev/null << 'EOF' 2>/dev/null
#include <stdio.h>
int main() { return 0; }
EOF
    then
        ASAN_CC="gcc"
    fi
    if "$ASAN_CC" -fsanitize=address,undefined -x c - -o /dev/null << 'EOF' 2>/dev/null
#include <stdio.h>
int main() { return 0; }
EOF
    then
        echo "=== Compilando y ejecutando test_script_engine (ASan / UBSan con $ASAN_CC) ==="
        "$ASAN_CC" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
            -Wall -Wextra \
            -Wno-unused-parameter -Wno-sign-compare \
            -idirafter "$OO_PS4_TOOLCHAIN/include" \
            -Ithird_party/lua -Isrc/input -Isrc/core -Isrc/ui \
            "${test_flags[@]}" "${LUA_SRCS[@]}" \
            src/input/controller.c \
            src/input/script_engine.c src/ui/lang.c \
            tests/test_script_engine.c \
            -lm \
            -o "$tmp_dir/test_script_engine_asan"
        "$tmp_dir/test_script_engine_asan" "${test_args[@]}"
        check_metrics
    else
        echo "FAIL: ASan/UBSan compiler unavailable" >&2
        exit 1
    fi
fi

echo "=== SCRIPT ENGINE TESTS PASSED (selection=${X4_SCRIPT_TEST_ONLY:-full}) ==="
