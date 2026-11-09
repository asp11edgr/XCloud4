/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int test_clock(clockid_t,struct timespec *);
static int test_cond_wait(pthread_cond_t *,pthread_mutex_t *,const struct timespec *);
#if !X4_PUBLIC
static int test_printf(const char *,...);
#endif
#define clock_gettime test_clock
#define pthread_cond_timedwait test_cond_wait
#if !X4_PUBLIC
#define printf test_printf
#endif
#include "../src/audio/mic_capture.c"
#undef clock_gettime
#undef pthread_cond_timedwait
#undef printf

static unsigned high_calls,low_calls,close_calls;
static unsigned open_logs,state_logs,selected_logs;
#if !X4_PUBLIC
static char state_line[256],selected_line[256],high_line[256];
#endif
static bool absent=true;
static unsigned present_port;
static bool virtual_clock,cancel_wait,spurious_wait;
static uint64_t virtual_us;
static unsigned waits;
static uint64_t wait_delays[4];

#if !X4_PUBLIC
static int test_printf(const char *format,...)
{
    char text[512];va_list args;va_start(args,format);
    int count=vsnprintf(text,sizeof(text),format,args);va_end(args);
    assert(count>0&&(size_t)count<sizeof(text));
    if(strstr(text,"AudioIn open ")){++open_logs;
        if(strstr(text,"rate=48000")){assert(strlen(text)<sizeof(high_line));strcpy(high_line,text);}}
    if(strstr(text,"AudioIn silent_state ")){++state_logs;assert(strlen(text)<sizeof(state_line));strcpy(state_line,text);}
    if(strstr(text,"AudioIn selected_port=")){++selected_logs;assert(strlen(text)<sizeof(selected_line));strcpy(selected_line,text);}
    return count;
}
#endif
static int test_clock(clockid_t id,struct timespec *t)
{
    if(!virtual_clock)return clock_gettime(id,t);
    t->tv_sec=(time_t)(virtual_us/1000000u);t->tv_nsec=(long)(virtual_us%1000000u)*1000;
    return 0;
}
static int test_cond_wait(pthread_cond_t *condition,pthread_mutex_t *mutex,const struct timespec *deadline)
{
    assert(virtual_clock&&condition==&mic.wake&&mutex==&mic.lock&&waits<4u);
    uint64_t end=(uint64_t)deadline->tv_sec*1000000u+(uint64_t)deadline->tv_nsec/1000u;
    assert(end>=virtual_us);wait_delays[waits++]=end-virtual_us;
    if(cancel_wait){++mic.wanted.epoch;return 0;}
    if(spurious_wait){spurious_wait=false;return 0;}
    virtual_us=end;return ETIMEDOUT;
}
int x4_mic_test_audio_open(int32_t user,unsigned rate,unsigned port,unsigned samples)
{
    assert(user==7&&port<2u&&samples==GRAIN);
    if(rate==RATE_HIGH){++high_calls;return (int)UINT32_C(0x80260103);}
    assert(rate==RATE_LOW);++low_calls;return 100+(int)port;
}
int x4_mic_test_audio_read(int handle,int16_t *pcm)
{(void)handle;(void)pcm;assert(!"capture reads are not needed for configuration/absence test");return -1;}
int x4_mic_test_audio_state(int handle)
{assert(handle==100||handle==101);return absent||(unsigned)(handle-100)!=present_port?NO_DEVICE:0;}
int x4_mic_test_audio_close(int handle)
{assert(handle==100||handle==101);++close_calls;return 0;}

static void configuration(void)
{
    Capture wanted={.generation=1,.peer=1,.epoch=1,.user=7,.frame_ms=10};
    atomic_store(&capture_epoch,wanted.epoch);
    unsigned rate=0;int state=0;
    assert(open_port(wanted,&rate,&state)<0);
    assert(high_calls==1u&&low_calls==2u&&close_calls==2u&&high_rate_rejected);
    assert(open_port(wanted,&rate,&state)<0);
    assert(high_calls==1u&&low_calls==4u&&close_calls==4u);
#if !X4_PUBLIC
    assert(open_logs==3u&&state_logs==2u&&!selected_logs);
    assert(strstr(high_line,"rate=48000 port=0 rc=0x80260103"));
    assert(strstr(state_line,"port=1 value=0x00000001"));
#else
    assert(!open_logs&&!state_logs&&!selected_logs);
#endif
    absent=false;present_port=0;
    int handle=open_port(wanted,&rate,&state);assert(handle==100&&rate==RATE_LOW&&state==0);
    assert(close_port(handle));
#if !X4_PUBLIC
    assert(state_logs==3u&&selected_logs==1u);
    assert(strstr(state_line,"port=0 value=0x00000000")&&strstr(selected_line,"selected_port=0"));
#endif
    handle=open_port(wanted,&rate,&state);assert(handle==100&&close_port(handle));
#if !X4_PUBLIC
    assert(state_logs==3u&&selected_logs==1u&&open_logs==3u);
#endif
    present_port=1;
    handle=open_port(wanted,&rate,&state);assert(handle==101&&close_port(handle));
#if !X4_PUBLIC
    assert(state_logs==5u&&selected_logs==2u);
    assert(strstr(state_line,"port=1 value=0x00000000")&&strstr(selected_line,"selected_port=1"));
#endif
    ++wanted.epoch;++wanted.peer;++wanted.generation;atomic_store(&capture_epoch,wanted.epoch);
    handle=open_port(wanted,&rate,&state);assert(handle==101&&close_port(handle));
    assert(high_calls==1u); /* The fallback survives a different session/peer. */
#if !X4_PUBLIC
    assert(state_logs==5u&&selected_logs==2u&&open_logs==3u);
#endif
}
static void retry_deadline(void)
{
    assert(RETRY_US==2000000u);virtual_clock=true;virtual_us=10000000u;
    mic.wanted.epoch=9;waits=0;spurious_wait=true;
    lock_mic();retry_wait_locked(9);unlock_mic();
    assert(virtual_us==12000000u&&waits==2u&&wait_delays[0]==2000000u&&wait_delays[1]==2000000u);
    virtual_us=20000000u;waits=0;cancel_wait=true;
    lock_mic();retry_wait_locked(9);unlock_mic();
    assert(virtual_us==20000000u&&waits==1u&&mic.wanted.epoch==10u);
    virtual_clock=false;
}
int main(void)
{
    configuration();retry_deadline();
    puts("PASS T43 mic: sticky 48kHz rejection, immediate 16kHz, absent ports, changed-only logs, exact 2s deadline/spurious wake/cancel");
    return 0;
}
