/* SPDX-License-Identifier: GPL-3.0-only */
/* Real x4_rtc_local_description; only libdatachannel C boundary is simulated. */
#define _POSIX_C_SOURCE 200809L
#include "../src/streaming/rtc_transport.c"
#include <assert.h>
static const char *local_offer="v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nb=AS:64\r\na=rtpmap:111 opus/48000/2\r\nm=video 9 UDP/TLS/RTP/SAVPF 116\r\nc=IN IP4 0.0.0.0\r\nb=AS:12000\r\na=rtpmap:116 H264/90000\r\na=fmtp:116 packetization-mode=1;profile-level-id=42e02a\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";
static unsigned recorded[4],bandwidth_records;
static unsigned answer_recorded[4],answer_records;
static int remote_result;
static unsigned expected_video_diagnostics;
/* The receive worker is not linked by this SDP-only fixture. Its current
 * offers/answers contain no SSRC, so the diagnostic sink must stay unused. */
void x4_rtc_receive_diag_set_expected_video(uint32_t ssrc,uint32_t pt,const char *mid)
{(void)ssrc;(void)pt;(void)mid;++expected_video_diagnostics;}
int rtcGetLocalDescription(int pc,char *out,int cap)
{
    assert(pc==7);int size=(int)strlen(local_offer)+1;if(!out)return size;
    if(cap<size)return RTC_ERR_TOO_SMALL;
    memcpy(out,local_offer,(size_t)size);return size;
}
int rtcGetLocalDescriptionType(int pc,char *out,int cap)
{assert(pc==7&&cap>=6);memcpy(out,"offer",6);return 6;}
void x4_rtc_receive_setting(unsigned kind,uint64_t a,uint64_t b,uint64_t c)
{(void)kind;(void)a;(void)b;(void)c;}
void x4_trace_sdp_profile_record(X4Trace *t,unsigned scope,unsigned pt,uint32_t level)
{(void)t;(void)scope;(void)pt;(void)level;}
void x4_trace_sdp_bandwidth_record(X4Trace *t,unsigned scope,unsigned sections,unsigned lines,unsigned kbps,unsigned malformed)
{
    (void)t;assert(scope<2);unsigned *out=scope?answer_recorded:recorded;
    out[0]=sections;out[1]=lines;out[2]=kbps;out[3]=malformed;
    if(scope)++answer_records;else ++bandwidth_records;
}
int rtcSetRemoteDescription(int pc,const char *text,const char *type)
{assert(pc==7&&text&&!strcmp(type,"answer")&&answer_records);return remote_result;}
int rtcGetSelectedCandidatePair(int pc, char *l, int ls, char *r, int rs) {(void)pc;(void)l;(void)ls;(void)r;(void)rs; return -1;}
int rtcGetRtt(int pc) {(void)pc; return -1;}
int x4_rtc_receiver_nack_accept(int track,int pc){(void)track;assert(pc==7);return 0;}
int x4_rtc_receive_track_config(int track){(void)track;return 0;}
int x4_rtc_poll_receiver_nack(int track){(void)track;return 0;}
int sceKernelUsleep(unsigned us){(void)us;assert(0&&"no thread created in offer fixture");return 0;}
int scePthreadCreate(OrbisPthread *t,const pthread_attr_t *a,void *(*fn)(void *),void *p,const char *name)
{(void)t;(void)a;(void)fn;(void)p;(void)name;assert(0&&"no thread created in offer fixture");return -1;}
int main(void)
{
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile){
        X4Rtc r={0};r.pc=7;atomic_init(&r.error,0);atomic_init(&r.gathering_done,true);atomic_init(&r.offer_logged,false);
        r.request_config=x4_stream_request_config((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709);
        char out[4096];unsigned before=bandwidth_records;
        assert(x4_rtc_local_description(&r,out,sizeof(out))==1);
        X4SdpVideoBandwidth bandwidth;assert(x4_stream_sdp_video_bandwidth(out,strlen(out),&bandwidth));
        assert(bandwidth.video_sections==1&&bandwidth.as_lines==1&&bandwidth.first_as_kbps==(profile==5?8000u:12000u));
        assert(strstr(out,"m=audio 9 UDP/TLS/RTP/SAVPF 111\r\nb=AS:64\r\n"));
        if(profile!=5)assert(!strcmp(out,local_offer));
        assert(bandwidth_records==before+1&&recorded[0]==1&&recorded[1]==1&&recorded[2]==bandwidth.first_as_kbps&&!recorded[3]);
        char again[4096];assert(x4_rtc_local_description(&r,again,sizeof(again))==1&&!strcmp(out,again));
        atomic_store(&r.gathering_done,false);assert(!x4_rtc_local_description(&r,out,sizeof(out))&&!out[0]);
        atomic_store(&r.gathering_done,true);assert(x4_rtc_local_description(&r,out,5)==RTC_ERR_TOO_SMALL);
    }
    X4Rtc remote={0};remote.pc=7;remote.nack_thread_started=true;
    atomic_init(&remote.error,0);atomic_init(&remote.state,X4_RTC_CONNECTED);
    const char *answer="m=audio 9 X 111\nb=AS:64\nm=video 9 X 116\nb=AS:7777\n";
    remote_result=-87;assert(x4_rtc_set_remote_description(&remote,answer)==-87);
    assert(answer_records==1&&answer_recorded[0]==1&&answer_recorded[1]==1&&answer_recorded[2]==7777&&!answer_recorded[3]);
    atomic_store(&remote.error,0);atomic_store(&remote.state,X4_RTC_CONNECTED);
    remote_result=0;assert(!x4_rtc_set_remote_description(&remote,"m=video 9 X 116\na=recvonly\n"));
    assert(answer_records==2&&answer_recorded[0]==1&&!answer_recorded[1]&&!answer_recorded[2]);
    puts("PASS actual RTC local offer: G8 alone advertises 8000 kbps; five existing profiles unchanged; numeric metadata and repeated bounded retrieval");
    puts("PASS actual RTC answer bandwidth observed before native acceptance/rejection; only numeric public metadata retained");
    assert(!expected_video_diagnostics);
}
