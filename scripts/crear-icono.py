#!/usr/bin/env python3
"""Generate the project's own 512x512 package icon with only Python stdlib."""
# SPDX-License-Identifier: GPL-3.0-only
import pathlib
import re
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
SIZE = 512
PIXELS = bytearray(bytes((16, 21, 27, 255)) * (SIZE * SIZE))

def rect(x, y, w, h, color):
    for row in range(max(0, y), min(SIZE, y + h)):
        for col in range(max(0, x), min(SIZE, x + w)):
            offset = (row * SIZE + col) * 4
            PIXELS[offset:offset + 4] = bytes(color)

font = {}
source = (ROOT / 'src/ui/canvas.c').read_text()
for char, values in re.findall(r"\{'(.)',\{([\d,]+)\}\}", source):
    font[char] = [int(v) for v in values.split(',')]

def text(x, y, scale, label, color):
    for char in label:
        for row, bits in enumerate(font.get(char, [])):
            for col in range(5):
                if bits & (1 << (4 - col)):
                    rect(x + col * scale, y + row * scale, scale, scale, color)
        x += 6 * scale

rect(0, 0, 512, 12, (110, 216, 114, 255))
rect(56, 66, 400, 314, (28, 38, 48, 255))
text(96, 104, 28, 'X4', (110, 216, 114, 255))
text(112, 423, 7, 'XCLOUD4', (235, 241, 245, 255))

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

rows = b''.join(b'\x00' + PIXELS[y * SIZE * 4:(y + 1) * SIZE * 4] for y in range(SIZE))
png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
output = ROOT / 'assets/icon0.png'
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(png)
print(output)
