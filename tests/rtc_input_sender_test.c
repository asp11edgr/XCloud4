/* SPDX-License-Identifier: GPL-3.0-only */
/* Real sender and wire encoder, deterministic source/clock/native channel.
 * No packets leave the process; sleeps only advance the simulated clock. */
#define _POSIX_C_SOURCE 200809L
#include "../src/streaming/rtc_transport.c"
#include <assert.h>
#include <limits.h>
#include <pthread.h>

enum { CONTROL_CHANNEL=11, INPUT_CHANNEL=12, MAX_SENDS=2200, MAX_CONTROLS=16 };
typedef struct { uint64_t at; int rc, size; uint8_t bytes[INPUT_GAMEPAD_MAX]; } Attempt;
typedef struct { uint64_t at; unsigned index; bool added; int rc; } ControlAttempt;
static ControlAttempt controls[MAX_CONTROLS];
static unsigned control_attempts, fail_control_attempt;
static atomic_uint producer_frames[X4_GAMEPAD_COUNT];
static Attempt attempts[MAX_SENDS];
static unsigned attempted, priority_calls, source_polls, buffer_polls;
static uint64_t now_us, until_us, origin_us, blocked_until, send_cost;
static int priority_result, fail_attempt, buffer_result;
static unsigned scenario;
static X4Rtc *running;
static const uint64_t ORIGIN=1000000u;

