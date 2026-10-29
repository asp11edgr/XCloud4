#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Ownership/order regression checks over real source, never a runtime claim."""
from pathlib import Path
import re
import unittest
from stream_startup_ui_test import block

ROOT=Path(__file__).resolve().parents[1]


class StartupBridgeSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main=(ROOT/'src/core/main.c').read_text(encoding='utf-8')
        cls.auth=(ROOT/'src/auth/device_auth.c').read_text(encoding='utf-8')
        cls.xbox=(ROOT/'src/auth/xbox_live.c').read_text(encoding='utf-8')
        cls.screen=(ROOT/'src/ui/screen.c').read_text(encoding='utf-8')

    def test_main_binds_same_media_before_session_start(self):
        source=self.main
        bind='x4_auth_set_stream_startup(auth,x4_live_media_stream_startup(live))'
        create='x4_live_media_create_request(&live_error,&request_cfg)'
        callback='x4_auth_set_media_callback(auth, x4_live_media_receive, live, x4_live_media_trace(live))'
        start='x4_auth_start_session_request(auth,catalog_selected,&request_cfg)'
        self.assertLess(source.index(create),source.index(callback))
        self.assertLess(source.index(callback),source.index(bind))
        self.assertLess(source.index(bind),source.index(start))
        self.assertIn('if (!rc) rc = '+bind+';',source)

    def test_auth_bind_precedes_worker_and_sdp(self):
        start=block(self.auth,r'int\s+x4_auth_start_session_request\s*\([^)]*\)')
        self.assertLess(start.index('x4_auth_busy(a)'),start.index('join_worker(a)'))
        self.assertLess(start.index('join_worker(a)'),start.index('a->session_request ='))
        self.assertLess(start.index('x4_xbox_work_set_stream_startup(a->xwork, a->stream_startup)'),
                        start.index('scePthreadCreate('))
        stream=block(self.xbox,r'static\s+void\s+session_stream\s*\([^)]*\)')
        self.assertLess(stream.index('x4_rtc_set_stream_startup(w->rtc,w->stream_startup)'),stream.index('signal_sdp('))
        self.assertRegex(stream,r'if\(rc\)\{end_with\(end,X4_SESSION_ERROR,rc,0,"no se pudo preparar la petición de resolución"\);goto done;\}')

    def test_unregister_clears_pointer_only_after_successful_join(self):
        unregister=block(self.auth,r'int\s+x4_auth_set_media_callback\s*\([^)]*\)')
        self.assertLess(unregister.index('x4_auth_busy(a)'),unregister.index('join_worker(a)'))
        self.assertLess(unregister.index('if (rc) return rc;'),unregister.index('a->media_callback = callback;'))
        self.assertLess(unregister.index('a->media_callback = callback;'),unregister.index('if(!callback) a->stream_startup=NULL;'))
        setter=block(self.auth,r'int\s+x4_auth_set_stream_startup\s*\([^)]*\)')
        self.assertLess(setter.index('x4_auth_busy(a)'),setter.index('join_worker(a)'))
        self.assertLess(setter.index('if(rc) return rc;'),setter.index('a->stream_startup=startup;'))

    def test_readiness_does_not_consume_request_before_alias(self):
        stream=block(self.xbox,r'static\s+void\s+session_stream\s*\([^)]*\)')
        begin=stream.index('/* All producers share one limiter.')
        dispatch=stream[begin:stream.index('#else',begin)]
        self.assertLess(dispatch.index('x4_rtc_keyframe_startup_ready(w->rtc)'),dispatch.index('atomic_exchange('))
        self.assertLess(dispatch.index('x4_rtc_keyframe_startup_ready(w->rtc)'),dispatch.index('x4_keyframe_limiter_take('))
        self.assertLess(dispatch.index('x4_keyframe_limiter_take('),dispatch.index('x4_rtc_request_keyframe('))
        self.assertIn('x4_keyframe_limiter_request(w->keyframe_limiter,X4_KEYFRAME_LEGACY)',dispatch)
        legacy=stream[stream.index('if (connected_at && t - last_keyframe'):stream.index('if (!signal_keepalive')]
        self.assertRegex(legacy,r'&&\s*x4_rtc_keyframe_startup_ready\(w->rtc\)')
        self.assertIn('bool requested = w->keyframe_requested && atomic_exchange(w->keyframe_requested, 0);',legacy)

    def test_shared_dispatch_preserves_existing_recovery_deadlines(self):
        stream=block(self.xbox,r'static\s+void\s+session_stream\s*\([^)]*\)')
        begin=stream.index('/* All producers share one limiter.')
        dispatch=stream[begin:stream.index('#else',begin)]
        self.assertNotIn('X4_STREAM_INITIAL_RESOLUTION_RETRY_US',dispatch)
        self.assertIn('t-last_no_media>=X4_KEYFRAME_INTERVAL_US',dispatch)
        limiter=(ROOT/'src/streaming/keyframe_limiter.h').read_text(encoding='utf-8')
        self.assertIn('X4_KEYFRAME_INTERVAL_US = 500000',limiter)
        self.assertIn('X4_KEYFRAME_WINDOW_LIMIT = 10',limiter)
        self.assertIn('uint64_t keyframe_interval=500000;',stream) # Legacy remains available.
        header=(ROOT/'src/streaming/stream_startup.h').read_text(encoding='utf-8')
        self.assertIn('X4_STREAM_INITIAL_RESOLUTION_RETRY_US=300000',header)
        # The published baseline allows 30 s for the requested resolution;
        # this does not change the separate five-second BAD_FRAME recovery.
        self.assertIn('X4_STREAM_INITIAL_RESOLUTION_WAIT_US=30000000',header)
        recovery=(ROOT/'src/video/stream_idr_recovery.h').read_text(encoding='utf-8')
        self.assertIn('X4_STREAM_IDR_RETRY_US = 300000',recovery)
        self.assertIn('X4_STREAM_IDR_TIMEOUT_US = 5000000',recovery)

    def test_waiting_message_drawn_even_when_packet_state_is_streaming(self):
        expected='Esperando 1080p de Xbox…'
        self.assertEqual(self.main.count(expected),1)
        waiting=self.main[self.main.index('if(!live_error && live && !closing && !session_back &&'):]
        waiting=waiting[:waiting.index('#endif')]
        self.assertIn('x4_auth_busy(auth) && x4_live_media_waiting_initial_resolution(live)',waiting)
        self.assertRegex(waiting,r'snprintf\s*\(shown\.stage\s*,\s*sizeof\(shown\.stage\)\s*,\s*"'+re.escape(expected)+r'"\s*\)')
        self.assertRegex(waiting,r'if\s*\(wait_sec\s*>=\s*5\)')
        self.assertIn('Xbox está preparando el juego… %u s',waiting)
        self.assertIn('x4_stream_startup_alias_sent_us',waiting)
        self.assertNotIn('shown.state',waiting)
        self.assertNotIn('shown.error',waiting)
        self.assertNotIn('X4_SESSION_ERROR',waiting)
        draw=block(self.screen,r'void\s+x4_session_draw\s*\([^)]*\)')
        # The current renderer localizes the stage for every non-queue state,
        # including STREAMING; it no longer has the old state switch/layout.
        self.assertRegex(draw,r'else\s+x4_apple_text_wrap\([^;]+x4_tr_stage\(s->stage\)')
        self.assertNotIn('X4_SESSION_STREAMING',draw)
        self.assertLess(draw.index('s->waiting_for_resources'),draw.index('x4_tr_stage(s->stage)'))

    def test_error_uses_rejected_dimensions_in_status(self):
        draw=block(self.screen,r'void\s+x4_live_status_draw\s*\([^)]*\)')
        self.assertIn('m->video_rejected_width,m->video_rejected_height,video_error',draw)
        self.assertIn('x4_stream_video_error_text',draw)
        formatter=(ROOT/'src/ui/stream_video_status.h').read_text(encoding='utf-8')
        self.assertIn('x4_tr(X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT), width, height',formatter)
        self.assertIn('x4_tr(X4_TR_VIDEO_REJECTED_DIM_LAB_FMT), width, height',formatter)
        translations=(ROOT/'src/ui/lang.c').read_text(encoding='utf-8')
        self.assertIn('[X4_TR_VIDEO_REJECTED_DIM_LAB_FMT] = "Xbox envió %u×%u; esta PS4 solo admite hasta 1080p. Sal con OPTIONS."',translations)
        self.assertIn('[X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT] = "Xbox envió %u×%u; esta PS4 solo admite hasta 1080p. Cierra con botón PS."',translations)
        self.assertIn('[X4_TR_VIDEO_REJECTED_DIM_LAB_FMT] = "Xbox sent %u×%u; this PS4 supports up to 1080p. Exit with OPTIONS."',translations)


if __name__=='__main__':
    print('Source ownership/order checks only: no host C or console runtime claim',flush=True)
    unittest.main(verbosity=2)
