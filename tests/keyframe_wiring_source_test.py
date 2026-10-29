#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Source ownership/order contracts; native behavior is tested separately."""
from pathlib import Path
import unittest
from stream_startup_ui_test import block

ROOT = Path(__file__).resolve().parents[1]


class KeyframeWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.auth = (ROOT/'src/auth/device_auth.c').read_text(encoding='utf-8')
        cls.xbox = (ROOT/'src/auth/xbox_live.c').read_text(encoding='utf-8')
        cls.rtc = (ROOT/'src/streaming/rtc_transport.c').read_text(encoding='utf-8')

    def test_auth_registration_preserves_join_lifetime(self):
        setter = block(self.auth, r'int\s+x4_auth_set_keyframe_limiter\s*\([^)]*\)')
        self.assertLess(setter.index('x4_auth_busy(a)'), setter.index('join_worker(a)'))
        self.assertLess(setter.index('if(rc)return rc;'), setter.index('a->keyframe_limiter=limiter;'))
        unregister = block(self.auth, r'int\s+x4_auth_set_media_callback\s*\([^)]*\)')
        self.assertLess(unregister.index('if (rc) return rc;'), unregister.index('a->keyframe_limiter=NULL;'))

    def test_same_pointer_precedes_worker_http_and_sdp(self):
        start = block(self.auth, r'int\s+x4_auth_start_session_request\s*\([^)]*\)')
        self.assertLess(start.index('if(!a->keyframe_limiter)'), start.index('new_xbox_work(a)'))
        self.assertLess(start.index('new_xbox_work(a)'), start.index('x4_xbox_work_set_keyframe_limiter(a->xwork, a->keyframe_limiter)'))
        self.assertLess(start.index('x4_xbox_work_set_keyframe_limiter(a->xwork, a->keyframe_limiter)'), start.index('scePthreadCreate('))
        session = block(self.xbox, r'const\s+X4SessionSnapshot\s*\*x4_xbox_session\s*\([^)]*\)')
        self.assertLess(session.index('if(!w->keyframe_limiter)'), session.index('x4_http_open('))
        stream = block(self.xbox, r'static\s+void\s+session_stream\s*\([^)]*\)')
        self.assertLess(stream.index('x4_rtc_set_keyframe_limiter(w->rtc,w->keyframe_limiter)'), stream.index('signal_sdp('))

    def test_workspace_factory_captures_before_any_worker(self):
        factory = block(self.auth, r'static\s+X4XboxWork\s*\*new_xbox_work\s*\([^)]*\)')
        self.assertLess(factory.index('x4_access_country_get()'), factory.index('x4_xbox_work_new()'))
        self.assertLess(factory.index('x4_xbox_work_new()'), factory.index('x4_xbox_work_set_access_country(w, country)'))
        self.assertLess(factory.index('x4_xbox_work_set_access_country(w, country)'), factory.index('atomic_store_explicit(&a->access_country'))
        self.assertIn('x4_xbox_work_free(w);', factory)
        self.assertIn('return NULL;', factory)
        self.assertIn('return w;', factory)
        self.assertNotIn('scePthreadCreate(', factory)
        self.assertNotIn('x4_http_open(', factory)

    def test_new_route_reserves_only_after_startup_gate(self):
        stream = block(self.xbox, r'static\s+void\s+session_stream\s*\([^)]*\)')
        begin = stream.index('/* All producers share one limiter.')
        new = stream[begin:stream.index('#else', begin)]
        self.assertLess(new.index('x4_rtc_keyframe_startup_ready(w->rtc)'), new.index('atomic_exchange('))
        self.assertLess(new.index('x4_rtc_keyframe_startup_ready(w->rtc)'), new.index('x4_keyframe_limiter_take('))
        self.assertLess(new.index('x4_keyframe_limiter_take('), new.index('x4_rtc_request_keyframe('))
        self.assertIn('t-last_no_media>=X4_KEYFRAME_INTERVAL_US', new)
        self.assertNotIn('X4_STREAM_INITIAL_RESOLUTION_RETRY_US', new)
        self.assertNotIn('last_keyframe', new)

    def test_rtc_grant_consumed_before_unchanged_transport(self):
        request = block(self.rtc, r'int\s+x4_rtc_request_keyframe\s*\([^)]*\)')
        self.assertLess(request.index('x4_rtc_keyframe_startup_ready(rtc)'), request.index('state.granted<=rtc->keyframe_grant_consumed'))
        self.assertLess(request.index('rtc->keyframe_grant_consumed=state.granted;'), request.index('send_text('))
        self.assertIn('if(rtcIsOpen(rtc->channels[1]))', request)
        self.assertNotIn('rtcIsOpen(rtc->video)', request)
        self.assertIn(r'{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}', request)
        self.assertLess(request.index('rtcRequestKeyframe(rtc->video)'), request.index('x4_keyframe_limiter_note_transport('))
        self.assertIn('return rc;', request)


if __name__ == '__main__':
    print('Source contracts only; real C dispatch has a separate native-boundary fixture.', flush=True)
    unittest.main(verbosity=2)