uint64_t sceKernelGetProcessTime(void){return now_us;}
int sceKernelUsleep(unsigned us)
{
    assert(us>0);now_us+=us;
    if(now_us>=until_us)atomic_store(&running->input_stop,true);
    return 0;
}
OrbisPthread scePthreadSelf(void){return (OrbisPthread)1;}
int32_t scePthreadSetprio(OrbisPthread thread,int priority)
{assert(thread==(OrbisPthread)1 && priority==400);++priority_calls;return priority_result;}
bool rtcIsOpen(int channel){assert(channel==INPUT_CHANNEL || channel==CONTROL_CHANNEL);return true;}
int rtcGetBufferedAmount(int channel)
{assert(channel==INPUT_CHANNEL);++buffer_polls;return now_us<blocked_until?buffer_result:0;}
int rtcSendMessage(int channel,const char *data,int size)
{
    if(channel==CONTROL_CHANNEL) {
        assert(size==-1 && control_attempts<MAX_CONTROLS);
        unsigned index;char value[6]={0};int consumed=0;
        assert(sscanf(data,"{\"message\":\"gamepadChanged\",\"gamepadIndex\":%u,\"wasAdded\":%5[a-z]}%n",
            &index,value,&consumed)==2 && (size_t)consumed==strlen(data));
        assert(index<4 && (!strcmp(value,"true") || !strcmp(value,"false")));
        int rc=control_attempts+1==fail_control_attempt?-76:0;
        controls[control_attempts++]=(ControlAttempt){now_us,index,!strcmp(value,"true"),rc};
        return rc;
    }
    assert(channel==INPUT_CHANNEL && size>=38 && size<=INPUT_GAMEPAD_MAX && attempted<MAX_SENDS);
    assert(size==15+23*(uint8_t)data[14]);
    Attempt *a=&attempts[attempted++];a->at=now_us;a->size=size;
    memcpy(a->bytes,data,(size_t)size);a->rc=(int)attempted==fail_attempt?-97:0;
    now_us+=send_cost;return a->rc;
}
static bool source(void *user,X4GamepadRoster *roster)
{
    assert(user==running);++source_polls;
    *roster=(X4GamepadRoster){.connected_mask=1};
    X4GamepadFrame *out=&roster->pads[0];
    *out=(X4GamepadFrame){.connected=true,.sampled_us=origin_us};
    uint64_t elapsed=now_us-origin_us;
    switch(scenario) {
    case 1:if(elapsed>=1001){out->buttons=X4_GAMEPAD_A;out->sampled_us=origin_us+1001;}break;
    case 2:
        if(elapsed>=1001){out->buttons=X4_GAMEPAD_A;out->sampled_us=origin_us+1001;}
        if(elapsed>=3001){out->buttons=X4_GAMEPAD_B;out->sampled_us=origin_us+3001;}break;
    case 3:
        out->buttons=elapsed>=2000?1:0;
        out->left_x=elapsed>=2000?-32767:INT16_MIN;break;
    case 4:out->left_x=(int16_t)(elapsed/2000);out->sampled_us=now_us;break;
    case 5:
        if(elapsed>=5000){out->buttons=X4_GAMEPAD_Y;out->sampled_us=origin_us+5000;}break;
    case 6:
        if(elapsed>=2001){out->buttons=X4_GAMEPAD_X;out->sampled_us=origin_us+2001;}break;
    case 7:if(elapsed>=2000){out->connected=false;out->sampled_us=0;roster->connected_mask=0;}break;
    case 9:out->sampled_us=now_us+1;break;
    case 10:return false;
    case 11:out->sampled_us=now_us;break;
    case 8:
        out->buttons=(uint16_t)(0xff00u|((elapsed/1000u)&0xfeu));
        out->left_x=(int16_t)(elapsed%65535u-32767);out->left_y=INT16_MIN;
        out->right_x=2;out->right_y=-3;out->left_trigger=123;out->right_trigger=65432;
        out->sampled_us=now_us;break;
    case 12: case 13: case 14: case 15: case 16:
        roster->multiplayer=scenario!=12;
        roster->connected_mask=scenario==15 ? 0xff : scenario==12 ? 15 : 3;
        for(unsigned i=1;i<X4_GAMEPAD_COUNT;++i) {
            roster->pads[i]=(X4GamepadFrame){.connected=true,.buttons=(uint16_t)(X4_GAMEPAD_A<<i),
                .sampled_us=now_us};
            roster->changed_us[i]=origin_us;
            roster->generation[i]=1;
        }
        if(scenario==13) {
            if(elapsed>=500000u && elapsed<5500000u) {
                roster->connected_mask&=~1u;out->connected=false;
                roster->changed_us[0]=origin_us+500000u;roster->generation[0]=2;
            } else if(elapsed>=5500000u) {
                roster->changed_us[0]=origin_us+5500000u;roster->generation[0]=3;
            }
            if(elapsed>=100000u && elapsed<1100000u) {
                roster->connected_mask&=~2u;roster->changed_us[1]=origin_us+100000u;roster->generation[1]=2;
            } else if(elapsed>=1100000u && elapsed<1200000u) {
                roster->changed_us[1]=origin_us+1100000u;roster->generation[1]=3;
            } else if(elapsed>=1200000u && elapsed<3300000u) {
                roster->connected_mask&=~2u;roster->changed_us[1]=origin_us+1200000u;roster->generation[1]=4;
            } else if(elapsed>=3300000u) {
                roster->changed_us[1]=origin_us+3300000u;roster->generation[1]=5;
            }
        }
        if(scenario==14 && elapsed>=10000u && elapsed<2500000u)return false;
        if(scenario==15)for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i)
            roster->pads[i].left_x=(int16_t)atomic_load_explicit(&producer_frames[i],memory_order_relaxed);
        if(scenario==16 && elapsed>=10000u) {
            roster->connected_mask&=~2u;
            roster->changed_us[1]=origin_us+10000u;roster->generation[1]=2;
            /* Replug and unplug in one poll interval. The latest generation
             * and timestamp must still move the removal deadline. */
            if(elapsed>=1900000u) {
                roster->changed_us[1]=origin_us+1899990u;roster->generation[1]=4;
            }
        }
        break;
    default:break;
    }
    return true;
}
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static void fixture(X4Rtc *rtc,bool legacy,unsigned which,uint64_t duration)
{
    memset(rtc,0,sizeof(*rtc));memset(attempts,0,sizeof(attempts));
    now_us=origin_us=ORIGIN;until_us=origin_us+duration;scenario=which;
    attempted=priority_calls=source_polls=buffer_polls=control_attempts=fail_control_attempt=0;
    blocked_until=0;send_cost=0;priority_result=0;fail_attempt=0;buffer_result=38;running=rtc;
    atomic_flag_clear(&rtc->data_gate);atomic_init(&rtc->state,X4_RTC_CONNECTED);
    atomic_init(&rtc->error,0);atomic_init(&rtc->input_stop,false);
    atomic_init(&rtc->input_ready,false);atomic_init(&rtc->input_metadata_sent,true);
    atomic_init(&rtc->startup_complete,true);atomic_init(&rtc->input_packets,0);
    atomic_init(&rtc->input_dropped,0);atomic_init(&rtc->input_error,0);
    rtc->channels[1]=CONTROL_CHANNEL;rtc->channels[2]=INPUT_CHANNEL;rtc->started_at=origin_us;
    rtc->input_source=source;rtc->input_user=rtc;
    rtc->request_config=x4_stream_request_config(X4_REQUEST_1080P_G,X4_COLOR_AUTO_709);
    rtc->request_config.interaction_modes=legacy?X4_INTERACTION_INPUT_60HZ:0;
    x4_rtc_input_metrics_reset();
}

