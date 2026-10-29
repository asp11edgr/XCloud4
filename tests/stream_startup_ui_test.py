#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""C-preprocessed source regression checks, not a PS4/UI runtime test.

Native headers are deliberately removed before invoking the real host C
preprocessor: these checks project the production conditional branches, but do
not compile or execute main. Native object identity and console behavior need
their independent build/session gates. Optional authenticated baseline files
allow token-for-token profile-zero comparison.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASELINE = {
    "main.c": "2d13ad955c3c854aeb2ba4c292fde87fba081b96a40f4b24fcbee87d96f2a3f8",
    "screen.c": "7b47e916a3c2f190623624aa0c2d0f78fe42e14e75da592ab07d5dbb9e61dbb5",
}


def preprocess(source: str, profile: int) -> str:
    source = re.sub(r"(?m)^\s*#\s*include[^\n]*$", "", source)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-E", "-P", "-x", "c", "-DX4_VIDEO_PROBE=0",
        f"-DX4_STREAM_PROFILE={profile}", "-DX4_RUMBLE=0", "-",
    ]
    result = subprocess.run(command, input=source, text=True, capture_output=True, check=True)
    return result.stdout


def masked(source: str) -> str:
    # Preserve offsets while excluding braces in strings/characters/comments.
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*',
                  lambda match: " " * len(match[0]), source)


def block(source: str, pattern: str) -> str:
    clean = masked(source)
    search = re.search(pattern, clean)
    if search is None:
        raise AssertionError(f"missing production branch: {pattern}")
    start = clean.find("{", search.end())
    if start < 0 or clean[search.end():start].strip():
        raise AssertionError("production branch has no body")
    depth = 0
    for offset in range(start, len(source)):
        if clean[offset] == "{":
            depth += 1
        elif clean[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:offset]
    raise AssertionError("unbalanced production branch")


def compact(source: str) -> str:
    # Token matching ignores indentation without changing string contents.
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d+(?:\.\d+)?|\S'
    return "\0".join(re.findall(pattern, source))


