/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable bridge tests; no native timing/transport behavior is simulated. */
#include "../src/streaming/rtc_receive_diag.h"
#include "../src/media/live_trace.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_uint calls, reject_calls, setting_calls;
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
 assert(event==X4_MON_CR_RTC_SETTING&&stage==0xffffu);atomic_fetch_add(&setting_calls,1);}
static void *producer(void *unused)
{(void)unused;uint8_t p[12]={0x80,102,0,1};x4_rtc_receive_packet(X4_RD_UDP,p,sizeof(p),0,1);return NULL;}
static void *detacher(void *unused)
{(void)unused;x4_rtc_receive_detach(handle);atomic_store(&detached,true);return NULL;}

int main(void)
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
    puts("7 receive bridge cases passed");return 0;
}
