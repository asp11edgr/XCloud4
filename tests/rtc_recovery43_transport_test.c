/* SPDX-License-Identifier: GPL-3.0-only */
/* Real transport setup helper, offer and answer functions. RTC C API, random
 * and recovery registry are explicit doubles; no native peer or socket. */
#define _POSIX_C_SOURCE 200809L
#define X4_RECOVERY43_TRANSPORT_TEST 1
#include "../src/streaming/rtc_transport.c"
#include <assert.h>
static const char *offer="v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\na=rtpmap:102 H264/90000\r\na=rtcp-fb:102 nack\r\n";
static const char *full_answer="v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102 103 49\r\n"
    "a=rtpmap:102 H264/90000\r\na=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
    "a=rtpmap:49 flexfec-03/90000\r\na=ssrc-group:FID 7 8\r\na=ssrc-group:FEC-FR 7 9\r\n"
    "a=ssrc:7 cname:test\r\na=ssrc:8 cname:test\r\na=ssrc:9 cname:test\r\n";
static const char *rtx_answer="v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102 103\r\n"
    "a=rtpmap:102 H264/90000\r\na=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
    "a=ssrc-group:FID 7 8\r\na=ssrc:7 cname:test\r\na=ssrc:8 cname:test\r\n";
static unsigned sequence,begin_order,answer_order,native_order,answers;
static unsigned begin_mode,begin_loss,random_calls;
static bool random_fail;
static int native_result;
static X4VideoRepair43Config accepted;
int x4_native_random(void *out,size_t bytes)
{++random_calls;assert(bytes==4);if(random_fail)return -1;uint32_t seed=0x43123456;memcpy(out,&seed,4);return 0;}
int x4_recovery43_begin(uint32_t epoch,unsigned mode,unsigned loss,uint32_t seed)
{assert(epoch);assert(seed||!loss);if(X4_PUBLIC)assert(!seed&&!loss);begin_order=++sequence;begin_mode=mode;begin_loss=loss;return 0;}
int x4_recovery43_answer(uint32_t epoch,const X4VideoRepair43Config *config)
{assert(epoch&&config);accepted=*config;answer_order=++sequence;++answers;return 0;}
void x4_recovery43_poll(uint32_t epoch,uint64_t now,bool final){(void)epoch;(void)now;(void)final;}
int x4_log_printf(const char *format,...){(void)format;return 0;}
int rtcGetLocalDescription(int pc,char *out,int cap)
{assert(pc==7);int n=(int)strlen(offer)+1;if(!out)return n;if(cap<n)return RTC_ERR_TOO_SMALL;memcpy(out,offer,(size_t)n);return n;}
int rtcGetLocalDescriptionType(int pc,char *out,int cap)
{assert(pc==7&&cap>=6);memcpy(out,"offer",6);return 6;}
int rtcSetRemoteDescription(int pc,const char *text,const char *type)
{assert(pc==7&&text&&!strcmp(type,"answer"));native_order=++sequence;assert(answer_order&&answer_order<native_order);return native_result;}
int x4_rtc_receiver_nack_accept(int track,int pc){(void)track;assert(pc==7);return 0;}
int x4_rtc_receive_track_config(int track){(void)track;return 0;}
int x4_rtc_poll_receiver_nack(int track){(void)track;return 0;}
void x4_rtc_receive_setting(unsigned kind,uint64_t a,uint64_t b,uint64_t c)
{(void)kind;(void)a;(void)b;(void)c;}
void x4_rtc_receive_diag_set_expected_video(uint32_t ssrc,uint32_t pt,const char *mid)
{(void)ssrc;(void)pt;(void)mid;}
void x4_trace_sdp_profile_record(X4Trace *t,unsigned scope,unsigned pt,uint32_t level)
{(void)t;(void)scope;(void)pt;(void)level;}
void x4_trace_sdp_bandwidth_record(X4Trace *t,unsigned scope,unsigned sections,unsigned lines,unsigned kbps,unsigned malformed)
{(void)t;(void)scope;(void)sections;(void)lines;(void)kbps;(void)malformed;}
int sceKernelUsleep(unsigned us){(void)us;assert(0);return 0;}
uint64_t sceKernelGetProcessTime(void){return 1000;}
int scePthreadCreate(OrbisPthread *t,const pthread_attr_t *a,void *(*fn)(void *),void *p,const char *name)
{(void)t;(void)a;(void)fn;(void)p;(void)name;assert(0);return -1;}
static void setup(X4Rtc *rtc,unsigned mode,unsigned loss)
{
    memset(rtc,0,sizeof(*rtc));rtc->pc=7;rtc->nack_epoch=mode+1;
    rtc->nack_thread_started=true;
    atomic_init(&rtc->error,0);atomic_init(&rtc->state,X4_RTC_CONNECTED);
    atomic_init(&rtc->gathering_done,true);atomic_init(&rtc->offer_logged,false);
    rtc->request_config=x4_stream_request_config(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709);
    rtc->request_config.recovery_options=(unsigned char)(mode|((loss==3?2:loss)<<2));
    sequence=begin_order=answer_order=native_order=answers=random_calls=0;native_result=0;
    recovery43_prepare(rtc);assert(begin_order&&rtc->recovery43_started);
}
#if X4_PUBLIC
static void count_emission(void *context,const uint8_t *packet,size_t bytes,
    unsigned method,uint64_t now)
{
    assert(packet&&bytes==13&&method<=X4_REPAIR43_FEC&&now);
    ++((unsigned *)context)[method];
}
static void check_public_engine(const X4VideoRepair43Config *config,bool with_repair)
{
    X4VideoRepair43 *engine=x4_video_repair43_create(config);
    assert(engine);
    unsigned counts[3]={0};
    unsigned char media[13]={0x80,102,0,10,0,0,0,1,0,0,0,7,0x65};
    unsigned char rtx[15]={0x80,103,0,1,0,0,0,1,0,0,0,8,0,11,0x65};
    assert(!x4_video_repair43_process(engine,media,sizeof(media),1000,count_emission,counts));
    media[3]=12;
    assert(!x4_video_repair43_process(engine,media,sizeof(media),2000,count_emission,counts));
    assert(!x4_video_repair43_process(engine,rtx,sizeof(rtx),3000,count_emission,counts));
    assert(counts[X4_REPAIR43_NORMAL]==2&&counts[X4_REPAIR43_RTX]==(unsigned)with_repair);
    x4_video_repair43_free(engine);
}
static void test_incomplete_answer(const char *missing)
{
    X4Rtc rtc;setup(&rtc,2,3);
    char answer[4096];strcpy(answer,full_answer);
    char *at=strstr(answer,missing);assert(at);
    const size_t removed=strlen(missing);
    memmove(at,at+removed,strlen(at+removed)+1);
    char out[4096];assert(x4_rtc_local_description(&rtc,out,sizeof(out))==1);
    assert(!x4_rtc_set_remote_description(&rtc,answer));
    assert(answers==1&&begin_order<answer_order&&answer_order<native_order);
    assert(accepted.mode==0&&!accepted.rtx_accepted&&!accepted.fec_accepted);
    assert(!accepted.rtx_ssrc&&!accepted.fec_ssrc&&!accepted.rtx_pt&&!accepted.fec_pt);
}
#endif

