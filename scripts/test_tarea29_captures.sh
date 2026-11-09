#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?Owned task capture directory required}
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-account-capture.XXXXXXXX")
trap 'rm -r -- "$build"' EXIT
mkdir -p "$out"
sdk="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror
       -ffunction-sections -fdata-sections -DX4_PUBLIC=1 -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0
       -Itests/host_stubs -idirafter "$sdk")
"${CC:-clang}" "${flags[@]}" -Dx4_text_px=x4_text_px_impl \
    -Dx4_text_ellipsis_px=x4_text_ellipsis_px_impl -c src/ui/font.c -o "$build/font.o"
"${CC:-clang}" "${flags[@]}" tests/capture_tarea29_account.c src/ui/screen.c \
    src/ui/apple_ui.c src/ui/canvas.c src/ui/lang.c src/ui/net_hint.c \
    third_party/qrcodegen/qrcodegen.c "$build/font.o" \
    -Wl,--gc-sections -pthread -lm -o "$build/capture"
for language in es en;do
    for state in option warning;do "$build/capture" "$out" "$state" "$language";done
done
python3 -B - "$out" <<'PY'
import hashlib,pathlib,sys
root=pathlib.Path(sys.argv[1]);files=sorted(root.glob('account_*.ppm'))
assert len(files)==4
rows=[(hashlib.sha256(p.read_bytes()).hexdigest(),p.name) for p in files]
assert len(set(row[0] for row in rows))==4,'All four actual states must differ'
for digest,name in rows:print(digest,name)
print('PASS four distinct real account captures; host rendering, not PS4 execution')
PY
