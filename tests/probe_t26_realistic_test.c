/* SPDX-License-Identifier: GPL-3.0-only */
/* Appended to the historical streaming/GPU-double fixture by the script.
 * Synthetic8-byte AUs, immediate mock outputs and a deterministic HASH DOUBLE
 * test driver scheduling/ownership/aggregation only. These bytes are not an
 * H264 measurement fixture and no result here represents console performance.
 * Real SHA256/NV12 hashing and delayed native ownership have separate tests. */
#include "../src/video/probe_t26_realistic.h"
typedef struct {
    X4StreamPipeline *pipeline;
    pthread_t main_thread;
    atomic_bool ready,close_requested;
    bool main_created;
    int open_result,close_result;
    unsigned markers,hashes,cpu_samples;
    pthread_t cpu_owner;
} RealFixture;
static RealFixture *real_fixture;
static X4LiveVideo *real_video;
struct X4T26Platform {unsigned unused;};
int x4_t26_platform_create(X4T26Platform **out,X4T26BeforeNative before,void *ctx)
{assert(out&&!*out&&before&&ctx);*out=calloc(1,sizeof(**out));return *out?0:-1;}
void x4_t26_platform_free(X4T26Platform **p){if(p&&*p){free(*p);*p=NULL;}}
void x4_t26_platform_info(const X4T26Platform *p,X4T26PlatformInfo *out)
{(void)p;assert(out);memset(out,0,sizeof(*out));}
void x4_t26_platform_cpu(X4T26Platform *p,X4T26CpuSnapshot *out)
{
    assert(p&&out&&real_fixture&&pthread_equal(pthread_self(),decode_owner));
    if(real_fixture->cpu_samples)assert(pthread_equal(pthread_self(),real_fixture->cpu_owner));
    else real_fixture->cpu_owner=pthread_self();
    ++real_fixture->cpu_samples;
    struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
    *out=(X4T26CpuSnapshot){.thread_available=true,.thread_us=(uint64_t)t.tv_sec*1000000+t.tv_nsec/1000};
}
int x4_t26_platform_query(X4T26Platform *p,int64_t offset,X4T26DirectQuery *out,bool *called)
{
    assert(p&&offset==77&&out&&called);*called=true;
    if(!real_video||!real_video->state)return (int32_t)UINT32_C(0x8002000d);
    *out=(X4T26DirectQuery){.start=77,.end=77+1280*720*3/2,.memory_type=0};return 0;
}
int x4_t26_platform_largest(X4T26Platform *p,uint64_t *bytes,bool *called)
{assert(p&&bytes&&called);*called=true;*bytes=real_video&&real_video->state?67108864:134217728;return 0;}
unsigned x4_live_video_t26_allocations(const X4LiveVideo *v,X4T26Allocation out[X4_T26_ALLOCATION_MAX])
{assert(v&&out);if(!v->state)return 0;out[0]=(X4T26Allocation){.physical_offset=77,.bytes=1280*720*3/2,.memory_type=0};return 1;}
int x4_live_video_t26_raw_start(X4LiveVideo *v,const uint8_t *sps,size_t bytes,
    const atomic_bool *cancel,const X4T26NativeOptions *options)
{
    assert(options&&options->config.realistic&&options->config.depth==2&&options->config.api==2);
    int marker=options->before(options->context,"v2.QueryDecoderMemoryInfo");if(marker)return marker;
    int rc=x4_live_video_stream_raw_start_cpu_depth(v,sps,bytes,cancel,X4_CPU_0,1);
    v->stream_info.requested_depth=v->stream_info.active_depth=2;
    v->stream_info.input_count=4;v->stream_info.output_count=5;
    v->stream_info.sps.chroma_format_idc=1;
    v->stream_info.sps.bit_depth_luma=v->stream_info.sps.bit_depth_chroma=8;
    real_video=v;return rc;
}
int x4_t26_result_hash_nv12(const X4T26Output *out,char hash[65])
{
    assert(out&&out->pointer&&out->valid&&out->layout_verified&&real_fixture);
    assert(pthread_equal(pthread_self(),decode_owner)&&real_video&&real_video->state);
    ++real_fixture->hashes;assert(real_fixture->hashes<=3);
    /* Explicit deterministic double; never emitted as a real fixture SHA. */
    assert(snprintf(hash,65,"%064x",(unsigned)out->pointer[0])==64);return 0;
}
const uint8_t *x4_t26_clip_au(const X4T26ClipData *c,unsigned ordinal,size_t *bytes,uint64_t *pts)
{
    assert(c&&c->verified&&ordinal<X4_T26_SOURCE_AUS&&bytes&&pts);
    *bytes=c->size[ordinal];*pts=c->pts_us[ordinal];return c->data+c->offset[ordinal];
}
static uint64_t fixture_now(void *context){assert(context==real_fixture);return sceKernelGetProcessTime();}
static void fixture_sleep(void *context,unsigned us){assert(context==real_fixture);usleep(us);}
static int fixture_before(void *context,const char *operation)
{assert(context==real_fixture&&operation&&*operation);++real_fixture->markers;return 0;}
static void *real_main(void *context)
{
    RealFixture *f=context;main_owner=pthread_self();
    f->open_result=x4_stream_pipeline_display_open(f->pipeline);
    atomic_store_explicit(&f->ready,true,memory_order_release);
    while(!f->open_result&&!atomic_load_explicit(&f->close_requested,memory_order_acquire)){
        int rc=x4_stream_pipeline_present(f->pipeline,NULL,NULL);assert(!rc);
        assert(!x4_stream_pipeline_wait_present(f->pipeline,1000));
    }
    f->close_result=x4_stream_pipeline_close(f->pipeline);return NULL;
}
static int fixture_display_open(void *context,X4StreamPipeline *p)
{
    RealFixture *f=context;assert(f==real_fixture&&!f->main_created);f->pipeline=p;
    assert(!pthread_create(&f->main_thread,NULL,real_main,f));f->main_created=true;
    uint64_t until=sceKernelGetProcessTime()+1000000;
    while(!atomic_load_explicit(&f->ready,memory_order_acquire)){assert(sceKernelGetProcessTime()<until);usleep(100);}
    return f->open_result;
}
static int fixture_display_close(void *context,X4StreamPipeline *p)
{
    RealFixture *f=context;assert(f==real_fixture&&p==f->pipeline&&f->main_created);
    atomic_store_explicit(&f->close_requested,true,memory_order_release);
    assert(!x4_stream_pipeline_wake(p));assert(!pthread_join(f->main_thread,NULL));
    f->main_created=false;return f->close_result;
}
int main(int argc,char **argv)
{
    assert(argc==2);assert(!setvbuf(stdout,NULL,_IONBF,0));report_directory=argv[1];reset();
    gpu_signal=true;gpu_allowed=1;prepare_fail=scratch_fail=verify_corrupt=false;
    selected_backend=0;selected_quality_word=27;gpu_starts=gpu_destroys=0;
    RealFixture f={0};atomic_init(&f.ready,false);atomic_init(&f.close_requested,false);real_fixture=&f;
    X4T26ClipData clip={0};clip.verified=true;clip.bytes=clip.allocated_bytes=X4_T26_SOURCE_AUS*8u;
    clip.data=calloc(1,clip.bytes);assert(clip.data);
    for(uint64_t i=0;i<X4_T26_SOURCE_AUS;++i){clip.offset[i]=i*8;clip.size[i]=8;clip.pts_us[i]=i*1000000/60;memcpy(clip.data+i*8,&i,8);}
    X4T26GeneratedSample sample={.id="synthetic-driver-fixture-not-video",.sps={0,0,0,1,0x67},.sps_size=5,
        .pps={0,0,0,1,0x68,0x80},.pps_size=6};
    X4T26Config config;x4_t26_config_base(&config);config.realistic=true;
    char baseline[3][65];assert(snprintf(baseline[0],65,"%064x",96)==64);
    assert(snprintf(baseline[1],65,"%064x",95)==64);memcpy(baseline[2],baseline[1],65);
    atomic_bool cancel;atomic_init(&cancel,false);
    X4T26RunOps ops={.context=&f,.now_us=fixture_now,.sleep_us=fixture_sleep,.before=fixture_before,
        .cancel=&cancel,.display_open=fixture_display_open,.display_close=fixture_display_close};
    X4T26ModeResult result;X4StreamPipeline *retained=NULL;
    int run_rc=x4_t26_run_realistic(&config,&sample,&clip,baseline,&ops,&result,&retained);
    fprintf(stderr,"T26_REALISTIC_RESULT rc=%d status=%u native_rc=%d local_rc=%d retained=%u calls=%llu outputs=%llu no_output=%llu errors=%llu inference=%u invalid=%u decode_n=%llu au_n=%llu interval_n=%llu gpu_requested=%u gpu_frames=%llu cpu_frames=%llu gpu_reason=%s query=%d create=%d close=%d owned_clean=%u hash_observer_us=%llu\n",
        run_rc,result.status,result.native_result,result.local_result,retained!=NULL,
        (unsigned long long)result.measured_calls,(unsigned long long)result.measured_outputs,
        (unsigned long long)result.no_output_calls,(unsigned long long)result.error_outputs,
        result.inference_valid,result.invalid_reasons,(unsigned long long)result.decode.n,
        (unsigned long long)result.au_latency.n,(unsigned long long)result.output_interval.n,
        result.gpu_requested,(unsigned long long)result.gpu_frames,(unsigned long long)result.cpu_frames,
        result.gpu_reason,result.backend.v2.query_result,result.backend.v2.create_result,
        result.backend.close_result,result.backend.owned_clean,(unsigned long long)result.hash_observer_us);
    for(unsigned h=0;h<3;++h)fprintf(stderr,
        "T26_REALISTIC_HASH h=%u ordinal=%llu observed=%u compared=%u matches=%u expected=%s actual=%s\n",
        h,(unsigned long long)result.hashes[h].measured_ordinal,result.hashes[h].observed,
        result.hashes[h].compared,result.hashes[h].matches_base,baseline[h],result.hashes[h].sha256);
    assert(!run_rc);
    assert(!retained&&result.accepted&&result.completed&&result.measured_calls==600&&result.measured_outputs==600);
    assert(result.gpu_requested&&result.gpu_frames>0&&!result.cpu_frames&&result.gpu_reason[0]);
    assert(result.decode.n==600&&result.au_latency.n==600&&result.output_interval.n==599&&result.inference_valid);
    assert(result.decode_to_flip.n&&result.submit_to_observe.n==result.decode_to_flip.n);
    assert(result.backend.owned_clean&&result.backend.closed&&!result.backend.retained&&result.backend.allocations_before==1);
    assert(result.backend.allocations_after==0&&result.backend.allocation_checks[0].release_evidence);
    assert(result.cpu.thread_available&&f.cpu_samples==2&&!pthread_equal(f.cpu_owner,pthread_self()));
    assert(f.hashes==3&&!allocated&&!handle_open&&native_closes==1&&gpu_starts>0);
    for(unsigned i=0;i<3;++i)assert(result.hashes[i].observed&&result.hashes[i].compared&&result.hashes[i].matches_base);
    assert(!result.flush_calls&&!result.drained_outputs&&result.call_delay_hist[0]==600);
    atomic_store_explicit(&cancel,true,memory_order_release);
    assert(x4_t26_run_realistic(&config,&sample,&clip,baseline,&ops,&result,&retained)==3);
    assert(result.status==X4_T26_RESULT_CANCELLED&&!result.accepted&&!f.main_created&&!retained);
    free(clip.data);real_fixture=NULL;real_video=NULL;
    puts("PASS T26 realistic driver: independent producer/decode/main GPU owners,600 ordered images,3 prepublication hash-double calls,CPU measured on Decode owner,clean Delete/DirectQuery and cancellation; no console measurements");
    return 0;
}
