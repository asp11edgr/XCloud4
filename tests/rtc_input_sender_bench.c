/* SPDX-License-Identifier: GPL-3.0-only */
/* Measure the linked sender's poll-to-local-send path under the same stubs
 * for the extracted T37 tree and the T33 tree. No network or PS4 calls. */
#define _POSIX_C_SOURCE 200809L
#ifdef X4_T37_TRANSPORT_SOURCE
#include X4_T37_TRANSPORT_SOURCE
#else
#include "../src/streaming/rtc_transport.c"
#endif
#include <assert.h>
#include <time.h>

enum { INPUT_CHANNEL=12, CONTROL_CHANNEL=11, WARMUP=20000, SAMPLES=10000 };
static uint64_t sample_ns[SAMPLES], now_us, trial_start;
static bool begin_trial,multiplayer;
static unsigned sent, pads;
static volatile uint8_t sink;
static X4Rtc *running;

static uint64_t ticks_ns(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC_RAW,&ts)==0);
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}

uint64_t sceKernelGetProcessTime(void)
{
    if(begin_trial) {trial_start=ticks_ns();begin_trial=false;}
    return now_us;
}

int sceKernelUsleep(unsigned us)
{
    assert(us>0);now_us+=us;
    if(sent>=WARMUP+SAMPLES)atomic_store(&running->input_stop,true);
    else begin_trial=true;
    return 0;
}

OrbisPthread scePthreadSelf(void){return (OrbisPthread)1;}
int32_t scePthreadSetprio(OrbisPthread thread,int priority)
{assert(thread==(OrbisPthread)1 && priority==400);return 0;}
bool rtcIsOpen(int channel){return channel==INPUT_CHANNEL || channel==CONTROL_CHANNEL;}
int rtcGetBufferedAmount(int channel){assert(channel==INPUT_CHANNEL);return 0;}
int rtcSendMessage(int channel,const char *data,int size)
{
    if(channel==CONTROL_CHANNEL){assert(size==-1);return 0;}
    assert(channel==INPUT_CHANNEL && size==15+23*(int)pads && sent<WARMUP+SAMPLES);
    sink^=(uint8_t)data[size-1];
    if(sent>=WARMUP)sample_ns[sent-WARMUP]=ticks_ns()-trial_start;
    ++sent;
    return 0;
}

#ifdef X4_T37_TRANSPORT_SOURCE
static bool source(void *user,X4GamepadFrame *out)
{
    assert(user==running);
    *out=(X4GamepadFrame){.connected=true,.buttons=X4_GAMEPAD_A,
        .left_x=(int16_t)now_us,.sampled_us=now_us};
    return true;
}
#else
static bool source(void *user,X4GamepadRoster *out)
{
    assert(user==running);
    out->multiplayer=multiplayer;
    out->connected_mask=(1u<<pads)-1u;
    out->changed_us[0]=out->generation[0]=0;
    if(multiplayer){
        memset(out->changed_us,0,sizeof(out->changed_us));
        memset(out->generation,0,sizeof(out->generation));
    }
    for(unsigned i=0;i<pads;++i)
        out->pads[i]=(X4GamepadFrame){.connected=true,.buttons=(uint16_t)(X4_GAMEPAD_A<<i),
            .left_x=(int16_t)(now_us+i),.sampled_us=now_us};
    return true;
}
#endif

static int compare_ticks(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}

int main(int argc,char **argv)
{
    assert((argc==2 || argc==3) && (!strcmp(argv[1],"1") || !strcmp(argv[1],"4")));
    pads=(unsigned)(argv[1][0]-'0');
    if(argc==3)assert(pads==1 && !strcmp(argv[2],"off"));
    multiplayer=argc==2;
#ifdef X4_T37_TRANSPORT_SOURCE
    assert(pads==1);
    multiplayer=false;
    const char *label="T37";
#else
    const char *label="T33";
#endif
    X4Rtc rtc={0};running=&rtc;now_us=1000000;begin_trial=true;
    atomic_flag_clear(&rtc.data_gate);
    atomic_init(&rtc.state,X4_RTC_CONNECTED);atomic_init(&rtc.error,0);
    atomic_init(&rtc.input_stop,false);atomic_init(&rtc.input_ready,false);
    atomic_init(&rtc.input_metadata_sent,true);atomic_init(&rtc.startup_complete,true);
    atomic_init(&rtc.input_packets,0);atomic_init(&rtc.input_dropped,0);
    atomic_init(&rtc.input_error,0);
    rtc.channels[1]=CONTROL_CHANNEL;rtc.channels[2]=INPUT_CHANNEL;
    rtc.started_at=now_us;rtc.input_source=source;rtc.input_user=&rtc;
    rtc.request_config.interaction_modes=X4_INTERACTION_INPUT_60HZ;
    input_sender(&rtc);
    assert(sent==WARMUP+SAMPLES && atomic_load(&rtc.input_packets)==sent);
    qsort(sample_ns,SAMPLES,sizeof(sample_ns[0]),compare_ticks);
    printf("T33_BENCH variant=%s pads=%u p50_ns=%llu p95_ns=%llu samples=%u multiplayer=%u scope=poll_to_local_send_stub\n",
        label,pads,(unsigned long long)sample_ns[SAMPLES/2u],
        (unsigned long long)sample_ns[(SAMPLES*95u+99u)/100u-1u],(unsigned)SAMPLES,multiplayer?1u:0u);
    return 0;
}
