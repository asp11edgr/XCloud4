#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
sdk_include=${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include
units=(src/ui/apple_ui.c third_party/qrcodegen/qrcodegen.c src/ui/picture_settings.c
 src/ui/server_settings.c src/ui/access_country.c src/ui/stream_settings.c src/ui/canvas.c
 src/ui/font.c src/ui/lang.c src/streaming/stream_profile.c tests/script_settings_ui_stubs.c)
for public in 0 1; do
 "${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
  -DX4_STREAM_PROFILE=1 -DX4_VIDEO_PROBE=0 -DX4_PUBLIC="$public" -Itests/host_stubs -idirafter "$sdk_include" \
  "-DX4_LAB_RECOVERY_SETTINGS_PATH=\"$out/recovery.bin\"" \
  "-DX4_LAB_GPU_SETTINGS_PATH=\"$out/gpu.bin\"" ${X4_TEST_CFLAGS:-} \
  tests/settings43_test.c "${units[@]}" -pthread -lm -o "$out/settings-$public"
 "$out/settings-$public" "$out/recovery.bin"
 python3 - "$out/settings-$public" "$public" <<'PY'
from pathlib import Path
import sys
data=Path(sys.argv[1]).read_bytes(); public=int(sys.argv[2])
labels=("Pérdida simulada","Simulated loss","NACK + RTX + FlexFEC","NACK y reorden","Loss report to Xbox","Como Edge","Repaired (T43b)",
        "Aviso de ancho de banda (REMB)","Bandwidth feedback (REMB)","Límite fijo (REMB)","Fixed limit (REMB)")
assert all((label.encode('utf-8') in data)==(not public) for label in labels)
print('PASS T44a Lab-only recovery/loss/REMB UI labels in compiled binary, public='+str(public))
PY
done
