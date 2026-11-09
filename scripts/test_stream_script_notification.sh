#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?Owned task capture directory required}
build=$(mktemp -d "${TMPDIR:-/tmp}/x4-stream-script-toast.XXXXXXXX")
trap 'rm -r -- "$build"' EXIT
mkdir -p "$out"
python3 -B - "$build/main_stream_overlay_actual.inc" <<'PY'
import pathlib,re,sys
main=pathlib.Path('src/core/main.c').read_text()
start=re.search(r'typedef struct\s*\{\s*const X4LiveMediaSnapshot \*media;',main).start()
end=main.index('static void loading_begin_cached(',start)
actual=main[start:end]
assert actual.count('static void stream_overlay(')==1
assert re.search(r'x4_live_overlay\([^;]+;\s*#if !X4_PUBLIC\s*x4_script_engine_draw_notification\(pixels\);\s*#endif',actual)
pathlib.Path(sys.argv[1]).write_text(actual)
assert main.index('x4_script_engine_process_input(')<main.index('x4_live_media_stream_present(live,stream_overlay,')
media=pathlib.Path('src/media/live_media.c').read_text()
assert re.search(r'return m \? x4_stream_pipeline_present\(m->stream, overlay, context\)',media)
pipeline=pathlib.Path('src/video/stream_pipeline.c').read_text()
assert 'finish_conversion(p,overlay,context,false)' in pipeline
assert 'if(overlay)overlay(p->display_pixels[p->scanout_lease.slot],context);' in pipeline
print('PASS actual Context/function extracted unchanged; on_input and synchronous presentation callback are on main; no new locks or thread handoff')
PY
sdk="${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include"
lua_sources=(third_party/lua/lapi.c third_party/lua/lauxlib.c third_party/lua/lbaselib.c
 third_party/lua/lcode.c third_party/lua/lctype.c third_party/lua/ldebug.c third_party/lua/ldo.c
 third_party/lua/ldump.c third_party/lua/lfunc.c third_party/lua/lgc.c third_party/lua/llex.c
 third_party/lua/lmathlib.c third_party/lua/lmem.c third_party/lua/lobject.c third_party/lua/lopcodes.c
 third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c third_party/lua/lstrlib.c
 third_party/lua/ltable.c third_party/lua/ltablib.c third_party/lua/ltm.c third_party/lua/lundump.c
 third_party/lua/lutf8lib.c third_party/lua/lvm.c third_party/lua/lzio.c)
for public in 0 1;do
 for mode in plain sanitized;do
  flags=(-std=c23 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror
    -ffunction-sections -fdata-sections -DX4_PUBLIC="$public" -DX4_STREAM_PROFILE=2 -DX4_VIDEO_PROBE=0
    -DX4_SCRIPT_ENGINE_TEST=1 -Itests/host_stubs -I"$build" -Ithird_party/lua -idirafter "$sdk"
    -Wno-unused-parameter -Wno-sign-compare)
  if [[ $mode == sanitized ]];then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie);fi
  extra=();if [[ $public == 0 ]];then extra=("${lua_sources[@]}");fi
  "${CC:-clang}" "${flags[@]}" tests/stream_script_notification_test.c src/input/script_engine.c "${extra[@]}" \
    src/input/touch_menu.c src/ui/font.c src/ui/screen.c src/ui/apple_ui.c src/ui/canvas.c src/ui/lang.c \
    src/ui/net_hint.c third_party/qrcodegen/qrcodegen.c -Wl,--gc-sections -pthread -lm -o "$build/toast-$public-$mode"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$build/toast-$public-$mode" "$out/script_notification_es.ppm"
  if [[ $public == 1 ]];then
    if nm "$build/toast-$public-$mode" | grep -Eq 'x4_script_engine_|lua[A-Z_a-z]';then echo 'FAIL public script/Lua symbol' >&2;exit 1;fi
  fi
 done
done
python3 -B - "$out" <<'PY'
import hashlib,pathlib,struct,sys,zlib
root=pathlib.Path(sys.argv[1]);ppm=root/'script_notification_es.ppm'
raw=ppm.read_bytes();header=b'P6\n1920 1080\n255\n';assert raw.startswith(header)
rgb=raw[len(header):];assert len(rgb)==1920*1080*3
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
scan=b''.join(b'\x00'+rgb[y*5760:(y+1)*5760] for y in range(1080))
png=root/'script_notification_es.png'
png.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1920,1080,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan,9))+chunk(b'IEND',b''))
print(hashlib.sha256(png.read_bytes()).hexdigest(),png.name)
print('PASS es PNG derived losslessly from actual host renderer; no PS4 execution')
PY
