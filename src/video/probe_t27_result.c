/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t27_result.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include "../auth/json.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
typedef struct {char *text;size_t used,capacity;int error;} Writer;
static void t27_put(Writer *w,const char *format,...)
{
    if(w->error)return;
    va_list args;va_start(args,format);int n=vsnprintf(w->text+w->used,w->capacity-w->used,format,args);va_end(args);
    if(n<0||(size_t)n>=w->capacity-w->used){w->error=-2;return;}w->used+=(size_t)n;
}
static bool t27_bounded(const char *s,size_t capacity,size_t *n)
{if(!s)return false;for(size_t i=0;i<capacity;++i)if(!s[i]){if(n)*n=i;return true;}return false;}
static bool t27_hash_valid(const char *s)
{
    if(!s||!t27_bounded(s,65,NULL)||strlen(s)!=64)return false;
    for(unsigned i=0;i<64;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
    return true;
}
static bool t27_id_valid(const char *s)
{
    size_t n;if(!t27_bounded(s,65,&n)||!n)return false;
    for(size_t i=0;i<n;++i)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
        (s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||s[i]=='.'))return false;
    return true;
}
static void t27_quoted(Writer *w,const char *s)
{
    t27_put(w,"\"");
    for(const unsigned char *p=(const unsigned char *)s;*p;++p){
        if(*p=='"'||*p=='\\')t27_put(w,"\\%c",*p);
        else if(*p<32)t27_put(w,"\\u%04x",(unsigned)*p);
        else t27_put(w,"%c",*p);
    }
    t27_put(w,"\"");
}
static bool t27_stats_valid(const X4T26Stats *s)
{
    if(!s||s->n>1024||!isfinite(s->mean_us)||s->mean_us<0)return false;
    if(!s->n)return !s->p50_us&&!s->p95_us&&!s->p99_us&&!s->max_us&&s->mean_us==0;
    return s->p50_us<=s->p95_us&&s->p95_us<=s->p99_us&&s->p99_us<=s->max_us&&s->mean_us<=(double)s->max_us;
}
static void t27_stats(Writer *w,const X4T26Stats *s)
{
    if(!s->n){t27_put(w,"null");return;}
    t27_put(w,"{\"n\":%" PRIu64 ",\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64
        ",\"p99_us\":%" PRIu64 ",\"max_us\":%" PRIu64 ",\"mean_us\":%.6f}",
        s->n,s->p50_us,s->p95_us,s->p99_us,s->max_us,s->mean_us);
}
static bool t27_base_cell(unsigned group,unsigned factor)
{return (group==X4_T26_GROUP_FACTORS&&factor==0)||group==X4_T26_GROUP_BASE_EXTRA;}
static bool t27_cell_metadata(const X4T26CellResult *r)
{
 X4T27Cell cell;if(!x4_t27_plan_cell(r->cell,&cell))return false;
 return r->group==(cell.reference?X4_T26_GROUP_BASE_EXTRA:X4_T26_GROUP_REALISTIC)&&r->clip==cell.clip&&r->factor==0u&&x4_t26_config_equal(&r->config,&cell.config);
}
static bool t27_hashes_good(const X4T26ModeResult *m,bool base)
{
    static const uint64_t ordinal[3]={1,300,600};
    for(unsigned i=0;i<3;++i){const X4T26HashResult *h=&m->hashes[i];
        if(!h->observed||h->measured_ordinal!=ordinal[i]||!t27_hash_valid(h->sha256)||
            (h->compared?!h->matches_base:!base))return false;
    }
    return true;
}
static bool t27_mode_pass(const X4T26ModeResult *m,bool base)
{
    return m->present&&m->completed&&m->accepted&&m->status==X4_T26_RESULT_PASS&&
        m->native_result==0&&m->local_result==0&&!m->error_outputs&&m->inference_valid&&!m->invalid_reasons&&
        m->measured_outputs==600&&m->decode.n&&m->au_latency.n==600&&m->output_interval.n==599&&
        m->backend.owned_clean&&m->backend.closed&&!m->backend.retained&&m->backend.close_result==0&&
        !m->held_after_close&&t27_hashes_good(m,base);
}
static bool t27_valid(const X4T26CellResult *r)
{
    if(!r||!t27_id_valid(r->build_id)||!t27_hash_valid(r->manifest_sha256)||r->cell>=X4_T27_CELL_COUNT||
        r->group>X4_T26_GROUP_V1||r->clip>=X4_T26_CLIP_COUNT||
        r->status>X4_T26_RESULT_NOT_RUN||!r->final||!t27_bounded(r->reason,sizeof(r->reason),NULL)||
        !x4_t26_config_valid(&r->config)||!t27_cell_metadata(r))return false;
    for(unsigned i=0;i<2;++i){const X4T26ModeResult *m=&r->modes[i];if(!m->present)continue;
        if(m->mode!=i||m->status>X4_T26_RESULT_NOT_RUN||m->invalid_reasons>127||
            !t27_stats_valid(&m->decode)||!t27_stats_valid(&m->au_latency)||!t27_stats_valid(&m->output_interval)||
            !t27_stats_valid(&m->decode_to_flip)||!t27_stats_valid(&m->submit_to_observe)||
            !t27_bounded(m->gpu_reason,sizeof(m->gpu_reason),NULL)||
            (!r->config.realistic&&(m->gpu_requested||m->gpu_frames||m->cpu_frames||m->gpu_reason[0]))||
            (r->config.realistic&&m->accepted&&(!m->gpu_requested||!m->gpu_frames))||
            m->backend.allocations_before>16||m->backend.allocations_after>16||
            m->backend.platform.memory_observations>16||m->backend.v2.attempt_count>6||
            (m->backend.attempted&&!x4_t26_config_equal(&r->config,&m->backend.config))||
            (m->cpu.process_available&&(!m->cpu.wall_us||!isfinite(m->cpu.process_percent)||m->cpu.process_percent<0||
                fabs(m->cpu.process_percent-100.0*(double)m->cpu.process_us/(double)m->cpu.wall_us)>0.000001))||
            (m->cpu.thread_available&&(!m->cpu.wall_us||!isfinite(m->cpu.thread_percent)||m->cpu.thread_percent<0||
                fabs(m->cpu.thread_percent-100.0*(double)m->cpu.thread_us/(double)m->cpu.wall_us)>0.000001))||
            (!m->native_calls&&m->native_result!=X4_T26_PLATFORM_NOT_RUN))return false;
        for(unsigned j=0;j<3;++j)if(m->hashes[j].observed&&!t27_hash_valid(m->hashes[j].sha256))return false;
        if(m->accepted&&!t27_mode_pass(m,t27_base_cell(r->group,r->factor)))return false;
    }
    if(r->status==X4_T26_RESULT_PASS){
        for(unsigned i=0;i<2;++i)if((i==(r->config.realistic?X4_T26_MODE_PACED:X4_T26_MODE_MAXIMUM))&&
            (!r->modes[i].present||!r->modes[i].completed))return false;
    }
    return true;
}
static void t27_config(Writer *w,const X4T26Config *c)
{
    t27_put(w,"{\"api\":%u,\"resource_type\":%u,\"depth\":%u,\"output_memory_type\":%d,\"affinity_mask\":%" PRIu64
        ",\"priority\":%d,\"pipe_id\":%u,\"queue_id\":%u,\"realistic\":%s}",c->api,c->resource_type,c->depth,
        c->output_memory_type,c->affinity_mask,c->priority,c->pipe_id,c->queue_id,c->realistic?"true":"false");
}
static void t27_backend(Writer *w,const X4T26BackendInfo *b)
{
    t27_put(w,"{\"state\":[%u,%u,%u,%u,%u,%d,%d,%d,%u],\"sps\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],",
        b->attempted,b->opened,b->closed,b->retained,b->owned_clean,b->open_result,b->close_result,b->local_result,b->output_count,
        b->sps.coded_width,b->sps.coded_height,b->sps.visible_width,b->sps.visible_height,b->sps.profile_idc,b->sps.level_idc,
        b->sps.max_num_ref_frames,b->sps.max_dec_frame_buffering,b->sps.pic_order_cnt_type,b->sps.matrix_coefficients,b->sps.full_range);
    t27_put(w,"\"memory\":{\"owned\":[%u,%" PRIu64 ",%u,%" PRIu64 "],\"largest\":[%u,%d,%" PRIu64 ",%u,%d,%" PRIu64 "],\"checks\":[",
        b->allocations_before,b->owned_bytes_before,b->allocations_after,b->owned_bytes_after,b->largest_before_called,
        b->largest_before_rc,b->largest_before_bytes,b->largest_after_called,b->largest_after_rc,b->largest_after_bytes);
    for(unsigned i=0;i<b->allocations_before;++i){const X4T26AllocationCheck *c=&b->allocation_checks[i];
        t27_put(w,"%s[%" PRId64 ",%" PRIu64 ",%d,%u,%d,%u,%u,%d,%u,%u,%" PRId64 ",%" PRId64 ",%d,%" PRId64 ",%" PRId64 ",%d]",
            i?",":"",c->allocation.physical_offset,c->allocation.bytes,c->allocation.memory_type,c->before_called,c->before_rc,
            c->before_valid,c->after_called,c->after_rc,c->after_valid,c->release_evidence,c->before.start,c->before.end,
            c->before.memory_type,c->after.start,c->after.end,c->after.memory_type);
    }
    t27_put(w,"]},\"v2\":{\"config\":[%u,%u,%u,%u,%u,%u,%u,%u,%d,%" PRIu64 "],\"rc\":[%d,%d,%d,%d,%d,%d,%d],",
        b->v2.requested_depth,b->v2.active_depth,b->v2.input_count,b->v2.output_count,b->v2.held_output_mask,b->v2.live_input_count,
        b->v2.retained,b->v2.creation_uncertain,b->v2.decoder_cpu_priority,b->v2.decoder_cpu_affinity,
        b->v2.compute_query_result,b->v2.allocate_queue_result,b->v2.query_result,b->v2.create_result,
        b->v2.delete_decoder_result,b->v2.release_queue_result,b->v2.close_result);
    t27_put(w,"\"bytes\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"attempts\":[",
        b->v2.native_budget_bytes,b->v2.budget_auxiliary_bytes,b->v2.planned_native_bytes,b->v2.queried_compute_bytes,
        b->v2.queried_cpu_bytes,b->v2.queried_gpu_bytes,b->v2.queried_shared_bytes,b->v2.queried_frame_bytes);
    for(unsigned i=0;i<b->v2.attempt_count;++i){const X4StreamDecoderAttempt *a=&b->v2.attempts[i];
        t27_put(w,"%s[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]",
            i?",":"",a->profile,a->level,a->width,a->height,a->dpb,a->depth,a->kind,a->reason,a->failure_reason,a->skipped,
            a->compute_query,a->allocate_queue,a->query,a->create,a->cleanup,a->compute_bytes,a->cpu_bytes,a->gpu_bytes,
            a->shared_bytes,a->frame_bytes,a->planned_bytes);
    }
    const X4T26V1Info *v=&b->v1;
    t27_put(w,"]},\"v1\":{\"rc\":[%u,%d,%u,%d,%u,%u,%d,%d,%u,%d,%d,%u,%d,%d,%d,%d],",
        v->module_called,v->module_rc,v->arbitration_called,v->arbitration_rc,v->arbitration_retry,
        v->query_called_mask,v->query_rc[0],v->query_rc[1],v->create_called_mask,v->create_rc[0],v->create_rc[1],
        v->delete_called,v->delete_rc,v->last_decode_rc,v->last_flush_rc,v->local_result);
    t27_put(w,"\"resource\":[%u,%u,%u,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u],",
        v->config.codecType,v->config.profile,v->config.maxLevel,v->config.maxFrameWidth,v->config.maxFrameHeight,v->config.maxDpbFrameCount,
        v->config.videodecFlags,v->query_memory.cpuMemorySize,v->query_memory.cpuGpuMemorySize,v->query_memory.maxFrameBufferSize,
        v->query_memory.frameBufferAlignment);
    t27_put(w,"\"counts\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%u,%u,%u],\"resolve\":[%u,%d,%d,%d,%d,%d]},",
        v->query_calls,v->create_calls,v->decode_calls,v->flush_calls,v->delete_calls,v->native_calls,v->valid_outputs,v->without_output,
        v->flush_without_output,v->error_outputs,v->input_used_bytes,v->allocated_bytes,v->peak_allocated_bytes,v->allocation_count,
        v->held_mask,v->opened,v->decoder_live,v->retained,v->resolve_called_mask,v->resolve_rc[0],v->resolve_rc[1],v->resolve_rc[2],v->resolve_rc[3],v->resolve_rc[4]);
    const X4T26PlatformInfo *p=&b->platform;
    t27_put(w,"\"platform\":{\"resolve\":[%d,%d,%d,%d,%d,%d],\"modules\":[",p->kernel_handle,p->sysmodule_handle,p->decoder_handle,
        p->usage_resolve_rc,p->query_resolve_rc,p->available_resolve_rc);
    for(unsigned i=0;i<2;++i){const X4T26ModuleObservation *m=&p->modules[i];
        t27_put(w,"%s[%u,%u,%d,%u,%d,%u,%d]",i?",":"",m->module_id,m->before_called,m->before_rc,m->load_called,m->load_rc,m->after_called,m->after_rc);}
    t27_put(w,"],\"memory\":[");
    for(unsigned i=0;i<p->memory_observations;++i){const X4T26MemoryObservation *m=&p->memory[i];
        t27_put(w,"%s[%" PRId64 ",%" PRIu64 ",%d,%d,%d,%d,%d]",i?",":"",m->offset,m->bytes,m->memory_type,m->allocate_rc,m->map_rc,m->unmap_rc,m->release_rc);}
    t27_put(w,"],\"dropped\":%u}}",p->memory_observations_dropped);
}
static void t27_mode(Writer *w,const X4T26ModeResult *m)
{
    if(!m->present){t27_put(w,"null");return;}
    t27_put(w,"{\"mode\":%u,\"completed\":%s,\"accepted\":%s,\"status\":%u,\"native_rc\":%d,\"local_rc\":%d,\"inference\":[%u,%u],",
        m->mode,m->completed?"true":"false",m->accepted?"true":"false",m->status,m->native_result,m->local_result,m->inference_valid,m->invalid_reasons);
    t27_put(w,"\"elapsed_us\":%" PRIu64 ",\"wall_us\":%" PRIu64 ",\"output_span_us\":%" PRIu64 ",\"counts\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],",
        m->elapsed_us,m->measured_wall_us,m->output_span_us,m->measured_calls,m->measured_outputs,m->native_calls,m->decode_calls,
        m->no_output_calls,m->error_outputs,m->flush_calls,m->drained_outputs);
    t27_put(w,"\"output_fps\":");if(m->measured_wall_us)t27_put(w,"%.6f",(double)m->measured_outputs*1000000.0/(double)m->measured_wall_us);else t27_put(w,"null");
    t27_put(w,",\"interval_fps\":");if(m->output_span_us&&m->measured_outputs>1)t27_put(w,"%.6f",(double)(m->measured_outputs-1)*1000000.0/(double)m->output_span_us);else t27_put(w,"null");
    t27_put(w,",\"decode\":");t27_stats(w,&m->decode);t27_put(w,",\"au_to_output\":");t27_stats(w,&m->au_latency);
    t27_put(w,",\"output_interval\":");t27_stats(w,&m->output_interval);t27_put(w,",\"decode_to_flip\":");t27_stats(w,&m->decode_to_flip);
    t27_put(w,",\"submit_to_observe\":");t27_stats(w,&m->submit_to_observe);
    t27_put(w,",\"call_delay\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"measured_flush_outputs\":%" PRIu64 ",\"held\":[%u,%u],\"cpu\":{\"wall_us\":%" PRIu64 ",\"process\":",
        m->call_delay_hist[0],m->call_delay_hist[1],m->call_delay_hist[2],m->call_delay_hist[3],m->measured_flush_outputs,m->held_before_close,m->held_after_close,m->cpu.wall_us);
    if(m->cpu.process_available)t27_put(w,"[%" PRIu64 ",%.6f]",m->cpu.process_us,m->cpu.process_percent);else t27_put(w,"null");
    t27_put(w,",\"thread\":");if(m->cpu.thread_available)t27_put(w,"[%" PRIu64 ",%.6f]",m->cpu.thread_us,m->cpu.thread_percent);else t27_put(w,"null");
    t27_put(w,"},\"hashes\":[");
    for(unsigned i=0;i<3;++i){const X4T26HashResult *h=&m->hashes[i];
        if(i)t27_put(w,",");
        if(!h->observed){t27_put(w,"null");continue;}
        t27_put(w,"{\"image\":%" PRIu64 ",\"sha256\":",h->measured_ordinal);t27_quoted(w,h->sha256);
        t27_put(w,",\"compared\":%s,\"matches_base\":%s}",h->compared?"true":"false",h->matches_base?"true":"false");
    }
    t27_put(w,"],\"hash_observer_us\":%" PRIu64 ",\"realistic_gpu\":{\"requested\":%s,\"gpu_frames\":%" PRIu64 ",\"cpu_frames\":%" PRIu64 ",\"reason\":",
        m->hash_observer_us,m->gpu_requested?"true":"false",m->gpu_frames,m->cpu_frames);
    if(m->gpu_reason[0])t27_quoted(w,m->gpu_reason);else t27_put(w,"null");
    t27_put(w,",\"scope\":\"whole_run_warmup_and_measured\"},\"backend\":");t27_backend(w,&m->backend);t27_put(w,"}");
}
static int t27_encode_into(const X4T26CellResult *r,char *text)
{
    if(!t27_valid(r))return -1;
    Writer w={.text=text,.capacity=X4_T26_RESULT_TEXT_MAX};
    t27_put(&w,"{\"schema\":\"xcloud4-t27-native-v1\",\"build\":");t27_quoted(&w,r->build_id);
    t27_put(&w,",\"manifest_sha256\":");t27_quoted(&w,r->manifest_sha256);
    t27_put(&w,",\"cell\":%u,\"group\":%u,\"clip\":%u,\"factor\":%u,\"status\":%u,\"final\":true,\"error_code\":%d,\"reason\":",
        r->cell,r->group,r->clip,r->factor,r->status,r->error_code);t27_quoted(&w,r->reason);
    t27_put(&w,",\"config\":");t27_config(&w,&r->config);t27_put(&w,",\"modes\":[");t27_mode(&w,&r->modes[0]);t27_put(&w,",");t27_mode(&w,&r->modes[1]);t27_put(&w,"]}");
    X4JsonSpan check;if(w.error||x4_json_parse(text,w.used,&check))return -2;
    return (int)w.used;
}
int x4_t27_native_result_encode(const X4T26CellResult *r,char *out,size_t capacity)
{
    if(!out||!capacity)return -1;
    char *text=malloc(X4_T26_RESULT_TEXT_MAX);if(!text)return -2;
    int n=t27_encode_into(r,text);if(n>=0){if((size_t)n>=capacity)n=-2;else memcpy(out,text,(size_t)n+1);}free(text);return n;
}
int x4_t27_native_result_write(FILE *f,const X4T26CellResult *r)
{
    if(!f)return -1;
    char *text=malloc(X4_T26_RESULT_TEXT_MAX);if(!text)return -2;
    int n=t27_encode_into(r,text);if(n>=0)n=fwrite(text,1,(size_t)n,f)==(size_t)n&&!ferror(f)?0:-2;
    free(text);return n;
}
static int t27_object(X4JsonSpan value,const char *const *names,unsigned count,X4JsonSpan *fields)
{
    if(x4_json_type(value)!=X4_JSON_T_OBJECT||count>63)return -1;
    uint64_t seen=0;size_t cursor=0;X4JsonSpan key,val;int rc;
    while((rc=x4_json_object_member(value,&cursor,&key,&val))>0){
        /* object_member keys are raw unquoted spans. Accept only the exact
         * schema spellings; an escaped/unknown spelling is not canonical. */
        unsigned i=0;while(i<count&&(strlen(names[i])!=key.length||memcmp(key.data,names[i],key.length)))++i;
        if(i==count||(seen&(UINT64_C(1)<<i)))return -1;
        seen|=UINT64_C(1)<<i;fields[i]=val;
    }
    return rc<0||seen!=((UINT64_C(1)<<count)-1)?-1:0;
}
static int t27_u64(X4JsonSpan s,uint64_t *out)
{
    if(x4_json_type(s)!=X4_JSON_T_NUMBER||!s.length)return -1;
    uint64_t n=0;
    for(size_t i=0;i<s.length;++i){unsigned char c=(unsigned char)s.data[i];
        if(c<'0'||c>'9'||n>(UINT64_MAX-(c-'0'))/10)return -1;
        n=n*10+(c-'0');}
    *out=n;return 0;
}
static int t27_i32(X4JsonSpan s,int *out)
{
    if(x4_json_type(s)!=X4_JSON_T_NUMBER||!s.length)return -1;
    bool negative=s.data[0]=='-';X4JsonSpan magnitude={s.data+(negative?1:0),s.length-(negative?1:0)};
    uint64_t n;if(t27_u64(magnitude,&n)||n>(negative?UINT64_C(2147483648):INT_MAX))return -1;
    *out=negative?(n==UINT64_C(2147483648)?INT_MIN:-(int)n):(int)n;return 0;
}
static int t27_natural(X4JsonSpan s,unsigned *out)
{uint64_t n;if(t27_u64(s,&n)||n>UINT_MAX)return -1;*out=(unsigned)n;return 0;}
static int t27_boolean(X4JsonSpan s,bool *out)
{int n;if(x4_json_bool(s,&n))return -1;*out=n!=0;return 0;}
static int t27_floating(X4JsonSpan s,double *out)
{
    if(x4_json_type(s)!=X4_JSON_T_NUMBER||!s.length||s.length>=64)return -1;
    char text[64];memcpy(text,s.data,s.length);text[s.length]=0;char *end=NULL;
    double n=strtod(text,&end);if(end!=text+s.length||!isfinite(n)||n<0)return -1;*out=n;return 0;
}
static int t27_array(X4JsonSpan s,X4JsonSpan *values,unsigned capacity,unsigned *count)
{
    if(x4_json_type(s)!=X4_JSON_T_ARRAY)return -1;
    size_t cursor=0;unsigned n=0;X4JsonSpan value;int rc;
    while((rc=x4_json_item(s,&cursor,&value))>0){if(n>=capacity)return -1;values[n++]=value;}
    if(rc<0)return -1;
    *count=n;return 0;
}
static int t27_exact_array(X4JsonSpan s,X4JsonSpan *values,unsigned count)
{unsigned n;return t27_array(s,values,count,&n)||n!=count?-1:0;}
static int t27_integer_array(X4JsonSpan s,unsigned count)
{
    X4JsonSpan a[24];if(count>24||t27_exact_array(s,a,count))return -1;
    for(unsigned i=0;i<count;++i){X4JsonSpan v=a[i];bool negative=v.length&&v.data[0]=='-';
        if(negative){++v.data;--v.length;}
        uint64_t n;if(t27_u64(v,&n)||(negative&&n>UINT64_C(9223372036854775808)))return -1;}
    return 0;
}
static int t27_rows(X4JsonSpan s,unsigned max,unsigned columns,unsigned *count)
{
    X4JsonSpan a[16];if(max>16)return -1;unsigned n;if(t27_array(s,a,max,&n))return -1;
    for(unsigned i=0;i<n;++i)if(t27_integer_array(a[i],columns))return -1;
    if(count)*count=n;
    return 0;
}
static int t27_read_config(X4JsonSpan s,X4T26Config *c)
{
    static const char *const keys[]={"api","resource_type","depth","output_memory_type","affinity_mask","priority","pipe_id","queue_id","realistic"};
    X4JsonSpan a[9];
    if(t27_object(s,keys,9,a)||t27_natural(a[0],&c->api)||t27_natural(a[1],&c->resource_type)||t27_natural(a[2],&c->depth)||
        t27_i32(a[3],&c->output_memory_type)||t27_u64(a[4],&c->affinity_mask)||t27_i32(a[5],&c->priority)||
        t27_natural(a[6],&c->pipe_id)||t27_natural(a[7],&c->queue_id)||t27_boolean(a[8],&c->realistic)||!x4_t26_config_valid(c))return -1;
    return 0;
}
static int t27_read_stats(X4JsonSpan s,X4T26Stats *out)
{
    memset(out,0,sizeof(*out));if(x4_json_type(s)==X4_JSON_T_NULL)return 0;
    static const char *const keys[]={"n","p50_us","p95_us","p99_us","max_us","mean_us"};
    X4JsonSpan a[6];
    return t27_object(s,keys,6,a)||t27_u64(a[0],&out->n)||!out->n||t27_u64(a[1],&out->p50_us)||t27_u64(a[2],&out->p95_us)||
        t27_u64(a[3],&out->p99_us)||t27_u64(a[4],&out->max_us)||t27_floating(a[5],&out->mean_us)||!t27_stats_valid(out)?-1:0;
}
static int t27_read_cpu(X4JsonSpan s,X4T26CpuDelta *out)
{
    static const char *const keys[]={"wall_us","process","thread"};X4JsonSpan a[3],pair[2];
    if(t27_object(s,keys,3,a)||t27_u64(a[0],&out->wall_us))return -1;
    for(unsigned i=0;i<2;++i){if(x4_json_type(a[i+1])==X4_JSON_T_NULL)continue;
        uint64_t used;double percent;
        if(t27_exact_array(a[i+1],pair,2)||t27_u64(pair[0],&used)||t27_floating(pair[1],&percent)||!out->wall_us||
            fabs(percent-100.0*(double)used/(double)out->wall_us)>0.000001)return -1;
        if(!i){out->process_available=true;out->process_us=used;out->process_percent=percent;}
        else{out->thread_available=true;out->thread_us=used;out->thread_percent=percent;}
    }
    return 0;
}
static int t27_read_backend(X4JsonSpan s,X4T26BackendInfo *b)
{
    static const char *const keys[]={"state","sps","memory","v2","v1","platform"};X4JsonSpan a[6],values[24];
    if(t27_object(s,keys,6,a)||t27_exact_array(a[0],values,9)||t27_integer_array(a[0],9)||t27_integer_array(a[1],11))return -1;
    bool *flags[]={&b->attempted,&b->opened,&b->closed,&b->retained,&b->owned_clean};
    for(unsigned i=0;i<5;++i){uint64_t n;if(t27_u64(values[i],&n)||n>1)return -1;*flags[i]=n!=0;}
    if(t27_i32(values[5],&b->open_result)||t27_i32(values[6],&b->close_result)||t27_i32(values[7],&b->local_result)||t27_natural(values[8],&b->output_count))return -1;
    static const char *const mk[]={"owned","largest","checks"};X4JsonSpan memory[3],owned[4];unsigned checks;
    if(t27_object(a[2],mk,3,memory)||t27_exact_array(memory[0],owned,4)||t27_natural(owned[0],&b->allocations_before)||
        t27_u64(owned[1],&b->owned_bytes_before)||t27_natural(owned[2],&b->allocations_after)||t27_u64(owned[3],&b->owned_bytes_after)||
        b->allocations_before>16||b->allocations_after>16||t27_integer_array(memory[1],6)||t27_rows(memory[2],16,16,&checks)||checks!=b->allocations_before)return -1;
    if(b->owned_clean&&(!b->closed||b->retained||b->close_result||b->allocations_after||b->owned_bytes_after))return -1;
    static const char *const v2k[]={"config","rc","bytes","attempts"};X4JsonSpan v2[4];
    if(t27_object(a[3],v2k,4,v2)||t27_integer_array(v2[0],10)||t27_integer_array(v2[1],7)||t27_integer_array(v2[2],8)||t27_rows(v2[3],6,21,NULL))return -1;
    static const char *const v1k[]={"rc","resource","counts","resolve"};X4JsonSpan v1[4];
    if(t27_object(a[4],v1k,4,v1)||t27_integer_array(v1[0],16)||t27_integer_array(v1[1],11)||t27_integer_array(v1[2],18)||t27_integer_array(v1[3],6))return -1;
    static const char *const pk[]={"resolve","modules","memory","dropped"};X4JsonSpan p[4];unsigned modules,dropped;
    if(t27_object(a[5],pk,4,p)||t27_integer_array(p[0],6)||t27_rows(p[1],2,7,&modules)||modules!=2||t27_rows(p[2],16,7,NULL)||t27_natural(p[3],&dropped))return -1;
    return 0;
}
static int t27_read_hashes(X4JsonSpan s,X4T26HashResult h[3])
{
    X4JsonSpan values[3];if(t27_exact_array(s,values,3))return -1;
    static const char *const keys[]={"image","sha256","compared","matches_base"};
    static const uint64_t ordinal[3]={1,300,600};
    for(unsigned i=0;i<3;++i){if(x4_json_type(values[i])==X4_JSON_T_NULL)continue;X4JsonSpan a[4];
        if(t27_object(values[i],keys,4,a)||t27_u64(a[0],&h[i].measured_ordinal)||h[i].measured_ordinal!=ordinal[i]||
            x4_json_token(a[1],h[i].sha256,65)||!t27_hash_valid(h[i].sha256)||t27_boolean(a[2],&h[i].compared)||t27_boolean(a[3],&h[i].matches_base))return -1;
        h[i].observed=true;
    }
    return 0;
}
static int t27_read_gpu(X4JsonSpan s,X4T26ModeResult *m)
{
    static const char *const keys[]={"requested","gpu_frames","cpu_frames","reason","scope"};
    X4JsonSpan a[5];char scope[64],reason[384];
    if(t27_object(s,keys,5,a)||t27_boolean(a[0],&m->gpu_requested)||t27_u64(a[1],&m->gpu_frames)||t27_u64(a[2],&m->cpu_frames)||
        x4_json_token(a[4],scope,sizeof(scope))||strcmp(scope,"whole_run_warmup_and_measured"))return -1;
    if(x4_json_type(a[3])!=X4_JSON_T_NULL){
        if(x4_json_display_text(a[3],reason,sizeof(reason))||strlen(reason)>=sizeof(m->gpu_reason))return -1;
        memcpy(m->gpu_reason,reason,strlen(reason)+1);
    }
    return 0;
}
static int t27_read_mode(X4JsonSpan s,unsigned index,X4T26ModeResult *m)
{
    if(x4_json_type(s)==X4_JSON_T_NULL)return 0;
    static const char *const keys[]={"mode","completed","accepted","status","native_rc","local_rc","inference",
        "elapsed_us","wall_us","output_span_us","counts","output_fps","interval_fps","decode","au_to_output","output_interval",
        "decode_to_flip","submit_to_observe","call_delay","measured_flush_outputs","held","cpu","hashes","hash_observer_us","realistic_gpu","backend"};
    X4JsonSpan a[26],pair[2],counts[8],delay[4];
    if(t27_object(s,keys,26,a)||t27_natural(a[0],&m->mode)||m->mode!=index||t27_boolean(a[1],&m->completed)||t27_boolean(a[2],&m->accepted)||
        t27_natural(a[3],&m->status)||m->status>X4_T26_RESULT_NOT_RUN||t27_i32(a[4],&m->native_result)||t27_i32(a[5],&m->local_result)||
        t27_exact_array(a[6],pair,2)||t27_u64(a[7],&m->elapsed_us)||t27_u64(a[8],&m->measured_wall_us)||t27_u64(a[9],&m->output_span_us)||
        t27_exact_array(a[10],counts,8))return -1;
    unsigned infer;if(t27_natural(pair[0],&infer)||infer>1||t27_natural(pair[1],&m->invalid_reasons)||m->invalid_reasons>127)return -1;
    m->inference_valid=infer!=0;
    uint64_t *dest[]={&m->measured_calls,&m->measured_outputs,&m->native_calls,&m->decode_calls,&m->no_output_calls,&m->error_outputs,&m->flush_calls,&m->drained_outputs};
    for(unsigned i=0;i<8;++i)if(t27_u64(counts[i],dest[i]))return -1;
    if((!m->native_calls&&m->native_result!=X4_T26_PLATFORM_NOT_RUN)||t27_read_stats(a[13],&m->decode)||t27_read_stats(a[14],&m->au_latency)||
        t27_read_stats(a[15],&m->output_interval)||t27_read_stats(a[16],&m->decode_to_flip)||t27_read_stats(a[17],&m->submit_to_observe)||
        t27_exact_array(a[18],delay,4)||t27_u64(a[19],&m->measured_flush_outputs)||t27_exact_array(a[20],pair,2)||
        t27_read_cpu(a[21],&m->cpu)||t27_read_hashes(a[22],m->hashes)||t27_u64(a[23],&m->hash_observer_us)||t27_read_gpu(a[24],m)||t27_read_backend(a[25],&m->backend))return -1;
    uint64_t held[2];if(t27_u64(pair[0],&held[0])||t27_u64(pair[1],&held[1])||held[0]>31||held[1]>31)return -1;
    m->held_before_close=(uint32_t)held[0];m->held_after_close=(uint32_t)held[1];
    for(unsigned i=0;i<4;++i)if(t27_u64(delay[i],&m->call_delay_hist[i]))return -1;
    for(unsigned i=0;i<2;++i){bool available=i?(m->output_span_us&&m->measured_outputs>1):m->measured_wall_us!=0;
        if(!available){if(x4_json_type(a[11+i])!=X4_JSON_T_NULL)return -1;continue;}
        double fps,expected=i?(double)(m->measured_outputs-1)*1000000.0/(double)m->output_span_us:
            (double)m->measured_outputs*1000000.0/(double)m->measured_wall_us;
        if(t27_floating(a[11+i],&fps)||fabs(fps-expected)>0.000001)return -1;
    }
    m->present=true;return 0;
}
int x4_t27_native_result_read(const char *text,size_t bytes,const char *expected_build,const char *expected_manifest,X4T26RecoveredResult *out)
{
    if(!text||!bytes||bytes>=X4_T26_RESULT_TEXT_MAX||!out||!t27_id_valid(expected_build)||!t27_hash_valid(expected_manifest))return -1;
    X4JsonSpan root,a[13],modes[2];
    static const char *const keys[]={"schema","build","manifest_sha256","cell","group","clip","factor","status","final","error_code","reason","config","modes"};
    X4T26CellResult *r=calloc(1,sizeof(*r));if(!r)return -2;
    char schema[32],reason[768];int rc=-1;
    if(x4_json_parse(text,bytes,&root)||t27_object(root,keys,13,a)||x4_json_token(a[0],schema,sizeof(schema))||strcmp(schema,"xcloud4-t27-native-v1")||
        x4_json_token(a[1],r->build_id,65)||strcmp(r->build_id,expected_build)||x4_json_token(a[2],r->manifest_sha256,65)||
        strcmp(r->manifest_sha256,expected_manifest)||t27_natural(a[3],&r->cell)||t27_natural(a[4],&r->group)||t27_natural(a[5],&r->clip)||
        t27_natural(a[6],&r->factor)||t27_natural(a[7],&r->status)||t27_boolean(a[8],&r->final)||t27_i32(a[9],&r->error_code)||
        x4_json_display_text(a[10],reason,sizeof(reason))||strlen(reason)>=sizeof(r->reason)||t27_read_config(a[11],&r->config)||t27_exact_array(a[12],modes,2)||
        t27_read_mode(modes[0],0,&r->modes[0])||t27_read_mode(modes[1],1,&r->modes[1]))goto end;
    memcpy(r->reason,reason,strlen(reason)+1);
    for(unsigned i=0;i<2;++i)r->modes[i].backend.config=r->config;
    if(!t27_valid(r))goto end;
    X4T26RecoveredResult value={.cell=r->cell,.group=r->group,.clip=r->clip,.factor=r->factor,.status=r->status,
        .error_code=r->error_code,.config=r->config};memcpy(value.reason,r->reason,sizeof(value.reason));
    value.outcome.completed=true;bool base=t27_base_cell(r->group,r->factor),pass=r->status==X4_T26_RESULT_PASS&&!r->error_code;
    for(unsigned i=0;i<2;++i){X4T26ModeResult *m=&r->modes[i];
        bool needed=i==(r->config.realistic?X4_T26_MODE_PACED:X4_T26_MODE_MAXIMUM);
        if(m->present){value.mode_mask|=1u<<i;
            if(m->measured_wall_us){value.native_fps_available[i]=true;
                value.native_fps[i]=(double)m->measured_outputs*1000000.0/(double)m->measured_wall_us;}
            if(m->elapsed_us>UINT64_MAX-value.elapsed_us)goto end;
            value.elapsed_us+=m->elapsed_us;
            uint64_t errors=m->error_outputs+(m->native_result!=0&&m->native_result!=X4_T26_PLATFORM_NOT_RUN?1u:0u)+(m->local_result?1u:0u);
            if(errors<m->error_outputs||errors>UINT64_MAX-value.outcome.errors)goto end;
            value.outcome.errors+=errors;
            if(m->decode.p95_us>value.outcome.decode_p95_us)value.outcome.decode_p95_us=m->decode.p95_us;
            for(unsigned j=0;j<3;++j)if(m->hashes[j].observed)memcpy(value.hashes[i][j],m->hashes[j].sha256,65);
        }
        if(needed&&!t27_mode_pass(m,base))pass=false;
        if(!needed&&m->present)goto end;
    }
    if(base&&pass){
        value.base_hashes_valid=pass;
    }
    value.outcome.accepted=pass;value.outcome.hashes_ok=pass;
    *out=value;rc=0;
end:free(r);return rc;
}
static int t27_timing(FILE *f,const X4PipelineTimingSummary *s)
{
 return fprintf(f,"{\"samples\":%" PRIu64 ",\"sum_us\":%" PRIu64 ",\"max_us\":%" PRIu64
  ",\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64 ",\"p99_us\":%" PRIu64 ",\"finite_mask\":%u}",
  s->samples,s->sum_us,s->maximum_us,s->p50_us,s->p95_us,s->p99_us,s->finite_mask)<0?-1:0;
}
static int t27_write_presentation(FILE *f,const X4PipelinePresentationSummary *s)
{
 if(fprintf(f,"{\"modes\":[")<0)return -1;
 for(unsigned i=0;i<X4_PRESENT_MODE_COUNT;++i){const X4PipelinePresentationModeSummary *m=&s->mode[i];
  if(fprintf(f,"%s{\"mode\":%u,\"frames\":%" PRIu64 ",\"first_observed_us\":%" PRIu64 ",\"last_observed_us\":%" PRIu64 ",\"decode_to_flip\":",i?",":"",i,m->frames,m->first_observed_us,m->last_observed_us)<0||t27_timing(f,&m->decode_to_flip)||
   fputs(",\"submit_to_flip\":",f)==EOF||t27_timing(f,&m->submit_to_flip)||fputs(",\"flip_to_conversion\":",f)==EOF||t27_timing(f,&m->flip_to_conversion)||fputc('}',f)==EOF)return -1;
 }
 if(fputs("],\"decoder_slot_wait\":",f)==EOF||t27_timing(f,&s->decoder_slot_wait)||fputs(",\"decoder_ring_gate_wait\":",f)==EOF||t27_timing(f,&s->decoder_ring_gate_wait))return -1;
 if(fprintf(f,",\"decode_ownership_samples\":%" PRIu64 ",\"held_native_by_count\":[",s->decode_ownership_samples)<0)return -1;
 for(unsigned i=0;i<6u;++i)if(fprintf(f,"%s%" PRIu64,i?",":"",s->held_native_by_count[i])<0)return -1;
 if(fputs("],\"occupied_ring_by_count\":[",f)==EOF)return -1;
 for(unsigned i=0;i<6u;++i)if(fprintf(f,"%s%" PRIu64,i?",":"",s->occupied_ring_by_count[i])<0)return -1;
 return fprintf(f,"],\"ring_full_drops\":%" PRIu64 ",\"mailbox_replaced\":%" PRIu64 ",\"mailbox_discarded_old\":%" PRIu64
  ",\"mailbox_late_opportunities\":%" PRIu64 ",\"mailbox_late_flips\":%" PRIu64 ",\"clock_anchors\":%" PRIu64
  ",\"clock_invalid_anchors\":%" PRIu64 ",\"clock_residual_samples\":%" PRIu64 ",\"clock_residual_sum_us\":%" PRIu64
  ",\"clock_residual_max_us\":%" PRIu64 ",\"clock_period_us\":%" PRIu64 ",\"clock_margin_us\":%" PRIu64
  ",\"clock_warmup_submissions\":%" PRIu64 ",\"scanouts\":%u,\"scanout_bytes\":%" PRIu64 "}",
  s->ring_full_drops,s->mailbox_replaced,s->mailbox_discarded_old,s->mailbox_late_opportunities,s->mailbox_late_flips,
  s->clock_anchors,s->clock_invalid_anchors,s->clock_residual_samples,s->clock_residual_sum_us,s->clock_residual_max_us,
  s->clock_period_us,s->clock_margin_us,s->clock_warmup_submissions,s->scanouts,s->scanout_bytes)<0?-1:0;
}
static int t27_read_timing(X4JsonSpan object,X4PipelineTimingSummary *out)
{
 const char *const keys[]={"samples","sum_us","max_us","p50_us","p95_us","p99_us","finite_mask"};X4JsonSpan a[7];
 X4PipelineTimingSummary s={0};unsigned mask;
 if(t27_object(object,keys,7u,a)||t27_u64(a[0],&s.samples)||t27_u64(a[1],&s.sum_us)||t27_u64(a[2],&s.maximum_us)||
  t27_u64(a[3],&s.p50_us)||t27_u64(a[4],&s.p95_us)||t27_u64(a[5],&s.p99_us)||t27_natural(a[6],&mask)||mask>7u)return -1;
 s.finite_mask=mask;
 /* Presentation percentiles are upper bounds of 100-us histogram bins. */
 uint64_t upper=s.maximum_us>UINT64_MAX-100u?UINT64_MAX:s.maximum_us+100u;
 if((!s.samples&&(s.sum_us||s.maximum_us||s.p50_us||s.p95_us||s.p99_us))||
  ((mask&1u)&&s.p50_us>upper)||((mask&2u)&&s.p95_us>upper)||((mask&4u)&&s.p99_us>upper)||
  ((mask&3u)==3u&&s.p50_us>s.p95_us)||((mask&6u)==6u&&s.p95_us>s.p99_us))return -1;
 *out=s;return 0;
}
static int t27_read_presentation(X4JsonSpan object,X4PipelinePresentationSummary *out)
{
 static const char *const keys[]={"modes","decoder_slot_wait","decoder_ring_gate_wait","decode_ownership_samples",
  "held_native_by_count","occupied_ring_by_count","ring_full_drops","mailbox_replaced","mailbox_discarded_old",
  "mailbox_late_opportunities","mailbox_late_flips","clock_anchors","clock_invalid_anchors","clock_residual_samples",
  "clock_residual_sum_us","clock_residual_max_us","clock_period_us","clock_margin_us","clock_warmup_submissions","scanouts","scanout_bytes"};
 X4JsonSpan a[21],modes[3],held[6],occupied[6];X4PipelinePresentationSummary s={0};
 if(t27_object(object,keys,21u,a)||t27_exact_array(a[0],modes,3u)||t27_read_timing(a[1],&s.decoder_slot_wait)||
  t27_read_timing(a[2],&s.decoder_ring_gate_wait)||t27_u64(a[3],&s.decode_ownership_samples)||
  t27_exact_array(a[4],held,6u)||t27_exact_array(a[5],occupied,6u))return -1;
 for(unsigned i=0;i<3u;++i){static const char *const mk[]={"mode","frames","first_observed_us","last_observed_us","decode_to_flip","submit_to_flip","flip_to_conversion"};
  X4JsonSpan m[7];unsigned index;X4PipelinePresentationModeSummary *v=&s.mode[i];
  if(t27_object(modes[i],mk,7u,m)||t27_natural(m[0],&index)||index!=i||t27_u64(m[1],&v->frames)||
   t27_u64(m[2],&v->first_observed_us)||t27_u64(m[3],&v->last_observed_us)||t27_read_timing(m[4],&v->decode_to_flip)||
   t27_read_timing(m[5],&v->submit_to_flip)||t27_read_timing(m[6],&v->flip_to_conversion)||
   (v->frames&&v->last_observed_us<v->first_observed_us))return -1;
 }
 for(unsigned i=0;i<6u;++i)if(t27_u64(held[i],&s.held_native_by_count[i])||t27_u64(occupied[i],&s.occupied_ring_by_count[i]))return -1;
 uint64_t *fields[]={&s.ring_full_drops,&s.mailbox_replaced,&s.mailbox_discarded_old,&s.mailbox_late_opportunities,&s.mailbox_late_flips,
  &s.clock_anchors,&s.clock_invalid_anchors,&s.clock_residual_samples,&s.clock_residual_sum_us,&s.clock_residual_max_us,
  &s.clock_period_us,&s.clock_margin_us,&s.clock_warmup_submissions};
 for(unsigned i=0;i<13u;++i)if(t27_u64(a[6u+i],fields[i]))return -1;
 if(t27_natural(a[19],&s.scanouts)||(s.scanouts&&s.scanouts!=3u&&s.scanouts!=4u)||t27_u64(a[20],&s.scanout_bytes))return -1;
 *out=s;return 0;
}
#define T27_TIMING_SCOPE "native_output_ids_181_780"
#define T27_OWNERSHIP_SCOPE "acquisition_before_decode_returning_measured_output"
#define T27_COUNTER_SCOPE "whole_cell_including_180_au_warmup"
static bool t27_expected_text(X4JsonSpan value,const char *expected)
{char text[128];return !x4_json_token(value,text,sizeof(text))&&!strcmp(text,expected);}
int x4_t27_result_write(FILE *f,const X4T26CellResult *native,const X4PipelinePresentationSummary *presentation)
{
 X4T27Cell cell;if(!f||!native||!x4_t27_plan_cell(native->cell,&cell)||(cell.reference?presentation!=NULL:presentation==NULL))return -1;
 if(fprintf(f,"{\"schema\":\"X4T27_CELL1\",\"index\":%u,\"source_hz\":%u,\"vsync_policy\":%u,\"mailbox\":%s,\"timing_scope\":\"%s\",\"ownership_scope\":\"%s\",\"counter_scope\":\"%s\",\"native\":",
  cell.index,cell.presentation.source_hz,cell.presentation.vsync_policy,cell.presentation.mailbox?"true":"false",T27_TIMING_SCOPE,T27_OWNERSHIP_SCOPE,T27_COUNTER_SCOPE)<0||x4_t27_native_result_write(f,native)||fputs(",\"presentation\":",f)==EOF)return -1;
 if(cell.reference){if(fputs("null",f)==EOF)return -1;}else if(t27_write_presentation(f,presentation))return -1;
 return fputc('}',f)==EOF||ferror(f)?-1:0;
}
int x4_t27_result_read(const char *text,size_t size,const char *build,const char *manifest,X4T26RecoveredResult *out,X4PipelinePresentationSummary *presentation)
{
 if(!out)return -1;X4JsonSpan root,a[10];char schema[32];unsigned index,hz,policy;bool mailbox;
 const char *const keys[]={"schema","index","source_hz","vsync_policy","mailbox","timing_scope","ownership_scope","counter_scope","native","presentation"};
 X4T27Cell cell;X4PipelinePresentationSummary p={0};X4T26RecoveredResult r;
 if(x4_json_parse(text,size,&root)||t27_object(root,keys,10u,a)||x4_json_token(a[0],schema,sizeof(schema))||strcmp(schema,"X4T27_CELL1")||
  t27_natural(a[1],&index)||!x4_t27_plan_cell(index,&cell)||t27_natural(a[2],&hz)||hz!=cell.presentation.source_hz||
  t27_natural(a[3],&policy)||policy!=cell.presentation.vsync_policy||t27_boolean(a[4],&mailbox)||mailbox!=cell.presentation.mailbox||
  !t27_expected_text(a[5],T27_TIMING_SCOPE)||!t27_expected_text(a[6],T27_OWNERSHIP_SCOPE)||!t27_expected_text(a[7],T27_COUNTER_SCOPE)||
  x4_t27_native_result_read(a[8].data,a[8].length,build,manifest,&r)||r.cell!=index||
  (cell.reference?x4_json_type(a[9])!=X4_JSON_T_NULL:t27_read_presentation(a[9],&p)!=0))return -1;
 *out=r;if(presentation)*presentation=p;return 0;
}
#endif
