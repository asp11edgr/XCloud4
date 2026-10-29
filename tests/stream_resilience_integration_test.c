/* SPDX-License-Identifier: GPL-3.0-only */
/* HOST integration: real SPS parser, raw adapter, scheduler, ring, conversion,
 * diagnostics and exporter. Only native/kernel/VideoOut/thread/clock calls are
 * substituted. Report fopen maps the fixed /data basename to a real temporary
 * directory, preserving real exclusive creation, fflush/fsync/close failures.
 * Synthetic Annex-B slices drive a native decoder double; no PS4 codec,
 * server retransmission, throughput, color/coherency or ownership claim. */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/live_h264.c"
#include <orbis/VideoOut.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define main unused_synthetic_sps_unit_entry
#include "stream_sps_test.c"
#undef main

static const char *report_directory;
static FILE *report_fopen(const char *path,const char *mode)
{
    const char *prefix="/data/xcloud4-stream-";char local[768];
    assert(!strncmp(path,prefix,strlen(prefix)) && !strcmp(mode,"wbx"));
    const char *base=strrchr(path,'/');assert(base && !strchr(base+1,'/'));
    int n=snprintf(local,sizeof(local),"%s/%s",report_directory,base+1);
    assert(n>0 && (size_t)n<sizeof(local));return fopen(local,mode);
}
int sceKernelFsync(int descriptor);
#include "../src/video/stream_wait.h"
static int fixture_wait_block(X4StreamWait *,unsigned);
#define fopen report_fopen
#define x4_stream_wait_block fixture_wait_block
#include "../src/video/stream_pipeline.c"
#undef x4_stream_wait_block
#undef fopen
/* The startup harness links these same fixtures separately. Standalone
 * resilience keeps its original runner and completes only missing owners. */
#ifndef X4_STREAM_RESILIENCE_NO_MAIN
#include "stream_startup_metrics_stubs.c"
#endif

enum { HOST_RESERVATIONS=256, HOST_COMMANDS=128 };
enum { GOOD, ERROR_FRAME, NATIVE_FAILURE, NO_OUTPUT, INVALID_LAYOUT };
typedef struct {void *pointer;size_t bytes;int type;bool owned,mapped,native_buffer;} HostMemory;
typedef struct {const void *pointer;size_t bytes;uint64_t hash;unsigned submitted;} HostInput;
typedef struct {
    OrbisVideodec2DecoderConfigInfo config;pthread_t owner;unsigned generation,calls;
    OrbisVideodec2FrameBuffer pending;unsigned pending_tag,pending_behavior;
    HostInput inputs[4];void *held_outputs[5];unsigned held_count;
} HostDecoder;
static HostMemory host_memory[HOST_RESERVATIONS];
static OrbisVideodec2DecoderConfigInfo query_configs[64],create_configs[64];
static unsigned commands[HOST_COMMANDS],next_memory,queries,creates,deletes,native_decodes;
static unsigned open_decoders,threads_created,threads_joined,presented,output_widths[256];
static unsigned returned_tags[256],returned_count,without_output_calls,flush_calls;
static unsigned fail_query_count,fail_create_count,over_budget_count;
static bool fail_delete,cancel_on_delete,fail_video_close;
static unsigned video_close_calls;
static atomic_bool conversion_hold,conversion_entered[2];
static atomic_bool native_hold,native_entered;
static _Thread_local unsigned thread_role;
static _Thread_local uint64_t decoder_last_clock_read;
static pthread_t main_owner;
static atomic_bool external_stop;
static atomic_uint_fast64_t controlled_clock_us,decoder_idle_clock_seen,decoder_idle_sequence;
static X4StreamPipeline *current;
static int video_handle,pending_slot=-1,screen_slot=-1;
static uint32_t *scanout[3],pending_pixel,screen_pixel;
static uint64_t pending_argument,shown_argument;
static unsigned pending_polls;

