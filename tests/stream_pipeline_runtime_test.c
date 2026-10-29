/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual streaming scheduler, conversion and metrics. Native decoder,
 * VideoOut and stack wrapper are host substitutes; not PS4 evidence. */
#include "../src/video/stream_pipeline.h"
#include "../src/video/live_h264.h"
#include "../src/video/video_pipeline.h"
#include "../src/video/stream_cpu.h"
#include <orbis/VideoOut.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static unsigned native_outputs, native_closes, allocation_count, join_count;
static unsigned create_count, fail_thread, flip_delay, pending_polls, flip_count;
static int map_failure, flip_failure, close_failure, decoder_failure;
static int pending=-1,screen=-1,handle_open;
static uint32_t *scanout[3], pending_pixel,screen_pixel;
static uint64_t pending_arg,shown_arg,shown_time,flip_time_override;
static pthread_t main_owner,decode_owner;
static const char *report_directory;
/* Locale belongs to the UI, outside this scheduler/native-double harness.
 * This fixed value only completes stream_profile's existing link contract. */
const char *x4_lang_locale(void){return "es-MX";}
static void *allocated;
static atomic_uint decode_pause_us;
static atomic_bool decode_native_running;
static atomic_uint native_sleep_calls, pipeline_print_calls, pipeline_format_calls;
static atomic_uint_fast64_t fixture_clock;
static bool cancel_before_decode, reject_scanout_finish, start_cleanup_retained, start_rejected;
enum { DISPATCH_NONE,DISPATCH_CANCEL,DISPATCH_NOTIFY_ERROR,DISPATCH_CPU_ERROR,DISPATCH_WAIT_ERROR };
static atomic_uint dispatch_fault;
static atomic_bool cpu_fixture_hold,cpu_fixture_entered,cpu_fixture_continued;
static atomic_bool cpu_calls_enabled,cpu_affinity_fail_once;
static _Thread_local int cpu_current_priority=612;
static _Thread_local uint64_t cpu_current_affinity=0x2d;
static pthread_mutex_t cpu_fixture_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cpu_fixture_condition=PTHREAD_COND_INITIALIZER;
static bool cpu_fixture_release;
static atomic_bool cancel_fixture_arm,cancel_fixture_entered[2];
static pthread_mutex_t cancel_fixture_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cancel_fixture_condition=PTHREAD_COND_INITIALIZER;
static bool cancel_fixture_release;
static const atomic_bool *native_cancel;
uint64_t sceKernelGetProcessTime(void)
{uint64_t controlled=atomic_load(&fixture_clock);if(controlled)return controlled;
 struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
int sceKernelUsleep(unsigned n){atomic_fetch_add(&native_sleep_calls,1);return usleep(n);}
int sceKernelFsync(int fd){return fsync(fd);}
/* CPU-0 must not touch scheduler APIs. The optional CPU-2 scenario below uses
 * the real policy/helper with thread-local native scheduler/attribute doubles. */
OrbisPthread scePthreadSelf(void){return pthread_self();}
int32_t scePthreadGetprio(OrbisPthread thread,int32_t *priority)
{assert(atomic_load(&cpu_calls_enabled)&&pthread_equal(thread,pthread_self()));*priority=cpu_current_priority;return 0;}
int32_t scePthreadAttrInit(OrbisPthreadAttr *attr)
{assert(atomic_load(&cpu_calls_enabled));memset(attr,0,sizeof(*attr));return 0;}
int32_t scePthreadAttrGet(OrbisPthread thread,OrbisPthreadAttr *attr)
{assert(atomic_load(&cpu_calls_enabled)&&pthread_equal(thread,pthread_self())&&attr);return 0;}
int32_t scePthreadAttrGetaffinity(const OrbisPthreadAttr *attr,void *mask)
{assert(atomic_load(&cpu_calls_enabled)&&attr);*(uint64_t*)mask=cpu_current_affinity;return 0;}
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr)
{assert(atomic_load(&cpu_calls_enabled)&&attr);return 0;}
int scePthreadSetprio(OrbisPthread thread,int priority)
{assert(atomic_load(&cpu_calls_enabled)&&pthread_equal(thread,pthread_self()));cpu_current_priority=priority;return 0;}
int32_t scePthreadSetaffinity(OrbisPthread thread,uint64_t mask)
{assert(atomic_load(&cpu_calls_enabled)&&pthread_equal(thread,pthread_self()));
 if(mask==0x20&&atomic_exchange(&cpu_affinity_fail_once,false))return -EACCES;
 cpu_current_affinity=mask;return 0;}
int scePthreadJoin(OrbisPthread t,void **v){++join_count;return pthread_join(t,v);}
int x4_probe_thread_create(OrbisPthread *id,X4ProbeThread *t,void *(*entry)(void *),void *arg,const char *name)
{
    ++create_count;memset(t,0,sizeof(*t));snprintf(t->report.name,sizeof(t->report.name),"%s",name);
    t->report.configured_bytes=t->report.reported_bytes=262144;
    if(fail_thread==create_count){t->report.wrapper_result=-71;return -71;}
    pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,262144));
    int rc=pthread_create(id,&attr,entry,arg);assert(!pthread_attr_destroy(&attr));t->created=!rc;t->report.create_rc=rc;return rc;
}
void x4_probe_thread_joined(X4ProbeThread *t,int rc){t->report.join_rc=rc;if(!rc)t->created=false;}
typedef struct { uint8_t *pixels[4]; } Native;
int x4_live_video_stream_raw_start(X4LiveVideo *v,const uint8_t *sps,size_t bytes,const atomic_bool *cancel)
{
    assert(sps && bytes && !atomic_load(cancel));native_cancel=cancel;decode_owner=pthread_self();assert(!pthread_equal(main_owner,decode_owner));
    if(start_rejected){v->stream_info.sps.profile_idc=100;v->stream_info.sps.chroma_format_idc=2;
        v->stream_info.sps_diagnostics=(X4StreamSpsDiagnostics){.code=X4_SPS_CHROMA,.stage=X4_SPS_STAGE_FORMAT,.valid_mask=X4_SPS_HAVE_PROFILE|X4_SPS_HAVE_CHROMA};return X4_SPS_CHROMA;}
    Native *n=calloc(1,sizeof(*n));assert(n);v->state=n;
    for(unsigned i=0;i<4;++i){n->pixels[i]=malloc(1280u*720u*3u/2u);assert(n->pixels[i]);}
    v->stream_info.sps=(X4StreamSps){.profile_idc=66,.level_idc=32,.visible_width=1280,.visible_height=720,.coded_width=1280,.coded_height=720};
    v->stream_info.sps_diagnostics.syntax_complete=true;
    if(start_cleanup_retained){v->stream_info.retained=true;v->stream_info.close_result=-81;return -80;}
    return 0;
}
int x4_live_video_stream_raw_start_cpu(X4LiveVideo *v,const uint8_t *sps,size_t bytes,const atomic_bool *cancel,unsigned cpu)
{assert(cpu<=X4_CPU_3);int rc=x4_live_video_stream_raw_start(v,sps,bytes,cancel);
 X4StreamCpuConfig selected=x4_stream_cpu_config(cpu);
 v->stream_info.cpu_variant=cpu;v->stream_info.decoder_cpu_priority=selected.decoder_priority;
 v->stream_info.decoder_cpu_affinity=selected.decoder_affinity;return rc;}
