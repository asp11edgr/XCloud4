#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check Home publication wiring and compare real host pixels after late data."""

from hashlib import sha256
from pathlib import Path
import re
import subprocess
import sys

main = Path("src/core/main.c").read_text(encoding="utf-8")
request = re.compile(
    r"if \(\(screen\.page == 0 \|\| screen\.page == 5\)"
    r" && catalog\.state == X4_CATALOG_READY\) \{\s*"
    r"x4_catalog_assets_request_sigls\(catalog_assets, home_sigl_epoch\);\s*"
    r"if \(x4_catalog_assets_copy_sigls\(catalog_assets, home_sigls, &home_sigl_revision\)"
    r" > 0\)\s*home_rows_changed = true;"
)
assert request.search(main)
assert re.search(
    r"if \(home_rows_changed\)\s*x4_catalog_browser_home_rebuild"
    r"\(&catalog_browser, &catalog, &recent, home_sigls\);", main
)
idle_gate = main[main.index("#define UI_HASH(v)"):main.index("#undef UI_HASH")]
assert "UI_HASH(catalog_browser.home_revision);" in idle_gate
assert re.search(
    r"if \(screen\.page == 0\) \{\s*UI_HASH\(play_history\.revision\);"
    r".*?if \(screen\.signed_in\).*?UI_HASH\(\(unsigned char\)catalog\.gamertag\[i\]\);",
    idle_gate, re.S
)

languages = ("es", "en", "pt-BR", "fr", "it", "de")
frame_bytes = 1920 * 1080 * 4
with subprocess.Popen([sys.argv[1]], stdout=subprocess.PIPE) as run:
    assert run.stdout is not None
    for language in languages:
        warm = run.stdout.read(frame_bytes)
        cold = run.stdout.read(frame_bytes)
        assert len(warm) == len(cold) == frame_bytes
        assert sha256(warm).digest() == sha256(cold).digest()
        print(f"HOME50B {language} late_SIGL+gamertag=cold sha256={sha256(warm).hexdigest()}")
    assert not run.stdout.read(1)
    assert run.wait() == 0
