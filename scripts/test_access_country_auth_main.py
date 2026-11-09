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
fortnite_schedule_start = source.index("if (screen.page == 8 && account.state == X4_AUTH_AUTHORIZED &&")
fortnite_schedule = source[fortnite_schedule_start:source.index(dispatch)]
fortnite_enter = block("if (previous_page == 0 && screen.page == 8) {")
fortnite_home = "case X4_UI2_HOME_FORTNITE: screen.page = 8; break;"
assert source.count(fortnite_home) == 1
fortnite_back = block("if (previous_page == 8 && !game_page_ui.open &&")
fortnite_ready = block("if (!closing && screen.page == 8 && !picture_settings_ui.open &&")
game_model = block("static X4GamePageModel game_page_model(")
launch = block("if (launch_game_page &&")
assert "is_fortnite_session = launching_fortnite" in launch
assert "screen.page == 8" in launch
assert "x4_auth_start_session_request(auth,catalog_selected,&request_cfg)" in launch
assert "x4_auth_start_session(auth, catalog_selected)" in launch
assert source.count("x4_auth_start_session_request(") == source.count("x4_auth_start_session(") == 1
assert "action == X4_GAME_ACTION_PLAY) game_play = true;" in source
assert "game_page_ui.open && (game_play || pending_launch)" in source
assert "x4_picture_settings_set(" not in source
complete = block("if (access_query && access_query_started && !x4_auth_busy(auth) &&")
sync = block("if (picture_settings_ui.access_changed) {")
lab_gate = source[source.index("bool can_edit="):source.index(";",source.index("bool can_edit="))+1]
picture_gate = source[source.index("bool can_edit_pic ="):source.index(";",source.index("bool can_edit_pic ="))+1]
snapshot = "bool catalog_copied = x4_auth_catalog_snapshot_cached(auth, &catalog, &catalog_cursor) != 0;"
assert source.index(snapshot) < source.index(complete), "completion must follow the fresh catalog copy"
fortnite_complete = source[source.index(snapshot) + len(snapshot):source.index("/* Evaluate completion")]
assert "x4_ui2_fortnite_complete(&fortnite, &catalog, x4_auth_busy(auth))" in fortnite_complete
assert "x4_auth_access_country(auth)" in complete
assert "controller.pressed = 0;" in event and "ui_pressed = 0;" in event
program = (root / "tests/access_country_main_template.c").read_text(encoding="utf8")
for token, value in [("/* @ACCESS_EVENT@ */", event), ("/* @ACCESS_DISPATCH@ */", dispatch),
                     ("/* @ACCESS_COMPLETE@ */", snapshot + "\n(void)catalog_copied;\n" + fortnite_complete + complete),
                     ("/* @FORTNITE_SCHEDULE@ */", fortnite_schedule), ("/* @FORTNITE_GAME_MODEL@ */", game_model),
                     ("/* @FORTNITE_HOME@ */", fortnite_home), ("/* @FORTNITE_ENTER@ */", fortnite_enter),
                     ("/* @FORTNITE_BACK@ */", fortnite_back), ("/* @FORTNITE_READY@ */", fortnite_ready),
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
        command += ["-DX4_PUBLIC=0", "-ffunction-sections", "-fdata-sections",
                    "-I", str(root / "src"), "-I", str(root / "tests/host_stubs")]
        toolchain = os.environ.get("OO_PS4_TOOLCHAIN")
        if toolchain:
            command += ["-idirafter", str(Path(toolchain) / "include")]
        command += [str(test), *(str(root / unit) for unit in (
            "src/ui/lang.c", "src/ui/ui2_fortnite.c", "src/ui/ui2_home.c", "src/ui/game_page.c",
            "src/ui/font.c", "src/ui/canvas.c", "src/ui/product_metadata.c")), "-Wl,--gc-sections", "-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(binary)], check=True, env=env)
print("PASS exact main access event/dispatch/completion/sync/gates source SHA256:", hashlib.sha256((event+dispatch+complete+sync+lab_gate+picture_gate+fortnite_ready+launch).encode()).hexdigest())
