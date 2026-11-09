/* SPDX-License-Identifier: GPL-3.0-only */
/* The production callback and startup sequence, with existing native-boundary
 * doubles. No network packets or credentials are used by this fixture. */
#include <rtc/rtc.h>
#include <sched.h>
#include "../src/streaming/rtc_receive_diag.h"
#include "../src/streaming/nack_grace_inbox.h"
static int close_native_step(void);
#if !X4_PUBLIC
static int close_native_closed(int channel, rtcClosedCallbackFunc callback);
#define rtcSetClosedCallback close_native_closed
#endif
/* Only the dependency's asynchronous object operations are simulated. The
 * production close callback gate, drain and final snapshot run unchanged. */
#define rtcSetMessageCallback(...) close_native_step()
#define rtcSetOpenCallback(...) close_native_step()
#define rtcDeleteDataChannel(...) close_native_step()
#define rtcDeleteTrack(...) close_native_step()
#define rtcSetLocalDescriptionCallback(...) close_native_step()
#define rtcSetLocalCandidateCallback(...) close_native_step()
#define rtcSetStateChangeCallback(...) close_native_step()
#define rtcSetIceStateChangeCallback(...) close_native_step()
#define rtcSetGatheringStateChangeCallback(...) close_native_step()
#define rtcDeletePeerConnection(...) close_native_step()
#define x4_nack_grace_inbox_unbind(...) ((void)0)
#define x4_rtc_receive_detach(...) ((void)0)
#ifdef X4_T41_LIFETIME_INTEGRATION
#define x4_log_printf t41_genshin_log
#else
#define x4_log_printf printf
#endif
#define main rtc_startup_previous_test_main
#include "rtc_resolution_startup_test.c"
#undef main
#undef x4_log_printf
#include "../src/ui/picture_settings.h"
X4PictureSettingsConfig x4_picture_settings_config(void) {return (X4PictureSettingsConfig){0};}
X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption option)
{(void)option;return X4_REQUEST_1080P_G;}

static atomic_bool release_closing_callback,closing_callback_entered;
static int close_native_step(void)
{atomic_store(&release_closing_callback,true);return 0;}
#if !X4_PUBLIC
static int close_native_closed(int channel, rtcClosedCallbackFunc callback)
{(void)channel;(void)callback;return close_native_step();}
#endif
int x4_rtc_close_receiver_nack(int track){(void)track;return 0;}
int scePthreadJoin(OrbisPthread thread,void **out){return pthread_join(thread,out);}
int sceKernelUsleep(unsigned us){(void)us;sched_yield();return 0;}
uint64_t x4_receive_test_clock(void){return 1000001;}
uint64_t x4_receive_test_threadid(void){return 1;}
void x4_trace_monitor_transport_packet(X4Trace *trace,unsigned stage,bool valid,uint32_t ssrc,
    uint16_t sequence,unsigned pt,size_t bytes,uint64_t now,uint64_t previous,uint32_t flags)
{(void)trace;(void)stage;(void)valid;(void)ssrc;(void)sequence;(void)pt;(void)bytes;(void)now;(void)previous;(void)flags;}

