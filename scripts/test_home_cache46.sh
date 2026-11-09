#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
if [[ $# != 0 && $# != 2 ]]; then
    echo "Uso: $0 [ruta/fase-a/captures/after ruta/fase-b/captures/after]" >&2
    exit 2
fi
phase_a=${1:-}
phase_b=${2:-}
if [[ $# == 2 ]]; then
    phase_a=$(realpath -e -- "$phase_a")
    phase_b=$(realpath -e -- "$phase_b")
fi
cd "$(dirname "$0")/.."
out=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-menu-cache46.XXXXXX")
trap 'rm -f -- "$out/menu-lab" "$out/menu-public"; rmdir -- "$out"' EXIT

for edition in lab public; do
    public=0
    if [[ $edition == public ]]; then public=1; fi
    "${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Wall -Wextra -Werror \
        ${X4_TEST_CFLAGS:-} -DX4_PUBLIC="$public" -ffunction-sections -fdata-sections \
        -Itests/host_stubs -Isrc/ui -Isrc/auth -idirafter "$OO_PS4_TOOLCHAIN/include" \
        tests/test_home_cache46.c src/ui/screen.c src/ui/apple_ui.c src/ui/font.c \
        src/ui/canvas.c src/ui/lang.c third_party/qrcodegen/qrcodegen.c \
        -Wl,--gc-sections -Wl,--wrap=malloc -Wl,--wrap=x4_apple_vertical_gradient_round_rect \
        -lm -o "$out/menu-$edition"
done
python3 -B - "$out" "$phase_a" "$phase_b" <<'PY'
import json
import subprocess
import sys
from pathlib import Path

out = Path(sys.argv[1])
frame_bytes = 1920 * 1080 * 4
editions = ("lab", "public")
for edition in editions:
    command = out / f"menu-{edition}"
    with subprocess.Popen([command, "full"], stdout=subprocess.PIPE) as full, \
            subprocess.Popen([command, "incremental"], stdout=subprocess.PIPE) as incremental:
        for i in range(32):
            baseline = full.stdout.read(frame_bytes)
            current = incremental.stdout.read(frame_bytes)
            assert len(baseline) == len(current) == frame_bytes, (edition, i, "short frame")
            assert baseline == current, (edition, i, "different framebuffer pixels")
        assert not full.stdout.read(1) and not incremental.stdout.read(1), edition
        assert full.wait() == incremental.wait() == 0, edition
    print(f"PASS menú T46 {edition}: 32 pasos idénticos al repintado completo sin caché")

if sys.argv[2]:
    phase_a, phase_b = map(Path, sys.argv[2:])
    scenes = ("menu-continuar", "menu-sin-recientes", "menu-error-recientes", "menu-sin-sesion")
    for edition in editions:
        for language in ("es", "en"):
            relative = Path(edition) / language / "pixels.json"
            baseline = json.loads((phase_a / relative).read_text(encoding="ascii"))
            current = json.loads((phase_b / relative).read_text(encoding="ascii"))
            for scene in scenes:
                assert baseline[scene] == current[scene], (edition, language, scene)
    print("PASS menú T46: SHA de píxeles idéntico en 16 capturas de la fase A aprobada")
PY
