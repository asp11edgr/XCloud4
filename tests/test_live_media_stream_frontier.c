/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual media TU, real ingress and common limiter. Native pipeline, clock,
 * audio and trace are explicit doubles. No network or console result. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../src/media/live_media.c"
#pragma GCC diagnostic pop
#include <assert.h>
#include <time.h>
#include <sched.h>
#include "../src/video/stream_au_queue.h"
static uint64_t clock_us=1000;
static int submit_result;
static unsigned submits,valid_events,key_events,callback_rejects,callback_full,noted_requests;
static bool snapshot_available;
static X4StreamPipelineSnapshot pipeline_shadow;
static X4StreamAuQueue *forward_queue;
static X4LiveMedia *model_owner;
static unsigned model_arrivals,model_next_idr;

uint64_t sceKernelGetProcessTime(void){return clock_us;}
uint64_t x4_trace_now_us(void){return clock_us;}
uint64_t x4_ingress_test_now_us(void){return clock_us;}
int sceKernelUsleep(unsigned us){(void)us;abort();}
void x4_trace_record(X4Trace *t,uint16_t e,uint16_t f,uint64_t a,uint64_t b)
{(void)t;(void)f;(void)a;(void)b;if(e==X4_TRACE_AU_VALID)++valid_events;if(e==MT_KEY_REQUEST)++key_events;}
void x4_trace_monitor_critical(X4Trace *t,uint64_t n,unsigned e,unsigned s,uint32_t f,
    uint64_t i,uint64_t src,uint64_t a,uint64_t b,uint64_t c)
{(void)t;(void)n;(void)e;(void)s;(void)f;(void)i;(void)src;(void)a;(void)b;(void)c;}
void x4_trace_monitor_rx(X4Trace *t,unsigned k,bool v,uint32_t s,uint16_t q,size_t b,uint64_t n)
{(void)t;(void)k;(void)v;(void)s;(void)q;(void)b;(void)n;}
void x4_trace_monitor_state(X4Trace *t,unsigned a,unsigned p,uint64_t n)
{(void)t;(void)a;(void)p;(void)n;}
void x4_trace_poll(X4Trace *t){(void)t;}
X4NackPacket x4_rtc_nack_delivery_ticket(void){return (X4NackPacket){0};}
void x4_live_audio_receive(X4LiveAudio *a,const uint8_t *p,size_t n)
{(void)a;(void)p;(void)n;abort();}
void x4_stream_pipeline_received(X4StreamPipeline *p,size_t b,uint64_t n)
{(void)p;(void)b;(void)n;}
/* The scheduler is a double in this standalone media-frontier harness.
 * Complete its existing observation-only hook, without claiming coverage
 * of the real scheduler's post-halt diagnostics. */
void x4_stream_pipeline_note_post_halt_video(X4StreamPipeline *p,uint64_t now)
{(void)p;assert(now==clock_us);}
void x4_stream_pipeline_au_received(X4StreamPipeline *p,bool idr,uint64_t n)
{(void)p;(void)idr;(void)n;}
int x4_stream_pipeline_submit_parameters(X4StreamPipeline *p,const uint8_t *a,size_t b,
    uint64_t pts,uint64_t id,const uint8_t *s,size_t sn,const uint8_t *ps,size_t pn)
{
    (void)p;++submits;
    if(forward_queue){
        X4StreamAuInput input={.data=a,.bytes=b,.sps=s,.sps_bytes=sn,.pps=ps,.pps_bytes=pn,
            .pts_us=pts,.identity=id,.arrival_us=clock_us,.rtp_timestamp=(uint32_t)((pts*9+99)/100),
            .rtp_timestamp_valid=true,.is_idr=model_owner->idr};
        X4StreamAuOutcome out;int rc=x4_stream_au_queue_try_push(forward_queue,&input,clock_us,&out);
        if(out.request_keyframe){model_next_idr=model_arrivals+2;
            x4_keyframe_limiter_observe_gap(&model_owner->keyframe_limiter);
            x4_keyframe_limiter_request(&model_owner->keyframe_limiter,X4_KEYFRAME_AU_QUEUE);}
        return rc;
    }
    return submit_result;
}
void x4_stream_pipeline_note_keyframe_request(X4StreamPipeline *p,uint64_t now)
{(void)p;assert(now==clock_us);++noted_requests;}
void x4_stream_pipeline_note_callback_reject(X4StreamPipeline *p,bool full)
{(void)p;++callback_rejects;if(full)++callback_full;}
bool x4_stream_pipeline_try_snapshot(X4StreamPipeline *p,X4StreamPipelineSnapshot *s)
{(void)p;if(snapshot_available)*s=pipeline_shadow;return snapshot_available;}

