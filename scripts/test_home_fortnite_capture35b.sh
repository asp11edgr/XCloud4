#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
baseline=$(realpath -e "${1:?Árbol base T35 verificado}")
data=$(realpath -e "${2:?Directorio de datos públicos T35 + T35b}")
mkdir -p "${3:?Directorio de salida}"
out=$(realpath -e "$3")
[[ "$baseline" != "$root" ]]
build=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-t35b-home-fortnite.XXXXXXXX")
cc=${CC:-clang}
sources=(src/ui/screen.c src/ui/apple_ui.c src/ui/font.c src/ui/canvas.c src/ui/lang.c
         src/ui/access_country.c src/ui/net_hint.c src/ui/server_settings.c src/ui/product_metadata.c
         src/ui/image_area.c src/auth/json.c src/auth/public_image_policy.c third_party/qrcodegen/qrcodegen.c)

capture() (
    tree=$1
    mode=$2
    cd "$tree"
    extra=()
    if [[ "$mode" == 1 ]]; then
        extra=(-DX4_ASSETS_HOST_TEST src/ui/catalog_assets.c)
    fi
    "$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -DX4_PUBLIC=1 -DX4_CAPTURE_NEW_SURFACES="$mode" \
        -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -isystem "${OO_PS4_TOOLCHAIN:-$HOME/.local/share/xcloud4/OpenOrbis/PS4Toolchain}/include" \
        -Itests/host_stubs -Isrc/ui -Isrc/auth "$root/tests/home_fortnite_capture35b.c" \
        "${sources[@]}" "${extra[@]}" -Wl,--gc-sections -pthread -lm -o "$build/capture-$mode"
    "$build/capture-$mode" "$data" "$build"
)

capture "$baseline" 0
capture "$root" 1

python3 -B - "$build" "$out" <<'PY'
import hashlib
import pathlib
import struct
import sys
import zlib

source, output = map(pathlib.Path, sys.argv[1:])
width, height = 1920, 1080
signature = b"\x89PNG\r\n\x1a\n"


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def save(name, pixels, w, h):
    stride = w * 3
    assert len(pixels) == stride * h
    rows = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(h))
    encoded = zlib.compress(rows, 6)
    assert zlib.decompress(encoded) == rows
    header = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = signature + chunk(b"IHDR", header) + chunk(b"IDAT", encoded) + chunk(b"IEND", b"")
    target = output / (name + ".png")
    with target.open("xb") as stream:
        stream.write(png)
    assert target.read_bytes() == png
    print("PNG_PASS", target.name, f"{w}x{h}", hashlib.sha256(png).hexdigest(),
          "PIXELS_SHA256", hashlib.sha256(pixels).hexdigest())


def crop(pixels, x, y, w, h):
    assert 0 <= x < x + w <= width and 0 <= y < y + h <= height
    return b"".join(pixels[(row * width + x) * 3:(row * width + x + w) * 3]
                    for row in range(y, y + h))


regions = {
    "menu": [("menu-forza", 84, 304, 852, 448), ("menu-fortnite", 966, 304, 420, 448)],
    "fortnite": [("pantalla-fortnite", 84, 170, 550, 770)],
}
header = f"P6\n{width} {height}\n255\n".encode()
for screen, crops in regions.items():
    images = {}
    for mode in ("antes", "despues"):
        name = f"{screen}-{mode}"
        ppm = (source / (name + ".ppm")).read_bytes()
        assert ppm.startswith(header) and len(ppm) == len(header) + width * height * 3
        images[mode] = ppm[len(header):]
        save(name, images[mode], width, height)
        for region, x, y, w, h in crops:
            save(f"recorte-100-{region}-{mode}", crop(images[mode], x, y, w, h), w, h)
    for region, x, y, w, h in crops:
        assert crop(images["antes"], x, y, w, h) != crop(images["despues"], x, y, w, h), region
    print("PASS_RECORTE_SIN_REESCALA", screen)
PY
