/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual application callback, video_batch, grace_controls and H264 AU logic.
 * The included production TU is unmodified. Kernel/video/audio/trace bridges
 * below are explicit host doubles. No worker thread, SRTP, native Decode,
 * output picture, RGB conversion, network or console behavior is measured. */
#include "receiver_nack_pipeline_worker.h"
#ifndef X4_PIPELINE_MEDIA_SOURCE
#define X4_PIPELINE_MEDIA_SOURCE "../src/media/live_media.c"
#endif
/* These historical native TU lines are unchanged; host -Werror must not
 * rewrite the exact baseline just to repair its old formatting. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include X4_PIPELINE_MEDIA_SOURCE
#pragma GCC diagnostic pop
#include <assert.h>

struct X4Pipeline { X4LiveMedia *media; };
static X4PipelineEvent events[16384];
static size_t event_count, event_omitted;
static uint64_t feed_calls;
static uint8_t feed_bytes[X4_LIVE_AU_MAX];
static size_t feed_size;
static _Thread_local X4NackPacket delivered;

static void record(unsigned kind,unsigned event,unsigned stage,unsigned flags,
    uint64_t identity,uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{
    if(event_count<sizeof(events)/sizeof(events[0]))
        events[event_count++]=(X4PipelineEvent){x4_pipeline_clock(),identity,source,a,b,c,kind,event,stage,flags};
    else ++event_omitted;
}
uint64_t sceKernelGetProcessTime(void){return x4_pipeline_clock();}
uint64_t x4_trace_now_us(void){return x4_pipeline_clock();}
uint64_t x4_ingress_test_now_us(void){return x4_pipeline_clock();}
int sceKernelUsleep(unsigned us){(void)us;abort();}
int scePthreadCreate(OrbisPthread *t,const pthread_attr_t *a,void *(*f)(void *),void *p,const char *n)
{(void)t;(void)a;(void)f;(void)p;(void)n;abort();}
int scePthreadJoin(OrbisPthread t,void **r){(void)t;(void)r;abort();}
void x4_grace_inbox_test_sleep(unsigned us){(void)us;abort();}
void x4_grace_inbox_test_after_enqueue_load(X4NackGraceInbox *q,uint_fast64_t at){(void)q;(void)at;}
X4NackPacket x4_rtc_nack_delivery_ticket(void){return delivered;}
uint64_t x4_rtc_receive_delivery_time(void){return x4_pipeline_clock();}
void x4_trace_record(X4Trace *t,uint16_t e,uint16_t f,uint64_t a,uint64_t b)
{(void)t;record(1,e,0,f,0,0,a,b,0);}
void x4_trace_monitor_critical(X4Trace *t,uint64_t now,unsigned e,unsigned s,uint32_t f,
    uint64_t id,uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{(void)t;size_t previous=event_count;record(2,e,s,f,id,source,a,b,c);if(event_count>previous)events[previous].time=now;}
void x4_trace_poll(X4Trace *t){(void)t;}
void x4_trace_monitor_rx(X4Trace *t,unsigned k,bool v,uint32_t s,uint16_t q,size_t b,uint64_t n)
{(void)t;(void)k;(void)v;(void)s;(void)q;(void)b;(void)n;}
void x4_trace_monitor_queue(X4Trace *t,uint32_t d,uint64_t a,bool v){(void)t;(void)d;(void)a;(void)v;}
void x4_trace_monitor_state(X4Trace *t,unsigned a,unsigned p,uint64_t n){(void)t;(void)a;(void)p;(void)n;}
void x4_trace_monitor_epoch(X4Trace *t,unsigned a,unsigned e){(void)t;(void)a;(void)e;}
X4Trace *x4_trace_create(uint64_t n,unsigned r){(void)n;(void)r;return NULL;}
bool x4_trace_monitor_bind_queue(X4Trace *t,const X4VideoIngress *q){(void)t;(void)q;return true;}
bool x4_trace_monitor_config(X4Trace *t,const X4MonitorConfig *c){(void)t;(void)c;return true;}
int x4_trace_monitor_start(X4Trace *t){(void)t;return 0;}
int x4_trace_monitor_stop(X4Trace *t){(void)t;return 0;}
void x4_trace_set_active(X4Trace *t,bool a){(void)t;(void)a;}
void x4_trace_end_session(X4Trace *t){(void)t;}
int x4_trace_dump_file(X4Trace *t,const char *p){(void)t;(void)p;return X4_TRACE_DUMP_DISABLED;}
int x4_trace_monitor_dump(X4Trace *t,const char *p){(void)t;(void)p;return X4_TRACE_DUMP_DISABLED;}
bool x4_trace_free(X4Trace *t){(void)t;return true;}
void x4_live_audio_receive(X4LiveAudio *a,const uint8_t *p,size_t n){(void)a;(void)p;(void)n;abort();}
void x4_live_audio_snapshot(const X4LiveAudio *a,X4LiveAudioSnapshot *s){(void)a;memset(s,0,sizeof(*s));}
int x4_live_audio_stop(X4LiveAudio *a){(void)a;return 0;}
int x4_live_video_set_target(X4LiveVideo *v,uint32_t *p,size_t n){(void)v;(void)p;(void)n;return 0;}
int x4_live_video_convert_pending(X4LiveVideo *v){(void)v;return 0;}
int x4_live_video_start(X4LiveVideo *v,uint32_t *p,size_t n){(void)v;(void)p;(void)n;abort();}
int x4_live_video_stop(X4LiveVideo *v){(void)v;abort();}
int x4_live_video_feed(X4LiveVideo *v,const uint8_t *p,size_t n,uint64_t pts)
{(void)v;assert(n<=sizeof(feed_bytes));if(pts==UINT64_C(70000)*1000000/90000){memcpy(feed_bytes,p,n);feed_size=n;}++feed_calls;return 0;}

X4Pipeline *x4_pipeline_create(bool grace,uint32_t transport_epoch)
{
    event_count=event_omitted=feed_calls=feed_size=0;memset(&delivered,0,sizeof(delivered));
    X4Pipeline *p=calloc(1,sizeof(*p));assert(p);
    int rc=0;p->media=x4_live_media_create(&rc);assert(p->media&&rc==0);
    /* Host matrix controls only existing grace mode, never queue/reorder parameters. */
    x4_nack_grace_init(&p->media->grace,p->media->trace_session,grace);
    x4_nack_grace_inbox_bind(&p->media->grace_inbox,transport_epoch);
    assert(x4_live_media_set_payload_type(p->media,0,102)==0);
    atomic_store(&p->media->started,true);
    return p;
}
void x4_pipeline_receive(X4Pipeline *p,const void *bytes,size_t n,const X4NackPacket *ticket)
{delivered=ticket?*ticket:(X4NackPacket){0};x4_live_media_receive(p->media,0,bytes,n);}
void x4_pipeline_batch(X4Pipeline *p){video_batch(p->media);}
bool x4_pipeline_control(const X4NackGraceControl *control)
{return x4_rtc_nack_grace_control(control);}
unsigned x4_pipeline_reorder_code(bool late)
{return late?X4_LIVE_REORDER_LATE:X4_LIVE_REORDER_STORED;}
X4PipelineSnapshot x4_pipeline_snapshot(const X4Pipeline *p)
{
    const X4LiveMedia *m=p->media;
    return (X4PipelineSnapshot){m->grace.counters.pending_positions,m->grace.counters.admitted,
        m->grace.counters.accepted_positions,m->grace.counters.stale_source_offers,
        m->grace.counters.duplicate_offers,m->grace.counters.hold_queries,
        m->au_submitted,feed_calls,m->dropped_frames,m->recovery_episodes,
        x4_video_ingress_depth(&m->ingress),atomic_load(&m->grace_inbox.admitted),
        atomic_load(&m->grace_inbox.omitted),atomic_load(&m->grace_inbox.stale),event_omitted,
        m->grace.sequence_epoch,m->reorder.expected,m->reorder.buffered,m->track.locked,m->waiting_keyframe};
}
size_t x4_pipeline_event_count(void){return event_count;}
bool x4_pipeline_event(size_t i,X4PipelineEvent *e){if(i>=event_count)return false;*e=events[i];return true;}
size_t x4_pipeline_feed_bytes(void *out,size_t cap){assert(cap>=feed_size);memcpy(out,feed_bytes,feed_size);return feed_size;}
void x4_pipeline_close(X4Pipeline *p)
{x4_nack_grace_inbox_unbind(&p->media->grace_inbox);assert(x4_live_media_close(p->media)==0);free(p);}
