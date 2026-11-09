#!/usr/bin/env python3
"""Run the history blocks from main with simulated UI/auth/presentation results."""
from pathlib import Path
import errno
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/core/main.c").read_text(encoding="utf-8")
template = (root / "tests/history49_main_template.c").read_text(encoding="utf-8")

def between(start, end):
    first = source.index(start)
    return source[first:source.index(end, first + len(start))]

history = between("typedef struct {\n    char product[X4_PRODUCT_ID_CAP];\n    uint64_t started_us;",
                  "static const char *ui_status_quality")
selected = between("static const X4CatalogTitle *game_page_selected(", "static void game_page_rebind")
launch = between("bool launch_game_page = false;", "if (!closing && screen.page == 5 && previous_page == 5")
launch_gate = between("if (launch_game_page &&", "const bool launching_fortnite = screen.page == 8;")
begin = between("catalog_selected = game_source;", "live_presented = false;")
observe = between("if (live_status.video_present_calls)", "if (x4_auth_busy(auth))")
finish = between("if (local_session.active && (screen.page != 6", "#if X4_STREAM_PROFILE")
assert source.index("x4_live_media_snapshot(live, &live_status);") < source.index(observe)
assert source.count("x4_auth_start_session_request(") == source.count("x4_auth_start_session(") == 1
assert "action == X4_GAME_ACTION_PLAY) game_play = true;" in source
assert "game_page_open(&game_page_ui, 0, route.source," in source
assert "game_page_open(&game_page_ui, 5, source_index," in source
assert "game_page_follow(&game_page_ui, index," in source
assert "game_page_open(&game_page_ui, 8, 0," in source
assert "catalog_selected" not in history + observe + finish
for name, code in (
    ("HISTORY", history), ("SELECTED", selected), ("LAUNCH", launch), ("LAUNCH_GATE", launch_gate),
    ("BEGIN", begin), ("OBSERVE", observe), ("FINISH", finish),
):
    marker = f"/* @{name}@ */"
    assert template.count(marker) == 1
    template = template.replace(marker, code)

def run(binary, path, public):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
    result = subprocess.run([binary, path], env=env, text=True, capture_output=True)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="")
    result.check_returncode()
    omissions = [line for line in result.stdout.splitlines() if "historial omitido" in line]
    expected = [] if public else [
        "motivo=sin_imagen errno=0", "motivo=sin_producto errno=0",
        *(f"motivo=escritura errno={number}" for number in (errno.EACCES, errno.ENOSPC, errno.EIO, errno.EXDEV)),
    ]
    assert omissions == ["XCloud4: historial omitido " + line for line in expected], omissions

with tempfile.TemporaryDirectory(prefix="x4-history49-") as directory:
    temp = Path(directory)
    harness = temp / "main-history.c"
    harness.write_text(template, encoding="utf-8")
    for public, mode in ((0, "plain"), (0, "sanitized"), (1, "plain")):
        binary = temp / f"history-{public}-{mode}"
        fixture = temp / f"history-{public}-{mode}.bin"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if mode == "sanitized" else []
        headers = ["-I", str(root / "src"), "-I", str(root / "tests/host_stubs")]
        toolchain = os.environ.get("OO_PS4_TOOLCHAIN")
        if toolchain:
            headers += ["-idirafter", str(Path(toolchain) / "include")]
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", f"-DX4_PUBLIC={public}", "-O1", "-g",
            "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections", *headers,
            *shlex.split(os.environ.get("X4_TEST_CFLAGS", "")), *flags, str(harness),
            *(str(root / f"src/ui/{unit}.c") for unit in (
                "play_history", "product_metadata", "game_page", "font", "canvas", "lang",
            )),
            "-Wl,--gc-sections", *(f"-Wl,--wrap={name}" for name in ("fopen", "fwrite", "fclose", "rename", "remove")),
            "-lm", "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        print(f"Historial public={public} modo={mode}", flush=True)
        run(binary, fixture, public)
