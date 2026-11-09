/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_realistic.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "stream_pipeline.h"
#include "stream_au_queue.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { REAL_CONTRACT=-7660,REAL_MEMORY=-7661,REAL_CANCELLED=3,REAL_BUDGET=4,
    REAL_DRAIN_WAIT_US=3000000,REAL_SETUP_WAIT_US=10000000 };
typedef struct {
    X4T26RunOps ops;
    X4T26ModeResult measured;
    X4T26ModeResult *result;
    X4StreamPipeline *pipeline;
    X4T26Platform *platform;
    X4T26Matcher matcher;
    X4T26Samples decode,decode_to_flip,submit_to_observe;
    X4T26CpuSnapshot cpu_begin,cpu_end;
    uint64_t measure_begin,measure_end,first_output,last_output;
    atomic_uint_fast64_t arrival[X4_T26_SOURCE_AUS];
    atomic_uint_fast64_t output_id[X4_T26_METRIC_CAPACITY];
    atomic_uint_fast64_t source_ordinal[X4_T26_METRIC_CAPACITY];
    atomic_uint measured_outputs,observed_calls;
    atomic_int error;
    char baseline[3][65];
    bool baseline_present,snapshot_taken,cpu_started,cpu_finished;
    unsigned close_observations;
} RealScratch;
static int interrupted(const X4T26RunOps *ops)
{
    if(atomic_load_explicit(ops->cancel,memory_order_acquire))return REAL_CANCELLED;
    return ops->budget_expired&&ops->budget_expired(ops->context)?REAL_BUDGET:0;
}
static void remember_error(RealScratch *s,int rc)
{
    if(!rc)return;
    int empty=0;(void)atomic_compare_exchange_strong_explicit(&s->error,&empty,rc,
        memory_order_release,memory_order_relaxed);
}
static int pace(RealScratch *s,uint64_t target)
{
    for(;;){int rc=interrupted(&s->ops);if(rc)return rc;
        rc=atomic_load_explicit(&s->error,memory_order_acquire);if(rc)return rc;
        uint64_t now=s->ops.now_us(s->ops.context);if(now>=target)return 0;
        uint64_t left=target-now;s->ops.sleep_us(s->ops.context,(unsigned)(left>5000?5000:left));}
}
static X4T26Output common_output(const X4ProbeNativeOutput *n,int rc)
{
    X4T26Output o={.native_rc=n->t26_native_called?n->native_result:X4_T26_PLATFORM_NOT_RUN,
        .local_result=n->control_result,.native_called=n->t26_native_called,
        .valid=n->t26_native_valid!=0,.error=n->t26_native_error!=0,
        .layout_verified=n->layout_verified,.picture_count_native=true,
        .native_valid=n->t26_native_valid,.native_error=n->t26_native_error,
        .picture_count=n->t26_picture_count,.native_pointer=n->t26_native_pointer,
        .native_bytes=n->t26_native_bytes,.pointer=n->pixels,.slot=n->slot,
        .width=n->native_width,.height=n->native_height,.pitch=n->pitch,
        .owned_bytes=n->owned_bytes,.returned_bytes=n->returned_bytes,.layout=n->layout,
        .decode_end_us=n->decode_end_us,.decode_us=n->decode_us,
        .decode_call=n->native_call,.native_call=n->native_call};
    if(rc&&(!o.native_called||rc!=n->native_result))o.local_result=rc;
    if(n->decode_end_us>=n->decode_us)o.decode_begin_us=n->decode_end_us-n->decode_us;
    else o.local_result=REAL_CONTRACT;
    return o;
}
static void observe_decode(void *context,uint64_t ordinal,int rc,
    const X4ProbeNativeOutput *native,const X4StreamVideoInfo *info)
{
    RealScratch *s=context;X4T26ModeResult *r=s->result;
    r->backend.attempted=true;r->backend.v2=*info;r->backend.sps=info->sps;
    r->backend.output_count=info->output_count;r->backend.opened=info->create_result==0;
    r->backend.open_result=info->create_result;
    X4T26Output out=common_output(native,rc);
    if(!out.native_called){remember_error(s,out.local_result?out.local_result:REAL_CONTRACT);return;}
    ++r->native_calls;++r->decode_calls;r->native_result=out.native_rc;
    if(!out.native_valid)++r->no_output_calls;
    if(out.native_error)++r->error_outputs;
    if(!ordinal||ordinal>X4_T26_SOURCE_AUS){remember_error(s,REAL_CONTRACT);return;}
    uint64_t arrival=atomic_load_explicit(&s->arrival[ordinal-1],memory_order_acquire);
    X4T26CallRecord call={.call_index=r->decode_calls,.au_ordinal=ordinal,
        .au_begin_us=arrival,.output_ready_us=out.decode_end_us,.has_au=true,
        .output_valid=out.native_valid!=0,.is_error_frame=out.native_error!=0,
        .native_rc=out.native_rc,.picture_count=out.picture_count};
    X4T26Association association;
    int matched=x4_t26_matcher_call(&s->matcher,&call,&association);
    if(ordinal>X4_T26_WARMUP_AUS&&ordinal<=X4_T26_WARMUP_AUS+X4_T26_MEASURED_IMAGES){
        ++r->measured_calls;if(x4_t26_samples_add(&s->decode,out.decode_us))remember_error(s,REAL_CONTRACT);}
    if(matched||out.native_rc||out.native_error||out.local_result||
        (out.valid&&(!native->valid||!out.layout_verified))){
        if(out.local_result)x4_t26_matcher_invalidate(&s->matcher,X4_T26_MATCH_LOCAL_CONTRACT);
        remember_error(s,out.local_result?out.local_result:out.native_rc?out.native_rc:REAL_CONTRACT);
    }
    if(out.valid&&association.inferred){
        unsigned map=(unsigned)(native->output_id%X4_T26_METRIC_CAPACITY);
        atomic_store_explicit(&s->output_id[map],native->output_id,memory_order_relaxed);
        atomic_store_explicit(&s->source_ordinal[map],association.au_ordinal,memory_order_release);
        if(association.au_ordinal>180&&association.au_ordinal<=780){
            uint64_t measured=association.au_ordinal-180;
            ++r->measured_outputs;
            if(!s->first_output)s->first_output=out.decode_end_us;
            s->last_output=out.decode_end_us;
            int h=measured==1?0:measured==300?1:measured==600?2:-1;
            if(h>=0&&!atomic_load_explicit(&s->error,memory_order_acquire)){
                uint64_t began=s->ops.now_us(s->ops.context);
                X4T26HashResult *hash=&r->hashes[h];hash->measured_ordinal=measured;
                int hashed=x4_t26_result_hash_nv12(&out,hash->sha256);
                hash->observed=hashed==0;
                if(!hashed&&s->baseline_present){hash->compared=true;
                    hash->matches_base=!strcmp(s->baseline[h],hash->sha256);
                    if(!hash->matches_base)hashed=REAL_CONTRACT;}
                remember_error(s,hashed);
                r->hash_observer_us+=s->ops.now_us(s->ops.context)-began;
            }
            if(measured==600&&!s->cpu_finished){
                s->measure_end=s->ops.now_us(s->ops.context);
                x4_t26_platform_cpu(s->platform,&s->cpu_end);s->cpu_finished=true;
            }
        }
    }
    /* CPU counters are read by the real Decode owner after warmup, never by
     * the producer. Measured wall includes source pacing and the three hashes. */
    if(ordinal==180&&!s->cpu_started){
        x4_t26_platform_cpu(s->platform,&s->cpu_begin);
        s->measure_begin=s->ops.now_us(s->ops.context);s->cpu_started=true;
    }
    atomic_store_explicit(&s->observed_calls,(unsigned)r->decode_calls,memory_order_release);
    atomic_store_explicit(&s->measured_outputs,(unsigned)r->measured_outputs,memory_order_release);
}
static void observe_flip(void *context,uint64_t output_id,uint64_t decoded,
    uint64_t submitted,uint64_t observed)
{
    RealScratch *s=context;unsigned map=(unsigned)(output_id%X4_T26_METRIC_CAPACITY);
    uint64_t ordinal=atomic_load_explicit(&s->source_ordinal[map],memory_order_acquire);
    if(atomic_load_explicit(&s->output_id[map],memory_order_relaxed)!=output_id){
        remember_error(s,REAL_CONTRACT);return;}
    if(ordinal<=180||ordinal>780)return;
    if(observed<decoded||observed<submitted||
        x4_t26_samples_add(&s->decode_to_flip,observed-decoded)||
        x4_t26_samples_add(&s->submit_to_observe,observed-submitted))remember_error(s,REAL_CONTRACT);
}
static uint64_t allocation_bytes(const X4T26Allocation *a,unsigned n)
{
    uint64_t total=0;if(n>X4_T26_ALLOCATION_MAX)return UINT64_MAX;
    for(unsigned i=0;i<n;++i){if(a[i].bytes>UINT64_MAX-total)return UINT64_MAX;total+=a[i].bytes;}
    return total;
}
static void before_close(void *context,const X4LiveVideo *video,const X4StreamVideoInfo *info)
{
    RealScratch *s=context;X4T26ModeResult *r=s->result;X4T26BackendInfo *b=&r->backend;
    if(s->snapshot_taken){remember_error(s,REAL_CONTRACT);return;}
    s->snapshot_taken=true;++s->close_observations;b->attempted=true;b->v2=*info;b->sps=info->sps;
    b->opened=info->create_result==0;b->open_result=info->create_result;
    b->output_count=info->output_count;r->held_before_close=x4_live_video_stream_output_held_mask(video);
    X4T26Allocation a[X4_T26_ALLOCATION_MAX];unsigned n=x4_live_video_t26_allocations(video,a);
    b->allocations_before=n;b->owned_bytes_before=allocation_bytes(a,n);
    b->largest_before_rc=x4_t26_platform_largest(s->platform,&b->largest_before_bytes,&b->largest_before_called);
    if(n>X4_T26_ALLOCATION_MAX){remember_error(s,REAL_CONTRACT);return;}
    for(unsigned i=0;i<n;++i){X4T26AllocationCheck *c=&b->allocation_checks[i];
        c->allocation=a[i];c->after_rc=X4_T26_PLATFORM_NOT_RUN;
        c->before_rc=x4_t26_platform_query(s->platform,a[i].physical_offset,&c->before,&c->before_called);
        c->before_valid=c->before_called&&!c->before_rc&&x4_t26_query_contains(&c->before,&a[i]);}
}
static void after_close(void *context,const X4LiveVideo *video,const X4StreamVideoInfo *info,int rc)
{
    RealScratch *s=context;X4T26ModeResult *r=s->result;X4T26BackendInfo *b=&r->backend;
    b->v2=*info;r->held_after_close=x4_live_video_stream_output_held_mask(video);
    X4T26Allocation a[X4_T26_ALLOCATION_MAX];unsigned n=x4_live_video_t26_allocations(video,a);
    b->allocations_after=n;b->owned_bytes_after=allocation_bytes(a,n);
    b->owned_clean=!rc&&!n&&!video->state&&!r->held_after_close;
    b->closed=b->owned_clean;b->retained=!b->owned_clean;
    b->close_result=rc?rc:b->owned_clean?0:REAL_CONTRACT;
    unsigned checks=b->allocations_before;if(checks>X4_T26_ALLOCATION_MAX)checks=X4_T26_ALLOCATION_MAX;
    for(unsigned i=0;i<checks;++i){X4T26AllocationCheck *c=&b->allocation_checks[i];
        c->after_rc=x4_t26_platform_query(s->platform,c->allocation.physical_offset,&c->after,&c->after_called);
        c->after_valid=c->after_called&&!c->after_rc&&x4_t26_query_contains(&c->after,&c->allocation);
        c->release_evidence=b->owned_clean&&c->after_called&&x4_t26_query_release_evidence(c->before_valid,c->after_rc);}
    b->largest_after_rc=x4_t26_platform_largest(s->platform,&b->largest_after_bytes,&b->largest_after_called);
    /* Querying former physical offsets is diagnostic; no pixel read follows Delete. */
}
static unsigned failure_status(const X4T26ModeResult *r,int rc)
{
    if(rc==REAL_CANCELLED)return X4_T26_RESULT_CANCELLED;
    if(rc==REAL_BUDGET)return X4_T26_RESULT_BUDGET;
    const X4StreamVideoInfo *v=&r->backend.v2;
    if(v->query_result!=X4_PROBE_STACK_NOT_RUN&&v->query_result)return X4_T26_RESULT_QUERY_REJECTED;
    if(v->create_result!=X4_PROBE_STACK_NOT_RUN&&v->create_result)return X4_T26_RESULT_CREATE_REJECTED;
    return r->native_calls&&(r->native_result!=0||r->error_outputs)?
        X4_T26_RESULT_NATIVE_ERROR:X4_T26_RESULT_LOCAL_ERROR;
}
static bool baseline_valid(const char baseline[3][65])
{
    if(!baseline)return false;
    for(unsigned h=0;h<3;++h){
        if(baseline[h][64])return false;
        for(unsigned i=0;i<64;++i)if(!((baseline[h][i]>='0'&&baseline[h][i]<='9')||
            (baseline[h][i]>='a'&&baseline[h][i]<='f')))return false;
    }
    return true;
}
int x4_t26_run_realistic(const X4T26Config *config,const X4T26GeneratedSample *sample,
    const X4T26ClipData *clip,const char baseline[3][65],const X4T26RunOps *ops,
    X4T26ModeResult *result_out,X4StreamPipeline **retained)
{
    X4T26RealisticOptions selected={.source_hz=60,.vsync_policy=1,.mailbox=false};
#if X4_PROBE_T27
    struct X4PipelinePresentationSummary *presentation=ops?ops->presentation_summary:NULL;
    if(ops&&ops->presentation_source_hz){selected.source_hz=ops->presentation_source_hz;
        selected.vsync_policy=ops->presentation_vsync_policy;selected.mailbox=ops->presentation_mailbox;}
#endif
    const X4T26RealisticOptions *options=&selected;
    if((options->source_hz!=30&&options->source_hz!=60)||
        (options->vsync_policy!=1&&options->vsync_policy!=2)||
        (options->mailbox&&options->vsync_policy!=1))return REAL_CONTRACT;

    if(!config||config->api!=X4_T26_API_V2||!config->realistic||!x4_t26_config_valid(config)||
        !sample||!clip||!clip->verified||!baseline_valid(baseline)||!ops||!ops->now_us||!ops->sleep_us||!ops->before||
        !ops->cancel||!ops->display_open||!ops->display_close||!result_out||!retained||*retained)return REAL_CONTRACT;
    memset(result_out,0,sizeof(*result_out));result_out->present=true;result_out->mode=X4_T26_MODE_PACED;
    result_out->native_result=X4_T26_PLATFORM_NOT_RUN;result_out->backend.config=*config;
    result_out->backend.open_result=result_out->backend.close_result=X4_T26_PLATFORM_NOT_RUN;
    result_out->backend.v2.query_result=result_out->backend.v2.create_result=X4_PROBE_STACK_NOT_RUN;
    uint64_t began=ops->now_us(ops->context);RealScratch *s=calloc(1,sizeof(*s));
    if(!s){result_out->local_result=REAL_MEMORY;result_out->status=X4_T26_RESULT_LOCAL_ERROR;return REAL_MEMORY;}
    s->measured=*result_out;X4T26ModeResult *r=&s->measured;
    s->ops=*ops;s->result=r;atomic_init(&s->error,0);atomic_init(&s->measured_outputs,0);atomic_init(&s->observed_calls,0);
    for(unsigned i=0;i<X4_T26_SOURCE_AUS;++i)atomic_init(&s->arrival[i],0);
    for(unsigned i=0;i<X4_T26_METRIC_CAPACITY;++i){atomic_init(&s->output_id[i],0);atomic_init(&s->source_ordinal[i],0);}
    if(baseline){s->baseline_present=true;memcpy(s->baseline,baseline,sizeof(s->baseline));}
    int rc=interrupted(ops);
    if(!rc)rc=x4_t26_matcher_reset(&s->matcher,true,181,780);
    if(!rc)rc=x4_t26_platform_create(&s->platform,ops->before,ops->context);
    X4T26PipelineHooks hooks={.native={.config=*config,.before=ops->before,.context=ops->context},
        .on_decode=observe_decode,.on_flip=observe_flip,.before_close=before_close,.after_close=after_close,.context=s};
    X4StreamPipeline *p=NULL;bool display_handoff=false,started=false;
    if(!rc){p=x4_stream_pipeline_t26_create(began?began:1,ops->cancel,&hooks);if(!p)rc=REAL_MEMORY;s->pipeline=p;}
#if X4_PROBE_T27
    if(presentation)memset(presentation,0,sizeof(*presentation));
    if(!rc)rc=x4_stream_pipeline_t26_set_present_policy(p,options->vsync_policy,options->mailbox);
    if(!rc)rc=x4_stream_pipeline_t26_set_measure_window(p,181,780);
#endif
    if(!rc){display_handoff=true;rc=ops->display_open(ops->context,p);}
    if(!rc){rc=ops->before(ops->context,"warmup");if(!rc)rc=x4_stream_pipeline_start(p);started=rc==0;}
    uint64_t source_start=ops->now_us(ops->context);
    for(unsigned ordinal=1;!rc&&ordinal<=X4_T26_SOURCE_AUS;++ordinal){
        rc=pace(s,source_start+(uint64_t)(ordinal-1)*1000000/options->source_hz);if(rc)break;
        size_t bytes=0;uint64_t pts=0;const uint8_t *au=x4_t26_clip_au(clip,ordinal-1,&bytes,&pts);
        if(!au){rc=REAL_CONTRACT;break;}
        uint64_t arrival=ops->now_us(ops->context);
        atomic_store_explicit(&s->arrival[ordinal-1],arrival,memory_order_release);
        x4_stream_pipeline_au_received(p,ordinal==1,arrival);
        int pushed=x4_stream_pipeline_submit_parameters(p,au,bytes,pts,ordinal,
            sample->sps,sample->sps_size,sample->pps,sample->pps_size);
        if(pushed)rc=pushed<0?pushed:REAL_CONTRACT; /* A dropped AU invalidates ordered replay. */
        if(!rc&&ordinal==1){
            /* Query/Create are setup, not a steady-state queue-overload test.
             * This is the sole producer wait for Decode; subsequent AUs never
             * wait for their own Decode, conversion or presentation. */
            uint64_t setup_deadline=ops->now_us(ops->context)+REAL_SETUP_WAIT_US;
            while(!rc&&!atomic_load_explicit(&s->observed_calls,memory_order_acquire)){
                rc=pace(s,ops->now_us(ops->context)+1000);if(rc)break;
                X4StreamPipelineSnapshot snapshot;x4_stream_pipeline_snapshot(p,&snapshot);
                if(snapshot.error){rc=snapshot.error;break;}
                if(ops->now_us(ops->context)>=setup_deadline){rc=REAL_CONTRACT;break;}
            }
            source_start=ops->now_us(ops->context);
        }
    }
    uint64_t until=ops->now_us(ops->context)+REAL_DRAIN_WAIT_US;
    while(!rc&&atomic_load_explicit(&s->measured_outputs,memory_order_acquire)<600){
        rc=pace(s,ops->now_us(ops->context)+1000);if(rc)break;
        X4StreamPipelineSnapshot snapshot;x4_stream_pipeline_snapshot(p,&snapshot);
        if(snapshot.error){rc=snapshot.error;break;}
        if(ops->now_us(ops->context)>=until){rc=REAL_CONTRACT;break;}
    }
    if(!rc)rc=atomic_load_explicit(&s->error,memory_order_acquire);
    /* Give the independent display owner one bounded opportunity to observe
     * its last in-flight frame before shutdown; this never serializes Decode. */
    if(!rc){int wait=pace(s,ops->now_us(ops->context)+50000);if(wait)rc=wait;}
    if(p){
        x4_stream_pipeline_request_stop(p);
        int closed=display_handoff?ops->display_close(ops->context,p):x4_stream_pipeline_close(p);
        X4StreamPipelineSnapshot final;x4_stream_pipeline_snapshot(p,&final);
        bool cold=x4_stream_pipeline_t26_observers_stopped(p);
        if(cold){
            bool observed=x4_stream_pipeline_t26_gpu_status(p,&r->gpu_requested,
                &r->gpu_frames,&r->cpu_frames,r->gpu_reason);
            if(!rc&&(!observed||!r->gpu_requested||!r->gpu_frames))rc=REAL_CONTRACT;
        }
#if X4_PROBE_T27
        if(cold&&presentation&&!x4_stream_pipeline_t26_presentation_summary(p,presentation)&&!rc)
            rc=REAL_CONTRACT;
#endif
        if(closed||final.retained||!final.display_detached||!cold){if(!rc)rc=closed?closed:REAL_CONTRACT;*retained=p;}
        else {int freed=x4_stream_pipeline_free(p);if(freed){if(!rc)rc=freed;*retained=p;}}
        if(*retained&&!cold){
            /* A failed join does not authorize racing an observer. Its result
             * and scratch remain private/reachable through the retained hook;
             * publish only this cold failure envelope to the bank owner. */
            result_out->elapsed_us=ops->now_us(ops->context)-began;
            result_out->completed=true;result_out->accepted=false;
            result_out->local_result=rc?rc:REAL_CONTRACT;
            result_out->status=X4_T26_RESULT_LOCAL_ERROR;
            result_out->backend.retained=true;result_out->backend.owned_clean=false;
            return result_out->local_result;
        }
    }
    if(!rc)rc=atomic_load_explicit(&s->error,memory_order_acquire);
    if(started&&(!s->snapshot_taken||s->close_observations!=1)&&!rc)rc=REAL_CONTRACT;
    r->elapsed_us=ops->now_us(ops->context)-began;
    if(s->cpu_started&&s->cpu_finished&&s->measure_end>=s->measure_begin)r->measured_wall_us=s->measure_end-s->measure_begin;
    if(s->last_output>=s->first_output&&s->first_output)r->output_span_us=s->last_output-s->first_output;
    x4_t26_cpu_delta(&s->cpu_begin,&s->cpu_end,r->measured_wall_us,&r->cpu);
    x4_t26_platform_info(s->platform,&r->backend.platform);
    r->inference_valid=s->matcher.inference_valid;r->invalid_reasons=s->matcher.invalid_reasons;
    memcpy(r->call_delay_hist,s->matcher.call_delay_hist,sizeof(r->call_delay_hist));
    if(x4_t26_samples_stats(&s->decode,&r->decode)||x4_t26_samples_stats(&s->matcher.au_latency,&r->au_latency)||
        x4_t26_samples_stats(&s->matcher.output_interval,&r->output_interval)||
        x4_t26_samples_stats(&s->decode_to_flip,&r->decode_to_flip)||
        x4_t26_samples_stats(&s->submit_to_observe,&r->submit_to_observe))if(!rc)rc=REAL_MEMORY;
    /* No Flush/no global matcher_finish: depth2 intentionally has one pending
     * source picture781. The measured181..780 association must still be exact. */
    if(!rc&&(r->measured_calls!=600||r->measured_outputs!=600||r->au_latency.n!=600||
        r->output_interval.n!=599||!r->inference_valid||!r->backend.owned_clean))rc=REAL_CONTRACT;
    r->completed=rc!=REAL_CANCELLED&&rc!=REAL_BUDGET;r->accepted=rc==0;
    r->status=rc?failure_status(r,rc):X4_T26_RESULT_PASS;
    r->local_result=r->status==X4_T26_RESULT_NATIVE_ERROR||r->status==X4_T26_RESULT_QUERY_REJECTED||
        r->status==X4_T26_RESULT_CREATE_REJECTED?0:rc;
    *result_out=*r;
    if(!*retained){x4_t26_platform_free(&s->platform);free(s);}
    return rc;
}
#if X4_PROBE_T27
int x4_t26_run_realistic_options(const X4T26Config *config,const X4T26GeneratedSample *sample,
    const X4T26ClipData *clip,const char baseline[3][65],const X4T26RunOps *ops,
    const X4T26RealisticOptions *options,struct X4PipelinePresentationSummary *presentation,
    X4T26ModeResult *result,X4StreamPipeline **retained)
{
    if(!ops||!options||!presentation)return REAL_CONTRACT;
    X4T26RunOps configured=*ops;
    configured.presentation_source_hz=options->source_hz;
    configured.presentation_vsync_policy=options->vsync_policy;
    configured.presentation_mailbox=options->mailbox;
    configured.presentation_summary=presentation;
    return x4_t26_run_realistic(config,sample,clip,baseline,&configured,result,retained);
}
#endif
#endif
