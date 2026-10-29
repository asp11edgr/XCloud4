/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/streaming/stream_profile.h"
#include "../src/auth/json.h"
/* Use the production es-MX default, including exact /play locale JSON. */
#include "../src/ui/lang.c"
#include <assert.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Published f18bbeb includes the picture/request options; the alias remains
 * derived and is not an additional field in this 72-byte captured config. */
_Static_assert(sizeof(X4StreamRequestConfig)==72&&offsetof(X4StreamRequestConfig,os_name)==24&&
    offsetof(X4StreamRequestConfig,display_info)==48&&offsetof(X4StreamRequestConfig,interaction_modes)==51&&
    offsetof(X4StreamRequestConfig,cpu_variant)==49&&offsetof(X4StreamRequestConfig,decoder_depth)==50&&
    offsetof(X4StreamRequestConfig,convert_mode)==52&&offsetof(X4StreamRequestConfig,sharpness)==53&&
    offsetof(X4StreamRequestConfig,resolution_request_mode)==54&&offsetof(X4StreamRequestConfig,h264_profile)==56&&
    offsetof(X4StreamRequestConfig,max_fs)==60&&offsetof(X4StreamRequestConfig,max_mbps)==64,
    "published captured x86_64 config ABI; alias is derived, never stored");
_Static_assert(X4_REQUEST_720P60==0&&X4_REQUEST_1080P_G==1&&X4_REQUEST_1080P_H==2&&
    X4_REQUEST_1080P_HQ==3&&X4_REQUEST_720P_HQ==4&&X4_REQUEST_1080P_G8==5&&X4_REQUEST_PROFILE_MAX==5&&X4_REQUEST_PROFILE_COUNT==6&&
    X4_REQUEST_1080P_A==1&&X4_REQUEST_1080P_B==2&&X4_REQUEST_1080P_C==X4_REQUEST_PROFILE_MAX,
    "retain IDs0..3, append4 and extend the protected admission ceiling");

