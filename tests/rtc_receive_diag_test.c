/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable bridge tests; no native timing/transport behavior is simulated. */
#include "../src/streaming/rtc_receive_diag.h"
#include "../src/media/live_trace.h"
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static atomic_uint calls, reject_calls, setting_calls, nack_calls;
static atomic_bool hold, entered, release_hook, detached;
static bool last_valid;
static uint32_t last_ssrc, last_flags;
static uint16_t last_seq;
static unsigned last_pt, last_stage;
static uint64_t last_prior;
static atomic_ullong ticks;
static uint64_t simulated_id;
static X4Trace *handle = (X4Trace *)(uintptr_t)1;
uint64_t x4_receive_test_clock(void) { errno=ERANGE; return atomic_fetch_add(&ticks,1)+1; }
void x4_receive_test_sleep(unsigned us) { usleep(us); }
uint64_t x4_receive_test_threadid(void)
{ return simulated_id ? simulated_id : (uint64_t)(uintptr_t)pthread_self(); }
void x4_trace_monitor_transport_packet(X4Trace *t,unsigned stage,bool valid,uint32_t ssrc,
    uint16_t seq,unsigned pt,size_t bytes,uint64_t time,uint64_t prior,uint32_t flags)
{
    (void)bytes;(void)time;assert(t==handle);last_valid=valid;last_ssrc=ssrc;last_seq=seq;
    last_pt=pt;last_stage=stage;last_flags=flags;last_prior=prior;atomic_fetch_add(&calls,1);
    if(atomic_load(&hold)){atomic_store(&entered,true);while(!atomic_load(&release_hook))usleep(50);}
}
void x4_trace_monitor_transport_reject(X4Trace *t,unsigned stage,unsigned reason,int rc,
    bool valid,uint32_t ssrc,uint16_t seq,uint64_t time)
{(void)stage;(void)reason;(void)rc;(void)valid;(void)ssrc;(void)seq;(void)time;assert(t==handle);atomic_fetch_add(&reject_calls,1);}
void x4_trace_monitor_critical(X4Trace *t,uint64_t time,unsigned event,unsigned stage,
    uint32_t flags,uint64_t identity,uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{(void)time;(void)flags;(void)identity;(void)source;(void)a;(void)b;(void)c;assert(t==handle);
 assert(stage==0xffffu);
 if(event==X4_MON_CR_RTC_SETTING) atomic_fetch_add(&setting_calls,1);
 else if(event==X4_MON_CR_NACK_REQUEST) atomic_fetch_add(&nack_calls,1);
 else { assert(event==X4_MON_CR_NACK_COUNTER && a==109); }}
static void *producer(void *unused)
{(void)unused;uint8_t p[12]={0x80,102,0,1};x4_rtc_receive_packet(X4_RD_UDP,p,sizeof(p),0,1);return NULL;}
static void *detacher(void *unused)
{(void)unused;x4_rtc_receive_detach(handle);atomic_store(&detached,true);return NULL;}

static void *startup_producer(void *argument)
{
    uint64_t now = (uint64_t)(uintptr_t)argument;
    for (unsigned i = 0; i < 256; ++i) {
        x4_rtc_diag_startup_pli(now + i);
        x4_rtc_diag_startup_range(X4_STARTUP_RETURN, 1, now, now + i);
    }
    return NULL;
}

static void startup_fixture(void)
{
    static const uint64_t start = 1000000, end = 11000000;
    static const uint8_t video[12] = {0x80, 102};
    unsigned current, minimum, maximum;
    x4_rtc_receive_diag_reset();
    assert(!x4_rtc_diag_startup_active(start) && !x4_rtc_diag_startup_release_count());
    x4_rtc_diag_video_grace_stats(0, start, &current, &minimum, &maximum);
    assert(current == 80 && minimum == 80 && maximum == 80);
    x4_rtc_diag_record_rtt_sample(16);
    x4_rtc_diag_video_grace_stats(0, start, &current, &minimum, &maximum);
    assert(current == 70 && minimum == 70 && maximum == 80);
    x4_rtc_diag_record_rtt_sample(26);
    x4_rtc_diag_video_grace_stats(0, start, &current, &minimum, &maximum);
    assert(current == 95 && minimum == 70 && maximum == 95);
    x4_rtc_diag_video_presented(start);
    x4_rtc_diag_video_presented(start + 1000);
    assert(!x4_rtc_diag_startup_active(start - 1) && x4_rtc_diag_startup_active(start));
    assert(x4_rtc_diag_startup_active(end - 1) && !x4_rtc_diag_startup_active(end));
    x4_rtc_diag_video_grace_stats(0, end - 1, &current, &minimum, &maximum);
    assert(current == 250 && minimum == 70 && maximum == 250);
    x4_rtc_diag_video_grace_stats(0, end, &current, &minimum, &maximum);
    assert(current == 95 && minimum == 70 && maximum == 250);
    /* No SDP SSRC/PT published yet: the primary video PT still counts. */
    atomic_store(&ticks, start + 999);
    x4_rtc_receive_packet(X4_RD_UDP, video, sizeof(video), 0, X4_RD_UNAUTHENTICATED);
    atomic_store(&ticks, start + 1999);
    x4_rtc_receive_packet(X4_RD_TRACK, video, sizeof(video), 0, X4_RD_AUTHENTICATED);
    atomic_store(&ticks, start + 184999);
    x4_rtc_receive_packet(X4_RD_UDP, video, sizeof(video), 0, X4_RD_UNAUTHENTICATED);
    atomic_store(&ticks, start + 191999);
    x4_rtc_receive_packet(X4_RD_TRACK, video, sizeof(video), 0, X4_RD_AUTHENTICATED);
    x4_rtc_diag_startup_release(X4_STARTUP_RELEASE_REPAIRED);
    x4_rtc_diag_startup_release(X4_STARTUP_RELEASE_CAPACITY);
    assert(x4_rtc_diag_startup_release_count() == (X4_PUBLIC ? 0 : 2));
    for (unsigned i = 0; i < 34; ++i)
        x4_rtc_diag_startup_range(X4_STARTUP_DETECT, i + 1, start + 2000, start + 2000);
    x4_rtc_diag_startup_range(X4_STARTUP_NACK, 1, start + 2000, start + 4000);
    x4_rtc_diag_startup_range(X4_STARTUP_RETRY, 1, start + 2000, start + 24000);
    x4_rtc_diag_startup_range(X4_STARTUP_RETURN, 1, start + 2000, start + 192000);
    x4_rtc_diag_startup_pli(start + 193000);
    x4_rtc_diag_startup_report(end - 1, false);
    x4_rtc_diag_startup_report(end, false);
    x4_rtc_diag_startup_report(end, true);

    /* Ingress and PLI producers contend with the final report. */
    x4_rtc_receive_diag_reset();
    x4_rtc_diag_video_presented(start);
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_create(&workers[i], NULL, startup_producer,
            (void *)(uintptr_t)(start + 1000 + 1000 * i)));
    x4_rtc_diag_startup_report(start + 100000, true);
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
    x4_rtc_diag_startup_report(start + 100000, true);
    x4_rtc_receive_diag_reset();
    assert(!x4_rtc_diag_startup_active(start) && !x4_rtc_diag_startup_release_count());
}

