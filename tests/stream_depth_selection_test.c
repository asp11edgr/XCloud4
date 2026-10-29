/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/streaming/stream_profile.h"
#include "../src/ui/stream_settings.h"
/* This focused runner does not link the production locale owner. */
#include "../src/ui/lang.c"
#include <assert.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* The published f18bbeb config already contains the picture/request options;
 * retain a strict ABI check against that baseline rather than the older 64 B. */
_Static_assert(sizeof(X4StreamRequestConfig)==72&&offsetof(X4StreamRequestConfig,cpu_variant)==49&&
    offsetof(X4StreamRequestConfig,decoder_depth)==50&&offsetof(X4StreamRequestConfig,interaction_modes)==51&&
    offsetof(X4StreamRequestConfig,convert_mode)==52&&offsetof(X4StreamRequestConfig,sharpness)==53&&
    offsetof(X4StreamRequestConfig,resolution_request_mode)==54&&offsetof(X4StreamRequestConfig,h264_profile)==56&&
    offsetof(X4StreamRequestConfig,max_fs)==60&&offsetof(X4StreamRequestConfig,max_mbps)==64,
    "published x86_64 captured config layout and stable CPU/depth offsets");

static void resolution_policy(void)
{
    X4StreamRequestConfig initial=x4_stream_settings_get();
    assert(initial.profile==X4_REQUEST_1080P_G&&initial.cpu_variant==X4_CPU_0&&
        initial.decoder_depth==X4_DEPTH_AUTO);
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile){
        X4StreamRequestConfig c=x4_stream_request_config((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709);
        assert(c.decoder_depth==X4_DEPTH_AUTO);
        unsigned expected=(profile==X4_REQUEST_720P60||profile==X4_REQUEST_720P_HQ)?1u:2u;
        assert(x4_stream_request_resolve_depth(&c,0,0)==expected);
        /* Actual SPS wins over the request in Auto, including a source
         * switching 720p -> 1080p coded1088 -> 720p in one session. */
        assert(x4_stream_request_resolve_depth(&c,1280,720)==1);
        assert(x4_stream_request_resolve_depth(&c,1920,1088)==2);
        assert(x4_stream_request_resolve_depth(&c,1280,720)==1);
        for(unsigned depth=1;depth<=2;++depth){
            c.decoder_depth=(unsigned char)depth;
            assert(x4_stream_request_resolve_depth(&c,0,0)==depth);
            assert(x4_stream_request_resolve_depth(&c,1280,720)==depth);
            assert(x4_stream_request_resolve_depth(&c,1920,1080)==depth);
        }
    }
    initial.decoder_depth=255;
    assert(x4_stream_request_capture(&initial).decoder_depth==X4_DEPTH_AUTO);
    assert(x4_stream_request_resolve_depth(&initial,1920,1088)==2);
}
static void menu_and_snapshots(void)
{
    X4StreamSettingsUi ui={0};
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    assert(ui.depth==X4_DEPTH_AUTO);
    for(unsigned row=1;row<=3;++row)
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==row);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true)&&ui.depth==X4_DEPTH_1);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true)&&ui.depth==X4_DEPTH_2);
    assert(x4_stream_settings_get().decoder_depth==X4_DEPTH_AUTO);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL|X4_SETTINGS_CONFIRM,true));
    assert(!ui.open&&!ui.applied&&x4_stream_settings_get().decoder_depth==X4_DEPTH_AUTO);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    assert(ui.depth==X4_DEPTH_AUTO);
    for(unsigned depth=0;depth<=2;++depth){
        ui.row=3;
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
        assert(ui.open&&ui.depth==(X4StreamDecoderDepth)((depth+1u)%3u));
    }
    assert(ui.depth==X4_DEPTH_AUTO);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true)&&ui.depth==X4_DEPTH_2);
    ui.profile=X4_REQUEST_1080P_G8;ui.cpu=X4_CPU_2;ui.color=X4_COLOR_FORCE_601;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(!ui.open&&ui.applied);
    X4StreamRequestConfig captured=x4_stream_settings_get();
    assert(captured.decoder_depth==X4_DEPTH_2&&captured.profile==X4_REQUEST_1080P_G8&&
        captured.cpu_variant==X4_CPU_2&&captured.color_preference==X4_COLOR_FORCE_601);
    assert(x4_stream_settings_set_cpu(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709,X4_CPU_1));
    assert(x4_stream_settings_get().decoder_depth==X4_DEPTH_2);
    assert(x4_stream_settings_set(X4_REQUEST_1080P_G,X4_COLOR_FORCE_601));
    assert(x4_stream_settings_get().decoder_depth==X4_DEPTH_2&&x4_stream_settings_get().cpu_variant==X4_CPU_1);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true)&&ui.depth==X4_DEPTH_2);
    ui.depth=X4_DEPTH_AUTO;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,false));
    assert(!ui.open&&x4_stream_settings_get().decoder_depth==X4_DEPTH_2);
    assert(x4_stream_settings_set_all(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709,X4_CPU_0,X4_DEPTH_AUTO));
    X4StreamRequestConfig next=x4_stream_request_capture(NULL);
    assert(next.decoder_depth==X4_DEPTH_AUTO&&next.profile==X4_REQUEST_720P_HQ);
    /* The next session sees the commit; an already captured auth/media/RTC
     * request is immutable and both old and new requests retain their depth. */
    assert(x4_stream_request_capture(&captured).decoder_depth==X4_DEPTH_2);
    assert(x4_stream_request_capture(&captured).profile==X4_REQUEST_1080P_G8);
    assert(!x4_stream_settings_set_all(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709,X4_CPU_0,(X4StreamDecoderDepth)-1));
    assert(!x4_stream_settings_set_all(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709,X4_CPU_0,(X4StreamDecoderDepth)3));
    assert(x4_stream_settings_get().decoder_depth==X4_DEPTH_AUTO);
}
static void wire_unchanged(void)
{
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile){
        char device[640],play[1024],caps[512],dimensions[512];
        char actual_device[640],actual_play[1024],actual_caps[512],actual_dimensions[512];
        X4StreamRequestConfig c=x4_stream_request_config((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709);
        assert(x4_stream_request_device_info(&c,device,sizeof(device)));
        assert(x4_stream_request_play_body(&c,"FIXTURE",play,sizeof(play)));
        assert(x4_stream_request_capabilities(&c,caps,sizeof(caps)));
        assert(x4_stream_request_dimensions(&c,dimensions,sizeof(dimensions)));
        const char *alias=x4_stream_request_resolution_alias(&c),*fmtp=c.h264_fmtp;
        for(unsigned depth=1;depth<=2;++depth){
            c.decoder_depth=(unsigned char)depth;
            assert(x4_stream_request_device_info(&c,actual_device,sizeof(actual_device)));
            assert(x4_stream_request_play_body(&c,"FIXTURE",actual_play,sizeof(actual_play)));
            assert(x4_stream_request_capabilities(&c,actual_caps,sizeof(actual_caps)));
            assert(x4_stream_request_dimensions(&c,actual_dimensions,sizeof(actual_dimensions)));
            assert(!strcmp(device,actual_device)&&!strcmp(play,actual_play)&&!strcmp(caps,actual_caps)&&
                !strcmp(dimensions,actual_dimensions)&&fmtp==c.h264_fmtp&&alias==x4_stream_request_resolution_alias(&c));
        }
    }
}
static void *publisher(void *unused)
{
    (void)unused;
    for(unsigned n=0;n<100000;++n){
        bool alternate=!!(n&1u);
        assert(x4_stream_settings_set_all(alternate?X4_REQUEST_1080P_G8:X4_REQUEST_720P_HQ,
            alternate?X4_COLOR_FORCE_601:X4_COLOR_AUTO_709,alternate?X4_CPU_2:X4_CPU_0,
            alternate?X4_DEPTH_2:X4_DEPTH_AUTO));
    }
    return NULL;
}
static void atomic_publication(void)
{
    pthread_t writer;assert(!pthread_create(&writer,NULL,publisher,NULL));
    for(unsigned n=0;n<100000;++n){
        X4StreamRequestConfig c=x4_stream_settings_get();
        assert((c.profile==X4_REQUEST_720P_HQ&&c.color_preference==X4_COLOR_AUTO_709&&
                c.cpu_variant==X4_CPU_0&&c.decoder_depth==X4_DEPTH_AUTO)||
            (c.profile==X4_REQUEST_1080P_G8&&c.color_preference==X4_COLOR_FORCE_601&&
                c.cpu_variant==X4_CPU_2&&c.decoder_depth==X4_DEPTH_2));
    }
    assert(!pthread_join(writer,NULL));
}
int main(void)
{
    resolution_policy();menu_and_snapshots();wire_unchanged();atomic_publication();
    puts("PASS decoder depth: Auto real SPS 1->2->1, overrides, immutable captures, R3 apply/cancel, unchanged public JSON and atomic publication");
    return 0;
}