static void value(const char *body,const char *key,unsigned expected)
{
    X4JsonSpan root,v;
    assert(!x4_json_parse(body,strlen(body),&root));
    assert(x4_json_member(root,key,&v)==1);
    uint32_t number=0;assert(!x4_json_uint32(v,&number)&&number==expected);
}
static void *publish(void *unused)
{
    (void)unused;
    for(unsigned n=0;n<100000;++n)
        assert(x4_stream_settings_set(n&1?X4_REQUEST_720P_HQ:X4_REQUEST_720P60,
            n&1?X4_COLOR_FORCE_601:X4_COLOR_AUTO_709));
    return NULL;
}
int main(void)
{
    X4StreamRequestConfig original=x4_stream_settings_get();
    assert(original.profile==X4_REQUEST_1080P_G&&original.color_preference==X4_COLOR_AUTO_709);
    assert(original.cpu_variant==X4_CPU_0&&original.decoder_depth==X4_DEPTH_AUTO&&original.interaction_modes==X4_INTERACTION_VSYNC_OFF);
    assert(!x4_stream_settings_set((X4StreamRequestProfile)X4_REQUEST_PROFILE_COUNT,X4_COLOR_AUTO_709));
    assert(!x4_stream_settings_set(X4_REQUEST_720P60,(X4StreamColorPreference)-1));
    assert(x4_stream_settings_get().profile==X4_REQUEST_1080P_G);
    char device[640],play[1024],caps[512],dimensions[512],expected[1024];
    static const char *os[]={"android","windows","windows","windows","windows","windows"};
    static const char *labels[]={"720p60","1080p-G","1080p-H","1080p-HQ","720p-HQ","1080p-G8"};
    for(unsigned p=0;p<X4_REQUEST_PROFILE_COUNT;++p)for(unsigned color=0;color<2;++color) {
        bool hd=(p>=1&&p<=3)||p==5;
        X4StreamRequestConfig c=x4_stream_request_config((X4StreamRequestProfile)p,(X4StreamColorPreference)color);
        assert(c.profile==p&&c.color_preference==color&&c.width==(hd?1920u:1280u)&&c.height==(hd?1080u:720u));
        assert(c.fps==60&&c.bitrate_kbps==(hd?12000u:10000u)&&!strcmp(c.os_name,os[p]));
        assert(c.h264_profile==(hd?0x42e02au:0x42e020u));
        assert(c.max_fs==(hd?8704u:5120u)&&c.max_mbps==(hd?522240u:216000u));
        assert(c.display_info==(p!=0&&p!=2)&&!strcmp(c.label,labels[p]));
        const char *alias=x4_stream_request_resolution_alias(&c);
        assert(p?alias&&!strcmp(alias,p==4?"720HQ":p==3?"1080HQ":"1080"):alias==NULL);
        assert(x4_stream_request_device_info(&c,device,sizeof(device)));
        int n=(p!=0&&p!=2)?snprintf(expected,sizeof(expected),
            "{\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\","
            "\"clientAppVersion\":\"26.1.97\",\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\","
            "\"sdkInstallId\":\"\"}},\"dev\":{\"os\":{\"name\":\"%s\",\"ver\":\"22631.2715\",\"platform\":\"desktop\"},"
            "\"hw\":{\"make\":\"Microsoft\",\"model\":\"unknown\",\"sdktype\":\"web\"},"
            "\"browser\":{\"browserName\":\"chrome\",\"browserVersion\":\"140.0.3485.54\"},\"displayInfo\":{"
            "\"dimensions\":{\"widthInPixels\":%u,\"heightInPixels\":%u},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}}}}",
            os[p],4096u,2160u):
            snprintf(expected,sizeof(expected),"{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\","
            "\"clientAppType\":\"native\",\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},"
            "\"dev\":{\"hw\":{\"make\":\"Sony\",\"model\":\"PS4\"},\"os\":{\"name\":\"%s\",\"platform\":\"console\"}}}",os[p]);
        assert(n>0&&!strcmp(device,expected));
        assert(p||!strcmp(device,X4_STREAM_NATIVE_720_DEVICE_INFO));
        assert(x4_stream_request_play_body(&c,"TEST",play,sizeof(play)));
        n=snprintf(expected,sizeof(expected),
            "{\"clientSessionId\":\"\",\"titleId\":\"TEST\",\"systemUpdateGroup\":\"\",\"settings\":{"
            "\"nanoVersion\":\"V3;WebrtcTransport.dll\",\"enableOptionalDataCollection\":false,"
            "\"enableTextToSpeech\":false,\"highContrast\":0,\"locale\":\"es-MX\",\"useIceConnection\":false,"
            "\"timezoneOffsetMinutes\":-360,\"sdkType\":\"web\",\"osName\":\"%s\"},"
            "\"serverId\":\"\",\"fallbackRegionNames\":[]}",os[p]);
        assert(n>0&&!strcmp(play,expected));
        assert(x4_stream_request_capabilities(&c,caps,sizeof(caps)));
        if(p) {
            assert(!strcmp(caps,"{}"));
            assert(x4_stream_request_capabilities(&c,caps,3)&&!strcmp(caps,"{}"));
            assert(!x4_stream_request_capabilities(&c,caps,2)&&!caps[0]);
            assert(x4_stream_request_capabilities(&c,caps,sizeof(caps)));
        } else {
            n=snprintf(expected,sizeof(expected),"{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":60,\"maxWidth\":%u,\"maxHeight\":%u,\"maxBitrateKbps\":%u,\"video\":{\"width\":%u,\"height\":%u,\"maxWidth\":%u,\"maxHeight\":%u,\"maxBitrateKbps\":%u}}",c.width,c.height,c.bitrate_kbps,c.width,c.height,c.width,c.height,c.bitrate_kbps);
            assert(n>0&&!strcmp(caps,expected));
            value(caps,"supportsFps",60);value(caps,"maxWidth",c.width);value(caps,"maxHeight",c.height);value(caps,"maxBitrateKbps",c.bitrate_kbps);
        }
        assert(x4_stream_request_dimensions(&c,dimensions,sizeof(dimensions)));
        n=snprintf(expected,sizeof(expected),"{\"horizontal\":%u,\"vertical\":%u,\"preferredWidth\":%u,\"preferredHeight\":%u,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":%u,\"safeAreaBottom\":%u,\"supportsCustomResolution\":true}",c.width,c.height,c.width,c.height,c.width,c.height);
        assert(n>0&&!strcmp(dimensions,expected));
        value(dimensions,"horizontal",c.width);value(dimensions,"vertical",c.height);
        value(dimensions,"preferredWidth",c.width);value(dimensions,"preferredHeight",c.height);
        value(dimensions,"safeAreaRight",c.width);value(dimensions,"safeAreaBottom",c.height);
        /* The three local switches are never wire fields. All exact fixtures
         * above remain identical for every combination, in every profile. */
        for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes) {
            char actual[1024];c.interaction_modes=(unsigned char)modes;
            assert(x4_stream_request_device_info(&c,actual,sizeof(actual))&&!strcmp(actual,device));
            assert(x4_stream_request_play_body(&c,"TEST",actual,sizeof(actual))&&!strcmp(actual,play));
            assert(x4_stream_request_capabilities(&c,actual,sizeof(actual))&&!strcmp(actual,caps));
            assert(x4_stream_request_dimensions(&c,actual,sizeof(actual))&&!strcmp(actual,dimensions));
            assert(!strcmp(c.h264_fmtp,hd?
                "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240":
                "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000"));
        }
        if(!color)printf("PUBLIC_PROFILE_%u\nX-MS-Device-Info: %s\n/play: %s\ncapabilities: %s\ndimensions: %s\nresolution alias: %s\nSDP fmtp: %s\n",p,device,play,caps,dimensions,alias?alias:"none",c.h264_fmtp);
        assert(!x4_stream_request_device_info(&c,device,1)&&!device[0]);
        assert(!x4_stream_request_capabilities(&c,caps,1)&&!caps[0]);
        assert(!x4_stream_request_dimensions(&c,dimensions,1)&&!dimensions[0]);
        assert(!x4_stream_request_play_body(&c,"A",play,1)&&!play[0]);
        assert(!x4_stream_request_play_body(&c,"A\"B",play,sizeof(play)));
        assert(!x4_stream_request_play_body(&c,"../X/Y",play,sizeof(play)));
        assert(!x4_stream_request_play_body(&c,"",play,sizeof(play)));
        assert(!x4_stream_request_play_body(&c,NULL,play,sizeof(play)));
        assert(x4_stream_request_play_body(&c,"VALID.Game-12_3",play,sizeof(play)));
        char long_id[130];memset(long_id,'A',sizeof(long_id));long_id[129]=0;
        assert(!x4_stream_request_play_body(&c,long_id,play,sizeof(play)));
        long_id[128]=0;assert(x4_stream_request_play_body(&c,long_id,play,sizeof(play)));
        /* Callers cannot inject different OS, bitrate, FMTP, or dimensions by
         * mutating a captured public struct. Constructors canonicalize enums. */
        c.os_name="malicious\"";c.width=99999;c.h264_fmtp=NULL;c.display_info=!c.display_info;
        assert(x4_stream_request_device_info(&c,device,sizeof(device))&&!strstr(device,"malicious"));
        assert(strstr(device,os[p])&&(!!strstr(device,"displayInfo"))==(p!=0&&p!=2));
        assert(x4_stream_request_capabilities(&c,caps,sizeof(caps)));
        if(p)assert(!strcmp(caps,"{}"));else value(caps,"maxWidth",1280);
        alias=x4_stream_request_resolution_alias(&c);
        assert(p?alias&&!strcmp(alias,p==4?"720HQ":p==3?"1080HQ":"1080"):alias==NULL);
    }
    assert(!x4_stream_request_device_info(&original,NULL,0));
    X4StreamRequestConfig invalid=x4_stream_request_config((X4StreamRequestProfile)-1,(X4StreamColorPreference)99);
    assert(invalid.profile==X4_REQUEST_720P60&&invalid.color_preference==X4_COLOR_AUTO_709);
    invalid.profile=(X4StreamRequestProfile)99;
    assert(!x4_stream_request_resolution_alias(&invalid));
    X4StreamRequestConfig captured=x4_stream_request_config(X4_REQUEST_1080P_G,X4_COLOR_AUTO_709);
    assert(x4_stream_settings_set(X4_REQUEST_1080P_HQ,X4_COLOR_FORCE_601));
    assert(!strcmp(x4_stream_request_resolution_alias(&captured),"1080"));
    assert(!strcmp(x4_stream_request_resolution_alias(NULL),"1080HQ"));
    assert(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
    assert(!x4_stream_request_resolution_alias(NULL));
    assert(x4_stream_settings_set_extended(X4_REQUEST_720P60,X4_COLOR_AUTO_709,
        X4_CPU_3,X4_DEPTH_2,X4_INTERACTION_MASK));
    pthread_t id;assert(!pthread_create(&id,NULL,publish,NULL));
    for(unsigned n=0;n<100000;++n) {
        X4StreamRequestConfig c=x4_stream_settings_get();
        assert((c.profile==X4_REQUEST_720P60&&c.color_preference==X4_COLOR_AUTO_709)||
            (c.profile==X4_REQUEST_720P_HQ&&c.color_preference==X4_COLOR_FORCE_601));
        assert(original.profile==X4_REQUEST_1080P_G&&original.width==1920&&original.height==1080&&original.display_info&&!strcmp(original.os_name,"windows"));
        assert(c.cpu_variant==X4_CPU_3&&c.decoder_depth==X4_DEPTH_2&&c.interaction_modes==X4_INTERACTION_MASK);
    }
    assert(!pthread_join(id,NULL));
    captured.interaction_modes=255;
    X4StreamRequestConfig sanitized=x4_stream_request_capture(&captured);
    assert(sanitized.interaction_modes==X4_INTERACTION_MASK);
    assert(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
    puts("PASS runtime request profiles: exact public JSON, all profiles/colors, bounded IDs, capacities, immutable snapshots and atomic menu pairs");
}