/* Original one-pad wire block from XCloud4 T37, independent of the new
 * multi-pad block writer. The unchanged shared input_header is tested with
 * explicit byte fixtures in the phase-A comparison against the T37 archive. */
static void input_gamepad_t37(X4Rtc *rtc, const X4GamepadFrame *frame, uint8_t packet[38])
{
    input_header(rtc,packet,2);
    packet[14]=1;packet[15]=0;
    input_u16(packet+16,frame->buttons & 0xfffeu);
    int16_t axes[]={frame->left_x,frame->left_y,frame->right_x,frame->right_y};
    for(unsigned i=0;i<4;++i) {
        if(axes[i]<-32767)axes[i]=-32767;
        input_u16(packet+18+2*i,(uint16_t)axes[i]);
    }
    input_u16(packet+26,frame->left_trigger);input_u16(packet+28,frame->right_trigger);
    input_u32(packet+30,1);
    packet[34]=packet[35]=packet[36]=0;packet[37]=1;
}

/* Byte-for-byte scheduling reference copied from this project's 0.7.47.
 * No reference implementation of the new scheduler is substituted. */
static void *input_sender_0747_reference(void *pointer)
{
    X4Rtc *rtc=pointer;
    while(!atomic_load(&rtc->input_stop)) {
        uint64_t until=sceKernelGetProcessTime()+INPUT_INTERVAL_USEC;
        lock(&rtc->data_gate);
        X4GamepadSource source=rtc->input_source;
        void *user=rtc->input_user;
        unlock(&rtc->data_gate);
        bool ready=source && atomic_load(&rtc->input_metadata_sent) &&
            atomic_load(&rtc->startup_complete) && !atomic_load(&rtc->error) &&
            atomic_load(&rtc->state)==X4_RTC_CONNECTED && rtcIsOpen(rtc->channels[2]);
        atomic_store(&rtc->input_ready,ready);
        if(ready) {
            X4GamepadRoster roster={0};
            bool fresh=source(user,&roster);
            X4GamepadFrame frame=roster.pads[0];
            if(!fresh || !frame.connected)frame=(X4GamepadFrame){0};
            if(!atomic_load(&rtc->input_stop)) {
                int buffered=rtcGetBufferedAmount(rtc->channels[2]);
                if(buffered) {
                    atomic_fetch_add(&rtc->input_dropped,1);
                    if(buffered<0)input_error(rtc,buffered);
                } else {
                    uint8_t packet[38];
                    input_gamepad_t37(rtc,&frame,packet);
                    int rc=rtcSendMessage(rtc->channels[2],(const char *)packet,sizeof(packet));
                    if(rc<0) {atomic_fetch_add(&rtc->input_dropped,1);input_error(rtc,rc);}
                    else {atomic_fetch_add(&rtc->input_packets,1);input_error(rtc,0);}
                }
            }
        }
        /* No catch-up bursts: each iteration starts a fresh monotonic
         * deadline; interrupted sleeps cannot exceed the 60 Hz ceiling. */
        while(!atomic_load(&rtc->input_stop)) {
            uint64_t t=sceKernelGetProcessTime();
            if(t>=until)break;
            sceKernelUsleep((unsigned)(until-t));
        }
    }
    atomic_store(&rtc->input_ready,false);
    return NULL;
}

