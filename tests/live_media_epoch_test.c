/* SPDX-License-Identifier: GPL-3.0-only */
/* Real media/audio reception, ingress/reorder and common limiter; monotonic
 * host clock. Native pipeline/render and trace use narrow doubles. No PS4. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../src/media/live_media.c"
#pragma GCC diagnostic pop
#include <assert.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include "../src/audio/live_audio.c"
#include "../src/video/stream_au_queue.h"
static atomic_uint_fast64_t accepted_source,accepted_audio_source,accepted_aus;
static atomic_bool owner_stop,noise_stop;
static atomic_uint noise_source;
static unsigned valid_events,key_events,callback_rejects,callback_full,noted_requests;
static bool snapshot_available;
static X4StreamPipelineSnapshot pipeline_shadow;

uint64_t sceKernelGetProcessTime(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;}
uint64_t x4_trace_now_us(void){return sceKernelGetProcessTime();}
uint64_t x4_ingress_test_now_us(void){return sceKernelGetProcessTime();}
int sceKernelUsleep(unsigned us){struct timespec t={us/1000000u,(long)(us%1000000u)*1000};return nanosleep(&t,NULL);}
void x4_trace_record(X4Trace *t,uint16_t e,uint16_t f,uint64_t a,uint64_t b)
{(void)t;(void)f;(void)a;(void)b;if(e==X4_TRACE_AU_VALID)++valid_events;if(e==MT_KEY_REQUEST)++key_events;}
void x4_trace_monitor_critical(X4Trace *t,uint64_t n,unsigned e,unsigned s,uint32_t f,
    uint64_t i,uint64_t src,uint64_t a,uint64_t b,uint64_t c)
{(void)t;(void)n;(void)e;(void)s;(void)f;(void)i;(void)src;(void)a;(void)b;(void)c;}
void x4_trace_monitor_rx(X4Trace *t,unsigned k,bool v,uint32_t s,uint16_t q,size_t b,uint64_t n)
{(void)t;(void)k;(void)v;(void)s;(void)q;(void)b;(void)n;}
void x4_trace_monitor_state(X4Trace *t,unsigned a,unsigned p,uint64_t n)
{(void)t;(void)a;(void)p;(void)n;}
void x4_trace_monitor_queue(X4Trace *t,uint32_t d,uint64_t a,bool v){(void)t;(void)d;(void)a;(void)v;}
void x4_trace_monitor_epoch(X4Trace *t,unsigned a,unsigned e){(void)t;(void)a;(void)e;}
void x4_trace_poll(X4Trace *t){(void)t;}
X4NackPacket x4_rtc_nack_delivery_ticket(void){return (X4NackPacket){0};}
void x4_stream_pipeline_received(X4StreamPipeline *p,size_t b,uint64_t n)
{(void)p;(void)b;(void)n;}
/* The scheduler is a double in this standalone media-frontier harness.
 * Complete its existing observation-only hook, without claiming coverage
 * of the real scheduler's post-halt diagnostics. */
void x4_stream_pipeline_note_post_halt_video(X4StreamPipeline *p,uint64_t now)
{(void)p;(void)now;}
void x4_stream_pipeline_au_received(X4StreamPipeline *p,bool idr,uint64_t n)
{(void)p;(void)idr;(void)n;}
/* Backend stop would be unexpected in this reception-only epoch fixture. */
void x4_stream_pipeline_request_stop(X4StreamPipeline *p)
{(void)p;assert(!"unexpected native pipeline stop during receive epoch");}
void x4_stream_pipeline_rtp_gap(X4StreamPipeline *p,uint64_t now)
{(void)p;(void)now;} /* Observation only; no native pipeline in this fixture. */
int x4_stream_pipeline_submit_parameters(X4StreamPipeline *p,const uint8_t *a,size_t b,
    uint64_t pts,uint64_t id,const uint8_t *s,size_t sn,const uint8_t *ps,size_t pn)
{
    (void)p;(void)a;(void)b;(void)pts;(void)id;(void)s;(void)sn;(void)ps;(void)pn;atomic_fetch_add(&accepted_aus,1);return 0;
}
void x4_stream_pipeline_note_keyframe_request(X4StreamPipeline *p,uint64_t now)
{(void)p;(void)now;++noted_requests;}
void x4_stream_pipeline_note_callback_reject(X4StreamPipeline *p,bool full)
{(void)p;++callback_rejects;if(full)++callback_full;}
bool x4_stream_pipeline_try_snapshot(X4StreamPipeline *p,X4StreamPipelineSnapshot *s)
{(void)p;if(snapshot_available)*s=pipeline_shadow;return snapshot_available;}


