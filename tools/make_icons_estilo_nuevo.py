#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Render the approved concept 2 using the Inter Bold font in this tree.

Reproduce the checked-in bytes with Pillow 13.0.0, FreeType 2.14.3 and zlib
1.3.2. Run with --check to verify the assets without writing them.
"""

import argparse
import hashlib
import io
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ASSETS = Path(__file__).resolve().parents[1] / "assets"
FONT = str(ASSETS / "fonts" / "Inter-Bold.ttf")
SS = 4
NAVY = (11, 18, 32)
GRAPHITE = (30, 37, 51)
GREEN = (34, 227, 110)
CYAN = (0, 212, 255)
WHITE = (255, 255, 255)


def radial(w, h, cx, cy, radius, inner, outer):
    small = Image.new("RGB", (w // 8, h // 8))
    pixels = small.load()
    for y in range(small.height):
        for x in range(small.width):
            t = min(1.0, math.hypot(x * 8 - cx, y * 8 - cy) / radius)
            t = t * t * (3 - 2 * t)
            pixels[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3))
    return small.resize((w, h), Image.Resampling.BICUBIC)


def hgrad(w, h, left, right):
    gradient = Image.new("RGB", (w, h))
    draw = ImageDraw.Draw(gradient)
    for x in range(w):
        t = x / max(1, w - 1)
        draw.line([(x, 0), (x, h)], fill=tuple(int(left[i] + (right[i] - left[i]) * t) for i in range(3)))
    return gradient


def cloud_mask(size, cx, cy, w):
    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    h = w * 0.60
    x0 = cx - w / 2
    base = cy + h / 2
    radius = h * 0.30
    draw.rounded_rectangle([x0, base - 2 * radius, x0 + w, base], radius=radius, fill=255)
    for fx, fy, fr in ((0.30, 0.58, 0.23), (0.56, 0.40, 0.32), (0.79, 0.62, 0.19)):
        r = fr * w
        x = x0 + fx * w
        y = cy - h / 2 + fy * h
        draw.ellipse([x - r, y - r, x + r, y + r], fill=255)
    return mask


def cloud_outline(img, cy, width, thickness, blur):
    mask = cloud_mask(img.size, img.width * 0.5, cy, width)
    inner = mask
    for _ in range(thickness):
        inner = inner.filter(ImageFilter.MinFilter(3))
    outline = ImageChops.subtract(mask, inner)
    gradient = hgrad(img.width, img.height, CYAN, GREEN)
    img.paste(gradient, (0, 0), outline.filter(ImageFilter.GaussianBlur(blur)).point(lambda v: int(v * 0.6)))
    img.paste(gradient, (0, 0), outline)


def wordmark(img, y, size):
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, size)
    name, digit = "XCloud", "4"
    name_width = draw.textlength(name, font=font)
    digit_width = draw.textlength(digit, font=font)
    x = (img.width - name_width - digit_width) / 2
    draw.text((x, y), name, font=font, fill=WHITE)
    draw.text((x + name_width, y), digit, font=font, fill=GREEN)


def lab_badge(img, scale, margin):
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, 34 * scale)
    w, h = 124 * scale, 54 * scale
    x, y = img.width - margin * scale, margin * scale
    draw.rounded_rectangle([x - w, y, x, y + h], radius=12 * scale, fill=(242, 158, 46))
    left, top, right, bottom = draw.textbbox((0, 0), "LAB", font=font)
    draw.text((x - w / 2 - (right - left) / 2 - left, y + h / 2 - (bottom - top) / 2 - top),
              "LAB", font=font, fill=(24, 18, 8))


def icon(lab=False):
    size = 512 * SS
    img = radial(size, size, size * 0.5, size * 0.36, size * 0.78, GRAPHITE, NAVY)
    cloud_outline(img, size * 0.39, size * 0.62, 11 * SS, 18 * SS)
    wordmark(img, int(size * 0.73), 74 * SS)
    if lab:
        lab_badge(img, SS, 26)
    return img.resize((512, 512), Image.Resampling.LANCZOS)


def splash():
    w, h = 1920, 1080
    img = radial(w, h, w * 0.5, h * 0.45, w * 0.62, (26, 40, 58), NAVY)
    green_glow = Image.new("L", img.size, 0)
    ImageDraw.Draw(green_glow).ellipse([w * 0.62, h * 0.55, w * 1.25, h * 1.35], fill=255)
    img.paste(GREEN, (0, 0), green_glow.filter(ImageFilter.GaussianBlur(160)).point(lambda v: int(v * 0.22)))
    cyan_glow = Image.new("L", img.size, 0)
    ImageDraw.Draw(cyan_glow).ellipse([-w * 0.25, -h * 0.35, w * 0.38, h * 0.45], fill=255)
    img.paste(CYAN, (0, 0), cyan_glow.filter(ImageFilter.GaussianBlur(160)).point(lambda v: int(v * 0.14)))
    cloud_outline(img, h * 0.40, 330, 9, 14)
    wordmark(img, int(h * 0.56), 96)
    return img


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify PNG bytes without writing")
    args = parser.parse_args()

    public_splash = splash()
    lab_splash = public_splash.copy()
    lab_badge(lab_splash, 1, 56)
    images = {
        "icon0.png": icon(), "icon0-lab.png": icon(lab=True),
        "pic1.png": public_splash, "pic1-lab.png": lab_splash,
    }
    failed = False
    for name, image in images.items():
        output = ASSETS / name
        buffer = io.BytesIO()
        image.save(buffer, format="PNG")
        data = buffer.getvalue()
        matches = output.is_file() and output.read_bytes() == data
        if matches:
            status = "identical"
        elif args.check:
            status = "mismatch"
            failed = True
        else:
            output.write_bytes(data)
            status = "written"
        print(f"{hashlib.sha256(data).hexdigest()}  assets/{name}  {image.width}x{image.height}  {status}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