static void changed_and_latest(void)
{
    X4Rtc rtc;fixture(&rtc,false,1,12000);input_sender(&rtc);
    assert(priority_calls==1 && source_polls==6 && attempted==2);
    assert(attempts[0].at==ORIGIN && attempts[1].at==ORIGIN+4000);
    assert(attempts[1].at-(ORIGIN+1001)<=4000);
    assert(u16(attempts[1].bytes+16)==X4_GAMEPAD_A);
    assert(input_metrics.changes==2 && input_metrics.keepalives==0);
    assert(input_metrics.cap_delays==1 && input_metrics.state_age.count==2);
    fixture(&rtc,false,2,12000);input_sender(&rtc);
    assert(attempted==2 && u16(attempts[1].bytes+16)==X4_GAMEPAD_B);
    assert(input_metrics.changes==2); /* The older A change was not queued. */
}
static void mapped_normalization_and_connected(void)
{
    X4Rtc rtc;fixture(&rtc,false,3,12000);input_sender(&rtc);
    assert(attempted==1 && input_metrics.changes==1);
    assert(u16(attempts[0].bytes+18)==(uint16_t)-32767);
    fixture(&rtc,false,7,12000);input_sender(&rtc);
    assert(attempted==2 && attempts[1].at==ORIGIN+4000);
    assert(input_metrics.changes==2 && input_metrics.age_unavailable==1);
}
static void keepalive(void)
{
    X4Rtc rtc;fixture(&rtc,false,0,100000);input_sender(&rtc);
    assert(attempted==6 && input_metrics.changes==1 && input_metrics.keepalives==5);
    for(unsigned i=1;i<attempted;++i) {
        uint64_t gap=attempts[i].at-attempts[i-1].at;
        assert(gap>=16667 && gap<=18667);
    }
    /* Physical samples remain fresh every poll, but the mapped report is
     * unchanged. sampled_us must not create change-triggered sends. */
    fixture(&rtc,false,11,100000);input_sender(&rtc);
    assert(source_polls==50 && attempted==6 && input_metrics.changes==1 && input_metrics.keepalives==5);
    for(unsigned i=1;i<attempted;++i)assert(attempts[i].at-attempts[i-1].at>=16667);
}
static void backlog_retry_and_send_failure(void)
{
    X4Rtc rtc;fixture(&rtc,false,5,16000);blocked_until=ORIGIN+7000;input_sender(&rtc);
    assert(attempted==1 && attempts[0].at==ORIGIN+8000);
    assert(u16(attempts[0].bytes+16)==X4_GAMEPAD_Y);
    assert(input_metrics.busy_deferrals==4 && input_metrics.successful==1 && input_metrics.buffer_query_errors==0);
    fixture(&rtc,false,5,16000);blocked_until=ORIGIN+7000;buffer_result=-77;input_sender(&rtc);
    assert(attempted==1 && attempts[0].at==ORIGIN+8000 && u16(attempts[0].bytes+16)==X4_GAMEPAD_Y);
    assert(input_metrics.busy_deferrals==0 && input_metrics.buffer_query_errors==4 && input_metrics.successful==1);
    assert(atomic_load(&rtc.input_dropped)==4 && atomic_load(&rtc.input_error)==0);
    fixture(&rtc,false,6,16000);fail_attempt=1;input_sender(&rtc);
    assert(attempted==2 && attempts[0].rc==-97 && attempts[1].at==ORIGIN+4000);
    assert(u16(attempts[1].bytes+16)==X4_GAMEPAD_X);
    assert(input_metrics.successful==1 && input_metrics.send_errors==1);
    assert(input_metrics.changes==1 && atomic_load(&rtc.input_error)==0);
}
static void continuous_cap_and_nonfatal_priority(void)
{
    X4Rtc rtc;fixture(&rtc,false,4,1000000);priority_result=-55;send_cost=150;input_sender(&rtc);
    assert(attempted==250 && input_metrics.successful==250 && input_metrics.maximum_per_second==250);
    assert(input_metrics.priority_return==-55 && priority_calls==1 && atomic_load(&rtc.error)==0);
    for(unsigned i=1;i<attempted;++i)assert(attempts[i].at-attempts[i-1].at>=4000);
    assert(input_metrics.send_duration.count==250 && input_metrics.send_duration.maximum_us==150);
}
static void legacy_identical(void)
{
    X4Rtc rtc;
    static Attempt old[MAX_SENDS];
    const unsigned scenarios[]={8,7,10,12};
    for(size_t test=0;test<sizeof(scenarios)/sizeof(scenarios[0]);++test) {
        unsigned which=scenarios[test];
        fixture(&rtc,true,which,120000);blocked_until=ORIGIN+17000;fail_attempt=2;send_cost=150;
        input_sender_0747_reference(&rtc);
        unsigned n=attempted;memcpy(old,attempts,sizeof(old));
        uint64_t dropped=atomic_load(&rtc.input_dropped),packets=atomic_load(&rtc.input_packets);
        fixture(&rtc,true,which,120000);blocked_until=ORIGIN+17000;fail_attempt=2;send_cost=150;
        input_sender(&rtc);
        assert(attempted==n && !control_attempts &&
            atomic_load(&rtc.input_dropped)==dropped && atomic_load(&rtc.input_packets)==packets);
        for(unsigned i=0;i<n;++i)assert(old[i].at==attempts[i].at && old[i].rc==attempts[i].rc &&
            attempts[i].size==38 && !memcmp(old[i].bytes,attempts[i].bytes,38));
        assert(input_metrics.legacy && input_metrics.successful==packets && priority_calls==1);
    }
}