static X4LiveMedia *fixture(void)
{
    X4LiveMedia *m=calloc(1,sizeof(*m));assert(m);
    m->au=malloc(X4_LIVE_AU_MAX);assert(m->au);
    m->sps[0]=0x67;m->pps[0]=0x68;m->sps_size=m->pps_size=1;
    atomic_init(&m->keyframe,false);atomic_init(&m->ingress_gap,false);
    atomic_init(&m->started,true);atomic_init(&m->stop,false);
    atomic_init(&m->packets,0);atomic_init(&m->dropped,0);atomic_init(&m->requests,0);
    atomic_init(&m->callback_active,0);atomic_init(&m->callback_peak,0);
    atomic_init(&m->callback_calls,0);atomic_init(&m->callback_overlap,0);
    atomic_init(&m->callback_us,0);atomic_init(&m->callback_max_us,0);
    atomic_init(&m->reserve_us,0);atomic_init(&m->reserve_max_us,0);atomic_init(&m->rx_invalid,0);
    for(unsigned i=0;i<12;++i)atomic_init(&m->callback_hist[i],0);
    atomic_flag_clear(&m->mailbox_gate);
    assert(x4_stream_mutex_init(&m->stream_mailbox,"fixture-view")==0);
    m->stream_mailbox_initialized=true;
    assert(x4_stream_wait_init(&m->ingress_wait,"fixture-ingress")==0);
    x4_keyframe_limiter_init(&m->keyframe_limiter);
    x4_keyframe_limiter_set_observer(&m->keyframe_limiter,keyframe_granted,m);
    assert(x4_video_ingress_init(&m->ingress,2)==0);
    submit_result=0;submits=valid_events=key_events=callback_rejects=callback_full=noted_requests=0;
    snapshot_available=false;memset(&pipeline_shadow,0,sizeof(pipeline_shadow));clock_us=1000;
    forward_queue=NULL;model_owner=NULL;
    return m;
}
static void cleanup(X4LiveMedia *m)
{assert(x4_stream_wait_destroy(&m->ingress_wait)==0);
 assert(x4_stream_mutex_destroy(&m->stream_mailbox)==0);
 x4_video_ingress_stop(&m->ingress);assert(x4_video_ingress_free(&m->ingress));free(m->au);free(m);}
