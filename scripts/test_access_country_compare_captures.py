#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify old OPTIONS content exactly; only new third sidebar row may differ."""
import argparse
import json
from pathlib import Path


def ppm(path):
    raw = path.read_bytes()
    prefix = b"P6\n1920 1080\n255\n"
    if not raw.startswith(prefix) or len(raw) != len(prefix) + 1920 * 1080 * 3:
        raise ValueError(f"Unexpected full-resolution PPM format: {path}")
    return raw[len(prefix):]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rows = []
    for language in ("es", "en"):
        for screen in ("settings-picture", "settings-server", "settings-first-server"):
            name = f"{screen}-{language}.ppm"
            before, after = ppm(args.before / name), ppm(args.after / name)
            changes = []
            total = 0
            for i in range(1920 * 1080):
                if before[i * 3:i * 3 + 3] == after[i * 3:i * 3 + 3]:
                    continue
                total += 1
                x, y = i % 1920, i // 1920
                permitted = screen != "settings-first-server" and 132 <= x < 398 and 508 <= y < 590
                if not permitted and len(changes) < 10:
                    changes.append([x, y])
            if changes:
                raise AssertionError(f"{name}: differences outside the new Access sidebar entry: {changes}")
            rows.append({"case": name, "pixels_changed": total,
                "unchanged_outside_new_sidebar_entry": True,
                "whole_image_identical": total == 0})
    result = {"result": "PASS", "allowed_new_sidebar_rect": [132, 508, 266, 82], "cases": rows}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