static void notify(X4Rtc *rtc,const char *type,const char *body,const char *id)
{
    char escaped[1024],message[1600];size_t used=0;
    for(const char *p=body;*p;++p){
        CHECK(used+2<sizeof(escaped));
        if(*p=='"'||*p=='\\')escaped[used++]='\\';
        escaped[used++]=*p;
    }
    escaped[used]=0;
    int n=snprintf(message,sizeof(message),
        "{\"type\":\"%s\",\"target\":\"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect\",\"id\":%s,\"content\":\"%s\"}",
        type,id,escaped);
    CHECK(n>0&&(size_t)n<sizeof(message));
    channel_message(MESSAGE,message,-n-1,rtc->slot);
}
static void expect_ack(unsigned index,const char *id)
{
    char expected[160];
    snprintf(expected,sizeof(expected),"{\"type\":\"TransactionComplete\",\"content\":\"\\\"\\\"\",\"id\":%s,\"cv\":\"\"}",id);
    CHECK(index<sent&&events[index].channel==MESSAGE&&!strcmp(events[index].text,expected));
}
static X4RtcSnapshot snapshot(X4Rtc *rtc)
{X4RtcSnapshot out;x4_rtc_snapshot(rtc,&out);return out;}
static void idle(void)
{
    X4Rtc *rtc=fixture(1);
    notify(rtc,"TransactionStart","{\"reason\":\"WarningForBeingIdle\",\"secondsUntilKick\":37}","\"fake-transaction-1\"");
    X4RtcSnapshot out=snapshot(rtc);
    CHECK(!out.server_closed&&out.server_idle_sequence==1&&out.server_idle_seconds==37);
    CHECK(out.state==X4_RTC_CONNECTED&&!out.error);expect_ack(0,"\"fake-transaction-1\"");
    notify(rtc,"Message","{\"reason\":\"WarningForBeingIdle\"}","\"fake-transaction-2\"");
    out=snapshot(rtc);CHECK(!out.server_closed&&out.server_idle_sequence==2&&out.server_idle_seconds==120);
    expect_ack(1,"\"fake-transaction-2\"");
    notify(rtc,"Message","{\"reason\":\"WarningForBeingIdle\",\"secondsUntilKick\":0}","\"fake-id\"");
    notify(rtc,"Message","{\"reason\":\"WarningForBeingIdle\",\"secondsUntilKick\":3601}","\"fake-id\"");
    CHECK(sent==2&&snapshot(rtc).server_idle_sequence==2);destroy(rtc);
}
static void terminal(void)
{
    const char *reasons[]={"KickForClosedGame","SomeNewReason","reason.with.period","9InitialDigit"};
    for(unsigned i=0;i<4;++i){
        X4Rtc *rtc=fixture(1);char body[160];
        snprintf(body,sizeof(body),"{\"reason\":\"%s\",\"hr\":-2147467259}",reasons[i]);
        notify(rtc,i&1?"Message":"TransactionStart",body,"\"fake-terminal\"");
        X4RtcSnapshot out=snapshot(rtc);
        CHECK(out.server_closed&&out.server_hr_valid&&out.server_hr==0x80004005u);
        CHECK(!strcmp(out.server_reason,i<2?reasons[i]:""));
        CHECK(out.state==X4_RTC_CONNECTED&&!out.error);expect_ack(0,"\"fake-terminal\"");
        state_callback(0,RTC_CLOSED,rtc->slot);
        out=snapshot(rtc);
        CHECK(out.state==X4_RTC_CLOSED&&out.server_closed);
        /* Terminal state is immutable even if another callback reports a
         * different reason before the worker reacts. */
        notify(rtc,"Message","{\"reason\":\"OtherReason\",\"hr\":0}","\"fake-later\"");
        out=snapshot(rtc);CHECK(out.server_hr==0x80004005u&&!strcmp(out.server_reason,i<2?reasons[i]:""));
        destroy(rtc);
    }
    X4Rtc *rtc=fixture(1);
    notify(rtc,"Message","{\"reason\":\"KickForClosedGame\",\"hr\":\"0x80150001\"}","\"fake-hex\"");
    CHECK(snapshot(rtc).server_hr==0x80150001u&&snapshot(rtc).server_hr_valid);destroy(rtc);
}
static void terminal_after_transport(void)
{
    const char *ids[]={"1","\"1\"","9876543210123456789","-1.25e+4"};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);++i){
        X4Rtc *rtc=fixture(1);
        state_callback(0,RTC_CLOSED,rtc->slot);
        X4RtcSnapshot out=snapshot(rtc);
        CHECK(out.state==X4_RTC_CLOSED&&!out.server_closed);
        open_message=false;
        notify(rtc,"TransactionStart","{\"reason\":\"KickForOutOfStreamingTime\"}",ids[i]);
        expect_ack(0,ids[i]);
        CHECK(events[0].result<0); /* The SCTP channel can already be gone. */
        out=snapshot(rtc);
        CHECK(out.state==X4_RTC_CLOSED&&out.server_closed);
        CHECK(!strcmp(out.server_reason,"KickForOutOfStreamingTime"));
        destroy(rtc);
    }
}
static void opaque_and_object_notices(void)
{
    const char *ids[]={"\"fake+/==\"", "\"fake id\"", "\"fake\\\"id\"", "\"fake\\u002fvalue\""};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);++i){
        X4Rtc *rtc=fixture(1);
        notify(rtc,"TransactionStart","{\"reason\":\"KickForClosedGame\"}",ids[i]);
        X4RtcSnapshot view=snapshot(rtc);
        CHECK(view.server_closed&&!strcmp(view.server_reason,"KickForClosedGame"));
        expect_ack(0,ids[i]);
        destroy(rtc);
    }
    X4Rtc *rtc=fixture(1);
    static const char object[]="{\"type\":\"TransactionStart\",\"id\":\"fake-object\","
        "\"target\":\"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect\","
        "\"content\":{\"reason\":\"KickForClosedGame\",\"secondsUntilKick\":0,\"hr\":1,\"hr\":2}}";
    channel_message(MESSAGE,object,-(int)sizeof(object),rtc->slot);
    X4RtcSnapshot view=snapshot(rtc);
    CHECK(view.server_closed&&!view.server_hr_valid&&!strcmp(view.server_reason,"KickForClosedGame"));
    expect_ack(0,"\"fake-object\"");
    destroy(rtc);
}