static void metrics_scope_and_live_guard(void)
{
    X4Rtc rtc;fixture(&rtc,false,0,22000);input_sender(&rtc);
    FILE *f=tmpfile();assert(f);x4_rtc_input_metrics_write(f);rewind(f);
    char text[4096];size_t bytes=fread(text,1,sizeof(text)-1,f);text[bytes]=0;fclose(f);
    assert(strstr(text,"\"available\":true") && strstr(text,"\"mode\":\"immediate\""));
    assert(strstr(text,"\"priority_requested\":400") && strstr(text,"\"state_age\":{"));
    assert(strstr(text,"\"p50_upper_us\":") && strstr(text,"\"send_duration\":{"));
    x4_rtc_input_metrics_reset();assert(!input_metrics.available);
    assert(input_metrics_begin(false,0));input_metrics.successful=7;
    x4_rtc_input_metrics_reset();assert(input_metrics.successful==7);
    f=tmpfile();assert(f);x4_rtc_input_metrics_write(f);rewind(f);
    bytes=fread(text,1,sizeof(text)-1,f);text[bytes]=0;fclose(f);
    assert(strstr(text,"sender_not_joined"));input_metrics_end(true);
    assert(input_metrics.successful==7);
    fixture(&rtc,false,9,22000);input_sender(&rtc);
    assert(input_metrics.state_age.count==0 && input_metrics.age_unavailable==attempted);
    fixture(&rtc,false,10,22000);input_sender(&rtc);
    assert(input_metrics.state_age.count==0 && input_metrics.age_unavailable==attempted);
}
static void rumble_snapshot_only(void)
{
    X4Rtc rtc;fixture(&rtc,false,0,22000);
    X4Rumble *rumble=x4_rumble_new();assert(rumble && x4_rumble_begin(rumble));
    uint8_t report[13]={128,0,0,0,100,50,7,9,100,0,30,0,2};
    assert(x4_rumble_receive(rumble,report,sizeof(report),now_us)==X4_RUMBLE_VALID);
    assert(x4_rumble_receive(rumble,report,3,now_us)==X4_RUMBLE_MALFORMED);
    rtc.rumble=rumble;input_sender(&rtc);
    assert(input_metrics.rumble_available && input_metrics.rumble.valid==1 &&
        input_metrics.rumble.malformed==1 && input_metrics.rumble.binary_messages==2);
    FILE *f=tmpfile();assert(f);x4_rtc_input_metrics_write(f);rewind(f);
    char text[4096];size_t bytes=fread(text,1,sizeof(text)-1,f);text[bytes]=0;fclose(f);
    assert(strstr(text,"\"send_success_scope\":\"local rtcSendMessage") &&
        strstr(text,"\"rumble_available\":true") && strstr(text,"\"malformed\":1"));
    x4_rumble_disable(rumble);assert(x4_rumble_free(rumble));
}
static const uint8_t *block(const Attempt *attempt,unsigned index)
{
    for(unsigned i=0;i<attempt->bytes[14];++i) {
        const uint8_t *p=attempt->bytes+15u+i*23u;
        if(p[0]==index)return p;
    }
    return NULL;
}

static bool neutral_block(const uint8_t *p)
{
    if(!p)return false;
    for(unsigned i=1;i<15;++i)if(p[i])return false;
    return true;
}

