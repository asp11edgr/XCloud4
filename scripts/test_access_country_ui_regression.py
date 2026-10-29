#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run unchanged UI harnesses with the new production module dependency.

The original script and every C assertion stay on disk byte-for-byte. The
compiler source list gains access_country.c. For the one tests/ script using
BASH_SOURCE, $0 names the identical original path under bash -c. Its SHA and
adaptations are printed for the per-suite receipt. No expectation is rewritten.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SUITES = {
    "test_catalog_gui.sh": 1,
    "test_stream_settings.sh": 1,
    "test_stream_public_settings.sh": 1,
    "test_picture_settings.sh": 2,
    "test_server_settings.sh": 1,
    "test_fortnite_flow.sh": 2,
    "test_tarea19_ui.sh": 1,
    "test_tarea20_settings.sh": 1,
    "test_tarea20_screens.sh": 1,
    "test_tarea20_images.sh": 1,
    "test_tarea20b_menu.sh": 1,
    "test_tarea20b_loading.sh": 1,
    "test_tarea20c_rocket.sh": 1,
    "test_tarea20c_modals.sh": 1,
    "test_tarea20c_ui.sh": 1,
    "test_tarea20c_loading_queue.sh": 1,
    "test_tarea20c_queue_cards.sh": 1,
    "test_update_qr.sh": 1,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in SUITES:
        raise SystemExit("Expected an explicitly listed original UI test script basename")
    root = Path(__file__).resolve().parents[1]
    special = sys.argv[1] == "test_update_qr.sh"
    path = root / ("tests" if special else "scripts") / sys.argv[1]
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    token = '"${REPO_ROOT}/src/ui/apple_ui.c"' if special else "src/ui/apple_ui.c"
    count = text.count(token)
    if count != SUITES[path.name] or "src/ui/access_country.c" in text:
        raise SystemExit("Original harness source list differs; refusing unreviewed dependency injection")
    dependency = '"${REPO_ROOT}/src/ui/access_country.c" ' if special else "src/ui/access_country.c "
    compiled = text.replace(token, dependency + token)
    if special:
        if compiled.count("${BASH_SOURCE[0]}") != 1:
            raise SystemExit("Original BASH_SOURCE path routing differs")
        compiled = compiled.replace("${BASH_SOURCE[0]}", "$0")
    print(json.dumps({"original_script": path.name,
        "original_sha256": hashlib.sha256(raw).hexdigest(),
        "production_dependency_added": "src/ui/access_country.c",
        "source_list_insertions": count,
        "bash_source_uses_original_argv0": special,
        "test_sources_and_assertions_changed": False}), flush=True)
    result = subprocess.run(["bash", "-c", compiled, str(path), *sys.argv[2:]], cwd=root)
    if path.read_bytes() != raw:
        raise SystemExit("Original harness unexpectedly changed during execution")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