static void invalid(void)
{
    X4Rtc *rtc=fixture(1);
    const char *bad[]={"{","{}","{\"type\":\"Message\",\"type\":\"TransactionStart\"}",
        "{\"type\":\"Message\",\"target\":\"other\",\"content\":\"{}\",\"id\":\"fake-id\"}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
        channel_message(MESSAGE,bad[i],-(int)strlen(bad[i])-1,rtc->slot);
    notify(rtc,"Message","{","\"fake-id\"");
    notify(rtc,"Message","{\"reason\":\"KickForClosedGame\"}","true");
    notify(rtc,"Message","{\"reason\":\"KickForClosedGame\"}","null");
    notify(rtc,"Message","{\"reason\":\"KickForClosedGame\"}","12345678901234567890123456789012345678901234567890123456789012345678901234567890");
    notify(rtc,"Message","{\"reason\":\"KickForClosedGame\"}","\"\"");
    notify(rtc,"Message","{\"reason\":\"KickForClosedGame\"}","[]");
    char too_big[4098];memset(too_big,' ',sizeof(too_big));
    channel_message(MESSAGE,too_big,(int)sizeof(too_big),rtc->slot);
    CHECK(!sent&&!snapshot(rtc).server_closed&&!snapshot(rtc).server_idle_sequence);destroy(rtc);
}
static void repeated_alias_and_diagnostics(void)
{
    X4Rtc *rtc=fixture(1);rtc->handshake_ack=true;start_channels(rtc);
    CHECK(count_fragment("userRequestedResolutionUpdate")==1);
    x4_stream_startup_admit(&bound_startup,1000100);
    CHECK(bound_startup.admitted&&bound_startup.first_admitted_us==1000100);
    /* A new peer binds the same decoder-owned startup object. The decoder's
     * plain fields stay intact while the callback repeats its atomic record. */
    rtc->startup_sent=false;atomic_store(&rtc->startup_complete,false);
    start_channels(rtc);
    CHECK(count_fragment("userRequestedResolutionUpdate")==2&&x4_stream_startup_alias_sends(&bound_startup)==2);
    CHECK(bound_startup.admitted&&bound_startup.first_admitted_us==1000100);destroy(rtc);

    open_diagnostics(false);
    uint8_t packet[12]={0x80,102,0,1,0,0,0,0,1,2,3,4};
    x4_rtc_receive_packet(X4_RD_SRTP_VALID,packet,sizeof(packet),0,X4_RD_AUTHENTICATED);
    x4_rtc_diag_record_rtt_sample(17);
    x4_rtc_diag_reconnect_attempt(X4_RECONNECT_FAILED);
    x4_rtc_diag_reconnect_result(X4_RECONNECT_FAILED,true,87);
    open_diagnostics(true);
    X4InboundRtpSummary receive;x4_rtc_receive_diag_summary(&receive);
    X4RttSummary rtt;x4_rtc_diag_get_rtt_summary(&rtt);
    X4ReconnectSummary reconnect;x4_rtc_diag_get_reconnect_summary(&reconnect);
    CHECK(receive.stream_count==1&&receive.streams[0].packets==1&&rtt.sample_count==1);
    CHECK(reconnect.attempts==1&&reconnect.reconnects==1&&reconnect.records[0].first_video_ms==87);
    open_diagnostics(false);x4_rtc_receive_diag_summary(&receive);
    x4_rtc_diag_get_reconnect_summary(&reconnect);x4_rtc_diag_get_rtt_summary(&rtt);
    CHECK(!receive.stream_count&&!reconnect.attempts&&!rtt.sample_count);
    CHECK(input_metrics_begin(false,0));input_metrics.successful=9;
    input_timing_record(&input_metrics.state_age,1234,INPUT_AGE_QUANTUM_USEC);
    input_metrics_end(true);
    CHECK(input_metrics_begin_mode(false,0,true));
    CHECK(input_metrics.successful==9&&input_metrics.state_age.count==1);
    input_metrics_end(true);
    CHECK(input_metrics_begin(false,0));CHECK(!input_metrics.successful&&!input_metrics.state_age.count);
    input_metrics_end(true);
}
static void *late_terminal_callback(void *pointer)
{
    CallbackSlot *slot=pointer;X4Rtc *rtc=enter(slot);CHECK(rtc);
    atomic_store(&closing_callback_entered,true);
    while(!atomic_load(&release_closing_callback))sched_yield();
    static const char message[]={"{\"type\":\"Message\",\"target\":\"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect\",\"id\":27,\"content\":\"{\\\"reason\\\":\\\"KickForClosedGame\\\",\\\"hr\\\":0}\"}"};
    lifetime_message(rtc,MESSAGE,message,strlen(message));
    atomic_fetch_add(&rtc->video_packets,1);leave(slot);return NULL;
}
static void terminal_during_close(void)
{
    X4Rtc *rtc=fixture(1);CallbackSlot *slot=rtc->slot;pthread_t callback;
    state_callback(0,RTC_CLOSED,slot);
    atomic_store(&release_closing_callback,false);atomic_store(&closing_callback_entered,false);
    CHECK(!pthread_create(&callback,NULL,late_terminal_callback,slot));
    while(!atomic_load(&closing_callback_entered))sched_yield();
    X4RtcSnapshot before=snapshot(rtc);CHECK(before.state==X4_RTC_CLOSED&&!before.server_closed&&!before.video_packets);
    X4RtcSnapshot final;CHECK(!x4_rtc_close_snapshot(rtc,&final));
    CHECK(!pthread_join(callback,NULL));
    CHECK(final.server_closed&&!strcmp(final.server_reason,"KickForClosedGame")&&final.video_packets==1);
    expect_ack(0,"27");
    CHECK(!slot->context&&!slot->active);free(slot);
}
#ifdef X4_T41_LIFETIME_INTEGRATION
X4Rtc *t41_genshin_open(uint64_t at)
{
    fixture_clock_us=at;
    X4Rtc *rtc=fixture(1);
#if !X4_PUBLIC
    lab_transport_context=rtc;
#endif
    return rtc;
}
void t41_genshin_notice(X4Rtc *rtc)
{
    static const char fake_id[]="\"FAKE_PRIVATE_TRANSACTION+/==\"";
    notify(rtc,"TransactionStart","{\"reason\":\"KickForClosedGame\"}",fake_id);
    expect_ack(0,fake_id);
}
void t41_genshin_close(X4Rtc *rtc,uint64_t at)
{
    CHECK(at==fixture_clock_us+3600000);
    fixture_clock_us=at;
#if !X4_PUBLIC
    x4_rtc_lab_native_transport_state(80,0);
    x4_rtc_lab_native_transport_state(87,0);
    lab_channel_closed(MESSAGE,rtc->slot);
    CHECK(rtc->lab_closures[LAB_CLOSE_DTLS].at==at);
#endif
    state_callback(0,RTC_CLOSED,rtc->slot);
}
void t41_genshin_drain(X4Rtc *rtc,X4RtcSnapshot *final)
{
    CallbackSlot *slot=rtc->slot;
    CHECK(!x4_rtc_close_snapshot(rtc,final));
    CHECK(!slot->context&&!slot->active);
    free(slot);
}
#define main t41_rtc_lifetime_previous_main
#endif

int main(int argc,char **argv)
{
    /* These are registered by the native open path, not by this host fixture. */
    (void)description_callback;(void)candidate_callback;(void)state_callback;
    (void)ice_state_callback;(void)gathering_callback;(void)rtp_callback;

    idle();terminal();terminal_after_transport();opaque_and_object_notices();invalid();
    repeated_alias_and_diagnostics();terminal_during_close();
    x4_rtc_diag_reconnect_attempt(X4_RECONNECT_MANUAL);
    x4_rtc_diag_reconnect_result(X4_RECONNECT_MANUAL,false,0);
    x4_rtc_diag_reconnect_attempt(X4_RECONNECT_NO_VIDEO);
    x4_rtc_diag_reconnect_result(X4_RECONNECT_NO_VIDEO,true,114);
    if(argc==2){FILE *f=fopen(argv[1],"w");CHECK(f);fputc('{',f);
        x4_rtc_diag_write_rtt_json(f);fputs("\"end\":true}\n",f);CHECK(!fclose(f));}
    puts("PASS real channel_message: idle/default, strict malformed/oversize, sanitized terminal HRESULT, exact TransactionComplete; terminal does not masquerade as RTC failure");
    puts("PASS exact numeric and string TransactionComplete; terminal notice survives transport close and failed ACK");
    puts("PASS reconnect-open diagnostic policy retains receive counters/RTT/reconnect history; initial policy resets; same startup repeats alias without resetting decoder fields");
    puts("PASS real RTC close joins an entered late terminal callback and captures its notice/final packet count before freeing; worker can terminate instead of replacing peer");
    return 0;
}
