"""Capture and audit updater errors or manual installation with the real host renderer."""

import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import zlib


WIDTH, HEIGHT = 1920, 1080
STRIDE = WIDTH * 3
SCENES = 11
ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "tests/capture_tarea51b.c", "src/ui/ui2_modal.c", "src/ui/ui2_theme.c",
    "src/ui/ui2_info.c", "src/ui/ui2_shell.c", "src/ui/apple_ui.c", "src/ui/font.c",
    "src/ui/lang.c", "src/ui/canvas.c", "src/ui/product_metadata.c", "src/ui/image_area.c",
    "src/ui/ui_surface.c", "src/ui/ui2_home.c", "src/ui/play_history.c",
    "third_party/qrcodegen/qrcodegen.c",
)


def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def encode_png(rgb):
    raw = b"".join(b"\0" + rgb[i:i + STRIDE] for i in range(0, len(rgb), STRIDE))
    header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(raw, 4)) + png_chunk(b"IEND", b""))


def render(binary, language, out, mode):
    out.mkdir()
    frames = {}
    scene_count, marker_text = {
        "--errors-only": (SCENES, "PASS T51C_ERRORS"),
        "--manual": (1, "PASS T51F_MANUAL"),
        "--manual-and-updated": (2, "PASS T51G_PUBLIC_UI"),
    }[mode]
    with (out / "render.log").open("wb") as log:
        with subprocess.Popen([binary, language, mode], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=log) as proc:
            try:
                while marker := proc.stdout.readline(4096):
                    if marker.startswith(b"XCloud4 UI "):
                        continue
                    match = re.fullmatch(rb"FRAME ([a-z0-9-]+)\n", marker)
                    assert match, marker
                    name = match[1].decode("ascii")
                    assert name not in frames, name
                    pixels = proc.stdout.read(STRIDE * HEIGHT)
                    assert len(pixels) == STRIDE * HEIGHT, name
                    (out / (name + ".png")).write_bytes(encode_png(pixels))
                    frames[name] = hashlib.sha256(pixels).hexdigest()
                assert proc.wait() == 0, (out, proc.returncode)
            except BaseException:
                if proc.poll() is None:
                    proc.kill()
                raise
    assert len(frames) == scene_count and len(set(frames.values())) == scene_count, out
    log_text = (out / "render.log").read_text(encoding="utf-8")
    assert marker_text in log_text, out
    if mode == "--manual-and-updated":
        assert "PASS T51G_GREETING" in log_text, out
    (out / "pixels.json").write_text(json.dumps(frames, indent=2) + "\n", encoding="ascii")


def main():
    assert len(sys.argv) == 2 or (len(sys.argv) == 3 and sys.argv[2] in
        ("--manual", "--manual-and-updated")), (
        "usage: capture_update51c.py OUTPUT_DIRECTORY [--manual | --manual-and-updated]")
    mode = sys.argv[2] if len(sys.argv) == 3 else "--errors-only"
    errors = mode == "--errors-only"
    editions = ((1, "publica"),) if mode == "--manual-and-updated" else ((0, "lab"), (1, "publica"))
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=False)
    sdk = Path(os.environ["OO_PS4_TOOLCHAIN"]) / "include"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("X4_TEST_CFLAGS", ""))
    with tempfile.TemporaryDirectory(prefix="capture51c-") as temp:
        for public, edition in editions:
            binary = Path(temp) / edition
            command = compiler + [
                "-std=c11", "-D_POSIX_C_SOURCE=200809L", f"-DX4_PUBLIC={public}",
                "-DX4_STREAM_PROFILE=1", "-DX4_VIDEO_PROBE=0",
                "-O2", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
                "-Itests/host_ui", "-Itests/host_stubs", "-Isrc/ui", "-Isrc/auth", "-idirafter", str(sdk),
            ] + flags + ["-UNDEBUG", *SOURCES, "-Wl,--gc-sections", "-Wl,--wrap=x4_text_px",
                         "-Wl,--wrap=x4_text_ellipsis_px", "-lm", "-o", str(binary)]
            subprocess.run(command, cwd=ROOT, check=True)
            folder = output / edition
            folder.mkdir()
            for language in (("es", "en") if errors else ("es", "en", "de")):
                render(binary, language, folder / language, mode)
            for language in (("pt-br", "fr", "it", "de") if errors else ()):
                log_path = folder / ("layout-" + language + ".log")
                with log_path.open("wb") as log:
                    subprocess.run([binary, language, "--errors-only", "--audit-only"],
                                   cwd=ROOT, stdout=log, stderr=log, check=True)
                assert "PASS T51C_ERRORS" in log_path.read_text(encoding="utf-8"), log_path
    if mode == "--manual-and-updated":
        print("CAPTURE51G PASS: 6 PNG, es/en/de, public manual and updated, full package names")
    elif mode == "--manual":
        print("CAPTURE51F PASS: 6 PNG, es/en/de, Lab and public, full package names and conflict")
    else:
        print(f"CAPTURE51C PASS: {4 * SCENES} PNG, 11 messages, 6 languages, Lab and public")


if __name__ == "__main__":
    main()
