# SPDX-License-Identifier: GPL-3.0-only
"""All R3 interaction combinations through real UI/config and destination code.

The generated fixture executes the exact destination statements from main,
media and RTC. Native endpoints are doubles; no main loop, video or audio
worker is run. Existing focused tests cover the send/audio/rumble policies.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
main = (root / "src/core/main.c").read_text(encoding="utf8")
media = (root / "src/media/live_media.c").read_text(encoding="utf8")
rtc = (root / "src/streaming/rtc_transport.c").read_text(encoding="utf8")
profile = (root / "src/streaming/stream_profile.c").read_text(encoding="utf8")


def function(source, name):
    # Some production definitions put the legacy signature between #else
    # and #endif before their common body. Stop at a declaration/call's ';'.
    match = None
    for candidate in re.finditer(r"(?m)^[^\n;{}]*\b" + re.escape(name) + r"\s*\(", source):
        begin = source.find("{", candidate.end())
        semicolon = source.find(";", candidate.end())
        if begin >= 0 and (semicolon < 0 or begin < semicolon):
            match = candidate
            break
    assert match, name
    depth = 1
    end = begin + 1
    while depth:
        assert end < len(source), name
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def statement(source, expected):
    assert source.count(expected) == 1, expected
    return expected


# Bind every fragment to its real owner, rather than finding a dead copy.
begin_main = function(main, "interaction_main_begin")
main_capture = statement(main, "X4StreamRequestConfig request_cfg=x4_stream_settings_get();")
main_begin = statement(main, "interaction_main_begin(request_cfg.interaction_modes,sceKernelGetProcessTime());")
rumble_select = statement(main, "if(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)desired.lgMotor=desired.smMotor=0;")
assert main.index(main_capture) < main.index(main_begin)
assert "x4_live_media_create_request(&live_error,&request_cfg)" in main
assert "x4_auth_start_session_request(auth,catalog_selected,&request_cfg)" in main
media_create = function(media, "x4_live_media_create_request")
media_capture = statement(media_create, "X4StreamRequestConfig selected = x4_stream_request_capture(request);")
audio_select = statement(media_create, "m->stream_audio_previous=(selected.interaction_modes&X4_INTERACTION_AUDIO_PREVIOUS)!=0;")
audio_start = statement(media, "m->audio = x4_live_audio_start_mode(&m->audio_error, m->audio_payload_type, m->stream_audio_previous);")
assert media_create.index(media_capture) < media_create.index(audio_select)
rtc_capture = statement(function(rtc, "x4_rtc_open_request"), "rtc->request_config=x4_stream_request_capture(request);")
sender = function(rtc, "input_sender")
input_select = statement(sender, "legacy=(rtc->request_config.interaction_modes&X4_INTERACTION_INPUT_60HZ)!=0;")
assert "if(!legacy)" in sender and "} else while(!atomic_load(&rtc->input_stop))" in sender
capture = function(profile, "x4_stream_request_capture")
assert "c.interaction_modes=(unsigned char)(request->interaction_modes&X4_INTERACTION_MASK);" in capture

fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "src/ui/stream_settings.h"
#include "src/ui/ui_perf.h"
/* f18 already carries the picture/request fields after the interaction byte. */
_Static_assert(sizeof(X4StreamRequestConfig)==72&&offsetof(X4StreamRequestConfig,os_name)==24&&
    offsetof(X4StreamRequestConfig,display_info)==48&&offsetof(X4StreamRequestConfig,cpu_variant)==49&&
    offsetof(X4StreamRequestConfig,decoder_depth)==50&&offsetof(X4StreamRequestConfig,interaction_modes)==51&&
    offsetof(X4StreamRequestConfig,convert_mode)==52&&offsetof(X4StreamRequestConfig,sharpness)==53&&
    offsetof(X4StreamRequestConfig,resolution_request_mode)==54&&offsetof(X4StreamRequestConfig,h264_profile)==56&&
    offsetof(X4StreamRequestConfig,max_fs)==60&&offsetof(X4StreamRequestConfig,max_mbps)==64,
    "current base request ABI and exact interaction/picture offsets");
static struct { bool available; unsigned modes; uint64_t begin_us; } interaction_main;
/* The extracted begin helper also resets menu measurements. Their real type
 * is used here; the menu owner itself is not run by this capture-only test. */
static X4UiPerf menu_copy_times,menu_prepare_times;
static uint64_t menu_log_at,menu_interval_frames,menu_interval_open_frames;
static unsigned menu_cache_resets;
void x4_touch_menu_cache_reset(void){++menu_cache_resets;}
static uint64_t sceKernelGetProcessTime(void) { return 123456; }
@BEGIN_MAIN@
typedef struct { unsigned char lgMotor,smMotor; } TestVibe;
typedef struct { int sentinel; } X4LiveAudio;
typedef struct { bool stream_audio_previous; X4LiveAudio *audio; int audio_error,audio_payload_type; } TestMedia;
typedef struct { X4StreamRequestConfig request_config; } TestRtc;
static X4LiveAudio audio_double;
static bool audio_previous,input_previous;
static TestVibe received_vibration;
static X4LiveAudio *x4_live_audio_start_mode(int *error,int payload_type,bool previous)
{ assert(payload_type==111);*error=0;audio_previous=previous;return &audio_double; }
static void input_native_sink(bool previous) { input_previous=previous; }
static void rumble_native_sink(TestVibe vibration) { received_vibration=vibration; }
static TestMedia media_capture_request(const X4StreamRequestConfig *request)
{
    TestMedia value={.audio_payload_type=111};TestMedia *m=&value;
    @MEDIA_CAPTURE@
    @AUDIO_SELECT@
    return value;
}
static TestRtc rtc_capture_request(const X4StreamRequestConfig *request)
{
    TestRtc value={0};TestRtc *rtc=&value;
    @RTC_CAPTURE@
    return value;
}
static void audio_destination(TestMedia *m) { @AUDIO_START@ }
static void input_destination(TestRtc *rtc)
{ bool legacy=true;@INPUT_SELECT@ input_native_sink(legacy); }
static void rumble_destination(void)
{ TestVibe desired={91,37};@RUMBLE_SELECT@ rumble_native_sink(desired); }
static void edit_modes(X4StreamSettingsUi *ui,unsigned modes)
{
    assert(x4_stream_settings_ui_handle(ui,X4_SETTINGS_OPEN,true));
    assert(ui->open&&ui->row==0&&ui->interaction_modes==0);
    for(unsigned row=1;row<=7;++row) {
        assert(x4_stream_settings_ui_handle(ui,X4_SETTINGS_DOWN,true)&&ui->row==row);
        unsigned bit=row==4?X4_INTERACTION_INPUT_60HZ:row==5?X4_INTERACTION_RUMBLE_OFF:
            row==6?X4_INTERACTION_AUDIO_PREVIOUS:row==7?X4_INTERACTION_VSYNC_OFF:0;
        if(modes&bit)assert(x4_stream_settings_ui_handle(ui,X4_SETTINGS_RIGHT,true));
        assert(x4_stream_settings_get().interaction_modes==0);
    }
    assert(ui->interaction_modes==modes);
}
int main(void)
{
    for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes) {
        assert(x4_stream_settings_set_extended(X4_REQUEST_1080P_G8,X4_COLOR_AUTO_709,
            X4_CPU_0,X4_DEPTH_AUTO,0));
        X4StreamSettingsUi ui={0};
        edit_modes(&ui,modes);
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL|X4_SETTINGS_CONFIRM,true));
        assert(!ui.open&&x4_stream_settings_get().interaction_modes==0);
        edit_modes(&ui,modes);
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
        assert(!ui.open&&ui.applied&&x4_stream_settings_get().interaction_modes==modes);
        @MAIN_CAPTURE@
        menu_copy_times.n=7;menu_prepare_times.n=8;
        menu_log_at=menu_interval_frames=menu_interval_open_frames=9;
        unsigned previous_resets=menu_cache_resets;
        @MAIN_BEGIN@
        assert(!menu_copy_times.n&&!menu_prepare_times.n&&!menu_log_at&&
            !menu_interval_frames&&!menu_interval_open_frames&&menu_cache_resets==previous_resets+1);
        TestMedia media=media_capture_request(&request_cfg);
        TestRtc rtc=rtc_capture_request(&request_cfg);
        /* A later menu commit changes the next session only. */
        assert(x4_stream_settings_set_extended(X4_REQUEST_720P_HQ,X4_COLOR_FORCE_601,
            X4_CPU_3,X4_DEPTH_1,modes^X4_INTERACTION_MASK));
        assert(request_cfg.profile==X4_REQUEST_1080P_G8&&request_cfg.cpu_variant==X4_CPU_0&&
            request_cfg.decoder_depth==X4_DEPTH_AUTO&&request_cfg.interaction_modes==modes);
        assert(rtc.request_config.profile==X4_REQUEST_1080P_G8&&rtc.request_config.interaction_modes==modes);
        assert(interaction_main.available&&interaction_main.begin_us==123456&&interaction_main.modes==modes);
        audio_destination(&media);input_destination(&rtc);rumble_destination();
        assert(media.audio==&audio_double&&!media.audio_error);
        assert(audio_previous==!!(modes&X4_INTERACTION_AUDIO_PREVIOUS));
        assert(input_previous==!!(modes&X4_INTERACTION_INPUT_60HZ));
        assert(received_vibration.lgMotor==(modes&X4_INTERACTION_RUMBLE_OFF?0:91)&&
            received_vibration.smMotor==(modes&X4_INTERACTION_RUMBLE_OFF?0:37));
        printf("PASS R3 combination %u: captured destinations and cancel unchanged\n",modes);
    }
    puts("PASS interaction capture: 16 real R3/config combinations, exact destination fragments, menu metric reset, immutable session, no video/main loop/native worker");
    return 0;
}
'''
for name, value in {"BEGIN_MAIN": begin_main, "MAIN_CAPTURE": main_capture,
                    "MAIN_BEGIN": main_begin, "MEDIA_CAPTURE": media_capture,
                    "AUDIO_SELECT": audio_select, "AUDIO_START": audio_start,
                    "RTC_CAPTURE": rtc_capture, "INPUT_SELECT": input_select,
                    "RUMBLE_SELECT": rumble_select}.items():
    fixture = fixture.replace("@" + name + "@", value)
assert not re.search(r"@[A-Z_]+@", fixture)
with tempfile.TemporaryDirectory(prefix="x4-interaction-capture-") as directory:
    out = Path(directory)
    source = out / "capture.c"
    source.write_text(fixture, encoding="utf8")
    command = shlex.split(os.environ.get("CC", "gcc")) + [
        "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-DX4_STREAM_PROFILE=1",
        "-DX4_VIDEO_PROBE=0", "-I" + str(root), "-I" + str(root / "tests/host_stubs"),
        str(source), str(root / "src/ui/stream_settings.c"),
        str(root / "src/streaming/stream_profile.c"), "-Wl,--gc-sections",
    ] + shlex.split(os.environ.get("X4_TEST_CFLAGS", "")) + ["-o", str(out / "capture")]
    subprocess.run(command, check=True, cwd=root)
    subprocess.run([str(out / "capture")], check=True, cwd=root)
