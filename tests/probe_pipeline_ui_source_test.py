#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Probe controller wiring; C test covers deterministic selection and gates."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/core/probe_controller.c").read_text(encoding="utf-8")


def controller_branches(source):
    """Read the complete outer dispatch, not an arbitrary matching substring.

    This intentionally accepts only the reviewed two-flag T26 selector,
    legacy-probe elif, and final disabled stub. A weakened guard must fail.
    """
    match = re.search(
        r"(?ms)^#if X4_VIDEO_PROBE && X4_PROBE_T26\n(?P<t26>.*?)"
        r"^#elif X4_VIDEO_PROBE\n(?P<legacy>.*?)"
        r"^#else\n(?P<off>.*?)^#endif\s*\Z", source)
    if not match:
        raise ValueError("Missing exact T26/legacy/OFF controller guard chain")
    branches = {name: match[name].strip() for name in ("t26", "legacy", "off")}
    if not re.fullmatch(
        r'#include "../video/probe_t26_controller.h"\s+'
        r'int x4_probe_controller_run\(void\)\s*\{\s*return x4_t26_controller_run\(\);\s*\}',
        branches["t26"]):
        raise ValueError("T26 branch must contain only its include and delegated entry point")
    if branches["off"] != "int x4_probe_controller_run(void) { return -1; }":
        raise ValueError("Disabled controller entry point changed")
    if not branches["legacy"].startswith('#include "lifecycle.h"\n') or \
            "X4ProbePipelineChoice pipeline_choice = {0}" not in branches["legacy"] or \
            "x4_t26_controller_run" in branches["legacy"]:
        raise ValueError("Legacy probe body escaped or was replaced by T26")
    return branches


def selected_controller(source, probe, t26):
    branches = controller_branches(source)
    return branches["t26" if probe and t26 else "legacy" if probe else "off"]


class ProbePipelineUiTests(unittest.TestCase):
    def test_probe_off_entrypoint_unchanged(self):
        branches = controller_branches(SOURCE)
        self.assertEqual(branches["off"], "int x4_probe_controller_run(void) { return -1; }")
        for t26 in (0, 1):
            self.assertEqual(selected_controller(SOURCE, 0, t26), branches["off"])
        self.assertTrue(branches["legacy"].startswith('#include "lifecycle.h"\n'))
        self.assertIn("X4ProbePipelineChoice pipeline_choice = {0}", branches["legacy"])

    def test_t26_requires_both_flags_and_legacy_retains_its_body(self):
        branches = controller_branches(SOURCE)
        self.assertEqual(selected_controller(SOURCE, 1, 0), branches["legacy"])
        self.assertEqual(selected_controller(SOURCE, 1, 1), branches["t26"])
        self.assertNotIn("return x4_t26_controller_run();", branches["legacy"])
        self.assertNotIn("X4ProbePipelineChoice", branches["t26"])

    def test_dispatch_guard_or_off_stub_mutations_are_rejected(self):
        for old, new in (
            ("#if X4_VIDEO_PROBE && X4_PROBE_T26", "#if X4_VIDEO_PROBE || X4_PROBE_T26"),
            ("#if X4_VIDEO_PROBE && X4_PROBE_T26", "#if X4_PROBE_T26"),
            ("#elif X4_VIDEO_PROBE", "#elif X4_PROBE_T26"),
            ("int x4_probe_controller_run(void) { return -1; }",
             "int x4_probe_controller_run(void) { return x4_t26_controller_run(); }"),
        ):
            with self.subTest(mutation=new), self.assertRaises(ValueError):
                controller_branches(SOURCE.replace(old, new, 1))

    def test_choice_change_cannot_launch_same_frame(self):
        start = SOURCE.index("if (!files_view && !memory_view && !retained && !exit_requested && (pad.pressed & ORBIS_PAD_BUTTON_L2))")
        end = SOURCE.index("if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))")
        body = SOURCE[start:end]
        self.assertIn("moved = true", body)
        self.assertIn("x4_probe_pipeline_choice_change", body)

    def test_runtime_requires_files_then_own_compatibility(self):
        body = SOURCE[SOURCE.index("if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS))"):]
        self.assertLess(body.index("if (!files_checked)"), body.index("x4_probe_pipeline_run("))
        self.assertLess(body.index("x4_probe_pipeline_choice_allowed("), body.index("x4_probe_pipeline_run("))
        self.assertLess(body.index("x4_display_close(&display)"), body.index("x4_probe_pipeline_run("))
        self.assertIn("x4_probe_pipeline_converters(run_choice), &pad", body)
        self.assertIn("result.compatibility_passed && !result.report_result", body)
        self.assertIn("continue; /* Never launch a second route on the same X edge. */", body)

    def test_retained_runtime_never_reopens_or_releases(self):
        start = SOURCE.index("if (result.retained)")
        end = SOURCE.index("open_display_or_wait(&display)", start)
        body = SOURCE[start:end]
        self.assertIn("for (;;) sceKernelUsleep(100000)", body)
        self.assertNotIn("x4_exit_request", body)
        self.assertNotIn("x4_display_close", body)
        self.assertNotIn("free(", body)

    def test_legacy_memory_choice_is_not_experimental_memory(self):
        self.assertIn("if (pipeline_choice.selected != X4_PROBE_PIPELINE_CURRENT)", SOURCE)
        self.assertIn("x4_live_media_probe_create_memory(active, active_mode, metrics,", SOURCE)
        self.assertIn("x4_probe_memory_choice_type(active_memory_choice)", SOURCE)
        self.assertIn("Salida WB_ONION (0)", SOURCE)

    def test_new_failure_does_not_stop_current(self):
        start = SOURCE.index("if (result.stopped)")
        end = SOURCE.index("continue; /* Never launch a second route", start)
        body = SOURCE[start:end]
        self.assertIn("x4_probe_pipeline_choice_failed(&pipeline_choice, run_choice)", body)
        self.assertNotIn("session_stopped = true", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