static void multiplayer_debounce(void)
{
    X4Rtc rtc;fixture(&rtc,false,13,5600000u);input_sender(&rtc);
    assert(control_attempts==3);
    assert(controls[0].index==1 && controls[0].added && controls[0].at==ORIGIN);
    assert(controls[1].index==1 && !controls[1].added && controls[1].at==ORIGIN+3200000u);
    assert(controls[2].index==1 && controls[2].added && controls[2].at==ORIGIN+3300000u);
    bool short_neutral=false,long_neutral=false,removed=false,returned=false;
    for(unsigned i=0;i<attempted;++i) {
        const Attempt *a=&attempts[i];
        uint64_t t=a->at-ORIGIN;
        const uint8_t *p0=block(a,0),*p1=block(a,1);
        assert(p0);
        if(t>=500000u && t<5500000u)assert(neutral_block(p0));
        if(t>=100000u && t<1100000u) {
            assert(a->size==61 && neutral_block(p1));short_neutral=true;
        }
        if(t>=1200000u && t<3200000u) {
            assert(a->size==61 && neutral_block(p1));long_neutral=true;
        }
        if(t>=3200000u && t<3300000u){assert(a->size==38 && !p1);removed=true;}
        if(t>=3300000u){assert(a->size==61 && p1);returned=true;}
    }
    assert(short_neutral && long_neutral && removed && returned);
    fixture(&rtc,false,16,3950000u);input_sender(&rtc);
    assert(control_attempts==2 && !controls[1].added && controls[1].at==ORIGIN+3900000u);
    fixture(&rtc,false,14,2600000u);input_sender(&rtc);
    assert(control_attempts==1 && controls[0].added);
    for(unsigned i=0;i<attempted;++i) {
        const Attempt *a=&attempts[i];
        assert(a->size==61);
        if(a->at>=ORIGIN+10000u && a->at<ORIGIN+2500000u)
            assert(neutral_block(block(a,0)) && neutral_block(block(a,1)));
    }
    fixture(&rtc,false,13,20000u);fail_control_attempt=1;input_sender(&rtc);
    assert(control_attempts==2 && controls[0].rc==-76 && controls[1].rc==0 &&
        controls[1].index==1 && controls[1].added);
    assert(attempts[0].size==38 && attempts[1].size==61);
    puts("RTC multiplayer: permanent neutral P1, 1s cut, 2s debounce, stable readd, generation and stale guards PASS");
}

static void *producer(void *user)
{
    unsigned i=*(const unsigned *)user;
    for(unsigned n=0;n<70000u;++n)
        atomic_store_explicit(&producer_frames[i],n+i,memory_order_relaxed);
    return NULL;
}

static void four_concurrent_sources(void)
{
    X4Rtc rtc;fixture(&rtc,false,15,800000u);
    pthread_t workers[X4_GAMEPAD_COUNT];
    unsigned ids[X4_GAMEPAD_COUNT];
    for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i) {
        ids[i]=i;atomic_init(&producer_frames[i],i);
        assert(!pthread_create(&workers[i],NULL,producer,&ids[i]));
    }
    input_sender(&rtc);
    for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i)assert(!pthread_join(workers[i],NULL));
    assert(control_attempts==3 && attempted);
    for(unsigned i=0;i<3;++i)assert(controls[i].index==i+1 && controls[i].added);
    for(unsigned i=0;i<attempted;++i) {
        const Attempt *a=&attempts[i];
        assert(a->size==107 && a->bytes[14]==4);
        for(unsigned j=0;j<4;++j) {
            const uint8_t *p=a->bytes+15u+j*23u;
            assert(p[0]==j && p[15]==1 && p[16]==0 && p[17]==0 && p[18]==0 &&
                p[19]==0 && p[20]==0 && p[21]==0 && p[22]==1);
        }
    }
    puts("RTC multiplayer: four concurrent sources, one report per send and four indexed blocks PASS");
}

int main(void)
{
    changed_and_latest();mapped_normalization_and_connected();keepalive();
    backlog_retry_and_send_failure();continuous_cap_and_nonfatal_priority();
    legacy_identical();metrics_scope_and_live_guard();rumble_snapshot_only();
    multiplayer_debounce();four_concurrent_sources();
    puts("RTC input sender: change<=4ms, keepalive, retry, normalized state, cap, legacy golden and metrics PASS");
    return 0;
}
