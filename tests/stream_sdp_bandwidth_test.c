/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/streaming/stream_sdp_bandwidth.h"
#include "../src/streaming/stream_profile.h"
/* Preserve the real es-MX default used by inherited /play constructors. */
#include "../src/ui/lang.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void one(const char *input,const char *expected)
{
    char sdp[4096];strcpy(sdp,input);
    assert(!x4_stream_sdp_limit_video(sdp,sizeof(sdp),8000)&&!strcmp(sdp,expected));
    X4SdpVideoBandwidth v;assert(x4_stream_sdp_video_bandwidth(sdp,strlen(sdp),&v));
    assert(v.video_sections==1&&v.as_lines==1&&v.first_as_kbps==8000&&!v.malformed_as_lines);
    assert(!x4_stream_sdp_limit_video(sdp,sizeof(sdp),8000)&&!strcmp(sdp,expected));
}
int main(void)
{
    const char *golden="v=0\r\nb=AS:555\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nb=AS:64\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 116\r\nc=IN IP4 0.0.0.0\r\nb=AS:8000\r\na=recvonly\r\na=rtpmap:116 H264/90000\r\na=fmtp:116 packetization-mode=1;profile-level-id=42e02a\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nb=AS:123\r\n";
    one("v=0\r\nb=AS:555\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nb=AS:64\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 116\r\nc=IN IP4 0.0.0.0\r\na=recvonly\r\na=rtpmap:116 H264/90000\r\na=fmtp:116 packetization-mode=1;profile-level-id=42e02a\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nb=AS:123\r\n",golden);
    one("m=video 9 X 116\r\ni=sample\r\nc=IN IP4 0.0.0.0\r\nb=AS:12000\r\nb=AS:9999\r\na=recvonly\r\n",
        "m=video 9 X 116\r\ni=sample\r\nc=IN IP4 0.0.0.0\r\nb=AS:8000\r\na=recvonly\r\n");
    one("m=video 9 X 116\na=recvonly\n","m=video 9 X 116\nb=AS:8000\na=recvonly\n");
    one("m=video 9 X 116","m=video 9 X 116\nb=AS:8000\n");
    char multiple[512]="m=video 9 X 116\nb=AS:9\nm=audio 9 X 111\nb=AS:64\nm=video 9 X 117\nb=AS:10\n";
    assert(!x4_stream_sdp_limit_video(multiple,sizeof(multiple),8000));
    X4SdpVideoBandwidth bw;assert(x4_stream_sdp_video_bandwidth(multiple,strlen(multiple),&bw));
    assert(bw.video_sections==2&&bw.as_lines==2&&bw.first_as_kbps==8000);
    const char *answer="b=AS:55\nm=audio 9 X 111\nb=AS:64\nm=video 9 X 116\nb=AS:7777\nb=AS:broken\nm=application 9 X data\nb=AS:9999\n";
    assert(x4_stream_sdp_video_bandwidth(answer,strlen(answer),&bw));
    assert(bw.video_sections==1&&bw.as_lines==1&&bw.first_as_kbps==7777&&bw.malformed_as_lines==1);
    assert(!x4_stream_sdp_video_bandwidth(NULL,0,&bw));
    char tiny[32]="m=video 9 X 116\n";char original[32];strcpy(original,tiny);
    assert(x4_stream_sdp_limit_video(tiny,strlen(tiny)+1,8000)<0&&!strcmp(tiny,original));
    assert(x4_stream_sdp_limit_video(tiny,sizeof(tiny),0)<0&&!strcmp(tiny,original));
    memset(tiny,'X',sizeof(tiny));assert(x4_stream_sdp_limit_video(tiny,sizeof(tiny),8000)<0);
    char audio[64]="m=audio 9 X 111\nb=AS:64\n";char audio_before[64];strcpy(audio_before,audio);
    assert(x4_stream_sdp_limit_video(audio,sizeof(audio),8000)<0&&!strcmp(audio,audio_before));
    X4StreamRequestConfig g=x4_stream_request_config(X4_REQUEST_1080P_G,X4_COLOR_AUTO_709);
    X4StreamRequestConfig g8=x4_stream_request_config(X4_REQUEST_1080P_G8,X4_COLOR_AUTO_709);
    assert(g.width==g8.width&&g.height==g8.height&&g.bitrate_kbps==g8.bitrate_kbps&&!strcmp(g.h264_fmtp,g8.h264_fmtp));
    char a[1024],b[1024];
    assert(x4_stream_request_device_info(&g,a,sizeof(a))&&x4_stream_request_device_info(&g8,b,sizeof(b))&&!strcmp(a,b));
    assert(x4_stream_request_play_body(&g,"TEST",a,sizeof(a))&&x4_stream_request_play_body(&g8,"TEST",b,sizeof(b))&&!strcmp(a,b));
    assert(!strcmp(x4_stream_request_resolution_alias(&g8),"1080"));
    assert(x4_stream_request_capabilities(&g,a,sizeof(a))&&x4_stream_request_capabilities(&g8,b,sizeof(b))&&!strcmp(a,b));
    puts("PUBLIC_G8_OFFER_WITHOUT_KEYS");puts(golden);
    puts("PASS G8 exact inherited request and video-only b=AS insertion/replacement, LF/CRLF, duplicates, multiple media sections, bounded failures and answer numeric metadata");
}
