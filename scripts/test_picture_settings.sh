#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -f -- "$build_dir"/*; rmdir -- "$build_dir"' EXIT

compile() {
    local edition=$1 picture_source=$2 output=$3
    "${CC:-clang}" -std=c11 -O1 -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} -DX4_PUBLIC="$edition" \
        -Itests/host_stubs src/ui/apple_ui.c third_party/qrcodegen/qrcodegen.c "$picture_source" \
        src/ui/server_settings.c src/ui/access_country.c src/ui/canvas.c src/ui/font.c src/ui/lang.c \
        src/streaming/stream_profile.c tests/test_picture_settings.c -lpthread -lm -o "$output"
}

for edition in 0 1; do
    compile "$edition" src/ui/picture_settings.c "$build_dir/test-$edition"
    (cd "$build_dir" && "./test-$edition")
done

if [[ -n ${X4_PICTURE43C_CAPTURE_DIR:-} ]]; then
    mkdir -p -- "$X4_PICTURE43C_CAPTURE_DIR"
    "$build_dir/test-1" --capture "$build_dir/public.ppm"
    if [[ -n ${X4_PICTURE43C_LAB_BASE:-} ]]; then
        compile 0 "$X4_PICTURE43C_LAB_BASE" "$build_dir/lab-base"
        "$build_dir/lab-base" --capture "$build_dir/lab-base.ppm"
        "$build_dir/test-0" --capture "$build_dir/lab.ppm"
        cmp -- "$build_dir/lab-base.ppm" "$build_dir/lab.ppm"
    fi
    python3 - "$build_dir" "$X4_PICTURE43C_CAPTURE_DIR" <<'PY'
import hashlib
from pathlib import Path
import struct
import sys
import zlib

source, output = map(Path, sys.argv[1:])
width, height = 1920, 1080
stride = width * 3
prefix = b"P6\n1920 1080\n255\n"

def rgb(name):
    ppm = (source / (name + ".ppm")).read_bytes()
    assert ppm.startswith(prefix) and len(ppm) == len(prefix) + height * stride
    return ppm[len(prefix):]

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

if (source / "lab.ppm").exists():
    assert rgb("lab") == rgb("lab-base")
    print("PASS 43c Lab Picture settings unchanged PIXELS_SHA256", hashlib.sha256(rgb("lab")).hexdigest())
pixels = rgb("public")
scanlines = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))
compressed = zlib.compress(scanlines, 6)
assert zlib.decompress(compressed) == scanlines
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", compressed)
    + chunk(b"IEND", b"")
)
target = output / "calidad-imagen-publica-43c.png"
with target.open("xb") as stream:
    stream.write(png)
print("PASS 43c Public capture", target.name, "PNG_SHA256", hashlib.sha256(target.read_bytes()).hexdigest(),
      "PIXELS_SHA256", hashlib.sha256(pixels).hexdigest())
PY
fi
