/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual scheduler, leases, overlays and export; decoder/VideoOut/GPU ABI
 * substitutes are not console execution or shader-performance measurements. */
#include <stdlib.h>
#include <stdbool.h>
static bool scratch_fail;
static unsigned verify_delay_us;
static bool verify_corrupt;
static bool next_prepare_fail,next_scratch_fail;
static void *scratch_malloc(size_t);
#define malloc scratch_malloc
#define x4_gpu_quality_reference_rows test_gpu_quality_reference_rows
#define X4_GPU_INTEGRATION_TEST 1
#define main unused_legacy_scheduler_main
#include "stream_pipeline_runtime_test.c"
#undef main
#undef malloc
#undef x4_gpu_quality_reference_rows
static unsigned quality_reference_calls;
int x4_gpu_quality_reference_rows(const X4VideoNv12Layout *,const uint8_t *,size_t,
    uint32_t *,size_t,unsigned,unsigned,const X4VideoPipelineColor *,uint32_t,void *,size_t);
int test_gpu_quality_reference_rows(const X4VideoNv12Layout *layout,const uint8_t *source,size_t source_bytes,
    uint32_t *destination,size_t destination_bytes,unsigned first,unsigned rows,const X4VideoPipelineColor *color,
    uint32_t word,void *scratch,size_t scratch_bytes)
{
    ++quality_reference_calls;
    return x4_gpu_quality_reference_rows(layout,source,source_bytes,destination,destination_bytes,
        first,rows,color,word,scratch,scratch_bytes);
}
/* Native GPU is simulated by CPU writes into ordinary host memory, never WC.
 * This copy deliberately does not reach the legacy Lab-only MOVNTDQA helper. */
static int test_gpu_copy_rows(const uint32_t *source,size_t bytes,unsigned pitch,
    uint32_t *dst,size_t capacity,unsigned first,unsigned rows)
{
    assert(source&&dst&&pitch>=1920&&rows&&rows<=16&&first+rows<=1080);
    assert(bytes>=((size_t)(first+rows-1)*pitch+1920)*4&&capacity>=(size_t)rows*1920*4);
    for(unsigned y=0;y<rows;++y)memcpy(dst+(size_t)y*1920,source+(size_t)(first+y)*pitch,1920u*4);
    return 0;
}
static void *scratch_malloc(size_t n){return scratch_fail&&n==x4_gpu_quality_verify_scratch_bytes()?NULL:malloc(n);}

static unsigned gpu_starts,gpu_destroys;
#if !X4_PUBLIC
static unsigned selected_backend=1,selected_quality_word;
#endif
static bool gpu_signal,prepare_fail,gpu_timestamp_calibrated;
static int gpu_allowed=1;
static uint64_t label_ready_at,label_cancel_at;
static atomic_bool *label_cancel;
static unsigned label_reads;
static bool gpu_overlay_unique_tail;
static atomic_bool verify_worker_hold,verify_worker_entered,verify_worker_release;
struct X4GpuConvert { X4GpuConvertFrame frame;X4GpuConvertStatus status;bool signalled;
 atomic_uint snapshot_state;X4GpuVerifySnapshot snapshot;X4GpuVerifyResult result;
 uint8_t *snapshot_source;uint32_t *snapshot_actual;unsigned snapshot_first,snapshot_rows;
};
unsigned x4_stream_settings_conversion_backend(void)
{
#if X4_PUBLIC
 abort(); /* Any accidental Lab preference access fails the public test. */
#else
 return selected_backend;
#endif
}
unsigned x4_gpu_quality_settings_get(void)
{
#if X4_PUBLIC
 abort();
#else
 return selected_quality_word;
#endif
}
#if X4_PUBLIC
#include "../src/ui/picture_settings.h"
static X4PictureSettingsConfig public_picture={X4_PIC_QUALITY_1080P_ALTA,1,X4_VSYNC_AUTO,true};
static unsigned public_settings_reads;
X4PictureSettingsConfig x4_picture_settings_config(void){++public_settings_reads;return public_picture;}
X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption quality)
{assert(quality==X4_PIC_QUALITY_1080P_ALTA);return X4_REQUEST_1080P_G;}
#endif
X4GpuConvert *x4_gpu_convert_open(void)
{if(prepare_fail)return NULL;X4GpuConvert *g=calloc(1,sizeof(*g));assert(g);g->status.prepared=true;g->status.reason="no";
 atomic_init(&g->snapshot_state,0);
 g->status.verification_last_result=g->status.verification_reserve_result=g->status.verification_map_result=
 g->status.verification_control_reserve_result=g->status.verification_control_map_result=X4_GPU_CONVERT_NOT_RUN;
 g->status.quality_reason="off";g->status.quality_reserve_result=g->status.quality_map_result=
 g->status.quality_unmap_result=g->status.quality_release_result=g->status.intermediate_reserve_result=
 g->status.intermediate_map_result=g->status.intermediate_unmap_result=g->status.intermediate_release_result=X4_GPU_CONVERT_NOT_RUN;return g;}
bool x4_gpu_convert_admit(const X4VideoNv12Layout *l,X4VideoFormat f,const X4VideoPipelineColor *c,const char **r)
{bool ok=l&&c&&((l->visible_width==1920&&l->visible_height==1080)||
 (l->visible_width==1280&&l->visible_height==720))&&f==X4_VIDEO_FORMAT_NV12;*r=ok?"no":"geometry_not_supported";return ok;}