int x4_live_video_stream_raw_start_cpu_depth(X4LiveVideo *v,const uint8_t *sps,size_t bytes,const atomic_bool *cancel,unsigned cpu,unsigned depth)
{assert(depth==1);int rc=x4_live_video_stream_raw_start_cpu(v,sps,bytes,cancel,cpu);
 v->stream_info.requested_depth=v->stream_info.active_depth=1;v->stream_info.input_count=2;v->stream_info.output_count=4;return rc;}
unsigned x4_live_video_stream_output_held_mask(const X4LiveVideo *v){(void)v;return 0;}
int x4_live_video_stream_raw_decode(X4LiveVideo *v,unsigned slot,const uint8_t *au,size_t bytes,uint64_t pts,bool flush,X4ProbeNativeOutput *o)
{
    (void)pts;assert(!flush && pthread_equal(pthread_self(),decode_owner) && slot<4 && bytes==8);
    if(cancel_before_decode){atomic_store((atomic_bool*)native_cancel,true);return 3;}
    uint64_t identity;memcpy(&identity,au,8);assert(identity==native_outputs);
    unsigned pause=atomic_load(&decode_pause_us);
    uint64_t start=sceKernelGetProcessTime();
    atomic_store_explicit(&decode_native_running,true,memory_order_release);
    if(pause)usleep(pause);
    o->decode_end_us=sceKernelGetProcessTime();o->decode_us=o->decode_end_us-start;
    atomic_store_explicit(&decode_native_running,false,memory_order_release);++v->decode_calls;
    if(decoder_failure){o->native_result=decoder_failure;return decoder_failure;}
    Native *n=v->state;uint8_t *p=n->pixels[slot];memset(p,16+(native_outputs%100),1280u*720u);memset(p+1280u*720u,128,1280u*360u);
    o->pixels=p;o->owned_bytes=o->returned_bytes=1280u*720u*3u/2u;
    o->layout=(X4VideoNv12Layout){.visible_width=1280,.visible_height=720,.coded_width=1280,.coded_height=720,.pitch=1280,.storage_height=720,
        .y_bytes=1280u*720u,.uv_bytes=1280u*360u,.uv_plane_offset=1280u*720u,.total_nv12_bytes=1280u*1080u,
        .visible_uv_offset=1280u*720u,.rgb_pixels=1280u*720u,.rgb_bytes=1280u*720u*4u};
    o->slot=slot;o->native_width=1280;o->native_height=720;o->pitch=1280;o->accepted=o->valid=o->layout_verified=true;
    o->output_id=++native_outputs;
    ++v->decoded_frames;v->decode_us+=o->decode_us;if(o->decode_us>v->decode_max_us)v->decode_max_us=o->decode_us;
    return 0;
}
void x4_live_video_stream_info(const X4LiveVideo *v,X4StreamVideoInfo *i){*i=v->stream_info;}
int x4_live_video_stop(X4LiveVideo *v)
{
    if(!v->state)return 0;
    assert(pthread_equal(pthread_self(),decode_owner));Native *n=v->state;
    for(unsigned i=0;i<4;++i)free(n->pixels[i]);
    free(n);v->state=NULL;++native_closes;return 0;
}
size_t sceKernelGetDirectMemorySize(void){return 1024u*1024u*1024u;}
int sceKernelAllocateDirectMemory(off_t a,off_t b,size_t bytes,size_t align,int type,off_t *offset)
{(void)a;(void)b;(void)align;assert(type==3 && !allocated);allocated=malloc(bytes);assert(allocated);*offset=1;++allocation_count;return 0;}
int sceKernelMapDirectMemory(void **p,size_t bytes,int prot,int flags,off_t off,size_t align)
{(void)bytes;(void)prot;(void)flags;(void)off;(void)align;if(map_failure)return map_failure;*p=allocated;return 0;}
int sceKernelMunmap(void *p,size_t n){(void)n;assert(p==allocated);return 0;}
int sceKernelReleaseDirectMemory(off_t o,size_t n){(void)o;(void)n;free(allocated);allocated=NULL;--allocation_count;return 0;}
int sceVideoOutOpen(int a,int b,int c,const void *d){(void)a;(void)b;(void)c;(void)d;assert(!handle_open);handle_open=1;return 7;}
int sceVideoOutClose(int h){assert(h==7 && handle_open);if(close_failure)return close_failure;handle_open=0;pending=screen=-1;return 0;}
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *a,int f,int t,int r,unsigned w,unsigned h,unsigned p)
{(void)f;(void)t;(void)r;*a=(OrbisVideoOutBufferAttribute){w,h,p};}
int sceVideoOutRegisterBuffers(int h,int first,void *const *pixels,int count,const OrbisVideoOutBufferAttribute *a)
{assert(h==7 && first==0 && count==3 && a->width==1920);for(unsigned i=0;i<3;++i)scanout[i]=pixels[i];return 0;}
int sceVideoOutSetFlipRate(int h,int r){(void)r;assert(h==7);return 0;}
int sceVideoOutSubmitFlip(int h,int slot,int vsync,int64_t arg)
{
    (void)vsync;assert(h==7 && pending<0 && slot!=screen && (uint64_t)arg>shown_arg);
    if(flip_failure)return flip_failure;
    pending=slot;pending_arg=(uint64_t)arg;pending_polls=flip_delay;
    pending_pixel=scanout[slot][1920u*1080u-1];++flip_count;return 0;
}
int sceVideoOutGetFlipStatus(int h,OrbisVideoOutFlipStatus *s)
{
    assert(h==7);if(screen>=0)assert(scanout[screen][1920u*1080u-1]==screen_pixel);
    if(pending>=0){assert(scanout[pending][1920u*1080u-1]==pending_pixel);
        if(pending_polls)--pending_polls;else {screen=pending;pending=-1;shown_arg=pending_arg;screen_pixel=pending_pixel;
            shown_time=flip_time_override?flip_time_override:sceKernelGetProcessTime();}}
    *s=(OrbisVideoOutFlipStatus){.num=flip_count,.flipArg=(int64_t)shown_arg,.ptime=shown_time};return 0;
}
int sceGnmSubmitDone(void){return 0;}
void x4_trace_sdp_bandwidth_get(const X4Trace *trace,unsigned scope,unsigned values[4])
{(void)trace;(void)scope;memset(values,0,4*sizeof(*values));}
uint32_t x4_trace_sdp_profile_get(const X4Trace *trace,unsigned scope,unsigned pt)
{(void)trace;return pt==102?(scope?0x42e020:0x42e01f):0;}
static FILE *fixture_fopen(const char *path,const char *mode)
{char safe[512];const char *base=strrchr(path,'/');assert(base);int n=snprintf(safe,sizeof(safe),"%s/%s",report_directory,base+1);assert(n>0&&(size_t)n<sizeof(safe));return fopen(safe,mode);}
static int fixture_finish(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease,uint64_t id,uint64_t decoded,uint64_t converted)
{return reject_scanout_finish?X4_PIPELINE_STATE:x4_video_pipeline_finish_scanout(d,lease,id,decoded,converted);}
static int fixture_printf(const char *format,...)
{
    atomic_fetch_add(&pipeline_print_calls,1);va_list ap;va_start(ap,format);
    int rc=vprintf(format,ap);va_end(ap);return rc;
}
static int fixture_snprintf(char *destination,size_t bytes,const char *format,...)
{
    atomic_fetch_add(&pipeline_format_calls,1);va_list ap;va_start(ap,format);
    int rc=vsnprintf(destination,bytes,format,ap);va_end(ap);return rc;
}
#define fopen fixture_fopen
#define x4_video_pipeline_finish_scanout fixture_finish
#define printf fixture_printf
#define snprintf fixture_snprintf
#include "../src/video/stream_pipeline.c"
#undef fopen
#undef x4_video_pipeline_finish_scanout
#undef printf
#undef snprintf
/* Existing explicit empty diagnostics; no transport/input owner runs here. */
#include "stream_startup_metrics_stubs.c"
int x4_stream_pipeline_test_cpu_ready(X4StreamPipeline *p,unsigned index,int applied)
{
    (void)p;
    if(index||!atomic_load_explicit(&cpu_fixture_hold,memory_order_acquire))return applied;
    assert(!pthread_mutex_lock(&cpu_fixture_gate));
    atomic_store_explicit(&cpu_fixture_entered,true,memory_order_release);
    while(!cpu_fixture_release)assert(!pthread_cond_wait(&cpu_fixture_condition,&cpu_fixture_gate));
    assert(!pthread_mutex_unlock(&cpu_fixture_gate));
    atomic_store_explicit(&cpu_fixture_continued,true,memory_order_release);return -EACCES;
}
void x4_stream_pipeline_test_before_idle(X4StreamPipeline *p,unsigned index)
{
    (void)p;
    if(!atomic_load_explicit(&cancel_fixture_arm,memory_order_acquire))return;
    assert(!pthread_mutex_lock(&cancel_fixture_gate));
    atomic_store_explicit(&cancel_fixture_entered[index],true,memory_order_release);
    while(!cancel_fixture_release)assert(!pthread_cond_wait(&cancel_fixture_condition,&cancel_fixture_gate));
    assert(!pthread_mutex_unlock(&cancel_fixture_gate));
}
void x4_stream_pipeline_test_before_dispatch(X4StreamPipeline *p)
{
    unsigned fault=atomic_exchange_explicit(&dispatch_fault,DISPATCH_NONE,memory_order_acq_rel);
    if(fault==DISPATCH_CANCEL){
        atomic_store_explicit(&cancel_fixture_arm,true,memory_order_release);
        x4_stream_pipeline_request_stop(p);uint64_t deadline=sceKernelGetProcessTime()+1000000;
        for(unsigned i=0;i<2;++i)while(!atomic_load_explicit(&cancel_fixture_entered[i],memory_order_acquire)&&
            !atomic_load_explicit(&p->converters[i].finished,memory_order_acquire)){
            assert(sceKernelGetProcessTime()<deadline);usleep(100);}
        uint64_t before[2]={atomic_load(&p->converters[0].work_wait.wait_calls),atomic_load(&p->converters[1].work_wait.wait_calls)};
        assert(!pthread_mutex_lock(&cancel_fixture_gate));
        atomic_store_explicit(&cancel_fixture_arm,false,memory_order_release);cancel_fixture_release=true;
        assert(!pthread_cond_broadcast(&cancel_fixture_condition));assert(!pthread_mutex_unlock(&cancel_fixture_gate));
        /* The barrier places the cancellation before the idle stop decision.
         * Old OR stopping(owner) must finish; the fixed worker must execute a
         * new wait. No arbitrary sleep decides whether dispatch may proceed. */
        for(unsigned i=0;i<2;++i){
            while(!atomic_load_explicit(&p->converters[i].finished,memory_order_acquire)&&
                atomic_load(&p->converters[i].work_wait.wait_calls)<=before[i]){
                assert(sceKernelGetProcessTime()<deadline);usleep(100);}
            assert(!atomic_load_explicit(&p->converters[i].finished,memory_order_acquire));
        }
    }else if(fault==DISPATCH_NOTIFY_ERROR){
        x4_stream_wait_test_fail_notify(&p->converters[0].work_wait,EIO);
    }else if(fault==DISPATCH_CPU_ERROR){
            assert(atomic_load_explicit(&cpu_fixture_entered,memory_order_acquire));
            assert(!pthread_mutex_lock(&cpu_fixture_gate));cpu_fixture_release=true;
            assert(!pthread_cond_signal(&cpu_fixture_condition));assert(!pthread_mutex_unlock(&cpu_fixture_gate));
        uint64_t deadline=sceKernelGetProcessTime()+1000000;
        while(!atomic_load_explicit(&cpu_fixture_continued,memory_order_acquire)){
            assert(sceKernelGetProcessTime()<deadline);usleep(100);}
        assert(!atomic_load_explicit(&p->converters[0].finished,memory_order_acquire));
    }else if(fault==DISPATCH_WAIT_ERROR){
            x4_stream_wait_test_fail_wait(&p->converters[0].work_wait,EIO);
            assert(!x4_stream_wait_notify(&p->converters[0].work_wait));
        uint64_t deadline=sceKernelGetProcessTime()+1000000;
        while(!atomic_load_explicit(&p->converters[0].finished,memory_order_acquire)){
            assert(sceKernelGetProcessTime()<deadline);usleep(100);}
        assert(atomic_load_explicit(&p->converters[0].completed,memory_order_acquire)!=p->work);
    }
}
static int fixture_free(X4StreamPipeline *p)
{
    uint64_t session=p?p->session:0;
    printf("fixture FREE_BEGIN profile=%d case=%llu retained=%u clock=%llu\n",X4_STREAM_PROFILE,
        (unsigned long long)session,p&&p->retained?1u:0u,(unsigned long long)sceKernelGetProcessTime());
    int rc=x4_stream_pipeline_free(p);
    printf("fixture FREE_END profile=%d case=%llu rc=%d\n",X4_STREAM_PROFILE,(unsigned long long)session,rc);
    return rc;
}
/* Test-only checkpoint around the real free function, after its definition. */
#define x4_stream_pipeline_free fixture_free
typedef struct {X4StreamPipeline *pipeline;unsigned frames;int result;bool unpaced;} Producer;
static void *produce(void *arg)
{
    Producer *p=arg;const uint8_t sps[]={0,0,0,1,0x67},pps[]={0,0,0,1,0x68,0x80};
    for(uint64_t i=0;i<p->frames;++i){
        x4_stream_pipeline_au_received(p->pipeline,false,sceKernelGetProcessTime());
        p->result=x4_stream_pipeline_submit_parameters(p->pipeline,(const uint8_t*)&i,8,i*16667,i,sps,sizeof(sps),pps,sizeof(pps));
        if(p->result)break;
        /* Fixture pacing keeps this 100-output ownership/export test separate
         * from deliberate overload. Production submit itself never waits. */
        if(!p->unpaced){uint64_t deadline=sceKernelGetProcessTime()+10000000;
            while(atomic_load_explicit(&p->pipeline->consumed,memory_order_acquire)<=i &&
                !atomic_load(&p->pipeline->error) && !stopping(p->pipeline)) {
                uint64_t now=sceKernelGetProcessTime();
                if(now>=deadline){X4StreamAuStats stats={0};int qrc=x4_stream_au_queue_try_snapshot(p->pipeline->au_queue,&stats);
                    fprintf(stderr,"fixture pacing timeout i=%llu submitted=%llu consumed=%llu dequeued=%llu error=%d waiting_idr=%u queue_rc=%d ready=%u inflight=%u resident=%u lag_max=%llu queue_wait=%u transitions=%llu rejected_wait=%llu retired_nonIDR=%llu retired_IDR=%llu\n",
                        (unsigned long long)i,(unsigned long long)atomic_load(&p->pipeline->submitted),
                        (unsigned long long)atomic_load(&p->pipeline->consumed),(unsigned long long)atomic_load(&p->pipeline->dequeued),
                        atomic_load(&p->pipeline->error),p->pipeline->waiting_idr?1u:0u,qrc,stats.ready_entries,stats.inflight_entries,stats.resident_entries,
                        (unsigned long long)stats.lag_max_us,stats.waiting_idr?1u:0u,(unsigned long long)stats.recovery_transitions,
                        (unsigned long long)stats.rejected_incoming_wait_idr,(unsigned long long)stats.evicted_non_idr,(unsigned long long)stats.old_idr_retired);
                }
                assert(now<deadline);usleep(50);
            }
        }
    }
    return NULL;
}
static unsigned overlays;
static void overlay(uint32_t *pixels,void *ctx){(void)ctx;pixels[0]=0xffabcdef;++overlays;}
static void reset(void)
{
    /* Restore the real clock before every case, including after capacity's
     * frozen-time control. No subsequent failure deadline may stay frozen. */
    atomic_store(&fixture_clock,0);
    X4StreamRequestConfig fixture_request=x4_stream_settings_get();
    assert(x4_stream_settings_set_all(fixture_request.profile,fixture_request.color_preference,fixture_request.cpu_variant,X4_DEPTH_1));
    assert(!allocated&&!handle_open);native_outputs=native_closes=allocation_count=join_count=create_count=fail_thread=flip_count=overlays=0;
    map_failure=flip_failure=close_failure=decoder_failure=0;flip_delay=4;pending=screen=-1;
    shown_time=flip_time_override=0;
    cancel_before_decode=reject_scanout_finish=start_cleanup_retained=start_rejected=false;
    atomic_store(&decode_pause_us,500);main_owner=pthread_self();
    atomic_store(&dispatch_fault,DISPATCH_NONE);atomic_store(&cpu_fixture_hold,false);atomic_store(&cpu_fixture_entered,false);
    atomic_store(&cpu_fixture_continued,false);atomic_store(&cpu_calls_enabled,false);atomic_store(&cpu_affinity_fail_once,false);
    assert(!pthread_mutex_lock(&cpu_fixture_gate));cpu_fixture_release=false;assert(!pthread_mutex_unlock(&cpu_fixture_gate));
    atomic_store(&cancel_fixture_arm,false);for(unsigned i=0;i<2;++i)atomic_store(&cancel_fixture_entered[i],false);
    assert(!pthread_mutex_lock(&cancel_fixture_gate));cancel_fixture_release=false;assert(!pthread_mutex_unlock(&cancel_fixture_gate));
    x4_stream_wait_test_fail_notify(NULL,0);x4_stream_wait_test_fail_wait(NULL,0);
}
static X4StreamPipeline *new_pipeline(atomic_bool *stop,unsigned session)
{
    printf("fixture CASE_BEGIN profile=%d case=%u clock=%llu\n",X4_STREAM_PROFILE,session,(unsigned long long)sceKernelGetProcessTime());
    atomic_init(stop,false);X4StreamRequestConfig request=x4_stream_request_config(X4_STREAM_PROFILE==2?X4_REQUEST_1080P_A:X4_REQUEST_720P60,X4_COLOR_AUTO_709);
    /* This legacy scheduler double completes the offered slot immediately.
     * Auto/delayed depth2 is covered by the real-adapter resilience suite. */
    request.decoder_depth=X4_DEPTH_1;
    X4StreamPipeline *p=x4_stream_pipeline_create_request(session,NULL,stop,&request);assert(p);return p;
}
typedef struct {X4StreamPipeline *pipeline;atomic_bool done;int result;} AdmissionFailure;
static void *submit_admission_failure(void *argument)
{
    AdmissionFailure *call=argument;
    const uint8_t payload[]={0,0,0,1,0x65,0x80},sps[]={0x67};
    call->result=x4_stream_pipeline_submit_parameters(call->pipeline,payload,sizeof(payload),0,1,sps,sizeof(sps),NULL,0);
    atomic_store_explicit(&call->done,true,memory_order_release);return NULL;
}
static void admission_failure_without_wait(unsigned session,bool queue_fatal)
{
    reset();atomic_bool stop;X4StreamPipeline *p=new_pipeline(&stop,session);
    if(queue_fatal){
        /* Deliberately invalid public queue call, outside submission: poison
         * this fixture's queue to cover the pipeline's fatal-admission branch.
         * No production state layout or implementation is changed. */
        assert(x4_stream_au_queue_try_push(p->au_queue,NULL,0,NULL)==X4_STREAM_AU_FATAL);
    }else atomic_store(&p->submitted,UINT64_MAX);
    assert(!x4_stream_mutex_trylock(&p->status_gate));
    atomic_store(&native_sleep_calls,0);atomic_store(&pipeline_print_calls,0);atomic_store(&pipeline_format_calls,0);
    AdmissionFailure call={.pipeline=p};atomic_init(&call.done,false);pthread_t thread;
    assert(!pthread_create(&thread,NULL,submit_admission_failure,&call));
    uint64_t deadline=sceKernelGetProcessTime()+2000000;
    while(!atomic_load_explicit(&call.done,memory_order_acquire)){
        /* Host wall-clock guard, independent of the intercepted native sleep.
         * A regression that takes status_gate must fail, never hang the suite. */
        assert(sceKernelGetProcessTime()<deadline);usleep(100);
    }
    assert(!pthread_join(thread,NULL));
    int expected=queue_fatal?X4_STREAM_AU_FATAL:X4_PIPELINE_EXHAUSTED;
    assert(call.result==expected && atomic_load(&p->error)==expected && atomic_load(&p->cancel));
    assert(!atomic_load(&stop) && !native_outputs && !create_count);
    assert(atomic_load(&p->admission_failure)==(queue_fatal?2u:1u) && !p->failure_where[0]);
    assert(!atomic_load(&native_sleep_calls) && !atomic_load(&pipeline_print_calls) && !atomic_load(&pipeline_format_calls));
    assert(!x4_stream_mutex_unlock(&p->status_gate));
    /* Undo ONLY the artificial counter corruption, before any report. The
     * genuine queue admitted/taken/released counts all remain zero. */
    if(!queue_fatal){
        atomic_store(&p->submitted,0);
        /* Model a losing native failure writer that formatted its provisional
         * label before the admission CAS won. The winning token must prevail. */
        memcpy(p->failure_where,"provisional native failure",sizeof("provisional native failure"));
    }
    assert(!x4_stream_pipeline_close(p));
    assert(!strcmp(p->failure_where,queue_fatal?"AU admission state":"AU identity capacity"));
    assert(atomic_load(&pipeline_print_calls)==1 && atomic_load(&pipeline_format_calls)==1);
    assert(!x4_stream_pipeline_export(p,NULL));assert(!x4_stream_pipeline_free(p));
    puts("PASS fatal admission: retained status gate, bounded return, no native sleep/stdio, deferred close label and real export");
}
static void run(void)
{
    reset();atomic_bool stop;X4StreamPipeline *p=new_pipeline(&stop,11);assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));
    Producer pr={p,100,0,false};pthread_t producer;assert(!pthread_create(&producer,NULL,produce,&pr));
    uint64_t presentation_deadline=sceKernelGetProcessTime()+30000000;
    for(;;){assert(!x4_stream_pipeline_present(p,overlay,NULL));X4StreamPipelineSnapshot s;x4_stream_pipeline_snapshot(p,&s);
        if(s.outputs==100&&!p->converting&&p->display.pending<0)break;
        assert(!s.error && sceKernelGetProcessTime()<presentation_deadline);usleep(200);}
    assert(!pthread_join(producer,NULL)&&pr.result==0);
    assert(p->view.outputs==100 && p->view.presentations>0 && overlays>0 && p->display.peak_in_flight==1);
    assert(!x4_stream_pipeline_close(p));assert(join_count==3&&native_closes==1&&!allocated&&!handle_open);
    assert(!x4_stream_pipeline_export(p,NULL));assert(p->ring.occupied==0);x4_stream_pipeline_free(p);
    puts("PASS actual streaming runtime: bounded AU ownership, latest ring, two converters, delayed flips, overlay, export and cleanup");
}
static void failures(void)
{
    atomic_bool stop;
    reset();start_rejected=true;X4StreamPipeline *rejected=new_pipeline(&stop,38);assert(!x4_stream_pipeline_start(rejected));
    uint8_t prefix[80];memset(prefix,0xab,sizeof(prefix));uint64_t input=0;
    assert(!x4_stream_pipeline_submit_parameters(rejected,(const uint8_t*)&input,8,0,1,prefix,sizeof(prefix),prefix,sizeof(prefix)));
    for(unsigned i=0;i<1000&&!atomic_load(&rejected->decoder_done);++i)usleep(1000);
    assert(atomic_load(&rejected->decoder_done)&&atomic_load(&rejected->error)==X4_SPS_CHROMA);
    assert(!native_outputs&&!rejected->video.state&&atomic_load(&rejected->dequeued)==1&&atomic_load(&rejected->consumed)==1);
    assert(!atomic_load(&stop)); /* Video cancellation never cancels RTC/audio. */
    assert(!x4_stream_pipeline_close(rejected));assert(!x4_stream_pipeline_export(rejected,NULL));x4_stream_pipeline_free(rejected);
    for(unsigned i=1;i<=3;++i){reset();fail_thread=i;X4StreamPipeline *p=new_pipeline(&stop,20+i);assert(x4_stream_pipeline_start(p)==-71);assert(!x4_stream_pipeline_close(p));assert(join_count==i-1);x4_stream_pipeline_free(p);}
    reset();X4StreamPipeline *p=new_pipeline(&stop,30);assert(!x4_stream_pipeline_start(p));map_failure=-72;assert(x4_stream_pipeline_display_open(p)==-72);assert(!x4_stream_pipeline_close(p));assert(!allocated&&!handle_open);x4_stream_pipeline_free(p);
    reset();atomic_store(&fixture_clock,1000000);p=new_pipeline(&stop,31);
    /* Capacity control, not a concurrency test: no native workers, constant
     * monotonic clock and RTP PTS. An instantaneous 60fps-PTS burst would
     * legitimately exceed250ms before reaching32entries and test LATE. */
    const uint8_t capacity_sps[]={0,0,0,1,0x67};uint64_t capacity_input=0;
    for(unsigned i=0;i<32;++i){capacity_input=i;
        assert(x4_stream_pipeline_submit_parameters(p,(const uint8_t*)&capacity_input,8,0,i,capacity_sps,sizeof(capacity_sps),NULL,0)==X4_STREAM_AU_OK);}
    X4StreamAuStats capacity={0};assert(x4_stream_au_queue_try_snapshot(p->au_queue,&capacity)==X4_STREAM_AU_OK);
    assert(capacity.resident_entries==32 && capacity.ready_entries==32 && capacity.depth_max==32);
    assert(x4_stream_pipeline_submit_parameters(p,(const uint8_t*)&capacity_input,8,0,33,capacity_sps,sizeof(capacity_sps),NULL,0)==X4_STREAM_AU_DROPPED_FULL);
    assert(!atomic_load(&p->error) && !native_outputs && !create_count);
    atomic_store(&stop,true);x4_stream_pipeline_request_stop(p);
    assert(!x4_stream_pipeline_close(p));x4_stream_pipeline_free(p);
    Producer pr;
    reset();p=new_pipeline(&stop,32);assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));decoder_failure=-73;pr=(Producer){p,1,0,false};produce(&pr);for(unsigned i=0;i<1000&&!atomic_load(&p->error);++i)usleep(1000);assert(atomic_load(&p->error)==-73);assert(!x4_stream_pipeline_close(p));x4_stream_pipeline_free(p);
    reset();p=new_pipeline(&stop,34);cancel_before_decode=true;assert(!x4_stream_pipeline_start(p));pr=(Producer){p,1,0,false};produce(&pr);for(unsigned i=0;i<1000&&!atomic_load(&p->decoder_done);++i)usleep(1000);assert(atomic_load(&p->decoder_done)&&!atomic_load(&p->error));assert(!x4_stream_pipeline_close(p)&&!p->ring.occupied);x4_stream_pipeline_free(p);
    reset();p=new_pipeline(&stop,35);reject_scanout_finish=true;assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));pr=(Producer){p,1,0,false};produce(&pr);int result=0;for(unsigned i=0;i<1000&&!result;++i){result=x4_stream_pipeline_present(p,overlay,NULL);usleep(1000);}assert(result==X4_PIPELINE_STATE&&!p->view.conversions&&!flip_count);assert(!x4_stream_pipeline_close(p)&&!p->retained);x4_stream_pipeline_free(p);
    reset();p=new_pipeline(&stop,33);assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));close_failure=-74;assert(x4_stream_pipeline_close(p)==-74&&p->retained&&allocated&&handle_open);assert(join_count==3);assert(x4_stream_pipeline_close(p)==-74);assert(!p->view.display_detached);
    /* Host fixture cleanup only; production retains after a failed native close. */
    close_failure=0;assert(!close_display(p));p->retained=false;p->closed=true;x4_stream_pipeline_free(p);
    reset();p=new_pipeline(&stop,37);start_cleanup_retained=true;assert(!x4_stream_pipeline_start(p));pr=(Producer){p,1,0,false};produce(&pr);
    for(unsigned i=0;i<1000&&!atomic_load(&p->decoder_done);++i)usleep(1000);
    assert(atomic_load(&p->decoder_done)&&atomic_load(&p->error)==-80);
    assert(x4_stream_pipeline_close(p)==-81&&p->retained&&p->video.state&&native_closes==0&&join_count==3);
    /* Mock-only release after all host workers joined, never a production retry. */
    Native *n=p->video.state;for(unsigned i=0;i<4;++i)free(n->pixels[i]);free(n);p->video.state=NULL;p->retained=false;p->closed=true;x4_stream_pipeline_free(p);
    reset();p=new_pipeline(&stop,36);assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));close_failure=74;
    assert(x4_stream_pipeline_close(p)==-74&&p->retained&&allocated&&handle_open);
    X4StreamPipelineSnapshot retained;x4_stream_pipeline_snapshot(p,&retained);assert(retained.retained);
    close_failure=0;assert(!close_display(p));p->retained=false;p->closed=true;x4_stream_pipeline_free(p);
    reset();p=new_pipeline(&stop,39);
    /* Ownership negative: a fabricated host consumer violates the normal
     * joined-owner invariant. Pipeline free must report retention, leaving
     * both this lease and the parent-owned limiter binding alive. */
    const uint8_t held_payload[]={0,0,0,1,0x65,0x80},held_sps[]={0x67};
    assert(!x4_stream_pipeline_submit_parameters(p,held_payload,sizeof(held_payload),0,1,held_sps,sizeof(held_sps),NULL,0));
    X4StreamAuLease held_lease;X4StreamAuView held_view;
    assert(x4_stream_au_queue_try_take(p->au_queue,0,&held_lease,&held_view,NULL)==X4_STREAM_AU_OK);
    atomic_store(&p->dequeued,1); /* Fabricated host borrower took the real admission. */
    assert(x4_stream_pipeline_close(p)==X4_STREAM_AU_FATAL && p->retained);
    assert(!x4_stream_pipeline_export(p,NULL));
    assert(x4_stream_pipeline_free(p)<0 && p->retained);
    assert(held_view.data[4]==0x65 && held_view.data[5]==0x80 && p->diagnostics);
    assert(x4_stream_au_queue_release(p->au_queue,held_lease)==X4_STREAM_AU_OK);
    /* Fixture-only release after the fabricated borrower has finished. This
     * is never permission to retry failed native teardown in production. */
    p->retained=false;assert(!x4_stream_pipeline_free(p));
    puts("PASS thread/map/decode failures, cancellation while AU queue full and retained VideoOut close");
}
static void blocking_cancel_and_native_ownership(void)
{
    atomic_bool stop;uint64_t maximum=0;
    for(unsigned i=0;i<8;++i){reset();X4StreamPipeline *p=new_pipeline(&stop,60+i);
        assert(!x4_stream_pipeline_start(p));
        /* Measure cancellation of running blocking workers, not host thread
         * startup scheduling. Keep the strict 50ms assertion below. */
        uint64_t ready_deadline=sceKernelGetProcessTime()+1000000;
        while(!atomic_load_explicit(&p->decode_wait.wait_calls,memory_order_acquire)||
              !atomic_load_explicit(&p->converters[0].work_wait.wait_calls,memory_order_acquire)||
              !atomic_load_explicit(&p->converters[1].work_wait.wait_calls,memory_order_acquire)){
            assert(sceKernelGetProcessTime()<ready_deadline);usleep(100);}
        uint64_t began=sceKernelGetProcessTime();x4_stream_pipeline_request_stop(p);
        /* Consume cancellation's retained token, then prove the open error
         * screen does not turn its next main-loop wait into an active loop. */
        bool blocked=false;
        for(unsigned attempt=0;attempt<8&&!blocked;++attempt){
            uint64_t wait_begin=sceKernelGetProcessTime();assert(!x4_stream_pipeline_wait_present(p,5000));
            blocked=sceKernelGetProcessTime()-wait_begin>=4000;
        }
        assert(blocked); /* Done/publication notifications may race stop. */
        assert(!x4_stream_pipeline_close(p));uint64_t elapsed=sceKernelGetProcessTime()-began;
        if(elapsed>maximum)maximum=elapsed;
        printf("idle cancellation trial=%u elapsed_us=%llu joins=%u native_outputs=%u\n",i,
            (unsigned long long)elapsed,join_count,native_outputs);
        assert(elapsed<=50000&&join_count==3&&!native_outputs);
        assert(!x4_stream_pipeline_free(p));
    }
    printf("PASS idle worker cancellation/close max_us=%llu <=50000; stopped main retains blocking cadence\n",(unsigned long long)maximum);
    reset();X4StreamPipeline *p=new_pipeline(&stop,70);assert(!x4_stream_pipeline_start(p));
    atomic_store(&decode_pause_us,90000);atomic_store(&decode_native_running,false);
    /* The usual producer helper waits for retirement; this control must
     * return after submission so main can cancel while Decode is in flight. */
    Producer pr={p,1,0,true};produce(&pr);uint64_t deadline=sceKernelGetProcessTime()+1000000;
    while(!atomic_load_explicit(&decode_native_running,memory_order_acquire)){
        assert(sceKernelGetProcessTime()<deadline);usleep(100);}
    uint64_t began=sceKernelGetProcessTime();assert(p->video.state&&native_closes==0);
    x4_stream_pipeline_request_stop(p);assert(!x4_stream_pipeline_close(p));
    uint64_t elapsed=sceKernelGetProcessTime()-began;
    assert(elapsed>=50000&&!p->video.state&&!p->ring.occupied&&native_closes==1&&join_count==3);
    assert(p->decoder_join_wait_us>=50000&&p->native_decode_after_stop_us>=50000);
    assert(!p->native_decode_started_after_stop);assert(!x4_stream_pipeline_free(p));
    printf("PASS in-progress native Decode retained until real return/join; close_us=%llu (separate from cancellable waits)\n",(unsigned long long)elapsed);
}
static void destroy_failure_is_cold(void)
{
    atomic_bool stop;
    for(unsigned failure=0;failure<6;++failure){
        reset();X4StreamPipeline *p=new_pipeline(&stop,80+failure);
        assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_close(p));
        assert(p->closed&&join_count==3&&!p->retained);
        if(failure<4)x4_stream_wait_test_fail_destroy(failure+1,EBUSY);
        else x4_stream_mutex_test_fail_destroy(failure-3,EBUSY);
        assert(x4_stream_pipeline_free(p)==-EBUSY);
        assert(p->retained&&p->closed&&!p->au_queue&&p->diagnostics);
        /* No synchronization API may be called after any partial teardown:
         * some endpoints/gates are already destroyed and the AU queue freed.
         * A cold snapshot and repeated stop/close remain safe and observable. */
        uint64_t calls=x4_stream_wait_test_api_calls();
        X4StreamPipelineSnapshot blocking={0},nonblocking={0};
        x4_stream_pipeline_snapshot(p,&blocking);
        assert(x4_stream_pipeline_try_snapshot(p,&nonblocking));
        x4_stream_pipeline_request_stop(p);
        assert(x4_stream_pipeline_close(p)==-EBUSY);
        assert(x4_stream_pipeline_free(p)==X4_PIPELINE_STATE);
        assert(blocking.retained&&nonblocking.retained&&blocking.display_detached&&nonblocking.display_detached);
        assert(x4_stream_wait_test_api_calls()==calls);
        /* Test-only final release after proving production refuses retry.
         * No failed destroy released its object; successful destroys cleared
         * their initialized flags, so the fixture frees each live object once. */
        x4_stream_wait_test_fail_destroy(0,0);x4_stream_mutex_test_fail_destroy(0,0);
        for(unsigned i=0;i<2;++i)assert(!x4_stream_wait_destroy(&p->converters[i].work_wait));
        assert(!x4_stream_wait_destroy(&p->decode_wait));assert(!x4_stream_wait_destroy(&p->present_wait));
        if(p->ring_mutex_initialized)assert(!x4_stream_mutex_destroy(&p->ring_gate));
        if(p->status_mutex_initialized)assert(!x4_stream_mutex_destroy(&p->status_gate));
        x4_stream_diagnostics_free(p->diagnostics);free(p);
    }
    puts("PASS four semaphore and two mutex destroy failures: cold snapshots/stop/close, no stale API call, no retry or dangling AU queue");
}
static void dispatch_cancel_and_worker_failures(void)
{
    atomic_bool stop;
    for(unsigned fault=DISPATCH_CANCEL;fault<=DISPATCH_WAIT_ERROR;++fault){
        reset();X4StreamPipeline *p=new_pipeline(&stop,90+fault);
        if(fault==DISPATCH_CPU_ERROR)atomic_store_explicit(&cpu_fixture_hold,true,memory_order_release);
        assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));
        uint64_t deadline=sceKernelGetProcessTime()+1000000;
        if(fault==DISPATCH_CPU_ERROR)while(!atomic_load_explicit(&cpu_fixture_entered,memory_order_acquire)){
            assert(sceKernelGetProcessTime()<deadline);usleep(100);}
        Producer pr={p,1,0,false};produce(&pr);assert(!pr.result&&p->view.outputs==1);
        atomic_store_explicit(&dispatch_fault,fault,memory_order_release);
        int presented=x4_stream_pipeline_present(p,NULL,NULL);
        assert(p->converting&&p->work==1);
        assert(atomic_load_explicit(&p->converters[0].submitted,memory_order_acquire)==p->work);
        assert(atomic_load_explicit(&p->converters[1].submitted,memory_order_acquire)==p->work);
        if(fault==DISPATCH_CANCEL||fault==DISPATCH_CPU_ERROR)assert(!presented);
        else assert(presented==-EIO);
        if(fault==DISPATCH_CPU_ERROR){
            uint64_t limit=sceKernelGetProcessTime()+2000000;
            while(!p->view.presentations){assert(sceKernelGetProcessTime()<limit);
                assert(!x4_stream_pipeline_present(p,NULL,NULL));usleep(100);}
            assert(!atomic_load(&p->error)&&!stopping(p));
        }
        uint64_t began=sceKernelGetProcessTime();assert(!x4_stream_pipeline_close(p));
        uint64_t elapsed=sceKernelGetProcessTime()-began;
        assert(elapsed<500000&&join_count==3&&!p->retained&&!p->conversion_cleanup_failed);
        assert(!p->converting&&!atomic_load(&p->conversion_readers)&&!p->ring.occupied);
        assert(p->view.display_detached);
        assert(!allocated&&!handle_open&&native_closes==1);assert(!x4_stream_pipeline_free(p));
        printf("PASS dispatch fault=%u: both jobs published; finished/live proof, leases retired, clean join close_us=%llu (no 10s hold)\n",
            fault,(unsigned long long)elapsed);
    }
}
static void cpu_affinity_failure_continues_presenting(void)
{
    reset();atomic_bool stop;atomic_init(&stop,false);atomic_store(&cpu_calls_enabled,true);
    atomic_store(&cpu_affinity_fail_once,true);
    X4StreamRequestConfig request=x4_stream_request_config(X4_REQUEST_720P60,X4_COLOR_AUTO_709);
    request.cpu_variant=X4_CPU_2;request.decoder_depth=X4_DEPTH_1;
    X4StreamPipeline *p=x4_stream_pipeline_create_request(120,NULL,&stop,&request);assert(p);
    assert(!x4_stream_pipeline_start(p)&&!x4_stream_pipeline_display_open(p));
    Producer producer={p,6,0,false};produce(&producer);assert(!producer.result&&p->view.outputs==6);
    uint64_t deadline=sceKernelGetProcessTime()+3000000;
    while(p->view.presentations<1){assert(sceKernelGetProcessTime()<deadline);
        assert(!x4_stream_pipeline_present(p,NULL,NULL));usleep(100);}
    const X4StreamCpuReport *partial=&p->converters[1].cpu_report;
    assert(partial->result==-EACCES&&partial->affinity_result==-EACCES&&partial->partial_applied);
    assert(partial->before.valid&&partial->before.priority==612&&partial->before.affinity==0x2d);
    assert(partial->restore_priority_attempted&&partial->restore_affinity_attempted);
    assert(!partial->restore_priority_result&&!partial->restore_affinity_result);
    assert(partial->after.valid&&partial->after.priority==612&&partial->after.affinity==0x2d&&partial->outcome==X4_CPU_RESTORED);
    assert(!atomic_load(&p->converters[1].finished)&&!atomic_load(&p->error)&&!stopping(p));
    /* New input after the failed setter must still decode, convert and flip. */
    const uint8_t sps[]={0,0,0,1,0x67},pps[]={0,0,0,1,0x68,0x80};uint64_t identity=6;
    x4_stream_pipeline_au_received(p,false,sceKernelGetProcessTime());
    assert(!x4_stream_pipeline_submit_parameters(p,(const uint8_t*)&identity,8,identity*16667,identity,sps,sizeof(sps),pps,sizeof(pps)));
    while(p->view.presentations<2){assert(sceKernelGetProcessTime()<deadline);
        assert(!x4_stream_pipeline_present(p,NULL,NULL));usleep(100);}
    while(atomic_load_explicit(&p->consumed,memory_order_acquire)<7){
        assert(sceKernelGetProcessTime()<deadline);usleep(100);}
    assert(p->view.outputs==7&&!atomic_load(&p->error)&&!stopping(p));
    /* Existing audio/callback counters are attached to the captured CPU
     * variant. These are supplied observations, not a starvation diagnosis. */
    x4_stream_pipeline_note_callback_reject(p,false);
    x4_stream_pipeline_note_callback_reject(p,true);
    X4StreamAudioObservation audio={.available=true,.observed_us=sceKernelGetProcessTime(),
        .packets=100,.frames=98,.dropped=1,.lost=2,.bad_frames=3,.underflows=4,
        .startup_error=0,.error=-77,.stop_result=0};
    x4_stream_pipeline_record_audio(p,&audio);
    assert(!x4_stream_pipeline_close(p)&&!p->retained&&join_count==3);
    assert(!x4_stream_pipeline_export(p,NULL));assert(!x4_stream_pipeline_free(p));
    assert(!allocated&&!handle_open&&native_closes==1&&!atomic_load(&cpu_affinity_fail_once));
    puts("PASS actual pipeline CPU-2 converter1 Setaffinity(0x20)=-EACCES: captured real defaults, restored/observed/reported, video continued with later decode/conversion/flips and clean close");
}
#include "stream_main_polling_fixture.h"
int main(int argc,char **argv)
{
    assert(!setvbuf(stdout,NULL,_IOLBF,0));assert(argc==2);report_directory=argv[1];
    printf("fixture MAIN_BEGIN profile=%d\n",X4_STREAM_PROFILE);run();failures();
    admission_failure_without_wait(40,false);admission_failure_without_wait(41,true);
    blocking_cancel_and_native_ownership();
    destroy_failure_is_cold();
    dispatch_cancel_and_worker_failures();
    cpu_affinity_failure_continues_presenting();
    main_polling_measurements();
    printf("fixture MAIN_END profile=%d\n",X4_STREAM_PROFILE);return 0;
}
