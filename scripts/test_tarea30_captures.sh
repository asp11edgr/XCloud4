#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?Owned task capture directory required}
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-reconnect-capture.XXXXXXXX")
trap 'rm -r -- "$build"' EXIT
mkdir -p "$out"
sdk="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
flags=(-std=c23 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror
       -ffunction-sections -fdata-sections -DX4_PUBLIC=0 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0
       -Itests/host_stubs -idirafter "$sdk")
"${CC:-clang}" "${flags[@]}" tests/capture_tarea30_reconnect.c src/input/touch_menu.c src/ui/font.c src/ui/screen.c \
    src/ui/apple_ui.c src/ui/canvas.c src/ui/lang.c src/ui/net_hint.c \
    third_party/qrcodegen/qrcodegen.c -Wl,--gc-sections -pthread -lm -o "$build/capture"
for language in es en;do
    for state in reconnect menu;do "$build/capture" "$out" "$state" "$language";done
done
python3 -B - "$out" <<'PY'
import hashlib,pathlib,sys
files=sorted(pathlib.Path(sys.argv[1]).glob('*.ppm'))
assert len(files)==4
rows=[(hashlib.sha256(p.read_bytes()).hexdigest(),p.name) for p in files]
assert len({r[0] for r in rows})==4
for digest,name in rows:print(digest,name)
print('PASS four distinct captures through actual UI; host only')
PY
