/* SPDX-License-Identifier: GPL-3.0-only */
/* Appended by test_probe_t26_pipeline.sh to the existing GPU/scheduler-double
 * translation unit. The real pipeline, queues, waits, leases and converter
 * policy run; native Decode/VideoOut/GPU are explicit host substitutes.
 * This tests T26 wiring, not native ABI ownership or measured performance. */
enum { T26_PARTIAL_NONE,T26_PARTIAL_QUERY_REJECT,T26_PARTIAL_CREATE_CANCEL };
typedef struct {
    X4StreamPipeline *pipeline;
    X4T26Config expected;
    atomic_uint decode_observed;
    unsigned native_starts,markers,flips,before_close,after_close;
    bool reject_start;
    unsigned partial_start;
    uint64_t last_output,last_observed;
} T26PipelineFixture;
static T26PipelineFixture *t26_active_fixture;
static void t26_present_timeout(X4StreamPipeline *p,uint64_t count)
{
    X4GpuConvertStatus gpu={0};
    if(p->gpu)x4_gpu_convert_status(p->gpu,&gpu);
    fprintf(stderr,"T26_PRESENT_TIMEOUT depth=%u target=%llu presentations=%llu converting=%u gpu_active=%u gpu_pending=%u gpu_disabled=%u gpu_timeouts=%llu gpu_late=%llu gpu_quarantines=%llu gpu_completed=%llu gpu_reason=%s readers=%u error=%d consumed=%llu\n",
        p->t26_hooks.native.config.depth,(unsigned long long)count,
        (unsigned long long)p->view.presentations,(unsigned)p->converting,(unsigned)p->gpu_active,
        (unsigned)gpu.pending,(unsigned)gpu.disabled,(unsigned long long)gpu.timeouts,
        (unsigned long long)gpu.late_completed,(unsigned long long)gpu.quarantines,
        (unsigned long long)gpu.completed,gpu.reason?gpu.reason:"none",
        atomic_load_explicit(&p->conversion_readers,memory_order_acquire),
        atomic_load_explicit(&p->error,memory_order_acquire),
        (unsigned long long)atomic_load_explicit(&p->consumed,memory_order_acquire));
    fflush(stderr);
    lock(&p->ring_gate);
    fprintf(stderr,"T26_PRESENT_RING output_count=%u occupied=%u states=",
        p->ring.output_count,p->ring.occupied);
    for(unsigned i=0;i<p->ring.output_count;++i)
        fprintf(stderr,"%s%u",i?",":"",(unsigned)p->ring.slots[i].state);
    unlock(&p->ring_gate);
    fputc('\n',stderr);fflush(stderr);
}
static void t26_present_until(X4StreamPipeline *p,uint64_t count)
{
    uint64_t deadline=sceKernelGetProcessTime()+2000000;
    while(p->view.presentations<count){
        uint64_t now=sceKernelGetProcessTime();
        if(now>=deadline)t26_present_timeout(p,count);
        assert(now<deadline);
        assert(!x4_stream_pipeline_present(p,gpu_overlay,p));usleep(100);
    }
}
static int t26_marker(void *context,const char *operation)
{
    T26PipelineFixture *f=context;
    assert(f==t26_active_fixture);
    ++f->markers;
    if(!strcmp(operation,"QueryResourceInfo"))return f->reject_start?-EACCES:0;
    assert(f->partial_start==T26_PARTIAL_CREATE_CANCEL&&!strcmp(operation,"CreateDecoder"));
    x4_stream_pipeline_request_stop(f->pipeline);
    return X4_STREAM_VIDEO_CANCELLED;
}
int x4_live_video_t26_raw_start(X4LiveVideo *video,const uint8_t *sps,
    size_t bytes,const atomic_bool *cancel,const X4T26NativeOptions *options)
{
    T26PipelineFixture *f=t26_active_fixture;assert(f&&options);
    assert(x4_t26_config_equal(&options->config,&f->expected));
    assert(options->before==t26_marker&&options->context==f);
    ++f->native_starts;
    int marker=0;
    if(!f->partial_start){
        marker=options->before(options->context,"QueryResourceInfo");
        if(marker)return marker;
    }
    /* The temporary native double emits immediately and owns five buffers.
     * At most one native lease is in use in this hook-wiring test. Separate
     * native T26 tests cover delayed depth-two ownership/retirement. */
    int rc=x4_live_video_stream_raw_start_cpu_depth(video,sps,bytes,cancel,X4_CPU_0,1);
    video->stream_info.requested_depth=video->stream_info.active_depth=options->config.depth;
    video->stream_info.input_count=options->config.depth==2?4:2;
    video->stream_info.output_count=options->config.depth==2?5:4;
    video->stream_info.sps.chroma_format_idc=1;
    video->stream_info.sps.bit_depth_luma=video->stream_info.sps.bit_depth_chroma=8;
    if(!rc&&f->partial_start){
        /* These explicit partial starts own native resources before failing.
         * They exercise the real cleanup path rather than a retained flag. */
        marker=options->before(options->context,"QueryResourceInfo");
        assert(!marker&&video->state);
        if(f->partial_start==T26_PARTIAL_QUERY_REJECT){
            video->stream_info.query_result=-EACCES;return -EACCES;
        }
        return options->before(options->context,"CreateDecoder");
    }
    return rc;
}
static void t26_observe_decode(void *context,uint64_t identity,int rc,
    const X4ProbeNativeOutput *out,const X4StreamVideoInfo *info)
{
    T26PipelineFixture *f=context;X4StreamPipeline *p=f->pipeline;
    assert(p&&pthread_equal(pthread_self(),decode_owner)&&!rc&&out->valid);
    assert(info->active_depth==f->expected.depth);
    unsigned before=atomic_load_explicit(&f->decode_observed,memory_order_relaxed);
    assert(identity==before&&out->output_id==before+1&&p->view.outputs==before);
    /* It runs before accept_native: output is still a DECODE lease, and its
     * first pixel can be inspected while native storage remains owned. */
    assert(p->ring.slots[out->slot].state==X4_PIPELINE_DECODING);
    assert(out->pixels&&out->pixels[0]==16+identity);
    assert(!f->before_close&&!f->after_close);
    atomic_fetch_add_explicit(&f->decode_observed,1,memory_order_release);
}
static void t26_observe_flip(void *context,uint64_t identity,uint64_t decoded,
    uint64_t submitted,uint64_t observed)
{
    T26PipelineFixture *f=context;
    assert(pthread_equal(pthread_self(),main_owner));
    assert(identity>f->last_output&&decoded<=submitted&&submitted<=observed);
    assert(f->pipeline->view.presentations==f->flips);
    assert(!f->before_close&&!f->after_close);
    f->last_output=identity;f->last_observed=observed;++f->flips;
}
static void t26_before_close(void *context,const X4LiveVideo *video,
    const X4StreamVideoInfo *info)
{
    T26PipelineFixture *f=context;assert(video&&info);
    assert(!atomic_load_explicit(&f->pipeline->conversion_readers,memory_order_acquire));
    assert(f->before_close==f->after_close);
    if(!f->reject_start){assert(video->state&&info->active_depth==f->expected.depth);
        const Native *native=video->state;assert(native->pixels[0]);(void)native->pixels[0][0];
        if(f->partial_start)for(unsigned i=0;i<T26_MOCK_OUTPUTS;++i)assert(native->pixels[i]);}
    else assert(!video->state);
    ++f->before_close;
}
static void t26_after_close(void *context,const X4LiveVideo *video,
    const X4StreamVideoInfo *info,int result)
{
    T26PipelineFixture *f=context;assert(video&&info&&!result);
    assert(f->before_close==f->after_close+1&&!video->state);
    /* No pixel pointer is retained or read here after the native Delete. */
    ++f->after_close;
}
static void t26_run_hooks(unsigned depth,bool rejected)
{
    reset();gpu_signal=true;gpu_allowed=1;prepare_fail=scratch_fail=verify_corrupt=false;
    selected_backend=0;selected_quality_word=27;
    gpu_starts=gpu_destroys=0;
    assert(x4_stream_settings_set_lab(X4_REQUEST_1080P_HQ,X4_COLOR_FORCE_601,
        X4_CPU_3,X4_DEPTH_AUTO,X4_INTERACTION_MASK,0,0,2));
    X4StreamRequestConfig saved=x4_stream_settings_get();
    T26PipelineFixture fixture={.expected={.api=2,.resource_type=1,.depth=depth,
        .output_memory_type=0,.affinity_mask=0x3f,.priority=700,.realistic=true},
        .reject_start=rejected};
    atomic_init(&fixture.decode_observed,0);t26_active_fixture=&fixture;
    X4T26PipelineHooks hooks={.native={.config=fixture.expected,.before=t26_marker,.context=&fixture},
        .on_decode=t26_observe_decode,.on_flip=t26_observe_flip,
        .before_close=t26_before_close,.after_close=t26_after_close,.context=&fixture};
    atomic_bool stop;atomic_init(&stop,false);
    X4StreamPipeline *p=x4_stream_pipeline_t26_create(2600+depth+rejected*10,&stop,&hooks);
    assert(p);fixture.pipeline=p;
    bool gpu_requested=false;uint64_t observed_gpu=99,observed_cpu=99;char reason[64]="sentinel";
    assert(!x4_stream_pipeline_t26_gpu_status(p,&gpu_requested,&observed_gpu,&observed_cpu,reason));
    assert(!gpu_requested&&observed_gpu==99&&observed_cpu==99&&!strcmp(reason,"sentinel"));
    /* Mutate the caller's value before starting: every config/hook is copied. */
    memset(&hooks,0,sizeof(hooks));
    assert(p->t26_enabled&&p->request.cpu_variant==X4_CPU_0&&p->request.decoder_depth==depth);
    assert(p->request.convert_mode==1&&p->request.sharpness==2);
    /* The legacy replay used zero for VSYNC. With T27, zero is Auto, so
     * preserving that replay policy requires explicit Always. Exact equality
     * also forbids inherited input/audio/rumble, Mailbox and public-only bits. */
    assert(p->request.interaction_modes==X4_INTERACTION_VSYNC_ALWAYS);
    assert(x4_stream_request_vsync_policy(&p->request)==X4_VSYNC_ALWAYS);
    assert(x4_stream_diagnostics_vsync_effective(p->diagnostics));
    assert(!x4_stream_request_mailbox(&p->request));
    assert(p->gpu_metrics.requested==1&&!p->gpu_quality_requested_word);
    X4StreamRequestConfig after=x4_stream_settings_get();
    assert(after.profile==saved.profile&&after.color_preference==saved.color_preference&&
        after.cpu_variant==saved.cpu_variant&&after.decoder_depth==saved.decoder_depth&&
        after.interaction_modes==saved.interaction_modes&&after.convert_mode==saved.convert_mode&&
        after.sharpness==saved.sharpness&&after.resolution_request_mode==saved.resolution_request_mode);
    assert(selected_backend==0&&selected_quality_word==27);
    assert(!x4_stream_pipeline_start(p)&&!x4_stream_pipeline_display_open(p));
    if(!rejected){
        for(unsigned i=0;i<3;++i){one_more(p);t26_present_until(p,i+1);}
        assert(atomic_load_explicit(&fixture.decode_observed,memory_order_acquire)==3);
        assert(fixture.flips==3&&p->gpu_metrics.gpu_frames==3&&gpu_starts==3);
        assert(p->ring.output_count==(depth==2?5u:4u));
    }else{
        const uint8_t sps[]={0,0,0,1,0x67};uint64_t identity=0;
        assert(!x4_stream_pipeline_submit(p,(const uint8_t*)&identity,8,0,identity,sps,sizeof(sps)));
        uint64_t until=sceKernelGetProcessTime()+2000000;
        while(!atomic_load_explicit(&p->decoder_done,memory_order_acquire)){
            assert(sceKernelGetProcessTime()<until);usleep(100);}
        assert(atomic_load(&p->error)==-EACCES&&!fixture.flips&&!atomic_load(&fixture.decode_observed));
    }
    assert(!x4_stream_pipeline_close(p)&&!p->retained&&!p->video.state);
    assert(x4_stream_pipeline_t26_gpu_status(p,&gpu_requested,&observed_gpu,&observed_cpu,reason));
    assert(gpu_requested&&observed_gpu==(rejected?0u:3u)&&!observed_cpu&&reason[0]);
    assert(fixture.native_starts==1&&fixture.markers==1&&fixture.before_close==1&&fixture.after_close==1);
    assert(!allocated&&!handle_open&&native_closes==(rejected?0u:1u));
    assert(!x4_stream_pipeline_free(p));t26_active_fixture=NULL;
    printf("PASS T26 hooks depth=%u rejected=%u: value capture, exact raw start, decode before publication, confirmed flip, pre/post Delete, unchanged settings\n",depth,rejected);
}
static void t26_partial_start_cleanup(unsigned partial)
{
    assert(partial==T26_PARTIAL_QUERY_REJECT||partial==T26_PARTIAL_CREATE_CANCEL);
    reset();gpu_signal=true;gpu_allowed=1;prepare_fail=scratch_fail=verify_corrupt=false;
    selected_backend=0;selected_quality_word=27;gpu_starts=gpu_destroys=0;
    T26PipelineFixture fixture={.expected={.api=2,.resource_type=1,.depth=2,
        .output_memory_type=0,.affinity_mask=0x3f,.priority=700,.realistic=true},
        .partial_start=partial};
    atomic_init(&fixture.decode_observed,0);t26_active_fixture=&fixture;
    X4T26PipelineHooks hooks={.native={.config=fixture.expected,.before=t26_marker,.context=&fixture},
        .on_decode=t26_observe_decode,.on_flip=t26_observe_flip,
        .before_close=t26_before_close,.after_close=t26_after_close,.context=&fixture};
    atomic_bool stop;atomic_init(&stop,false);
    X4StreamPipeline *p=x4_stream_pipeline_t26_create(2620+partial,&stop,&hooks);
    assert(p);fixture.pipeline=p;
    assert(!x4_stream_pipeline_start(p)&&!x4_stream_pipeline_display_open(p));
    const uint8_t sps[]={0,0,0,1,0x67};uint64_t identity=0;
    assert(!x4_stream_pipeline_submit(p,(const uint8_t*)&identity,8,0,identity,sps,sizeof(sps)));
    uint64_t deadline=sceKernelGetProcessTime()+2000000;
    while(!atomic_load_explicit(&p->decoder_done,memory_order_acquire)){
        assert(sceKernelGetProcessTime()<deadline);usleep(100);
    }
    assert(p->video.state&&!native_closes&&!native_outputs&&!fixture.flips);
    assert(!atomic_load_explicit(&fixture.decode_observed,memory_order_acquire));
    assert(!fixture.before_close&&!fixture.after_close);
    if(partial==T26_PARTIAL_QUERY_REJECT){
        assert(atomic_load(&p->error)==-EACCES&&p->native_info.query_result==-EACCES);
    }else assert(!atomic_load(&p->error)&&atomic_load(&p->cancel));
    assert(!x4_stream_pipeline_close(p)&&!p->retained&&!p->video.state);
    assert(!p->decoder_close_result&&!p->ring.occupied&&!allocated&&!handle_open);
    assert(native_closes==1&&fixture.native_starts==1&&fixture.before_close==1&&fixture.after_close==1);
    assert(fixture.markers==(partial==T26_PARTIAL_CREATE_CANCEL?2u:1u));
    assert(!gpu_starts&&!atomic_load(&p->conversion_readers));
    assert(!x4_stream_pipeline_free(p));t26_active_fixture=NULL;
    printf("PASS T26 partial start=%u: allocated outputs, no Decode, pre/post close hooks, one native-close double and zero retained resources\n",partial);
}
int main(int argc,char **argv)
{
    assert(argc==2);report_directory=argv[1];
    atomic_bool stop;atomic_init(&stop,false);X4T26PipelineHooks invalid={0};
    assert(!x4_stream_pipeline_t26_create(1,&stop,NULL));
    assert(!x4_stream_pipeline_t26_create(1,&stop,&invalid));
    invalid.native.config=(X4T26Config){.api=2,.depth=2,.realistic=false};
    assert(!x4_stream_pipeline_t26_create(1,&stop,&invalid));
    invalid.native.config.realistic=true;invalid.native.config.api=1;
    assert(!x4_stream_pipeline_t26_create(1,&stop,&invalid));
    t26_run_hooks(1,false);t26_run_hooks(2,false);t26_run_hooks(2,true);
    t26_partial_start_cleanup(T26_PARTIAL_QUERY_REJECT);
    t26_partial_start_cleanup(T26_PARTIAL_CREATE_CANCEL);
    puts("PASS T26 real production-scheduler hooks; host native/GPU doubles, not console evidence");
    return 0;
}