int x4_gpu_convert_verification_prepare(X4GpuConvert *g)
{
 if(!g)return -1;g->snapshot_source=malloc(1920u*1088*3/2);g->snapshot_actual=malloc(16u*1920*4);
 if(!g->snapshot_source||!g->snapshot_actual)return -1;
 g->status.verification_prepared=true;
 g->status.verification_reserve_result=g->status.verification_map_result=
 g->status.verification_control_reserve_result=g->status.verification_control_map_result=0;
 g->status.verification_native_bytes=3276800;
 g->status.verification_control_bytes=65536;return 0;
}
bool x4_gpu_convert_verification_request(X4GpuConvert*g,unsigned first,unsigned rows)
{
 unsigned expected=0;if(!atomic_compare_exchange_strong(&g->snapshot_state,&expected,1)){++g->status.verification_skipped;return false;}
 g->snapshot_first=first;g->snapshot_rows=rows;return true;
}
bool x4_gpu_convert_verification_take(X4GpuConvert*g,X4GpuVerifySnapshot*out)
{unsigned expected=2;if(!atomic_compare_exchange_strong_explicit(&g->snapshot_state,&expected,3,memory_order_acq_rel,memory_order_relaxed))return false;
 if(atomic_load_explicit(&verify_worker_hold,memory_order_acquire)){
  atomic_store_explicit(&verify_worker_entered,true,memory_order_release);
  while(!atomic_load_explicit(&verify_worker_release,memory_order_acquire))usleep(100);
 }
 if(verify_delay_us)usleep(verify_delay_us);*out=g->snapshot;return true;}
int x4_gpu_convert_verification_complete(X4GpuConvert*g,const X4GpuVerifyResult*r)
{assert(atomic_load(&g->snapshot_state)==3);assert(r->token==g->snapshot.token&&r->owner_epoch==g->snapshot.owner_epoch);
 g->result=*r;atomic_store_explicit(&g->snapshot_state,4,memory_order_release);return 0;}
bool x4_gpu_convert_verification_collect(X4GpuConvert*g,X4GpuVerifyResult*out)
{
 if(!g||atomic_load_explicit(&g->snapshot_state,memory_order_acquire)!=4)return false;
 X4GpuVerifyResult ignored;if(!out)out=&ignored;*out=g->result;
 ++g->status.verification_checks;g->status.verification_pixels+=out->checked_pixels;g->status.verification_outside+=out->pixels_outside;
 g->status.verification_last_worker_us=out->worker_us;g->status.verification_last_result=out->result;
 if(out->max_channel_difference>g->status.verification_max_difference)g->status.verification_max_difference=out->max_channel_difference;
 if(out->result||out->pixels_outside){++g->status.verification_errors;g->status.verification_failed=true;g->status.disabled=true;g->status.reason="verification_mismatch";}
 atomic_store_explicit(&g->snapshot_state,0,memory_order_release);return true;
}
bool x4_gpu_convert_verification_cancel(X4GpuConvert*g)
{unsigned st=atomic_load(&g->snapshot_state);if(st==3||g->status.pending)return false;atomic_store(&g->snapshot_state,0);return true;}
int x4_gpu_convert_start(X4GpuConvert *g,const X4GpuConvertFrame *f,uint64_t now)
{X4GpuVerifyResult ignored;(void)x4_gpu_convert_verification_collect(g,&ignored);assert(pthread_equal(pthread_self(),main_owner));if(g->status.disabled)return X4_GPU_CONVERT_CPU;
 uint32_t word=x4_gpu_quality_normalize(f->quality_word);
 bool applicable=f->layout.visible_width==1920 && f->layout.visible_height==1080 && f->color.convert_mode==X4_CONVERT_SMOOTH;
 g->status.quality_requested_word=word;g->status.quality_effective_word=0;
 g->status.quality_active=applicable&&x4_gpu_quality_active(word,f->color.sharpness);
 g->status.quality_cas=g->status.quality_active&&(word&X4_GPU_QUALITY_CAS)!=0&&f->color.sharpness!=0;
 g->status.quality_key=g->status.quality_active?(word&15u)|(f->color.sharpness<<4):0;
 g->status.quality_reason=word&&!applicable?"mejoras_solo_1080p_suave":g->status.quality_active?"pending":"off";
 g->status.allowed_result=gpu_allowed;
 if(!gpu_allowed){if(!g->status.pause_active){g->status.pause_active=true;g->status.pause_started_us=now;
     ++g->status.pause_episodes;g->status.pause_current_frames=0;}
   g->status.reason="submits_paused";
   if(g->status.quality_active&&(word&X4_GPU_QUALITY_REPEAT)){++g->status.pausa_repetidas;g->status.quality_reason="pause_repeat";return X4_GPU_CONVERT_REPEAT;}
   ++g->status.pause_current_frames;++g->status.pause_cpu_frames;return X4_GPU_CONVERT_CPU;}
 if(g->status.pause_active){g->status.pause_active=false;g->status.pause_last_us=now-g->status.pause_started_us;
   g->status.pause_last_frames=g->status.pause_current_frames;
   if(g->status.pause_last_us>g->status.pause_max_us)g->status.pause_max_us=g->status.pause_last_us;}
 if(gpu_allowed<0){g->status.disabled=true;g->status.reason="submits_error";return X4_GPU_CONVERT_CPU;}
 if(g->status.quality_active){g->status.quality_prepared=true;g->status.quality_arena_bytes=262144;
   g->status.quality_effective_word=word;g->status.quality_reason="active";
   if(g->status.quality_cas)g->status.quality_intermediate_bytes=33226752;}
 g->status.reason="no";
 assert(!g->status.pending);g->frame=*f;g->status.pending=true;g->status.submitted_us=now;++g->status.accepted;++gpu_starts;return 0;}
