/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_metrics.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void x4_t26_samples_reset(X4T26Samples *s)
{if(s)s->count=0;}
int x4_t26_samples_add(X4T26Samples *s,uint64_t value)
{
    if(!s||s->count>=X4_T26_METRIC_CAPACITY)return -1;
    s->values[s->count++]=value;return 0;
}
static int ascending(const void *a,const void *b)
{uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x<y?-1:x>y?1:0;}
int x4_t26_samples_stats(const X4T26Samples *s,X4T26Stats *out)
{
    if(!s||!out||s->count>X4_T26_METRIC_CAPACITY)return -1;
    X4T26Stats r={0};unsigned n=s->count;
    if(!n){*out=r;return 0;}
    uint64_t *sorted=malloc((size_t)n*sizeof(*sorted));if(!sorted)return -2;
    memcpy(sorted,s->values,(size_t)n*sizeof(*sorted));qsort(sorted,n,sizeof(*sorted),ascending);
    /* Each quotient is floor(value/n). Their sum is <= UINT64_MAX; each
     * remainder is < n, hence their sum is <1024^2. No integer sum overflows. */
    uint64_t quotient=0,remainder=0;
    for(unsigned i=0;i<n;++i){quotient+=s->values[i]/n;remainder+=s->values[i]%n;}
    r.n=n;r.p50_us=sorted[((uint64_t)n*50+99)/100-1];
    r.p95_us=sorted[((uint64_t)n*95+99)/100-1];r.p99_us=sorted[((uint64_t)n*99+99)/100-1];
    r.max_us=sorted[n-1];r.mean_us=(double)quotient+(double)remainder/(double)n;
    free(sorted);*out=r;return 0;
}
static bool stats_valid(const X4T26Stats *s)
{
    if(!s||s->n>X4_T26_METRIC_CAPACITY||!isfinite(s->mean_us)||s->mean_us<0)return false;
    if(!s->n)return !s->p50_us&&!s->p95_us&&!s->p99_us&&!s->max_us&&s->mean_us==0;
    return s->p50_us<=s->p95_us&&s->p95_us<=s->p99_us&&s->p99_us<=s->max_us&&s->mean_us<=(double)s->max_us;
}
int x4_t26_stats_json(FILE *f,const X4T26Stats *s)
{
    if(!f||!stats_valid(s))return -1;
    int rc;
    if(!s->n)rc=fputs("null",f);
    else rc=fprintf(f,"{\"n\":%" PRIu64 ",\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64
        ",\"p99_us\":%" PRIu64 ",\"max_us\":%" PRIu64 ",\"mean_us\":%.6f}",
        s->n,s->p50_us,s->p95_us,s->p99_us,s->max_us,s->mean_us);
    return rc<0||ferror(f)?-2:0;
}

