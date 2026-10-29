#!/usr/bin/env python3
"""Execute the exact update-notice gates/input blocks from main.c, no network."""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/"src/core/main.c").read_text(encoding="utf8")
ui=(root/"src/ui/apple_ui.c").read_text(encoding="utf8")

def block(marker):
    start=source.index(marker);begin=source.index("{",start);depth=1;end=begin+1
    while depth:
        if source[end]=="{":depth+=1
        elif source[end]=="}":depth-=1
        end+=1
    return source[start:end],end

consume,_=block("if (update_notice.open) {")
offer=source[source.index("bool update_home_idle ="):source.index("bool ui_reuse_work=false;")]
lab,end=block("if (stream_settings_ui.open && can_edit &&")
lab+=source[end:source.index(";",end)+1]
assert source.index(consume)<source.index("uint32_t ui_pressed = controller.pressed;")<source.index("x4_screen_update(&screen")
assert source.rfind("#if X4_STREAM_PROFILE && !X4_PUBLIC",0,source.index(lab))>source.rfind("#elif",0,source.index(lab))
assert source.index(offer)>source.index("x4_screen_update(&screen")
assert "if (!ui_reuse_work && screen.page == 0 && update_notice.open)" in source
pure_ui=ui[ui.index("static bool update_tag_valid("):ui.index("static void update_notice_qr(")]
signature="\n".join(line.strip() for line in source.splitlines() if line.strip().startswith("UI_HASH(update_notice."))
assert signature == "UI_HASH(update_notice.open);\nUI_HASH(update_notice.shown);"
program=(root/"tests/access_country_update_main_template.c").read_text(encoding="utf8")
for token,value in [("/* @UPDATE_PURE_UI@ */",pure_ui),("/* @UPDATE_CONSUME@ */",consume),
                    ("/* @UPDATE_OFFER@ */",offer),("/* @UPDATE_LAB@ */",lab),
                    ("/* @UPDATE_SIGNATURE@ */",signature)]:
    assert program.count(token)==1
    program=program.replace(token,value)
with tempfile.TemporaryDirectory(prefix="x4-update-main-") as directory:
    path=Path(directory);test=path/"update-blocks.c";test.write_text(program,encoding="utf8")
    for public in (0,1):
        for mode in ("plain","sanitized"):
            binary=path/f"update-{public}-{mode}"
            flags=["-fsanitize=address,undefined","-fno-omit-frame-pointer","-no-pie"] if mode=="sanitized" else []
            command=shlex.split(os.environ.get("CC","cc"))+["-std=gnu11","-O2","-g","-Wall","-Wextra","-Werror"]
            command+=shlex.split(os.environ.get("X4_TEST_CFLAGS",""))+flags+[f"-DX4_PUBLIC={public}","-DX4_STREAM_PROFILE=1","-I",str(root/"src"),str(test),"-o",str(binary)]
            subprocess.run(command,check=True)
            subprocess.run([str(binary)],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",UBSAN_OPTIONS="halt_on_error=1"))
print("PASS exact update main/pure-UI/signature SHA256:",hashlib.sha256((consume+offer+lab+pure_ui+signature).encode()).hexdigest())