static uint64_t host_wall_us(void)
{
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
uint64_t sceKernelGetProcessTime(void)
{
    if(thread_role==1||thread_role==2){
        unsigned i=thread_role-1;
        if(atomic_load_explicit(&conversion_hold,memory_order_acquire)){
            atomic_store_explicit(&conversion_entered[i],true,memory_order_release);
            while(atomic_load_explicit(&conversion_hold,memory_order_acquire)){
                struct timespec pause={0,100000};nanosleep(&pause,NULL);
            }
        }
    }
    uint64_t controlled=atomic_load(&controlled_clock_us),value=controlled?controlled:host_wall_us();
    if(thread_role==3)decoder_last_clock_read=value;
    return value;
}
int sceKernelUsleep(unsigned us)
{
    if(thread_role==3&&atomic_load(&controlled_clock_us)){
        atomic_store_explicit(&decoder_idle_clock_seen,decoder_last_clock_read,memory_order_release);
        atomic_fetch_add_explicit(&decoder_idle_sequence,1,memory_order_release);
    }
    struct timespec t={(time_t)(us/1000000),(long)(us%1000000)*1000};return nanosleep(&t,NULL);
}
static int fixture_wait_block(X4StreamWait *w,unsigned us)
{
    if(thread_role==3&&atomic_load(&controlled_clock_us)){
        atomic_store_explicit(&decoder_idle_clock_seen,decoder_last_clock_read,memory_order_release);
        atomic_fetch_add_explicit(&decoder_idle_sequence,1,memory_order_release);
    }
    return x4_stream_wait_block(w,us);
}
int sceKernelFsync(int fd){return fsync(fd);}
size_t sceKernelGetDirectMemorySize(void){return 512u*1024u*1024u;}
int sceKernelAllocateDirectMemory(off_t from,off_t to,size_t bytes,size_t alignment,int type,off_t *offset)
{
    (void)from;(void)to;assert(bytes && next_memory+1<HOST_RESERVATIONS);
    unsigned i=++next_memory;void *memory=NULL;
    assert(!posix_memalign(&memory,alignment,bytes));
    host_memory[i]=(HostMemory){memory,bytes,type,true,false,!pthread_equal(main_owner,pthread_self())};*offset=(off_t)i;return 0;
}
int sceKernelMapDirectMemory(void **memory,size_t bytes,int prot,int flags,off_t offset,size_t alignment)
{
    (void)prot;(void)flags;(void)alignment;assert(offset>0 && (unsigned)offset<=next_memory);
    HostMemory *m=&host_memory[offset];assert(m->owned&&!m->mapped&&m->bytes==bytes);
    *memory=m->pointer;m->mapped=true;return 0;
}
int sceKernelMunmap(void *memory,size_t bytes)
{
    for(unsigned i=1;i<=next_memory;++i)if(host_memory[i].pointer==memory){
        HostMemory *m=&host_memory[i];assert(m->owned&&m->mapped&&m->bytes==bytes);
        m->mapped=false;return 0;
    }
    assert(!"unmap of foreign memory");return -1;
}
int sceKernelReleaseDirectMemory(off_t offset,size_t bytes)
{
    assert(offset>0 && (unsigned)offset<=next_memory);HostMemory *m=&host_memory[offset];
    /* Independent VideoOut memory can close while a failed native Delete
     * retains decoder-owned blocks. That does not retire native ownership. */
    assert(!open_decoders||!m->native_buffer);
    assert(m->owned&&!m->mapped&&m->bytes==bytes);free(m->pointer);m->pointer=NULL;m->owned=false;return 0;
}
static unsigned memory_owned(void)
{unsigned n=0;for(unsigned i=1;i<=next_memory;++i)n+=host_memory[i].owned;return n;}
static int32_t host_query_compute(OrbisVideodec2ComputeMemoryInfo *m)
{m->cpuGpuMemorySize=16384;return 0;}
static int32_t host_allocate_queue(const OrbisVideodec2ComputeConfigInfo *c,const OrbisVideodec2ComputeMemoryInfo *m,void **queue)
{assert(c->thisSize==sizeof(*c)&&m->pCpuGpuMemory);*queue=(void*)(uintptr_t)0x1234;return 0;}
static int32_t host_release_queue(void *q){assert(q==(void*)(uintptr_t)0x1234);return 0;}
static int32_t host_query_decoder(const OrbisVideodec2DecoderConfigInfo *c,OrbisVideodec2DecoderMemoryInfo *m)
{
    assert(!pthread_equal(main_owner,pthread_self()) && queries<64 && c->thisSize==sizeof(*c) &&
        (c->decodePipelineDepth==1||c->decodePipelineDepth==2));
    query_configs[queries++]=*c;
    if(fail_query_count){--fail_query_count;return -10101;} /* Scripted unknown native return. */
    m->cpuMemorySize=m->gpuMemorySize=m->cpuGpuMemorySize=16384;
    if(over_budget_count){--over_budget_count;m->cpuMemorySize=m->gpuMemorySize=64u*1024u*1024u;}
    m->maxFrameBufferSize=(size_t)c->maxFrameWidth*c->maxFrameHeight*3u/2u;
    m->frameBufferAlignment=16384;return 0;
}
static int32_t host_create_decoder(const OrbisVideodec2DecoderConfigInfo *c,const OrbisVideodec2DecoderMemoryInfo *m,void **decoder)
{
    assert(creates<64 && queries && !memcmp(c,&query_configs[queries-1],sizeof(*c)));
    create_configs[creates++]=*c;
    if(fail_create_count){--fail_create_count;return -10102;}
    assert(!open_decoders && m->pCpuMemory&&m->pGpuMemory&&m->pCpuGpuMemory);
    HostDecoder *d=calloc(1,sizeof(*d));assert(d);
    d->config=*c;d->owner=pthread_self();d->generation=creates;
    *decoder=d;++open_decoders;return 0;
}
static uint64_t input_hash(const void *,size_t);
static int32_t host_delete_decoder(void *decoder)
{
    HostDecoder *d=decoder;assert(d && pthread_equal(d->owner,pthread_self()) && open_decoders==1);
    for(unsigned i=0;i<4;++i)if(d->inputs[i].pointer)
        assert(input_hash(d->inputs[i].pointer,d->inputs[i].bytes)==d->inputs[i].hash);
    ++deletes;
    if(fail_delete)return -10103;
    free(d);--open_decoders;
    if(cancel_on_delete)atomic_store_explicit(&external_stop,true,memory_order_release);
    return 0;
}
static uint64_t input_hash(const void *pointer,size_t bytes)
{
    const uint8_t *p=pointer;uint64_t hash=UINT64_C(1469598103934665603);
    for(size_t i=0;i<bytes;++i){hash^=p[i];hash*=UINT64_C(1099511628211);}return hash;
}
static void remember_input(HostDecoder *d,const OrbisVideodec2InputData *in)
{
    HostInput *reuse=NULL;
    for(unsigned i=0;i<4;++i){HostInput *old=&d->inputs[i];
        if(old->pointer&&d->calls-old->submitted<4u)
            assert(input_hash(old->pointer,old->bytes)==old->hash); /* Three later returns not complete. */
        if(old->pointer==in->pAuData){assert(d->calls-old->submitted>=4u);reuse=old;}
        else if(!old->pointer&&!reuse)reuse=old;
    }
    assert(reuse);*reuse=(HostInput){in->pAuData,in->auSize,input_hash(in->pAuData,in->auSize),d->calls};
}
static void hold_output(HostDecoder *d,const OrbisVideodec2FrameBuffer *f)
{
    assert(d->held_count<5);
    for(unsigned i=0;i<d->held_count;++i)assert(d->held_outputs[i]!=f->pFrameBuffer);
    d->held_outputs[d->held_count++]=f->pFrameBuffer;
}
static void return_output(HostDecoder *d,void *pointer)
{
    for(unsigned i=0;i<d->held_count;++i)if(d->held_outputs[i]==pointer){
        d->held_outputs[i]=d->held_outputs[--d->held_count];return;}
    assert(!"returned output was not retained");
}
static void fill_frame(const HostDecoder *d,OrbisVideodec2FrameBuffer *f,unsigned tag)
{
    unsigned w=(unsigned)d->config.maxFrameWidth,h=(unsigned)d->config.maxFrameHeight;
    size_t y=(size_t)w*h;assert(f->frameBufferSize>=y+y/2);
    memset(f->pFrameBuffer,32+tag%160,y);memset((uint8_t*)f->pFrameBuffer+y,128,y/2);
}
static void emit_frame(HostDecoder *d,const OrbisVideodec2FrameBuffer *f,unsigned tag,unsigned behavior,
    OrbisVideodec2OutputInfo *out)
{
    unsigned w=(unsigned)d->config.maxFrameWidth,h=(unsigned)d->config.maxFrameHeight;
    out->isValid=true;out->isErrorFrame=behavior==ERROR_FRAME;out->pictureCount=1;out->codecType=1;
    out->frameWidth=w;out->frameHeight=h;out->framePitch=behavior==INVALID_LAYOUT?1:w;
    out->pFrameBuffer=f->pFrameBuffer;out->frameBufferSize=f->frameBufferSize;
    assert(returned_count<256);returned_tags[returned_count++]=tag;
}
static int32_t host_decode(void *decoder,const OrbisVideodec2InputData *in,OrbisVideodec2FrameBuffer *f,OrbisVideodec2OutputInfo *out)
{
    HostDecoder *d=decoder;assert(d && pthread_equal(d->owner,pthread_self()));
    assert(in->pAuData && in->auSize && f->pFrameBuffer && f->frameBufferSize);
    unsigned tag=((const uint8_t*)in->pAuData)[in->auSize-1]&0x7f;
    assert(tag<HOST_COMMANDS && native_decodes<256);output_widths[native_decodes++]=(unsigned)d->config.maxFrameWidth;
    unsigned behavior=commands[tag];f->isAccepted=true;++d->calls;
    if(d->config.decodePipelineDepth==2){
        remember_input(d,in);hold_output(d,f);
        /* Deliberately false even though native ownership is retained: the
         * real adapter must use its conservative ledger, never isAccepted. */
        f->isAccepted=false;
        if(atomic_load_explicit(&native_hold,memory_order_acquire)){
            atomic_store_explicit(&native_entered,true,memory_order_release);
            while(atomic_load_explicit(&native_hold,memory_order_acquire)){
                struct timespec pause={0,100000};nanosleep(&pause,NULL);}
        }
        if(behavior==NATIVE_FAILURE)return -10104; /* Both offers remain held until Delete. */
        fill_frame(d,f,tag);
        OrbisVideodec2FrameBuffer previous=d->pending;
        unsigned previous_tag=d->pending_tag,previous_behavior=d->pending_behavior;
        d->pending=*f;d->pending_tag=tag;d->pending_behavior=behavior;
        if(!previous.pFrameBuffer){++without_output_calls;return 0;}
        emit_frame(d,&previous,previous_tag,previous_behavior,out);return_output(d,previous.pFrameBuffer);
        return 0;
    }
    if(behavior==NATIVE_FAILURE)return -10104; /* Unknown ABI return; ownership uncertain. */
    if(behavior==NO_OUTPUT){++without_output_calls;return 0;}
    fill_frame(d,f,tag);emit_frame(d,f,tag,behavior,out);return 0;
}
static int32_t host_flush(void *decoder,OrbisVideodec2FrameBuffer *f,OrbisVideodec2OutputInfo *out)
{(void)decoder;(void)out;++flush_calls;f->isAccepted=false;return 0;}
int x4_module_open(const char *name){assert(!strcmp(name,"libSceVideodec2"));return 1;}
int x4_module_symbol(int handle,const char *name,void **out)
{
    assert(handle==1);
#define NATIVE(n,f) if(!strcmp(name,"sceVideodec2" n)){*out=(void*)(f);return 0;}
    NATIVE("QueryComputeMemoryInfo",host_query_compute) NATIVE("AllocateComputeQueue",host_allocate_queue)
    NATIVE("ReleaseComputeQueue",host_release_queue) NATIVE("QueryDecoderMemoryInfo",host_query_decoder)
    NATIVE("CreateDecoder",host_create_decoder) NATIVE("DeleteDecoder",host_delete_decoder)
    NATIVE("Decode",host_decode) NATIVE("Flush",host_flush)
#undef NATIVE
    assert(!"unknown native export");return -1;
}
int scePthreadCreate(OrbisPthread *id,const pthread_attr_t *attr,void *(*entry)(void *),void *arg,const char *name)
{(void)name;return attr?pthread_create(id,attr,entry,arg):EAGAIN;}
/* CPU-0 fixtures do not change scheduling; CPU variants are exercised by
 * test_stream_cpu_sdp.sh against the real CPU module with explicit doubles. */
OrbisPthread scePthreadSelf(void){return pthread_self();}
int32_t scePthreadGetprio(OrbisPthread thread,int32_t *priority)
{(void)thread;(void)priority;assert(!"unexpected CPU getter in CPU-0 fixture");return -1;}
int32_t scePthreadAttrInit(OrbisPthreadAttr *attr)
{(void)attr;assert(!"unexpected CPU attr init in CPU-0 fixture");return -1;}
int32_t scePthreadAttrGet(OrbisPthread thread,OrbisPthreadAttr *attr)
{(void)thread;(void)attr;assert(!"unexpected CPU attr get in CPU-0 fixture");return -1;}
int32_t scePthreadAttrGetaffinity(const OrbisPthreadAttr *attr,void *mask)
{(void)attr;(void)mask;assert(!"unexpected CPU affinity get in CPU-0 fixture");return -1;}
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr)
{(void)attr;assert(!"unexpected CPU attr destroy in CPU-0 fixture");return -1;}
int scePthreadSetprio(OrbisPthread thread,int priority){(void)thread;(void)priority;assert(!"unexpected CPU change in CPU-0 fixture");return -1;}
int32_t scePthreadSetaffinity(OrbisPthread thread,uint64_t mask){(void)thread;(void)mask;assert(!"unexpected affinity in CPU-0 fixture");return -1;}
int scePthreadJoin(OrbisPthread id,void **result)
{int rc=pthread_join(id,result);if(!rc)++threads_joined;return rc;}
typedef struct {void *(*entry)(void*);void *arg;unsigned role;} ThreadStart;
static void *host_thread_entry(void *arg)
{ThreadStart start=*(ThreadStart*)arg;free(arg);thread_role=start.role;return start.entry(start.arg);}
int x4_probe_thread_create(OrbisPthread *id,X4ProbeThread *t,void *(*entry)(void*),void *arg,const char *name)
{
    memset(t,0,sizeof(*t));snprintf(t->report.name,sizeof(t->report.name),"%s",name);
    t->report.configured_bytes=t->report.reported_bytes=262144;
    unsigned role=strstr(name,"-c0")?1:strstr(name,"-c1")?2:3;
    ThreadStart *start=malloc(sizeof(*start));assert(start);*start=(ThreadStart){entry,arg,role};
    pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,262144));
    int rc=pthread_create(id,&attr,host_thread_entry,start);assert(!pthread_attr_destroy(&attr));
    if(rc)free(start);else ++threads_created;t->created=!rc;t->report.create_rc=rc;return rc;
}
void x4_probe_thread_joined(X4ProbeThread *t,int rc)
{t->report.join_rc=rc;if(!rc)t->created=false;}
int sceVideoOutOpen(int user,int bus,int index,const void *p)
{(void)user;(void)bus;(void)index;(void)p;assert(pthread_equal(main_owner,pthread_self())&&!video_handle);video_handle=7;return video_handle;}
int sceVideoOutClose(int h)
{assert(pthread_equal(main_owner,pthread_self())&&h==7&&video_handle==7);++video_close_calls;if(fail_video_close)return -10105;video_handle=0;pending_slot=screen_slot=-1;return 0;}
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *a,int f,int t,int r,unsigned w,unsigned h,unsigned pitch)
{(void)f;(void)t;(void)r;*a=(OrbisVideoOutBufferAttribute){w,h,pitch};}
int sceVideoOutRegisterBuffers(int h,int first,void *const *pixels,int count,const OrbisVideoOutBufferAttribute *a)
{assert(h==7&&!first&&count==3&&a->width==1920&&a->height==1080&&a->pitch==1920);for(unsigned i=0;i<3;++i)scanout[i]=pixels[i];return 0;}
int sceVideoOutSetFlipRate(int h,int rate){(void)rate;assert(h==7);return 0;}
int sceVideoOutSubmitFlip(int h,int slot,int vsync,int64_t argument)
{
    (void)vsync;assert(h==7&&video_handle==7&&pending_slot<0&&slot!=screen_slot&&slot>=0&&slot<3);
    assert((uint64_t)argument>shown_argument);pending_slot=slot;pending_argument=(uint64_t)argument;pending_polls=2;
    pending_pixel=scanout[slot][1920u*1080u-1];assert((pending_pixel>>24)==255);++presented;return 0;
}
int sceVideoOutGetFlipStatus(int h,OrbisVideoOutFlipStatus *out)
{
    assert(h==7&&video_handle==7);
    if(screen_slot>=0)assert(scanout[screen_slot][1920u*1080u-1]==screen_pixel);
    if(pending_slot>=0){assert(scanout[pending_slot][1920u*1080u-1]==pending_pixel);
        if(pending_polls)--pending_polls;
        else {screen_slot=pending_slot;pending_slot=-1;shown_argument=pending_argument;screen_pixel=pending_pixel;}}
    *out=(OrbisVideoOutFlipStatus){.num=presented,.flipArg=(int64_t)shown_argument,.ptime=0};return 0;
}
int sceGnmSubmitDone(void){return 0;}

