#!/usr/bin/env python3
"""Run unchanged access-card event/coordinator blocks extracted from main.c.

The fixture replaces only external UI/auth/native results. It verifies main's
ordering and ownership, not a PS4 controller or live Xbox response.
"""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/core/main.c").read_text(encoding="utf8")

def block(marker):
    start = source.index(marker)
    begin = source.index("{", start)
    depth = 1
    end = begin + 1
    while depth:
        if source[end] == "{": depth += 1
        elif source[end] == "}": depth -= 1
        end += 1
    return source[start:end]

event = block("if (!picture_settings_ui.open && !pic_consumed &&")
dispatch = block("if (pending_auth >= 0) {")
complete = block("if (access_query && access_query_started && !x4_auth_busy(auth) &&")
sync = block("if (picture_settings_ui.access_changed) {")
lab_gate = source[source.index("bool can_edit=!catalog_browser.editing"):source.index(";",source.index("bool can_edit=!catalog_browser.editing"))+1]
picture_gate = source[source.index("bool can_edit_pic ="):source.index(";",source.index("bool can_edit_pic ="))+1]
snapshot = "bool catalog_copied = x4_auth_catalog_snapshot_cached(auth, &catalog, &catalog_cursor) != 0;"
assert source.index(snapshot) < source.index(complete), "completion must follow the fresh catalog copy"
assert "x4_auth_access_country(auth)" in complete
assert "controller.pressed = 0;" in event and "ui_pressed = 0;" in event
program = (root / "tests/access_country_main_template.c").read_text(encoding="utf8")
for token, value in [("/* @ACCESS_EVENT@ */", event), ("/* @ACCESS_DISPATCH@ */", dispatch),
                     ("/* @ACCESS_COMPLETE@ */", snapshot + "\n(void)catalog_copied;\n" + complete),
                     ("/* @ACCESS_SYNC@ */", sync), ("/* @ACCESS_LAB_GATE@ */",lab_gate),
                     ("/* @ACCESS_PICTURE_GATE@ */",picture_gate)]:
    assert program.count(token) == 1
    program = program.replace(token, value)
with tempfile.TemporaryDirectory(prefix="x4-access-main-") as directory:
    path = Path(directory)
    test = path / "main-blocks.c"
    test.write_text(program, encoding="utf8")
    for mode in ("plain", "sanitized"):
        binary = path / mode
        sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if mode == "sanitized" else []
        command = shlex.split(os.environ.get("CC", "cc")) + ["-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Werror"]
        command += shlex.split(os.environ.get("X4_TEST_CFLAGS", "")) + sanitizers
        command += ["-I", str(root / "src"), str(test), str(root / "src/ui/lang.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(binary)], check=True, env=env)
print("PASS exact main access event/dispatch/completion/sync/gates source SHA256:", hashlib.sha256((event+dispatch+complete+sync+lab_gate+picture_gate).encode()).hexdigest())
