/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse native-ABI-only doubles; call the real Xbox workspace setter. */
#define _POSIX_C_SOURCE 200809L
/* This standalone runner does not link the locale owner separately. */
#include "../src/ui/lang.c"
#define main existing_http_fixture_main
#include "stream_os_profile_http_test.c"
#undef main

int main(void)
{
    X4StreamStartup first={0},replacement={0};
    X4KeyframeLimiter limiter,replacement_limiter;
    x4_keyframe_limiter_init(&limiter);x4_keyframe_limiter_init(&replacement_limiter);
    CHECK(!x4_xbox_work_set_stream_startup(NULL,&first));
    X4XboxWork *w=x4_xbox_work_new();CHECK(w);
    CHECK(!x4_xbox_work_set_keyframe_limiter(NULL,&limiter));
    CHECK(!x4_xbox_work_set_keyframe_limiter(w,NULL));
    CHECK(x4_xbox_work_set_keyframe_limiter(w,&limiter)&&w->keyframe_limiter==&limiter);
    CHECK(x4_xbox_work_set_stream_startup(w,&first));CHECK(w->stream_startup==&first);
    w->http=(X4Http *)(uintptr_t)1;
    CHECK(!x4_xbox_work_set_keyframe_limiter(w,&replacement_limiter)&&w->keyframe_limiter==&limiter);
    CHECK(!x4_xbox_work_set_stream_startup(w,&replacement));CHECK(w->stream_startup==&first);w->http=NULL;
    w->rtc=(X4Rtc *)(uintptr_t)1;
    CHECK(!x4_xbox_work_set_keyframe_limiter(w,&replacement_limiter)&&w->keyframe_limiter==&limiter);
    CHECK(!x4_xbox_work_set_stream_startup(w,&replacement));CHECK(w->stream_startup==&first);w->rtc=NULL;
    strcpy(w->session_id,"FIXTURE");
    CHECK(!x4_xbox_work_set_keyframe_limiter(w,&replacement_limiter)&&w->keyframe_limiter==&limiter);
    CHECK(!x4_xbox_work_set_stream_startup(w,&replacement));CHECK(w->stream_startup==&first);w->session_id[0]=0;
    CHECK(x4_xbox_work_set_stream_startup(w,NULL));CHECK(!w->stream_startup);
    CHECK(x4_xbox_work_set_stream_startup(w,&replacement));CHECK(w->stream_startup==&replacement);
    CHECK(x4_xbox_work_set_keyframe_limiter(w,&replacement_limiter)&&w->keyframe_limiter==&replacement_limiter);
    x4_xbox_work_free(w);
    CHECK(!next_resource&&!templates&&!terminated);
    printf("startup Xbox workspace bridge PASS: %u checks; bind only before HTTP/RTC/session and preserve busy binding; no network\n",checks);
    return 0;
}