int main(int argc,char **argv)
{
    uint8_t p[24]={0x80,0x80|102,0xff,0xff,0,0,0,0,0x12,0x34,0x56,0x78};
    x4_rtc_receive_attach(handle);
    errno=EACCES;uint64_t now=x4_rtc_receive_packet(X4_RD_UDP,p,12,0,1);
    assert(errno==EACCES&&last_valid&&last_ssrc==0x12345678&&last_seq==65535&&last_pt==102);
    assert(x4_rtc_receive_socket_time()==now&&last_prior==0);
    puts("PASS fixed RTP header, wrap value, socket timestamp and errno");
    p[0]=0x83;x4_rtc_receive_packet(X4_RD_SRTP_INPUT,p,12,0,1);assert(!last_valid);
    x4_rtc_receive_packet(X4_RD_SRTP_INPUT,p,24,3,1);assert(last_valid&&last_prior==3);
    p[0]=0x90;x4_rtc_receive_packet(X4_RD_SRTP_INPUT,p,12,0,1);assert(last_valid);
    uint8_t captured[12]={0x83,102,0,1,0,0,0,0,1,2,3,4};
    x4_rtc_receive_packet(X4_RD_DTLS_ACCEPT,captured,1500,0,1);
    assert(last_valid&&last_ssrc==0x01020304&&last_seq==1);
    puts("PASS CSRC length and encrypted extension not inspected");
    p[1]=200;x4_rtc_receive_packet(X4_RD_RTCP,p,12,0,2);assert(!last_valid&&(last_flags&32));
    p[0]=0;x4_rtc_receive_packet(X4_RD_UDP,p,24,0,1);assert(!last_valid);
    x4_rtc_receive_packet(X4_RD_UDP,NULL,0,0,1);assert(!last_valid);
    puts("PASS RTCP/STUN/empty tuples remain unclassified");
    p[0]=0x80;p[1]=102;now=x4_rtc_receive_packet(X4_RD_TRACK_POP,p,12,7,2);
    assert(x4_rtc_receive_delivery_time()==now&&last_stage==X4_RD_TRACK_POP);
    errno=E2BIG;x4_rtc_receive_reject(4,X4_RD_SRTP_ERROR,7,p,12);assert(errno==E2BIG&&reject_calls==1);
    unsigned settings_before=atomic_load(&setting_calls);
    x4_rtc_receive_setting(4,6,0,0);assert(setting_calls==settings_before+1);
    puts("PASS independent reject, settings and delivery clock");
    errno=E2BIG;x4_rtc_nack_event(X4_MON_CR_NACK_REQUEST,0,3,4,5,6,7);
    assert(errno==E2BIG && atomic_load(&nack_calls)==1);
    x4_rtc_nack_delivery(0x100000002ull,0x8000006612345678ull,65536,789,123456);
    X4NackPacket ticket=x4_rtc_nack_delivery_ticket();
    assert(ticket.identity==0x100000002ull && ticket.source==0x8000006612345678ull &&
        ticket.extended_sequence==65536 && ticket.rtp_timestamp==789 && ticket.authenticated_us==123456);
    assert(!ticket.sequence.sequence_epoch);
    X4NackSequenceTag sequence={.anchor_extended=6,.previous_high_extended=65538,
        .anchor_us=123000,.transport_epoch=1,.sequence_epoch=2,.previous_epoch=1,
        .anchor_rtp_timestamp=789,.reason=X4_NACK_SEQUENCE_OMISSION};
    x4_rtc_nack_sequence_delivery(&sequence);ticket=x4_rtc_nack_delivery_ticket();
    assert(ticket.sequence.sequence_epoch==2&&ticket.sequence.transport_epoch==1&&
        ticket.sequence.anchor_extended==6&&ticket.sequence.previous_high_extended==65538&&
        ticket.identity==0x100000002ull);
    x4_rtc_nack_delivery(0,0,0,0,0);ticket=x4_rtc_nack_delivery_ticket();
    assert(!ticket.identity && !ticket.source && !ticket.extended_sequence && !ticket.rtp_timestamp && !ticket.authenticated_us&&
        !ticket.sequence.sequence_epoch&&!ticket.sequence.transport_epoch);
    x4_rtc_nack_delivery(0,0x8000006612345678ull,6,789,123456);
    x4_rtc_nack_sequence_delivery(&sequence);ticket=x4_rtc_nack_delivery_ticket();
    assert(!ticket.identity&&ticket.sequence.sequence_epoch==2&&ticket.source);
    x4_rtc_nack_sequence_delivery(NULL);ticket=x4_rtc_nack_delivery_ticket();
    assert(!ticket.sequence.sequence_epoch&&!ticket.identity&&ticket.source);
    puts("PASS ordinary provenance, watched ticket, and delivery reset remain separate");
    puts("PASS numeric NACK critical observation and clearing of delivery tickets");
    atomic_store(&hold,true);pthread_t writer,closer;
    assert(!pthread_create(&writer,NULL,producer,NULL));while(!atomic_load(&entered))usleep(50);
    assert(!pthread_create(&closer,NULL,detacher,NULL));usleep(10000);assert(!atomic_load(&detached));
    atomic_store(&release_hook,true);assert(!pthread_join(writer,NULL));assert(!pthread_join(closer,NULL));
    unsigned before=atomic_load(&calls);x4_rtc_receive_packet(X4_RD_UDP,p,12,0,1);assert(calls==before);
    puts("PASS detach drains entered producers and later hooks see NULL");
    x4_rtc_receive_attach(handle);atomic_store(&hold,false);
    x4_rtc_receive_detach((X4Trace *)(uintptr_t)2);
    x4_rtc_receive_packet(X4_RD_UDP,p,12,0,1);assert(calls==before+1);
    x4_rtc_receive_detach(handle);puts("PASS wrong-owner detach preserves the active sink");
    for(unsigned i=1;i<=33;++i){simulated_id=i;(void)x4_rtc_receive_socket_time();}
    simulated_id=100;assert(x4_rtc_receive_socket_time()==0);
    assert(x4_rtc_receive_delivery_time()==0);
    puts("PASS bounded thread identity registry returns unknown when full");

    x4_rtc_receive_diag_reset();
    uint8_t rtp_video[12] = {0x80, 102, 0, 1, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
    x4_rtc_receive_packet(X4_RD_SRTP_VALID, rtp_video, sizeof(rtp_video), 0, 2);
    x4_rtc_receive_packet(X4_RD_SRTP_VALID, rtp_video, sizeof(rtp_video), 0, 2);
    uint8_t rtp_audio[12] = {0x80, 111, 0, 2, 0, 0, 0, 0, 0x99, 0x88, 0x77, 0x66};
    x4_rtc_receive_packet(X4_RD_SRTP_VALID, rtp_audio, sizeof(rtp_audio), 0, 2);
    x4_rtc_receive_reject(X4_RD_SRTP_INPUT, X4_RD_SRTP_AUTH_FAIL, -1, NULL, 0);
    x4_rtc_receive_reject(X4_RD_SRTP_INPUT, X4_RD_DEMUX_UNKNOWN, -1, NULL, 0);
    x4_rtc_receive_reject(X4_RD_PEER, X4_RD_NO_TRACK, -1, NULL, 0);
    x4_rtc_receive_diag_set_expected_video(0x12345678, 102, "video");
    X4InboundRtpSummary summary;
    x4_rtc_receive_diag_summary(&summary);
    assert(summary.sdp_video_ssrc == 0x12345678);
    assert(summary.sdp_video_pt == 102);
    assert(!strcmp(summary.sdp_video_mid, "video"));
    assert(summary.stream_count == 2);
    assert(summary.srtp_decrypt_failures == 1);
    assert(summary.unknown_ssrc_packets == 1);
    assert(summary.unknown_pt_packets == 1);
    puts("PASS inbound RTP stream breakdown and lowest-level reject counters");

    static const char host_candidate[] = "candidate:1 1 UDP 2130706431 192.0.2.50 50000 typ host";
    static const char srflx_candidate[] =
        "a=candidate:2 1 UDP 1694498815 203.0.113.5 50001 typ srflx raddr 192.0.2.50 rport 50000";
    x4_rtc_diag_record_remote_candidate(host_candidate, "video");
    x4_rtc_diag_record_remote_candidate(srflx_candidate, "198.51.100.99");
    x4_rtc_diag_record_selected_pair(srflx_candidate, host_candidate);
    X4IceConnectionSummary ice_summary;
    x4_rtc_diag_get_ice_summary(&ice_summary);
    assert(ice_summary.remote_candidate_count == 2);
    assert(strcmp(ice_summary.remote_candidates[0].type, "host") == 0);
    assert(strcmp(ice_summary.remote_candidates[0].ip, "192.0.2.50") == 0);
    assert(ice_summary.remote_candidates[0].port == 50000);
    assert(strcmp(ice_summary.remote_candidates[1].type, "srflx") == 0);
    assert(strcmp(ice_summary.remote_candidates[1].ip, "203.0.113.5") == 0);
    assert(ice_summary.remote_candidates[1].port == 50001);
    assert(ice_summary.has_selected_pair);
    assert(strcmp(ice_summary.selected_remote, host_candidate) == 0);
    assert(strcmp(ice_summary.selected_local, srflx_candidate) == 0);
    puts("PASS ICE remote candidates and selected candidate pair recording");

    x4_rtc_diag_record_remote_candidate("candidate:3 1 TCP 1677734911 2001:db8::5 50002 typ relay", "video");
    /* Even an unrecognized type, protocol or MID may not smuggle an address
     * into a classification field. A non-IP endpoint has no prefix. */
    x4_rtc_diag_record_remote_candidate("candidate:4 1 2001:db8::9 1 198.51.100.2 50003 typ 2001:db8::7", "video");
    x4_rtc_diag_record_selected_pair("candidate:5 1 UDP 1 example.invalid 54321 typ host", NULL);
    x4_rtc_diag_record_selected_pair("candidate:6 1 udp 1 2001:db8:ab12::6 54321 typ host", "[2001:db8::5]:50002");
    FILE *ice_json=tmpfile();assert(ice_json);
    fputc('{',ice_json);x4_rtc_diag_write_ice_json(ice_json);fputs("\"end\":true}\n",ice_json);
    assert(!fflush(ice_json));long json_bytes=ftell(ice_json);assert(json_bytes>0&&json_bytes<8192);
    rewind(ice_json);char json[8192];assert(fread(json,1,(size_t)json_bytes,ice_json)==(size_t)json_bytes);json[json_bytes]=0;
#if X4_PUBLIC
    assert(strstr(json,"\"remote_candidate_counts\":{\"host\":1,\"srflx\":1,\"relay\":1,\"prflx\":0,\"unknown\":1}"));
    assert(strstr(json,"\"remote_type\":\"relay\",\"transport\":\"TCP\""));
    assert(!strstr(json,".x.x")&&!strstr(json,"::/32")&&!strstr(json,"2001:db8")&&!strstr(json,"selected_candidate_pair"));
#else
    assert(strstr(json,"\"remote_candidates\":[")&&strstr(json,"\"selected_candidate_pair\":{"));
    assert(strstr(json,"192.0.x.x")&&strstr(json,"2001:db8::/32")&&strstr(json,"\"remote\":\"relay/tcp\""));
#endif
    assert(!strstr(json,"candidate:")&&!strstr(json,"\"port\"")&&!strstr(json,"game_server_"));
    assert(!strstr(json,"192.0.2.")&&!strstr(json,"203.0.113.")&&!strstr(json,"198.51.100."));
    assert(!strstr(json,"2001:db8::5")&&!strstr(json,"2001:db8:ab12")&&!strstr(json,"50002"));
    if(argc==2){FILE *copy=fopen(argv[1],"wb");assert(copy);assert(fwrite(json,1,(size_t)json_bytes,copy)==(size_t)json_bytes);assert(!fclose(copy));}
    assert(!fclose(ice_json));
    puts("PASS real ICE JSON exporter: public types and protocols; Lab adds masked prefixes");

    /* Test RTT sampling, aggregation (p50, p95), and reset */
    x4_rtc_receive_diag_reset();
    X4RttSummary rtt_sum;
    x4_rtc_diag_get_rtt_summary(&rtt_sum);
    assert(rtt_sum.sample_count == 0);
    assert(rtt_sum.p50 == -1);

    /* Negative samples ignored */
    x4_rtc_diag_record_rtt_sample(-1);
    x4_rtc_diag_record_rtt_sample(-5);
    x4_rtc_diag_get_rtt_summary(&rtt_sum);
    assert(rtt_sum.sample_count == 0);

    /* Record 10 known samples: 20, 30, 40, 50, 60, 70, 80, 90, 100, 110 */
    /* Sorted: [20, 30, 40, 50, 60, 70, 80, 90, 100, 110], n=10 */
    /* Median at n/2 = index 5 -> 70 */
    /* p95 at (10*95)/100 = 9 -> 110 */
    int test_rtts[] = {80, 20, 110, 40, 60, 30, 100, 50, 70, 90};
    for (int i = 0; i < 10; ++i) {
        x4_rtc_diag_record_rtt_sample(test_rtts[i]);
    }
    x4_rtc_diag_get_rtt_summary(&rtt_sum);
    assert(rtt_sum.sample_count == 10);
    assert(rtt_sum.p50 == 70);
    assert(rtt_sum.p95 == 110);
    for (int i = 0; i < 10; ++i) {
        assert(rtt_sum.samples[i] == test_rtts[i]); /* original arrival order preserved in series */
    }

    /* Reset clears everything */
    x4_rtc_receive_diag_reset();
    x4_rtc_diag_get_rtt_summary(&rtt_sum);
    assert(rtt_sum.sample_count == 0);
    assert(rtt_sum.p50 == -1);
    x4_rtc_diag_get_ice_summary(&ice_summary);
    assert(ice_summary.remote_candidate_count == 0);
    assert(!ice_summary.has_selected_pair);
    puts("PASS RTT sampling, percentile calculation, and clean session reset");

    assert(x4_rtc_diag_rtt_p50_ms() == -1 && x4_rtc_diag_video_grace_ms() == 60);
    for (unsigned i = 0; i < X4_RTT_MAX_SAMPLES; ++i)
        x4_rtc_diag_record_rtt_sample(7);
    assert(x4_rtc_diag_rtt_p50_ms() == 7 && x4_rtc_diag_video_grace_ms() == 45);
    for (unsigned i = 0; i < 31; ++i) x4_rtc_diag_record_rtt_sample(26);
    assert(x4_rtc_diag_rtt_p50_ms() == 26 && x4_rtc_diag_video_grace_ms() == 67);
    for (unsigned i = 0; i < 31; ++i) x4_rtc_diag_record_rtt_sample(INT_MAX);
    assert(x4_rtc_diag_video_grace_ms() == 100);
    x4_rtc_diag_reset_video_rtt();
    assert(x4_rtc_diag_rtt_p50_ms() == -1 && x4_rtc_diag_video_grace_ms() == 60);
    x4_rtc_diag_get_rtt_summary(&rtt_sum);
    assert(rtt_sum.sample_count == X4_RTT_MAX_SAMPLES && rtt_sum.p50 == 7);
    puts("PASS bounded rolling video RTT, grace clamp, and peer reset");

    startup_fixture();
    puts("PASS first presentation, adaptive bounds, startup collection/close and reset");
    puts("12 receive bridge cases passed");return 0;
}
