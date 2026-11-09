/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_diagnostics.h"
#include "../streaming/stream_profile.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t bins[X4_STREAM_METRIC_BINS], count, sum, maximum, overflow;
} Stage;
typedef struct { uint64_t ordinal, begin_us, end_us; unsigned evidence, classification; } Pause;
enum { VSYNC_HISTORY=1024, VSYNC_EVENTS=64 };
typedef struct { Stage timing[3];uint64_t frames,first_us,last_us; } PresentationMode;
typedef struct {
    unsigned policy,near30_streak,near60_streak;
    bool have_bucket;
    uint64_t bucket,current_count,complete_buckets,zero_buckets,other_buckets,late_events;
    uint64_t state_since_us,state_us[2],transitions,history_omitted,notification_omitted;
    X4StreamVsyncTransition history[VSYNC_HISTORY],events[VSYNC_EVENTS];
    atomic_bool effective;
    atomic_uint_fast64_t written,read;
} VsyncPolicy;
enum { PACING_MS_BINS=32, PACING_READY_BINS=5, PACING_DRIVER_BINS=9 };
typedef struct {
    Stage ready,interval;
    uint64_t ready_ms[PACING_MS_BINS];
    uint64_t ready_depth[PACING_READY_BINS],driver_depth[PACING_DRIVER_BINS],gpu_depth[PACING_DRIVER_BINS];
    uint64_t mode_frames[X4_PRESENT_METRIC_MODE_COUNT];
    uint64_t frames,first_us,last_us,last_vsync_ptime,over20,interval_invalid;
    uint64_t native_times,fallback_times,ready_invalid,repeat_estimate,repeat_intervals,repeat_unknown;
    uint64_t queue_samples,queue_invalid,recovery_drops,mailbox_drops,ring_full_drops,ring_latest_drops;
    uint64_t next_log_us,log_omitted;
    unsigned ready_max,driver_max,gpu_max;
    bool final_logged;
    unsigned last_mode;
    uint64_t cola_vsync_wait_drops;
} Pacing;
struct X4StreamDiagnostics {
    uint64_t started, first_present, last_present, presentations;
    uint64_t intervals_over_100ms, longest_interval;
    Stage stage[X4_STREAM_STAGE_COUNT];
    atomic_uint_fast64_t first_receive, last_receive, received_bytes, received_packets;
    atomic_uint_fast64_t seconds[X4_STREAM_BITRATE_SECONDS];
    atomic_uint_fast64_t bitrate_omitted_packets, bitrate_omitted_bytes;
    uint16_t au_seconds[X4_STREAM_BITRATE_SECONDS],present_seconds[X4_STREAM_BITRATE_SECONDS];
    uint16_t idr_seconds[X4_STREAM_BITRATE_SECONDS];
    uint64_t au_count,au_omitted,present_bucket_omitted,idr_bucket_omitted;
    /* Auth-grant observer producer, assembly consumer. No allocation/wait. */
    uint64_t requests[X4_STREAM_KEYFRAME_REQUESTS];
    atomic_uint_fast64_t request_written,request_read,request_total,request_omitted;
    uint32_t idr_latency_bins[X4_STREAM_IDR_LATENCY_BINS];
    uint64_t idr_latency_count,idr_latency_max,idr_latency_overflow,observed_idrs,last_idr_us,requests_ambiguous;
    atomic_uint interval_evidence;
    uint64_t pause_counts[6],pause_omitted;
    Pause pauses[X4_STREAM_PAUSE_RECORDS];
    /* Reserved once; export is single-owner after recording quiesces. */
    uint64_t sort[X4_STREAM_BITRATE_SECONDS];
    VsyncPolicy vsync;
    PresentationMode presentation[X4_PRESENT_MODE_COUNT];
    Stage slot_wait,ring_gate_wait;
    uint64_t ownership_samples,held_native[6],occupied_ring[6],ring_full_drops;
    /* Append only: decoder-owner counter offsets remain byte-identical to T34. */
    PresentationMode synced_presentation;
    Pacing pacing;
    atomic_uint_fast64_t pacing_received_aus;
};
static PresentationMode *presentation_mode(X4StreamDiagnostics *d,unsigned mode)
{return mode==X4_PRESENT_VSYNC_SYNC?&d->synced_presentation:&d->presentation[mode];}
static const PresentationMode *presentation_mode_const(const X4StreamDiagnostics *d,unsigned mode)
{return mode==X4_PRESENT_VSYNC_SYNC?&d->synced_presentation:&d->presentation[mode];}
static const char *const names[X4_STREAM_STAGE_COUNT] = {
    "decode", "conversion", "stripe0", "stripe1", "decode_end_to_flip_observation",
    "conversion_end_to_submit", "flip_call", "submit_return_to_flip_observation",
    "new_presentation_interval", "decode_IDR", "decode_non_IDR",
    "flip_end_to_conversion_begin", "controller_read_interval",
    "decode_with_output", "decode_without_output", "native_output_interval",
    "decoder_slot_acquisition_wait", "decoder_ring_gate_wait"
};
X4StreamDiagnostics *x4_stream_diagnostics_create(uint64_t now)
{
    X4StreamDiagnostics *d = calloc(1, sizeof(*d)); if (!d) return NULL;
    d->started = now; atomic_init(&d->first_receive, 0); atomic_init(&d->last_receive, 0);
    atomic_init(&d->received_bytes, 0); atomic_init(&d->received_packets, 0);
    atomic_init(&d->bitrate_omitted_packets, 0); atomic_init(&d->bitrate_omitted_bytes, 0);
    atomic_init(&d->request_written,0);atomic_init(&d->request_read,0);
    atomic_init(&d->request_total,0);atomic_init(&d->request_omitted,0);
    atomic_init(&d->interval_evidence,0);
    atomic_init(&d->pacing_received_aus,0);
    for (unsigned i=0;i<X4_STREAM_BITRATE_SECONDS;++i) atomic_init(&d->seconds[i],0);
    x4_stream_diagnostics_vsync_init(d,X4_VSYNC_AUTO);
    return d;
}
void x4_stream_diagnostics_free(X4StreamDiagnostics *d) { free(d); }
size_t x4_stream_diagnostics_storage_bytes(void) { return sizeof(X4StreamDiagnostics); }
static void record_stage(Stage *s,uint64_t us)
{
    uint64_t b=us/X4_STREAM_METRIC_QUANTUM_US;
    if (b>=X4_STREAM_METRIC_BINS-1) {b=X4_STREAM_METRIC_BINS-1;++s->overflow;}
    ++s->bins[b]; ++s->count; s->sum+=us; if(us>s->maximum)s->maximum=us;
}
void x4_stream_diagnostics_record(X4StreamDiagnostics *d,X4StreamStage id,uint64_t us)
{if(d&&(unsigned)id<X4_STREAM_STAGE_COUNT)record_stage(&d->stage[id],us);}
static bool initial_vsync(unsigned policy)
{return policy!=X4_VSYNC_NEVER&&policy!=X4_VSYNC_AUTO_T27;}
void x4_stream_diagnostics_vsync_init(X4StreamDiagnostics *d,unsigned policy)
{
    if(!d)return;memset(&d->vsync,0,sizeof(d->vsync));
    d->vsync.policy=policy<=X4_VSYNC_AUTO_T27?policy:X4_VSYNC_AUTO;
    d->vsync.state_since_us=d->started;
    atomic_init(&d->vsync.effective,initial_vsync(d->vsync.policy));
    atomic_init(&d->vsync.written,0);atomic_init(&d->vsync.read,0);
}
bool x4_stream_diagnostics_vsync_effective(const X4StreamDiagnostics *d)
{return d&&atomic_load_explicit(&d->vsync.effective,memory_order_acquire);}
static void vsync_complete(X4StreamDiagnostics *d,uint64_t second,uint64_t count,uint64_t at)
{
    VsyncPolicy *v=&d->vsync;++v->complete_buckets;
    if(count>=27&&count<=33){if(v->near30_streak<3)++v->near30_streak;v->near60_streak=0;}
    else if(count>=55&&count<=65){if(v->near60_streak<3)++v->near60_streak;v->near30_streak=0;}
    else {if(!count)++v->zero_buckets;else ++v->other_buckets;return;}
    if(v->policy!=X4_VSYNC_AUTO_T27)return;
    bool previous=atomic_load_explicit(&v->effective,memory_order_relaxed),next=previous;
    if(!previous&&v->near30_streak==3)next=true;
    else if(previous&&v->near60_streak==3)next=false;
    if(next==previous)return;
    v->state_us[previous?1:0]+=at-v->state_since_us;v->state_since_us=at;
    X4StreamVsyncTransition event={++v->transitions,at,second,count,previous,next};
    if(event.ordinal<=VSYNC_HISTORY)v->history[event.ordinal-1]=event;else ++v->history_omitted;
    uint64_t written=atomic_load_explicit(&v->written,memory_order_relaxed);
    uint64_t read=atomic_load_explicit(&v->read,memory_order_acquire);
    if(written-read<VSYNC_EVENTS){v->events[written%VSYNC_EVENTS]=event;
        atomic_store_explicit(&v->written,written+1,memory_order_release);}
    else ++v->notification_omitted;
    atomic_store_explicit(&v->effective,next,memory_order_release);
}
static void vsync_au(X4StreamDiagnostics *d,uint64_t second,uint64_t observed_us)
{
    VsyncPolicy *v=&d->vsync;if(second==UINT64_MAX)return;
    if(!v->have_bucket){v->have_bucket=true;v->bucket=second;v->zero_buckets+=second;v->complete_buckets+=second;}
    if(second<v->bucket){++v->late_events;return;}
    if(second>v->bucket){
        /* The same sole assembly producer has now finished the preceding
         * existing AU bucket. No reader races its plain per-second array. */
        uint64_t count=v->bucket<X4_STREAM_BITRATE_SECONDS?d->au_seconds[v->bucket]:v->current_count;
        vsync_complete(d,v->bucket,count,observed_us);
        uint64_t zeros=second-v->bucket-1;v->zero_buckets+=zeros;v->complete_buckets+=zeros;
        v->bucket=second;v->current_count=0;
    }
    if(v->current_count!=UINT64_MAX)++v->current_count;
}
bool x4_stream_diagnostics_vsync_take_transition(X4StreamDiagnostics *d,X4StreamVsyncTransition *out)
{
    if(!d||!out)return false;VsyncPolicy *v=&d->vsync;
    uint64_t read=atomic_load_explicit(&v->read,memory_order_relaxed);
    uint64_t written=atomic_load_explicit(&v->written,memory_order_acquire);
    if(read==written)return false;*out=v->events[read%VSYNC_EVENTS];
    atomic_store_explicit(&v->read,read+1,memory_order_release);return true;
}
void x4_stream_diagnostics_presentation(X4StreamDiagnostics *d,unsigned mode,
    uint64_t decoded,uint64_t submitted,uint64_t now)
{
    if(!d||mode>=X4_PRESENT_METRIC_MODE_COUNT)return;PresentationMode *m=presentation_mode(d,mode);
    record_stage(&m->timing[0],decoded);record_stage(&m->timing[1],submitted);
    if(!m->frames)m->first_us=now;m->last_us=now;++m->frames;
}
void x4_stream_diagnostics_flip_to_conversion(X4StreamDiagnostics *d,unsigned mode,uint64_t us)
{if(d&&mode<X4_PRESENT_METRIC_MODE_COUNT)record_stage(&presentation_mode(d,mode)->timing[2],us);}
void x4_stream_diagnostics_pacing_observe(X4StreamDiagnostics *d,unsigned mode,
    uint64_t ready,uint64_t ptime,uint64_t observed,uint64_t period)
{
    if(!d||mode>=X4_PRESENT_METRIC_MODE_COUNT)return;
    Pacing *p=&d->pacing;
    bool native=ptime&&ptime>=d->started&&ptime<=observed&&(!ready||ptime>=ready);
    uint64_t event=native?ptime:observed;
    if(!ready||event<ready)++p->ready_invalid;
    else {
        uint64_t wait=event-ready;
        record_stage(&p->ready,wait);
        uint64_t bin=wait/1000u;
        ++p->ready_ms[bin<PACING_MS_BINS-1?bin:PACING_MS_BINS-1];
    }
    if(native)++p->native_times;else ++p->fallback_times;
    if(p->frames){
        if(event>p->last_us){uint64_t interval=event-p->last_us;
            record_stage(&p->interval,interval);if(interval>20000u)++p->over20;
        }else ++p->interval_invalid;
    }else p->first_us=event;
    if(event>=p->last_us)p->last_us=event;
    ++p->frames;++p->mode_frames[mode];p->last_mode=mode;
    if(native&&mode!=X4_PRESENT_HSYNC&&period>=15000u&&period<=18000u){
        if(p->last_vsync_ptime&&ptime>p->last_vsync_ptime){
            uint64_t delta=ptime-p->last_vsync_ptime;
            uint64_t cycles=delta/period+(delta%period>=period/2u);
            uint64_t expected=cycles<=UINT64_MAX/period?cycles*period:0;
            uint64_t residual=delta>expected?delta-expected:expected-delta;
            if(cycles&&expected&&residual<=2000u){
                p->repeat_estimate+=cycles-1u;++p->repeat_intervals;
            }else ++p->repeat_unknown;
        }
        p->last_vsync_ptime=ptime;
    }else {p->last_vsync_ptime=0;++p->repeat_unknown;}
}
void x4_stream_diagnostics_pacing_queue(X4StreamDiagnostics *d,unsigned ready,int driver,int gpu)
{
    if(!d)return;Pacing *p=&d->pacing;
    ++p->queue_samples;
    ++p->ready_depth[ready<PACING_READY_BINS-1?ready:PACING_READY_BINS-1];
    if(ready>p->ready_max)p->ready_max=ready;
    if(driver<0||gpu<0){++p->queue_invalid;return;}
    unsigned a=(unsigned)driver,b=(unsigned)gpu;
    ++p->driver_depth[a<PACING_DRIVER_BINS-1?a:PACING_DRIVER_BINS-1];
    ++p->gpu_depth[b<PACING_DRIVER_BINS-1?b:PACING_DRIVER_BINS-1];
    if(a>p->driver_max)p->driver_max=a;if(b>p->gpu_max)p->gpu_max=b;
}
void x4_stream_diagnostics_pacing_drops(X4StreamDiagnostics *d,uint64_t recovery,
    uint64_t mailbox,uint64_t ring_full,uint64_t ring_latest,uint64_t cola_vsync_wait)
{
    if(!d)return;Pacing *p=&d->pacing;
    p->recovery_drops=recovery;p->mailbox_drops=mailbox;
    p->ring_full_drops=ring_full;p->ring_latest_drops=ring_latest;
    p->cola_vsync_wait_drops=cola_vsync_wait;
}
void x4_stream_diagnostics_decoder_ownership(X4StreamDiagnostics *d,uint64_t slot,uint64_t gate,
    unsigned held,unsigned occupied,uint64_t drops)
{
    if(!d)return;record_stage(&d->slot_wait,slot);record_stage(&d->ring_gate_wait,gate);
    ++d->ownership_samples;++d->held_native[held>5?5:held];++d->occupied_ring[occupied>5?5:occupied];
    d->ring_full_drops+=drops;
}
void x4_stream_diagnostics_received(X4StreamDiagnostics *d,size_t bytes,uint64_t now)
{
    if(!d || !bytes || now<d->started) return;
    uint64_t first=0;
    (void)atomic_compare_exchange_strong_explicit(&d->first_receive,&first,now,memory_order_relaxed,memory_order_relaxed);
    uint64_t last=atomic_load_explicit(&d->last_receive,memory_order_relaxed);
    /* No retry loop in the callback. A contended update may understate the last
     * timestamp; bitrate uses fixed bucket boundaries rather than this field. */
    if(now>last)(void)atomic_compare_exchange_strong_explicit(&d->last_receive,&last,now,memory_order_relaxed,memory_order_relaxed);
    atomic_fetch_add_explicit(&d->received_bytes,bytes,memory_order_relaxed);
    atomic_fetch_add_explicit(&d->received_packets,1,memory_order_relaxed);
    uint64_t second=(now-d->started)/1000000;
    if(second<X4_STREAM_BITRATE_SECONDS)atomic_fetch_add_explicit(&d->seconds[second],bytes,memory_order_relaxed);
    else {atomic_fetch_add_explicit(&d->bitrate_omitted_packets,1,memory_order_relaxed);atomic_fetch_add_explicit(&d->bitrate_omitted_bytes,bytes,memory_order_relaxed);}
}
void x4_stream_diagnostics_evidence(X4StreamDiagnostics *d,unsigned flags)
{if(d)atomic_fetch_or_explicit(&d->interval_evidence,flags&15u,memory_order_relaxed);}
void x4_stream_diagnostics_keyframe_request(X4StreamDiagnostics *d,uint64_t now)
{
    if(!d)return;
    atomic_fetch_add_explicit(&d->request_total,1,memory_order_relaxed);
    uint64_t sent=atomic_load_explicit(&d->request_written,memory_order_relaxed);
    uint64_t read=atomic_load_explicit(&d->request_read,memory_order_acquire);
    if(now<d->started||sent==UINT64_MAX||sent-read>=X4_STREAM_KEYFRAME_REQUESTS){
        atomic_fetch_add_explicit(&d->request_omitted,1,memory_order_relaxed);return;}
    d->requests[sent%X4_STREAM_KEYFRAME_REQUESTS]=now;
    atomic_store_explicit(&d->request_written,sent+1,memory_order_release);
}
void x4_stream_diagnostics_au_received(X4StreamDiagnostics *d,bool idr,uint64_t now)
{
    if(!d)return;
    atomic_fetch_add_explicit(&d->pacing_received_aus,1,memory_order_relaxed);
    ++d->au_count;
    uint64_t second=now>=d->started?(now-d->started)/1000000:UINT64_MAX;
    if(second<X4_STREAM_BITRATE_SECONDS&&d->au_seconds[second]!=UINT16_MAX)++d->au_seconds[second];
    else ++d->au_omitted;
    vsync_au(d,second,now);
    if(!idr)return;
    ++d->observed_idrs;
    if(second<X4_STREAM_BITRATE_SECONDS&&d->idr_seconds[second]!=UINT16_MAX)++d->idr_seconds[second];
    else ++d->idr_bucket_omitted;
    uint64_t read=atomic_load_explicit(&d->request_read,memory_order_relaxed);
    uint64_t sent=atomic_load_explicit(&d->request_written,memory_order_acquire);
    while(read<sent){
        uint64_t request=d->requests[read%X4_STREAM_KEYFRAME_REQUESTS];if(request>now)break;
        /* Producer publication can race the preceding complete IDR. Such a
         * late record cannot be attributed to this next IDR without inventing
         * an interval. Retire it explicitly as ambiguous instead. */
        if(d->last_idr_us&&request<=d->last_idr_us){++d->requests_ambiguous;++read;continue;}
        uint64_t elapsed=now-request,bin=elapsed/1000;
        ++d->idr_latency_count;if(elapsed>d->idr_latency_max)d->idr_latency_max=elapsed;
        if(bin>=X4_STREAM_IDR_LATENCY_BINS-1){bin=X4_STREAM_IDR_LATENCY_BINS-1;++d->idr_latency_overflow;}
        if(d->idr_latency_bins[bin]!=UINT32_MAX)++d->idr_latency_bins[bin];
        ++read;
    }
    d->last_idr_us=now;
    atomic_store_explicit(&d->request_read,read,memory_order_release);
}
static unsigned pause_class(unsigned flags)
{
    if(!flags)return 0;
    if(flags&(flags-1))return 5;
    return flags==1?1:flags==2?2:flags==4?3:4;
}
void x4_stream_diagnostics_present(X4StreamDiagnostics *d,uint64_t now)
{
    if(!d)return;
    unsigned evidence=atomic_exchange_explicit(&d->interval_evidence,0,memory_order_acq_rel);
    if(d->last_present && now>=d->last_present) {
        uint64_t gap=now-d->last_present;
        x4_stream_diagnostics_record(d,X4_STREAM_PRESENT_INTERVAL,gap);
        if(gap>100000){
            uint64_t ordinal=++d->intervals_over_100ms;
            unsigned classification=pause_class(evidence);++d->pause_counts[classification];
            if(ordinal>X4_STREAM_PAUSE_RECORDS)++d->pause_omitted;
            d->pauses[(ordinal-1)%X4_STREAM_PAUSE_RECORDS]=(Pause){ordinal,d->last_present,now,evidence,classification};
        }
        if(gap>d->longest_interval)d->longest_interval=gap;
    }
    if(!d->presentations)d->first_present=now;
    d->last_present=now; ++d->presentations;
    uint64_t second=now>=d->started?(now-d->started)/1000000:UINT64_MAX;
    if(second<X4_STREAM_BITRATE_SECONDS&&d->present_seconds[second]!=UINT16_MAX)++d->present_seconds[second];
    else ++d->present_bucket_omitted;
}
uint64_t x4_stream_diagnostics_presentations(const X4StreamDiagnostics *d){return d?d->presentations:0;}
uint64_t x4_stream_diagnostics_first_present(const X4StreamDiagnostics *d){return d?d->first_present:0;}
static bool percentile(const Stage *s,unsigned p,uint64_t *upper)
{
    if(!s->count)return false;
    uint64_t wanted=(s->count*p+99)/100, count=0;
    for(unsigned i=0;i<X4_STREAM_METRIC_BINS;++i) {
        count+=s->bins[i];if(count>=wanted) {
            if(i==X4_STREAM_METRIC_BINS-1)return false;
            *upper=(uint64_t)(i+1)*X4_STREAM_METRIC_QUANTUM_US;return true;
        }
    }
    return false;
}
static void summary_stage(const Stage *s,X4PipelineTimingSummary *out)
{
    memset(out,0,sizeof(*out));out->samples=s->count;out->sum_us=s->sum;out->maximum_us=s->maximum;
    if(percentile(s,50,&out->p50_us))out->finite_mask|=1u;
    if(percentile(s,95,&out->p95_us))out->finite_mask|=2u;
    if(percentile(s,99,&out->p99_us))out->finite_mask|=4u;
}
#if !X4_PUBLIC
static unsigned pacing_depth_quantile(const uint64_t *bins,unsigned count,unsigned percent)
{
    uint64_t total=0,seen=0;
    for(unsigned i=0;i<count;++i)total+=bins[i];
    if(!total)return 0;
    uint64_t target=total/100u*percent+(total%100u*percent+99u)/100u;
    for(unsigned i=0;i<count;++i){seen+=bins[i];if(seen>=target)return i;}
    return count-1;
}
static bool pacing_append(char *line,size_t capacity,size_t *used,const char *format,...)
{
    if(*used>=capacity)return false;
    va_list args;va_start(args,format);
    int n=vsnprintf(line+*used,capacity-*used,format,args);va_end(args);
    if(n<0||(size_t)n>=capacity-*used)return false;
    *used+=(size_t)n;return true;
}
#endif
void x4_stream_diagnostics_pacing_log(X4StreamDiagnostics *d,uint64_t now,bool final)
{
#if !X4_PUBLIC
    if(!d)return;Pacing *p=&d->pacing;
    if(p->final_logged)return;
    if(!final){
        uint64_t due=p->next_log_us?p->next_log_us:d->started+10000000u;
        if(now<due)return;
        p->next_log_us=now>UINT64_MAX-10000000u?UINT64_MAX:now+10000000u;
    }else p->final_logged=true;
    const char *const modes[]={"HSYNC","VSYNC_Cola","VSYNC_Mailbox","VSYNC_Synced"};
    const char *mode=p->frames?modes[p->last_mode]:"none";
    const char *effective=x4_stream_diagnostics_vsync_effective(d)?"VSYNC":"HSYNC";
    X4PipelineTimingSummary ready,interval,decode;
    summary_stage(&p->ready,&ready);summary_stage(&p->interval,&interval);
    summary_stage(&presentation_mode_const(d,p->last_mode)->timing[0],&decode);
    uint64_t received=atomic_load_explicit(&d->pacing_received_aus,memory_order_relaxed);
    double minutes=p->last_us>p->first_us?(double)(p->last_us-p->first_us)/60000000.:0.;
    char line[1000];size_t used=0;
    bool ok=pacing_append(line,sizeof(line),&used,
        "XCloud4 present %s policy=%u mode=%s effective=%s decode_flip_us=%llu/%llu decode_finite=%u "
        "ready_us=%llu/%llu/%llu ready_finite=%u "
        "interval_us=%llu/%llu interval_finite=%u shown=%llu aus=%llu "
        "q50_95_max=%u/%u/%u driver50_95_max=%u/%u/%u gpu_max=%u q_invalid=%llu drop=%llu/%llu/%llu/%llu "
        "cola_vsync_wait_drop=%llu repeat_est=%llu repeat_unknown=%llu over20=%llu over20_min=%.3f "
        "ptime=%llu fallback=%llu hist1ms=",
        final?"final":"10s",d->vsync.policy,mode,effective,
        (unsigned long long)decode.p50_us,(unsigned long long)decode.p95_us,decode.finite_mask&3u,
        (unsigned long long)ready.p50_us,(unsigned long long)ready.p95_us,(unsigned long long)ready.p99_us,ready.finite_mask,
        (unsigned long long)interval.p95_us,(unsigned long long)interval.p99_us,interval.finite_mask,
        (unsigned long long)p->frames,(unsigned long long)received,
        pacing_depth_quantile(p->ready_depth,PACING_READY_BINS,50),
        pacing_depth_quantile(p->ready_depth,PACING_READY_BINS,95),p->ready_max,
        pacing_depth_quantile(p->driver_depth,PACING_DRIVER_BINS,50),
        pacing_depth_quantile(p->driver_depth,PACING_DRIVER_BINS,95),p->driver_max,
        p->gpu_max,(unsigned long long)p->queue_invalid,
        (unsigned long long)p->recovery_drops,(unsigned long long)p->mailbox_drops,
        (unsigned long long)p->ring_full_drops,(unsigned long long)p->ring_latest_drops,
        (unsigned long long)p->cola_vsync_wait_drops,
        (unsigned long long)p->repeat_estimate,(unsigned long long)p->repeat_unknown,
        (unsigned long long)p->over20,minutes?(double)p->over20/minutes:0.,
        (unsigned long long)p->native_times,(unsigned long long)p->fallback_times);
    for(unsigned i=0;ok&&i<PACING_MS_BINS;++i)
        ok=pacing_append(line,sizeof(line),&used,"%s%llu",i?",":"",(unsigned long long)p->ready_ms[i]);
    if(ok)printf("%s\n",line);
    else {++p->log_omitted;
        printf("XCloud4 present %s policy=%u mode=%s effective=%s decode_flip_us=%llu/%llu decode_finite=%u cola_vsync_wait_drop=%llu repeat_est=%llu log_overflow=1 shown=%llu aus=%llu histogram_in_json=1\n",
            final?"final":"10s",d->vsync.policy,mode,effective,
            (unsigned long long)decode.p50_us,(unsigned long long)decode.p95_us,decode.finite_mask&3u,
            (unsigned long long)p->cola_vsync_wait_drops,(unsigned long long)p->repeat_estimate,
            (unsigned long long)p->frames,(unsigned long long)received);}
#else
    (void)d;(void)now;(void)final;
#endif
}
bool x4_stream_diagnostics_presentation_summary(const X4StreamDiagnostics *d,X4PipelinePresentationSummary *out)
{
    if(!d||!out)return false;memset(out,0,sizeof(*out));
    for(unsigned i=0;i<X4_PRESENT_MODE_COUNT;++i){const PresentationMode *m=&d->presentation[i];
        X4PipelinePresentationModeSummary *s=&out->mode[i];s->frames=m->frames;
        s->first_observed_us=m->first_us;s->last_observed_us=m->last_us;
        summary_stage(&m->timing[0],&s->decode_to_flip);summary_stage(&m->timing[1],&s->submit_to_flip);
        summary_stage(&m->timing[2],&s->flip_to_conversion);}
    summary_stage(&d->slot_wait,&out->decoder_slot_wait);summary_stage(&d->ring_gate_wait,&out->decoder_ring_gate_wait);
    out->decode_ownership_samples=d->ownership_samples;out->ring_full_drops=d->ring_full_drops;
    memcpy(out->held_native_by_count,d->held_native,sizeof(d->held_native));
    memcpy(out->occupied_ring_by_count,d->occupied_ring,sizeof(d->occupied_ring));return true;
}
static void write_summary(FILE *f,const Stage *s)
{
    fprintf(f,"{\"count\":%llu,\"sum_us\":%llu,\"maximum_us\":%llu,\"mean_us\":",
        (unsigned long long)s->count,(unsigned long long)s->sum,(unsigned long long)s->maximum);
    if(s->count)fprintf(f,"%.6f",(double)s->sum/(double)s->count);else fputs("null",f);
    const unsigned ps[]={50,95,99};
    for(unsigned k=0;k<3;++k){uint64_t value=0;fprintf(f,",\"p%u_upper_us\":",ps[k]);
        if(percentile(s,ps[k],&value))fprintf(f,"%llu",(unsigned long long)value);else fputs("null",f);}
    fprintf(f,",\"histogram_quantum_us\":100,\"overflow_count\":%llu}",(unsigned long long)s->overflow);
}
static void write_pacing(FILE *f,const X4StreamDiagnostics *d)
{
    const Pacing *p=&d->pacing;
    uint64_t received=atomic_load_explicit(&d->pacing_received_aus,memory_order_relaxed);
    uint64_t interval=p->last_us>p->first_us?p->last_us-p->first_us:0;
    fputs(",\"presentation_pacing\":{\"scope\":\"real image flips only, excluding UI-only redraws; cumulative session metrics; READY time is recorded after conversion and overlay publication; native ptime is used only when within the same monotonic clock interval, otherwise explicitly counted CPU observation fallback\",\"ready_to_flip_us\":",f);
    write_summary(f,&p->ready);
    fprintf(f,",\"ready_invalid\":%llu,\"ready_histogram_quantum_us\":1000,\"ready_histogram_overflow_lower_us\":31000,\"ready_histogram_counts\":[",
        (unsigned long long)p->ready_invalid);
    for(unsigned i=0;i<PACING_MS_BINS;++i)fprintf(f,"%s%llu",i?",":"",(unsigned long long)p->ready_ms[i]);
    fputs("],\"displayed_image_interval_us\":",f);write_summary(f,&p->interval);
    fprintf(f,",\"interval_invalid\":%llu,\"native_ptime_samples\":%llu,\"observation_fallback_samples\":%llu,\"shown_images\":%llu,\"received_complete_AUs\":%llu,\"shown_per_received_AU\":",
        (unsigned long long)p->interval_invalid,(unsigned long long)p->native_times,
        (unsigned long long)p->fallback_times,(unsigned long long)p->frames,(unsigned long long)received);
    if(received)fprintf(f,"%.6f",(double)p->frames/(double)received);else fputs("null",f);
    fprintf(f,",\"shown_by_mode\":[%llu,%llu,%llu,%llu],\"first_flip_us\":%llu,\"last_flip_us\":%llu,\"intervals_over_20ms\":%llu,\"intervals_over_20ms_per_minute\":",
        (unsigned long long)p->mode_frames[0],(unsigned long long)p->mode_frames[1],
        (unsigned long long)p->mode_frames[2],(unsigned long long)p->mode_frames[3],
        (unsigned long long)p->first_us,(unsigned long long)p->last_us,(unsigned long long)p->over20);
    if(interval)fprintf(f,"%.6f",(double)p->over20*60000000./(double)interval);else fputs("null",f);
    fprintf(f,",\"vblank_repeats\":{\"estimated\":%llu,\"validated_intervals\":%llu,\"unknown_intervals\":%llu,\"scope\":\"estimate from gaps of consecutive real VSYNC flip ptime, rounded to an explicitly valid 15-18ms period with <=2ms residual; unavailable for HSYNC, invalid timestamps, before first or after last flip; not an exact hardware vblank counter\"},\"queue_depths\":{\"samples\":%llu,\"invalid_driver_samples\":%llu,\"scope\":\"existing FlipStatus sampling points, not time-weighted; READY own queue excludes submitted and displayed images; driver flip and GPU pending counts are separate and never added\",\"own_ready_max\":%u,\"driver_pending_max\":%u,\"driver_gpu_pending_max\":%u,\"own_ready_last_bin_lower\":4,\"driver_last_bin_lower\":8,\"own_ready_counts\":[",
        (unsigned long long)p->repeat_estimate,(unsigned long long)p->repeat_intervals,
        (unsigned long long)p->repeat_unknown,(unsigned long long)p->queue_samples,
        (unsigned long long)p->queue_invalid,p->ready_max,p->driver_max,p->gpu_max);
    for(unsigned i=0;i<PACING_READY_BINS;++i)fprintf(f,"%s%llu",i?",":"",(unsigned long long)p->ready_depth[i]);
    fputs("],\"driver_pending_counts\":[",f);
    for(unsigned i=0;i<PACING_DRIVER_BINS;++i)fprintf(f,"%s%llu",i?",":"",(unsigned long long)p->driver_depth[i]);
    fputs("],\"driver_gpu_pending_counts\":[",f);
    for(unsigned i=0;i<PACING_DRIVER_BINS;++i)fprintf(f,"%s%llu",i?",":"",(unsigned long long)p->gpu_depth[i]);
    fprintf(f,"]},\"drops\":{\"recovery\":%llu,\"mailbox\":%llu,\"ring_full\":%llu,\"ring_latest\":%llu},\"cola_vsync_wait_drop\":%llu,\"cola_vsync_wait_scope\":\"Lab only; subset of latest READY discards decoded between submit and observation of the preceding Cola VSYNC flip; excludes ring-full drops and does not infer sole cause\",\"live_log_records_omitted\":%llu}",
        (unsigned long long)p->recovery_drops,(unsigned long long)p->mailbox_drops,
        (unsigned long long)p->ring_full_drops,(unsigned long long)p->ring_latest_drops,
        (unsigned long long)p->cola_vsync_wait_drops,(unsigned long long)p->log_omitted);
}
static void write_presentation(FILE *f,const X4StreamDiagnostics *d,uint64_t ended)
{
    const VsyncPolicy *v=&d->vsync;bool effective=atomic_load_explicit(&v->effective,memory_order_relaxed);
    uint64_t state_us[2]={v->state_us[0],v->state_us[1]};
    if(ended>=v->state_since_us)state_us[effective?1:0]+=ended-v->state_since_us;
    fprintf(f,",\"vsync_policy\":{\"requested\":%u,\"initial_vsync\":%s,\"effective_vsync_at_end\":%s,\"transitions_total\":%llu,\"history_omitted\":%llu,\"live_log_events_omitted\":%llu,\"state_no_us\":%llu,\"state_yes_us\":%llu,\"state_no_seconds\":%.6f,\"state_yes_seconds\":%.6f,\"near_30_streak\":%u,\"near_60_streak\":%u,\"complete_buckets_seen\":%llu,\"zero_buckets_ignored\":%llu,\"other_buckets_ignored\":%llu,\"late_timestamp_events\":%llu,\"scope\":\"same complete-AU counter closed by its sole assembly owner on the next AU; state duration and transition time use actual publication observation, not backdated bucket boundary; no RTP-packet or Decode-output cadence; zero/other preserve both streaks; rolling current bucket continues after the diagnostic one-hour array\",\"transitions\":[",
        v->policy,initial_vsync(v->policy)?"true":"false",effective?"true":"false",
        (unsigned long long)v->transitions,(unsigned long long)v->history_omitted,(unsigned long long)v->notification_omitted,
        (unsigned long long)state_us[0],(unsigned long long)state_us[1],(double)state_us[0]/1000000.,(double)state_us[1]/1000000.,
        v->near30_streak,v->near60_streak,(unsigned long long)v->complete_buckets,
        (unsigned long long)v->zero_buckets,(unsigned long long)v->other_buckets,(unsigned long long)v->late_events);
    uint64_t n=v->transitions<VSYNC_HISTORY?v->transitions:VSYNC_HISTORY;
    for(uint64_t i=0;i<n;++i){const X4StreamVsyncTransition *t=&v->history[i];
        fprintf(f,"%s{\"ordinal\":%llu,\"at_us\":%llu,\"complete_second\":%llu,\"complete_AUs\":%llu,\"previous_vsync\":%s,\"vsync\":%s}",i?",":"",
            (unsigned long long)t->ordinal,(unsigned long long)t->at_us,(unsigned long long)t->complete_second,
            (unsigned long long)t->au_count,t->previous_vsync?"true":"false",t->vsync?"true":"false");}
    fputs("]},\"presentation_modes\":{",f);
    const char *const mode_names[]={"HSYNC","VSYNC_Cola","VSYNC_Mailbox","VSYNC_Synced"};
    for(unsigned i=0;i<X4_PRESENT_METRIC_MODE_COUNT;++i){const PresentationMode *m=presentation_mode_const(d,i);
        fprintf(f,"%s\"%s\":{\"frames\":%llu,\"first_observed_us\":%llu,\"last_observed_us\":%llu,\"fps\":",i?",":"",mode_names[i],
            (unsigned long long)m->frames,(unsigned long long)m->first_us,(unsigned long long)m->last_us);
        if(m->frames>1&&m->last_us>m->first_us)fprintf(f,"%.6f",(double)(m->frames-1)*1000000./(double)(m->last_us-m->first_us));else fputs("null",f);
        fputs(",\"decode_end_to_flip_observation\":",f);write_summary(f,&m->timing[0]);
        fputs(",\"submit_return_to_flip_observation\":",f);write_summary(f,&m->timing[1]);
        fputs(",\"flip_end_to_conversion_begin\":",f);write_summary(f,&m->timing[2]);fputc('}',f);}
    fprintf(f,"},\"decoder_ownership\":{\"samples\":%llu,\"scope\":\"Decode owner before each call associated with the returned valid output; histograms include zero wait; probe measurement windows filter by returned output id\",\"slot_acquisition_wait\":",(unsigned long long)d->ownership_samples);
    write_summary(f,&d->slot_wait);fputs(",\"ring_gate_wait\":",f);write_summary(f,&d->ring_gate_wait);
    fputs(",\"held_native_by_count\":[",f);for(unsigned i=0;i<6;++i)fprintf(f,"%s%llu",i?",":"",(unsigned long long)d->held_native[i]);
    fputs("],\"occupied_ring_by_count\":[",f);for(unsigned i=0;i<6;++i)fprintf(f,"%s%llu",i?",":"",(unsigned long long)d->occupied_ring[i]);
    fprintf(f,"],\"ring_full_drops\":%llu}",(unsigned long long)d->ring_full_drops);
}
static int compare_u64(const void *a,const void *b)
{ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y); }
static const char *const pause_names[]={"unknown","rtp_gap","waiting_idr","decoder","presentation","mixed"};
static void write_cadence(FILE *f,const uint16_t *seconds,uint64_t complete,uint64_t duration,uint64_t total,uint64_t omitted)
{
    uint64_t zero=0,near30=0,near60=0,other=0;
    for(unsigned i=0;i<complete;++i){unsigned n=seconds[i];if(!n)++zero;else if(n>=27&&n<=33)++near30;else if(n>=55&&n<=65)++near60;else ++other;}
    fprintf(f,"{\"total\":%llu,\"omitted_events\":%llu,\"complete_seconds\":%llu,\"zero_seconds\":%llu,\"near_30_seconds\":%llu,\"near_60_seconds\":%llu,\"other_seconds\":%llu,\"near_30_bounds\":[27,33],\"near_60_bounds\":[55,65],\"counts_by_complete_second\":[",
        (unsigned long long)total,(unsigned long long)omitted,(unsigned long long)complete,(unsigned long long)zero,(unsigned long long)near30,(unsigned long long)near60,(unsigned long long)other);
    for(unsigned i=0;i<complete;++i)fprintf(f,"%s%u",i?",":"",seconds[i]);
    uint64_t trailing=duration/1000000;
    fprintf(f,"],\"trailing_partial_duration_us\":%llu,\"trailing_partial_count\":",(unsigned long long)(duration%1000000));
    if(trailing<X4_STREAM_BITRATE_SECONDS)fprintf(f,"%u",seconds[trailing]);else fputs("null",f);
    fprintf(f,",\"seconds_beyond_capacity\":%llu}",(unsigned long long)(trailing>X4_STREAM_BITRATE_SECONDS?trailing-X4_STREAM_BITRATE_SECONDS:0));
}
static void write_new_diagnostics(FILE *f,const X4StreamDiagnostics *d,uint64_t ended,uint64_t complete)
{
    uint64_t duration=ended-d->started;
    fputs(",\"cadence\":{\"bucket_origin_us\":",f);fprintf(f,"%llu",(unsigned long long)d->started);
    fputs(",\"scope\":\"complete one-second buckets from pipeline creation; initialization/loading zeros included; trailing partial excluded from histogram; no claim about game-generated cadence\",\"received_complete_AU\":",f);
    write_cadence(f,d->au_seconds,complete,duration,d->au_count,d->au_omitted);
    fputs(",\"presented\":",f);write_cadence(f,d->present_seconds,complete,duration,d->presentations,d->present_bucket_omitted);fputc('}',f);
    /* Complete IDRs at the assembly frontier, including deliberately filtered
     * work. Native validated IDRs are a separate recovery observation. */
    fputs(",\"received_idr\":",f);
    write_cadence(f,d->idr_seconds,complete,duration,d->observed_idrs,d->idr_bucket_omitted);
    uint64_t idrs=0;for(unsigned i=0;i<complete;++i)idrs+=d->idr_seconds[i];
    fprintf(f,",\"idr_per_minute\":{\"complete_seconds\":%llu,\"count\":%llu,\"rate\":%.6f,\"scope\":\"complete structurally assembled IDR AUs per complete one-second bucket; pipeline creation origin, loading included; not validated Decode outputs\",\"complete_minute_counts\":[",
        (unsigned long long)complete,(unsigned long long)idrs,complete?(double)idrs*60./(double)complete:0.);
    for(unsigned minute=0;minute<complete/60;++minute){uint64_t n=0;for(unsigned i=0;i<60;++i)n+=d->idr_seconds[minute*60+i];fprintf(f,"%s%llu",minute?",":"",(unsigned long long)n);}
    fputs("]}",f);
    uint64_t wanted=(d->idr_latency_count+1)/2,seen=0,median=0;bool finite=false;
    for(unsigned i=0;d->idr_latency_count&&i<X4_STREAM_IDR_LATENCY_BINS;++i){seen+=d->idr_latency_bins[i];if(seen>=wanted){finite=i<X4_STREAM_IDR_LATENCY_BINS-1;median=(uint64_t)(i+1)*1000;break;}}
    uint64_t written=atomic_load(&d->request_written),read=atomic_load(&d->request_read);
    fprintf(f,",\"keyframe_to_idr\":{\"scope\":\"common limiter logical grant before local transport attempts to next complete IDR assembled before filtering; multiple grants may share an IDR; no remote receipt or send causality\",\"requests\":%llu,\"n\":%llu,\"p50_upper_us\":",(unsigned long long)atomic_load(&d->request_total),(unsigned long long)d->idr_latency_count);
    if(finite)fprintf(f,"%llu",(unsigned long long)median);else fputs("null",f);
    fprintf(f,",\"maximum_us\":%llu,\"histogram_quantum_us\":1000,\"overflow_count\":%llu,\"observed_complete_idrs\":%llu,\"requests_pending\":%llu,\"requests_omitted\":%llu,\"requests_ambiguous_publication\":%llu,\"oldest_pending_age_us\":",
        (unsigned long long)d->idr_latency_max,(unsigned long long)d->idr_latency_overflow,(unsigned long long)d->observed_idrs,(unsigned long long)(written-read),(unsigned long long)atomic_load(&d->request_omitted),(unsigned long long)d->requests_ambiguous);
    if(written>read){uint64_t at=d->requests[read%X4_STREAM_KEYFRAME_REQUESTS];fprintf(f,"%llu",(unsigned long long)(ended>=at?ended-at:0));}else fputs("null",f);fputc('}',f);
    fprintf(f,",\"pause_evidence\":{\"scope\":\"temporal evidence since preceding observed flip, not exclusive causal attribution; rtp_gap means local reorder-gap abandonment or deliberate RTP omission, never proof of network loss; decoder means a Decode call over 100ms, presentation means submit-to-observation over 100ms; unknown and mixed remain explicit\",\"capacity\":%u,\"record_count\":%llu,\"omitted_records\":%llu,\"counts\":{",X4_STREAM_PAUSE_RECORDS,(unsigned long long)d->intervals_over_100ms,(unsigned long long)d->pause_omitted);
    for(unsigned i=0;i<6;++i)fprintf(f,"%s\"%s\":%llu",i?",":"",pause_names[i],(unsigned long long)d->pause_counts[i]);
    fputs("},\"records\":[",f);uint64_t first=d->intervals_over_100ms>X4_STREAM_PAUSE_RECORDS?d->intervals_over_100ms-X4_STREAM_PAUSE_RECORDS:0;
    for(uint64_t i=first;i<d->intervals_over_100ms;++i){const Pause *p=&d->pauses[i%X4_STREAM_PAUSE_RECORDS];fprintf(f,"%s{\"ordinal\":%llu,\"begin_us\":%llu,\"end_us\":%llu,\"duration_us\":%llu,\"evidence_mask\":%u,\"classification\":\"%s\"}",i==first?"":",",(unsigned long long)p->ordinal,(unsigned long long)p->begin_us,(unsigned long long)p->end_us,(unsigned long long)(p->end_us-p->begin_us),p->evidence,pause_names[p->classification]);}
    uint64_t open=d->last_present&&ended>d->last_present?ended-d->last_present:0;unsigned flags=atomic_load(&d->interval_evidence);
    fprintf(f,"],\"terminal_open_interval\":{\"duration_us\":%llu,\"over_100ms\":%s,\"evidence_mask\":%u,\"classification\":\"%s\",\"scope\":\"no following flip; may include intentional close time; excluded from closed interval counts\"}}",(unsigned long long)open,open>100000?"true":"false",flags,pause_names[pause_class(flags)]);
}
int x4_stream_diagnostics_write(FILE *f,const X4StreamDiagnostics *constant,uint64_t ended)
{
    if(!f || !constant || ended<constant->started)return -1;
    X4StreamDiagnostics *d=(X4StreamDiagnostics*)constant;
    uint64_t duration=ended-d->started, complete=duration/1000000;
    if(complete>X4_STREAM_BITRATE_SECONDS)complete=X4_STREAM_BITRATE_SECONDS;
    uint64_t sum=0;for(unsigned i=0;i<complete;++i){d->sort[i]=atomic_load(&d->seconds[i]);sum+=d->sort[i];}
    qsort(d->sort,(size_t)complete,sizeof(uint64_t),compare_u64);
    uint64_t p95=complete?d->sort[(complete*95+99)/100-1]*8:0;
    fprintf(f,"{\"storage_bytes\":%llu,\"started_us\":%llu,\"ended_us\":%llu,\"first_present_us\":%llu,\"last_present_us\":%llu,\"presentations\":%llu,",
        (unsigned long long)sizeof(*d),(unsigned long long)d->started,(unsigned long long)ended,(unsigned long long)d->first_present,(unsigned long long)d->last_present,(unsigned long long)d->presentations);
    uint64_t interval=d->presentations>1&&d->last_present>=d->first_present?d->last_present-d->first_present:0;
    fprintf(f,"\"presentation_interval_us\":%llu,\"images_per_second\":%.6f,\"rate_scope\":\"(observed presentations minus one)/(last minus first observed flip); excludes before-first/after-last time, includes game loading/transitions within; local output identity only\",\"intervals_over_100ms\":%llu,\"longest_interval_us\":%llu,",
        (unsigned long long)interval,interval?(double)(d->presentations-1)*1e6/(double)interval:0.,(unsigned long long)d->intervals_over_100ms,(unsigned long long)d->longest_interval);
    fprintf(f,"\"received_video_rtp\":{\"packets\":%llu,\"bytes\":%llu,\"bucket_origin_us\":%llu,\"complete_seconds\":%llu,\"mean_bps\":%.3f,\"p95_bps\":%llu,\"scope\":\"complete one-second callback-RTP buckets from session start, zeros included; trailing partial second excluded\",\"omitted_packets\":%llu,\"omitted_bytes\":%llu},\"stages\":{",
        (unsigned long long)atomic_load(&d->received_packets),(unsigned long long)atomic_load(&d->received_bytes),(unsigned long long)d->started,(unsigned long long)complete,complete?(double)sum*8/(double)complete:0.,(unsigned long long)p95,(unsigned long long)atomic_load(&d->bitrate_omitted_packets),(unsigned long long)atomic_load(&d->bitrate_omitted_bytes));
    for(unsigned i=0;i<X4_STREAM_STAGE_COUNT;++i) {
        const Stage *s=&d->stage[i];if(i)fputc(',',f);
        fprintf(f,"\"%s\":{\"count\":%llu,\"sum_us\":%llu,\"maximum_us\":%llu,\"histogram_quantum_us\":100,\"overflow_count\":%llu,\"percentile_scope\":\"whole session cumulative histogram; finite values are upper bounds\"",names[i],(unsigned long long)s->count,(unsigned long long)s->sum,(unsigned long long)s->maximum,(unsigned long long)s->overflow);
        fputs(",\"mean_us\":",f);if(s->count)fprintf(f,"%.6f",(double)s->sum/(double)s->count);else fputs("null",f);
        const unsigned ps[]={50,95,99};for(unsigned k=0;k<3;++k){uint64_t value=0;fprintf(f,",\"p%u_upper_us\":",ps[k]);if(percentile(s,ps[k],&value))fprintf(f,"%llu",(unsigned long long)value);else fputs("null",f);}
        fputc('}',f);
    }
    fputc('}',f);write_new_diagnostics(f,d,ended,complete);write_presentation(f,d,ended);write_pacing(f,d);fputc('}',f);return ferror(f)?-1:0;
}
bool x4_stream_diagnostics_stage_percentile(const X4StreamDiagnostics *d, X4StreamStage stage,
    unsigned p, uint64_t *out_us, uint64_t *out_max_us)
{
    if (!d || (unsigned)stage >= X4_STREAM_STAGE_COUNT) return false;
    const Stage *s = &d->stage[stage];
    if (out_max_us) *out_max_us = s->maximum;
    return percentile(s, p, out_us);
}
int x4_stream_diagnostics_write_gpu(FILE *f,const X4StreamGpuDiagnostics *g)
{
    if(!f||!g)return -1;
    fprintf(f,"{\"requested\":\"%s\",\"effective\":\"%s\",\"admitted\":%s,\"admission_or_fallback_reason\":\"%s\",\"prepared\":%s,\"disabled\":%s,\"pending\":%s,\"quarantined\":%s,",
        g->requested?"GPU":"CPU",
#if X4_PUBLIC
        g->effective?"GPU":"CPU",
#else
        g->effective==2?"repeat_previous":g->effective?"GPU":"CPU",
#endif
        g->admitted?"true":"false",
        g->reason?g->reason:"not_requested",g->prepared?"true":"false",g->disabled?"true":"false",
        g->pending?"true":"false",g->quarantined?"true":"false");
    fprintf(f,"\"gpu_frames\":%llu,\"cpu_frames\":%llu,\"timeouts\":%llu,\"quarantines\":%llu,\"late_completed\":%llu,\"arena_WB_ONION_bytes\":%u,\"verification_CPU_scratch_bytes\":%u,\"timing_scope\":\"start before cache/preparation through CPU observation of matching EOP; includes preparation, submit and polling; on-time completions only, late/timeouts counted separately; excludes verification; not GPU-only time\",\"timing_us\":",
        (unsigned long long)g->gpu_frames,(unsigned long long)g->cpu_frames,
        (unsigned long long)g->timeouts,(unsigned long long)g->quarantines,(unsigned long long)g->late_completed,
        g->arena_bytes,g->verification_bytes);
    x4_ui_perf_write(f,&g->time);
    fprintf(f,",\"verification_background\":{\"prepared\":%s,\"failed\":%s,\"captures\":%llu,\"skipped_busy\":%llu,\"snapshot_WB_bytes\":%llu,\"control_WB_bytes\":%llu,\"priority\":767,\"priority_result\":%d,\"last_result\":%d,\"native_returns\":{\"snapshot_reserve\":%d,\"snapshot_map\":%d,\"control_reserve\":%d,\"control_map\":%d},\"enqueue_cpu_us\":",
        g->verification_prepared?"true":"false",g->verification_failed?"true":"false",
        (unsigned long long)g->verification_captures,(unsigned long long)g->verification_skipped,
        (unsigned long long)g->verification_native_bytes,(unsigned long long)g->verification_control_bytes,
        g->verification_priority_result,g->verification_last_result,g->verification_reserve_result,g->verification_map_result,
        g->verification_control_reserve_result,g->verification_control_map_result);
    x4_ui_perf_write(f,&g->verification_enqueue);fputs(",\"copy_gpu_real_us\":",f);x4_ui_perf_write(f,&g->verification_copy_real);
    fprintf(f,",\"copy_valid_samples\":%llu,\"scope\":\"one immutable owned WB snapshot, at most every 600 GPU frames; GPU copy EOP completes before image leases retire; CPU reference and cache readback run on low-priority worker after lease release; unsupported GPU copy timestamps omitted, not zero; mismatch permanently selects CPU and quarantines any pending GPU leases\"}",(unsigned long long)g->verification_copy_valid);
    fputs(",\"prep_us\":",f);x4_ui_perf_write(f,&g->prep);
    fputs(",\"eop_us\":",f);x4_ui_perf_write(f,&g->eop);
    fputs(",\"sondeo_us\":",f);x4_ui_perf_write(f,&g->spin);
    fprintf(f,",\"sondeo_listo\":%llu,\"sondeo_agotado\":%llu,\"sondeo_scope\":\"owner-only flushed-label polling bounded by caller timeout; does not consume semaphore; cancellation exits without counting timeout\"",
        (unsigned long long)g->spin_ready,(unsigned long long)g->spin_timeout);
    fputs(",\"by_resolution\":{",f);
    for(unsigned i=0;i<2;++i){const X4StreamGpuResolutionDiagnostics *r=&g->resolution[i];
        if(i)fputc(',',f);
        fprintf(f,"\"%s\":{\"gpu_real_us\":",i?"1080p":"720p");x4_ui_perf_write(f,&r->real);
        fprintf(f,",\"gpu_clock_hz\":%llu,\"clock_calibrated\":%s,\"marcas_no_soportadas\":%s,\"ts_samples\":%llu,\"ts_validas\":%llu,\"ts_invalidas\":%llu,\"ts_pending_calibration\":%llu,\"ts_pending_overflow\":%llu,\"first_end_ticks\":%llu,\"last_end_ticks\":%llu,\"first_observed_us\":%llu,\"last_observed_us\":%llu,\"latest_start_ticks\":%llu,\"latest_end_ticks\":%llu,\"scope\":\"raw timestamp accounting includes completed EOP observations; real histogram only on-time DONE after >=10s empirical calibration, not retroactive or CPU observation latency\"}",
            (unsigned long long)r->clock_hz,r->clock_calibrated?"true":"false",r->unsupported?"true":"false",
            (unsigned long long)r->samples,(unsigned long long)r->valid_samples,(unsigned long long)r->invalid,
            (unsigned long long)r->pending_samples,(unsigned long long)r->pending_overflow,
            (unsigned long long)r->first_end_ticks,(unsigned long long)r->last_end_ticks,
            (unsigned long long)r->first_observed_us,(unsigned long long)r->last_observed_us,
            (unsigned long long)r->latest_start_ticks,(unsigned long long)r->latest_end_ticks);
    }
    fputc('}',f);
    fprintf(f,",\"split_timing_scope\":\"same on-time completed population as timing_us; prep=start through Submit return, eop=Submit return through CPU EOP observation; SubmitDone and polling included in eop\",\"pause_episodes\":%llu,\"pause_cpu_frames\":%llu,\"pause_cpu_selections\":%llu,\"pause_max_us\":%llu,\"pause_active\":%s,\"pause_current_us\":%llu,",
        (unsigned long long)g->pause_episodes,(unsigned long long)g->pause_cpu_frames,
        (unsigned long long)g->pause_cpu_selections,(unsigned long long)g->pause_max_us,
        g->pause_active?"true":"false",(unsigned long long)g->pause_current_us);
    fprintf(f,"\"native_returns\":{\"not_run\":2147483647,\"reserve\":%d,\"map\":%d,\"submit\":%d,\"submit_done\":%d,\"are_submits_allowed\":%d},\"verification\":{\"every_gpu_frames\":600,\"rows\":16,\"readback_bytes_per_load\":0,\"tolerance\":1,\"checks\":%llu,\"checked_pixels\":%llu,\"pixels_outside_tolerance\":%llu,\"maximum_channel_difference\":%llu,\"errors\":%llu,\"verify_us\":",
        g->reserve_result,g->map_result,g->submit_result,g->submit_done_result,g->allowed_result,
        (unsigned long long)g->checks,(unsigned long long)g->checked_pixels,
        (unsigned long long)g->pixels_outside,(unsigned long long)g->max_channel_difference,
        (unsigned long long)g->verification_errors);
    x4_ui_perf_write(f,&g->verify);
    fputs(",\"timing_scope\":\"background CPU reference and comparison of copied WB snapshot; all synchronous scanout readback removed; worker never owns a native/scanout lease; worker_us excluded from image path, enqueue and copy timing reported separately\"}",f);
#if !X4_PUBLIC
    fprintf(f,",\"quality\":{\"requested_word\":%u,\"effective_word\":%u,\"last_key\":%u,\"reason\":\"%s\",\"pausa_repetidas\":%llu,\"dither_frames\":%llu,\"deband_soft_frames\":%llu,\"deband_strong_frames\":%llu,\"cas_low_frames\":%llu,\"cas_medium_frames\":%llu,\"control_WB_ONION_bytes\":%u,\"intermediate_WB_ONION_bytes\":%u,",
        g->quality_requested_word,g->quality_effective_word,g->last_quality_key,g->quality_reason?g->quality_reason:"off",
        (unsigned long long)g->pausa_repetidas,(unsigned long long)g->dither_frames,
        (unsigned long long)g->deband_soft_frames,(unsigned long long)g->deband_strong_frames,
        (unsigned long long)g->cas_low_frames,(unsigned long long)g->cas_medium_frames,
        g->quality_arena_bytes,g->quality_intermediate_bytes);
    fprintf(f,"\"native_returns\":{\"control_reserve\":%d,\"control_map\":%d,\"control_unmap\":%d,\"control_release\":%d,\"intermediate_reserve\":%d,\"intermediate_map\":%d,\"intermediate_unmap\":%d,\"intermediate_release\":%d},\"by_combination\":[",
        g->quality_reserve_result,g->quality_map_result,g->quality_unmap_result,g->quality_release_result,
        g->intermediate_reserve_result,g->intermediate_map_result,g->intermediate_unmap_result,g->intermediate_release_result);
    bool comma=false;
    for(unsigned key=0;key<X4_STREAM_GPU_QUALITY_COMBINATIONS;++key){
        const X4StreamGpuQualityDiagnostics *q=&g->by_quality[key];if(!q->frames)continue;
        if(comma)fputc(',',f);comma=true;
        fprintf(f,"{\"key\":%u,\"dither\":%s,\"deband\":%u,\"CAS\":%s,\"sharpness\":%u,\"frames\":%llu,\"gpu_real_us\":",
            key,(key&1u)?"true":"false",(key>>1)&3u,(key&8u)?"true":"false",(key>>4)&3u,
            (unsigned long long)q->frames);
        x4_ui_perf_write(f,&q->real);fputc('}',f);
    }
    fputs("],\"scope\":\"captured session options; completed on-time quality GPU conversions, not guaranteed flips; calibrated timestamp samples only; key=visual_word_low4|(sharpness<<4), repeat policy is not a visual combination; defaults zero, CPU and non-1080p-smooth ignore enhancements\"}}",f);
#else
    fprintf(f,",\"quality\":{\"requested_dither\":%s,\"effective_dither\":%s,\"dither_frames\":%llu,\"reason\":\"%s\"}}",
        (g->quality_requested_word&1u)?"true":"false",(g->quality_effective_word&1u)?"true":"false",
        (unsigned long long)g->dither_frames,g->quality_reason?g->quality_reason:"off");
#endif
    return ferror(f)?-1:0;
}
