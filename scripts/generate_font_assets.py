#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline Inter and Noto Sans alpha-atlas generation. Pillow/FreeType are PC-only.

There is no downloader in this generator. Input TTF hashes must match the
committed official-source manifest; glyphs are never rasterized in the app.
"""
import argparse
import hashlib
import json
import pathlib
import struct

from PIL import Image, ImageDraw, ImageFont, __version__ as pillow_version

MAGIC = b"X4FNT01\0"
SIZES = (18, 26, 28, 32, 38, 46, 56)
CODEPOINTS = sorted(set(range(32, 127)) | set(range(160, 384)) |
                    set(range(0x300, 0x370)) |
                    {0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201c,
                     0x201d, 0x201e, 0x2022, 0x2026, 0x2122, 0xfffd})
INTER_CODEPOINTS = sorted(set(CODEPOINTS) | {0x2039, 0x203a})
INTER_FACE_SPECS = (
    (0, "Inter-Regular.ttf", (18, 22, 26, 28, 32)),
    (1, "Inter-SemiBold.ttf", (20, 24, 28, 32, 40)),
    (2, "Inter-Bold.ttf", (24, 28, 32, 44, 64)),
)
MAX_ATLAS_BYTES = 4 * 1024 * 1024


def build(directory, sizes=SIZES, weights=(0, 1)):
    faces, glyphs, alpha = [], [], bytearray()
    manifest = json.loads((directory / "SOURCE.json").read_text(encoding="utf-8"))
    for weight in weights:
        filename = "NotoSans-SemiBold.ttf" if weight else "NotoSans-Regular.ttf"
        path = directory / filename
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != manifest["files"][filename]["sha256"]:
            raise ValueError(f"Unverified font source: {filename}")
        for size in sizes:
            font = ImageFont.truetype(str(path), size,
                                      layout_engine=ImageFont.Layout.BASIC)
            ascent, descent = font.getmetrics()
            first = len(glyphs)
            for cp in CODEPOINTS:
                char = chr(cp)
                left, top, right, bottom = font.getbbox(char, anchor="ls")
                width, height = right - left, bottom - top
                glyph = Image.new("L", (max(1, width), max(1, height)))
                if width and height:
                    ImageDraw.Draw(glyph).text((-left, -top), char, font=font,
                                              fill=255, anchor="ls")
                offset = len(alpha)
                if width and height:
                    alpha.extend(glyph.tobytes())
                advance = int(round(font.getlength(char) * 64))
                glyphs.append((cp, offset, width, height, left, ascent + top,
                               advance, 0))
            faces.append((size, weight, first, len(CODEPOINTS), ascent,
                          ascent + descent, 0, 0))
    header = struct.pack("<8s6I", MAGIC, 1, len(faces), len(glyphs),
                         len(alpha), 32, 24)
    payload = (header + b"".join(struct.pack("<8I", *f) for f in faces) +
               b"".join(struct.pack("<IIHHhhII", *g) for g in glyphs) + alpha)
    if len(payload) > MAX_ATLAS_BYTES:
        raise ValueError("Atlas exceeds the fixed 4 MiB runtime budget")
    return bytes(payload), len(alpha), len(faces), len(glyphs)


def build_inter(directory, max_bytes):
    faces, glyphs, alpha = [], [], bytearray()
    manifest = json.loads((directory / "SOURCE.json").read_text(encoding="utf-8"))
    for weight, filename, sizes in INTER_FACE_SPECS:
        path = directory / filename
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != manifest["files"][filename]["sha256"]:
            raise ValueError(f"Unverified font source: {filename}")
        for size in sizes:
            font = ImageFont.truetype(str(path), size,
                                      layout_engine=ImageFont.Layout.BASIC)
            ascent, descent = font.getmetrics()
            first = len(glyphs)
            for cp in INTER_CODEPOINTS:
                char = chr(cp)
                left, top, right, bottom = font.getbbox(char, anchor="ls")
                width, height = right - left, bottom - top
                glyph = Image.new("L", (max(1, width), max(1, height)))
                if width and height:
                    ImageDraw.Draw(glyph).text((-left, -top), char, font=font,
                                              fill=255, anchor="ls")
                offset = len(alpha)
                if width and height:
                    alpha.extend(glyph.tobytes())
                advance = int(round(font.getlength(char) * 64))
                glyphs.append((cp, offset, width, height, left, ascent + top,
                               advance, 0))
            faces.append((size, weight, first, len(INTER_CODEPOINTS), ascent,
                          ascent + descent, 0, 0))
    header = struct.pack("<8s6I", MAGIC, 1, len(faces), len(glyphs),
                         len(alpha), 32, 24)
    payload = (header + b"".join(struct.pack("<8I", *f) for f in faces) +
               b"".join(struct.pack("<IIHHhhII", *g) for g in glyphs) + alpha)
    if len(payload) > max_bytes:
        raise ValueError(f"Inter atlas ({len(payload)} B) exceeds runtime budget ({max_bytes} B)")
    return bytes(payload), len(alpha), len(faces), len(glyphs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1] /
                                "assets" / "fonts")
    args = parser.parse_args()
    payload, alpha, faces, glyphs = build(args.directory)
    target = args.directory / "noto_sans.x4font"
    target.write_bytes(payload)
    fallback, _, _, _ = build(args.directory, sizes=(32,), weights=(0,))
    header = args.directory / "font_fallback.h"
    rows = ["/* Generated by scripts/generate_font_assets.py. Do not edit. */",
            "/* Noto Project Authors; SIL Open Font License 1.1, see OFL.txt. */",
            "static const unsigned char x4_font_fallback_data[] = {"]
    rows.extend("    " + ",".join(f"0x{b:02x}" for b in fallback[i:i + 24]) + ","
                for i in range(0, len(fallback), 24))
    rows.append("};\n")
    header.write_text("\n".join(rows), encoding="utf-8", newline="\n")

    max_heap_bytes = MAX_ATLAS_BYTES - len(fallback)
    inter_payload, inter_alpha, inter_faces, inter_glyphs = build_inter(
        args.directory, max_heap_bytes
    )
    inter_target = args.directory / "inter.x4font"
    inter_target.write_bytes(inter_payload)

    receipt = {"format": "X4FNT01", "generator": "Pillow " + pillow_version,
               "layout": "BASIC, independent glyph advances, no kerning/shaping",
               "sizes": list(SIZES), "weights": ["Regular", "SemiBold"],
               "codepoints": len(CODEPOINTS), "faces": faces, "glyphs": glyphs,
               "atlas_bytes": len(payload), "alpha_bytes": alpha,
               "fallback_bytes": len(fallback), "runtime_combined_limit": MAX_ATLAS_BYTES,
               "maximum_heap_atlas_bytes": max_heap_bytes,
               "atlas_sha256": hashlib.sha256(payload).hexdigest(),
               "fallback_sha256": hashlib.sha256(fallback).hexdigest(),
               "inter": {
                   "weights": ["Regular", "SemiBold", "Bold"],
                   "face_specs": [
                       {"weight": w, "file": fn, "sizes": list(sz)}
                       for w, fn, sz in INTER_FACE_SPECS
                   ],
                   "codepoints": len(INTER_CODEPOINTS),
                   "faces": inter_faces,
                   "glyphs": inter_glyphs,
                   "atlas_bytes": len(inter_payload),
                   "alpha_bytes": inter_alpha,
                   "atlas_sha256": hashlib.sha256(inter_payload).hexdigest(),
               }}
    (args.directory / "ATLAS.json").write_text(json.dumps(receipt, indent=2) + "\n",
                                              encoding="utf-8", newline="\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