static void reset_host(void)
{
    assert(!memory_owned()&&!open_decoders&&!video_handle);
    memset(host_memory,0,sizeof(host_memory));memset(query_configs,0,sizeof(query_configs));memset(create_configs,0,sizeof(create_configs));
    memset(commands,0,sizeof(commands));memset(output_widths,0,sizeof(output_widths));
    memset(returned_tags,0,sizeof(returned_tags));returned_count=without_output_calls=flush_calls=0;
    next_memory=queries=creates=deletes=native_decodes=threads_created=threads_joined=presented=0;
    fail_query_count=fail_create_count=over_budget_count=0;fail_delete=cancel_on_delete=fail_video_close=false;video_close_calls=0;
    pending_slot=screen_slot=-1;pending_argument=shown_argument=0;pending_polls=0;current=NULL;
    atomic_store(&external_stop,false);atomic_store(&conversion_hold,false);
    atomic_store(&native_hold,false);atomic_store(&native_entered,false);
    atomic_store(&controlled_clock_us,0);atomic_store(&decoder_idle_clock_seen,0);atomic_store(&decoder_idle_sequence,0);
    for(unsigned i=0;i<2;++i)atomic_store(&conversion_entered[i],false);
    main_owner=pthread_self();thread_role=0;
}
static Fixture dimensions(unsigned width,unsigned height)
{
    Fixture f=standard();f.profile=77;f.constraints=0;f.width=width;f.height=height;
    f.crop_b=height==1088?8:0;f.level=width==1920?42:32;f.refs=1;f.dpb=1;f.matrix=1;return f;
}
static X4StreamPipeline *begin_case_depth(unsigned session,X4StreamDecoderDepth depth)
{
    reset_host();X4StreamRequestConfig request=x4_stream_request_config(X4_STREAM_PROFILE==2?X4_REQUEST_1080P_A:X4_REQUEST_720P60,X4_COLOR_AUTO_709);
    request.decoder_depth=(unsigned char)depth;
    current=x4_stream_pipeline_create_request(session,NULL,&external_stop,&request);assert(current);
    assert(!x4_stream_pipeline_start(current));assert(!x4_stream_pipeline_display_open(current));return current;
}
/* Preserve the fifteen previous fixtures' synchronous depth-one contract. */
static X4StreamPipeline *begin_case(unsigned session)
{return begin_case_depth(session,X4_DEPTH_1);}
static int submit_frame(X4StreamPipeline *p,const Fixture *f,unsigned tag,bool idr)
{
    uint8_t sps[512],au[1024];const uint8_t pps[]={0,0,0,1,0x68,0xce,0x38,0x80};
    size_t n=fixture(sps,f);memcpy(au,sps,n);memcpy(au+n,pps,sizeof(pps));size_t bytes=n+sizeof(pps);
    const uint8_t slice[]={0,0,0,1,0x65,0x80};memcpy(au+bytes,slice,sizeof(slice));au[bytes+4]=idr?0x65:0x41;
    bytes+=sizeof(slice);au[bytes++]=(uint8_t)(0x80|tag);
    x4_stream_pipeline_au_received(p,idr,sceKernelGetProcessTime());
    int rc=x4_stream_pipeline_submit_parameters(p,au,bytes,(uint64_t)tag*16667,tag,sps,n,pps,sizeof(pps));
    if(rc){X4StreamAuStats q={0};int qrc=x4_stream_au_queue_try_snapshot(p->au_queue,&q);
        fprintf(stderr,"fixture submit tag=%u IDR=%u rc=%d p_error=%d p_wait=%u qrc=%d q_wait=%u ready=%u inflight=%u resident=%u dropped_busy=%llu dropped_late=%llu rejected_wait=%llu transitions=%llu\n",
            tag,idr?1u:0u,rc,atomic_load(&p->error),p->waiting_idr?1u:0u,qrc,q.waiting_idr?1u:0u,q.ready_entries,q.inflight_entries,q.resident_entries,
            (unsigned long long)q.dropped_busy,(unsigned long long)q.dropped_late,(unsigned long long)q.rejected_incoming_wait_idr,(unsigned long long)q.recovery_transitions);}
    return rc;
}
static X4StreamPipelineSnapshot snapshot(X4StreamPipeline *p)
{X4StreamPipelineSnapshot s;x4_stream_pipeline_snapshot(p,&s);return s;}
static void pump_outputs(X4StreamPipeline *p,uint64_t wanted)
{
    uint64_t deadline=host_wall_us()+10000000;
    for(;;){assert(!x4_stream_pipeline_present(p,NULL,NULL));X4StreamPipelineSnapshot s=snapshot(p);
        assert(!s.error);lock(&p->ring_gate);bool no_ready=p->ring.ready==0;unlock(&p->ring_gate);
        if(s.outputs>=wanted&&s.presentations&&s.conversions&&!p->converting&&p->display.pending<0&&no_ready)return;
        assert(host_wall_us()<deadline);sceKernelUsleep(100);
    }
}
static void pump_dequeued(X4StreamPipeline *p,uint64_t wanted)
{
    uint64_t deadline=host_wall_us()+10000000;
    while(atomic_load_explicit(&p->dequeued,memory_order_acquire)<wanted){
        int rc=x4_stream_pipeline_present(p,NULL,NULL);assert(!rc);assert(host_wall_us()<deadline);sceKernelUsleep(100);
    }
    /* Dequeue precedes native work. Let the same AU leave the actual owner. */
    while(atomic_load_explicit(&p->consumed,memory_order_acquire)<wanted&&!snapshot(p).error){
        assert(!x4_stream_pipeline_present(p,NULL,NULL));assert(host_wall_us()<deadline);sceKernelUsleep(100);
    }
}
static void freeze_clock(void)
{atomic_store(&controlled_clock_us,host_wall_us());}
static void advance_clock_and_wait_for_idle_poll(X4StreamPipeline *p,uint64_t delta)
{
    uint64_t sequence=atomic_load_explicit(&decoder_idle_sequence,memory_order_acquire);
    uint64_t target=atomic_fetch_add(&controlled_clock_us,delta)+delta;
    uint64_t deadline=host_wall_us()+10000000;
    while((atomic_load_explicit(&decoder_idle_clock_seen,memory_order_acquire)<target||
        atomic_load_explicit(&decoder_idle_sequence,memory_order_acquire)==sequence)&&!snapshot(p).error){
        assert(host_wall_us()<deadline);sceKernelUsleep(100);}
}
static bool consume_keyframe_after_publication(X4StreamPipeline *p)
{
    uint64_t deadline=host_wall_us()+2000000;
    do {
        if(x4_stream_pipeline_take_keyframe_request(p)){
            x4_stream_pipeline_note_keyframe_request(p,sceKernelGetProcessTime());return true;}
        if(snapshot(p).error||host_wall_us()>=deadline)return false;
        sceKernelUsleep(100);
    } while(true);
}
static void await_consumed_without_present(X4StreamPipeline *p,uint64_t wanted)
{
    uint64_t deadline=host_wall_us()+10000000;
    while(atomic_load_explicit(&p->consumed,memory_order_acquire)<wanted){
        assert(!snapshot(p).error&&host_wall_us()<deadline);sceKernelUsleep(100);}
}
static void finish_case(X4StreamPipeline *p)
{
    assert(!x4_stream_pipeline_close(p));assert(!snapshot(p).retained);
    assert(threads_created==3&&threads_joined==3&&!memory_owned()&&!open_decoders&&!video_handle);
    assert(!p->ring.occupied);assert(!x4_stream_pipeline_export(p,NULL));x4_stream_pipeline_free(p);current=NULL;
}
static void intermittent_errors(void)
{
    X4StreamPipeline *p=begin_case(101);Fixture f=dimensions(1280,720);
    assert(!submit_frame(p,&f,1,true));pump_outputs(p,1);
    freeze_clock();
    commands[2]=ERROR_FRAME;assert(!submit_frame(p,&f,2,true));pump_dequeued(p,2);
    assert(snapshot(p).frame_errors==1&&!snapshot(p).error);
    assert(consume_keyframe_after_publication(p));assert(!x4_stream_pipeline_take_keyframe_request(p));
    assert(!submit_frame(p,&f,3,false));pump_dequeued(p,3);assert(native_decodes==2&&snapshot(p).frame_errors==1);
    assert(!submit_frame(p,&f,4,true));pump_outputs(p,2);assert(!snapshot(p).consecutive_frame_errors);
    commands[5]=NATIVE_FAILURE;assert(!submit_frame(p,&f,5,true));pump_dequeued(p,5);
    assert(snapshot(p).frame_errors==2&&!snapshot(p).error);assert(consume_keyframe_after_publication(p));
    assert(!submit_frame(p,&f,6,false));pump_dequeued(p,6);assert(native_decodes==4);
    assert(!submit_frame(p,&f,7,true));pump_outputs(p,3);
    assert(creates==2&&deletes==1&&!atomic_load(&external_stop));finish_case(p);
    puts("PASS real integration B2: error frame discarded, unknown native failure restarts only after Delete, local keyframe flag consumed, later IDR presents");
}
static void persistent_errors(void)
{
    X4StreamPipeline *p=begin_case(102);Fixture f=dimensions(1280,720);
    freeze_clock();
    commands[1]=ERROR_FRAME;assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    uint64_t absolute=p->idr_recovery.deadline_us;
    for(unsigned i=2;i<=20;++i){assert(!submit_frame(p,&f,i,false));pump_dequeued(p,i);
        assert(snapshot(p).frame_errors==1&&p->idr_recovery.deadline_us==absolute);}
    assert(native_decodes==1&&snapshot(p).waiting_idr);
    advance_clock_and_wait_for_idle_poll(p,6000000); /* No AU: idle poll must expire. */
    X4StreamPipelineSnapshot s=snapshot(p);assert(s.error==X4_STREAM_IDR_WAIT_TIMEOUT&&s.frame_errors==1&&s.consecutive_frame_errors==1);
    assert(submit_frame(p,&f,21,true)==X4_STREAM_IDR_WAIT_TIMEOUT&&native_decodes==1);
    assert(!atomic_load(&external_stop));assert(!x4_stream_pipeline_halt_video(p));s=snapshot(p);
    assert(s.display_detached&&!s.retained&&!memory_owned()&&!open_decoders&&!video_handle);
    x4_stream_pipeline_received(p,31,sceKernelGetProcessTime()); /* Callback diagnostics outlive video shutdown. */
    finish_case(p);puts("PASS BAD_FRAME: dependent frames do not Decode/count errors/renew deadline; no-AU poll expires by five seconds, six-second IDR rejected and RTC/audio stop remains false");
}
static void delayed_idr_recovers(void)
{
    X4StreamPipeline *p=begin_case(114);Fixture f=dimensions(1280,720);
    assert(!submit_frame(p,&f,1,true));pump_outputs(p,1);
    freeze_clock();
    /* Preserve a READY output from before the damaged frame. Presenting it
     * during the wait consumes the interval's initial WAIT_IDR evidence. The
     * recovered flip two seconds later must get the re-armed end evidence. */
    assert(!submit_frame(p,&f,2,false));await_consumed_without_present(p,2);
    commands[3]=ERROR_FRAME;assert(!submit_frame(p,&f,3,false));await_consumed_without_present(p,3);
    assert(snapshot(p).waiting_idr&&snapshot(p).frame_errors==1);
    pump_outputs(p,2);assert(snapshot(p).presentations==2&&snapshot(p).waiting_idr);
    uint64_t original=p->idr_recovery.deadline_us;unsigned requests=0;
    advance_clock_and_wait_for_idle_poll(p,0);
    for(unsigned i=0;i<10;++i){
        if(x4_stream_pipeline_take_keyframe_request(p)){++requests;x4_stream_pipeline_note_keyframe_request(p,sceKernelGetProcessTime());}
        advance_clock_and_wait_for_idle_poll(p,200000);
        assert(!snapshot(p).error&&p->idr_recovery.deadline_us==original);
    }
    assert(requests==5); /* Exactly 0/.4/.8/1.2/1.6 s with acknowledged 200ms steps. */
    assert(!submit_frame(p,&f,4,false));pump_dequeued(p,4);assert(native_decodes==3);
    assert(!submit_frame(p,&f,5,true));pump_outputs(p,3);
    assert(!snapshot(p).waiting_idr&&!snapshot(p).consecutive_frame_errors&&native_decodes==4);
    assert(p->idr_recovery.recoveries==1&&creates==1&&!deletes&&!atomic_load(&external_stop));
    finish_case(p);puts("PASS real scheduler/native double: IDR delayed two controlled seconds recovers, periodic requests continue without AUs and dependent frames are never decoded");
}
static void bad_idrs_never_renew_deadline(void)
{
    X4StreamPipeline *p=begin_case(115);Fixture f=dimensions(1280,720);
    freeze_clock();
    commands[1]=ERROR_FRAME;assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    uint64_t absolute=p->idr_recovery.deadline_us;
    for(unsigned i=2;i<=17;++i){
        advance_clock_and_wait_for_idle_poll(p,300000);commands[i]=ERROR_FRAME;
        assert(!submit_frame(p,&f,i,true));pump_dequeued(p,i);
        assert(!snapshot(p).error&&snapshot(p).waiting_idr&&p->idr_recovery.deadline_us==absolute);
    }
    assert(native_decodes==17&&snapshot(p).frame_errors==17&&p->idr_recovery.waits==1&&!p->idr_recovery.recoveries);
    advance_clock_and_wait_for_idle_poll(p,300000);
    assert(snapshot(p).error==X4_STREAM_IDR_WAIT_TIMEOUT&&native_decodes==17);
    atomic_fetch_add(&controlled_clock_us,900000);assert(submit_frame(p,&f,18,true)==X4_STREAM_IDR_WAIT_TIMEOUT);
    finish_case(p);puts("PASS periodic damaged IDRs never reset the absolute five-second deadline or fall back to a ten-BAD_FRAME limit");
}
static void resolution_changes(void)
{
    X4StreamPipeline *p=begin_case(103);Fixture a=dimensions(1280,720),b=dimensions(1920,1088);
    assert(!submit_frame(p,&a,1,true));pump_outputs(p,1);
    assert(!submit_frame(p,&a,2,true));uint64_t deadline=sceKernelGetProcessTime()+10000000;
    while(snapshot(p).outputs<2){assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    atomic_store(&conversion_hold,true);assert(!x4_stream_pipeline_present(p,NULL,NULL));
    while(!atomic_load(&conversion_entered[0])||!atomic_load(&conversion_entered[1])){assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    assert(p->converting);assert(!submit_frame(p,&b,3,true));
    while(!snapshot(p).reconfiguring){assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    assert(creates==1&&deletes==0); /* Old readers still own their actual output. */
    atomic_store(&conversion_hold,false);pump_outputs(p,3);
    assert(snapshot(p).width==1920&&snapshot(p).height==1080&&snapshot(p).reconfigurations==1);
    assert(!submit_frame(p,&a,4,true));pump_outputs(p,4);
    assert(snapshot(p).width==1280&&snapshot(p).height==720&&snapshot(p).reconfigurations==2);
    assert(creates==3&&deletes==2&&native_decodes==4&&output_widths[0]==1280&&output_widths[1]==1280&&output_widths[2]==1920&&output_widths[3]==1280);
    assert(!atomic_load(&external_stop));finish_case(p);
    puts("PASS real integration B3: 720->1080->720 retains conversion leases, recreates only after readers finish, converts and presents every epoch, releases all resources");
}
static void attempts_and_budget(void)
{
    Fixture a=dimensions(1280,720);a.level=31;
    X4StreamPipeline *p=begin_case(104);fail_query_count=2;
    assert(!submit_frame(p,&a,1,true));pump_outputs(p,1);
    assert(queries==3&&creates==1&&p->native_info.attempt_count==3);
    assert(query_configs[0].profile==77&&query_configs[0].maxLevel==31&&query_configs[0].maxDpbFrameCount==4);
    assert(query_configs[1].profile==77&&query_configs[1].maxLevel==32&&query_configs[1].maxDpbFrameCount==4);
    assert(query_configs[2].profile==66&&query_configs[2].maxLevel==31&&query_configs[2].maxDpbFrameCount==4);
    finish_case(p);
    p=begin_case(105);over_budget_count=1;assert(!submit_frame(p,&a,1,true));pump_outputs(p,1);
    assert(queries==2&&creates==1&&p->native_info.attempt_count==2&&p->native_info.attempts[0].failure_reason==X4_STREAM_POLICY_QUERY_BUDGET&&p->native_info.attempts[0].query==0&&p->native_info.attempts[0].create==INT32_MIN);
    finish_case(p);
    Fixture b=dimensions(1920,1088);b.level=51;
    p=begin_case(106);fail_query_count=1;assert(!submit_frame(p,&b,1,true));pump_outputs(p,1);
    assert(queries==2&&creates==1&&query_configs[0].profile==77&&query_configs[0].maxLevel==51);
    assert(query_configs[1].profile==77&&query_configs[1].maxLevel==42&&query_configs[1].maxDpbFrameCount==4);
    finish_case(p);puts("PASS real integration B4/B5: DPB floor four, exact Query/Create matching, three 720 retries, budget retry and 1080 geometry-level retry without legacy level31");
}
static void oversized_sps(void)
{
    X4StreamPipeline *p=begin_case(107);Fixture f=dimensions(2560,1440);f.level=51;
    if(p->startup.enabled){freeze_clock();x4_stream_startup_note_alias(&p->startup,0,sceKernelGetProcessTime());}
    assert(!submit_frame(p,&f,1,true));uint64_t deadline=sceKernelGetProcessTime()+10000000;
    if(p->startup.enabled){
        pump_dequeued(p,1);assert(!queries&&!creates&&!native_decodes&&!snapshot(p).error);
        advance_clock_and_wait_for_idle_poll(p,X4_STREAM_INITIAL_RESOLUTION_WAIT_US);
    }
    while(!snapshot(p).error){(void)x4_stream_pipeline_present(p,NULL,NULL);assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    X4StreamPipelineSnapshot s=snapshot(p);assert(s.error==(p->startup.enabled?X4_STREAM_INITIAL_RESOLUTION_TIMEOUT:X4_SPS_SIZE)&&s.rejected_width==2560&&s.rejected_height==1440);
    assert(!queries&&!creates&&!native_decodes&&!atomic_load(&external_stop));assert(!x4_stream_pipeline_halt_video(p));
    assert(snapshot(p).display_detached);finish_case(p);
    puts("PASS real integration B6: default720 rejects immediately; aliased initial over-limit SPS waits ten seconds before failing, with no Query/Create and only video halted");
}
static void cancel_reconfiguration(void)
{
    X4StreamPipeline *p=begin_case(108);Fixture a=dimensions(1280,720),b=dimensions(1920,1088);
    assert(!submit_frame(p,&a,1,true));pump_outputs(p,1);cancel_on_delete=true;
    assert(!submit_frame(p,&b,2,true));uint64_t deadline=sceKernelGetProcessTime()+10000000;
    while(!atomic_load(&external_stop)){(void)x4_stream_pipeline_present(p,NULL,NULL);assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    assert(!snapshot(p).error);finish_case(p);assert(creates==1&&deletes==1);
    puts("PASS real integration B3 cancellation: stopping after old decoder Delete prevents new Create and releases every lease");
}
static void persistent_native_errors(void)
{
    X4StreamPipeline *p=begin_case(110);Fixture f=dimensions(1280,720);
    for(unsigned i=1;i<=10;++i){commands[i]=NATIVE_FAILURE;assert(!submit_frame(p,&f,i,true));
        if(i<10){pump_dequeued(p,i);assert(snapshot(p).consecutive_frame_errors==i);assert(consume_keyframe_after_publication(p));}}
    uint64_t deadline=sceKernelGetProcessTime()+10000000;
    while(!snapshot(p).error){(void)x4_stream_pipeline_present(p,NULL,NULL);assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    assert(snapshot(p).error==X4_STREAM_FRAME_FAILURE_LIMIT&&snapshot(p).frame_errors==10);
    assert(!atomic_load(&external_stop));assert(!x4_stream_pipeline_halt_video(p));
    assert(creates==10&&deletes==10&&native_decodes==10);finish_case(p);
    puts("PASS real integration B2: error streak survives nine native Delete/Create epochs and stops at ten, bounded epoch omissions stay explicit");
}
static void no_image_does_not_reset_errors(void)
{
    X4StreamPipeline *p=begin_case(111);Fixture f=dimensions(1280,720);
    freeze_clock();
    commands[1]=ERROR_FRAME;assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    assert(snapshot(p).consecutive_frame_errors==1);assert(consume_keyframe_after_publication(p));
    commands[2]=NO_OUTPUT;assert(!submit_frame(p,&f,2,true));pump_dequeued(p,2);
    assert(snapshot(p).consecutive_frame_errors==1&&!snapshot(p).outputs);
    commands[3]=ERROR_FRAME;assert(!submit_frame(p,&f,3,true));pump_dequeued(p,3);
    assert(snapshot(p).consecutive_frame_errors==2);
    /* The repeated damaged IDR stays in the original wait. The next request
     * is due after 300ms, not immediately after every malformed output. */
    uint64_t absolute=p->idr_recovery.deadline_us;
    assert(!x4_stream_pipeline_take_keyframe_request(p));
    advance_clock_and_wait_for_idle_poll(p,300000);
    assert(p->idr_recovery.deadline_us==absolute&&consume_keyframe_after_publication(p));
    assert(!submit_frame(p,&f,4,true));pump_outputs(p,1);assert(!snapshot(p).consecutive_frame_errors);
    assert(creates==1&&native_decodes==4&&!snapshot(p).error);finish_case(p);
    puts("PASS real integration B2: successful no-image Decode does not reset errors; only a validated correct output resets the streak");
}
static void retained_reconfiguration(void)
{
    X4StreamPipeline *p=begin_case(109);Fixture a=dimensions(1280,720),b=dimensions(1920,1088);
    assert(!submit_frame(p,&a,1,true));pump_outputs(p,1);fail_delete=true;
    assert(!submit_frame(p,&b,2,true));uint64_t deadline=sceKernelGetProcessTime()+10000000;
    while(!snapshot(p).error){(void)x4_stream_pipeline_present(p,NULL,NULL);assert(sceKernelGetProcessTime()<deadline);sceKernelUsleep(100);}
    assert(snapshot(p).error==-10103&&!atomic_load(&external_stop));
    assert(x4_stream_pipeline_halt_video(p)<0);X4StreamPipelineSnapshot s=snapshot(p);
    assert(s.retained&&s.display_detached&&creates==1&&deletes==1&&open_decoders==1);
    assert(threads_joined==3&&memory_owned());unsigned before=deletes;
    assert(x4_stream_pipeline_close(p)==-10103&&deletes==before); /* No unsafe implicit retry. */
    assert(x4_stream_pipeline_close(p)==-10103&&deletes==before);
    assert(!x4_stream_pipeline_export(p,NULL)); /* Genuine retained-error report, never a success. */
    /* Fixture-only disposal after all three host threads joined. Production
     * deliberately retains this state; clearing a mock fault is no native
     * quiescence evidence and never enables a production retry. */
    assert(!p->decoder_thread.created&&!p->converters[0].thread.created&&!p->converters[1].thread.created);
    VideoState *native=p->video.state;assert(native&&native->decoder);free(native->decoder);native->decoder=NULL;open_decoders=0;
    for(unsigned i=1;i<=next_memory;++i)if(host_memory[i].owned){free(host_memory[i].pointer);host_memory[i].pointer=NULL;host_memory[i].owned=host_memory[i].mapped=false;}
    free(native);p->video.state=NULL;p->retained=false;p->closed=true;p->display_memory=NULL;p->display_allocated=false;
    assert(!video_handle);x4_stream_pipeline_free(p);current=NULL;
    puts("PASS real integration retained B3: failed Delete prevents replacement and reuse, joins CPU readers, keeps native ownership and does not retry cleanup");
}
static void retained_display_close(void)
{
    X4StreamPipeline *p=begin_case(112);Fixture f=dimensions(1280,720);
    assert(!submit_frame(p,&f,1,true));pump_outputs(p,1);fail_video_close=true;
    assert(x4_stream_pipeline_halt_video(p)==-10105);X4StreamPipelineSnapshot s=snapshot(p);
    assert(s.error==-10105&&s.retained&&!s.display_detached&&!atomic_load(&external_stop));
    assert(threads_joined==3&&video_close_calls==1&&deletes==1&&video_handle==7&&memory_owned()==1);
    assert(x4_stream_pipeline_halt_video(p)==-10105&&x4_stream_pipeline_close(p)==-10105);
    assert(x4_stream_pipeline_close(p)==-10105&&video_close_calls==1&&deletes==1);
    assert(!x4_stream_pipeline_export(p,NULL));
    /* Fixture-only disposal: the fake VideoOut has no asynchronous owner.
     * Production must retain this allocation and must not reopen UI VideoOut. */
    for(unsigned i=1;i<=next_memory;++i)if(host_memory[i].owned){free(host_memory[i].pointer);host_memory[i].pointer=NULL;host_memory[i].owned=host_memory[i].mapped=false;}
    video_handle=0;pending_slot=screen_slot=-1;p->display_memory=NULL;p->display_allocated=false;p->retained=false;p->closed=true;
    x4_stream_pipeline_free(p);current=NULL;
    puts("PASS real integration E5: failed VideoOutClose retains display memory, display_detached stays false and repeated halt/close never retries native teardown");
}
static void waiting_idr_eof(void)
{
    X4StreamPipeline *p=begin_case(113);Fixture f=dimensions(1280,720);
    assert(!submit_frame(p,&f,1,true));pump_outputs(p,1);
    commands[2]=NATIVE_FAILURE;assert(!submit_frame(p,&f,2,true));pump_dequeued(p,2);
    assert(snapshot(p).waiting_idr&&creates==1&&deletes==1);
    assert(!submit_frame(p,&f,3,false));pump_dequeued(p,3);assert(native_decodes==2);
    assert(p->epoch_count==1&&p->epochs[0].close_us&&p->epochs[0].close_result==0);
    uint64_t first_close=p->epochs[0].close_us;
    assert(!x4_stream_pipeline_close(p));
    assert(p->epochs[0].close_us==first_close&&p->epochs[0].close_result==0&&deletes==1);
    finish_case(p);
    puts("PASS real integration waiting-IDR EOF: no replacement decoder, exactly one epoch close timestamp, idempotent safe close and genuine export");
}
static void depth_two_continuous(void)
{
    X4StreamPipeline *p=begin_case_depth(201,X4_DEPTH_2);Fixture f=dimensions(1920,1088);
    assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    assert(!snapshot(p).error&&snapshot(p).outputs==0&&without_output_calls==1);
    assert(p->native_info.active_depth==2&&p->ring.output_count==5);
    for(unsigned tag=2;tag<=80;++tag){
        assert(!submit_frame(p,&f,tag,false));pump_outputs(p,tag-1);
        assert(returned_count==tag-1&&returned_tags[tag-2]==tag-1);
        assert(!snapshot(p).frame_errors&&!snapshot(p).waiting_idr);
    }
    assert(native_decodes==80&&without_output_calls==1&&returned_count==79);
    assert(p->video.no_picture_calls==1&&p->native_info.input_count==4&&p->native_info.output_count==5);
    assert(p->native_info.requested_depth==2&&!p->native_info.depth_fallback_used);
    assert(p->native_info.native_budget_bytes==192u*1024u*1024u);
    assert(queries==1&&creates==1&&query_configs[0].decodePipelineDepth==2&&create_configs[0].decodePipelineDepth==2);
    finish_case(p);assert(!flush_calls);
    puts("PASS depth2 continuous real adapter/pipeline: previous pFrameBuffer, false isAccepted, first no-output, four AU canaries and five output leases, Delete without Flush");
}
static void depth_two_waiting_idr_and_bad_frame(void)
{
    X4StreamPipeline *p=begin_case_depth(202,X4_DEPTH_2);Fixture f=dimensions(1920,1088);
    assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    assert(!submit_frame(p,&f,2,false));pump_outputs(p,1);
    commands[3]=ERROR_FRAME;
    assert(!submit_frame(p,&f,3,false));pump_outputs(p,2);
    assert(!submit_frame(p,&f,4,false));pump_dequeued(p,4);
    assert(native_decodes==4&&snapshot(p).waiting_idr&&snapshot(p).frame_errors==1);
    unsigned pending_mask=x4_live_video_stream_output_held_mask(&p->video);assert(pending_mask);
    assert(!submit_frame(p,&f,5,false));pump_dequeued(p,5);
    assert(native_decodes==4&&snapshot(p).frame_errors==1&&
        x4_live_video_stream_output_held_mask(&p->video)==pending_mask);
    assert(!submit_frame(p,&f,6,true));pump_outputs(p,3);
    /* Returning pre-wait picture4 from Decode(IDR6) is legal and does not
     * associate that picture with IDR6; the exported ABI has no output PTS. */
    assert(returned_count==4&&returned_tags[3]==4&&native_decodes==5);
    assert(!snapshot(p).waiting_idr&&!snapshot(p).error&&snapshot(p).frame_errors==1);
    assert(p->waiting_idr_drops==1&&creates==1&&!deletes);
    finish_case(p);assert(!flush_calls);
    puts("PASS depth2 BAD_FRAME/IDR wait: only damaged returned slot retires; dependent AU skips Decode; pending pre-IDR picture returns safely on the next IDR without false error");
}
static void depth_two_native_restart(void)
{
    X4StreamPipeline *p=begin_case_depth(203,X4_DEPTH_2);Fixture f=dimensions(1920,1088);
    assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    assert(!submit_frame(p,&f,2,false));pump_outputs(p,1);
    commands[3]=NATIVE_FAILURE;assert(!submit_frame(p,&f,3,false));pump_dequeued(p,3);
    assert(creates==1&&deletes==1&&!open_decoders&&snapshot(p).waiting_idr);
    assert(!submit_frame(p,&f,4,false));pump_dequeued(p,4);assert(native_decodes==3);
    assert(!submit_frame(p,&f,5,true));pump_dequeued(p,5);
    assert(creates==2&&deletes==1&&without_output_calls==2&&p->native_info.active_depth==2);
    assert(!submit_frame(p,&f,6,false));pump_outputs(p,2);
    assert(returned_tags[returned_count-1]==5&&snapshot(p).frame_errors==1&&!snapshot(p).error);
    assert(queries==2&&query_configs[0].decodePipelineDepth==2&&query_configs[1].decodePipelineDepth==2);
    finish_case(p);assert(!flush_calls);
    puts("PASS depth2 unknown native error: quiesce/Delete retires both held outputs; replacement ledger clean; initial no-output repeats once and IDR input resumes");
}
static void depth_auto_resolution_changes(void)
{
    X4StreamPipeline *p=begin_case_depth(204,X4_DEPTH_AUTO);Fixture a=dimensions(1280,720),b=dimensions(1920,1088);
    assert(!submit_frame(p,&a,1,true));pump_outputs(p,1);
    assert(p->native_info.active_depth==1&&p->ring.output_count==4);
    assert(!submit_frame(p,&a,2,false));await_consumed_without_present(p,2);
    atomic_store(&conversion_hold,true);assert(!x4_stream_pipeline_present(p,NULL,NULL));
    uint64_t deadline=host_wall_us()+10000000;
    while(!atomic_load(&conversion_entered[0])||!atomic_load(&conversion_entered[1])){
        assert(host_wall_us()<deadline);sceKernelUsleep(100);}
    assert(!submit_frame(p,&b,3,true));
    while(!snapshot(p).reconfiguring){assert(host_wall_us()<deadline);sceKernelUsleep(100);}
    assert(creates==1&&!deletes&&p->converting); /* Decoder cannot Delete under active readers. */
    atomic_store(&conversion_hold,false);pump_dequeued(p,3);
    assert(creates==2&&deletes==1&&p->native_info.active_depth==2&&p->ring.output_count==5);
    assert(!submit_frame(p,&b,4,false));pump_outputs(p,3);
    assert(snapshot(p).width==1920&&snapshot(p).height==1080);
    assert(!submit_frame(p,&a,5,true));pump_outputs(p,4);
    assert(p->native_info.active_depth==1&&p->ring.output_count==4&&snapshot(p).reconfigurations==2);
    assert(queries==3&&creates==3&&deletes==2&&native_decodes==5);
    assert(query_configs[0].decodePipelineDepth==1&&query_configs[1].decodePipelineDepth==2&&query_configs[2].decodePipelineDepth==1);
    assert(output_widths[0]==1280&&output_widths[1]==1280&&output_widths[2]==1920&&
        output_widths[3]==1920&&output_widths[4]==1280);
    finish_case(p);assert(!flush_calls);
    puts("PASS Auto real SPS 720->1080->720: depth1/4outputs -> depth2/5outputs -> depth1/4outputs; quiesce waits conversion readers, Delete resets ledgers and geometry");
}
static void depth_two_cancel_in_native(void)
{
    X4StreamPipeline *p=begin_case_depth(205,X4_DEPTH_2);Fixture f=dimensions(1920,1088);
    assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    atomic_store(&native_hold,true);assert(!submit_frame(p,&f,2,false));
    uint64_t deadline=host_wall_us()+10000000;
    while(!atomic_load_explicit(&native_entered,memory_order_acquire)){
        assert(host_wall_us()<deadline);sceKernelUsleep(100);}
    VideoState *native=p->video.state;HostDecoder *d=native->decoder;
    assert(d->held_count==2&&x4_decoder_depth_outputs_held(&native->stream_depth)==2);
    x4_stream_pipeline_request_stop(p);
    assert(!deletes&&open_decoders==1&&memory_owned()); /* A native call is not interruptible. */
    atomic_store_explicit(&native_hold,false,memory_order_release);
    finish_case(p);assert(!flush_calls);
    puts("PASS depth2 cancellation while Decode holds two output offers: no premature Delete or release, native return joins safely, Delete retires pending input/output without Flush");
}
static void depth_two_failed_delete(void)
{
    X4StreamPipeline *p=begin_case_depth(206,X4_DEPTH_2);Fixture f=dimensions(1920,1088);
    assert(!submit_frame(p,&f,1,true));pump_dequeued(p,1);
    commands[2]=NATIVE_FAILURE;fail_delete=true;
    assert(!submit_frame(p,&f,2,false));uint64_t deadline=host_wall_us()+10000000;
    while(!snapshot(p).error){(void)x4_stream_pipeline_present(p,NULL,NULL);assert(host_wall_us()<deadline);sceKernelUsleep(100);}
    assert(snapshot(p).error==-10103&&creates==1&&deletes==1&&open_decoders==1);
    assert(x4_stream_pipeline_halt_video(p)==-10103&&snapshot(p).retained&&snapshot(p).display_detached);
    VideoState *native=p->video.state;HostDecoder *d=native->decoder;
    assert(d->held_count==2&&x4_decoder_depth_outputs_held(&native->stream_depth)==2);
    assert(x4_decoder_depth_inputs_live(&native->stream_depth)==2&&memory_owned());
    unsigned before=deletes;
    assert(x4_stream_pipeline_close(p)==-10103&&x4_stream_pipeline_close(p)==-10103&&deletes==before);
    assert(!x4_stream_pipeline_export(p,NULL));
    assert(!p->decoder_thread.created&&!p->converters[0].thread.created&&!p->converters[1].thread.created);
    /* Fixture-only disposal of an intentionally retained native failure;
     * this does not prove a failed real Delete safe or enable a retry. */
    free(native->decoder);native->decoder=NULL;open_decoders=0;
    for(unsigned i=1;i<=next_memory;++i)if(host_memory[i].owned){
        free(host_memory[i].pointer);host_memory[i].pointer=NULL;host_memory[i].owned=host_memory[i].mapped=false;}
    free(native);p->video.state=NULL;p->retained=false;p->closed=true;p->display_memory=NULL;p->display_allocated=false;
    assert(!video_handle);assert(!x4_stream_pipeline_free(p));current=NULL;assert(!flush_calls);
    puts("PASS depth2 Delete failure: both pending output reservations and two input leases retained; no re-create/reuse/retry/free; real retained-error export accepted separately");
}
static void depth_two_fallbacks(void)
{
    Fixture f=dimensions(1920,1088);
    for(unsigned failure=0;failure<2;++failure){
        X4StreamPipeline *p=begin_case_depth(207+failure,X4_DEPTH_2);
        if(failure)fail_create_count=1;else fail_query_count=1;
        assert(!submit_frame(p,&f,1,true));pump_outputs(p,1);
        assert(queries==2&&creates==(failure?2u:1u)&&query_configs[0].decodePipelineDepth==2&&query_configs[1].decodePipelineDepth==1);
        assert(p->native_info.requested_depth==2&&p->native_info.active_depth==1&&p->native_info.depth_fallback_used);
        assert(p->native_info.input_count==2&&p->native_info.output_count==4&&p->ring.output_count==4);
        assert(p->native_info.native_budget_bytes==128u*1024u*1024u&&!snapshot(p).error);
        finish_case(p);assert(!flush_calls);
    }
    puts("PASS depth2 Query/Create native rejection: same SPS policy retry at depth1, same Query/Create config, no video error, 128MiB budget and four output slots restored");
}
#ifndef X4_STREAM_RESILIENCE_NO_MAIN
int main(int argc,char **argv)
{
    assert(argc==2);report_directory=argv[1];
    intermittent_errors();persistent_errors();delayed_idr_recovers();bad_idrs_never_renew_deadline();resolution_changes();attempts_and_budget();oversized_sps();cancel_reconfiguration();persistent_native_errors();no_image_does_not_reset_errors();retained_reconfiguration();retained_display_close();waiting_idr_eof();
    depth_two_continuous();depth_two_waiting_idr_and_bad_frame();depth_two_native_restart();depth_auto_resolution_changes();
    depth_two_cancel_in_native();depth_two_failed_delete();depth_two_fallbacks();
    puts("PASS real resilience integration, no PS4 execution or timing claim");return 0;
}
#endif
