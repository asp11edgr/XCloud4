/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_result.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "../auth/json.h"
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#define SHA_START mbedtls_sha256_starts
#define SHA_UPDATE mbedtls_sha256_update
#define SHA_FINISH mbedtls_sha256_finish
#else
#define SHA_START mbedtls_sha256_starts_ret
#define SHA_UPDATE mbedtls_sha256_update_ret
#define SHA_FINISH mbedtls_sha256_finish_ret
#endif

typedef struct {char *text;size_t used,capacity;int error;} Writer;
static void put(Writer *w,const char *format,...)
{
    if(w->error)return;
    va_list args;va_start(args,format);int n=vsnprintf(w->text+w->used,w->capacity-w->used,format,args);va_end(args);
    if(n<0||(size_t)n>=w->capacity-w->used){w->error=-2;return;}w->used+=(size_t)n;
}
static bool bounded(const char *s,size_t capacity,size_t *n)
{if(!s)return false;for(size_t i=0;i<capacity;++i)if(!s[i]){if(n)*n=i;return true;}return false;}
static bool hash_valid(const char *s)
{
    if(!s||!bounded(s,65,NULL)||strlen(s)!=64)return false;
    for(unsigned i=0;i<64;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
    return true;
}
static bool id_valid(const char *s)
{
    size_t n;if(!bounded(s,65,&n)||!n)return false;
    for(size_t i=0;i<n;++i)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
        (s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||s[i]=='.'))return false;
    return true;
}
static void quoted(Writer *w,const char *s)
{
    put(w,"\"");
    for(const unsigned char *p=(const unsigned char *)s;*p;++p){
        if(*p=='"'||*p=='\\')put(w,"\\%c",*p);
        else if(*p<32)put(w,"\\u%04x",(unsigned)*p);
        else put(w,"%c",*p);
    }
    put(w,"\"");
}
static bool stats_valid(const X4T26Stats *s)
{
    if(!s||s->n>1024||!isfinite(s->mean_us)||s->mean_us<0)return false;
    if(!s->n)return !s->p50_us&&!s->p95_us&&!s->p99_us&&!s->max_us&&s->mean_us==0;
    return s->p50_us<=s->p95_us&&s->p95_us<=s->p99_us&&s->p99_us<=s->max_us&&s->mean_us<=(double)s->max_us;
}
static void stats(Writer *w,const X4T26Stats *s)
{
    if(!s->n){put(w,"null");return;}
    put(w,"{\"n\":%" PRIu64 ",\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64
        ",\"p99_us\":%" PRIu64 ",\"max_us\":%" PRIu64 ",\"mean_us\":%.6f}",
        s->n,s->p50_us,s->p95_us,s->p99_us,s->max_us,s->mean_us);
}
static bool base_cell(unsigned group,unsigned factor)
{return (group==X4_T26_GROUP_FACTORS&&factor==0)||group==X4_T26_GROUP_BASE_EXTRA;}
static bool cell_metadata(const X4T26CellResult *r)
{
    X4T26Config expected;unsigned group,clip,factor;bool fixed=true;
    x4_t26_config_base(&expected);
    if(r->cell<26){group=X4_T26_GROUP_FACTORS;factor=r->cell/2;clip=(r->cell&1)?3:1;
        if(x4_t26_factor_config(factor,&expected))return false;}
    else if(r->cell<29){static const unsigned clips[]={0,2,4};group=X4_T26_GROUP_BASE_EXTRA;factor=0;clip=clips[r->cell-26];}
    else if(r->cell<34){group=X4_T26_GROUP_BEST;factor=UINT_MAX;clip=r->cell-29;fixed=false;}
    else if(r->cell<38){group=X4_T26_GROUP_REALISTIC;factor=(r->cell&1)?UINT_MAX:0;clip=r->cell<36?1:2;
        expected.realistic=true;fixed=factor==0;}
    else{static const unsigned clips[]={1,0,2,3,4};group=X4_T26_GROUP_V1;factor=UINT_MAX;clip=clips[r->cell-38];
        expected=(X4T26Config){.api=X4_T26_API_V1,.depth=1};}
    return r->group==group&&r->clip==clip&&r->factor==factor&&
        (fixed?x4_t26_config_equal(&r->config,&expected):
            r->config.api==X4_T26_API_V2&&r->config.realistic==(group==X4_T26_GROUP_REALISTIC));
}
static bool hashes_good(const X4T26ModeResult *m,bool base)
{
    static const uint64_t ordinal[3]={1,300,600};
    for(unsigned i=0;i<3;++i){const X4T26HashResult *h=&m->hashes[i];
        if(!h->observed||h->measured_ordinal!=ordinal[i]||!hash_valid(h->sha256)||
            (h->compared?!h->matches_base:!base))return false;
    }
    return true;
}
static bool mode_pass(const X4T26ModeResult *m,bool base)
{
    return m->present&&m->completed&&m->accepted&&m->status==X4_T26_RESULT_PASS&&
        m->native_result==0&&m->local_result==0&&!m->error_outputs&&m->inference_valid&&!m->invalid_reasons&&
        m->measured_outputs==600&&m->decode.n&&m->au_latency.n==600&&m->output_interval.n==599&&
        m->backend.owned_clean&&m->backend.closed&&!m->backend.retained&&m->backend.close_result==0&&
        !m->held_after_close&&hashes_good(m,base);
}
static bool valid(const X4T26CellResult *r)
{
    if(!r||!id_valid(r->build_id)||!hash_valid(r->manifest_sha256)||r->cell>=X4_T26_CELL_COUNT||
        r->group>X4_T26_GROUP_V1||r->clip>=X4_T26_CLIP_COUNT||
        r->status>X4_T26_RESULT_NOT_RUN||!r->final||!bounded(r->reason,sizeof(r->reason),NULL)||
        !x4_t26_config_valid(&r->config)||!cell_metadata(r))return false;
    for(unsigned i=0;i<2;++i){const X4T26ModeResult *m=&r->modes[i];if(!m->present)continue;
        if(m->mode!=i||m->status>X4_T26_RESULT_NOT_RUN||m->invalid_reasons>127||
            !stats_valid(&m->decode)||!stats_valid(&m->au_latency)||!stats_valid(&m->output_interval)||
            !stats_valid(&m->decode_to_flip)||!stats_valid(&m->submit_to_observe)||
            !bounded(m->gpu_reason,sizeof(m->gpu_reason),NULL)||
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
        for(unsigned j=0;j<3;++j)if(m->hashes[j].observed&&!hash_valid(m->hashes[j].sha256))return false;
        if(m->accepted&&!mode_pass(m,base_cell(r->group,r->factor)))return false;
    }
    if(r->status==X4_T26_RESULT_PASS){
        for(unsigned i=0;i<2;++i)if((!r->config.realistic||i==X4_T26_MODE_PACED)&&
            (!r->modes[i].present||!r->modes[i].completed))return false;
    }
    return true;
}
static void config(Writer *w,const X4T26Config *c)
{
    put(w,"{\"api\":%u,\"resource_type\":%u,\"depth\":%u,\"output_memory_type\":%d,\"affinity_mask\":%" PRIu64
        ",\"priority\":%d,\"pipe_id\":%u,\"queue_id\":%u,\"realistic\":%s}",c->api,c->resource_type,c->depth,
        c->output_memory_type,c->affinity_mask,c->priority,c->pipe_id,c->queue_id,c->realistic?"true":"false");
}
static void backend(Writer *w,const X4T26BackendInfo *b)
{
    put(w,"{\"state\":[%u,%u,%u,%u,%u,%d,%d,%d,%u],\"sps\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],",
        b->attempted,b->opened,b->closed,b->retained,b->owned_clean,b->open_result,b->close_result,b->local_result,b->output_count,
        b->sps.coded_width,b->sps.coded_height,b->sps.visible_width,b->sps.visible_height,b->sps.profile_idc,b->sps.level_idc,
        b->sps.max_num_ref_frames,b->sps.max_dec_frame_buffering,b->sps.pic_order_cnt_type,b->sps.matrix_coefficients,b->sps.full_range);
    put(w,"\"memory\":{\"owned\":[%u,%" PRIu64 ",%u,%" PRIu64 "],\"largest\":[%u,%d,%" PRIu64 ",%u,%d,%" PRIu64 "],\"checks\":[",
        b->allocations_before,b->owned_bytes_before,b->allocations_after,b->owned_bytes_after,b->largest_before_called,
        b->largest_before_rc,b->largest_before_bytes,b->largest_after_called,b->largest_after_rc,b->largest_after_bytes);
    for(unsigned i=0;i<b->allocations_before;++i){const X4T26AllocationCheck *c=&b->allocation_checks[i];
        put(w,"%s[%" PRId64 ",%" PRIu64 ",%d,%u,%d,%u,%u,%d,%u,%u,%" PRId64 ",%" PRId64 ",%d,%" PRId64 ",%" PRId64 ",%d]",
            i?",":"",c->allocation.physical_offset,c->allocation.bytes,c->allocation.memory_type,c->before_called,c->before_rc,
            c->before_valid,c->after_called,c->after_rc,c->after_valid,c->release_evidence,c->before.start,c->before.end,
            c->before.memory_type,c->after.start,c->after.end,c->after.memory_type);
    }
    put(w,"]},\"v2\":{\"config\":[%u,%u,%u,%u,%u,%u,%u,%u,%d,%" PRIu64 "],\"rc\":[%d,%d,%d,%d,%d,%d,%d],",
        b->v2.requested_depth,b->v2.active_depth,b->v2.input_count,b->v2.output_count,b->v2.held_output_mask,b->v2.live_input_count,
        b->v2.retained,b->v2.creation_uncertain,b->v2.decoder_cpu_priority,b->v2.decoder_cpu_affinity,
        b->v2.compute_query_result,b->v2.allocate_queue_result,b->v2.query_result,b->v2.create_result,
        b->v2.delete_decoder_result,b->v2.release_queue_result,b->v2.close_result);
    put(w,"\"bytes\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"attempts\":[",
        b->v2.native_budget_bytes,b->v2.budget_auxiliary_bytes,b->v2.planned_native_bytes,b->v2.queried_compute_bytes,
        b->v2.queried_cpu_bytes,b->v2.queried_gpu_bytes,b->v2.queried_shared_bytes,b->v2.queried_frame_bytes);
    for(unsigned i=0;i<b->v2.attempt_count;++i){const X4StreamDecoderAttempt *a=&b->v2.attempts[i];
        put(w,"%s[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]",
            i?",":"",a->profile,a->level,a->width,a->height,a->dpb,a->depth,a->kind,a->reason,a->failure_reason,a->skipped,
            a->compute_query,a->allocate_queue,a->query,a->create,a->cleanup,a->compute_bytes,a->cpu_bytes,a->gpu_bytes,
            a->shared_bytes,a->frame_bytes,a->planned_bytes);
    }
    const X4T26V1Info *v=&b->v1;
    put(w,"]},\"v1\":{\"rc\":[%u,%d,%u,%d,%u,%u,%d,%d,%u,%d,%d,%u,%d,%d,%d,%d],",
        v->module_called,v->module_rc,v->arbitration_called,v->arbitration_rc,v->arbitration_retry,
        v->query_called_mask,v->query_rc[0],v->query_rc[1],v->create_called_mask,v->create_rc[0],v->create_rc[1],
        v->delete_called,v->delete_rc,v->last_decode_rc,v->last_flush_rc,v->local_result);
    put(w,"\"resource\":[%u,%u,%u,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u],",
        v->config.codecType,v->config.profile,v->config.maxLevel,v->config.maxFrameWidth,v->config.maxFrameHeight,v->config.maxDpbFrameCount,
        v->config.videodecFlags,v->query_memory.cpuMemorySize,v->query_memory.cpuGpuMemorySize,v->query_memory.maxFrameBufferSize,
        v->query_memory.frameBufferAlignment);
    put(w,"\"counts\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%u,%u,%u,%u],\"resolve\":[%u,%d,%d,%d,%d,%d]},",
        v->query_calls,v->create_calls,v->decode_calls,v->flush_calls,v->delete_calls,v->native_calls,v->valid_outputs,v->without_output,
        v->flush_without_output,v->error_outputs,v->input_used_bytes,v->allocated_bytes,v->peak_allocated_bytes,v->allocation_count,
        v->held_mask,v->opened,v->decoder_live,v->retained,v->resolve_called_mask,v->resolve_rc[0],v->resolve_rc[1],v->resolve_rc[2],v->resolve_rc[3],v->resolve_rc[4]);
    const X4T26PlatformInfo *p=&b->platform;
    put(w,"\"platform\":{\"resolve\":[%d,%d,%d,%d,%d,%d],\"modules\":[",p->kernel_handle,p->sysmodule_handle,p->decoder_handle,
        p->usage_resolve_rc,p->query_resolve_rc,p->available_resolve_rc);
    for(unsigned i=0;i<2;++i){const X4T26ModuleObservation *m=&p->modules[i];
        put(w,"%s[%u,%u,%d,%u,%d,%u,%d]",i?",":"",m->module_id,m->before_called,m->before_rc,m->load_called,m->load_rc,m->after_called,m->after_rc);}
    put(w,"],\"memory\":[");
    for(unsigned i=0;i<p->memory_observations;++i){const X4T26MemoryObservation *m=&p->memory[i];
        put(w,"%s[%" PRId64 ",%" PRIu64 ",%d,%d,%d,%d,%d]",i?",":"",m->offset,m->bytes,m->memory_type,m->allocate_rc,m->map_rc,m->unmap_rc,m->release_rc);}
    put(w,"],\"dropped\":%u}}",p->memory_observations_dropped);
}
static void mode(Writer *w,const X4T26ModeResult *m)
{
    if(!m->present){put(w,"null");return;}
    put(w,"{\"mode\":%u,\"completed\":%s,\"accepted\":%s,\"status\":%u,\"native_rc\":%d,\"local_rc\":%d,\"inference\":[%u,%u],",
        m->mode,m->completed?"true":"false",m->accepted?"true":"false",m->status,m->native_result,m->local_result,m->inference_valid,m->invalid_reasons);
    put(w,"\"elapsed_us\":%" PRIu64 ",\"wall_us\":%" PRIu64 ",\"output_span_us\":%" PRIu64 ",\"counts\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],",
        m->elapsed_us,m->measured_wall_us,m->output_span_us,m->measured_calls,m->measured_outputs,m->native_calls,m->decode_calls,
        m->no_output_calls,m->error_outputs,m->flush_calls,m->drained_outputs);
    put(w,"\"output_fps\":");if(m->measured_wall_us)put(w,"%.6f",(double)m->measured_outputs*1000000.0/(double)m->measured_wall_us);else put(w,"null");
    put(w,",\"interval_fps\":");if(m->output_span_us&&m->measured_outputs>1)put(w,"%.6f",(double)(m->measured_outputs-1)*1000000.0/(double)m->output_span_us);else put(w,"null");
    put(w,",\"decode\":");stats(w,&m->decode);put(w,",\"au_to_output\":");stats(w,&m->au_latency);
    put(w,",\"output_interval\":");stats(w,&m->output_interval);put(w,",\"decode_to_flip\":");stats(w,&m->decode_to_flip);
    put(w,",\"submit_to_observe\":");stats(w,&m->submit_to_observe);
    put(w,",\"call_delay\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"measured_flush_outputs\":%" PRIu64 ",\"held\":[%u,%u],\"cpu\":{\"wall_us\":%" PRIu64 ",\"process\":",
        m->call_delay_hist[0],m->call_delay_hist[1],m->call_delay_hist[2],m->call_delay_hist[3],m->measured_flush_outputs,m->held_before_close,m->held_after_close,m->cpu.wall_us);
    if(m->cpu.process_available)put(w,"[%" PRIu64 ",%.6f]",m->cpu.process_us,m->cpu.process_percent);else put(w,"null");
    put(w,",\"thread\":");if(m->cpu.thread_available)put(w,"[%" PRIu64 ",%.6f]",m->cpu.thread_us,m->cpu.thread_percent);else put(w,"null");
    put(w,"},\"hashes\":[");
    for(unsigned i=0;i<3;++i){const X4T26HashResult *h=&m->hashes[i];
        if(i)put(w,",");
        if(!h->observed){put(w,"null");continue;}
        put(w,"{\"image\":%" PRIu64 ",\"sha256\":",h->measured_ordinal);quoted(w,h->sha256);
        put(w,",\"compared\":%s,\"matches_base\":%s}",h->compared?"true":"false",h->matches_base?"true":"false");
    }
    put(w,"],\"hash_observer_us\":%" PRIu64 ",\"realistic_gpu\":{\"requested\":%s,\"gpu_frames\":%" PRIu64 ",\"cpu_frames\":%" PRIu64 ",\"reason\":",
        m->hash_observer_us,m->gpu_requested?"true":"false",m->gpu_frames,m->cpu_frames);
    if(m->gpu_reason[0])quoted(w,m->gpu_reason);else put(w,"null");
    put(w,",\"scope\":\"whole_run_warmup_and_measured\"},\"backend\":");backend(w,&m->backend);put(w,"}");
}
static int encode_into(const X4T26CellResult *r,char *text)
{
    if(!valid(r))return -1;
    Writer w={.text=text,.capacity=X4_T26_RESULT_TEXT_MAX};
    put(&w,"{\"schema\":\"xcloud4-t26-cell-v1\",\"build\":");quoted(&w,r->build_id);
    put(&w,",\"manifest_sha256\":");quoted(&w,r->manifest_sha256);
    put(&w,",\"cell\":%u,\"group\":%u,\"clip\":%u,\"factor\":%u,\"status\":%u,\"final\":true,\"error_code\":%d,\"reason\":",
        r->cell,r->group,r->clip,r->factor,r->status,r->error_code);quoted(&w,r->reason);
    put(&w,",\"config\":");config(&w,&r->config);put(&w,",\"modes\":[");mode(&w,&r->modes[0]);put(&w,",");mode(&w,&r->modes[1]);put(&w,"]}");
    X4JsonSpan check;if(w.error||x4_json_parse(text,w.used,&check))return -2;
    return (int)w.used;
}
int x4_t26_result_encode(const X4T26CellResult *r,char *out,size_t capacity)
{
    if(!out||!capacity)return -1;
    char *text=malloc(X4_T26_RESULT_TEXT_MAX);if(!text)return -2;
    int n=encode_into(r,text);if(n>=0){if((size_t)n>=capacity)n=-2;else memcpy(out,text,(size_t)n+1);}free(text);return n;
}
int x4_t26_result_write(FILE *f,const X4T26CellResult *r)
{
    if(!f)return -1;
    char *text=malloc(X4_T26_RESULT_TEXT_MAX);if(!text)return -2;
    int n=encode_into(r,text);if(n>=0)n=fwrite(text,1,(size_t)n,f)==(size_t)n&&!ferror(f)?0:-2;
    free(text);return n;
}
int x4_t26_result_hash_nv12(const X4T26Output *output,char hex[65])
{
    if(!output||!hex||!output->layout_verified||!output->pointer||!output->valid||output->error||output->native_rc||output->local_result)return -1;
    const X4VideoNv12Layout *l=&output->layout;
    if(!l->visible_width||!l->visible_height||!l->pitch||!l->storage_height||
        ((l->visible_width|l->visible_height|l->pitch|l->storage_height)&1)||l->pitch<l->visible_width||
        l->total_nv12_bytes>output->owned_bytes||l->total_nv12_bytes>output->returned_bytes||
        l->visible_y_offset>l->y_bytes||l->visible_uv_offset<l->uv_plane_offset||l->visible_uv_offset>l->total_nv12_bytes)return -1;
    size_t y_bytes,uv_bytes,total;
    if(x4_video_size_mul(l->pitch,l->storage_height,&y_bytes)||x4_video_size_mul(l->pitch,l->storage_height/2,&uv_bytes)||
        x4_video_size_add(y_bytes,uv_bytes,&total)||l->y_bytes!=y_bytes||l->uv_bytes!=uv_bytes||
        l->uv_plane_offset!=y_bytes||l->total_nv12_bytes!=total)return -1;
    size_t crop_y=l->visible_y_offset/l->pitch,crop_x=l->visible_y_offset%l->pitch,expected_uv;
    if((crop_x|crop_y)&1||crop_x>l->pitch||l->visible_width>l->pitch-crop_x||
        crop_y>l->storage_height||l->visible_height>l->storage_height-crop_y||
        x4_video_size_mul(crop_y/2,l->pitch,&expected_uv)||x4_video_size_add(expected_uv,y_bytes,&expected_uv)||
        x4_video_size_add(expected_uv,crop_x,&expected_uv)||l->visible_uv_offset!=expected_uv)return -1;
    size_t y_end,uv_end,span;
    if(x4_video_size_mul(l->visible_height-1,l->pitch,&span)||x4_video_size_add(l->visible_y_offset,span,&y_end)||
        x4_video_size_add(y_end,l->visible_width,&y_end)||y_end>l->y_bytes||
        x4_video_size_mul(l->visible_height/2-1,l->pitch,&span)||x4_video_size_add(l->visible_uv_offset,span,&uv_end)||
        x4_video_size_add(uv_end,l->visible_width,&uv_end)||uv_end>l->total_nv12_bytes)return -1;
    mbedtls_sha256_context context;mbedtls_sha256_init(&context);int rc=SHA_START(&context,0);
    for(unsigned row=0;!rc&&row<l->visible_height;++row)rc=SHA_UPDATE(&context,output->pointer+l->visible_y_offset+(size_t)row*l->pitch,l->visible_width);
    for(unsigned row=0;!rc&&row<l->visible_height/2;++row)rc=SHA_UPDATE(&context,output->pointer+l->visible_uv_offset+(size_t)row*l->pitch,l->visible_width);
    unsigned char digest[32];if(!rc)rc=SHA_FINISH(&context,digest);mbedtls_sha256_free(&context);
    if(rc)return rc;
    static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){hex[i*2]=digits[digest[i]>>4];hex[i*2+1]=digits[digest[i]&15];}hex[64]=0;return 0;
}

