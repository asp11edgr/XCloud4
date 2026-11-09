#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?Owned task capture directory required}
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
mkdir -p "$build/app" "$build/data" "$build/usb" "$out"
cp assets/scripts/giroscopio.lua assets/scripts/reasignar.lua \
    assets/scripts/sticks.lua assets/scripts/turbo.lua "$build/app/"
sdk="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
lua=(third_party/lua/lapi.c third_party/lua/lauxlib.c third_party/lua/lbaselib.c
    third_party/lua/lcode.c third_party/lua/lctype.c third_party/lua/ldebug.c
    third_party/lua/ldo.c third_party/lua/ldump.c third_party/lua/lfunc.c
    third_party/lua/lgc.c third_party/lua/llex.c third_party/lua/lmathlib.c
    third_party/lua/lmem.c third_party/lua/lobject.c third_party/lua/lopcodes.c
    third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c
    third_party/lua/lstrlib.c third_party/lua/ltable.c third_party/lua/ltablib.c
    third_party/lua/ltm.c third_party/lua/lundump.c third_party/lua/lutf8lib.c
    third_party/lua/lvm.c third_party/lua/lzio.c)
"${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare \
    -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -DX4_PUBLIC=0 -Itests/host_stubs -idirafter "$sdk" \
    -DX4_SCRIPT_DIR_APP=\""$build/app"\" -DX4_SCRIPT_DIR_DATA=\""$build/data"\" \
    -DX4_SCRIPT_DIR_REL=\""$build/app"\" -DX4_SCRIPT_DIR_USB=\""$build/usb"\" \
    -DX4_SCRIPT_PERSIST_PATH=\""$build/selection.bin"\" \
    -Ithird_party/lua "${lua[@]}" src/input/script_engine.c src/ui/stream_settings.c \
    src/ui/canvas.c src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c \
    tests/capture_tarea28b_screens.c -lm -o "$build/capture"
for language in es en;do
    for state in none selected warning;do "$build/capture" "$out" "$state" "$language";done
done
python3 -B - "$out" <<'PY'
import hashlib,pathlib,sys
p=pathlib.Path(sys.argv[1]);files=sorted(p.glob('r3_script_*.ppm'))
assert len(files)==6
hashes={hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
assert len(hashes)==6,'All six real-state captures must differ'
for f in files:print(hashlib.sha256(f.read_bytes()).hexdigest(),f.name)
print('PASS six distinct real R3 captures; not PS4 runtime evidence')
PY