static void au(X4LiveMedia *m,uint32_t timestamp,bool idr)
{
    uint8_t p[]={idr?0x65:0x61,0x11};
    X4LiveRtp r={.payload=p,.size=sizeof(p),.timestamp=timestamp,.marker=true};
    depacketize(m,&r,NULL);
}
static void test_only_zero_is_admitted(void)
{
    X4LiveMedia *m=fixture();
    submit_result=X4_STREAM_AU_DROPPED_FULL;au(m,90000,true);
    assert(submits==1 && m->au_submitted==0 && valid_events==0);
    assert(m->waiting_keyframe && m->video.error==0 && !m->au_open);
    assert(m->reset_counts[RESET_FEED]==0 && m->dropped_frames==1);
    submit_result=0;au(m,91500,true);
    assert(submits==2 && m->au_submitted==1 && valid_events==1 && !m->waiting_keyframe);
    assert(m->video.error==0 && m->recovery_completed==1);
    submit_result=-999;au(m,93000,false);
    assert(m->video.error==-999 && m->waiting_keyframe && m->reset_counts[RESET_FEED]==1);
    assert(m->au_submitted==1 && valid_events==1);cleanup(m);
}
static void test_wait_filter_and_heartbeat(void)
{
    X4LiveMedia *m=fixture();m->waiting_keyframe=true;
    for(unsigned i=0;i<100;++i){clock_us=1000+i*1000;au(m,90000+i*1500,false);}
    X4KeyframeLimiterSnapshot s;x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);
    assert(s.requested==0 && s.gap_generation==0 && submits==0 && m->waiting_keyframe);
    assembly_keyframe_retry(m,499999);x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);assert(s.requested==0);
    assembly_keyframe_retry(m,500000);assembly_keyframe_retry(m,500001);
    x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);assert(s.requested==1 && s.gap_generation==0);
    assembly_keyframe_retry(m,1000000);x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);
    assert(s.requested==2 && s.coalesced==1 && s.gap_generation==0);
    cleanup(m);
}
static void test_full_callback_explicit_and_deduplicated(void)
{
    X4LiveMedia *m=fixture();uint8_t packet[14]={0x80,0xe6,0,1,0,1,0,0,0,0,0,7,0x61,0x11};
    for(unsigned i=0;i<4;++i){packet[3]=(uint8_t)i;x4_live_media_receive(m,0,packet,sizeof(packet));}
    assert(atomic_load(&m->packets)==2 && atomic_load(&m->dropped)==2);
    assert(callback_rejects==2 && callback_full==2 && atomic_load(&m->ingress_gap));
    X4KeyframeLimiterSnapshot s;x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);
    assert(s.requested==1 && s.gap_generation==1);
    assert(atomic_exchange(&m->ingress_gap,false));reset_au(m,true,RESET_INGRESS,0);
    x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);assert(s.requested==1 && s.gap_generation==1);
    x4_live_media_receive(m,0,packet,sizeof(packet));
    x4_keyframe_limiter_snapshot(&m->keyframe_limiter,&s);assert(s.requested==2 && s.gap_generation==2 && callback_full==3);
    cleanup(m);
}
static void test_snapshot_and_common_grant(void)
{
    X4LiveMedia *m=fixture();m->stream_worker_shadow.decode_calls=42;
    X4LiveMediaSnapshot out;prepare_worker_view(m,&out);assert(out.video_decode_calls==42);
    pipeline_shadow.decode_calls=43;snapshot_available=true;prepare_worker_view(m,&out);
    assert(out.video_decode_calls==43 && m->stream_worker_shadow.decode_calls==43);
    assert(x4_stream_mutex_lock(&m->stream_mailbox)==0);publish_worker_view(m);
    assert(m->worker_view.video_decode_calls==0);assert(x4_stream_mutex_unlock(&m->stream_mailbox)==0);
    publish_worker_view(m);assert(m->worker_view.video_decode_calls==43);
    assert(x4_live_media_keyframe_limiter(m)==&m->keyframe_limiter);
    x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_STARTUP);
    assert(!x4_live_media_take_keyframe_request(m));
    X4KeyframeGrant grant;assert(x4_keyframe_limiter_take(&m->keyframe_limiter,clock_us,&grant));
    assert(atomic_load(&m->requests)==1 && key_events==1 && noted_requests==1);
    cleanup(m);
}
static uint32_t span_hash(const uint8_t *p,size_t n)
{uint32_t h=2166136261u;for(size_t i=0;i<n;++i)h=(h^p[i])*16777619u;return h;}
static void test_real_rtp_frontier_60au_s_decode_stalls(void)
{
    /* Real callback -> real RTP queue -> real RTP parser -> real AU assembler
     * -> real variable AU queue. Decode and time are explicit host doubles;
     * draining bypasses reorder/grace, whose unchanged contracts have their
     * separate existing integration suite. No PS4 scheduling claim. */
    X4LiveMedia *m=fixture();forward_queue=x4_stream_au_queue_create();assert(forward_queue);
    model_owner=m;model_arrivals=0;model_next_idr=0;
    const unsigned wanted=60*60;unsigned decoded=0;
    uint64_t next_arrival=0,decode_done=UINT64_MAX;
    bool held=false;X4StreamAuLease lease={0};X4StreamAuView view={0};uint32_t held_hash=0;
    while(model_arrivals<wanted || held){
        uint64_t now=model_arrivals<wanted?next_arrival:UINT64_MAX;
        if(decode_done<now)now=decode_done;
        assert(now!=UINT64_MAX);clock_us=now;
        if(held && decode_done==now){assert(span_hash(view.data,view.bytes)==held_hash);
            assert(x4_stream_au_queue_release(forward_queue,lease)==X4_STREAM_AU_OK);
            held=false;decode_done=UINT64_MAX;++decoded;}
        if(model_arrivals<wanted && next_arrival==now){
            uint8_t packet[X4_LIVE_RTP_MAX]={0};size_t bytes=40+(model_arrivals%29)*61;
            packet[0]=0x80;packet[1]=0xe6;packet[2]=(uint8_t)(model_arrivals>>8);packet[3]=(uint8_t)model_arrivals;
            uint32_t ts=model_arrivals*1500;
            for(unsigned i=0;i<4;++i)packet[4+i]=(uint8_t)(ts>>(24-8*i));
            packet[11]=7;memset(packet+12,(int)(model_arrivals%251),bytes-12);
            packet[12]=(model_arrivals==0 || model_arrivals==model_next_idr)?0x65:0x61;
            x4_live_media_receive(m,X4_LIVE_KIND_VIDEO,packet,bytes);
            assert(!atomic_load(&m->ingress_gap));
            uint8_t popped[X4_LIVE_RTP_MAX];size_t n;
            while((n=x4_video_ingress_pop(&m->ingress,popped,NULL))!=0){
                X4LiveRtp r;assert(x4_live_rtp_parse(popped,n,&r)==0);depacketize(m,&r,NULL);
            }
            ++model_arrivals;next_arrival=(uint64_t)model_arrivals*1000000/60;
        }
        if(!held){X4StreamAuOutcome out;
            X4StreamAuResult rc=x4_stream_au_queue_try_take(forward_queue,now,&lease,&view,&out);
            if(out.request_keyframe){model_next_idr=model_arrivals+1;
                x4_keyframe_limiter_observe_gap(&m->keyframe_limiter);
                x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_AU_QUEUE);}
            if(rc==X4_STREAM_AU_OK){held=true;held_hash=span_hash(view.data,view.bytes);
                decode_done=now+((decoded%23==22)?90000:20000);}
            else assert(rc==X4_STREAM_AU_EMPTY || rc==X4_STREAM_AU_DROPPED_LATE || rc==X4_STREAM_AU_DROPPED_WAIT_IDR);
        }
    }
    X4StreamAuStats q;assert(x4_stream_au_queue_try_snapshot(forward_queue,&q)==X4_STREAM_AU_OK);
    assert(model_arrivals==wanted && atomic_load(&m->packets)==wanted);
    assert(atomic_load(&m->dropped)==0 && atomic_load(&m->ingress.full)==0 && callback_full==0 && callback_rejects==0);
    assert(decoded>1500 && decoded<wanted && q.recovery_transitions>0 && q.evicted_non_idr>0);
    assert(q.resident_entries<=32 && q.resident_bytes<=8*1024*1024 && m->video.error==0);
    printf("integrated real RTP model: seconds=60 arrivals=%u decoded=%u callback_rejects=%u RTP_FULL=%llu AU_recoveries=%llu metadata_bytes=%zu\n",
        model_arrivals,decoded,callback_rejects,(unsigned long long)atomic_load(&m->ingress.full),
        (unsigned long long)q.recovery_transitions,q.metadata_bytes);
    x4_stream_au_queue_stop(forward_queue);assert(x4_stream_au_queue_destroy(forward_queue));
    forward_queue=NULL;model_owner=NULL;cleanup(m);
}
typedef struct {X4LiveMedia *media;atomic_bool ready;int result;} WaitOwner;
static void *wait_ingress(void *arg)
{
    WaitOwner *w=arg;atomic_store_explicit(&w->ready,true,memory_order_release);
    do {w->result=x4_stream_wait_block(&w->media->ingress_wait,20000);}
    while(w->result==X4_STREAM_WAIT_TIMEOUT||w->result==X4_STREAM_WAIT_EVENT);
    return NULL;
}
static uint64_t real_us(void)
{struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void test_actual_callback_notification_and_cancel(void)
{
    X4LiveMedia *m=fixture();
    assert(ingress_wait_timeout(m)==(X4_NACK_GRACE_MS?1000u:20000u));
    m->reorder.buffered=1;assert(ingress_wait_timeout(m)==1000);m->reorder.buffered=0;
    m->grace.active_count=1;assert(ingress_wait_timeout(m)==1000);m->grace.active_count=0;
    uint8_t packet[14]={0x80,0xe6,0,1,0,1,0,0,0,0,0,7,0x61,0x11};
    /* Production callback publishes before the wait begins: token survives. */
    x4_live_media_receive(m,X4_LIVE_KIND_VIDEO,packet,sizeof(packet));
    assert(x4_stream_wait_block(&m->ingress_wait,20000)==X4_STREAM_WAIT_EVENT);
    uint8_t out[14];assert(x4_video_ingress_pop(&m->ingress,out,NULL)==sizeof(packet));
    assert(!memcmp(out,packet,sizeof(packet)));
    WaitOwner w={.media=m};atomic_init(&w.ready,false);pthread_t thread;
    assert(pthread_create(&thread,NULL,wait_ingress,&w)==0);
    while(!atomic_load_explicit(&w.ready,memory_order_acquire))sched_yield();
    uint64_t began=real_us();atomic_store(&m->stop,true);assert(x4_stream_wait_close(&m->ingress_wait)==0);
    assert(pthread_join(thread,NULL)==0);uint64_t elapsed=real_us()-began;
    printf("ingress cancel observed result=%d elapsed_us=%llu\n",w.result,(unsigned long long)elapsed);
    fflush(stdout);assert(w.result==X4_STREAM_WAIT_CLOSED&&elapsed<=50000);
    X4StreamWaitStats stats;x4_stream_wait_snapshot(&m->ingress_wait,&stats);
    assert(stats.notifications==2&&stats.errors==0);
    printf("PASS actual callback retained wake + ingress cancellation %llu us; grace=%u cadence=%u us\n",
        (unsigned long long)elapsed,(unsigned)X4_NACK_GRACE_MS,ingress_wait_timeout(m));
    cleanup(m);
}
int main(void)
{
    test_only_zero_is_admitted();test_wait_filter_and_heartbeat();
    test_full_callback_explicit_and_deduplicated();test_snapshot_and_common_grant();
    test_real_rtp_frontier_60au_s_decode_stalls();
    test_actual_callback_notification_and_cancel();
    puts("PASS actual stream media frontier: positive drops are not admission, WAIT is not new loss, explicit callback-full failures, try-only snapshots and one common grant owner");
    return 0;
}