int x4_stream_pipeline_begin_receive_epoch(X4StreamPipeline *p)
{(void)p;return 0;} /* Native decoder/display are not part of this RTP test. */
static void packet(uint8_t out[14],uint32_t ssrc,unsigned pt,unsigned seq,unsigned ts,unsigned nal)
{
    memset(out,0,14);out[0]=0x80;out[1]=(uint8_t)(0x80|pt);
    out[2]=(uint8_t)(seq>>8);out[3]=(uint8_t)seq;
    for(unsigned i=0;i<4;++i){out[4+i]=(uint8_t)(ts>>(24-8*i));out[8+i]=(uint8_t)(ssrc>>(24-8*i));}
    out[12]=(uint8_t)nal;out[13]=0x80;
}
static X4LiveMedia *fixture(void)
{
    X4LiveMedia *m=calloc(1,sizeof(*m));assert(m);m->au=malloc(X4_LIVE_AU_MAX);assert(m->au);
    x4_live_track_init(&m->track,102);m->waiting_keyframe=true;
    assert(!x4_video_ingress_init(&m->ingress,256));
    assert(!x4_live_reorder_init_video(&m->reorder,128,32,25000,m->reorder_tickets));
    x4_nack_grace_inbox_init(&m->grace_inbox);x4_nack_grace_init(&m->grace,1,false);
    x4_nack_binding_init(&m->grace_binding,0,0);x4_keyframe_limiter_init(&m->keyframe_limiter);
    atomic_init(&m->started,true);atomic_init(&m->stop,false);
    atomic_init(&m->receive_epoch_hold,false);atomic_init(&m->receive_epoch_callbacks,0);
    atomic_init(&m->receive_epoch_requested,0);atomic_init(&m->receive_epoch_applied,0);
    atomic_init(&m->retired_video_source,0);atomic_init(&m->receive_epoch_rejected,0);
    atomic_init(&m->receive_epoch_result,0);
    assert(!x4_stream_wait_init(&m->ingress_wait,"epoch-test"));
    X4LiveAudio *a=calloc(1,sizeof(*a));assert(a);m->audio=a;
    assert(!x4_live_ring_init(&a->ring,96));assert(!x4_live_reorder_init(&a->reorder,64,8,30000));
    x4_live_track_init(&a->track,111);atomic_init(&a->stop,false);atomic_init(&a->error,0);
    atomic_init(&a->epoch_hold,false);atomic_init(&a->epoch_callbacks,0);
    atomic_init(&a->epoch_requested,0);atomic_init(&a->epoch_applied,0);atomic_init(&a->retired_source,0);
    return m;
}
static void *owner(void *opaque)
{
    X4LiveMedia *m=opaque;
    while(!atomic_load(&owner_stop)){
        receive_epoch_service(m);audio_epoch_service(m->audio);
        if(!atomic_load(&m->receive_epoch_hold)){
            video_batch(m);
            if(m->track.locked)atomic_store_explicit(&accepted_source,m->track.ssrc,memory_order_release);
            uint8_t bytes[X4_LIVE_RTP_MAX];X4LiveRtp r;
            while(x4_live_ring_pop(&m->audio->ring,bytes)){
                assert(!x4_live_rtp_parse(bytes,14,&r));
                if(x4_live_track_accept(&m->audio->track,&r)>=0)atomic_store_explicit(&accepted_audio_source,r.ssrc,memory_order_release);
            }
        }
        sched_yield();
    }
    return NULL;
}
static void send_initial(X4LiveMedia *m,uint32_t source,unsigned pt)
{
    uint8_t p[14];packet(p,source,pt,1,1000,0x67);x4_live_media_receive(m,0,p,14);
    packet(p,source,pt,2,2000,0x68);x4_live_media_receive(m,0,p,14);
    packet(p,source,pt,3,3000,0x65);x4_live_media_receive(m,0,p,14);
    packet(p,source+10000,source==10?111:112,1,1000,0x11);x4_live_media_receive(m,1,p,14);
}
static void wait_source(uint32_t source,uint64_t before)
{
    uint64_t began=sceKernelGetProcessTime();
    while(atomic_load_explicit(&accepted_source,memory_order_acquire)!=source||atomic_load_explicit(&accepted_audio_source,memory_order_acquire)!=source+10000||atomic_load(&accepted_aus)<=before){
        assert(sceKernelGetProcessTime()-began<500000);sched_yield();
    }
}
static void *noise(void *opaque)
{
    X4LiveMedia *m=opaque;uint8_t p[14];unsigned seq=100;
    while(!atomic_load(&noise_stop)){
        unsigned source=atomic_load(&noise_source);
        if(source){packet(p,source,102,seq++,5000,0x61);x4_live_media_receive(m,0,p,14);
            packet(p,source+10000,112,seq,5000,0x11);x4_live_media_receive(m,1,p,14);}
        sceKernelUsleep(50);
    }
    return NULL;
}
static void test_epochs(void)
{
    X4LiveMedia *m=fixture();pthread_t worker,producer;
    atomic_init(&accepted_source,0);atomic_init(&accepted_audio_source,0);atomic_init(&accepted_aus,0);atomic_init(&owner_stop,false);
    atomic_init(&noise_stop,false);atomic_init(&noise_source,0);
    assert(!pthread_create(&worker,NULL,owner,m));
    send_initial(m,10,102);wait_source(10,0);
    assert(!pthread_create(&producer,NULL,noise,m));
    for(unsigned i=0;i<1000;++i){
        unsigned old=10+i,new_source=old+1;atomic_store(&noise_source,old);
        uint64_t before=atomic_load(&accepted_aus);
        assert(!x4_live_media_begin_epoch(m));
        uint64_t rejected=atomic_load(&m->receive_epoch_rejected);
        uint8_t p[14];packet(p,old,102,100,5000,0x65);x4_live_media_receive(m,0,p,14);
        assert(atomic_load(&m->receive_epoch_rejected)>rejected);
        uint64_t audio_dropped=atomic_load(&m->audio->dropped);
        packet(p,old+10000,112,100,5000,0x11);x4_live_media_receive(m,1,p,14);
        assert(atomic_load(&m->audio->dropped)>audio_dropped);
        send_initial(m,new_source,103);wait_source(new_source,before);
    }
    atomic_store(&noise_stop,true);assert(!pthread_join(producer,NULL));
    /* A valid new-peer token also supports an Xbox peer reusing the same
     * SSRC. The old immutable peer token is rejected independently of SSRC. */
    uint64_t old_epoch=x4_live_media_current_epoch(m),before=atomic_load(&accepted_aus);
    assert(old_epoch==1000&&!x4_live_media_begin_epoch(m));
    uint64_t new_epoch=x4_live_media_current_epoch(m);assert(new_epoch==old_epoch+1);
    uint8_t same[14];packet(same,1010,103,1,1000,0x67);
    uint64_t rejected=atomic_load(&m->receive_epoch_rejected);
    x4_live_media_receive_for_epoch(m,0,same,14,old_epoch);
    x4_live_media_receive_for_epoch(m,1,same,14,old_epoch);
    assert(atomic_load(&m->receive_epoch_rejected)==rejected+2);
    x4_live_media_receive_for_epoch(m,0,same,14,new_epoch);
    packet(same,1010,103,2,2000,0x68);x4_live_media_receive_for_epoch(m,0,same,14,new_epoch);
    packet(same,1010,103,3,3000,0x65);x4_live_media_receive_for_epoch(m,0,same,14,new_epoch);
    uint64_t audio_packets=atomic_load(&m->audio->packets);
    packet(same,11010,112,1,1000,0x11);x4_live_media_receive_for_epoch(m,1,same,14,new_epoch);
    assert(atomic_load(&m->audio->packets)==audio_packets+1);wait_source(1010,before);
    atomic_store(&owner_stop,true);assert(!pthread_join(worker,NULL));
    assert(atomic_load(&m->receive_epoch_applied)==1001);
    assert(atomic_load(&m->audio->epoch_applied)==1001);
    assert(!m->video.error);x4_video_ingress_stop(&m->ingress);assert(x4_video_ingress_free(&m->ingress));
    x4_live_reorder_free(&m->reorder);x4_live_reorder_free(&m->audio->reorder);
    x4_live_ring_free(&m->audio->ring);free(m->audio);free(m->au);
    assert(!x4_stream_wait_destroy(&m->ingress_wait));free(m);
    puts("PASS real media+audio: A/epoch/B IDR, old A rejected before B, 1000 concurrent epochs, stale token rejected and same SSRC accepted for new peer, no native decoder/output calls");
}
static void test_inflight_au_survives(void)
{
    X4StreamAuQueue *q=x4_stream_au_queue_create();assert(q);
    uint8_t data[16]={0x65,7};X4StreamAuInput in={.data=data,.sps=data,.sps_bytes=1,.bytes=sizeof(data),.is_idr=true,.identity=1};
    X4StreamAuOutcome out;X4StreamAuLease lease;X4StreamAuView view;
    assert(x4_stream_au_queue_try_push(q,&in,1,&out)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_try_take(q,2,&lease,&view,&out)==X4_STREAM_AU_OK);
    in.identity=2;in.is_idr=false;assert(x4_stream_au_queue_try_push(q,&in,3,&out)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_require_idr(q,X4_STREAM_AU_LOSS_BUSY,&out)==X4_STREAM_AU_OK);
    X4StreamAuStats stats;assert(x4_stream_au_queue_try_snapshot(q,&stats)==X4_STREAM_AU_OK);
    assert(stats.inflight_entries==1&&stats.ready_entries==0&&stats.waiting_idr);
    assert(!memcmp(view.data,data,sizeof(data)));assert(x4_stream_au_queue_release(q,lease)==X4_STREAM_AU_OK);
    x4_stream_au_queue_stop(q);assert(x4_stream_au_queue_destroy(q));
    puts("PASS queue epoch: READY retired; inflight Decode AU bytes/lease preserved until release");
}
int main(void){test_inflight_au_survives();test_epochs();return 0;}
