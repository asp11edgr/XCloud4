/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_runner.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_state.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { RUN_CONTRACT=-7660, RUN_MEMORY=-7661, RUN_CANCELLED=3, RUN_BUDGET=4 };
typedef struct {
    X4T26Matcher matcher;
    X4T26Samples decode;
    X4T26Output saved[3];
    bool saved_valid[3];
    X4T26CpuSnapshot cpu_begin,cpu_end;
    uint64_t measure_begin,measure_end,first_output,last_output;
    uint64_t submitted,returned;
    uint32_t hash_reserved;
} RunScratch;
static int interrupted(const X4T26RunOps *ops)
{
    if(atomic_load_explicit(ops->cancel,memory_order_acquire))return RUN_CANCELLED;
    return ops->budget_expired&&ops->budget_expired(ops->context)?RUN_BUDGET:0;
}
static int pace(const X4T26RunOps *ops,uint64_t target)
{
    for(;;){int rc=interrupted(ops);if(rc)return rc;
        uint64_t now=ops->now_us(ops->context);if(now>=target)return 0;
        uint64_t left=target-now;ops->sleep_us(ops->context,(unsigned)(left>5000?5000:left));}
}
static unsigned free_slot(const X4T26Config *config,X4T26Backend *backend,uint32_t reserved)
{
    uint32_t used=x4_t26_backend_held_mask(backend)|reserved;
    unsigned count=config->api==X4_T26_API_V1?5:config->depth+3;
    for(unsigned i=0;i<count;++i)if(!(used&(UINT32_C(1)<<i)))return i;
    return UINT_MAX;
}
static int observe(RunScratch *s,X4T26ModeResult *r,const X4T26Output *out,
    uint64_t ordinal,bool flush,uint64_t au_begin)
{
    if(!out->native_called){x4_t26_matcher_invalidate(&s->matcher,X4_T26_MATCH_LOCAL_CONTRACT);
        return out->local_result?out->local_result:RUN_CONTRACT;}
    ++r->native_calls;if(!flush)++r->decode_calls;else ++r->flush_calls;
    r->native_result=out->native_rc;
    if(!flush&&!out->native_valid)++r->no_output_calls;
    if(out->native_error)++r->error_outputs;
    if(!flush&&ordinal>X4_T26_WARMUP_AUS&&ordinal<=X4_T26_WARMUP_AUS+X4_T26_MEASURED_IMAGES){
        ++r->measured_calls;if(x4_t26_samples_add(&s->decode,out->decode_us))return RUN_CONTRACT;}
    X4T26CallRecord call={.call_index=r->decode_calls,.au_ordinal=ordinal,
        .au_begin_us=au_begin,.output_ready_us=out->decode_end_us,.has_au=!flush,
        .output_valid=out->native_valid!=0,.is_error_frame=out->native_error!=0,
        .flushing=flush,.native_rc=out->native_rc,.picture_count=out->picture_count};
    X4T26Association association;
    int matched=x4_t26_matcher_call(&s->matcher,&call,&association);
    if(out->native_rc||out->native_error)return out->native_rc?out->native_rc:RUN_CONTRACT;
    if(out->local_result||(!out->layout_verified&&out->valid)){
        x4_t26_matcher_invalidate(&s->matcher,X4_T26_MATCH_LOCAL_CONTRACT);
        return out->local_result?out->local_result:RUN_CONTRACT;}
    if(matched||!s->matcher.inference_valid)return RUN_CONTRACT;
    if(out->valid){
        ++s->returned;
        if(association.au_ordinal>X4_T26_WARMUP_AUS&&
            association.au_ordinal<=X4_T26_WARMUP_AUS+X4_T26_MEASURED_IMAGES){
            uint64_t measured=association.au_ordinal-X4_T26_WARMUP_AUS;
            ++r->measured_outputs;
            if(!s->first_output)s->first_output=out->decode_end_us;
            s->last_output=out->decode_end_us;
            int h=measured==1?0:measured==300?1:measured==600?2:-1;
            if(h>=0){
                if(out->slot>=5||s->saved_valid[h]||(s->hash_reserved&(UINT32_C(1)<<out->slot)))return RUN_CONTRACT;
                s->saved[h]=*out;s->saved_valid[h]=true;s->hash_reserved|=UINT32_C(1)<<out->slot;
                r->hashes[h].measured_ordinal=measured;
            }
        }
    }
    return 0;
}
static unsigned failure_status(const X4T26ModeResult *r,int rc)
{
    if(rc==RUN_CANCELLED)return X4_T26_RESULT_CANCELLED;
    if(rc==RUN_BUDGET)return X4_T26_RESULT_BUDGET;
    const X4T26BackendInfo *b=&r->backend;
    if(b->config.api==X4_T26_API_V2){
        if(b->v2.query_result!=X4_PROBE_STACK_NOT_RUN&&b->v2.query_result)return X4_T26_RESULT_QUERY_REJECTED;
        if(b->v2.create_result!=X4_PROBE_STACK_NOT_RUN&&b->v2.create_result)return X4_T26_RESULT_CREATE_REJECTED;
    }else{
        for(unsigned i=0;i<2;++i)if((b->v1.query_called_mask&(1u<<i))&&b->v1.query_rc[i])return X4_T26_RESULT_QUERY_REJECTED;
        for(unsigned i=0;i<2;++i)if((b->v1.create_called_mask&(1u<<i))&&b->v1.create_rc[i])return X4_T26_RESULT_CREATE_REJECTED;
    }
    return r->native_calls&&(r->native_result!=0||r->error_outputs)?X4_T26_RESULT_NATIVE_ERROR:X4_T26_RESULT_LOCAL_ERROR;
}
int x4_t26_run_raw(const X4T26Config *config,unsigned mode,const X4T26GeneratedSample *sample,
    const X4T26ClipData *clip,const char baseline[3][65],const X4T26RunOps *ops,
    X4T26ModeResult *r,X4T26Backend **retained)
{
    if(!config||config->realistic||!sample||!clip||!clip->verified||!ops||!ops->now_us||!ops->sleep_us||
        !ops->before||!ops->cancel||!r||!retained||*retained||mode>=X4_T26_MODE_COUNT)return RUN_CONTRACT;
    memset(r,0,sizeof(*r));r->present=true;r->mode=mode;r->native_result=X4_T26_PLATFORM_NOT_RUN;r->backend.config=*config;
    uint64_t began=ops->now_us(ops->context);RunScratch *s=calloc(1,sizeof(*s));
    if(!s){r->local_result=RUN_MEMORY;r->status=X4_T26_RESULT_LOCAL_ERROR;return RUN_MEMORY;}
    (void)x4_t26_matcher_reset(&s->matcher,true,181,780);
    X4T26Backend *b=NULL;
    X4T26BackendOptions options={.config=*config,.sps=sample->sps,.sps_bytes=sample->sps_size,
        .cancel=ops->cancel,.before=ops->before,.context=ops->context,.input_arena_bytes=clip->v1_input_arena_bytes};
    int rc=x4_t26_backend_create(&b,&options);
    if(!rc)rc=x4_t26_backend_open(b);
    if(rc)goto close;
    rc=ops->before(ops->context,"warmup");if(rc)goto close;
    uint64_t source_start=ops->now_us(ops->context);
    for(unsigned ordinal=1;ordinal<=X4_T26_SOURCE_AUS;++ordinal){
        rc=interrupted(ops);if(rc)break;
        if(ordinal<=X4_T26_WARMUP_AUS+1||mode==X4_T26_MODE_PACED){
            uint64_t target=source_start+(uint64_t)(ordinal-1)*1000000/60;
            rc=pace(ops,target);if(rc)break;
        }
        if(ordinal==181){
            rc=ops->before(ops->context,"Decode");if(rc)break;
            x4_t26_backend_cpu(b,&s->cpu_begin);s->measure_begin=ops->now_us(ops->context);
            /* Maximum mode begins immediately after the same180 source AUs;
             * paced mode keeps the60Hz source timebase. */
        }
        unsigned offered=free_slot(config,b,s->hash_reserved);if(offered==UINT_MAX){rc=RUN_CONTRACT;break;}
        size_t bytes=0;uint64_t pts=0;const uint8_t *au=x4_t26_clip_au(clip,ordinal-1,&bytes,&pts);
        if(!au){rc=RUN_CONTRACT;break;}
        uint64_t arrival=ops->now_us(ops->context);X4T26Output out;
        int decoded=x4_t26_backend_decode(b,offered,au,bytes,pts,&out);
        int observed=observe(s,r,&out,ordinal,false,arrival);++s->submitted;
        if(decoded||observed){rc=decoded?decoded:observed;break;}
        if(r->measured_outputs==600)break;
    }
    /* An API with more than one call of delay can return the final measured
     * image only at EOF. Its measured latency is a Flush observation, never
     * an invented N+1 Decode call. Keep the hash leases during that drain. */
    bool flush_marked=false;
    while(!rc&&r->measured_outputs<600&&r->flush_calls<8){
        rc=interrupted(ops);if(rc)break;
        if(!flush_marked){rc=ops->before(ops->context,"Flush");flush_marked=true;if(rc)break;}
        unsigned offered=free_slot(config,b,s->hash_reserved);if(offered==UINT_MAX){rc=RUN_CONTRACT;break;}
        X4T26Output out;int flushed=x4_t26_backend_flush(b,offered,&out);
        int observed=observe(s,r,&out,0,true,0);
        if(flushed||observed){rc=flushed?flushed:observed;break;}
        if(!out.valid)break;
    }
    s->measure_end=ops->now_us(ops->context);x4_t26_backend_cpu(b,&s->cpu_end);
    if(!rc&&r->measured_outputs!=600)rc=RUN_CONTRACT;
    /* All pixel reads are outside Decode/CPU/wall measurement. Saved slots
     * were excluded from every offer, even after native ownership retired. */
    if(!rc){
        uint64_t hash_begin=ops->now_us(ops->context);
        for(unsigned i=0;i<3&&!rc;++i){
            if(!s->saved_valid[i]){rc=RUN_CONTRACT;break;}
            rc=x4_t26_result_hash_nv12(&s->saved[i],r->hashes[i].sha256);
            r->hashes[i].observed=rc==0;
            if(!rc&&baseline&&baseline[i][0]){
                r->hashes[i].compared=true;r->hashes[i].matches_base=!strcmp(baseline[i],r->hashes[i].sha256);
                if(!r->hashes[i].matches_base)rc=RUN_CONTRACT;
            }
        }
        r->hash_observer_us=ops->now_us(ops->context)-hash_begin;
    }
    /* Revoke only our hash leases BEFORE Flush; decoder-held slots remain
     * excluded until their pointer returns or Delete succeeds. */
    s->hash_reserved=0;
    if(!rc){if(!flush_marked)rc=ops->before(ops->context,"Flush");
        while(!rc&&r->flush_calls<8){
            rc=interrupted(ops);if(rc)break;
            unsigned offered=free_slot(config,b,0);if(offered==UINT_MAX){rc=RUN_CONTRACT;break;}
            X4T26Output out;int flushed=x4_t26_backend_flush(b,offered,&out);
            int observed=observe(s,r,&out,0,true,0);
            if(flushed||observed){rc=flushed?flushed:observed;break;}
            if(!out.valid)break;
        }
        if(!rc&&!x4_t26_matcher_finish(&s->matcher,s->submitted,s->submitted))rc=RUN_CONTRACT;
    }
close:
    if(b){r->held_before_close=x4_t26_backend_held_mask(b);int closed=x4_t26_backend_close(b);
        x4_t26_backend_info(b,&r->backend);r->held_after_close=x4_t26_backend_held_mask(b);
        if(closed||!r->backend.owned_clean){if(!rc)rc=closed?closed:RUN_CONTRACT;*retained=b;}
        else {int destroyed=x4_t26_backend_destroy(&b);if(destroyed){if(!rc)rc=destroyed;*retained=b;}}}
    r->elapsed_us=ops->now_us(ops->context)-began;
    if(s->measure_begin&&s->measure_end>=s->measure_begin)r->measured_wall_us=s->measure_end-s->measure_begin;
    if(s->last_output>=s->first_output&&s->first_output)r->output_span_us=s->last_output-s->first_output;
    x4_t26_cpu_delta(&s->cpu_begin,&s->cpu_end,r->measured_wall_us,&r->cpu);
    r->inference_valid=s->matcher.inference_valid;r->invalid_reasons=s->matcher.invalid_reasons;
    r->drained_outputs=s->matcher.drained_outputs;r->measured_flush_outputs=s->matcher.measured_flush_outputs;
    memcpy(r->call_delay_hist,s->matcher.call_delay_hist,sizeof(r->call_delay_hist));
    if(x4_t26_samples_stats(&s->decode,&r->decode)||
        x4_t26_samples_stats(&s->matcher.au_latency,&r->au_latency)||
        x4_t26_samples_stats(&s->matcher.output_interval,&r->output_interval))if(!rc)rc=RUN_MEMORY;
    r->completed=rc!=RUN_CANCELLED&&rc!=RUN_BUDGET;
    r->accepted=!rc&&r->measured_calls==600&&r->measured_outputs==600&&r->inference_valid&&r->backend.owned_clean;
    r->status=rc?failure_status(r,rc):X4_T26_RESULT_PASS;
    r->local_result=r->status==X4_T26_RESULT_NATIVE_ERROR||r->status==X4_T26_RESULT_QUERY_REJECTED||
        r->status==X4_T26_RESULT_CREATE_REJECTED?0:rc;
    free(s);return rc;
}
#endif