static int object(X4JsonSpan value,const char *const *names,unsigned count,X4JsonSpan *fields)
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
static int u64(X4JsonSpan s,uint64_t *out)
{
    if(x4_json_type(s)!=X4_JSON_T_NUMBER||!s.length)return -1;
    uint64_t n=0;
    for(size_t i=0;i<s.length;++i){unsigned char c=(unsigned char)s.data[i];
        if(c<'0'||c>'9'||n>(UINT64_MAX-(c-'0'))/10)return -1;
        n=n*10+(c-'0');}
    *out=n;return 0;
}
static int i32(X4JsonSpan s,int *out)
{
    if(x4_json_type(s)!=X4_JSON_T_NUMBER||!s.length)return -1;
    bool negative=s.data[0]=='-';X4JsonSpan magnitude={s.data+(negative?1:0),s.length-(negative?1:0)};
    uint64_t n;if(u64(magnitude,&n)||n>(negative?UINT64_C(2147483648):INT_MAX))return -1;
    *out=negative?(n==UINT64_C(2147483648)?INT_MIN:-(int)n):(int)n;return 0;
}
static int natural(X4JsonSpan s,unsigned *out)
{uint64_t n;if(u64(s,&n)||n>UINT_MAX)return -1;*out=(unsigned)n;return 0;}
static int boolean(X4JsonSpan s,bool *out)
{int n;if(x4_json_bool(s,&n))return -1;*out=n!=0;return 0;}
static int floating(X4JsonSpan s,double *out)
{
    if(x4_json_type(s)!=X4_JSON_T_NUMBER||!s.length||s.length>=64)return -1;
    char text[64];memcpy(text,s.data,s.length);text[s.length]=0;char *end=NULL;
    double n=strtod(text,&end);if(end!=text+s.length||!isfinite(n)||n<0)return -1;*out=n;return 0;
}
static int array(X4JsonSpan s,X4JsonSpan *values,unsigned capacity,unsigned *count)
{
    if(x4_json_type(s)!=X4_JSON_T_ARRAY)return -1;
    size_t cursor=0;unsigned n=0;X4JsonSpan value;int rc;
    while((rc=x4_json_item(s,&cursor,&value))>0){if(n>=capacity)return -1;values[n++]=value;}
    if(rc<0)return -1;
    *count=n;return 0;
}
static int exact_array(X4JsonSpan s,X4JsonSpan *values,unsigned count)
{unsigned n;return array(s,values,count,&n)||n!=count?-1:0;}
static int integer_array(X4JsonSpan s,unsigned count)
{
    X4JsonSpan a[24];if(count>24||exact_array(s,a,count))return -1;
    for(unsigned i=0;i<count;++i){X4JsonSpan v=a[i];bool negative=v.length&&v.data[0]=='-';
        if(negative){++v.data;--v.length;}
        uint64_t n;if(u64(v,&n)||(negative&&n>UINT64_C(9223372036854775808)))return -1;}
    return 0;
}
static int rows(X4JsonSpan s,unsigned max,unsigned columns,unsigned *count)
{
    X4JsonSpan a[16];if(max>16)return -1;unsigned n;if(array(s,a,max,&n))return -1;
    for(unsigned i=0;i<n;++i)if(integer_array(a[i],columns))return -1;
    if(count)*count=n;
    return 0;
}
static int read_config(X4JsonSpan s,X4T26Config *c)
{
    static const char *const keys[]={"api","resource_type","depth","output_memory_type","affinity_mask","priority","pipe_id","queue_id","realistic"};
    X4JsonSpan a[9];
    if(object(s,keys,9,a)||natural(a[0],&c->api)||natural(a[1],&c->resource_type)||natural(a[2],&c->depth)||
        i32(a[3],&c->output_memory_type)||u64(a[4],&c->affinity_mask)||i32(a[5],&c->priority)||
        natural(a[6],&c->pipe_id)||natural(a[7],&c->queue_id)||boolean(a[8],&c->realistic)||!x4_t26_config_valid(c))return -1;
    return 0;
}
static int read_stats(X4JsonSpan s,X4T26Stats *out)
{
    memset(out,0,sizeof(*out));if(x4_json_type(s)==X4_JSON_T_NULL)return 0;
    static const char *const keys[]={"n","p50_us","p95_us","p99_us","max_us","mean_us"};
    X4JsonSpan a[6];
    return object(s,keys,6,a)||u64(a[0],&out->n)||!out->n||u64(a[1],&out->p50_us)||u64(a[2],&out->p95_us)||
        u64(a[3],&out->p99_us)||u64(a[4],&out->max_us)||floating(a[5],&out->mean_us)||!stats_valid(out)?-1:0;
}
static int read_cpu(X4JsonSpan s,X4T26CpuDelta *out)
{
    static const char *const keys[]={"wall_us","process","thread"};X4JsonSpan a[3],pair[2];
    if(object(s,keys,3,a)||u64(a[0],&out->wall_us))return -1;
    for(unsigned i=0;i<2;++i){if(x4_json_type(a[i+1])==X4_JSON_T_NULL)continue;
        uint64_t used;double percent;
        if(exact_array(a[i+1],pair,2)||u64(pair[0],&used)||floating(pair[1],&percent)||!out->wall_us||
            fabs(percent-100.0*(double)used/(double)out->wall_us)>0.000001)return -1;
        if(!i){out->process_available=true;out->process_us=used;out->process_percent=percent;}
        else{out->thread_available=true;out->thread_us=used;out->thread_percent=percent;}
    }
    return 0;
}
static int read_backend(X4JsonSpan s,X4T26BackendInfo *b)
{
    static const char *const keys[]={"state","sps","memory","v2","v1","platform"};X4JsonSpan a[6],values[24];
    if(object(s,keys,6,a)||exact_array(a[0],values,9)||integer_array(a[0],9)||integer_array(a[1],11))return -1;
    bool *flags[]={&b->attempted,&b->opened,&b->closed,&b->retained,&b->owned_clean};
    for(unsigned i=0;i<5;++i){uint64_t n;if(u64(values[i],&n)||n>1)return -1;*flags[i]=n!=0;}
    if(i32(values[5],&b->open_result)||i32(values[6],&b->close_result)||i32(values[7],&b->local_result)||natural(values[8],&b->output_count))return -1;
    static const char *const mk[]={"owned","largest","checks"};X4JsonSpan memory[3],owned[4];unsigned checks;
    if(object(a[2],mk,3,memory)||exact_array(memory[0],owned,4)||natural(owned[0],&b->allocations_before)||
        u64(owned[1],&b->owned_bytes_before)||natural(owned[2],&b->allocations_after)||u64(owned[3],&b->owned_bytes_after)||
        b->allocations_before>16||b->allocations_after>16||integer_array(memory[1],6)||rows(memory[2],16,16,&checks)||checks!=b->allocations_before)return -1;
    if(b->owned_clean&&(!b->closed||b->retained||b->close_result||b->allocations_after||b->owned_bytes_after))return -1;
    static const char *const v2k[]={"config","rc","bytes","attempts"};X4JsonSpan v2[4];
    if(object(a[3],v2k,4,v2)||integer_array(v2[0],10)||integer_array(v2[1],7)||integer_array(v2[2],8)||rows(v2[3],6,21,NULL))return -1;
    static const char *const v1k[]={"rc","resource","counts","resolve"};X4JsonSpan v1[4];
    if(object(a[4],v1k,4,v1)||integer_array(v1[0],16)||integer_array(v1[1],11)||integer_array(v1[2],18)||integer_array(v1[3],6))return -1;
    static const char *const pk[]={"resolve","modules","memory","dropped"};X4JsonSpan p[4];unsigned modules,dropped;
    if(object(a[5],pk,4,p)||integer_array(p[0],6)||rows(p[1],2,7,&modules)||modules!=2||rows(p[2],16,7,NULL)||natural(p[3],&dropped))return -1;
    return 0;
}
static int read_hashes(X4JsonSpan s,X4T26HashResult h[3])
{
    X4JsonSpan values[3];if(exact_array(s,values,3))return -1;
    static const char *const keys[]={"image","sha256","compared","matches_base"};
    static const uint64_t ordinal[3]={1,300,600};
    for(unsigned i=0;i<3;++i){if(x4_json_type(values[i])==X4_JSON_T_NULL)continue;X4JsonSpan a[4];
        if(object(values[i],keys,4,a)||u64(a[0],&h[i].measured_ordinal)||h[i].measured_ordinal!=ordinal[i]||
            x4_json_token(a[1],h[i].sha256,65)||!hash_valid(h[i].sha256)||boolean(a[2],&h[i].compared)||boolean(a[3],&h[i].matches_base))return -1;
        h[i].observed=true;
    }
    return 0;
}
static int read_gpu(X4JsonSpan s,X4T26ModeResult *m)
{
    static const char *const keys[]={"requested","gpu_frames","cpu_frames","reason","scope"};
    X4JsonSpan a[5];char scope[64],reason[384];
    if(object(s,keys,5,a)||boolean(a[0],&m->gpu_requested)||u64(a[1],&m->gpu_frames)||u64(a[2],&m->cpu_frames)||
        x4_json_token(a[4],scope,sizeof(scope))||strcmp(scope,"whole_run_warmup_and_measured"))return -1;
    if(x4_json_type(a[3])!=X4_JSON_T_NULL){
        if(x4_json_display_text(a[3],reason,sizeof(reason))||strlen(reason)>=sizeof(m->gpu_reason))return -1;
        memcpy(m->gpu_reason,reason,strlen(reason)+1);
    }
    return 0;
}
static int read_mode(X4JsonSpan s,unsigned index,X4T26ModeResult *m)
{
    if(x4_json_type(s)==X4_JSON_T_NULL)return 0;
    static const char *const keys[]={"mode","completed","accepted","status","native_rc","local_rc","inference",
        "elapsed_us","wall_us","output_span_us","counts","output_fps","interval_fps","decode","au_to_output","output_interval",
        "decode_to_flip","submit_to_observe","call_delay","measured_flush_outputs","held","cpu","hashes","hash_observer_us","realistic_gpu","backend"};
    X4JsonSpan a[26],pair[2],counts[8],delay[4];
    if(object(s,keys,26,a)||natural(a[0],&m->mode)||m->mode!=index||boolean(a[1],&m->completed)||boolean(a[2],&m->accepted)||
        natural(a[3],&m->status)||m->status>X4_T26_RESULT_NOT_RUN||i32(a[4],&m->native_result)||i32(a[5],&m->local_result)||
        exact_array(a[6],pair,2)||u64(a[7],&m->elapsed_us)||u64(a[8],&m->measured_wall_us)||u64(a[9],&m->output_span_us)||
        exact_array(a[10],counts,8))return -1;
    unsigned infer;if(natural(pair[0],&infer)||infer>1||natural(pair[1],&m->invalid_reasons)||m->invalid_reasons>127)return -1;
    m->inference_valid=infer!=0;
    uint64_t *dest[]={&m->measured_calls,&m->measured_outputs,&m->native_calls,&m->decode_calls,&m->no_output_calls,&m->error_outputs,&m->flush_calls,&m->drained_outputs};
    for(unsigned i=0;i<8;++i)if(u64(counts[i],dest[i]))return -1;
    if((!m->native_calls&&m->native_result!=X4_T26_PLATFORM_NOT_RUN)||read_stats(a[13],&m->decode)||read_stats(a[14],&m->au_latency)||
        read_stats(a[15],&m->output_interval)||read_stats(a[16],&m->decode_to_flip)||read_stats(a[17],&m->submit_to_observe)||
        exact_array(a[18],delay,4)||u64(a[19],&m->measured_flush_outputs)||exact_array(a[20],pair,2)||
        read_cpu(a[21],&m->cpu)||read_hashes(a[22],m->hashes)||u64(a[23],&m->hash_observer_us)||read_gpu(a[24],m)||read_backend(a[25],&m->backend))return -1;
    uint64_t held[2];if(u64(pair[0],&held[0])||u64(pair[1],&held[1])||held[0]>31||held[1]>31)return -1;
    m->held_before_close=(uint32_t)held[0];m->held_after_close=(uint32_t)held[1];
    for(unsigned i=0;i<4;++i)if(u64(delay[i],&m->call_delay_hist[i]))return -1;
    for(unsigned i=0;i<2;++i){bool available=i?(m->output_span_us&&m->measured_outputs>1):m->measured_wall_us!=0;
        if(!available){if(x4_json_type(a[11+i])!=X4_JSON_T_NULL)return -1;continue;}
        double fps,expected=i?(double)(m->measured_outputs-1)*1000000.0/(double)m->output_span_us:
            (double)m->measured_outputs*1000000.0/(double)m->measured_wall_us;
        if(floating(a[11+i],&fps)||fabs(fps-expected)>0.000001)return -1;
    }
    m->present=true;return 0;
}
int x4_t26_result_read(const char *text,size_t bytes,const char *expected_build,const char *expected_manifest,X4T26RecoveredResult *out)
{
    if(!text||!bytes||bytes>=X4_T26_RESULT_TEXT_MAX||!out||!id_valid(expected_build)||!hash_valid(expected_manifest))return -1;
    X4JsonSpan root,a[13],modes[2];
    static const char *const keys[]={"schema","build","manifest_sha256","cell","group","clip","factor","status","final","error_code","reason","config","modes"};
    X4T26CellResult *r=calloc(1,sizeof(*r));if(!r)return -2;
    char schema[32],reason[768];int rc=-1;
    if(x4_json_parse(text,bytes,&root)||object(root,keys,13,a)||x4_json_token(a[0],schema,sizeof(schema))||strcmp(schema,"xcloud4-t26-cell-v1")||
        x4_json_token(a[1],r->build_id,65)||strcmp(r->build_id,expected_build)||x4_json_token(a[2],r->manifest_sha256,65)||
        strcmp(r->manifest_sha256,expected_manifest)||natural(a[3],&r->cell)||natural(a[4],&r->group)||natural(a[5],&r->clip)||
        natural(a[6],&r->factor)||natural(a[7],&r->status)||boolean(a[8],&r->final)||i32(a[9],&r->error_code)||
        x4_json_display_text(a[10],reason,sizeof(reason))||strlen(reason)>=sizeof(r->reason)||read_config(a[11],&r->config)||exact_array(a[12],modes,2)||
        read_mode(modes[0],0,&r->modes[0])||read_mode(modes[1],1,&r->modes[1]))goto end;
    memcpy(r->reason,reason,strlen(reason)+1);
    for(unsigned i=0;i<2;++i)r->modes[i].backend.config=r->config;
    if(!valid(r))goto end;
    X4T26RecoveredResult value={.cell=r->cell,.group=r->group,.clip=r->clip,.factor=r->factor,.status=r->status,
        .error_code=r->error_code,.config=r->config};memcpy(value.reason,r->reason,sizeof(value.reason));
    value.outcome.completed=true;bool base=base_cell(r->group,r->factor),pass=r->status==X4_T26_RESULT_PASS&&!r->error_code;
    for(unsigned i=0;i<2;++i){X4T26ModeResult *m=&r->modes[i];
        bool needed=!r->config.realistic||i==X4_T26_MODE_PACED;
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
        if(needed&&!mode_pass(m,base))pass=false;
        if(!needed&&m->present)goto end;
    }
    if(base&&pass){
        for(unsigned j=0;j<3;++j)if(strcmp(value.hashes[0][j],value.hashes[1][j]))pass=false;
        value.base_hashes_valid=pass;
    }
    value.outcome.accepted=pass;value.outcome.hashes_ok=pass;
    *out=value;rc=0;
end:free(r);return rc;
}
#endif