int main(void)
{
    /* Fixed Lab mode-2 wire fixture, shared byte-for-byte by both products. */
    const char *mode2_offer="v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102 103 49\r\n"
        "a=rtpmap:102 H264/90000\r\na=rtcp-fb:102 nack\r\n"
        "a=rtpmap:103 rtx/90000\r\na=fmtp:103 apt=102\r\n"
        "a=rtpmap:49 flexfec-03/90000\r\na=fmtp:49 repair-window=10000000\r\n";
#if X4_PUBLIC
    random_fail=true;
    for(unsigned mode=0;mode<=2;++mode){
        X4Rtc rtc;setup(&rtc,mode,3);char out[4096];
        assert(begin_mode==2&&!begin_loss&&!random_calls);
        assert(x4_rtc_local_description(&rtc,out,sizeof(out))==1&&!strcmp(out,mode2_offer));
        assert(!x4_rtc_set_remote_description(&rtc,full_answer));
        assert(accepted.mode==2&&accepted.rtx_accepted&&accepted.fec_accepted);
        assert(accepted.media_ssrc==7&&accepted.rtx_ssrc==8&&accepted.fec_ssrc==9);
        check_public_engine(&accepted,true);
        assert(answers==1&&begin_order<answer_order&&answer_order<native_order);
        assert(!x4_rtc_set_remote_description(&rtc,full_answer)&&answers==1);
    }
    X4Rtc partial;setup(&partial,2,3);
    assert(!x4_rtc_set_remote_description(&partial,rtx_answer));
    assert(!accepted.mode&&!accepted.rtx_accepted&&!accepted.fec_accepted&&accepted.media_ssrc==7);
    check_public_engine(&accepted,false);
    test_incomplete_answer(" 49");
    test_incomplete_answer("a=fmtp:103 apt=102\r\n");
    test_incomplete_answer("a=ssrc-group:FID 7 8\r\n");
    test_incomplete_answer("a=ssrc-group:FEC-FR 7 9\r\n");
    test_incomplete_answer("a=ssrc:8 cname:test\r\n");
    test_incomplete_answer("a=ssrc:9 cname:test\r\n");
    X4Rtc too_small;setup(&too_small,2,3);
    char out[4096];assert(x4_rtc_local_description(&too_small,out,strlen(offer)+1)==1);
    assert(too_small.recovery43_offer_failed&&!strcmp(out,offer));
    assert(!x4_rtc_set_remote_description(&too_small,full_answer));
    assert(!accepted.mode&&!accepted.rtx_accepted&&!accepted.fec_accepted);
    puts("PASS 43c public SDP: same bytes as Lab mode 2; incomplete mapping or offer failure leaves only NACK");
#else
    for(unsigned mode=0;mode<=2;++mode){
        X4Rtc rtc;setup(&rtc,mode,mode?1:0);char out[4096];
        assert(begin_mode==mode&&begin_loss==(mode?1u:0u));
        assert(x4_rtc_local_description(&rtc,out,sizeof(out))==1);
        if(!mode)assert(!strcmp(out,offer));
        else if(mode==2)assert(!strcmp(out,mode2_offer));
        else{assert(strstr(out,"a=rtpmap:103 rtx/90000\r\n"));
            assert(strstr(out,"a=fmtp:103 apt=102\r\n"));
            assert((strstr(out,"a=rtpmap:49 flexfec-03/90000")!=NULL)==(mode==2));}
        assert(!x4_rtc_set_remote_description(&rtc,full_answer));
        assert(answers==1&&begin_order<answer_order&&answer_order<native_order);
        assert(accepted.rtx_accepted==(mode>=1)&&accepted.fec_accepted==(mode==2));
        assert(accepted.media_ssrc==7);
        assert(!x4_rtc_set_remote_description(&rtc,full_answer)&&answers==1);
    }
    X4Rtc partial;setup(&partial,2,3);char out[4096];
    assert(x4_rtc_local_description(&partial,out,sizeof(out))==1);
    assert(!x4_rtc_set_remote_description(&partial,rtx_answer));
    assert(accepted.rtx_accepted&&!accepted.fec_accepted&&accepted.loss_percent==3);
    X4Rtc failing;setup(&failing,1,0);native_result=-77;
    assert(x4_rtc_set_remote_description(&failing,full_answer)==-77);
    assert(atomic_load(&failing.error)==-77&&atomic_load(&failing.state)==X4_RTC_FAILED);
    random_fail=true;X4Rtc fallback;setup(&fallback,2,3);
    assert(!fallback.recovery43_mode&&!fallback.recovery43_loss&&begin_mode==0&&begin_loss==0);
    assert(x4_rtc_local_description(&fallback,out,sizeof(out))==1&&!strcmp(out,offer));
    puts("PASS recovery43 transport: real setup helper, Solo NACK byte-exact, offer modes, "
        "answer before native apply, per-feature fallback, native error unchanged, RNG fallback");
#endif
}
