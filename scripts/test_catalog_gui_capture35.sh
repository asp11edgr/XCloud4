#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
data=${1:?Pass the retained Phase A data directory}
out=${2:?Pass a new capture output directory}
base=${3:?Pass the verified T35 captures directory}
cd "$(dirname "$0")/.."
cc=${CC:-clang}
build=$(mktemp -d "${TMPDIR:-/tmp}/xcloud4-t35b-capture.XXXXXXXX")
mkdir -p "$out"
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -DX4_CATALOG_GUI_TEST -O2 -g -Wall -Wextra -Werror
       -ffunction-sections -fdata-sections -Itests/host_ui)
sources=(tests/catalog_gui_capture35.c src/ui/catalog_gui.c src/ui/catalog_browser.c
         src/ui/home_rows.c src/ui/recent_games.c src/ui/font.c
         src/ui/apple_ui.c src/ui/lang.c src/ui/net_hint.c src/ui/access_country.c
         src/ui/product_metadata.c
         src/ui/image_area.c src/auth/json.c src/auth/public_image_policy.c third_party/qrcodegen/qrcodegen.c)
"$cc" "${flags[@]}" "${sources[@]}" -Wl,--gc-sections -pthread -lm -o "$build/plain"
"$build/plain" "$data" "$build"
python3 -B - "$build" "$out" "$base" <<'PY'
import hashlib
import pathlib
import struct
import sys
import zlib

source, output, baseline = map(pathlib.Path, sys.argv[1:])
names = ("01-jugar-forza", "05-catalogo", "06-recientes")
t35_png_sha = {
    "01-jugar-forza": "24983d0bffbef475b46b01bda901ad7be4084493747d13b04730c3aaa770ac3d",
    "05-catalogo": "f6852b6805a6d137de515d407e6118353f206ac756c1043393e9b2de91811c50",
}
signature = b"\x89PNG\r\n\x1a\n"
header_data = struct.pack(">IIBBBBB", 1920, 1080, 8, 2, 0, 0, 0)
row_bytes = 1920 * 3
row_stride = row_bytes + 1

def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def rgb_pixels(png):
    # Both the prior and current captures have fixed RGB8 rows with filter 0.
    assert png.startswith(signature)
    pos = len(signature)
    chunks = []
    while pos < len(png):
        size, kind = struct.unpack_from(">I4s", png, pos)
        end = pos + 12 + size
        assert end <= len(png)
        data = png[pos + 8:end - 4]
        assert png[pos:end] == chunk(kind, data)
        chunks.append((kind, data))
        pos = end
    assert [kind for kind, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"]
    assert chunks[0][1] == header_data and chunks[2][1] == b""
    rows = zlib.decompress(chunks[1][1])
    assert len(rows) == 1080 * row_stride and rows[::row_stride] == b"\0" * 1080
    return b"".join(rows[y + 1:y + row_stride] for y in range(0, len(rows), row_stride))


for name in names:
    ppm = (source / (name + ".ppm")).read_bytes()
    header = b"P6\n1920 1080\n255\n"
    assert ppm.startswith(header) and len(ppm) == len(header) + row_bytes * 1080
    pixels = ppm[len(header):]
    pixel_sha = hashlib.sha256(pixels).hexdigest()
    if name in t35_png_sha:
        original = (baseline / (name + ".png")).read_bytes()
        assert hashlib.sha256(original).hexdigest() == t35_png_sha[name], name
        t35_pixels = rgb_pixels(original)
        assert t35_pixels == pixels, f"{name}: pixels changed from T35"
        print("PASS_PIXELES_T35", name, pixel_sha)

    rows = b"".join(b"\0" + pixels[y * row_bytes:(y + 1) * row_bytes] for y in range(1080))
    png = signature + chunk(b"IHDR", header_data) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    target = output / (name + ".png")
    with target.open("xb") as stream:
        stream.write(png)
    assert rgb_pixels(target.read_bytes()) == pixels
    print("PNG_PASS", target.name, len(png), hashlib.sha256(png).hexdigest(), "PIXELS_SHA256", pixel_sha)
PY
printf 'Capture host binaries retained at %s\n' "$build"