static void invalidate(X4T26Matcher *m,unsigned reason)
{
    m->invalid_reasons|=reason;
    if(m->inference_valid){
        m->inference_valid=false;m->head=m->pending=0;
        x4_t26_samples_reset(&m->au_latency);x4_t26_samples_reset(&m->output_interval);
        memset(m->call_delay_hist,0,sizeof(m->call_delay_hist));m->measured_outputs=0;
        m->measured_flush_outputs=0;m->have_last_output=false;m->last_output_us=0;
    }
}
void x4_t26_matcher_invalidate(X4T26Matcher *m,unsigned reason)
{
    if(!m)return;
    if(!reason||(reason&~127u))reason=X4_T26_MATCH_LOCAL_CONTRACT;
    invalidate(m,reason);
}
int x4_t26_matcher_reset(X4T26Matcher *m,bool verified,uint64_t first,uint64_t last)
{
    if(!m||!first||last<first||last-first>=X4_T26_METRIC_CAPACITY)return -1;
    memset(m,0,sizeof(*m));m->first_measured=first;m->last_measured=last;
    m->inference_valid=verified;m->invalid_reasons=verified?0:X4_T26_MATCH_NOT_VERIFIED;return 0;
}
int x4_t26_matcher_call(X4T26Matcher *m,const X4T26CallRecord *c,X4T26Association *a)
{
    if(!m||!c||!a)return -1;
    memset(a,0,sizeof(*a));a->has_output=c->output_valid;a->from_flush=c->flushing;
    if(m->pending>X4_T26_METRIC_CAPACITY||m->head>=X4_T26_METRIC_CAPACITY||
        m->au_latency.count>X4_T26_METRIC_CAPACITY||m->output_interval.count>X4_T26_METRIC_CAPACITY){
        invalidate(m,X4_T26_MATCH_CAPACITY);return 1;
    }
    if(c->flushing){
        if(m->flush_calls==UINT64_MAX){invalidate(m,X4_T26_MATCH_ORDER);return 1;}
        ++m->flush_calls;
        if(c->has_au||c->call_index!=m->decode_calls)invalidate(m,X4_T26_MATCH_ORDER);
    }else{
        if(m->decode_calls==UINT64_MAX){invalidate(m,X4_T26_MATCH_ORDER);return 1;}
        ++m->decode_calls;
        if(c->call_index!=m->decode_calls||!c->has_au)invalidate(m,X4_T26_MATCH_ORDER);
        if(!c->output_valid)++m->no_output_calls;
    }
    if(c->has_au&&!c->flushing){
        if(m->submitted==UINT64_MAX){invalidate(m,X4_T26_MATCH_ORDER);return 1;}
        ++m->submitted;
        if(c->au_ordinal!=m->submitted)invalidate(m,X4_T26_MATCH_ORDER);
        if(c->au_begin_us<m->last_au_us||c->output_ready_us<c->au_begin_us)invalidate(m,X4_T26_MATCH_CLOCK);
        m->last_au_us=c->au_begin_us;
    }
    if(c->output_valid){
        if(m->outputs==UINT64_MAX){invalidate(m,X4_T26_MATCH_COUNT);return 1;}
        ++m->outputs;if(c->flushing)++m->drained_outputs;
    }
    if((c->output_valid&&c->picture_count!=1)||(!c->output_valid&&c->picture_count))invalidate(m,X4_T26_MATCH_COUNT);
    if(c->native_rc||c->is_error_frame)invalidate(m,X4_T26_MATCH_NATIVE_ERROR);
    if(!m->inference_valid)return 1;
    if(c->has_au){
        if(m->pending==X4_T26_METRIC_CAPACITY){invalidate(m,X4_T26_MATCH_CAPACITY);return 1;}
        unsigned slot=(m->head+m->pending)%X4_T26_METRIC_CAPACITY;
        m->ordinal[slot]=c->au_ordinal;m->began_us[slot]=c->au_begin_us;m->call[slot]=c->call_index;++m->pending;
    }
    if(!c->output_valid)return 0;
    if(!m->pending){invalidate(m,X4_T26_MATCH_COUNT);return 1;}
    unsigned slot=m->head;uint64_t ordinal=m->ordinal[slot],began=m->began_us[slot],call=m->call[slot];
    if(c->output_ready_us<began||(!c->flushing&&c->call_index<call)){
        invalidate(m,X4_T26_MATCH_CLOCK);return 1;
    }
    m->head=(m->head+1)%X4_T26_METRIC_CAPACITY;--m->pending;
    a->inferred=true;a->au_ordinal=ordinal;a->submitted_call=call;
    a->returned_call=c->flushing?0:c->call_index;
    a->call_delay=c->flushing?0:c->call_index-call;a->latency_us=c->output_ready_us-began;
    if(ordinal>=m->first_measured&&ordinal<=m->last_measured){
        if(x4_t26_samples_add(&m->au_latency,a->latency_us)){
            invalidate(m,X4_T26_MATCH_CAPACITY);a->inferred=false;return 1;
        }
        if(m->have_last_output){
            if(c->output_ready_us<m->last_output_us){invalidate(m,X4_T26_MATCH_CLOCK);a->inferred=false;return 1;}
            if(x4_t26_samples_add(&m->output_interval,c->output_ready_us-m->last_output_us)){
                invalidate(m,X4_T26_MATCH_CAPACITY);a->inferred=false;return 1;
            }
        }
        m->have_last_output=true;m->last_output_us=c->output_ready_us;++m->measured_outputs;
        if(c->flushing)++m->measured_flush_outputs;
        else ++m->call_delay_hist[a->call_delay<3?(unsigned)a->call_delay:3];
    }
    return 0;
}
bool x4_t26_matcher_finish(X4T26Matcher *m,uint64_t inputs,uint64_t outputs)
{
    if(!m)return false;
    if(!m->inference_valid)return false;
    uint64_t expected=m->last_measured-m->first_measured+1;
    if(m->submitted!=inputs||m->outputs!=outputs||inputs!=outputs||m->pending||
        m->measured_outputs!=expected||m->au_latency.count!=expected||
        m->output_interval.count!=(expected?expected-1:0)){
        invalidate(m,X4_T26_MATCH_COUNT);return false;
    }
    return true;
}
#endif