class StartupUiSourceRegression(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = (ROOT / "src/core/main.c").read_text(encoding="utf-8")
        cls.screen = (ROOT / "src/ui/screen.c").read_text(encoding="utf-8")
        cls.projected = {
            p: (preprocess(cls.main, p), preprocess(cls.screen, p)) for p in (0, 1, 2)
        }

    def test_profile_zero_keeps_legacy_cancellation(self) -> None:
        main, screen = self.projected[0]
        self.assertNotIn("video_start_error", main)
        self.assertNotIn("No se pudo iniciar video", main)
        self.assertNotIn("NO SE PUDO INICIAR VIDEO. CODIGO", screen)
        failure = block(main, r"if\s*\(\s*live_status\.video_error\s*\)")
        self.assertRegex(failure, r"trace_inactive\s*\(\s*live\s*\)")
        self.assertRegex(failure, r"x4_auth_cancel\s*\(\s*auth\s*\)")

    def test_profile_zero_authenticated_baseline_tokens(self) -> None:
        folder = os.environ.get("X4_UI_BASELINE_DIR")
        if not folder:
            self.skipTest("set X4_UI_BASELINE_DIR for authenticated baseline source comparison")
        for index, name in enumerate(("main.c", "screen.c")):
            data = (Path(folder) / name).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), BASELINE[name])
            before = preprocess(data.decode("utf-8"), 0)
            self.assertEqual(compact(before), compact(self.projected[0][index]), name)

    def test_stream_startup_failure_preserves_auth_audio_owner(self) -> None:
        for profile in (1, 2):
            main, _ = self.projected[profile]
            failure = block(main, r"if\s*\(\s*live_status\.video_error\s*\)")
            self.assertIn("stream_halt_video_error", failure)
            stop = block(main, r"static\s+void\s+stream_halt_video_error\s*\([^)]*\)")
            self.assertRegex(stop, r"if\s*\(\s*\*handled\s*\)\s*return;")
            self.assertRegex(stop, r"\*handled\s*=\s*true")
            self.assertEqual(stop.count("x4_live_media_stream_halt_video("), 1)
            self.assertLess(stop.index("x4_live_media_stream_halt_video"), stop.index("x4_live_media_snapshot"))
            for forbidden in ("x4_auth_cancel", "trace_inactive", "x4_live_media_close",
                              "x4_live_audio_stop", "x4_live_media_request_stop"):
                self.assertNotIn(forbidden, failure)
                self.assertNotIn(forbidden, stop)
            self.assertRegex(main, r"if\s*\(\s*!live_error\s*\)\s*live_error\s*=\s*live_status\.video_error")

    def test_midstream_and_flip_error_use_video_only_stop(self) -> None:
        for profile in (1, 2):
            main, _ = self.projected[profile]
            failure = block(main, r"if\s*\(\s*live_status\.video_error\s*\)")
            self.assertNotIn("video_ready", failure)
            self.assertNotIn("video_frames", failure)
            for pattern in (r"if\s*\(\s*stream_rc\s*<\s*0\s*&&\s*!live_error\s*\)",):
                failure = block(main, pattern)
                self.assertIn("stream_halt_video_error", failure)
                self.assertNotIn("x4_auth_cancel", failure)

    def test_no_ui_reopen_without_detached_proof(self) -> None:
        for profile in (1, 2):
            main, _ = self.projected[profile]
            reopen = block(main, r"if\s*\(\s*stream_display_active\s*&&\s*live_status\.video_display_detached\s*\)")
            self.assertIn("x4_display_open", reopen)
            self.assertIn("stream_display_active=false", reopen)
            self.assertNotIn("x4_live_media_close", reopen)
            self.assertRegex(main, r"if\s*\(\s*!stop_rc\s*\)\s*live_status\.video_display_detached\s*=\s*true")
            stop = block(main, r"static\s+void\s+stream_halt_video_error\s*\([^)]*\)")
            retained = block(stop, r"if\s*\(\s*!status->video_display_detached\s*\)")
            self.assertIn("no se puede dibujar el aviso", retained)
            active = block(main, r"if\s*\(\s*stream_display_active\s*\)")
            self.assertIn("!live_error", active)
            self.assertIn("continue;", active)
            self.assertNotIn("x4_display_pixels", active)

    def test_error_unmasks_ui_and_session_navigation_stays_owned_by_main(self) -> None:
        for profile in (1, 2):
            main, screen = self.projected[profile]
            predicate = re.search(r"bool\s+game_input\s*=([^;]+);", main)
            self.assertIsNotNone(predicate)
            self.assertIn("!live_error", predicate[1])
            self.assertEqual(main.count("ui_pressed &= ~ORBIS_PAD_BUTTON_OPTIONS"), 1)
            self.assertRegex(main, r"if\s*\(game_input\)\s*ui_pressed\s*&=\s*~ORBIS_PAD_BUTTON_OPTIONS;")
            self.assertRegex(main, r"x4_screen_update\s*\(&screen,\s*ui_pressed,\s*controller.data.buttons\)")
            update = block(screen, r"void\s+x4_screen_update\s*\([^)]*\)")
            self.assertRegex(update,r"if\s*\(s->page\s*==\s*6\)\s*return;")
            # The published UI no longer closes from OPTIONS in screen.c.
            # Main owns Circle cancellation and waits for remote cleanup;
            # its Lab touch-menu exit action uses the same safe-close path.
            session = block(main,r"if\s*\(!closing\s*&&\s*screen.page\s*==\s*6\s*&&\s*previous_page\s*==\s*6\)")
            cancel = block(session,r"if\s*\(\(controller.pressed\s*&\s*ORBIS_PAD_BUTTON_CIRCLE\)\s*&&\s*!game_input\s*&&\s*!session_back\)")
            self.assertIn("x4_auth_cancel(auth)",cancel)
            self.assertIn("session_back = 1",cancel)
            self.assertRegex(session,r"if\s*\(session_back\s*&&\s*!x4_auth_busy\(auth\)\)")
            self.assertRegex(main,r"case\s+X4_TOUCH_MENU_ACTION_EXIT_APP\s*:\s*screen.exit_requested\s*=\s*1;")
            exit_path=block(main,r"if\s*\(screen.exit_requested\)")
            self.assertIn("x4_auth_cancel(auth)",exit_path)
            self.assertIn("closing = 1",exit_path)

    def test_localized_numeric_error_is_drawn_while_session_stays_busy(self) -> None:
        for profile in (1, 2):
            main, screen = self.projected[profile]
            self.assertRegex(main, r"if\s*\(\s*live_error\s*&&\s*\(\s*!x4_auth_busy\(auth\)\s*\|\|\s*video_start_error\s*\)\s*\)")
            self.assertIn("x4_stream_video_error_caption(shown.stage,sizeof(shown.stage),live_error)", main)
            status = block(screen, r"void\s+x4_live_status_draw\s*\([^)]*\)")
            self.assertIn("x4_stream_video_error_text", status)
            self.assertIn("m->video_rejected_width,m->video_rejected_height", status)
            self.assertIn('phase2_tr("Vídeo: %d · Audio: %s · 0x%08x","Video: %d · Audio: %s · 0x%08x")',status)
            self.assertRegex(status,r'\),\s*video_error,\s*m->audio_error')
            formatter=(ROOT/'src/ui/stream_video_status.h').read_text(encoding='utf8')
            for key in ('X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT','X4_TR_VIDEO_REJECTED_DIM_LAB_FMT',
                        'X4_TR_VIDEO_STOPPED_PUBLIC_FMT','X4_TR_VIDEO_STOPPED_LAB_FMT'):
                self.assertIn('x4_tr('+key+')',formatter)
            translations=(ROOT/'src/ui/lang.c').read_text(encoding='utf8')
            self.assertIn('[X4_TR_VIDEO_STOPPED_LAB_FMT] = "Vídeo detenido (código %d). Sal con OPTIONS."',translations)
            self.assertIn('[X4_TR_VIDEO_STOPPED_LAB_FMT] = "Video stopped (code %d). Exit with OPTIONS."',translations)
            self.assertRegex(main, r"x4_session_draw\s*\(&shown,\s*x4_auth_busy\(auth\),\s*closing,")
            self.assertGreaterEqual(len(re.findall(r"video_start_error\s*=\s*false", main)), 3)

    def test_media_audio_callback_is_independent_of_video_error(self) -> None:
        media = (ROOT / "src/media/live_media.c").read_text(encoding="utf-8")
        receive = block(media, r"void\s+x4_live_media_receive\s*\([^)]*\)")
        audio = block(receive, r"if\s*\(kind\s*==\s*X4_LIVE_KIND_AUDIO\)")
        self.assertIn("x4_live_audio_receive", audio)
        self.assertIn("return;", audio)
        self.assertNotIn("video_error", audio)
        self.assertNotIn("video.error", audio)
        pipeline = (ROOT / "src/video/stream_pipeline.c").read_text(encoding="utf-8")
        failure = block(pipeline, r"static\s+int\s+fail\s*\([^)]*\)")
        self.assertIn("&p->cancel", failure)
        self.assertNotIn("external_stop", failure)


if __name__ == "__main__":
    print("C-preprocessed source checks only: no PS4 rendering/audio/exit runtime claim", flush=True)
    unittest.main(verbosity=2)