X4GpuConvertPoll x4_gpu_convert_poll(X4GpuConvert *g,uint64_t now,uint64_t *elapsed)
{
    X4GpuVerifyResult ignored;(void)x4_gpu_convert_verification_collect(g,&ignored);
    if(!g->status.pending)return X4_GPU_CONVERT_IDLE;
    *elapsed=now-g->status.submitted_us;
    if(g->status.verification_failed&&!g->status.quarantined){g->status.quarantined=true;++g->status.quarantines;return X4_GPU_CONVERT_VERIFY_QUARANTINE;}
    if(gpu_signal){
        if(g->status.quality_active){
            size_t bytes=x4_gpu_quality_reference_scratch_bytes(16);void *scratch=malloc(bytes);assert(scratch);
            for(unsigned first=0;first<1080;first+=16){unsigned rows=1080-first<16?1080-first:16;
                assert(!x4_gpu_quality_reference_rows(&g->frame.layout,g->frame.source,g->frame.source_bytes,
                    g->frame.destination+(size_t)first*1920,(size_t)rows*1920*4,first,rows,
                    &g->frame.color,g->frame.quality_word,scratch,bytes));}
            free(scratch);
        }else for(unsigned stripe=0;stripe<2;++stripe)
            assert(!x4_video_pipeline_convert_stripe_color(&g->frame.layout,g->frame.source,g->frame.source_bytes,
                g->frame.destination,g->frame.destination_bytes,1920,2,stripe,&g->frame.color));
        if(atomic_load_explicit(&g->snapshot_state,memory_order_acquire)==1){
            assert(!x4_gpu_quality_verify_plan(&g->frame.layout,&g->frame.color,g->status.quality_effective_word,g->snapshot_first,g->snapshot_rows,&g->snapshot));
            g->snapshot.token=++g->status.verification_captures;g->snapshot.owner_epoch=1;
            g->snapshot.color=g->frame.color;g->snapshot.quality_word=g->status.quality_effective_word;
            memcpy(g->snapshot_source,g->frame.source,g->frame.source_bytes);
            g->snapshot.source=g->snapshot_source;g->snapshot.source_bytes=g->frame.source_bytes;
            assert(!test_gpu_copy_rows(g->frame.destination,g->frame.destination_bytes,1920,
                g->snapshot_actual,16u*1920*4,g->snapshot_first,g->snapshot_rows));
            if(verify_corrupt)g->snapshot_actual[0]^=0xff000000u;
            g->snapshot.actual=g->snapshot_actual;++g->status.verification_copy_completed;
            g->status.verification_last_copy_real_valid=gpu_timestamp_calibrated;g->status.verification_last_copy_real_us=25;
            g->status.verification_last_enqueue_us=10;atomic_store_explicit(&g->snapshot_state,2,memory_order_release);
        }
        if(*elapsed>=50000&&!g->status.disabled){g->status.disabled=true;g->status.reason="timeout";
            ++g->status.timeouts;++g->status.quarantines;}
        g->status.pending=false;g->status.quarantined=false;g->signalled=true;++g->status.completed;
        g->status.last_prep_us=*elapsed<100?*elapsed:100;
        g->status.last_eop_us=*elapsed-g->status.last_prep_us;
        g->status.last_resolution=g->frame.layout.visible_width==1280?X4_GPU_RESOLUTION_720:X4_GPU_RESOLUTION_1080;
        X4GpuTimestampStatus *ts=&g->status.timestamps[g->status.last_resolution];
        ++ts->samples;g->status.last_start_ticks=1000000+ts->samples*20000;
        g->status.last_end_ticks=g->status.last_start_ticks+250;
        ts->last_end_ticks=g->status.last_end_ticks;ts->last_observed_us=now;
        if(!ts->first_end_ticks){ts->first_end_ticks=ts->last_end_ticks;ts->first_observed_us=now;}
        g->status.last_real_valid=gpu_timestamp_calibrated;
        if(gpu_timestamp_calibrated){++ts->valid_samples;ts->clock_calibrated=true;ts->clock_hz=1000000;g->status.last_real_us=250;}
        else ++ts->pending_samples;
        if(g->status.disabled){++g->status.late_completed;return X4_GPU_CONVERT_LATE_DONE;}
        return X4_GPU_CONVERT_DONE;
    }
    if(*elapsed>=50000&&!g->status.disabled){g->status.disabled=true;g->status.reason="timeout";
        g->status.quarantined=true;++g->status.timeouts;++g->status.quarantines;return X4_GPU_CONVERT_TIMEOUT;}
    return X4_GPU_CONVERT_WAIT;
}
bool x4_gpu_convert_pending(const X4GpuConvert *g){return g&&g->status.pending;}
bool x4_gpu_convert_label_ready(X4GpuConvert *g)
{
    assert(g&&g->status.pending);++label_reads;
    uint64_t controlled=atomic_load(&fixture_clock);
    if(controlled){controlled+=25;atomic_store(&fixture_clock,controlled);
        if(label_cancel_at&&controlled>=label_cancel_at)atomic_store(label_cancel,true);
        return label_ready_at&&controlled>=label_ready_at;}
    return gpu_signal;
}
bool x4_gpu_convert_disabled(const X4GpuConvert *g){return g&&g->status.disabled;}
const char *x4_gpu_convert_reason(const X4GpuConvert *g){return g?g->status.reason:"prepare_memory";}
void x4_gpu_convert_status(const X4GpuConvert *g,X4GpuConvertStatus *out){*out=g->status;}
bool x4_gpu_convert_close(X4GpuConvert **g,uint64_t now)
{(void)now;if(!*g)return true;if((*g)->status.pending||atomic_load(&(*g)->snapshot_state)==3)return false;free((*g)->snapshot_source);free((*g)->snapshot_actual);free(*g);*g=NULL;++gpu_destroys;return true;}
static void promote_ready(X4StreamPipeline *p)
{
    /* The shared scheduler double delivers 720p. Replace one unclaimed mock
     * native output with an owned 1080p NV12 allocation, keeping the SAME
     * real ring lease and native destructor. No producer/converter reads it. */
    lock(&p->ring_gate);Native *n=p->video.state;bool found=false;
    for(unsigned i=0;i<p->ring.output_count;++i)if(p->ring.slots[i].state==X4_PIPELINE_READY){
        assert(i<4);free(n->pixels[i]);n->pixels[i]=malloc(1920u*1088*3/2);assert(n->pixels[i]);
        memset(n->pixels[i],90,1920u*1088);memset(n->pixels[i]+1920u*1088,128,1920u*544);
        X4ProbeNativeOutput *o=&p->images[i];o->pixels=n->pixels[i];o->owned_bytes=o->returned_bytes=1920u*1088*3/2;
        o->layout=(X4VideoNv12Layout){.visible_width=1920,.visible_height=1080,.coded_width=1920,.coded_height=1088,
            .storage_height=1088,.pitch=1920,.y_bytes=1920u*1088,.uv_bytes=1920u*544,.uv_plane_offset=1920u*1088,
            .visible_uv_offset=1920u*1088,.total_nv12_bytes=1920u*1632,.rgb_pixels=1920u*1080,.rgb_bytes=1920u*1080*4};
        o->native_width=1920;o->native_height=1088;o->pitch=1920;found=true;
    }
    p->native_info.sps.chroma_format_idc=1;p->native_info.sps.bit_depth_luma=p->native_info.sps.bit_depth_chroma=8;
    unlock(&p->ring_gate);assert(found);
}
static X4StreamPipeline *opened(atomic_bool *stop,unsigned session,bool hd)
{
    reset();gpu_signal=verify_corrupt=gpu_timestamp_calibrated=false;gpu_overlay_unique_tail=false;
    atomic_store(&verify_worker_hold,false);atomic_store(&verify_worker_entered,false);atomic_store(&verify_worker_release,false);
    prepare_fail=next_prepare_fail;scratch_fail=next_scratch_fail;next_prepare_fail=next_scratch_fail=false;
    atomic_store(&cpu_calls_enabled,true);
    quality_reference_calls=0;
    verify_delay_us=0;gpu_allowed=1;gpu_starts=gpu_destroys=0;
    X4StreamPipeline *p;
#if X4_PUBLIC
    /* Capture actual public settings mapping; the legacy native double below
     * completes an offered slot immediately, so depth1 isolates presentation. */
    atomic_init(stop,false);X4StreamRequestConfig request=x4_stream_settings_get();
    request.decoder_depth=X4_DEPTH_1;
    p=x4_stream_pipeline_create_request(session,NULL,stop,&request);assert(p);
#else
    if(selected_quality_word){
        /* The legacy request constructor leaves picture fields at zero
         * (Original/No). Quality cases must request Smooth/Medium before
         * capture, including later native epochs and the 720p guard case. */
        printf("fixture CASE_BEGIN profile=%d case=%u clock=%llu\n",X4_STREAM_PROFILE,session,(unsigned long long)sceKernelGetProcessTime());
        atomic_init(stop,false);
        X4StreamRequestConfig request=x4_stream_request_config(X4_STREAM_PROFILE==2?X4_REQUEST_1080P_A:X4_REQUEST_720P60,X4_COLOR_AUTO_709);
        request.decoder_depth=X4_DEPTH_1;request.convert_mode=X4_CONVERT_SMOOTH;request.sharpness=X4_SHARPNESS_MED;
        p=x4_stream_pipeline_create_request(session,NULL,stop,&request);assert(p);
    }else p=new_pipeline(stop,session);
#endif
    assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));
    Producer one={p,1,0,false};produce(&one);assert(!one.result);if(hd)promote_ready(p);
    else {p->native_info.sps.chroma_format_idc=1;p->native_info.sps.bit_depth_luma=p->native_info.sps.bit_depth_chroma=8;}
    return p;
}
static void one_more(X4StreamPipeline *p)
{
    uint64_t identity=native_outputs;const uint8_t sps[]={0,0,0,1,0x67},pps[]={0,0,0,1,0x68,0x80};
    assert(!x4_stream_pipeline_submit_parameters(p,(const uint8_t*)&identity,8,identity*16667,identity,sps,sizeof(sps),pps,sizeof(pps)));
    uint64_t deadline=sceKernelGetProcessTime()+1000000;
    while(atomic_load(&p->consumed)<=identity){assert(sceKernelGetProcessTime()<deadline);usleep(100);}
}
static void gpu_overlay(uint32_t *pixels,void *context)
{
    X4StreamPipeline *p=context;
    if(p->gpu&&x4_gpu_convert_pending(p->gpu))assert(pixels!=p->gpu->frame.destination);
    else if(p->gpu_metrics.effective)assert(p->gpu->signalled);
    pixels[0]=0xffabcdef;
    if(gpu_overlay_unique_tail)pixels[1920u*1080u-1]=0xff000000u|(uint32_t)p->converting_image.output_id;
    ++overlays;
}
static void present_until(X4StreamPipeline *p,uint64_t count)
{
    uint64_t deadline=sceKernelGetProcessTime()+2000000;
    while(p->view.presentations<count){assert(sceKernelGetProcessTime()<deadline);
        assert(!x4_stream_pipeline_present(p,gpu_overlay,p));usleep(100);}
}
static void clean(X4StreamPipeline *p)
{
#if X4_PUBLIC
 assert(!p->exported); /* This fixture never invokes the public exporter manually. */
#endif
 assert(!x4_stream_pipeline_close(p));assert(!p->retained&&!allocated&&!handle_open&&native_closes==1);
 assert(!atomic_load(&p->conversion_readers)&&!p->ring.occupied);
#if X4_PUBLIC
 assert(p->exported);assert(!x4_stream_pipeline_close(p)); /* idempotent, no second export */
#else
 assert(!x4_stream_pipeline_export(p,NULL));
#endif
 assert(!x4_stream_pipeline_free(p));
}
static void pause_log_rate(void)
{
    X4StreamPipeline *p=calloc(1,sizeof(*p));assert(p);
    p->gpu=calloc(1,sizeof(*p->gpu));assert(p->gpu);p->gpu_metrics.requested=1;
    for(unsigned episode=1;episode<=3;++episode){
        p->gpu->status.pause_episodes=episode;p->gpu->status.pause_active=true;
        p->gpu->status.pause_started_us=now_us();gpu_status(p);
        ++p->gpu_metrics.pause_cpu_frames;p->gpu->status.pause_active=false;
        p->gpu->status.pause_last_us=episode*100;
        gpu_log(p,episode==1?0:episode==2?500000:700000);
        assert(p->gpu_pause_log_emitted&&p->gpu_pause_log_at==0&&p->gpu_pause_log_seen==episode);
        assert(p->gpu_pause_log_pending==(episode!=1));
    }
    assert(p->gpu_metrics.pause_episodes==3&&p->gpu_pause_log_duration==300&&p->gpu_pause_log_frames==1);
    gpu_log(p,1000000);assert(!p->gpu_pause_log_pending&&p->gpu_pause_log_at==1000000);
    gpu_log(p,1000001);assert(!p->gpu_pause_log_pending&&p->gpu_pause_log_at==1000000);
    free(p->gpu);free(p);
    puts("PASS pause log coalesces two completions within one second, preserves three episodes and handles first emission at clock zero");
}
static void quick_label_wait(void)
{
    X4StreamPipeline *p=calloc(1,sizeof(*p));assert(p);
    p->gpu=calloc(1,sizeof(*p->gpu));assert(p->gpu);p->gpu->status.pending=true;
    atomic_bool stop;atomic_init(&stop,false);atomic_init(&p->cancel,false);p->external_stop=&stop;
    assert(!x4_stream_wait_init(&p->present_wait,"label-wait-test"));
    assert(!x4_stream_wait_notify(&p->present_wait));
    X4StreamWaitStats before={0},after={0};x4_stream_wait_snapshot(&p->present_wait,&before);
    uint64_t api_before=x4_stream_wait_test_api_calls();
    atomic_store(&fixture_clock,1000000);label_reads=0;label_ready_at=1000200;
    assert(!x4_stream_pipeline_wait_present(p,1000));
    assert(atomic_load(&fixture_clock)==1000200&&label_reads==8&&p->gpu_metrics.spin_ready==1);
    assert(p->gpu_metrics.spin.max==200&&p->gpu->status.pending);
    x4_stream_wait_snapshot(&p->present_wait,&after);
    assert(!memcmp(&before,&after,sizeof(before))&&x4_stream_wait_test_api_calls()==api_before);
    /* No label before the deadline: exactly timeout_us, not a 1/5-ms wait. */
    label_ready_at=0;label_reads=0;atomic_store(&fixture_clock,2000000);
    assert(!x4_stream_pipeline_wait_present(p,375));
    assert(atomic_load(&fixture_clock)==2000375&&label_reads==15&&p->gpu_metrics.spin_timeout==1);
    assert(x4_stream_wait_test_api_calls()==api_before);
    /* A zero budget does not even load/flush the label. */
    label_reads=0;assert(!x4_stream_pipeline_wait_present(p,0));assert(!label_reads);
    assert(p->gpu_metrics.spin_timeout==2);
    label_cancel=&stop;label_cancel_at=3000100;atomic_store(&fixture_clock,3000000);
    assert(!x4_stream_pipeline_wait_present(p,1000));assert(atomic_load(&fixture_clock)==3000100);
    assert(atomic_load(&stop)&&p->gpu_metrics.spin_timeout==2);
    label_cancel_at=0;label_cancel=NULL;atomic_store(&fixture_clock,0);
    /* Already-cancelled error screens keep their old blocking/token path,
     * even if unsafe-to-free GPU work remains pending. */
    unsigned reads=label_reads;assert(!x4_stream_pipeline_wait_present(p,1000));
    x4_stream_wait_snapshot(&p->present_wait,&after);
    assert(after.wait_calls==before.wait_calls+1&&after.wakeups==before.wakeups+1&&after.timeouts==before.timeouts);
    assert(label_reads==reads);atomic_store(&stop,false);p->gpu->status.pending=false;
    /* Without GPU work the unchanged normal path consumes exactly one token. */
    assert(!x4_stream_wait_notify(&p->present_wait));assert(!x4_stream_pipeline_wait_present(p,1000));
    x4_stream_wait_snapshot(&p->present_wait,&after);
    assert(after.wait_calls==before.wait_calls+2&&after.wakeups==before.wakeups+2&&after.timeouts==before.timeouts);
    assert(!x4_stream_wait_close(&p->present_wait));assert(!x4_stream_wait_destroy(&p->present_wait));
    free(p->gpu);free(p);
    puts("PASS fast-label owner wait: T=200us<1000us, exact 375us deadline, zero budget, cancellation and retained semaphore consumed only on unchanged normal path");
}
static void timestamp_resolution_publication(void)
{
    X4StreamPipeline *p=calloc(1,sizeof(*p));assert(p);
    p->gpu=calloc(1,sizeof(*p->gpu));assert(p->gpu);
    X4GpuConvertStatus *s=&p->gpu->status;
    /* A completed 720p observation owns the global latest raw pair. */
    s->last_resolution=X4_GPU_RESOLUTION_720;s->timestamps[0].samples=1;
    s->last_start_ticks=100;s->last_end_ticks=120;gpu_status(p);
    assert(p->gpu_metrics.resolution[0].latest_start_ticks==100&&p->gpu_metrics.resolution[0].latest_end_ticks==120);
    /* Start changes last_resolution before EOP and leaves the OLD raw pair.
     * Pending must not publish that pair as a sample of the new resolution. */
    s->pending=true;s->last_resolution=X4_GPU_RESOLUTION_1080;gpu_status(p);
    assert(!p->gpu_metrics.resolution[1].latest_end_ticks);
    s->pending=false;s->timestamps[1].samples=1;s->last_start_ticks=200;s->last_end_ticks=240;gpu_status(p);
    assert(p->gpu_metrics.resolution[1].latest_start_ticks==200&&p->gpu_metrics.resolution[1].latest_end_ticks==240);
    /* Return to a resolution with an existing sample. Until this EOP, keep
     * its own previous pair, including in a retained/pending export. */
    s->pending=true;s->last_resolution=X4_GPU_RESOLUTION_720;gpu_status(p);
    assert(p->gpu_metrics.pending&&p->gpu_metrics.resolution[0].samples==1);
    assert(p->gpu_metrics.resolution[0].latest_start_ticks==100&&p->gpu_metrics.resolution[0].latest_end_ticks==120);
    assert(p->gpu_metrics.resolution[1].latest_start_ticks==200&&p->gpu_metrics.resolution[1].latest_end_ticks==240);
    s->pending=false;s->timestamps[0].samples=2;s->last_start_ticks=300;s->last_end_ticks=340;gpu_status(p);
    assert(p->gpu_metrics.resolution[0].latest_start_ticks==300&&p->gpu_metrics.resolution[0].latest_end_ticks==340);
    assert(p->gpu_metrics.resolution[1].latest_end_ticks==240);
    free(p->gpu);free(p);
    puts("PASS 720p->1080p->720p raw timestamp publication: pending work never reattributes the preceding EOP pair");
}
static void await_verification(X4StreamPipeline*p)
{
 uint64_t end=now_us()+2000000;
 while(!p->gpu_metrics.checks){X4GpuVerifyResult result;(void)x4_gpu_convert_verification_collect(p->gpu,&result);gpu_status(p);assert(now_us()<end);usleep(100);}
}
#if !X4_PUBLIC
static void mailbox_mismatch_ownership(atomic_bool *stop)
{
 selected_backend=1;selected_quality_word=0;
 X4StreamPipeline *p=opened(stop,2399,true);
 /* The inherited VideoOut mock registers three indices. The fourth test-only
  * allocation is used ONLY by the WRITING job that will be quarantined: it
  * is never submitted. Production's real 4-registration is covered by T27.
  * No live owner exists yet, so initialize the 4-slot model cold. */
 uint32_t *fourth=malloc(STREAM_SCANOUT_BYTES);assert(fourth);memset(fourth,0x5a,STREAM_SCANOUT_BYTES);
 p->display_pixels[3]=fourth;
 assert(!x4_video_pipeline_display_init_mode(&p->display,p->session,4,true));
 assert(!x4_video_pipeline_display_mailbox_admission(&p->display,true));
 x4_stream_diagnostics_vsync_init(p->diagnostics,X4_VSYNC_ALWAYS);
 atomic_store(&verify_worker_hold,true);verify_corrupt=true;gpu_signal=true;gpu_overlay_unique_tail=true;
 present_until(p,1);uint64_t deadline=now_us()+2000000;
 while(!atomic_load_explicit(&verify_worker_entered,memory_order_acquire)){assert(now_us()<deadline);usleep(100);}
 assert(p->display.screen>=0&&p->gpu_metrics.gpu_frames==1);
 unsigned screen_slot=(unsigned)p->display.screen;X4VideoPipelineScanout old_screen=p->display.slots[screen_slot];
 /* Hold the second VSYNC pending while GPU completes a third READY. */
 flip_delay=100000;one_more(p);promote_ready(p);deadline=now_us()+2000000;
 while(p->display.pending<0){assert(now_us()<deadline);assert(!x4_stream_pipeline_present(p,gpu_overlay,p));usleep(100);}
 unsigned pending_slot=(unsigned)p->display.pending;X4VideoPipelineScanout old_pending=p->display.slots[pending_slot];
 assert(old_pending.presentation_mode==X4_PRESENT_VSYNC_MAILBOX);
 one_more(p);promote_ready(p);deadline=now_us()+2000000;
 while(p->view.conversions<3){assert(now_us()<deadline);assert(!x4_stream_pipeline_present(p,gpu_overlay,p));usleep(100);}
 unsigned ready_slot=4;for(unsigned i=0;i<4;++i)if(p->display.slots[i].state==X4_SCANOUT_READY)ready_slot=i;
 assert(ready_slot<3&&p->scanout_gpu[ready_slot]&&p->display.slots[ready_slot].output_id==3);
 one_more(p);promote_ready(p);gpu_signal=false;assert(!x4_stream_pipeline_present(p,gpu_overlay,p));
 assert(p->gpu_active&&p->converting&&p->scanout_lease.slot==3&&p->display.slots[3].state==X4_SCANOUT_WRITING);
 X4VideoPipelineLease held_image=p->image_lease;unsigned flips_before=flip_count;
 uint32_t screen_tail=p->display_pixels[screen_slot][1920u*1080-1];
 uint32_t pending_tail=p->display_pixels[pending_slot][1920u*1080-1];
 assert(p->display.slots[screen_slot].state==X4_SCANOUT_SCREEN&&p->display.slots[pending_slot].state==X4_SCANOUT_PENDING);
 /* Publish the worker mismatch only once all four ownership states coexist. */
 atomic_store_explicit(&verify_worker_release,true,memory_order_release);deadline=now_us()+2000000;
 while(atomic_load_explicit(&p->gpu->snapshot_state,memory_order_acquire)!=4){assert(now_us()<deadline);usleep(100);}
 assert(!x4_stream_pipeline_present(p,gpu_overlay,p));
 assert(p->gpu_metrics.verification_failed&&p->gpu_metrics.disabled&&!p->gpu_metrics.timeouts);
 assert(p->gpu_quarantine&&p->display.slots[3].state==X4_SCANOUT_QUARANTINED);
 assert(p->ring.slots[held_image.slot].state==X4_PIPELINE_CONVERTING&&atomic_load(&p->conversion_readers)==1);
 assert(p->display.slots[ready_slot].state==X4_SCANOUT_FREE&&p->gpu_ready_discarded_verification==1);
 assert(flip_count==flips_before&&p->display.pending==(int)pending_slot&&p->display.screen==(int)screen_slot);
 assert(!memcmp(&old_screen,&p->display.slots[screen_slot],sizeof(old_screen)));
 assert(!memcmp(&old_pending,&p->display.slots[pending_slot],sizeof(old_pending)));
 assert(p->display_pixels[screen_slot][1920u*1080-1]==screen_tail&&p->display_pixels[pending_slot][1920u*1080-1]==pending_tail);
 /* Matching late EOP, not the mismatch, retires the fourth/native leases. */
 gpu_signal=true;assert(!x4_stream_pipeline_present(p,gpu_overlay,p));
 assert(!p->gpu_quarantine&&!atomic_load(&p->conversion_readers));
 assert(p->display.slots[3].state==X4_SCANOUT_FREE&&gpu_starts==4);
 /* A new CPU fallback READY remains valid even with verification_failed set. */
 one_more(p);promote_ready(p);deadline=now_us()+2000000;
 while(p->view.conversions<4){assert(now_us()<deadline);assert(!x4_stream_pipeline_present(p,gpu_overlay,p));usleep(100);}
 unsigned cpu_ready=4;for(unsigned i=0;i<4;++i)if(p->display.slots[i].state==X4_SCANOUT_READY)cpu_ready=i;
 assert(cpu_ready<3&&!p->scanout_gpu[cpu_ready]&&p->display.slots[cpu_ready].output_id==5);
 assert(p->gpu_ready_discarded_verification==1&&flip_count==flips_before);
 flip_delay=0;pending_polls=0;present_until(p,3);
 assert(p->display.slots[p->display.screen].output_id==5&&p->gpu_ready_discarded_verification==1);
 assert(p->display.submitted==3&&p->display.last_submitted_output_id==5);
 assert(!x4_stream_pipeline_close(p)&&!p->retained&&!allocated&&!handle_open&&native_closes==1);
 assert(!p->ring.occupied&&!atomic_load(&p->conversion_readers));free(fourth);p->display_pixels[3]=NULL;
 assert(!x4_stream_pipeline_free(p));
 puts("PASS Mailbox mismatch with SCREEN/PENDING/READY/WRITING: suspect unsent GPU READY dropped, pending VSYNC untouched, WRITING/native quarantined until EOP, CPU READY preserved");
}
int main(int argc,char **argv)
{
 assert(argc==2);report_directory=argv[1];atomic_bool stop;
 quick_label_wait();timestamp_resolution_publication();pause_log_rate();
 selected_backend=1;selected_quality_word=0;X4StreamPipeline*p=opened(&stop,2301,true);
 assert(!x4_stream_pipeline_present(p,gpu_overlay,p));assert(p->gpu_active&&atomic_load(&p->conversion_readers)==1);
 gpu_signal=gpu_timestamp_calibrated=true;verify_delay_us=20000;present_until(p,1);
 /* Worker may still be calculating, but native and scanout leases have already retired. */
 assert(!atomic_load(&p->conversion_readers)&&!p->converting&&overlays==1);
 await_verification(p);assert(p->gpu_metrics.checks==1&&!p->gpu_metrics.pixels_outside);
 assert(p->gpu_metrics.verify.max>=20000&&p->gpu_metrics.checked_pixels==16u*1920);
 assert(p->view.convert_us==p->conversion_menu[0].total);assert(p->gpu_metrics.verification_priority_result==0);
 clean(p);assert(gpu_destroys==1);puts("PASS copied snapshot CPU verification runs after native lease retirement; join before free; 767 priority recorded");
 p=opened(&stop,2310,true);gpu_allowed=0;present_until(p,1);one_more(p);promote_ready(p);present_until(p,2);
 gpu_allowed=1;gpu_signal=true;one_more(p);promote_ready(p);present_until(p,3);await_verification(p);
 assert(gpu_starts==1&&p->gpu_metrics.pause_cpu_frames==2&&!p->gpu_metrics.disabled);clean(p);
 p=opened(&stop,2311,true);gpu_signal=true;verify_corrupt=true;
 /* This case tests mismatch AFTER a GPU image is already SCREEN. Without a
  * barrier the verifier can win before submit, and the correct suspect-READY
  * discard leaves no image to satisfy present_until. Case 2399 covers that
  * unsent-READY/quarantined-WRITING path separately. */
 atomic_store_explicit(&verify_worker_hold,true,memory_order_release);present_until(p,1);
 assert(p->display.screen>=0&&p->gpu_metrics.gpu_frames==1&&!p->gpu_metrics.verification_failed);
 uint64_t entered_deadline=now_us()+2000000;
 while(!atomic_load_explicit(&verify_worker_entered,memory_order_acquire))assert(now_us()<entered_deadline);
 atomic_store_explicit(&verify_worker_release,true,memory_order_release);await_verification(p);
 assert(p->gpu_metrics.disabled&&p->gpu_metrics.pixels_outside==1&&p->gpu_metrics.max_channel_difference>1);
 one_more(p);promote_ready(p);present_until(p,2);assert(gpu_starts==1&&p->gpu_metrics.cpu_frames==1);clean(p);
 puts("PASS mismatch disables future GPU; next frame completes CPU without touching old pixels");
 p=opened(&stop,2302,true);assert(!x4_stream_pipeline_present(p,gpu_overlay,p));uint64_t until=now_us()+500000;
 while(!p->gpu_quarantine){assert(now_us()<until);assert(!x4_stream_pipeline_present(p,gpu_overlay,p));usleep(100);}
 unsigned held=p->gpu_held_image.slot;assert(p->ring.slots[held].state==X4_PIPELINE_CONVERTING);
 one_more(p);present_until(p,1);assert(p->gpu_metrics.cpu_frames&&gpu_starts==1);
 gpu_signal=true;assert(!x4_stream_pipeline_present(p,gpu_overlay,p));assert(!p->gpu_quarantine&&!atomic_load(&p->conversion_readers));clean(p);
 puts("PASS timeout quarantines native+destination until EOP; CPU uses other scanout safely");
 p=opened(&stop,2303,true);assert(!x4_stream_pipeline_present(p,gpu_overlay,p));gpu_signal=true;clean(p);assert(!overlays);
 p=opened(&stop,2304,false);gpu_signal=true;present_until(p,1);await_verification(p);
 assert(!p->gpu_metrics.max_channel_difference&&p->gpu_metrics.verification_bytes==x4_gpu_quality_verify_scratch_bytes());clean(p);
 next_prepare_fail=true;p=opened(&stop,2305,true);present_until(p,1);assert(!gpu_starts&&p->gpu_metrics.disabled&&p->gpu_metrics.cpu_frames);clean(p);
 next_scratch_fail=true;p=opened(&stop,2307,true);present_until(p,1);assert(!gpu_starts&&p->gpu_metrics.disabled);clean(p);
 selected_quality_word=X4_GPU_QUALITY_DITHER|(1u<<1)|X4_GPU_QUALITY_CAS;
 p=opened(&stop,2313,true);unsigned captured=p->gpu_quality_requested_word;selected_quality_word=0;gpu_signal=true;
 present_until(p,1);await_verification(p);assert(captured==11&&!p->gpu_metrics.pixels_outside&&p->gpu_metrics.dither_frames==1);clean(p);
 puts("PASS immutable quality/Dither snapshot, 720p reference, preparation failure and cancel lifecycle");
 mailbox_mismatch_ownership(&stop);return 0;
}
#else
int main(
    int argc,char **argv)
{
 assert(argc==2);report_directory=argv[1];atomic_bool stop;
 /* Keep shared fast-label / per-resolution / pause-accounting checks in the
  * public TU without invoking any Lab preference or enhancement scenario. */
 quick_label_wait();timestamp_resolution_publication();pause_log_rate();
 X4StreamPipeline*p=opened(&stop,2401,true);
 assert(public_settings_reads&&p->request.profile==X4_REQUEST_1080P_G);
 assert(p->gpu_metrics.requested==1&&p->gpu_quality_requested_word==X4_GPU_QUALITY_DITHER);
 assert(!x4_stream_request_gpu_force_cpu(&p->request)&&x4_stream_request_dither_enabled(&p->request));
 assert(p->scanout_count==3&&!p->display.mailbox_capable);
 gpu_signal=gpu_timestamp_calibrated=true;present_until(p,1);await_verification(p);
 assert(gpu_starts==1&&p->gpu_metrics.gpu_frames==1&&!p->gpu_metrics.cpu_frames);
 assert(p->gpu->frame.quality_word==X4_GPU_QUALITY_DITHER&&p->gpu_metrics.dither_frames==1);
 assert(!p->gpu_metrics.pixels_outside);clean(p);
 puts("PASS PUBLIC: default settings captured by value select GPU+Dither, actual scheduler presents, no Lab getter reached");
 public_picture.conversion_backend=0;p=opened(&stop,2402,true);
 assert(x4_stream_request_gpu_force_cpu(&p->request)&&p->gpu_metrics.requested==0&&!p->gpu);
 assert(!p->verification_thread.created);
 assert(p->gpu_metrics.verification_priority_result==X4_GPU_CONVERT_NOT_RUN);
 assert(p->gpu_metrics.verification_last_result==X4_GPU_CONVERT_NOT_RUN);
 assert(p->gpu_metrics.verification_reserve_result==X4_GPU_CONVERT_NOT_RUN&&p->gpu_metrics.verification_map_result==X4_GPU_CONVERT_NOT_RUN);
 assert(p->gpu_metrics.verification_control_reserve_result==X4_GPU_CONVERT_NOT_RUN&&p->gpu_metrics.verification_control_map_result==X4_GPU_CONVERT_NOT_RUN);
 assert(!strcmp(p->gpu_metrics.quality_reason,"ignored_cpu"));present_until(p,1);
 assert(!strcmp(p->gpu_metrics.quality_reason,"ignored_cpu")&&p->gpu_metrics.quality_effective_word==0);
 assert(p->gpu_metrics.verification_priority_result==X4_GPU_CONVERT_NOT_RUN);

 assert(!gpu_starts&&!p->gpu_metrics.gpu_frames&&p->gpu_metrics.cpu_frames==1&&!p->gpu_metrics.dither_frames);
 clean(p);puts("PASS PUBLIC: explicit FORCE_CPU creates no GPU/verifier and presents via unchanged CPU conversion");
 public_picture.conversion_backend=1;p=opened(&stop,2403,true);gpu_allowed=0;
 assert(p->gpu_quality_requested_word==X4_GPU_QUALITY_DITHER);present_until(p,1);
 assert(!gpu_starts&&!p->gpu_metrics.disabled&&p->gpu_metrics.pause_active);
 assert(p->gpu_metrics.cpu_frames==1&&!p->gpu_metrics.dither_frames&&!p->gpu_metrics.gpu_frames);
 assert(p->gpu_metrics.quality_effective_word==0&&!p->gpu->status.quality_prepared);
 /* A later allowed submission resumes GPU with captured Dither. */
 gpu_allowed=1;gpu_signal=true;one_more(p);promote_ready(p);present_until(p,2);await_verification(p);
 assert(gpu_starts==1&&p->gpu_metrics.dither_frames==1&&p->gpu_metrics.cpu_frames==1&&!p->gpu_metrics.disabled);
 clean(p);puts("PASS PUBLIC: AreSubmitsAllowed0 converts CPU without Dither; later GPU resumes; no Repeat/CAS scenarios");
 /* A diagnostic I/O failure cannot turn a clean media close into a failure.
  * Use a regular log-file path as directory; no existing file is changed. */
 public_picture.conversion_backend=0;p=opened(&stop,2404,true);
 char blocked[1024];int n=snprintf(blocked,sizeof(blocked),"%s/runtime.log",argv[1]);assert(n>0&&(size_t)n<sizeof(blocked));
 report_directory=blocked;assert(!x4_stream_pipeline_close(p));
 assert(p->closed&&!p->retained&&!p->exported&&!p->decoder_thread.created&&!p->verification_thread.created);
 assert(!allocated&&!handle_open&&native_closes==1&&!atomic_load(&p->conversion_readers));
 assert(!x4_stream_pipeline_close(p)&&!p->exported); /* automatic diagnostic attempted once */
 assert(!x4_stream_pipeline_free(p));report_directory=argv[1];
 puts("PASS PUBLIC: close automatically exports while trace is alive; failed export is logged once and leaves clean close successful");
 return 0;
}
#endif
