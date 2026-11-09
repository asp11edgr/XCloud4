/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_pipeline.h"
#if X4_STREAM_PROFILE && (!X4_VIDEO_PROBE || X4_PROBE_T26)
#include "stream_diagnostics.h"
#include "stream_wait.h"
#include "stream_cpu.h"
#include "stream_au_queue.h"
#include "../streaming/keyframe_limiter.h"
#include "stream_idr_recovery.h"
#include "stream_color_policy.h"
#include "video_pipeline.h"
#include "live_h264.h"
#include "../streaming/rtc_receive_diag.h"
#include "../core/build_identity.h"
#include "../ui/server_settings.h"
#include "../ui/net_hud.h"
#include "gpu_convert.h"
#include "gpu_quality_ref.h"
#include <emmintrin.h>
#if !X4_PUBLIC
#include "../ui/stream_settings.h"
#endif
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#if defined(X4_OPENORBIS) && X4_PUBLIC
/* Last connected peer in this session. Copies numeric values only. */
extern void x4_rtc_diag_write_udp_json(FILE *);
#endif
#if X4_PUBLIC
#include <sys/stat.h>
#endif
#if X4_PUBLIC && !X4_VIDEO_PROBE && (defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST))
#include "../streaming/remb_estimator.h"
#endif

enum { STREAM_TIMEOUT_US=10000000, STREAM_SCANOUT_BYTES=0x800000,
    STREAM_SPS_BYTES=4096,
    STREAM_EPOCH_RECORDS=8, STREAM_ERROR_RECORDS=32 };
/* Same process can open many games. A late native status from an earlier
 * handle must never transfer a current buffer's ownership. */
static uint64_t next_stream_flip=UINT64_C(1)<<61;
typedef struct {
    const uint8_t *data,*sps,*pps;
    size_t bytes,sps_bytes,pps_bytes;uint64_t pts,identity;
    bool is_idr;
} StreamAU;
typedef struct {
    struct X4StreamPipeline *owner;unsigned index;
    OrbisPthread id;X4ProbeThread thread;
    atomic_uint_fast64_t submitted,completed;
    atomic_bool stop,finished;
    X4ProbeNativeOutput image;uint32_t *destination;int result;
    X4VideoPipelineColor color;
    X4StreamWait work_wait;
    X4StreamCpuReport cpu_report;
} StreamConverter;
typedef enum { STREAM_INITIAL, STREAM_SPS_CHANGE, STREAM_FRAME_RESTART } StreamEpochReason;
typedef struct {
    uint64_t ordinal, au_identity, begin_us, close_us;
    unsigned reason;
    int start_result, close_result;
    X4StreamVideoInfo native;
    uint8_t sps[64],pps[64];size_t sps_bytes,pps_bytes;
} StreamEpoch;
typedef struct {
    uint64_t ordinal, au_identity, observed_us;
    size_t input_bytes;
    unsigned frame_class, slot, consecutive,native_width,native_height,pitch;
    int native_result, control_result;
    bool accepted, completed_output, ownership_uncertain;
    char stage[48];
} StreamFrameError;
#include "../ui/ui_perf.h"
struct X4StreamPipeline {
    uint64_t session,started,ended;
    const atomic_bool *external_stop;
    X4Trace *trace;
    X4StreamAuQueue *au_queue;
    X4KeyframeLimiter *keyframe_limiter;
    atomic_uint_fast64_t callback_rejected,callback_full_design_failures,ingress_stall_recoveries,first_video_rtp_us;
    atomic_uint_fast64_t post_halt_video_packets,first_post_halt_video_us;
    atomic_uint_fast64_t submitted,consumed;
    atomic_uint_fast64_t dequeued;
    atomic_bool cancel,cleanup_allowed,decoder_done,decoder_finished;
    atomic_bool keyframe_pending,reconfiguring;
    atomic_uint conversion_readers;
    atomic_uint_fast64_t keyframe_created,keyframe_coalesced,keyframe_taken;
    atomic_uint_fast64_t current_au;
    atomic_uint_fast64_t stop_requested_us;
    atomic_int error;
    atomic_uint admission_failure;
    X4LiveVideo video;
    X4StreamVideoInfo native_info;
    uint8_t sps_prefix[64],pps_prefix[64];size_t sps_size,pps_size;
    uint8_t active_sps[STREAM_SPS_BYTES];size_t active_sps_size;
    uint8_t observed_sps_prefix[64],observed_pps_prefix[64];size_t observed_sps_size,observed_pps_size;
    X4StreamSps observed_sps;X4StreamSpsDiagnostics observed_diagnostics;
    StreamEpoch epochs[STREAM_EPOCH_RECORDS];uint64_t epoch_count,epoch_records_omitted;
    StreamFrameError frame_error_records[STREAM_ERROR_RECORDS];uint64_t frame_error_count,frame_records_omitted;
    uint64_t reconfiguration_count,restart_count,reconfiguration_discarded_ready,waiting_idr_drops;
    uint64_t parameter_log_count,parameter_logs_omitted;
    uint64_t quiesce_calls,quiesce_us,quiesce_max_us;
    unsigned consecutive_errors,maximum_consecutive_errors,consecutive_native_errors;
    bool waiting_idr;
    char failure_where[48];
    X4VideoPipelineColor color;
    bool native_start_attempted;
    uint64_t scalar_outputs;
    unsigned conversion_path;
    bool conversion_path_seen;
    int native_decode_result;
    uint64_t calls_with_output,calls_without_output,calls_with_error,last_native_output_us;
    uint64_t first_native_output_us,last_valid_native_output_us,native_output_epochs;
    OrbisPthread decoder;X4ProbeThread decoder_thread;
    StreamConverter converters[2];
    X4VideoPipelineRing ring;X4ProbeNativeOutput images[X4_PIPELINE_OUTPUT_CAPACITY];
    X4StreamMutex ring_gate,status_gate;
    bool ring_mutex_initialized,status_mutex_initialized;
    X4StreamWait decode_wait,present_wait;
    X4StreamWaitStats external_wait_stats[X4_STREAM_WAIT_EXTERNAL_ACTORS];
    uint64_t external_wait_elapsed[X4_STREAM_WAIT_EXTERNAL_ACTORS];
    bool external_wait_recorded[X4_STREAM_WAIT_EXTERNAL_ACTORS];
    X4StreamCpuReport decode_cpu_report;
    X4StreamCpuReport ingress_cpu_report;
    bool ingress_cpu_recorded;
    X4StreamAudioObservation audio_observation;
    bool audio_observed;
    uint64_t controller_previous_us;
    uint64_t flip_end_us,flip_time_valid,flip_time_invalid,flip_to_conversion_pending_omitted;
    uint64_t last_flip_ptime,last_flip_submitted_us,last_flip_observed_us;
    bool flip_to_conversion_pending;
    uint64_t halt_begin_us,halt_end_us,decoder_join_wait_us,native_stop_us;
    uint64_t native_decode_after_stop_us,native_decode_started_after_stop;
    X4StreamPipelineSnapshot view;
    X4StreamDiagnostics *diagnostics;
    X4StreamRequestConfig request;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    bool t26_enabled;
    X4T26PipelineHooks t26_hooks;
#endif
    X4StreamIdrRecovery idr_recovery;
    atomic_uint_fast64_t diagnostic_calls,diagnostic_us,diagnostic_max_us;
    X4VideoPipelineDisplay display;
    int display_handle,display_allocate,display_map,display_register,display_rate;
    int display_close,display_unmap,display_release;
    off_t display_offset;void *display_memory;bool display_allocated;
    uint32_t *display_pixels[X4_PIPELINE_SCANOUT_CAPACITY];
    /* Main-owned, allocated only for a frozen reconnect notice. No steady
     * frame copy; at most one8MiB snapshot including any existing overlay. */
    uint32_t *notice_backing;
    bool notice_scanout[X4_PIPELINE_SCANOUT_CAPACITY];
    bool notice_mode_active,notice_mailbox_capable,notice_mailbox_admission;
    unsigned scanout_count;
    X4VideoPipelineMailboxClock mailbox_clock;
    uint64_t mailbox_late_flips;
    unsigned last_flip_mode;bool last_flip_measured;
    uint64_t measure_first,measure_last;
    uint64_t acquired_slot_wait_us,acquired_ring_gate_us,acquired_full_drops;
    unsigned acquired_held_native,acquired_occupied;
    bool converting,closed,retained,exported,video_halted,native_teardown_failed;
    bool conversion_cleanup_failed;
    X4GpuConvert *gpu;
    X4StreamGpuDiagnostics gpu_metrics;
    OrbisPthread verification_worker;X4ProbeThread verification_thread;
    X4StreamWait verification_wait;
    atomic_bool verification_stop,verification_finished;
    void *verification_scratch;
    atomic_int verification_priority_result;
    uint64_t verification_status_checks,verification_status_captures,verification_status_copies;
    unsigned gpu_quality_requested_word,gpu_quality_accepted_word;
    unsigned gpu_quality_log_word,gpu_quality_log_width,gpu_quality_log_sharp;
    const char *gpu_quality_log_reason;
    bool gpu_quality_logged;
    bool gpu_prepare_attempted,gpu_active,gpu_quarantine,gpu_pause_conversion;
    X4VideoPipelineColor gpu_color;
    X4VideoPipelineLease gpu_held_image,gpu_held_scanout;
    uint64_t gpu_log_at;
    uint64_t gpu_pause_log_at,gpu_pause_log_seen,gpu_pause_log_duration,gpu_pause_log_frames,gpu_pause_cpu_base;
    bool gpu_pause_log_pending,gpu_pause_log_emitted;
    bool gpu_timestamp_unsupported_logged[X4_GPU_RESOLUTION_COUNT];
    int halt_result;
    X4StreamStartup startup;
    X4VideoPipelineLease image_lease,scanout_lease;
    X4ProbeNativeOutput converting_image;
    uint64_t conversion_begin,work;
    /* Diagnostic tags only; the ring policy and converter job are unchanged. */
    atomic_bool menu_open;
    bool conversion_menu_open,scanout_menu[X4_PIPELINE_SCANOUT_CAPACITY];
    /* Main-owned provenance for completed unsubmitted READY only. A GPU
     * verification failure must not cancel a valid CPU fallback image. */
    bool scanout_gpu[X4_PIPELINE_SCANOUT_CAPACITY];
    uint64_t gpu_ready_discarded_verification;
    X4UiPerf conversion_menu[2];
    uint64_t menu_presentations,menu_discarded_latency,menu_discarded_full,menu_log_at;
    int decoder_close_result;
    /* Presentation-only additions stay after every inherited owner field. */
    X4VideoPipelineSyncHistory synced_history;
    uint64_t scanout_ready_us[X4_PIPELINE_SCANOUT_CAPACITY];
    uint64_t pacing_ring_full,pacing_ring_latest;
    bool notice_synced;
#if !X4_PUBLIC
    uint64_t pacing_cola_vsync_wait_drops;
#endif
};
static uint64_t now_us(void){return sceKernelGetProcessTime();}
#if X4_VIDEO_PROBE && X4_PROBE_T26
/* Same sole-owner teardown and successful-Delete ownership proof as the
 * production path. Observers bracket the real stop; native time excludes
 * their work. Reconfiguration uses the same brackets as final shutdown. */
static int t26_stop_native(X4StreamPipeline *p,uint64_t *elapsed_us)
{
    if(p->t26_enabled){
        x4_live_video_stream_info(&p->video,&p->native_info);
        if(p->t26_hooks.before_close)
            p->t26_hooks.before_close(p->t26_hooks.context,&p->video,&p->native_info);
    }
    uint64_t begin=now_us();int rc=x4_live_video_stop(&p->video);
    if(elapsed_us)*elapsed_us=now_us()-begin;
    if(p->t26_enabled){
        x4_live_video_stream_info(&p->video,&p->native_info);
        if(p->t26_hooks.after_close)
            p->t26_hooks.after_close(p->t26_hooks.context,&p->video,&p->native_info,rc);
    }
    return rc;
}
#endif
static bool stopping(const X4StreamPipeline *p)
{return atomic_load_explicit(&p->cancel,memory_order_acquire)||(p->external_stop&&atomic_load_explicit(p->external_stop,memory_order_acquire));}
/* Both gates are initialized before publication and destroyed after all
 * owners join. Their critical sections contain no native Decode, waits,
 * allocations or I/O. The ingress owner uses trylock only. */
static void lock(X4StreamMutex *gate){(void)x4_stream_mutex_lock(gate);}
static void unlock(X4StreamMutex *gate){(void)x4_stream_mutex_unlock(gate);}
static void wake_all(X4StreamPipeline *p)
{
    (void)x4_stream_wait_notify(&p->decode_wait);
    (void)x4_stream_wait_notify(&p->present_wait);
    (void)x4_stream_wait_notify(&p->verification_wait);
    for(unsigned i=0;i<2;++i)(void)x4_stream_wait_notify(&p->converters[i].work_wait);
}
static int fail(X4StreamPipeline *p,int error)
{
    if(error>0)error=-error;
    if(!error)error=X4_PIPELINE_STATE;
    int expected=0;(void)atomic_compare_exchange_strong_explicit(&p->error,&expected,error,memory_order_acq_rel,memory_order_relaxed);
    atomic_store_explicit(&p->cancel,true,memory_order_release);wake_all(p);return error;
}
static int fail_named(X4StreamPipeline *p,int error,const char *where)
{
    lock(&p->status_gate);bool first=atomic_load_explicit(&p->error,memory_order_relaxed)==0;
    if(first)snprintf(p->failure_where,sizeof(p->failure_where),"%s",where);
    int rc=fail(p,error);unlock(&p->status_gate);
    if(first)printf("XCloud4: stream fatal stage=%s input_AU=%llu code=%d\n",where,
        (unsigned long long)atomic_load_explicit(&p->current_au,memory_order_relaxed),rc);
    return rc;
}
static int fail_admission(X4StreamPipeline *p,int error,unsigned stage)
{
    /* The RTP-drain owner must not enter a status lock or stdio even on a
     * fatal branch. The first error publishes its bounded stage token; the
     * stopped/joined close owner formats and logs that token later. */
    if(error>0)error=-error;
    if(!error)error=X4_PIPELINE_STATE;
    int expected=0;
    if(atomic_compare_exchange_strong_explicit(&p->error,&expected,error,
            memory_order_acq_rel,memory_order_relaxed))
        atomic_store_explicit(&p->admission_failure,stage,memory_order_release);
    atomic_store_explicit(&p->cancel,true,memory_order_release);wake_all(p);
    return error;
}
static void publish_admission_failure(X4StreamPipeline *p)
{
    /* Full close promises all CPU owners have joined. No producer writes the
     * failure label and no formatting occurs on AU submission. */
    unsigned stage=atomic_load_explicit(&p->admission_failure,memory_order_acquire);
    if(!stage)return;
    /* This token is set only by the winning first-error CAS. A named error
     * could have sampled zero immediately before that CAS; its provisional
     * label must not replace the actual admission failure in the report. */
    const char *where=stage==1?"AU identity capacity":"AU admission state";
    snprintf(p->failure_where,sizeof(p->failure_where),"%s",where);
    printf("XCloud4: stream fatal stage=%s input_AU=%llu code=%d (deferred after join)\n",where,
        (unsigned long long)atomic_load_explicit(&p->current_au,memory_order_relaxed),
        atomic_load_explicit(&p->error,memory_order_acquire));
}
static void request_keyframe(X4StreamPipeline *p)
{
    x4_keyframe_limiter_request(p->keyframe_limiter,X4_KEYFRAME_PIPELINE);
    if(atomic_exchange_explicit(&p->keyframe_pending,true,memory_order_acq_rel))
        atomic_fetch_add_explicit(&p->keyframe_coalesced,1,memory_order_relaxed);
    else atomic_fetch_add_explicit(&p->keyframe_created,1,memory_order_relaxed);
}
static void set_waiting_idr(X4StreamPipeline *p,bool waiting)
{
    if(waiting){x4_stream_idr_begin(&p->idr_recovery,now_us());
        x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_WAIT_IDR);}
    else {
        if(p->idr_recovery.waiting)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_WAIT_IDR);
        x4_stream_idr_validated(&p->idr_recovery);
    }
    p->waiting_idr=waiting;
    lock(&p->status_gate);p->view.waiting_idr=waiting;unlock(&p->status_gate);
}
static int service_idr_wait(X4StreamPipeline *p)
{
    bool request=false;int rc=x4_stream_idr_poll(&p->idr_recovery,now_us(),&request);
    if(request)request_keyframe(p);return rc;
}
static int service_initial_resolution(X4StreamPipeline *p)
{
    if(!p->startup.enabled||p->startup.admitted)return 0;
    bool request=false;int rc=x4_stream_startup_poll(&p->startup,now_us(),&request);
    if(request)request_keyframe(p);
    if(rc)printf("XCloud4: stream initial resolution timeout alias_sent_us=%llu last_coded=%ux%u dropped_AU=%llu wait_us=%u code=%d\n",
        (unsigned long long)x4_stream_startup_alias_sent_us(&p->startup),p->startup.last_width,p->startup.last_height,
        (unsigned long long)p->startup.dropped_aus,X4_STREAM_INITIAL_RESOLUTION_WAIT_US,rc);
    return rc;
}
static void admit_initial_resolution(X4StreamPipeline *p,unsigned width,unsigned height)
{
    if(!p->startup.enabled||p->startup.admitted)return;
    x4_stream_startup_admit(&p->startup,now_us());
    lock(&p->status_gate);p->view.rejected_width=p->view.rejected_height=0;unlock(&p->status_gate);
    printf("XCloud4: stream initial resolution admitted coded=%ux%u alias_sent_us=%llu observed_us=%llu elapsed_available=%u alias_to_SPS_us=%llu dropped_AU=%llu (SPS admission, not decoder or display success)\n",
        width,height,(unsigned long long)x4_stream_startup_alias_sent_us(&p->startup),
        (unsigned long long)p->startup.first_admitted_us,p->startup.elapsed_available,
        (unsigned long long)p->startup.alias_to_sps_us,(unsigned long long)p->startup.dropped_aus);
}
static void duration(X4StreamPipeline *p,X4StreamStage stage,uint64_t us)
{
    uint64_t start=now_us();x4_stream_diagnostics_record(p->diagnostics,stage,us);
    uint64_t cost=now_us()-start;
    atomic_fetch_add_explicit(&p->diagnostic_calls,1,memory_order_relaxed);
    atomic_fetch_add_explicit(&p->diagnostic_us,cost,memory_order_relaxed);
    uint64_t old=atomic_load_explicit(&p->diagnostic_max_us,memory_order_relaxed);
    /* A bounded one-shot update; under contention maximum is a lower bound. */
    if(cost>old)(void)atomic_compare_exchange_strong_explicit(&p->diagnostic_max_us,&old,cost,memory_order_relaxed,memory_order_relaxed);
}
static bool measured_output(const X4StreamPipeline *p,uint64_t output_id)
{return !p->measure_first||(output_id>=p->measure_first&&output_id<=p->measure_last);}
static void note_conversion_begin(X4StreamPipeline *p,uint64_t began)
{
    if(p->flip_to_conversion_pending){
        duration(p,X4_STREAM_FLIP_END_TO_CONVERSION,began-p->flip_end_us);
        if(p->last_flip_measured)x4_stream_diagnostics_flip_to_conversion(p->diagnostics,p->last_flip_mode,began-p->flip_end_us);
        p->flip_to_conversion_pending=false;
    }
}
static void *stream_pipeline_convert_worker(void *argument)
{
    StreamConverter *w=argument;uint64_t seen=0;
    int applied=x4_stream_cpu_apply_current_thread(w->owner->request.cpu_variant,
        X4_CPU_ROLE_CONVERTER,w->index,&w->cpu_report);
#if X4_STREAM_PIPELINE_TEST_HOOKS
    extern int x4_stream_pipeline_test_cpu_ready(X4StreamPipeline *,unsigned,int);
    applied=x4_stream_pipeline_test_cpu_ready(w->owner,w->index,applied);
#endif
    if(applied)printf("XCloud4: converter %u CPU settings rc=%d; variante parcialmente aplicada, continua con restauracion/default registrado\n",w->index,applied);
    for(;;){
        uint64_t work=atomic_load_explicit(&w->submitted,memory_order_acquire);
#if X4_STREAM_PIPELINE_TEST_HOOKS
        extern void x4_stream_pipeline_test_before_idle(X4StreamPipeline *,unsigned);
        if(work==seen)x4_stream_pipeline_test_before_idle(w->owner,w->index);
#endif
        if(work!=seen){
            uint64_t start=now_us();
            w->result=x4_video_pipeline_convert_stripe_color(&w->image.layout,w->image.pixels,w->image.owned_bytes,
                w->destination,STREAM_SCANOUT_BYTES,1920,2,w->index,&w->color);
            duration(w->owner,(X4StreamStage)(X4_STREAM_STRIPE0+w->index),now_us()-start);
            seen=work;atomic_store_explicit(&w->completed,work,memory_order_release);
            int notified=x4_stream_wait_notify(&w->owner->present_wait);
            if(notified){(void)fail_named(w->owner,notified,"converter completion notification");break;}
        /* Main is the only dispatch owner and publishes this stop only after
         * draining any submitted conversion. Global cancellation can race
         * publication, so it wakes idle workers but never retires them here. */
        }else if(atomic_load_explicit(&w->stop,memory_order_acquire))break;
        else {int waited=x4_stream_wait_block(&w->work_wait,X4_STREAM_WAIT_MAX_US);
            if(waited<0){(void)fail_named(w->owner,waited,"converter wait");break;}}
    }
    atomic_store_explicit(&w->finished,true,memory_order_release);return NULL;
}
/* Called while holding the existing ring gate. No additional lock, pixel
 * access or policy decision is introduced by this accounting. */
static void menu_ring_drops(X4StreamPipeline *p,uint64_t latency,uint64_t full)
{
    if(atomic_load_explicit(&p->menu_open,memory_order_relaxed)){
        p->menu_discarded_latency+=p->ring.discarded_latency-latency;
        p->menu_discarded_full+=p->ring.discarded_ready-full;
    }
}
static int acquire_native(X4StreamPipeline *p,X4VideoPipelineLease *lease)
{
    uint64_t start=now_us(),gate_total=0,full_total=0;
    while(!stopping(p)){
        int recovery=service_idr_wait(p);if(recovery)return recovery;
        uint64_t gate_begin=now_us();lock(&p->ring_gate);gate_total+=now_us()-gate_begin;
        uint64_t before_latency=p->ring.discarded_latency,before_full=p->ring.discarded_ready;
        int rc=x4_video_pipeline_acquire_decode(&p->ring,lease,NULL);
        menu_ring_drops(p,before_latency,before_full);full_total+=p->ring.discarded_ready-before_full;
        unsigned occupied=p->ring.occupied;unlock(&p->ring_gate);
        if(rc!=X4_PIPELINE_WAIT){
            p->acquired_slot_wait_us=now_us()-start;p->acquired_ring_gate_us=gate_total;
            p->acquired_full_drops=full_total;p->acquired_occupied=occupied;
            unsigned mask=x4_live_video_stream_output_held_mask(&p->video),held=0;
            for(;mask;mask>>=1)held+=mask&1u;p->acquired_held_native=held;return rc;}
        if(now_us()-start>STREAM_TIMEOUT_US)return X4_PIPELINE_EXHAUSTED;
        int waited=x4_stream_wait_block(&p->decode_wait,X4_STREAM_WAIT_MAX_US);
        if(waited<0)return waited;
    }
    return X4_PIPELINE_WAIT;
}
static int accept_native(X4StreamPipeline *p,X4VideoPipelineLease offered,const X4ProbeNativeOutput *out)
{
    int rc=0;lock(&p->ring_gate);
    if(out->valid){
        X4VideoPipelineLease returned;
        rc=x4_video_pipeline_decoding_lease(&p->ring,out->slot,&returned);
        if(!rc){p->images[out->slot]=*out;rc=x4_video_pipeline_publish_ready(&p->ring,returned,out->output_id,out->decode_end_us);}
    }
    /* Depth-two native ownership is independent of isAccepted. Ported from
     * the validated probe owner: only returned pointer or Delete retires it. */
    bool unowned=p->native_info.active_depth==2?
        !(x4_live_video_stream_output_held_mask(&p->video)&(1u<<offered.slot)):!out->accepted;
    if(!rc&&unowned&&(!out->valid||out->slot!=offered.slot))rc=x4_video_pipeline_abort_decode(&p->ring,offered);
    unlock(&p->ring_gate);
    if(!rc)rc=x4_stream_wait_notify(&p->present_wait);
    return rc;
}
static void parameter_log(const char *name,const uint8_t *bytes,size_t size)
{
    static const char alphabet[]="0123456789abcdef";
    char hex[129];size_t captured=size<64?size:64;
    for(size_t i=0;i<captured;++i){hex[2*i]=alphabet[bytes[i]>>4];hex[2*i+1]=alphabet[bytes[i]&15];}
    hex[2*captured]=0;
    printf("XCloud4: stream %s bytes=%zu captured=%zu truncated=%u hex=%s\n",name,size,captured,size>captured,hex);
}
static size_t nal_prefix(const uint8_t *p,size_t n,size_t at)
{
    if(at+3<=n&&!p[at]&&!p[at+1]&&p[at+2]==1)return 3;
    if(at+4<=n&&!p[at]&&!p[at+1]&&!p[at+2]&&p[at+3]==1)return 4;
    return 0;
}
static bool find_nal(const uint8_t *p,size_t n,unsigned type,size_t *begin,size_t *length)
{
    for(size_t at=0;at+3<=n;){
        size_t prefix=nal_prefix(p,n,at);if(!prefix){++at;continue;}
        size_t header=at+prefix;if(header>=n)return false;
        size_t end=header+1;while(end+3<=n&&!nal_prefix(p,n,end))++end;
        if(end+3>n)end=n;
        if((p[header]&31)==type){if(begin)*begin=at;if(length)*length=end-at;return true;}
        at=end;
    }
    return false;
}
static void inline_parameters(StreamAU *au)
{
    size_t at=0,bytes=0;
    if(find_nal(au->data,au->bytes,7,&at,&bytes)&&bytes<=STREAM_SPS_BYTES){au->sps=au->data+at;au->sps_bytes=bytes;}
    if(find_nal(au->data,au->bytes,8,&at,&bytes)&&bytes<=STREAM_SPS_BYTES){au->pps=au->data+at;au->pps_bytes=bytes;}
}
static void note_parameters(X4StreamPipeline *p,const StreamAU *au)
{
    p->observed_sps_size=au->sps_bytes;p->observed_pps_size=au->pps_bytes;
    memcpy(p->observed_sps_prefix,au->sps,au->sps_bytes<64?au->sps_bytes:64);
    memcpy(p->observed_pps_prefix,au->pps,au->pps_bytes<64?au->pps_bytes:64);
    if(!p->native_start_attempted){
        p->sps_size=au->sps_bytes;p->pps_size=au->pps_bytes;
        memcpy(p->sps_prefix,p->observed_sps_prefix,64);memcpy(p->pps_prefix,p->observed_pps_prefix,64);
    }
    if(p->parameter_log_count++<STREAM_EPOCH_RECORDS){parameter_log("SPS",au->sps,au->sps_bytes);parameter_log("PPS",au->pps,au->pps_bytes);}
    else p->parameter_logs_omitted+=2;
}
static void observed_sps(X4StreamPipeline *p,const X4StreamSps *s,const X4StreamSpsDiagnostics *d)
{
    p->observed_sps=*s;p->observed_diagnostics=*d;
    if(d->code==X4_SPS_SIZE){lock(&p->status_gate);p->view.rejected_width=s->coded_width;
        p->view.rejected_height=s->coded_height;unlock(&p->status_gate);}
}
static void begin_epoch(X4StreamPipeline *p,const StreamAU *au,unsigned reason,int rc,uint64_t begin)
{
    ++p->epoch_count;if(p->epoch_count>STREAM_EPOCH_RECORDS)++p->epoch_records_omitted;
    StreamEpoch *e=&p->epochs[(p->epoch_count-1)%STREAM_EPOCH_RECORDS];memset(e,0,sizeof(*e));
    e->ordinal=p->epoch_count;e->au_identity=au->identity;e->reason=reason;e->begin_us=begin;
    e->start_result=rc;e->close_result=X4_PROBE_STACK_NOT_RUN;e->native=p->native_info;
    e->sps_bytes=au->sps_bytes;e->pps_bytes=au->pps_bytes;
    memcpy(e->sps,p->observed_sps_prefix,64);memcpy(e->pps,p->observed_pps_prefix,64);
}
static void close_epoch(X4StreamPipeline *p,int rc)
{
    if(!p->epoch_count)return;
    StreamEpoch *e=&p->epochs[(p->epoch_count-1)%STREAM_EPOCH_RECORDS];
    if(e->close_us)return; /* Waiting for a later IDR is outside this epoch. */
    e->close_us=now_us();e->close_result=rc;e->native=p->native_info;
}
static int frame_error(X4StreamPipeline *p,const StreamAU *au,const X4ProbeNativeOutput *out)
{
    ++p->frame_error_count;++p->consecutive_errors;
    if(p->consecutive_errors>p->maximum_consecutive_errors)p->maximum_consecutive_errors=p->consecutive_errors;
    if(p->frame_error_count>STREAM_ERROR_RECORDS)++p->frame_records_omitted;
    StreamFrameError *e=&p->frame_error_records[(p->frame_error_count-1)%STREAM_ERROR_RECORDS];
    *e=(StreamFrameError){.ordinal=p->frame_error_count,.au_identity=au->identity,.observed_us=now_us(),.input_bytes=au->bytes,
        .frame_class=out->frame_class,.slot=out->slot,.consecutive=p->consecutive_errors,
        .native_width=out->native_width,.native_height=out->native_height,.pitch=out->pitch,
        .native_result=out->native_result,.control_result=out->control_result,.accepted=out->accepted,
        .completed_output=out->completed_output,.ownership_uncertain=out->ownership_uncertain};
    snprintf(e->stage,sizeof(e->stage),"%s",p->video.stage);
    if(p->frame_error_count<=STREAM_ERROR_RECORDS)printf("XCloud4: stream frame error input_AU=%llu class=%u native=%d control=%d accepted=%u completed=%u uncertain=%u slot=%u consecutive=%u\n",
        (unsigned long long)au->identity,out->frame_class,out->native_result,out->control_result,out->accepted,out->completed_output,out->ownership_uncertain,out->slot,p->consecutive_errors);
    lock(&p->status_gate);p->view.frame_errors=p->frame_error_count;p->view.consecutive_frame_errors=p->consecutive_errors;unlock(&p->status_gate);
    /* BAD_FRAME waits for an IDR with a five-second absolute deadline, not
     * ten more dependent frames. Other uncertain native failure policy stays
     * bounded separately; the recorded streak is diagnostic for BAD_FRAME. */
    if(out->frame_class!=X4_STREAM_FRAME_BAD_FRAME&&out->frame_class!=X4_STREAM_FRAME_FATAL&&
        ++p->consecutive_native_errors>=X4_STREAM_FRAME_ERROR_LIMIT)return X4_STREAM_FRAME_FAILURE_LIMIT;
    if(out->frame_class!=X4_STREAM_FRAME_FATAL&&out->frame_class!=X4_STREAM_FRAME_BAD_FRAME)request_keyframe(p);
    return 0;
}
static int discard_bad_output(X4StreamPipeline *p,X4VideoPipelineLease offered,const X4ProbeNativeOutput *out)
{
    int rc=0;lock(&p->ring_gate);
    if(out->completed_output){X4VideoPipelineLease completed;rc=x4_video_pipeline_decoding_lease(&p->ring,out->slot,&completed);
        if(!rc)rc=x4_video_pipeline_abort_decode(&p->ring,completed);}
    bool unowned=p->native_info.active_depth==2?
        !(x4_live_video_stream_output_held_mask(&p->video)&(1u<<offered.slot)):!out->accepted;
    if(!rc&&unowned&&(!out->completed_output||out->slot!=offered.slot))rc=x4_video_pipeline_abort_decode(&p->ring,offered);
    unlock(&p->ring_gate);return rc;
}
static bool recoverable_sps_syntax(int rc,const X4StreamSpsDiagnostics *d)
{
    return rc==X4_SPS_INVALID||rc==X4_SPS_PROFILE||rc==X4_SPS_CROP||rc==X4_SPS_COLOR||
        rc==X4_SPS_TRUNCATED||rc==X4_SPS_EBSP||rc==X4_SPS_SYNTAX||(rc==X4_SPS_SIZE&&!d->syntax_complete);
}
/* Decode owner only. Main may finish an existing conversion/flip, but cannot
 * take a new native image after reconfiguring is published under ring_gate. */
static int quiesce_native(X4StreamPipeline *p)
{
    lock(&p->ring_gate);atomic_store_explicit(&p->reconfiguring,true,memory_order_release);
    p->reconfiguration_discarded_ready+=x4_video_pipeline_cancel_ready(&p->ring);unlock(&p->ring_gate);
    lock(&p->status_gate);p->view.native_ready=false;unlock(&p->status_gate);
    uint64_t begin=now_us();int wait_result=0;
    while(atomic_load_explicit(&p->conversion_readers,memory_order_acquire)){
        if(stopping(p)){wait_result=X4_STREAM_VIDEO_CANCELLED;break;}
        if(now_us()-begin>STREAM_TIMEOUT_US){wait_result=X4_STREAM_RECONFIGURATION_TIMEOUT;break;}
        int waited=x4_stream_wait_block(&p->decode_wait,X4_STREAM_WAIT_MAX_US);
        if(waited<0){wait_result=waited;break;}
    }
    uint64_t elapsed=now_us()-begin;++p->quiesce_calls;p->quiesce_us+=elapsed;
    if(elapsed>p->quiesce_max_us)p->quiesce_max_us=elapsed;
    if(wait_result)return wait_result;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    int rc=t26_stop_native(p,NULL);
#else
    int rc=x4_live_video_stop(&p->video);
#endif
    x4_live_video_stream_info(&p->video,&p->native_info);close_epoch(p,rc);
    if(rc||p->video.state){p->native_teardown_failed=true;
        p->decoder_close_result=rc?rc:X4_PROBE_STACK_RETAINED;return p->decoder_close_result;}
    /* Successful Delete is the ownership proof for ALL native-held leases. */
    lock(&p->ring_gate);p->reconfiguration_discarded_ready+=x4_video_pipeline_cancel_ready(&p->ring);
    for(unsigned i=0;i<p->ring.output_count&&!rc;++i)if(p->ring.slots[i].state==X4_PIPELINE_DECODING){X4VideoPipelineLease old;
        rc=x4_video_pipeline_decoding_lease(&p->ring,i,&old);if(!rc)rc=x4_video_pipeline_abort_decode(&p->ring,old);}
    if(!rc&&!x4_video_pipeline_ring_idle(&p->ring))rc=X4_PIPELINE_STATE;
    unlock(&p->ring_gate);return rc;
}
static void queue_signals(X4StreamPipeline *p,const X4StreamAuOutcome *outcome)
{
    if(outcome->waiting_idr)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_WAIT_IDR);
    if(!outcome->request_keyframe)return;
    x4_keyframe_limiter_observe_gap(p->keyframe_limiter);
    x4_keyframe_limiter_request(p->keyframe_limiter,X4_KEYFRAME_AU_QUEUE);
}
/* Reconnect bridge: READY/WRITING AUs from the old peer are invalidated by
 * the existing queue recovery generation. INFLIGHT remains pinned until its
 * original Decode lease is released. No native ledger/output ring changes. */
int x4_stream_pipeline_begin_receive_epoch(X4StreamPipeline *p)
{
    if(!p||stopping(p))return X4_STREAM_AU_STOPPED;
    X4StreamAuOutcome outcome={0};
    int rc=x4_stream_au_queue_require_idr(p->au_queue,X4_STREAM_AU_LOSS_BUSY,&outcome);
    if(rc!=X4_STREAM_AU_OK)return rc;
    queue_signals(p,&outcome);
    (void)x4_stream_wait_notify(&p->decode_wait);
    return 0;
}

static int retire_au(X4StreamPipeline *p,X4StreamAuLease lease,bool *active)
{
    if(!*active)return 0;
    int rc=x4_stream_au_queue_release(p->au_queue,lease);
    if(rc)return fail_named(p,X4_STREAM_AU_FATAL,"AU lease release");
    *active=false;atomic_fetch_add_explicit(&p->consumed,1,memory_order_release);return 0;
}
static void *stream_pipeline_decode_worker(void *argument)
{
    X4StreamPipeline *p=argument;bool native_started=false,au_active=false;int rc=0;
    rc=x4_stream_cpu_apply_current_thread(p->request.cpu_variant,X4_CPU_ROLE_DECODE,0,&p->decode_cpu_report);
    if(rc)printf("XCloud4: decode caller CPU settings rc=%d; variante parcialmente aplicada, continua\n",rc);
    rc=0; /* Scheduling failures are diagnostic; Query/Create retain their policy. */
    X4StreamAuLease au_lease={0};
    unsigned start_reason=STREAM_INITIAL;const char *failure="decode worker";
    while(!stopping(p)){
        rc=retire_au(p,au_lease,&au_active);if(rc){failure="AU lease release";break;}
        rc=service_initial_resolution(p);
        if(rc){failure="initial resolution did not arrive within ten seconds";break;}
        rc=service_idr_wait(p);
        if(rc){failure="IDR did not arrive within five seconds";break;}
        X4StreamAuView input={0};X4StreamAuOutcome outcome={0};
        int admission=x4_stream_au_queue_try_take(p->au_queue,now_us(),&au_lease,&input,&outcome);
        queue_signals(p,&outcome);
        if(admission==X4_STREAM_AU_EMPTY||admission==X4_STREAM_AU_BUSY){
            int waited=x4_stream_wait_block(&p->decode_wait,X4_STREAM_WAIT_MAX_US);
            if(waited<0){rc=waited;failure="AU wait";break;}continue;}
        if(admission==X4_STREAM_AU_STOPPED){rc=0;break;}
        if(admission<0){rc=admission;failure="AU queue state";break;}
        if(admission!=X4_STREAM_AU_OK)continue;
        au_active=true;
        StreamAU current={.data=input.data,.sps=input.sps,.pps=input.pps,.bytes=input.bytes,
            .sps_bytes=input.sps_bytes,.pps_bytes=input.pps_bytes,.pts=input.pts_us,
            .identity=input.identity,.is_idr=input.is_idr};
        StreamAU *au=&current;
        atomic_store_explicit(&p->current_au,au->identity,memory_order_relaxed);
        atomic_fetch_add_explicit(&p->dequeued,1,memory_order_relaxed);
        /* Do this before SPS processing: a dependent AU during the wait is
         * discarded without Decode, another error, or a renewed deadline. */
        if(p->waiting_idr&&!find_nal(au->data,au->bytes,5,NULL,NULL)){
            ++p->waiting_idr_drops;/* AU lease retires before the next iteration or worker exit. */continue;}
        inline_parameters(au);bool parameters_noted=false;
        /* Initial aliased admission only. Do not submit an oversized AU to
         * Query/Create/Decode while Xbox switches resolutions. A malformed,
         * unsupported-format or interlaced SPS keeps the original rejection
         * path, and a later native epoch/reconfiguration never gets this wait. */
        if(p->startup.enabled&&!p->startup.admitted&&!p->native_start_attempted){
            X4StreamSps candidate={0};X4StreamSpsDiagnostics diagnostic={0};
            int inspected=x4_stream_sps_inspect_diagnostic(au->sps,au->sps_bytes,&candidate,&diagnostic);
            if(!inspected)admit_initial_resolution(p,candidate.coded_width,candidate.coded_height);
            else if(inspected==X4_SPS_SIZE&&diagnostic.syntax_complete&&
                (diagnostic.valid_mask&X4_SPS_HAVE_SIZE)&&candidate.frame_mbs_only_flag&&
                candidate.chroma_format_idc==1&&!candidate.separate_colour_plane_flag&&
                candidate.bit_depth_luma==8&&candidate.bit_depth_chroma==8&&
                (candidate.coded_width>1920||candidate.coded_height>1088)&&
                x4_stream_startup_hold_size(&p->startup,candidate.coded_width,candidate.coded_height)){
                note_parameters(p,au);observed_sps(p,&candidate,&diagnostic);
                if(p->startup.dropped_aus==1)printf("XCloud4: stream initial resolution waiting coded=%ux%u max_coded=1920x1088 alias_sent_us=%llu (AU discarded before native startup)\n",
                    candidate.coded_width,candidate.coded_height,(unsigned long long)x4_stream_startup_alias_sent_us(&p->startup));
                /* AU lease retires before the next iteration or worker exit. */continue;
            }
        }
        bool changed=p->active_sps_size&&(p->active_sps_size!=au->sps_bytes||memcmp(p->active_sps,au->sps,au->sps_bytes));
        if(changed){
            note_parameters(p,au);parameters_noted=true;
            X4StreamSps candidate={0};X4StreamSpsDiagnostics diagnostic={0};
            rc=x4_stream_sps_inspect_diagnostic(au->sps,au->sps_bytes,&candidate,&diagnostic);
            observed_sps(p,&candidate,&diagnostic);
            printf("XCloud4: stream SPS change input_AU=%llu code=%d reason=%u stage=%s valid=%llu coded=%ux%u visible=%ux%u complete=%u\n",
                (unsigned long long)au->identity,rc,diagnostic.reason,x4_stream_sps_stage_name(diagnostic.stage),
                (unsigned long long)diagnostic.valid_mask,candidate.coded_width,candidate.coded_height,candidate.visible_width,candidate.visible_height,diagnostic.syntax_complete);
            if(rc){
                if(!recoverable_sps_syntax(rc,&diagnostic)){failure="unsupported changed SPS";break;}
                X4ProbeNativeOutput malformed={.slot=X4_PIPELINE_OUTPUT_CAPACITY,.frame_class=X4_STREAM_FRAME_RECONFIGURE,.control_result=rc};
                rc=frame_error(p,au,&malformed);if(rc){failure="consecutive malformed SPS limit";break;}
                set_waiting_idr(p,true);/* AU lease retires before the next iteration or worker exit. */continue;
            }
            if(!x4_stream_sps_compatible(&candidate,&p->native_info.sps)){
                ++p->reconfiguration_count;
                lock(&p->status_gate);p->view.reconfigurations=p->reconfiguration_count;unlock(&p->status_gate);
                if(native_started){rc=quiesce_native(p);if(rc){failure="SPS quiesce or native delete";break;}native_started=false;}
                else {lock(&p->ring_gate);atomic_store_explicit(&p->reconfiguring,true,memory_order_release);unlock(&p->ring_gate);}
                start_reason=STREAM_SPS_CHANGE;set_waiting_idr(p,true);
                if(!find_nal(au->data,au->bytes,5,NULL,NULL))request_keyframe(p);
            }
            memcpy(p->active_sps,au->sps,au->sps_bytes);p->active_sps_size=au->sps_bytes;
        }
        if(p->waiting_idr&&!find_nal(au->data,au->bytes,5,NULL,NULL)){
            ++p->waiting_idr_drops;/* AU lease retires before the next iteration or worker exit. */continue;
        }
        if(!native_started){
            /* Stop may be published while Delete/quiesce is in progress.
             * Do not create a replacement after the session was cancelled. */
            if(stopping(p)){rc=0;break;}
            if(!parameters_noted)note_parameters(p,au);
            p->video.trace=p->trace;p->native_start_attempted=true;
            X4StreamSps depth_sps={0};
            X4StreamSpsDiagnostics depth_diagnostic={0};
            (void)x4_stream_sps_inspect_diagnostic(au->sps,au->sps_bytes,&depth_sps,&depth_diagnostic);
            unsigned selected_depth=x4_stream_request_resolve_depth(&p->request,depth_sps.coded_width,depth_sps.coded_height);
            uint64_t began=now_us();
#if X4_VIDEO_PROBE && X4_PROBE_T26
            if(p->t26_enabled)
                rc=x4_live_video_t26_raw_start(&p->video,au->sps,au->sps_bytes,&p->cancel,&p->t26_hooks.native);
            else
#endif
            rc=x4_live_video_stream_raw_start_cpu_depth(&p->video,au->sps,au->sps_bytes,&p->cancel,p->request.cpu_variant,selected_depth);
            x4_live_video_stream_info(&p->video,&p->native_info);
            observed_sps(p,&p->native_info.sps,&p->native_info.sps_diagnostics);
            begin_epoch(p,au,start_reason,rc,began);
            if(rc==X4_STREAM_VIDEO_CANCELLED&&stopping(p)){rc=0;break;}
            if(rc){
                if(!p->video.state&&recoverable_sps_syntax(rc,&p->native_info.sps_diagnostics)){
                    X4ProbeNativeOutput malformed={.slot=X4_PIPELINE_OUTPUT_CAPACITY,.frame_class=X4_STREAM_FRAME_RECONFIGURE,.control_result=rc};
                    rc=frame_error(p,au,&malformed);if(rc){failure="consecutive startup SPS limit";break;}
                    set_waiting_idr(p,true);/* AU lease retires before the next iteration or worker exit. */continue;
                }
                failure="native startup or unsupported SPS";break;
            }
            /* Start/recreate owns no outstanding ring reader. Preserve ring
             * generations/statistics; switch only its active count after Query
             * and Create select the actual depth (including fallback). */
            lock(&p->ring_gate);
            if(!x4_video_pipeline_ring_idle(&p->ring))rc=X4_PIPELINE_STATE;
            else p->ring.output_count=p->native_info.active_depth==2?5:4;
            unlock(&p->ring_gate);
            if(rc){failure="depth ring reconfiguration while owned";break;}
            p->last_native_output_us=0;
            printf("XCloud4: stream depth selection=%u requested=%u active=%u fallback=%u outputs=%u inputs=%u budget=%llu auxiliary=%llu\n",
                p->request.decoder_depth,p->native_info.requested_depth,p->native_info.active_depth,p->native_info.depth_fallback_used,
                p->ring.output_count,p->native_info.input_count,(unsigned long long)p->native_info.native_budget_bytes,(unsigned long long)p->native_info.budget_auxiliary_bytes);
            admit_initial_resolution(p,p->native_info.sps.coded_width,p->native_info.sps.coded_height);
            memcpy(p->active_sps,au->sps,au->sps_bytes);p->active_sps_size=au->sps_bytes;
            unsigned received_matrix=p->native_info.sps.matrix_coefficients;
            unsigned effective_matrix=x4_stream_color_resolve(received_matrix,p->native_info.sps.visible_width,
                p->native_info.sps.visible_height,p->request.color_preference);
            lock(&p->ring_gate);p->color=(X4VideoPipelineColor){effective_matrix,p->native_info.sps.full_range!=0,
                p->request.convert_mode,p->request.sharpness};
            atomic_store_explicit(&p->reconfiguring,false,memory_order_release);unlock(&p->ring_gate);
            printf("XCloud4: stream color matrix_received=%u selected_matrix=%u selected=%s full_range=%u warnings=%u epoch=%llu\n",
                received_matrix,p->color.matrix_coefficients,p->color.matrix_coefficients==1?"BT709":"BT601",p->color.full_range,p->native_info.sps_diagnostics.warning_mask,(unsigned long long)p->epoch_count);
            native_started=true;lock(&p->status_gate);p->view.native_ready=true;
            p->view.received_matrix=received_matrix;p->view.effective_matrix=effective_matrix;
            p->view.full_range=p->color.full_range;p->view.color_notice_started_us=now_us();
            p->view.width=p->native_info.sps.visible_width;p->view.height=p->native_info.sps.visible_height;
            p->view.requested_depth=p->request.decoder_depth;p->view.active_depth=p->native_info.active_depth;
            unlock(&p->status_gate);
        }
        /* An IDR may itself be damaged. Admit it without retiring the
         * recovery deadline; only a validated output ends this recovery.
         * A successful no-output IDR may need dependent frames to drain. */
        p->waiting_idr=false;lock(&p->status_gate);p->view.waiting_idr=false;unlock(&p->status_gate);
        X4VideoPipelineLease lease;rc=acquire_native(p,&lease);
        if(rc){if(rc==X4_PIPELINE_WAIT&&stopping(p))rc=0;failure="native output acquisition";break;}
        p->video.trace_au=au->identity;X4ProbeNativeOutput output={0};
        rc=x4_live_video_stream_raw_decode(&p->video,lease.slot,au->data,au->bytes,au->pts,false,&output);
        if(output.valid&&measured_output(p,output.output_id))
            x4_stream_diagnostics_decoder_ownership(p->diagnostics,p->acquired_slot_wait_us,
                p->acquired_ring_gate_us,p->acquired_held_native,p->acquired_occupied,p->acquired_full_drops);
#if X4_VIDEO_PROBE && X4_PROBE_T26
        /* Inspect the returned native bytes before publication/conversion.
         * The observer cannot accept/reject or change any lease. */
        if(p->t26_enabled&&p->t26_hooks.on_decode){
            x4_live_video_stream_info(&p->video,&p->native_info);
            p->t26_hooks.on_decode(p->t26_hooks.context,au->identity,rc,&output,&p->native_info);
        }
#endif
        uint64_t stopped_at=atomic_load_explicit(&p->stop_requested_us,memory_order_acquire);
        /* The raw adapter stamps exactly the SCE call, excluding input copy,
         * validation and cancellation branches that never entered Decode. */
        if(output.decode_end_us&&output.decode_us<=output.decode_end_us&&stopped_at&&stopped_at<output.decode_end_us){
            uint64_t native_call_end=output.decode_end_us,native_call_begin=native_call_end-output.decode_us;
            uint64_t overlap_begin=stopped_at>native_call_begin?stopped_at:native_call_begin;
            p->native_decode_after_stop_us+=native_call_end-overlap_begin;
            if(stopped_at<native_call_begin)++p->native_decode_started_after_stop;
        }
        if(rc==X4_STREAM_VIDEO_CANCELLED&&stopping(p)){rc=0;break;}
        if(output.decode_end_us){p->native_decode_result=output.native_result;duration(p,X4_STREAM_DECODE,output.decode_us);
            duration(p,au->is_idr?X4_STREAM_DECODE_IDR:X4_STREAM_DECODE_NON_IDR,output.decode_us);
            if(output.completed_output){++p->calls_with_output;duration(p,X4_STREAM_DECODE_WITH_OUTPUT,output.decode_us);}
            else {++p->calls_without_output;duration(p,X4_STREAM_DECODE_WITHOUT_OUTPUT,output.decode_us);}
            if(output.native_result)++p->calls_with_error;
            /* isValid=0 is normal pipeline priming, not pause evidence. */
            if(output.completed_output&&output.decode_us>100000)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_DECODER);}
        lock(&p->status_gate);p->view.decode_calls=p->video.decode_calls;p->view.decode_us=p->video.decode_us;
        p->view.decode_max_us=p->video.decode_max_us;unlock(&p->status_gate);
        if(!rc&&p->idr_recovery.waiting&&now_us()>=p->idr_recovery.deadline_us){
            bool ignored=false;rc=x4_stream_idr_poll(&p->idr_recovery,now_us(),&ignored);
            failure="IDR did not produce a valid output within five seconds";break;}
        if(rc||output.frame_class==X4_STREAM_FRAME_FATAL){
            if(output.control_result==X4_SPS_SIZE&&(output.native_width>1920||output.native_height>1088)){
                lock(&p->status_gate);p->view.rejected_width=output.native_width;p->view.rejected_height=output.native_height;unlock(&p->status_gate);}
            output.frame_class=X4_STREAM_FRAME_FATAL;(void)frame_error(p,au,&output);
            if(!rc)rc=X4_PIPELINE_STATE;
            failure="unsafe native output or resources";break;
        }
        if(output.frame_class==X4_STREAM_FRAME_BAD_FRAME){
            rc=discard_bad_output(p,lease,&output);if(rc){failure="damaged output ownership";break;}
            rc=frame_error(p,au,&output);if(rc){failure="consecutive frame error limit";break;}
            set_waiting_idr(p,true);
            /* AU lease retires before the next iteration or worker exit. */continue;
        }
        if(output.frame_class==X4_STREAM_FRAME_RECOVERABLE_RESTART||output.frame_class==X4_STREAM_FRAME_RECONFIGURE){
            rc=frame_error(p,au,&output);if(rc){failure="consecutive native error limit";break;}
            /* Unknown native acceptance/error never permits immediate reuse.
             * The old Decode has returned, converters finish, then Delete
             * retires every accepted output and both retained input buffers. */
            rc=quiesce_native(p);if(rc){failure="frame error quiesce or native delete";break;}
            native_started=false;++p->restart_count;start_reason=STREAM_FRAME_RESTART;set_waiting_idr(p,true);
            /* AU lease retires before the next iteration or worker exit. */continue;
        }
        if(output.valid&&!output.layout_verified){rc=X4_PIPELINE_STATE;failure="unverified native output layout";break;}
        if(output.valid&&p->idr_recovery.waiting)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_WAIT_IDR);
        rc=accept_native(p,lease,&output);if(rc){failure="native publication ownership";break;}
        if(output.valid){
            /* During IDR recovery depth two may return the preceding picture.
             * It is not an error and does not establish input/output identity. */
            /* No output PTS: this is an accepted IDR input with a validated
             * output in the same call, not proof of output/input identity. */
            if(au->is_idr&&output.accepted)x4_keyframe_limiter_observe_idr(p->keyframe_limiter);
            unsigned path=0;rc=x4_video_pipeline_color_path(&output.layout,output.returned_bytes,&path);
            if(rc){failure="conversion geometry or source extent";break;}
            if(!p->last_native_output_us)++p->native_output_epochs;
            if(!p->first_native_output_us)p->first_native_output_us=output.decode_end_us;
            p->last_valid_native_output_us=output.decode_end_us;
            if(p->last_native_output_us&&output.decode_end_us>=p->last_native_output_us)
                duration(p,X4_STREAM_NATIVE_OUTPUT_INTERVAL,output.decode_end_us-p->last_native_output_us);
            p->last_native_output_us=output.decode_end_us;
            if(!p->conversion_path_seen||path!=p->conversion_path)printf("XCloud4: stream conversion=%s visible=%ux%u coded=%ux%u storage_height=%u pitch=%u\n",path==X4_PIPELINE_CONVERT_SSE2?"SSE2":"scalar",output.layout.visible_width,output.layout.visible_height,output.layout.coded_width,output.layout.coded_height,output.layout.storage_height,output.layout.pitch);
            p->conversion_path=path;p->conversion_path_seen=true;
            if(path==X4_PIPELINE_CONVERT_SCALAR)++p->scalar_outputs;
            set_waiting_idr(p,false);
            p->consecutive_errors=p->consecutive_native_errors=0;lock(&p->status_gate);++p->view.outputs;p->view.consecutive_frame_errors=0;
            p->view.width=output.layout.visible_width;p->view.height=output.layout.visible_height;
            p->view.native_width=output.native_width;p->view.native_height=output.native_height;p->view.pitch=output.pitch;
            p->view.latest_picture_us=output.decode_end_us;unlock(&p->status_gate);
        }
        /* The producer may reuse this AU and parameter storage only now. */
        /* AU lease retires before the next iteration or worker exit. */
    }
    {int released=retire_au(p,au_lease,&au_active);if(!rc)rc=released;}
    x4_stream_startup_cancel(&p->startup);
    if(rc==X4_STREAM_VIDEO_CANCELLED&&stopping(p))rc=0;
    if(rc)fail_named(p,rc,failure);
    atomic_store_explicit(&p->decoder_done,true,memory_order_release);
    (void)x4_stream_wait_notify(&p->present_wait);
    while(!atomic_load_explicit(&p->cleanup_allowed,memory_order_acquire))sceKernelUsleep(100);
    /* GPU timeouts do not revoke the native input lease. Main has joined all
     * CPU converters before cleanup_allowed, so a remaining reader is GPU.
     * Retain the decoder instead of Delete/free while its pixels are read. */
    if(atomic_load_explicit(&p->conversion_readers,memory_order_acquire)){
        p->native_teardown_failed=true;p->decoder_close_result=X4_PROBE_STACK_RETAINED;
    }else
    if(p->native_teardown_failed||(!native_started&&p->video.state
#if X4_VIDEO_PROBE && X4_PROBE_T26
        /* T26 start deliberately leaves partial allocations to its caller.
         * Let the sole Decode owner attempt the ordinary safe stop: it still
         * rejects uncertain Create handles, failed Delete and live readers. */
        &&!p->t26_enabled
#endif
        ))
        p->decoder_close_result=p->decoder_close_result?p->decoder_close_result:(p->native_info.close_result?p->native_info.close_result:X4_PROBE_STACK_RETAINED);
    else {
#if X4_VIDEO_PROBE && X4_PROBE_T26
        p->decoder_close_result=t26_stop_native(p,&p->native_stop_us);
#else
        uint64_t began=now_us();p->decoder_close_result=x4_live_video_stop(&p->video);
        p->native_stop_us=now_us()-began;
#endif
    }
    x4_live_video_stream_info(&p->video,&p->native_info);close_epoch(p,p->decoder_close_result);
    if(!p->decoder_close_result&&p->video.state)p->decoder_close_result=X4_PIPELINE_STATE;
    if(!p->decoder_close_result){
        lock(&p->ring_gate);(void)x4_video_pipeline_cancel_ready(&p->ring);
        for(unsigned i=0;i<p->ring.output_count;++i)if(p->ring.slots[i].state==X4_PIPELINE_DECODING){X4VideoPipelineLease old;
            if(!x4_video_pipeline_decoding_lease(&p->ring,i,&old))(void)x4_video_pipeline_abort_decode(&p->ring,old);}
        unlock(&p->ring_gate);
    }
    atomic_store_explicit(&p->decoder_finished,true,memory_order_release);return NULL;
}
X4StreamPipeline *x4_stream_pipeline_create(uint64_t session,X4Trace *trace,const atomic_bool *stop)
{
    X4StreamRequestConfig request=x4_stream_settings_get();
    return x4_stream_pipeline_create_request(session,trace,stop,&request);
}
X4StreamPipeline *x4_stream_pipeline_create_request(uint64_t session,X4Trace *trace,const atomic_bool *stop,const X4StreamRequestConfig *request)
{
    if(!session||!stop||!request || (unsigned)request->profile > X4_REQUEST_1080P_C ||
        (unsigned)request->color_preference > X4_COLOR_FORCE_601||request->cpu_variant>X4_CPU_3||request->decoder_depth>X4_DEPTH_2)return NULL;
    X4StreamPipeline *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->session=session;p->started=now_us();p->trace=trace;p->external_stop=stop;p->display_handle=-1;
    p->request=x4_stream_request_capture(request);
    bool synced=x4_stream_request_vsync_policy(&p->request)==X4_VSYNC_SYNCED;
    p->scanout_count=synced||x4_stream_request_mailbox(&p->request)?4u:3u;
    x4_video_pipeline_mailbox_clock_init(&p->mailbox_clock);
    x4_video_pipeline_sync_history_init(&p->synced_history);
    atomic_init(&p->verification_priority_result,X4_GPU_CONVERT_NOT_RUN);
    atomic_init(&p->verification_stop,false);atomic_init(&p->verification_finished,false);
#if !X4_PUBLIC
#if X4_VIDEO_PROBE && X4_PROBE_T26
    /* Probe construction never consults or mutates the catalog preferences. */
    p->gpu_metrics.requested=1u;
    p->gpu_quality_requested_word=0u;
#else
    p->gpu_metrics.requested=x4_stream_settings_conversion_backend();
    p->gpu_quality_requested_word=x4_gpu_quality_settings_get();
#endif
#else
    p->gpu_metrics.requested=x4_stream_request_gpu_force_cpu(&p->request)?0u:1u;
    p->gpu_quality_requested_word=x4_stream_request_dither_enabled(&p->request)?X4_GPU_QUALITY_DITHER:0u;
#endif
    p->gpu_metrics.quality_requested_word=p->gpu_quality_requested_word;
    p->gpu_metrics.quality_reason=p->gpu_metrics.requested?"awaiting_native_layout":
        p->gpu_quality_requested_word?"ignored_cpu":"off";
    p->gpu_metrics.verification_priority_result=p->gpu_metrics.verification_last_result=
        p->gpu_metrics.verification_reserve_result=p->gpu_metrics.verification_map_result=
        p->gpu_metrics.verification_control_reserve_result=p->gpu_metrics.verification_control_map_result=X4_GPU_CONVERT_NOT_RUN;
    p->gpu_metrics.quality_reserve_result=p->gpu_metrics.quality_map_result=
        p->gpu_metrics.quality_unmap_result=p->gpu_metrics.quality_release_result=
        p->gpu_metrics.intermediate_reserve_result=p->gpu_metrics.intermediate_map_result=
        p->gpu_metrics.intermediate_unmap_result=p->gpu_metrics.intermediate_release_result=X4_GPU_CONVERT_NOT_RUN;
    p->gpu_metrics.reason=p->gpu_metrics.requested?"awaiting_native_layout":"not_requested";
    p->gpu_metrics.reserve_result=p->gpu_metrics.map_result=
        p->gpu_metrics.submit_result=p->gpu_metrics.submit_done_result=
        p->gpu_metrics.allowed_result=X4_GPU_CONVERT_NOT_RUN;
    p->color.convert_mode=p->request.convert_mode;p->color.sharpness=p->request.sharpness;
    x4_stream_startup_init_mode(&p->startup,x4_stream_request_resolution_alias(&p->request)!=NULL,
        p->request.resolution_request_mode & X4_RESOLUTION_REQUEST_MODE_MASK);
    p->display_allocate=p->display_map=p->display_register=p->display_rate=
        p->display_close=p->display_unmap=p->display_release=X4_PROBE_STACK_NOT_RUN;
    p->native_decode_result=X4_PROBE_STACK_NOT_RUN;
    p->native_info.compute_query_result=p->native_info.allocate_queue_result=
        p->native_info.query_result=p->native_info.create_result=
        p->native_info.delete_decoder_result=p->native_info.release_queue_result=
        p->native_info.close_result=X4_PROBE_STACK_NOT_RUN;
    atomic_init(&p->callback_rejected,0);atomic_init(&p->callback_full_design_failures,0);
    atomic_init(&p->post_halt_video_packets,0);atomic_init(&p->first_post_halt_video_us,0);
    atomic_init(&p->submitted,0);atomic_init(&p->consumed,0);atomic_init(&p->cancel,false);atomic_init(&p->menu_open,false);
    atomic_init(&p->dequeued,0);
    atomic_init(&p->current_au,0);atomic_init(&p->stop_requested_us,0);atomic_init(&p->keyframe_pending,false);
    atomic_init(&p->reconfiguring,false);atomic_init(&p->conversion_readers,0);
    atomic_init(&p->keyframe_created,0);atomic_init(&p->keyframe_coalesced,0);atomic_init(&p->keyframe_taken,0);
    p->view.display_detached=true;
    atomic_init(&p->cleanup_allowed,false);atomic_init(&p->decoder_done,false);atomic_init(&p->decoder_finished,false);
    atomic_init(&p->error,0);atomic_init(&p->admission_failure,0);atomic_init(&p->diagnostic_calls,0);atomic_init(&p->diagnostic_us,0);atomic_init(&p->diagnostic_max_us,0);
    int sync_rc=x4_stream_mutex_init(&p->ring_gate,"x4-stream-ring");
    if(sync_rc){printf("XCloud4: stream sync init ring mutex rc=%d\n",sync_rc);goto memory_fail;}
    p->ring_mutex_initialized=true;
    sync_rc=x4_stream_mutex_init(&p->status_gate,"x4-stream-status");
    if(sync_rc){printf("XCloud4: stream sync init status mutex rc=%d\n",sync_rc);goto memory_fail;}
    p->status_mutex_initialized=true;
    sync_rc=x4_stream_wait_init(&p->decode_wait,"x4-stream-au");
    if(sync_rc){printf("XCloud4: stream sync init decode semaphore rc=%d\n",sync_rc);goto memory_fail;}
    sync_rc=x4_stream_wait_init(&p->present_wait,"x4-stream-main");
    if(sync_rc){printf("XCloud4: stream sync init main semaphore rc=%d\n",sync_rc);goto memory_fail;}
    sync_rc=x4_stream_wait_init(&p->verification_wait,"x4-gpu-verify");
    if(sync_rc)goto memory_fail;
    for(unsigned i=0;i<2;++i){sync_rc=x4_stream_wait_init(&p->converters[i].work_wait,
        i?"x4-stream-c1":"x4-stream-c0");
        if(sync_rc){printf("XCloud4: stream sync init converter %u semaphore rc=%d\n",i,sync_rc);goto memory_fail;}}
    if(x4_video_pipeline_ring_init(&p->ring,session))goto memory_fail;
    int display_init=synced?x4_video_pipeline_display_init_synced(&p->display,session):
        x4_video_pipeline_display_init_mode(&p->display,session,p->scanout_count,p->scanout_count==4u);
    if(display_init)goto memory_fail;
    p->au_queue=x4_stream_au_queue_create();if(!p->au_queue)goto memory_fail;
    p->diagnostics=x4_stream_diagnostics_create(p->started);if(!p->diagnostics)goto memory_fail;
    x4_stream_diagnostics_vsync_init(p->diagnostics,(unsigned)x4_stream_request_vsync_policy(&p->request));
    for(unsigned i=0;i<2;++i){StreamConverter *w=&p->converters[i];w->owner=p;w->index=i;
        atomic_init(&w->submitted,0);atomic_init(&w->completed,0);atomic_init(&w->stop,false);atomic_init(&w->finished,false);}
    return p;
memory_fail:
    x4_stream_au_queue_stop(p->au_queue);
    (void)x4_stream_au_queue_destroy(p->au_queue);
    for(unsigned i=0;i<2;++i)(void)x4_stream_wait_destroy(&p->converters[i].work_wait);
    (void)x4_stream_wait_destroy(&p->decode_wait);(void)x4_stream_wait_destroy(&p->present_wait);
    (void)x4_stream_wait_destroy(&p->verification_wait);
    if(p->status_mutex_initialized)(void)x4_stream_mutex_destroy(&p->status_gate);
    if(p->ring_mutex_initialized)(void)x4_stream_mutex_destroy(&p->ring_gate);
    x4_stream_diagnostics_free(p->diagnostics);free(p);return NULL;
}
#if X4_VIDEO_PROBE && X4_PROBE_T26
X4StreamPipeline *x4_stream_pipeline_t26_create(uint64_t session,
    const atomic_bool *stop,const X4T26PipelineHooks *hooks)
{
    if(!hooks||!hooks->native.before||!x4_t26_config_valid(&hooks->native.config)||
        hooks->native.config.api!=X4_T26_API_V2||
        !hooks->native.config.realistic||hooks->native.config.depth<1u||
        hooks->native.config.depth>2u)return NULL;
    /* No network request/alias is involved in local replay. Geometry comes
     * from the clip SPS and native.config is the exact measured cell. */
    X4StreamRequestConfig request=x4_stream_request_config(X4_REQUEST_720P60,X4_COLOR_AUTO_709);
    request.cpu_variant=X4_CPU_0;
    request.decoder_depth=(unsigned char)hooks->native.config.depth;
    request.interaction_modes=X4_INTERACTION_VSYNC_ALWAYS;
    request.convert_mode=1;request.sharpness=2;
    X4StreamPipeline *p=x4_stream_pipeline_create_request(session,NULL,stop,&request);
    if(p){p->t26_hooks=*hooks;p->t26_enabled=true;}
    return p;
}
int x4_stream_pipeline_t26_set_present_policy(X4StreamPipeline *p,unsigned mode,bool mailbox)
{
    if(!p||!p->t26_enabled||mode>2u||p->display_handle>=0||p->decoder_thread.created||
        p->converters[0].thread.created||p->converters[1].thread.created)return X4_PIPELINE_STATE;
#if X4_PUBLIC
    mailbox=false;
#endif
    p->scanout_count=mailbox?4u:3u;
    int rc=x4_video_pipeline_display_init_mode(&p->display,p->session,p->scanout_count,mailbox);
    if(!rc){x4_stream_diagnostics_vsync_init(p->diagnostics,mode);x4_video_pipeline_mailbox_clock_init(&p->mailbox_clock);}return rc;
}
int x4_stream_pipeline_t26_set_measure_window(X4StreamPipeline *p,uint64_t first,uint64_t last)
{
    if(!p||!p->t26_enabled||!first||last<first||p->decoder_thread.created)return X4_PIPELINE_INVALID;
    p->measure_first=first;p->measure_last=last;return 0;
}
bool x4_stream_pipeline_t26_presentation_summary(const X4StreamPipeline *p,X4PipelinePresentationSummary *out)
{
    if(!x4_stream_pipeline_t26_observers_stopped(p)||!out||
        !x4_stream_diagnostics_presentation_summary(p->diagnostics,out))return false;
    out->mailbox_replaced=p->display.mailbox_replaced;out->mailbox_discarded_old=p->display.mailbox_discarded_old;
    out->mailbox_late_opportunities=p->mailbox_clock.missed_opportunities;out->mailbox_late_flips=p->mailbox_late_flips;
    out->clock_anchors=p->mailbox_clock.anchors;out->clock_invalid_anchors=p->mailbox_clock.invalid_anchors;
    out->clock_residual_samples=p->mailbox_clock.residual_samples;out->clock_residual_sum_us=p->mailbox_clock.residual_sum_us;
    out->clock_residual_max_us=p->mailbox_clock.residual_max_us;out->clock_period_us=p->mailbox_clock.period_us;
    out->clock_margin_us=p->mailbox_clock.margin_us;out->clock_warmup_submissions=p->mailbox_clock.warmup_submissions;
    out->scanouts=p->scanout_count;out->scanout_bytes=(uint64_t)p->scanout_count*STREAM_SCANOUT_BYTES;return true;
}
bool x4_stream_pipeline_t26_observers_stopped(const X4StreamPipeline *p)
{
    return p&&p->closed&&!p->decoder_thread.created&&
        !p->converters[0].thread.created&&!p->converters[1].thread.created&&!p->verification_thread.created;
}
bool x4_stream_pipeline_t26_gpu_status(const X4StreamPipeline *p,
    bool *requested,uint64_t *gpu_frames,uint64_t *cpu_frames,char reason[64])
{
    if(!requested||!gpu_frames||!cpu_frames||!reason||
        !x4_stream_pipeline_t26_observers_stopped(p))return false;
    *requested=p->gpu_metrics.requested!=0;
    *gpu_frames=p->gpu_metrics.gpu_frames;*cpu_frames=p->gpu_metrics.cpu_frames;
    snprintf(reason,64,"%s",p->gpu_metrics.reason?p->gpu_metrics.reason:"unknown");
    return true;
}
#endif
void x4_stream_pipeline_set_keyframe_limiter(X4StreamPipeline *p,X4KeyframeLimiter *limiter)
{if(p)p->keyframe_limiter=limiter;}
X4StreamStartup *x4_stream_pipeline_startup(X4StreamPipeline *p)
{return p?&p->startup:NULL;}
int x4_stream_pipeline_start(X4StreamPipeline *p)
{
    if(!p||p->decoder_thread.created||stopping(p))return X4_PIPELINE_STATE;
    for(unsigned i=0;i<2;++i){StreamConverter *w=&p->converters[i];int rc=x4_probe_thread_create(&w->id,&w->thread,stream_pipeline_convert_worker,w,i?"x4-stream-c1":"x4-stream-c0");if(rc)return fail_named(p,rc,"converter thread creation");}
    int rc=x4_probe_thread_create(&p->decoder,&p->decoder_thread,stream_pipeline_decode_worker,p,"x4-stream-decode");
    return rc?fail_named(p,rc,"decode thread creation"):0;
}
int x4_stream_pipeline_submit(X4StreamPipeline *p,const uint8_t *au,size_t bytes,uint64_t pts,uint64_t identity,const uint8_t *sps,size_t sps_bytes)
{return x4_stream_pipeline_submit_parameters(p,au,bytes,pts,identity,sps,sps_bytes,NULL,0);}
int x4_stream_pipeline_submit_parameters(X4StreamPipeline *p,const uint8_t *au,size_t bytes,uint64_t pts,uint64_t identity,const uint8_t *sps,size_t sps_bytes,const uint8_t *pps,size_t pps_bytes)
{
    if(!p||!au||!bytes||bytes>X4_LIVE_AU_MAX||!sps||!sps_bytes||sps_bytes>STREAM_SPS_BYTES||
        (pps_bytes&&!pps)||pps_bytes>STREAM_SPS_BYTES)return X4_PIPELINE_INVALID;
    int error=atomic_load_explicit(&p->error,memory_order_acquire);
    if(error)return error;
    if(stopping(p))return X4_STREAM_AU_STOPPED;
    if(atomic_load_explicit(&p->submitted,memory_order_relaxed)==UINT64_MAX)
        return fail_admission(p,X4_PIPELINE_EXHAUSTED,1);
    uint64_t now=now_us();
    X4StreamAuInput input={.data=au,.sps=sps,.pps=pps,.bytes=bytes,
        .sps_bytes=sps_bytes,.pps_bytes=pps_bytes,.pts_us=pts,.identity=identity,
        .arrival_us=now,.is_idr=find_nal(au,bytes,5,NULL,NULL),
        .rtp_timestamp_valid=pts<=((uint64_t)UINT32_MAX*1000000/90000)};
    if(input.rtp_timestamp_valid)input.rtp_timestamp=(uint32_t)((pts*9+99)/100);
    X4StreamAuOutcome outcome={0};
    int rc=x4_stream_au_queue_try_push(p->au_queue,&input,now,&outcome);
    queue_signals(p,&outcome);
    if(rc==X4_STREAM_AU_OK){atomic_fetch_add_explicit(&p->submitted,1,memory_order_release);
        int notified=x4_stream_wait_notify(&p->decode_wait);
        if(notified)return fail_admission(p,notified,2);}
    else if(rc<0)return fail_admission(p,rc,2);
    return rc;
}
void x4_stream_pipeline_received(X4StreamPipeline *p,size_t bytes,uint64_t now)
{
    if(p){
        x4_stream_startup_note_video_packet(&p->startup);
        if(!atomic_load_explicit(&p->first_video_rtp_us,memory_order_relaxed)){
            uint64_t unset=0;
            (void)atomic_compare_exchange_strong_explicit(&p->first_video_rtp_us,&unset,now,memory_order_release,memory_order_relaxed);
        }
        x4_stream_diagnostics_received(p->diagnostics,bytes,now);
    }
}
void x4_stream_pipeline_note_post_halt_video(X4StreamPipeline *p,uint64_t now)
{
    if(p){
        atomic_fetch_add_explicit(&p->post_halt_video_packets,1,memory_order_relaxed);
        uint64_t empty=0;
        (void)atomic_compare_exchange_strong_explicit(&p->first_post_halt_video_us,&empty,now,
            memory_order_release,memory_order_relaxed);
    }
}
void x4_stream_pipeline_au_received(X4StreamPipeline *p,bool idr,uint64_t now)
{if(p)x4_stream_diagnostics_au_received(p->diagnostics,idr,now);}
void x4_stream_pipeline_rtp_gap(X4StreamPipeline *p,uint64_t now)
{(void)now;if(p)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_RTP_GAP);}
void x4_stream_pipeline_note_keyframe_request(X4StreamPipeline *p,uint64_t now)
{if(p){
    if(atomic_exchange_explicit(&p->keyframe_pending,false,memory_order_acq_rel))
        atomic_fetch_add_explicit(&p->keyframe_taken,1,memory_order_relaxed);
    x4_stream_diagnostics_keyframe_request(p->diagnostics,now);
}}
bool x4_stream_pipeline_take_keyframe_request(X4StreamPipeline *p)
{
    if(!p||!atomic_exchange_explicit(&p->keyframe_pending,false,memory_order_acq_rel))return false;
    atomic_fetch_add_explicit(&p->keyframe_taken,1,memory_order_relaxed);return true;
}
void x4_stream_pipeline_request_stop(X4StreamPipeline *p)
{if(p&&!p->closed){uint64_t unset=0,requested=now_us();
    (void)atomic_compare_exchange_strong_explicit(&p->stop_requested_us,&unset,requested,memory_order_release,memory_order_relaxed);
    atomic_store_explicit(&p->cancel,true,memory_order_release);x4_stream_au_queue_stop(p->au_queue);wake_all(p);}}
void x4_stream_pipeline_note_callback_reject(X4StreamPipeline *p,bool queue_full)
{
    if(!p)return;
    atomic_fetch_add_explicit(&p->callback_rejected,1,memory_order_relaxed);
    if(queue_full)atomic_fetch_add_explicit(&p->callback_full_design_failures,1,memory_order_relaxed);
}
void x4_stream_pipeline_note_ingress_stall_recovery(X4StreamPipeline *p)
{
    if(!p)return;
    atomic_fetch_add_explicit(&p->ingress_stall_recoveries,1,memory_order_relaxed);
}
uint64_t x4_stream_pipeline_ingress_stall_recoveries(const X4StreamPipeline *p)
{
    if(!p)return 0;
    return atomic_load_explicit(&p->ingress_stall_recoveries,memory_order_relaxed);
}
bool x4_stream_pipeline_try_snapshot(X4StreamPipeline *p,X4StreamPipelineSnapshot *out)
{
    if(!p||!out)return false;
    /* closed is published only by the close owner after every CPU producer
     * joined. A partially failed free may already have destroyed these gates;
     * its retained numerical context is cold and must not lock them again. */
    if(!p->closed&&x4_stream_mutex_trylock(&p->status_gate))return false;
    X4StreamPipelineSnapshot view=p->view;if(!p->closed)unlock(&p->status_gate);
    if(!p->closed&&x4_stream_mutex_trylock(&p->ring_gate))return false;
    view.discarded_latency=p->ring.discarded_latency;view.discarded_ring_full=p->ring.discarded_ready;
    if(!p->closed)unlock(&p->ring_gate);
    view.accepted_aus=atomic_load_explicit(&p->submitted,memory_order_acquire);
    view.error=atomic_load_explicit(&p->error,memory_order_acquire);
    view.reconfiguring=atomic_load_explicit(&p->reconfiguring,memory_order_acquire);
    view.keyframe_requests=atomic_load_explicit(&p->keyframe_taken,memory_order_relaxed);
    *out=view;return true;
}
void x4_stream_pipeline_snapshot(X4StreamPipeline *p,X4StreamPipelineSnapshot *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));if(!p)return;
    if(!p->closed)lock(&p->status_gate);
    *out=p->view;if(!p->closed)unlock(&p->status_gate);
    out->accepted_aus=atomic_load_explicit(&p->submitted,memory_order_acquire);
    out->error=atomic_load_explicit(&p->error,memory_order_acquire);
    out->reconfiguring=atomic_load_explicit(&p->reconfiguring,memory_order_acquire);
    out->keyframe_requests=atomic_load_explicit(&p->keyframe_taken,memory_order_relaxed);
    if(!p->closed)lock(&p->ring_gate);
    out->discarded_latency=p->ring.discarded_latency;out->discarded_ring_full=p->ring.discarded_ready;
    if(!p->closed)unlock(&p->ring_gate);
}
static void gpu_prepare(X4StreamPipeline *p);
int x4_stream_pipeline_display_open(X4StreamPipeline *p)
{
    if(!p||p->display_handle>=0||p->closed||p->video_halted||stopping(p))return X4_PIPELINE_STATE;
    p->display_handle=sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,ORBIS_VIDEO_OUT_BUS_MAIN,0,NULL);
    if(p->display_handle<0)return fail_named(p,p->display_handle,"VideoOut open");
    lock(&p->status_gate);p->view.display_detached=false;unlock(&p->status_gate);
    int rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),(size_t)p->scanout_count*STREAM_SCANOUT_BYTES,0x200000,3,&p->display_offset);
    p->display_allocate=rc;if(rc)return fail_named(p,rc,"scanout direct allocation");p->display_allocated=true;
    rc=sceKernelMapDirectMemory(&p->display_memory,(size_t)p->scanout_count*STREAM_SCANOUT_BYTES,0x33,0,p->display_offset,0x200000);p->display_map=rc;if(rc)return fail_named(p,rc,"scanout direct map");
    memset(p->display_memory,0,(size_t)p->scanout_count*STREAM_SCANOUT_BYTES);
    for(unsigned i=0;i<p->scanout_count;++i)p->display_pixels[i]=(uint32_t*)((uint8_t*)p->display_memory+i*STREAM_SCANOUT_BYTES);
    OrbisVideoOutBufferAttribute attribute={0};
    sceVideoOutSetBufferAttribute(&attribute,ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,1920,1080,1920);
    rc=sceVideoOutRegisterBuffers(p->display_handle,0,(void*const*)p->display_pixels,p->scanout_count,&attribute);p->display_register=rc;if(rc)return fail_named(p,rc,"VideoOut register");
    rc=sceVideoOutSetFlipRate(p->display_handle,ORBIS_VIDEO_OUT_FLIP_60HZ);p->display_rate=rc;if(rc)return fail_named(p,rc,"VideoOut flip rate");
    gpu_prepare(p);
    lock(&p->status_gate);p->view.display_open=true;unlock(&p->status_gate);return 0;
}
static int observe_display(X4StreamPipeline *p)
{
    if(p->display.pending<0)return 0;
    OrbisVideoOutFlipStatus status={0};int rc=sceVideoOutGetFlipStatus(p->display_handle,&status);if(rc)return rc;
    X4VideoPipelineDisplayOccupancy occupancy;
    if(!x4_video_pipeline_display_occupancy(&p->display,&occupancy))
        x4_stream_diagnostics_pacing_queue(p->diagnostics,occupancy.ready,
            status.numFlipPending,status.numGpuFlipPending);
    uint64_t now=now_us();X4VideoPipelineLease shown;
    rc=x4_video_pipeline_observe_flip(&p->display,(uint64_t)status.flipArg,now,&shown);
    if(rc==X4_PIPELINE_WAIT)return now-p->display.slots[p->display.pending].submitted_us>STREAM_TIMEOUT_US?X4_PIPELINE_STATE:0;
    if(rc)return rc;
    const X4VideoPipelineScanout *s=&p->display.slots[shown.slot];
    if(p->notice_scanout[shown.slot]){
        p->notice_scanout[shown.slot]=false;
        p->display.mailbox_capable=p->notice_mailbox_capable;
        p->display.mailbox_admission=p->notice_mailbox_admission;
        p->display.synced=p->notice_synced;
        p->notice_mode_active=false;
        /* This is the same picture with UI, not a newly decoded/presented AU.
         * Do not refresh latency/output cadence timestamps or histograms. */
        return 0;
    }
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(p->t26_enabled&&p->t26_hooks.on_flip)
        p->t26_hooks.on_flip(p->t26_hooks.context,s->output_id,s->decode_end_us,s->submitted_us,now);
#endif
    if(p->scanout_menu[shown.slot])++p->menu_presentations;
    p->last_flip_ptime=status.ptime;p->last_flip_submitted_us=s->submitted_us;p->last_flip_observed_us=now;
    p->last_flip_mode=s->presentation_mode;p->last_flip_measured=measured_output(p,s->output_id);
    if(p->last_flip_measured)x4_stream_diagnostics_presentation(p->diagnostics,s->presentation_mode,
        now-s->decode_end_us,now-s->submitted_us,now);
    if(p->last_flip_measured)x4_stream_diagnostics_pacing_observe(p->diagnostics,s->presentation_mode,
        p->scanout_ready_us[shown.slot]?p->scanout_ready_us[shown.slot]:s->converted_us,
        status.ptime,now,p->mailbox_clock.period_us);
    if(p->display.synced){
        rc=x4_video_pipeline_synced_observed(&p->display,&p->synced_history,now);
        if(rc)return rc;
    }
    x4_video_pipeline_mailbox_anchor(&p->mailbox_clock,status.ptime,s->submitted_us,now,
        s->presentation_mode!=X4_PRESENT_HSYNC);
    if(s->presentation_mode==X4_PRESENT_VSYNC_MAILBOX&&s->target_vblank_us&&
        status.ptime>s->target_vblank_us+p->mailbox_clock.period_us/2)++p->mailbox_late_flips;
    /* SDK ptime occupies the flip process-time field. Accept it only in the
     * submitted..observed process-time interval. Invalid/unknown clock values
     * are omitted, never silently replaced by the later observation time. */
    if(p->flip_to_conversion_pending)++p->flip_to_conversion_pending_omitted;
    p->flip_to_conversion_pending=false;
    if(status.ptime>=s->submitted_us&&status.ptime<=now&&status.ptime){
        p->flip_end_us=status.ptime;p->flip_to_conversion_pending=true;++p->flip_time_valid;
    }else ++p->flip_time_invalid;
    duration(p,X4_STREAM_DECODE_TO_FLIP,now-s->decode_end_us);duration(p,X4_STREAM_SUBMIT_TO_OBSERVATION,now-s->submitted_us);
    if(now-s->submitted_us>100000)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_PRESENTATION);
    x4_stream_diagnostics_present(p->diagnostics,now);
    lock(&p->status_gate);++p->view.presentations;unlock(&p->status_gate);return 0;
}
static int submit_display(X4StreamPipeline *p)
{
    X4VideoPipelineLease ready;int rc=x4_video_pipeline_ready_scanout(&p->display,&ready);
    if(rc==X4_PIPELINE_WAIT)return 0;
    if(rc)return rc;
    bool vsync=x4_stream_diagnostics_vsync_effective(p->diagnostics);
    bool synced=p->display.synced&&vsync;
    bool mailbox=!synced&&p->display.mailbox_capable&&vsync;
    if(synced||(p->notice_mode_active&&p->notice_synced)){
        OrbisVideoOutFlipStatus status={0};
        rc=sceVideoOutGetFlipStatus(p->display_handle,&status);if(rc)return rc;
        X4VideoPipelineDisplayOccupancy occupancy;
        if(!x4_video_pipeline_display_occupancy(&p->display,&occupancy))
            x4_stream_diagnostics_pacing_queue(p->diagnostics,occupancy.ready,
                status.numFlipPending,status.numGpuFlipPending);
        /* The native queue and our lease must both be empty before another
         * VSYNC. Retain READY while an external/native GPU flip is pending. */
        if(status.numFlipPending!=0||status.numGpuFlipPending!=0)return 0;
    }
    uint64_t begin=now_us(),target=0;
    /* Only the validated typed FlipStatus clock is used. Unknown/stale phase
     * submits a conservative VSYNC warmup, never a guessed HSYNC deadline. */
    if(mailbox){rc=x4_video_pipeline_mailbox_due(&p->mailbox_clock,begin,p->display.ready_since_us,&target);
        if(rc==X4_PIPELINE_WAIT)return 0;if(rc)return rc;}
    if(next_stream_flip>=(uint64_t)INT64_MAX)return X4_PIPELINE_EXHAUSTED;
    uint64_t arg=next_stream_flip++;
    int mode=vsync?ORBIS_VIDEO_OUT_FLIP_VSYNC:ORBIS_VIDEO_OUT_FLIP_HSYNC;
    rc=sceVideoOutSubmitFlip(p->display_handle,ready.slot,mode,(int64_t)arg);
    uint64_t end=now_us();if(!p->notice_scanout[ready.slot])duration(p,X4_STREAM_FLIP_CALL,end-begin);if(rc)return rc;
    X4VideoPipelineScanout *slot=&p->display.slots[ready.slot];
    slot->presentation_mode=synced?X4_PRESENT_VSYNC_SYNC:
        vsync?(mailbox?X4_PRESENT_VSYNC_MAILBOX:X4_PRESENT_VSYNC_COLA):X4_PRESENT_HSYNC;
    slot->target_vblank_us=target;
    if(!p->notice_scanout[ready.slot])duration(p,X4_STREAM_CONVERT_TO_SUBMIT,begin-slot->converted_us);
    rc=x4_video_pipeline_mark_submitted(&p->display,ready,arg,end);return rc?rc:sceGnmSubmitDone();
}
static void gpu_status(X4StreamPipeline *p)
{
    if(!p->gpu)return;
    X4GpuConvertStatus s={0};x4_gpu_convert_status(p->gpu,&s);
    X4StreamGpuDiagnostics *g=&p->gpu_metrics;
    g->prepared=s.prepared;g->disabled=g->disabled||s.disabled;g->pending=s.pending;g->quarantined=s.quarantined;
    g->timeouts=s.timeouts;g->quarantines=s.quarantines;g->late_completed=s.late_completed;
    g->arena_bytes=(unsigned)s.arena_bytes;g->reserve_result=s.reserve_result;g->map_result=s.map_result;
    g->submit_result=s.submit_result;g->submit_done_result=s.submit_done_result;
    g->allowed_result=s.allowed_result;
    g->checks=s.verification_checks;g->checked_pixels=s.verification_pixels;
    g->pixels_outside=s.verification_outside;g->max_channel_difference=s.verification_max_difference;
    g->verification_errors=s.verification_errors;g->verification_captures=s.verification_captures;
    g->verification_skipped=s.verification_skipped;g->verification_prepared=s.verification_prepared;
    g->verification_failed=s.verification_failed;g->verification_native_bytes=s.verification_native_bytes;
    g->verification_control_bytes=s.verification_control_bytes;g->verification_priority_result=atomic_load_explicit(&p->verification_priority_result,memory_order_acquire);
    g->verification_last_result=s.verification_last_result;g->verification_reserve_result=s.verification_reserve_result;
    g->verification_map_result=s.verification_map_result;g->verification_control_reserve_result=s.verification_control_reserve_result;
    g->verification_control_map_result=s.verification_control_map_result;
    if(s.verification_checks>p->verification_status_checks){
        x4_ui_perf_record(&g->verify,s.verification_last_worker_us);
        p->verification_status_checks=s.verification_checks;
    }
    if(s.verification_copy_completed>p->verification_status_copies){
        if(s.verification_last_copy_real_valid){x4_ui_perf_record(&g->verification_copy_real,s.verification_last_copy_real_us);++g->verification_copy_valid;}
        p->verification_status_copies=s.verification_copy_completed;
    }
    if(s.verification_captures>p->verification_status_captures){
        x4_ui_perf_record(&g->verification_enqueue,s.verification_last_enqueue_us);
        p->verification_status_captures=s.verification_captures;
    }

    g->quality_effective_word=s.quality_effective_word;g->last_quality_key=s.quality_key;
    g->quality_reason=s.quality_reason;g->pausa_repetidas=s.pausa_repetidas;
    g->quality_arena_bytes=s.quality_arena_bytes;g->quality_intermediate_bytes=s.quality_intermediate_bytes;
    g->quality_reserve_result=s.quality_reserve_result;g->quality_map_result=s.quality_map_result;
    g->quality_unmap_result=s.quality_unmap_result;g->quality_release_result=s.quality_release_result;
    g->intermediate_reserve_result=s.intermediate_reserve_result;g->intermediate_map_result=s.intermediate_map_result;
    g->intermediate_unmap_result=s.intermediate_unmap_result;g->intermediate_release_result=s.intermediate_release_result;
    for(unsigned i=0;i<X4_GPU_RESOLUTION_COUNT;++i){
        const X4GpuTimestampStatus *t=&s.timestamps[i];
        X4StreamGpuResolutionDiagnostics *r=&g->resolution[i];
        r->samples=t->samples;r->valid_samples=t->valid_samples;r->invalid=t->invalid;
        r->pending_samples=t->pending_samples;r->pending_overflow=t->pending_overflow;
        r->clock_hz=t->clock_hz;r->clock_calibrated=t->clock_calibrated;r->unsupported=t->unsupported;
        r->first_end_ticks=t->first_end_ticks;r->last_end_ticks=t->last_end_ticks;
        r->first_observed_us=t->first_observed_us;r->last_observed_us=t->last_observed_us;
        if(!s.pending&&s.last_resolution==i&&t->samples){r->latest_start_ticks=s.last_start_ticks;r->latest_end_ticks=s.last_end_ticks;}
        if(t->unsupported&&!p->gpu_timestamp_unsupported_logged[i]){
            printf("XCloud4 GPU marcas: resolucion=%s marcas=no_soportadas muestras=%llu invalidas=%llu\n",
                i==X4_GPU_RESOLUTION_720?"720p":"1080p",(unsigned long long)t->samples,(unsigned long long)t->invalid);
            p->gpu_timestamp_unsupported_logged[i]=true;
        }
    }
    if(s.pause_episodes>g->pause_episodes)p->gpu_pause_cpu_base=g->pause_cpu_frames;
    g->pause_episodes=s.pause_episodes;
    g->pause_cpu_selections=s.pause_cpu_frames;
    uint64_t observed=p->closed?p->ended:now_us();
    uint64_t active=s.pause_active&&observed>=s.pause_started_us?observed-s.pause_started_us:0;
    g->pause_active=s.pause_active&&!p->closed;g->pause_current_us=g->pause_active?active:0;
    uint64_t longest=s.pause_max_us>active?s.pause_max_us:active;
    if(longest>g->pause_max_us)g->pause_max_us=longest;
    uint64_t completed=s.pause_episodes-(s.pause_active&&!p->closed?1u:0u);
    if(completed>p->gpu_pause_log_seen){
        p->gpu_pause_log_seen=completed;p->gpu_pause_log_pending=true;
        p->gpu_pause_log_duration=s.pause_active?active:s.pause_last_us;
        p->gpu_pause_log_frames=g->pause_cpu_frames-p->gpu_pause_cpu_base;
    }
    if(s.disabled){g->reason=s.reason;g->effective=0;}
}
static void gpu_discard_failed_ready(X4StreamPipeline *p)
{
    if(!p->gpu)return;
    /* The worker owns copied WB pixels only. Collecting a result may disable
     * future GPU starts, but never retires a live native/scanout GPU lease. */
    (void)x4_gpu_convert_verification_collect(p->gpu,NULL);gpu_status(p);
    if(!p->gpu_metrics.verification_failed)return;
    for(unsigned i=0;i<p->display.scanout_count;++i){
        if(p->display.slots[i].state!=X4_SCANOUT_READY||!p->scanout_gpu[i])continue;
        uint64_t output=p->display.slots[i].output_id;
        /* Retire only unsubmitted GPU READY after EOP. Synced can retain two
         * READY, so its lease operation preserves a valid CPU picture. */
        unsigned dropped;
        if(p->display.synced){
            X4VideoPipelineLease lease={i,p->display.epoch,p->display.slots[i].lease_generation};
            dropped=x4_video_pipeline_cancel_ready_lease(&p->display,lease)==0?1u:0u;
        }else dropped=x4_video_pipeline_cancel_ready_scanout(&p->display);
        p->gpu_ready_discarded_verification+=dropped;p->scanout_gpu[i]=false;
        if(dropped)printf("XCloud4 GPU: READY no enviada descartada por verificacion output=%llu\n",
            (unsigned long long)output);
        if(!p->display.synced)return;
    }
}
static void gpu_log(X4StreamPipeline *p,uint64_t now)
{
    if(!p->gpu_metrics.requested)return;
    gpu_status(p);
    /* Coalesce completed episodes within a second; cumulative counters remain
     * exact. This logs the most recent completion without waiting or looping. */
    if(p->gpu_pause_log_pending&&(!p->gpu_pause_log_emitted||now-p->gpu_pause_log_at>=1000000)){
        printf("XCloud4 GPU pausa: fin duracion_us=%llu frames_cpu=%llu\n",
            (unsigned long long)p->gpu_pause_log_duration,(unsigned long long)p->gpu_pause_log_frames);
        p->gpu_pause_log_at=now;p->gpu_pause_log_pending=false;p->gpu_pause_log_emitted=true;
    }
    if(now-p->gpu_log_at<5000000)return;
    p->gpu_log_at=now;const X4StreamGpuDiagnostics *g=&p->gpu_metrics;
    printf("XCloud4 GPU conv: frames=%llu gpu_p50_us=%llu gpu_p95_us=%llu gpu_max_us=%llu esperas_timeout=%llu respaldo_cpu=%s verificacion_fuera=%llu pausas=%llu frames_pausa=%llu pausa_max_us=%llu prep_p50_us=%llu prep_p95_us=%llu prep_max_us=%llu eop_p50_us=%llu eop_p95_us=%llu eop_max_us=%llu verify_p50_us=%llu verify_max_us=%llu",
        (unsigned long long)g->gpu_frames,(unsigned long long)x4_ui_perf_percentile(&g->time,50),
        (unsigned long long)x4_ui_perf_percentile(&g->time,95),(unsigned long long)g->time.max,
        (unsigned long long)g->timeouts,g->reason?g->reason:"no",(unsigned long long)g->pixels_outside,
        (unsigned long long)g->pause_episodes,(unsigned long long)g->pause_cpu_frames,(unsigned long long)g->pause_max_us,
        (unsigned long long)x4_ui_perf_percentile(&g->prep,50),(unsigned long long)x4_ui_perf_percentile(&g->prep,95),(unsigned long long)g->prep.max,
        (unsigned long long)x4_ui_perf_percentile(&g->eop,50),(unsigned long long)x4_ui_perf_percentile(&g->eop,95),(unsigned long long)g->eop.max,
        (unsigned long long)x4_ui_perf_percentile(&g->verify,50),(unsigned long long)g->verify.max);
    printf(" sondeo_p50_us=%llu sondeo_p95_us=%llu sondeo_max_us=%llu sondeo_listo=%llu sondeo_agotado=%llu",
        (unsigned long long)x4_ui_perf_percentile(&g->spin,50),
        (unsigned long long)x4_ui_perf_percentile(&g->spin,95),(unsigned long long)g->spin.max,
        (unsigned long long)g->spin_ready,(unsigned long long)g->spin_timeout);
    for(unsigned i=0;i<X4_GPU_RESOLUTION_COUNT;++i){
        const X4StreamGpuResolutionDiagnostics *r=&g->resolution[i];
        printf(" %s_gpu_real_p50_us=%llu %s_gpu_real_p95_us=%llu %s_gpu_real_max_us=%llu %s_gpu_clock_hz=%llu %s_ts_invalidas=%llu",
            i?"1080p":"720p",(unsigned long long)x4_ui_perf_percentile(&r->real,50),
            i?"1080p":"720p",(unsigned long long)x4_ui_perf_percentile(&r->real,95),
            i?"1080p":"720p",(unsigned long long)r->real.max,
            i?"1080p":"720p",(unsigned long long)r->clock_hz,
            i?"1080p":"720p",(unsigned long long)r->invalid);
    }
#if !X4_PUBLIC
    printf(" pausa_repetidas=%llu dither_frames=%llu deband_suave_frames=%llu deband_fuerte_frames=%llu cas_baja_frames=%llu cas_media_frames=%llu",
        (unsigned long long)g->pausa_repetidas,(unsigned long long)g->dither_frames,
        (unsigned long long)g->deband_soft_frames,(unsigned long long)g->deband_strong_frames,
        (unsigned long long)g->cas_low_frames,(unsigned long long)g->cas_medium_frames);
    for(unsigned i=0;i<X4_STREAM_GPU_QUALITY_COMBINATIONS;++i){
        const X4StreamGpuQualityDiagnostics *q=&g->by_quality[i];if(!q->frames)continue;
        printf(" quality_%u_frames=%llu quality_%u_gpu_real_p50_us=%llu quality_%u_gpu_real_p95_us=%llu quality_%u_gpu_real_max_us=%llu",
            i,(unsigned long long)q->frames,i,(unsigned long long)x4_ui_perf_percentile(&q->real,50),
            i,(unsigned long long)x4_ui_perf_percentile(&q->real,95),i,(unsigned long long)q->real.max);
    }
#endif
    printf("\n");
}
static int gpu_release_image(X4StreamPipeline *p,X4VideoPipelineLease image)
{
    lock(&p->ring_gate);int rc=x4_video_pipeline_release_conversion(&p->ring,image);unlock(&p->ring_gate);
    if(rc){p->conversion_cleanup_failed=true;return rc;}
    atomic_fetch_sub_explicit(&p->conversion_readers,1,memory_order_release);
    return 0;
}
static int gpu_retire_quarantine(X4StreamPipeline *p)
{
    if(!p->gpu_quarantine)return 0;
    uint64_t elapsed=0;X4GpuConvertPoll rc=x4_gpu_convert_poll(p->gpu,now_us(),&elapsed);gpu_status(p);
    if(rc==X4_GPU_CONVERT_WAIT||rc==X4_GPU_CONVERT_TIMEOUT||rc==X4_GPU_CONVERT_VERIFY_QUARANTINE)return 0;
    if(rc!=X4_GPU_CONVERT_LATE_DONE&&rc!=X4_GPU_CONVERT_DONE)return X4_PIPELINE_STATE;
    int released=x4_video_pipeline_retire_quarantine(&p->display,p->gpu_held_scanout);
    if(released)return released;
    released=gpu_release_image(p,p->gpu_held_image);if(released)return released;
    p->gpu_quarantine=false;
    return x4_stream_wait_notify(&p->decode_wait);
}
static void *stream_pipeline_verification_worker(void *argument)
{
    X4StreamPipeline *p=argument;
    atomic_store_explicit(&p->verification_priority_result,scePthreadSetprio(scePthreadSelf(),767),memory_order_release);
    for(;;){
        X4GpuVerifySnapshot snapshot;
        uint64_t begin=now_us();
        if(x4_gpu_convert_verification_take(p->gpu,&snapshot)){
            X4GpuVerifyResult result={0};
            (void)x4_gpu_quality_verify_snapshot(&snapshot,&result,p->verification_scratch,
                x4_gpu_quality_verify_scratch_bytes());
            result.worker_us=now_us()-begin;
            (void)x4_gpu_convert_verification_complete(p->gpu,&result);
            (void)x4_stream_wait_notify(&p->present_wait);
        }else if(atomic_load_explicit(&p->verification_stop,memory_order_acquire))break;
        else (void)x4_stream_wait_block(&p->verification_wait,X4_STREAM_WAIT_MAX_US);
    }
    atomic_store_explicit(&p->verification_finished,true,memory_order_release);return NULL;
}
static void gpu_prepare(X4StreamPipeline *p)
{
    X4StreamGpuDiagnostics *g=&p->gpu_metrics;
    if(!g->requested||p->gpu_prepare_attempted)return;
    p->gpu_prepare_attempted=true;p->gpu=x4_gpu_convert_open();
    p->verification_scratch=malloc(x4_gpu_quality_verify_scratch_bytes());
    int rc=p->gpu?x4_gpu_convert_verification_prepare(p->gpu):X4_PIPELINE_EXHAUSTED;
    if(!rc&&p->verification_scratch){
        g->verification_bytes=x4_gpu_quality_verify_scratch_bytes();
        rc=x4_probe_thread_create(&p->verification_worker,&p->verification_thread,
            stream_pipeline_verification_worker,p,"x4-gpu-verify");
    }else if(!rc)rc=X4_PIPELINE_EXHAUSTED;
    if(rc){g->disabled=true;g->reason="verification_prepare_failed";}
    gpu_status(p);
}
static void gpu_quality_log(X4StreamPipeline *p,const X4VideoPipelineColor *color,
    unsigned effective,const char *reason)
{
    X4StreamGpuDiagnostics *g=&p->gpu_metrics;
#if !X4_PUBLIC
    unsigned requested=p->gpu_quality_requested_word;
#endif
    if(!p->gpu_quality_logged||p->gpu_quality_log_word!=effective||
        p->gpu_quality_log_width!=p->converting_image.layout.visible_width||p->gpu_quality_log_sharp!=color->sharpness||
        strcmp(p->gpu_quality_log_reason,reason)){
        g->quality_effective_word=effective;
        g->quality_reason=reason;
#if !X4_PUBLIC
        printf("XCloud4 GPU calidad: requested_word=%u effective_word=%u dither=%u deband=%u CAS=%u sharpness=%u pause_repeat=%u source_width=%u reason=%s\n",
            requested,effective,!!(effective&X4_GPU_QUALITY_DITHER),(effective&X4_GPU_QUALITY_DEBAND_MASK)>>X4_GPU_QUALITY_DEBAND_SHIFT,
            !!(effective&X4_GPU_QUALITY_CAS),color->sharpness,!!(effective&X4_GPU_QUALITY_REPEAT),
            p->converting_image.layout.visible_width,g->quality_reason);
#else
        printf("XCloud4 GPU: Dither=%u reason=%s\n",!!(effective&X4_GPU_QUALITY_DITHER),reason);
#endif
        p->gpu_quality_logged=true;p->gpu_quality_log_word=effective;
        p->gpu_quality_log_width=p->converting_image.layout.visible_width;p->gpu_quality_log_sharp=color->sharpness;
        p->gpu_quality_log_reason=reason;
    }
}
static int gpu_start(X4StreamPipeline *p,const X4VideoPipelineColor *color)
{
    X4StreamGpuDiagnostics *g=&p->gpu_metrics;
    p->gpu_pause_conversion=false;p->gpu_quality_accepted_word=0;
    unsigned requested=p->gpu_quality_requested_word;
    bool applicable=g->requested&&p->converting_image.layout.visible_width==1920&&color->convert_mode==X4_CONVERT_SMOOTH;
    unsigned effective=applicable&&x4_gpu_quality_active(requested,color->sharpness)?requested:0;
    const char *inactive=requested&&!applicable?"mejoras_solo_1080p_suave":"off";
    if(!g->requested){
        /* Capture already names the CPU-only applicability result; preserve
         * it in the later layout log instead of relabeling it as geometry. */
        gpu_quality_log(p,color,0,g->quality_reason);return 0;}
    if(g->disabled){g->effective=0;gpu_quality_log(p,color,0,"gpu_disabled");return 0;}
    const char *reason=NULL;
    /* layout_verified is established by the raw NV12 adapter. The SPS also
     * proves 4:2:0 8-bit; a guessed format is never admitted to the shader. */
    const X4StreamSps *s=&p->native_info.sps;
    X4VideoFormat format=p->converting_image.layout_verified&&s->chroma_format_idc==1&&
        s->bit_depth_luma==8&&s->bit_depth_chroma==8?X4_VIDEO_FORMAT_NV12:X4_VIDEO_FORMAT_UNKNOWN;
    if(!x4_gpu_convert_admit(&p->converting_image.layout,format,color,&reason)){
        gpu_quality_log(p,color,0,requested?"mejoras_solo_1080p_suave":"off");
        g->effective=0;g->reason=reason;return false;
    }
    if(!p->gpu||!p->verification_thread.created){g->disabled=true;g->reason="verification_not_ready";return 0;}
    if((g->gpu_frames%600u)==0u){
        unsigned first=(unsigned)(((g->gpu_frames/600u)*16u)%(1080u-16u+1u));
        (void)x4_gpu_convert_verification_request(p->gpu,first,16u);
    }
    X4GpuConvertFrame frame={.layout=p->converting_image.layout,.format=format,.color=*color,
        .source=p->converting_image.pixels,.source_bytes=p->converting_image.owned_bytes,
        .destination=p->display_pixels[p->scanout_lease.slot],.destination_bytes=STREAM_SCANOUT_BYTES,.destination_pitch=1920,
        .quality_word=requested};
    int rc=x4_gpu_convert_start(p->gpu,&frame,now_us());gpu_status(p);
    gpu_quality_log(p,color,g->quality_effective_word,
        g->quality_reason?g->quality_reason:inactive);
    if(rc==X4_GPU_CONVERT_REPEAT){
        rc=x4_video_pipeline_abort_scanout(&p->display,p->scanout_lease);
        if(rc){p->conversion_cleanup_failed=true;return rc;}
        rc=gpu_release_image(p,p->image_lease);if(rc)return rc;
        p->converting=false;p->gpu_active=false;p->gpu_quality_accepted_word=0;
        g->effective=2;g->reason="repeat_previous";
        rc=x4_stream_wait_notify(&p->decode_wait);
        return rc?rc:2;
    }
    if(rc!=X4_GPU_CONVERT_ACCEPTED){g->effective=0;g->reason=x4_gpu_convert_reason(p->gpu);
        p->gpu_pause_conversion=g->pause_active&&g->allowed_result==0;return false;}
    p->gpu_active=true;p->gpu_color=*color;p->gpu_quality_accepted_word=effective;
    g->admitted=true;g->effective=1;g->reason="no";return 1;
}
static int finish_conversion(X4StreamPipeline *p,X4StreamOverlay overlay,void *context,bool abandon)
{
    if(!p->converting)return 0;
    if(p->conversion_cleanup_failed)return X4_PIPELINE_STATE;
    int rc=0;
    bool gpu_finished=false;
    if(p->gpu_active){
        uint64_t elapsed=0;X4GpuConvertPoll state=x4_gpu_convert_poll(p->gpu,now_us(),&elapsed);gpu_status(p);
        if(state==X4_GPU_CONVERT_WAIT)return X4_PIPELINE_WAIT;
        if(state==X4_GPU_CONVERT_TIMEOUT||state==X4_GPU_CONVERT_VERIFY_QUARANTINE){
            rc=x4_video_pipeline_quarantine_scanout(&p->display,p->scanout_lease);if(rc)return rc;
            p->gpu_held_image=p->image_lease;p->gpu_held_scanout=p->scanout_lease;
            p->gpu_quarantine=true;p->gpu_active=false;p->converting=false;
            /* Both leases and the GPU reader remain live. CPU fallback can
             * use the other two scanouts; quarantine is never displayable. */
            return 0;
        }
        if(state==X4_GPU_CONVERT_LATE_DONE){
            rc=x4_video_pipeline_abort_scanout(&p->display,p->scanout_lease);if(rc)return rc;
            rc=gpu_release_image(p,p->image_lease);if(rc)return rc;
            p->gpu_active=false;p->converting=false;
            return x4_stream_wait_notify(&p->decode_wait);
        }
        if(state!=X4_GPU_CONVERT_DONE)return X4_PIPELINE_STATE;
        gpu_finished=true;p->gpu_active=false;++p->gpu_metrics.gpu_frames;
        x4_ui_perf_record(&p->gpu_metrics.time,elapsed);
        X4GpuConvertStatus timing={0};x4_gpu_convert_status(p->gpu,&timing);
        x4_ui_perf_record(&p->gpu_metrics.prep,timing.last_prep_us);
        x4_ui_perf_record(&p->gpu_metrics.eop,timing.last_eop_us);
        if(timing.last_real_valid&&timing.last_resolution<X4_GPU_RESOLUTION_COUNT)
            x4_ui_perf_record(&p->gpu_metrics.resolution[timing.last_resolution].real,timing.last_real_us);
        unsigned qword=p->gpu_quality_accepted_word;
        if(x4_gpu_quality_active(qword,p->gpu_color.sharpness)){
            unsigned key=(qword&15u)|(p->gpu_color.sharpness<<4);
            X4StreamGpuDiagnostics *q=&p->gpu_metrics;
            if(qword&X4_GPU_QUALITY_DITHER)++q->dither_frames;
            unsigned deband=(qword&X4_GPU_QUALITY_DEBAND_MASK)>>X4_GPU_QUALITY_DEBAND_SHIFT;
            if(deband==1)++q->deband_soft_frames;else if(deband==2)++q->deband_strong_frames;
            if(qword&X4_GPU_QUALITY_CAS){if(p->gpu_color.sharpness==1)++q->cas_low_frames;
                else if(p->gpu_color.sharpness==2)++q->cas_medium_frames;}
            if(key<X4_STREAM_GPU_QUALITY_COMBINATIONS){++q->by_quality[key].frames;
                if(timing.last_real_valid)x4_ui_perf_record(&q->by_quality[key].real,timing.last_real_us);}
        }
        (void)x4_stream_wait_notify(&p->verification_wait);
    }
    if(!gpu_finished){
    for(unsigned i=0;i<2;++i){StreamConverter *w=&p->converters[i];
        if(atomic_load_explicit(&w->completed,memory_order_acquire)==p->work)continue;
        /* An entry that failed CPU initialization/wait before dispatch may
         * already be finished. It cannot read this job. Do not wait for an
         * impossible completed tag, but still wait for every live stripe. */
        if(atomic_load_explicit(&w->finished,memory_order_acquire)){
            int failed=atomic_load_explicit(&p->error,memory_order_acquire);
            rc=failed?failed:X4_PIPELINE_STATE;
        }else return now_us()-p->conversion_begin>STREAM_TIMEOUT_US?X4_PROBE_STACK_TIMEOUT:X4_PIPELINE_WAIT;
    }
    }
    uint64_t end=now_us();duration(p,X4_STREAM_CONVERSION,end-p->conversion_begin);
    if(!abandon)x4_ui_perf_record(&p->conversion_menu[p->conversion_menu_open?1:0],end-p->conversion_begin);
    if(!gpu_finished)
    for(unsigned i=0;i<2;++i)if(p->converters[i].result)rc=p->converters[i].result;
    if(!rc&&!abandon&&!stopping(p)){
        p->scanout_menu[p->scanout_lease.slot]=atomic_load_explicit(&p->menu_open,memory_order_relaxed);
        /* The next real conversion supersedes a frozen UI snapshot. No
         * decoder, conversion or displayed-buffer ownership is changed. */
        free(p->notice_backing);p->notice_backing=NULL;
        p->notice_scanout[p->scanout_lease.slot]=false;
        if(overlay)overlay(p->display_pixels[p->scanout_lease.slot],context);
        rc=x4_video_pipeline_finish_scanout(&p->display,p->scanout_lease,p->converting_image.output_id,p->converting_image.decode_end_us,end);
        if(!rc){p->scanout_ready_us[p->scanout_lease.slot]=now_us();
            p->scanout_gpu[p->scanout_lease.slot]=gpu_finished&&
                p->display.slots[p->scanout_lease.slot].state==X4_SCANOUT_READY;
            lock(&p->status_gate);++p->view.conversions;p->view.convert_us+=end-p->conversion_begin;
            if(end-p->conversion_begin>p->view.convert_max_us)p->view.convert_max_us=end-p->conversion_begin;
            unlock(&p->status_gate);
            if(!gpu_finished){++p->gpu_metrics.cpu_frames;
                if(p->gpu_pause_conversion)++p->gpu_metrics.pause_cpu_frames;}
        }
        else {int aborted=x4_video_pipeline_abort_scanout(&p->display,p->scanout_lease);
            if(aborted){p->conversion_cleanup_failed=true;return aborted;}}
    }else {int aborted=x4_video_pipeline_abort_scanout(&p->display,p->scanout_lease);
        if(aborted){p->conversion_cleanup_failed=true;return aborted;}}
    lock(&p->ring_gate);int released=x4_video_pipeline_release_conversion(&p->ring,p->image_lease);unlock(&p->ring_gate);
    if(released){p->conversion_cleanup_failed=true;return released;}
    p->converting=false;
    /* Publication is last: a successful acquire-load of zero by the Decode
     * owner proves that neither stripe still reads the old native memory. */
    atomic_fetch_sub_explicit(&p->conversion_readers,1,memory_order_release);
    int notified=x4_stream_wait_notify(&p->decode_wait);if(!rc&&notified)rc=notified;
    return rc;
}
int x4_stream_pipeline_redraw_notice(X4StreamPipeline *p,X4StreamOverlay overlay,void *context)
{
    if(!p||p->display_handle<0||p->closed||p->video_halted)return X4_PIPELINE_STATE;
    if(p->converting)return X4_PIPELINE_WAIT;
    int rc=observe_display(p);if(rc)return rc;
    if(p->display.pending>=0||p->display.screen<0)return X4_PIPELINE_WAIT;
    for(unsigned i=0;i<p->scanout_count;++i)
        if(p->display.slots[i].state==X4_SCANOUT_READY||p->display.slots[i].state==X4_SCANOUT_WRITING)
            return X4_PIPELINE_WAIT;
    unsigned source=(unsigned)p->display.screen;
    if(p->display.slots[source].state!=X4_SCANOUT_SCREEN)return X4_PIPELINE_STATE;
    if(!p->notice_backing){
        p->notice_backing=malloc(STREAM_SCANOUT_BYTES);if(!p->notice_backing)return -ENOMEM;
        memcpy(p->notice_backing,p->display_pixels[source],STREAM_SCANOUT_BYTES);
    }
    /* Mailbox normally requires monotonically newer native output IDs. The
     * sole quiet UI repeat keeps the same ID and uses the existing single-
     * flight policy temporarily. Restore capability/admission at observation;
     * no real conversion, READY or pending frame can race this exception. */
    p->notice_mailbox_capable=p->display.mailbox_capable;
    p->notice_mailbox_admission=p->display.mailbox_admission;
    p->notice_synced=p->display.synced;p->display.synced=false;
    p->display.mailbox_capable=false;p->display.mailbox_admission=false;
    X4VideoPipelineLease target={0};
    rc=x4_video_pipeline_acquire_scanout(&p->display,&target);
    if(rc){p->display.mailbox_capable=p->notice_mailbox_capable;
        p->display.mailbox_admission=p->notice_mailbox_admission;
        p->display.synced=p->notice_synced;return rc;}
    memcpy(p->display_pixels[target.slot],p->notice_backing,STREAM_SCANOUT_BYTES);
    if(overlay)overlay(p->display_pixels[target.slot],context);
    const X4VideoPipelineScanout *previous=&p->display.slots[source];
    rc=x4_video_pipeline_finish_scanout(&p->display,target,previous->output_id,previous->decode_end_us,now_us());
    if(rc){(void)x4_video_pipeline_abort_scanout(&p->display,target);
        p->display.mailbox_capable=p->notice_mailbox_capable;p->display.mailbox_admission=p->notice_mailbox_admission;
        p->display.synced=p->notice_synced;return rc;}
    p->notice_scanout[target.slot]=true;p->scanout_gpu[target.slot]=false;
    p->notice_mode_active=true;
    return submit_display(p);
}

#if !X4_PUBLIC
static bool decoded_during_flip(const X4StreamPipeline *p,uint64_t decoded_us)
{
    return decoded_us>=p->last_flip_submitted_us&&decoded_us<=p->last_flip_observed_us;
}

/* Snapshot under the ring gate, immediately before the normal latest selection.
 * This is a conservative subset: only READY images decoded while the last
 * measured Cola VSYNC was pending, not ring-full or other presentation drops. */
static unsigned ready_during_cola_vsync(const X4StreamPipeline *p)
{
    if(p->ring.ready<2||!p->last_flip_measured||p->last_flip_mode!=X4_PRESENT_VSYNC_COLA)return 0;
    unsigned count=0;
    for(unsigned i=0;i<p->ring.output_count;++i){
        const X4VideoPipelineOutput *slot=&p->ring.slots[i];
        if(slot->state==X4_PIPELINE_READY&&decoded_during_flip(p,slot->decode_end_us))++count;
    }
    return count;
}
#endif

static void pacing_log(X4StreamPipeline *p,uint64_t now,bool final)
{
#if !X4_PUBLIC
    bool locked=!final&&!x4_stream_mutex_trylock(&p->ring_gate);
    if(final||locked){
        p->pacing_ring_full=p->ring.discarded_ready;
        p->pacing_ring_latest=p->ring.discarded_latency;
        if(locked)unlock(&p->ring_gate);
    }
    x4_stream_diagnostics_pacing_drops(p->diagnostics,p->synced_history.synced_recovery_drops,
        p->display.mailbox_replaced+p->display.mailbox_discarded_old,
        p->pacing_ring_full,p->pacing_ring_latest,p->pacing_cola_vsync_wait_drops);
    x4_stream_diagnostics_pacing_log(p->diagnostics,now,final);
#else
    (void)p;(void)now;(void)final;
#endif
}

int x4_stream_pipeline_present(X4StreamPipeline *p,X4StreamOverlay overlay,void *context)
{
    if(!p||p->display_handle<0||p->closed||p->video_halted)return X4_PIPELINE_STATE;
    int gpu_retired=gpu_retire_quarantine(p);if(gpu_retired)return fail_named(p,gpu_retired,"GPU quarantine ownership");
    gpu_log(p,now_us());
    X4StreamVsyncTransition transition;
    while(x4_stream_diagnostics_vsync_take_transition(p->diagnostics,&transition)){
#if !X4_PUBLIC
        printf("XCloud4 VSYNC Auto T27: segundo=%llu AU=%llu anterior=%u actual=%u at_us=%llu\n",
            (unsigned long long)transition.complete_second,(unsigned long long)transition.au_count,
            transition.previous_vsync,transition.vsync,(unsigned long long)transition.at_us);
#endif
    }
    (void)x4_video_pipeline_display_mailbox_admission(&p->display,
        p->display.mailbox_capable&&x4_stream_diagnostics_vsync_effective(p->diagnostics));
    int rc=finish_conversion(p,overlay,context,false);if(rc<0)return fail_named(p,rc,"conversion completion");
    /* A completed worker result can arrive with or without a current GPU
     * job. Remove suspect unsubmitted GPU READY before any new SubmitFlip;
     * CPU READY is intentionally preserved and pending flips are immutable. */
    gpu_discard_failed_ready(p);
    rc=observe_display(p);if(rc)return fail_named(p,rc,"VideoOut flip observation");
    pacing_log(p,now_us(),false);
    if(stopping(p))return atomic_load(&p->error);
    rc=submit_display(p);if(rc)return fail_named(p,rc,"VideoOut flip submission");
    if(!p->converting){
        rc=x4_video_pipeline_acquire_scanout(&p->display,&p->scanout_lease);
        if(!rc){
            X4VideoPipelineColor color;
            lock(&p->ring_gate);
            uint64_t before_latency=p->ring.discarded_latency,before_full=p->ring.discarded_ready;
#if !X4_PUBLIC
            unsigned waited_ready=ready_during_cola_vsync(p);
#endif
            rc=atomic_load_explicit(&p->reconfiguring,memory_order_acquire)?X4_PIPELINE_WAIT:
                x4_video_pipeline_take_ready(&p->ring,&p->image_lease,NULL);
            menu_ring_drops(p,before_latency,before_full);
#if !X4_PUBLIC
            if(!rc&&waited_ready){
                uint64_t kept=p->ring.slots[p->image_lease.slot].decode_end_us;
                p->pacing_cola_vsync_wait_drops+=waited_ready-decoded_during_flip(p,kept);
            }
#endif
            if(!rc){p->converting_image=p->images[p->image_lease.slot];color=p->color;
                p->converting=true;atomic_fetch_add_explicit(&p->conversion_readers,1,memory_order_release);}
            unlock(&p->ring_gate);
            if(!rc){p->conversion_menu_open=atomic_load_explicit(&p->menu_open,memory_order_relaxed);
                p->conversion_begin=now_us();++p->work;
                note_conversion_begin(p,p->conversion_begin);
#if X4_STREAM_PIPELINE_TEST_HOOKS
                extern void x4_stream_pipeline_test_before_dispatch(X4StreamPipeline *);
                x4_stream_pipeline_test_before_dispatch(p);
#endif
                int gpu_result=gpu_start(p,&color);
                if(gpu_result<0)return fail_named(p,gpu_result,"GPU repeat ownership");
                if(gpu_result)return atomic_load_explicit(&p->error,memory_order_acquire);
                /* Publish the complete job before any notification. Even a
                 * failed first Signal must not leave stripe1 unpublished. */
                for(unsigned i=0;i<2;++i){StreamConverter *w=&p->converters[i];w->image=p->converting_image;w->color=color;
                    w->destination=p->display_pixels[p->scanout_lease.slot];atomic_store_explicit(&w->submitted,p->work,memory_order_release);}
                int notify_error=0;
                for(unsigned i=0;i<2;++i){int notified=x4_stream_wait_notify(&p->converters[i].work_wait);
                    if(notified&&!notify_error)notify_error=notified;}
                if(notify_error)return fail_named(p,notify_error,"converter work notification");
            }else {int aborted=x4_video_pipeline_abort_scanout(&p->display,p->scanout_lease);if(aborted)return fail_named(p,aborted,"unused scanout release");if(rc!=X4_PIPELINE_WAIT)return fail_named(p,rc,"ready native image acquisition");}
        }else if(rc!=X4_PIPELINE_WAIT)return fail_named(p,rc,"scanout acquisition");
    }
    return atomic_load_explicit(&p->error,memory_order_acquire);
}
void x4_stream_pipeline_note_menu_open(X4StreamPipeline *p,bool open)
{
    if(!p||p->closed||p->video_halted)return;
    atomic_store_explicit(&p->menu_open,open,memory_order_relaxed);
#if !X4_PUBLIC
    uint64_t now=now_us();
    if(now-p->menu_log_at>=5000000u){
        p->menu_log_at=now;
        lock(&p->ring_gate);
        uint64_t discarded_latency=p->menu_discarded_latency,discarded_full=p->menu_discarded_full;
        unlock(&p->ring_gate);
        printf("XCloud4 UI menu pipeline: convert_closed_n=%llu p50_us=%llu p95_us=%llu max_us=%llu convert_open_n=%llu p50_us=%llu p95_us=%llu max_us=%llu menu_presented=%llu discarded_latency_while_open=%llu discarded_full_while_open=%llu\n",
            (unsigned long long)p->conversion_menu[0].n,(unsigned long long)x4_ui_perf_percentile(&p->conversion_menu[0],50),
            (unsigned long long)x4_ui_perf_percentile(&p->conversion_menu[0],95),(unsigned long long)p->conversion_menu[0].max,
            (unsigned long long)p->conversion_menu[1].n,(unsigned long long)x4_ui_perf_percentile(&p->conversion_menu[1],50),
            (unsigned long long)x4_ui_perf_percentile(&p->conversion_menu[1],95),(unsigned long long)p->conversion_menu[1].max,
            (unsigned long long)p->menu_presentations,(unsigned long long)discarded_latency,(unsigned long long)discarded_full);
    }
#endif
}
unsigned x4_stream_pipeline_main_state(const X4StreamPipeline *p)
{
    if(!p||p->closed||p->video_halted)return 0;
    return (p->converting?X4_STREAM_MAIN_CONVERTING:0u)|
        (p->display.pending>=0||((p->display.mailbox_admission||p->display.synced)&&
            p->display.ready_since_us)?X4_STREAM_MAIN_FLIP_PENDING:0u);
}
void x4_stream_pipeline_note_controller_read(X4StreamPipeline *p,uint64_t sampled_us)
{
    if(!p||p->closed||!sampled_us)return;
    if(p->controller_previous_us&&sampled_us>=p->controller_previous_us)
        duration(p,X4_STREAM_CONTROLLER_READ_INTERVAL,sampled_us-p->controller_previous_us);
    p->controller_previous_us=sampled_us;
}
void x4_stream_pipeline_record_audio(X4StreamPipeline *p,const X4StreamAudioObservation *observation)
{if(p&&observation){p->audio_observation=*observation;p->audio_observed=true;}}
int x4_stream_pipeline_wait_present(X4StreamPipeline *p,unsigned timeout_us)
{
    if(!p||p->closed)return X4_PIPELINE_STATE;
    if(p->gpu&&x4_gpu_convert_pending(p->gpu)&&!stopping(p)){
        uint64_t begin=now_us();bool ready=false;
        /* The submission owner polls only the flushed label. No semaphore
         * token is consumed, GPU state stays untouched, and the deadline is
         * exactly the caller's. A token remains for the next normal wait. */
        while(!stopping(p)&&now_us()-begin<timeout_us){
            if(x4_gpu_convert_label_ready(p->gpu)){ready=true;break;}
            _mm_pause();
        }
        uint64_t elapsed=now_us()-begin;
        x4_ui_perf_record(&p->gpu_metrics.spin,elapsed);
        if(ready)++p->gpu_metrics.spin_ready;
        else if(!stopping(p)&&elapsed>=timeout_us)++p->gpu_metrics.spin_timeout;
        return 0;
    }
    /* Cancellation wakes the current wait, but a still-open error screen
     * must keep blocking on later iterations instead of returning in a loop. */
    int rc=x4_stream_wait_block(&p->present_wait,timeout_us);
    return rc<0?fail_named(p,rc,"main presentation wait"):0;
}
int x4_stream_pipeline_wake(X4StreamPipeline *p)
{return p?x4_stream_wait_notify(&p->present_wait):X4_PIPELINE_INVALID;}
void x4_stream_pipeline_record_wait_stats(X4StreamPipeline *p,unsigned actor,
    const X4StreamWaitStats *stats,uint64_t elapsed_us)
{
    if(!p||!stats||actor>=X4_STREAM_WAIT_EXTERNAL_ACTORS)return;
    p->external_wait_stats[actor]=*stats;p->external_wait_elapsed[actor]=elapsed_us;
    p->external_wait_recorded[actor]=true;
}
void x4_stream_pipeline_record_ingress_cpu(X4StreamPipeline *p,const X4StreamCpuReport *report)
{if(p&&report){p->ingress_cpu_report=*report;p->ingress_cpu_recorded=true;}}
static int close_display(X4StreamPipeline *p)
{
    if(p->display_handle>=0){p->display_close=sceVideoOutClose(p->display_handle);if(p->display_close)return p->display_close;p->display_handle=-1;}
    if(p->gpu&&x4_gpu_convert_pending(p->gpu)){
        /* VideoOut can detach, but the GPU still owns part of this allocation.
         * Keep ALL scanouts and native input storage; a timeout is not EOP. */
        lock(&p->status_gate);p->view.display_open=false;p->view.display_detached=true;unlock(&p->status_gate);
        return X4_PROBE_STACK_RETAINED;
    }
    int rc=x4_video_pipeline_display_closed(&p->display);if(rc)return rc;
    if(p->display_memory){p->display_unmap=sceKernelMunmap(p->display_memory,(size_t)p->scanout_count*STREAM_SCANOUT_BYTES);if(p->display_unmap)return p->display_unmap;p->display_memory=NULL;}
    if(p->display_allocated){p->display_release=sceKernelReleaseDirectMemory(p->display_offset,(size_t)p->scanout_count*STREAM_SCANOUT_BYTES);if(p->display_release)return p->display_release;p->display_allocated=false;}
    lock(&p->status_gate);p->view.display_open=false;p->view.display_detached=true;unlock(&p->status_gate);return 0;
}
static int retain(X4StreamPipeline *p,int rc)
{
    int result=rc<0?rc:rc?-rc:X4_PROBE_STACK_RETAINED;
    p->retained=true;
    if(!p->closed)lock(&p->status_gate);
    p->view.retained=true;if(!p->closed)unlock(&p->status_gate);
    if(p->closed&&!p->halt_result)p->halt_result=result;
    return result;
}
static int halt_failed(X4StreamPipeline *p,int rc,const char *where)
{
    p->halt_result=retain(p,rc);(void)fail_named(p,p->halt_result,where);
    printf("XCloud4: stream halt stage=%s result=%d display_detached=%u retained=1\n",where,p->halt_result,p->view.display_detached);
    return p->halt_result;
}
int x4_stream_pipeline_halt_video(X4StreamPipeline *p)
{
    if(!p)return 0;
    if(p->video_halted)return p->halt_result;
    /* A failed native teardown is attempted once. Callbacks may still count
     * receipts; AU/diagnostic storage outlives the later full media close. */
    p->video_halted=true;p->halt_begin_us=now_us();
    x4_stream_pipeline_request_stop(p);uint64_t deadline=now_us()+STREAM_TIMEOUT_US;
    while(p->converting){int rc=finish_conversion(p,NULL,NULL,true);
        if(rc<0){
            if(p->converting||p->conversion_cleanup_failed)return halt_failed(p,rc,"halt conversion completion");
            /* Both stripes are completed/finished and both leases retired.
             * Record the video failure, then join the remaining live owners. */
            (void)fail_named(p,rc,"halt abandoned conversion");
        }
        if(p->converting){if(now_us()>deadline)return halt_failed(p,X4_PROBE_STACK_TIMEOUT,"halt conversion timeout");sceKernelUsleep(100);}}
    for(unsigned i=0;i<2;++i){atomic_store_explicit(&p->converters[i].stop,true,memory_order_release);
        (void)x4_stream_wait_notify(&p->converters[i].work_wait);}
    for(unsigned i=0;i<2;++i){StreamConverter *w=&p->converters[i];if(w->thread.created){
        while(!atomic_load_explicit(&w->finished,memory_order_acquire)&&!w->thread.report.wrapper_result){if(now_us()>deadline)return halt_failed(p,X4_PROBE_STACK_TIMEOUT,"halt converter timeout");sceKernelUsleep(100);}
        int rc=scePthreadJoin(w->id,NULL);x4_probe_thread_joined(&w->thread,rc);if(rc)return halt_failed(p,rc,"halt converter join");
        w->thread.created=false;}}
    /* A late EOP may have arrived while CPU converters joined. No additional
     * timeout is started, no GPU wait is unbounded, and no submit occurs. */
    int gpu_retired=gpu_retire_quarantine(p);
    if(gpu_retired)return halt_failed(p,gpu_retired,"halt GPU quarantine ownership");
    atomic_store_explicit(&p->verification_stop,true,memory_order_release);
    (void)x4_stream_wait_notify(&p->verification_wait);
    if(p->verification_thread.created){
        while(!atomic_load_explicit(&p->verification_finished,memory_order_acquire)&&!p->verification_thread.report.wrapper_result){
            if(now_us()>deadline)return halt_failed(p,X4_PROBE_STACK_TIMEOUT,"halt verification timeout");sceKernelUsleep(100);}
        int joined=scePthreadJoin(p->verification_worker,NULL);x4_probe_thread_joined(&p->verification_thread,joined);
        if(joined)return halt_failed(p,joined,"halt verification join");p->verification_thread.created=false;
    }
    if(p->gpu){X4GpuVerifyResult verified;(void)x4_gpu_convert_verification_collect(p->gpu,&verified);gpu_status(p);
        if(!x4_gpu_convert_verification_cancel(p->gpu)&&!x4_gpu_convert_pending(p->gpu))return halt_failed(p,X4_PROBE_STACK_RETAINED,"halt verification ownership");}
    atomic_store_explicit(&p->cleanup_allowed,true,memory_order_release);
    (void)x4_stream_wait_notify(&p->decode_wait);
    if(p->decoder_thread.created){
        uint64_t began=now_us();
        while(!atomic_load_explicit(&p->decoder_finished,memory_order_acquire)&&!p->decoder_thread.report.wrapper_result){if(now_us()>deadline)return halt_failed(p,X4_PROBE_STACK_TIMEOUT,"halt decoder timeout");sceKernelUsleep(100);}
        int rc=scePthreadJoin(p->decoder,NULL);x4_probe_thread_joined(&p->decoder_thread,rc);if(rc)return halt_failed(p,rc,"halt decoder join");p->decoder_thread.created=false;
        p->decoder_join_wait_us=now_us()-began;
    }
    /* CPU readers are joined. VideoOut owns separate allocations and may be
     * detached even if Delete retained decoder memory. The retained decoder
     * context itself is never freed, reused or re-created. */
    int display_rc=close_display(p);
    lock(&p->status_gate);p->view.native_ready=false;unlock(&p->status_gate);
    if(display_rc)return halt_failed(p,display_rc,"halt VideoOut close/unmap/release");
    if(p->decoder_close_result)return halt_failed(p,p->decoder_close_result,"halt decoder native teardown");
    p->halt_end_us=now_us();
    return 0;
}
int x4_stream_pipeline_close(X4StreamPipeline *p)
{
    if(!p)return 0;
    if(p->closed)return p->halt_result;
    int rc=x4_stream_pipeline_halt_video(p);
    /* Full close is called after the ingress owner/callbacks have stopped.
     * Export remains possible on retained native state only after ALL CPU
     * producers joined; otherwise it must not race a live report writer. */
    if(!p->decoder_thread.created&&!p->converters[0].thread.created&&!p->converters[1].thread.created&&!p->verification_thread.created){
        p->ended=now_us();p->closed=true;
        gpu_log(p,p->ended);
        pacing_log(p,p->ended,true);
        publish_admission_failure(p);
        if(!x4_stream_au_queue_quiescent(p->au_queue)){
            int ownership=retain(p,X4_STREAM_AU_FATAL);p->halt_result=rc?rc:ownership;
            (void)fail_named(p,ownership,"AU queue not quiescent after joined close");
            return rc?rc:ownership;
        }
    }
#if X4_PUBLIC && !X4_VIDEO_PROBE
    /* Full close is the existing ingress/callback quiescence contract, unlike
     * halt_video. Main unregisters/join-proves RTC before live_media joins the
     * AU producer, then calls us with wait/audio snapshots already captured.
     * Trace remains alive here; free happens after live_media releases it.
     * Diagnostics never change a successful playback-close return code. */
    if(!rc&&p->closed&&!p->retained&&p->view.display_detached&&!p->exported){
        int exported=x4_stream_pipeline_export(p,NULL); /* ingress stack unavailable here */
        printf("XCloud4: public stream diagnostic export rc=%d\n",exported);
    }
#endif
    return rc;
}
static void write_stack(FILE *f,const X4ProbeStackReport *s)
{
    if(!s){fputs("null",f);return;}
    fprintf(f,"{\"name\":\"%s\",\"configured_bytes\":%llu,\"reported_bytes\":%llu,\"guard_bytes\":%llu,\"high_water_bytes\":%llu,\"low_excluded_bytes\":%llu,\"top_excluded_bytes\":%llu,\"high_water_is_lower_bound\":true,\"create_result\":%d,\"measure_result\":%d,\"join_result\":%d}",
        s->name,(unsigned long long)s->configured_bytes,(unsigned long long)s->reported_bytes,(unsigned long long)s->guard_bytes,(unsigned long long)s->high_water_bytes,(unsigned long long)s->low_excluded_bytes,(unsigned long long)s->top_excluded_bytes,s->create_rc,s->measure_rc,s->join_rc);
}
static void write_parameter(FILE *f,const uint8_t *prefix,size_t bytes)
{
    size_t captured=bytes<64?bytes:64;
    fprintf(f,"{\"bytes\":%zu,\"captured_bytes\":%zu,\"truncated\":%s,\"hex\":\"",bytes,captured,bytes>captured?"true":"false");
    for(size_t i=0;i<captured;++i)fprintf(f,"%02x",(unsigned)prefix[i]);
    fputs("\"}",f);
}
static void write_sps(FILE *f,const X4StreamSps *s,const X4StreamSpsDiagnostics *d)
{
    fprintf(f,"{\"parsed_during_native_start\":%s",d->syntax_complete?"true":"false");
    /* Partial values are observations only where diagnostics.valid_mask says
     * the field was read. Zero is otherwise a default, never evidence. */
#define SPS_FIELD(name) fprintf(f,",\"" #name "\":%u",s->name)
    SPS_FIELD(profile_idc);SPS_FIELD(constraints);SPS_FIELD(level_idc);
    SPS_FIELD(coded_width);SPS_FIELD(coded_height);SPS_FIELD(visible_width);SPS_FIELD(visible_height);
    SPS_FIELD(crop_left);SPS_FIELD(crop_top);SPS_FIELD(crop_right);SPS_FIELD(crop_bottom);
    SPS_FIELD(chroma_format_idc);SPS_FIELD(bit_depth_luma);SPS_FIELD(bit_depth_chroma);
    SPS_FIELD(max_num_ref_frames);SPS_FIELD(max_dec_frame_buffering);SPS_FIELD(num_reorder_frames);
    SPS_FIELD(matrix_coefficients);SPS_FIELD(full_range);SPS_FIELD(seq_parameter_set_id);
    SPS_FIELD(log2_max_frame_num_minus4);SPS_FIELD(pic_order_cnt_type);SPS_FIELD(log2_max_pic_order_cnt_lsb_minus4);
    SPS_FIELD(pic_width_in_mbs_minus1);SPS_FIELD(pic_height_in_map_units_minus1);
    SPS_FIELD(separate_colour_plane_flag);SPS_FIELD(frame_mbs_only_flag);SPS_FIELD(mb_adaptive_frame_field_flag);
    SPS_FIELD(direct_8x8_inference_flag);SPS_FIELD(vui_present);SPS_FIELD(video_signal_type_present);
    SPS_FIELD(colour_description_present);SPS_FIELD(colour_primaries);SPS_FIELD(transfer_characteristics);
    SPS_FIELD(bitstream_restriction_present);SPS_FIELD(dpb_source);
#undef SPS_FIELD
    fputc('}',f);
}
static void write_sdp_profiles(FILE *f,const X4Trace *trace,unsigned scope)
{
    bool comma=false;fputc('[',f);
    for(unsigned pt=0;pt<128;++pt){uint32_t profile=x4_trace_sdp_profile_get(trace,scope,pt);
        if(profile){fprintf(f,"%s{\"payload_type\":%u,\"profile_level_id\":\"%06x\",\"profile_idc\":%u,\"constraints\":%u,\"level_idc\":%u}",comma?",":"",pt,profile,profile>>16,(profile>>8)&255,profile&255);comma=true;}}
    fputc(']',f);
}
static void write_attempts(FILE *f,const X4StreamVideoInfo *v)
{
    fputc('[',f);
    for(unsigned i=0;i<v->attempt_count&&i<sizeof(v->attempts)/sizeof(v->attempts[0]);++i){const X4StreamDecoderAttempt *a=&v->attempts[i];
        fprintf(f,"%s{\"depth\":%u,\"fallback\":%s,\"kind\":%u,\"profile\":%u,\"level\":%u,\"coded_width\":%u,\"coded_height\":%u,\"dpb\":%u,\"dpb_requested\":%u,\"dpb_minimum\":%u,\"dpb_level_cap\":%u,\"level_known\":%s,\"skipped\":%s,\"reason\":%u,\"failure_reason\":%u,\"failure_stage\":\"%s\",\"compute_query\":%d,\"allocate_queue\":%d,\"query\":%d,\"create\":%d,\"cleanup\":%d,\"compute_bytes\":%llu,\"cpu_bytes\":%llu,\"gpu_bytes\":%llu,\"shared_bytes\":%llu,\"frame_bytes\":%llu,\"planned_bytes\":%llu}",
            i?",":"",a->depth,a->fallback?"true":"false",a->kind,a->profile,a->level,a->width,a->height,a->dpb,a->dpb_requested,a->dpb_minimum,a->dpb_level_cap,a->level_known?"true":"false",a->skipped?"true":"false",a->reason,a->failure_reason,a->failure_stage,a->compute_query,a->allocate_queue,a->query,a->create,a->cleanup,
            (unsigned long long)a->compute_bytes,(unsigned long long)a->cpu_bytes,(unsigned long long)a->gpu_bytes,(unsigned long long)a->shared_bytes,(unsigned long long)a->frame_bytes,(unsigned long long)a->planned_bytes);}
    fputc(']',f);
}
static void write_sps_diagnostics(FILE *f,const X4StreamSpsDiagnostics *d,bool observed)
{
    fprintf(f,"{\"observed\":%s,\"code\":%d,\"reason\":%u,\"name\":\"%s\",\"stage\":%u,\"stage_name\":\"%s\",\"reader_error\":%u,\"valid_mask\":%llu,\"warning_mask\":%u,\"rbsp_bit_offset\":%zu,\"ebsp_byte_offset\":%zu,\"syntax_complete\":%s}",
        observed?"true":"false",d->code,d->reason,x4_stream_sps_error_name(d->code),d->stage,x4_stream_sps_stage_name(d->stage),d->reader_error,(unsigned long long)d->valid_mask,d->warning_mask,d->rbsp_bit_offset,d->ebsp_byte_offset,d->syntax_complete?"true":"false");
}
static void write_decoder_start(FILE *f,const X4StreamVideoInfo *v)
{
    fprintf(f,"{\"attempt_count\":%u,\"active_attempt\":%u,\"fallback_used\":%s,\"fallback_policy\":\"SPS_then_geometry_level_then_720_only_legacy_66_31_4\",\"attempts\":",v->attempt_count,v->active_attempt,v->fallback_used?"true":"false");
    write_attempts(f,v);
    fprintf(f,",\"requested_depth\":%u,\"active_depth\":%u,\"depth_fallback_used\":%s}",
        v->requested_depth,v->active_depth,v->depth_fallback_used?"true":"false");
}
static void write_recovery(FILE *f,const X4StreamPipeline *p)
{
    fprintf(f,"{\"bad_frame_error_limit\":null,\"other_native_error_limit\":%u,\"frame_errors\":%llu,\"consecutive_errors\":%u,\"maximum_consecutive_errors\":%u,\"keyframe_created\":%llu,\"keyframe_coalesced\":%llu,\"keyframe_consumed\":%llu,\"keyframe_pending\":%s,\"keyframe_scope\":\"local creation and media/auth bridge consumption; never network delivery\",\"restarts\":%llu,\"reconfigurations\":%llu,\"waiting_idr_drops\":%llu,\"discarded_ready_for_reconfiguration\":%llu,\"quiesce_calls\":%llu,\"quiesce_wait_us\":%llu,\"quiesce_wait_max_us\":%llu,\"parameter_logs_omitted\":%llu,\"failure_stage\":\"%s\",\"error_capacity\":%u,\"error_records_omitted\":%llu,\"errors\":[",
        X4_STREAM_FRAME_ERROR_LIMIT,(unsigned long long)p->frame_error_count,p->consecutive_errors,p->maximum_consecutive_errors,
        (unsigned long long)atomic_load(&p->keyframe_created),(unsigned long long)atomic_load(&p->keyframe_coalesced),(unsigned long long)atomic_load(&p->keyframe_taken),atomic_load(&p->keyframe_pending)?"true":"false",
        (unsigned long long)p->restart_count,(unsigned long long)p->reconfiguration_count,(unsigned long long)p->waiting_idr_drops,(unsigned long long)p->reconfiguration_discarded_ready,
        (unsigned long long)p->quiesce_calls,(unsigned long long)p->quiesce_us,(unsigned long long)p->quiesce_max_us,(unsigned long long)p->parameter_logs_omitted,p->failure_where,STREAM_ERROR_RECORDS,(unsigned long long)p->frame_records_omitted);
    uint64_t first=p->frame_error_count>STREAM_ERROR_RECORDS?p->frame_error_count-STREAM_ERROR_RECORDS:0;
    for(uint64_t i=first;i<p->frame_error_count;++i){const StreamFrameError *e=&p->frame_error_records[i%STREAM_ERROR_RECORDS];
        fprintf(f,"%s{\"ordinal\":%llu,\"input_AU\":%llu,\"input_bytes\":%zu,\"observed_us\":%llu,\"frame_class\":%u,\"slot\":%u,\"consecutive\":%u,\"native_width\":%u,\"native_height\":%u,\"pitch\":%u,\"stage\":\"%s\",\"native_result\":%d,\"control_result\":%d,\"accepted\":%s,\"completed_output\":%s,\"ownership_uncertain\":%s}",i==first?"":",",(unsigned long long)e->ordinal,(unsigned long long)e->au_identity,e->input_bytes,(unsigned long long)e->observed_us,e->frame_class,e->slot,e->consecutive,e->native_width,e->native_height,e->pitch,e->stage,e->native_result,e->control_result,e->accepted?"true":"false",e->completed_output?"true":"false",e->ownership_uncertain?"true":"false");}
    fprintf(f,"],\"epoch_capacity\":%u,\"epoch_count\":%llu,\"epoch_records_omitted\":%llu,\"epochs\":[",STREAM_EPOCH_RECORDS,(unsigned long long)p->epoch_count,(unsigned long long)p->epoch_records_omitted);
    first=p->epoch_count>STREAM_EPOCH_RECORDS?p->epoch_count-STREAM_EPOCH_RECORDS:0;
    for(uint64_t i=first;i<p->epoch_count;++i){const StreamEpoch *e=&p->epochs[i%STREAM_EPOCH_RECORDS];
        fprintf(f,"%s{\"ordinal\":%llu,\"input_AU\":%llu,\"begin_us\":%llu,\"close_us\":%llu,\"reason\":%u,\"start_result\":%d,\"close_result\":%d,\"retained\":%s,\"creation_uncertain\":%s,\"sps\":",i==first?"":",",(unsigned long long)e->ordinal,(unsigned long long)e->au_identity,(unsigned long long)e->begin_us,(unsigned long long)e->close_us,e->reason,e->start_result,e->close_result,e->native.retained?"true":"false",e->native.creation_uncertain?"true":"false");
        write_sps(f,&e->native.sps,&e->native.sps_diagnostics);fputs(",\"sps_diagnostics\":",f);write_sps_diagnostics(f,&e->native.sps_diagnostics,true);
        fputs(",\"parameter_sets\":{\"sps\":",f);write_parameter(f,e->sps,e->sps_bytes);fputs(",\"pps\":",f);write_parameter(f,e->pps,e->pps_bytes);fputs("},\"decoder_start\":",f);write_decoder_start(f,&e->native);fputc('}',f);}
    fprintf(f,"],\"consecutive_other_native_errors\":%u,\"idr_wait\":{\"resume_scope\":\"absolute recovery deadline is retired only after a validated output; bad IDRs never renew it; presentation remains separate\",\"retry_us\":%u,\"timeout_us\":%u,\"waiting\":%s,\"begin_us\":%llu,\"absolute_deadline_us\":%llu,\"waits\":%llu,\"recoveries\":%llu,\"timeouts\":%llu}}",
        p->consecutive_native_errors,
        X4_STREAM_IDR_RETRY_US,X4_STREAM_IDR_TIMEOUT_US,p->idr_recovery.waiting?"true":"false",
        (unsigned long long)p->idr_recovery.begin_us,(unsigned long long)p->idr_recovery.deadline_us,
        (unsigned long long)p->idr_recovery.waits,(unsigned long long)p->idr_recovery.recoveries,(unsigned long long)p->idr_recovery.timeouts);
}
static void write_au_queue(FILE *f,const X4StreamAuStats *s)
{
    fprintf(f,"{\"capacity_entries\":%u,\"maximum_AU_bytes\":%u,\"late_limit_us\":%u,\"arena_bytes\":%zu,\"metadata_bytes_including_parameters\":%zu,\"scope\":\"single producer/consumer variable byte ring; admission never waits; lag observations at push and nonempty take use max of monotonic residence and forward modular RTP90k content distance; EMPTY polls excluded; metadata includes descriptor SPS/PPS storage\"",
        X4_STREAM_AU_ENTRIES,X4_STREAM_AU_MAX_BYTES,X4_STREAM_AU_LAG_US,s->arena_bytes,s->metadata_bytes);
#define QUEUE_COUNT(name) fprintf(f,",\"" #name "\":%llu",(unsigned long long)s->name)
    QUEUE_COUNT(admitted);QUEUE_COUNT(taken);QUEUE_COUNT(released);
    QUEUE_COUNT(dropped_full);QUEUE_COUNT(dropped_late);QUEUE_COUNT(dropped_wait_idr);QUEUE_COUNT(dropped_busy);
    QUEUE_COUNT(rejected_incoming_full);QUEUE_COUNT(rejected_incoming_late);QUEUE_COUNT(rejected_incoming_wait_idr);QUEUE_COUNT(rejected_incoming_busy);
    QUEUE_COUNT(retired_ready_full);QUEUE_COUNT(retired_ready_late);QUEUE_COUNT(retired_ready_busy);
    QUEUE_COUNT(evicted_non_idr);QUEUE_COUNT(old_idr_retired);QUEUE_COUNT(recovery_transitions);
    QUEUE_COUNT(request_intentions);QUEUE_COUNT(request_coalesced);QUEUE_COUNT(padding_bytes_total);
    QUEUE_COUNT(samples);QUEUE_COUNT(lag_max_us);QUEUE_COUNT(lag_histogram_overflow);
    QUEUE_COUNT(depth_max);
    QUEUE_COUNT(resident_entries);QUEUE_COUNT(ready_entries);QUEUE_COUNT(inflight_entries);
    QUEUE_COUNT(resident_bytes);QUEUE_COUNT(payload_bytes);
#undef QUEUE_COUNT
    fputs(",\"depth_p50\":",f);if(!s->samples)fputs("null",f);else fprintf(f,"%u",s->depth_p50);
    fputs(",\"depth_p95\":",f);if(!s->samples)fputs("null",f);else fprintf(f,"%u",s->depth_p95);
    fputs(",\"lag_p50_upper_us\":",f);if(!s->samples||s->lag_p50_upper_us==UINT64_MAX)fputs("null",f);else fprintf(f,"%llu",(unsigned long long)s->lag_p50_upper_us);
    fputs(",\"lag_p95_upper_us\":",f);if(!s->samples||s->lag_p95_upper_us==UINT64_MAX)fputs("null",f);else fprintf(f,"%llu",(unsigned long long)s->lag_p95_upper_us);
    fprintf(f,",\"lag_histogram_quantum_us\":1000,\"waiting_idr\":%s,\"stopped\":%s,\"fatal\":%s}",s->waiting_idr?"true":"false",s->stopped?"true":"false",s->fatal?"true":"false");
}
static void write_keyframe_limiter(FILE *f,const X4KeyframeLimiter *limiter)
{
    X4KeyframeLimiterSnapshot s={0};x4_keyframe_limiter_snapshot(limiter,&s);
    fprintf(f,"{\"attached\":%s,\"minimum_interval_us\":%u,\"window_us\":%u,\"window_limit\":%u,\"storage_bytes\":%zu,\"scope\":\"logical reservations before local control/RTCP attempts; both mechanisms may be attempted once per grant; success is local, never a remote ACK; counters exact only after all producers and transport owner stop\"",
        limiter?"true":"false",X4_KEYFRAME_INTERVAL_US,X4_KEYFRAME_WINDOW_US,X4_KEYFRAME_WINDOW_LIMIT,sizeof(X4KeyframeLimiter));
#define LIMITER_COUNT(name) fprintf(f,",\"" #name "\":%llu",(unsigned long long)s.name)
    LIMITER_COUNT(requested);LIMITER_COUNT(coalesced);LIMITER_COUNT(deferred);LIMITER_COUNT(granted);LIMITER_COUNT(retired_on_idr);
    LIMITER_COUNT(interval_deferred);LIMITER_COUNT(cap_deferred);LIMITER_COUNT(clock_deferred);
    LIMITER_COUNT(gap_generation);LIMITER_COUNT(idr_generation);LIMITER_COUNT(granted_gap_generation);LIMITER_COUNT(last_granted_us);
    LIMITER_COUNT(transport_decisions);LIMITER_COUNT(control_attempts);LIMITER_COUNT(control_local_successes);LIMITER_COUNT(control_errors);
    LIMITER_COUNT(pli_attempts);LIMITER_COUNT(pli_local_successes);LIMITER_COUNT(pli_errors);
    LIMITER_COUNT(pending_reasons);
#undef LIMITER_COUNT
    fprintf(f,",\"counter_overflow\":%s}",s.counter_overflow?"true":"false");
}
static void write_wait_stats(FILE *f,const char *name,const X4StreamWaitStats *s,uint64_t elapsed_us)
{
    fprintf(f,"{\"name\":\"%s\",\"elapsed_us\":%llu,\"wait_calls\":%llu,\"wakeups\":%llu,\"timeouts\":%llu,\"notifications\":%llu,\"coalesced_notifications\":%llu,\"errors\":%llu,\"first_error\":%d,\"wakeups_per_second\":",
        name,(unsigned long long)elapsed_us,(unsigned long long)s->wait_calls,
        (unsigned long long)s->wakeups,(unsigned long long)s->timeouts,
        (unsigned long long)s->notifications,(unsigned long long)s->coalesced,
        (unsigned long long)s->errors,s->first_error);
    if(elapsed_us)fprintf(f,"%.6f",(double)s->wakeups*1000000.0/(double)elapsed_us);
    else fputs("null",f);
    fputc('}',f);
}
static void write_waits(FILE *f,const X4StreamPipeline *p)
{
    uint64_t elapsed=p->ended>=p->started?p->ended-p->started:0;
    X4StreamWaitStats s={0};
    fputs("{\"scope\":\"returns from blocking wait including timeout and retained tokens; not scheduler context switches; native Decode and conversion cannot be interrupted by this mechanism\",\"worker_timeout_us\":20000,\"threads\":[",f);
    x4_stream_wait_snapshot(&p->decode_wait,&s);write_wait_stats(f,"decode",&s,elapsed);
    for(unsigned i=0;i<2;++i){fputc(',',f);x4_stream_wait_snapshot(&p->converters[i].work_wait,&s);
        write_wait_stats(f,i?"converter1":"converter0",&s,elapsed);}
    for(unsigned i=0;i<X4_STREAM_WAIT_EXTERNAL_ACTORS;++i)if(p->external_wait_recorded[i]){
        fputc(',',f);write_wait_stats(f,i==X4_STREAM_WAIT_MAIN?"main":"x4-video",
            &p->external_wait_stats[i],p->external_wait_elapsed[i]);}
    if(!p->external_wait_recorded[X4_STREAM_WAIT_MAIN]){fputc(',',f);
        x4_stream_wait_snapshot(&p->present_wait,&s);write_wait_stats(f,"main",&s,elapsed);}
    fputs("]}",f);
}
static bool cpu_partial(const X4StreamPipeline *p)
{
    if(p->decode_cpu_report.partial_applied||p->decode_cpu_report.result)return true;
    if(p->ingress_cpu_recorded&&(p->ingress_cpu_report.partial_applied||p->ingress_cpu_report.result))return true;
    for(unsigned i=0;i<2;++i)if(p->converters[i].cpu_report.partial_applied||p->converters[i].cpu_report.result)return true;
    return false;
}
static void write_cpu_starvation(FILE *f,const X4StreamPipeline *p)
{
    const X4StreamAudioObservation *a=&p->audio_observation;
    bool available=p->audio_observed&&a->available;
    fprintf(f,"{\"variant\":%u,\"rtp_callbacks_rejected\":%llu,\"rtp_callbacks_full_design_failures\":%llu,\"audio\":{\"available\":%s,\"scope\":\"existing audio counters sampled immediately before audio stop; stop_result separately records the joined close, not an attribution of starvation\",\"observed_us\":%llu,\"startup_error\":%d,\"stop_result\":%d",
        (unsigned)p->request.cpu_variant,(unsigned long long)atomic_load(&p->callback_rejected),
        (unsigned long long)atomic_load(&p->callback_full_design_failures),available?"true":"false",
        (unsigned long long)a->observed_us,a->startup_error,a->stop_result);
#define AUDIO_COUNTER(field) fputs(",\"" #field "\":",f);if(available)fprintf(f,"%llu",(unsigned long long)a->field);else fputs("null",f)
    AUDIO_COUNTER(packets);AUDIO_COUNTER(frames);AUDIO_COUNTER(dropped);AUDIO_COUNTER(lost);
    AUDIO_COUNTER(bad_frames);AUDIO_COUNTER(underflows);
#undef AUDIO_COUNTER
    fputs(",\"error\":",f);if(available)fprintf(f,"%d",a->error);else fputs("null",f);
    fputs("}}",f);
}
#if X4_PUBLIC && !X4_VIDEO_PROBE && (defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST))
static void write_remb(FILE *f,const X4RembSummary *summary)
{
    const int64_t delay=summary->max_delay_us;
    const uint32_t delay_us=(uint32_t)(delay<0?-delay:delay);
    /* Integer formatting keeps the decimal point independent of locale. */
    fprintf(f,"\"remb\":{\"modo\":\"adaptable\",\"enviados\":%llu,"
        "\"actual_mbps\":%u.%06u,\"min_mbps\":%u.%06u,\"max_mbps\":%u.%06u,"
        "\"bajadas\":%llu,\"subidas\":%llu,\"R_mbps\":%u.%06u,"
        "\"L_max_ppm\":%u,\"D_max_ms\":%s%u.%03u},",
        (unsigned long long)summary->sent,
        summary->sent_bps/1000000u,summary->sent_bps%1000000u,
        summary->min_sent_bps/1000000u,summary->min_sent_bps%1000000u,
        summary->max_sent_bps/1000000u,summary->max_sent_bps%1000000u,
        (unsigned long long)summary->decreases,(unsigned long long)summary->increases,
        summary->received_bps/1000000u,summary->received_bps%1000000u,
        summary->max_loss_ppm,delay<0?"-":"",delay_us/1000u,delay_us%1000u);
}
#endif
int x4_stream_pipeline_export(X4StreamPipeline *p,const X4ProbeStackReport *ingress_thread)
{
    if(!p||!p->closed||p->exported)return X4_PIPELINE_STATE;
#if X4_PUBLIC && !X4_VIDEO_PROBE && (defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST))
    /* RTC and the receive sampler have stopped before the export. Emit the
     * closing summary even if the report cannot be opened or written. */
    X4RembSummary remb;
    x4_rtc_remb_finish_summary(p->trace,p->started,&remb);
#endif
    X4StreamAuStats queue={0};int queue_rc=x4_stream_au_queue_try_snapshot(p->au_queue,&queue);
    if(queue_rc){printf("XCloud4 stream: export AU snapshot result=%d (owners must be stopped)\n",queue_rc);return X4_PIPELINE_STATE;}
#if X4_PUBLIC
    const char *path="/data/xcloud4/last-stream.json";
    const char *temporary="/data/xcloud4/last-stream.json.tmp";
    errno=0;
    if(mkdir("/data/xcloud4",0777)&&errno!=EEXIST)return errno?-errno:-1;
    errno=0;FILE *f=fopen(temporary,"wb");
#else
    char path[160];int n=snprintf(path,sizeof(path),"/data/xcloud4-stream-%.12s-p%u-%llu.json",X4_SOURCE_ID,(unsigned)p->request.profile,(unsigned long long)p->session);
    if(n<0||(size_t)n>=sizeof(path))return X4_PIPELINE_INVALID;
    errno=0;FILE *f=fopen(path,"wbx");
#endif
    if(!f){int e=errno;printf("XCloud4 stream: export path=%s result=-1 errno=%d\n",path,e);return e?-e:-1;}
    const X4StreamVideoInfo *v=&p->native_info;
    unsigned report_depth=v->active_depth?v->active_depth:1;
    uint64_t report_budget=v->native_budget_bytes?v->native_budget_bytes:UINT64_C(134217728);
    fprintf(f,"{\"schema\":\"X4STREAM_PIPELINE5\",\"source_id\":\"%s\",\"profile\":%u,\"ready_policy\":\"latest\",\"display_policy\":\"%s\",\"converters\":2,\"native_pipeline_depth\":%u,\"output_memory_type\":0,\"decoder_budget\":%llu,\"scanout_bytes\":%llu,\"AU_queue_bytes\":%zu,\"parameter_queue_bytes\":%u,\"AU_queue_metadata_bytes_including_parameters\":%zu,\"control_context_bytes\":%zu,",
        X4_SOURCE_ID,(unsigned)p->request.profile,p->display.synced?"synced":p->display.mailbox_capable?"mailbox":"one_in_flight",report_depth,(unsigned long long)report_budget,(unsigned long long)p->scanout_count*STREAM_SCANOUT_BYTES,queue.arena_bytes,2u*X4_STREAM_AU_PARAMETER_BYTES*X4_STREAM_AU_ENTRIES,queue.metadata_bytes,sizeof(*p));
    fputs("\"network_hud\":",f);x4_net_hud_write_json(f);fputc(',',f);
    fputs("\"menu_pipeline\":{\"conversion_without_menu_us\":",f);x4_ui_perf_write(f,&p->conversion_menu[0]);
    fputs(",\"conversion_with_menu_us\":",f);x4_ui_perf_write(f,&p->conversion_menu[1]);
    fprintf(f,",\"presented_with_menu\":%llu,\"discarded_latency_while_open\":%llu,\"discarded_ring_full_while_open\":%llu,\"scope\":\"conversion tagged at dispatch; presentation tagged at overlay then confirmed by flip; ring drops tagged at the event while open; excludes reconfiguration cancellations\"},",
        (unsigned long long)p->menu_presentations,(unsigned long long)p->menu_discarded_latency,(unsigned long long)p->menu_discarded_full);
    fputs("\"au_queue\":",f);write_au_queue(f,&queue);
    fputs(",\"keyframe_limiter\":",f);write_keyframe_limiter(f,p->keyframe_limiter);
    fputs(",\"blocking_waits\":",f);write_waits(f,p);
#if X4_VIDEO_PROBE && X4_PROBE_T26
    /* The offline bank owns no live-media/audio/input session. Its own
     * report does not call this legacy exporter; keep the unused TU linked
     * without enabling those product workers in the probe. */
    fputs(",\"interaction\":null",f);
#else
    fputs(",\"interaction\":",f);x4_live_interaction_metrics_write(f);
#endif
    fprintf(f,",\"synchronization\":{\"requested_policy\":%u,\"effective_vsync_at_end\":%s,\"mailbox_capable\":%s,\"scanouts\":%u,\"display_budget_extension_bytes\":%llu,\"decode_end_metric\":\"decode_end_to_flip_observation\",\"submit_end_metric\":\"submit_return_to_flip_observation\",\"scope\":\"each submitted slot captures its actual native mode; per-mode p50/p95/p99 and max in metrics.presentation_modes; local observation is not physical scanout or input latency\"}",
        (unsigned)x4_stream_request_vsync_policy(&p->request),x4_stream_diagnostics_vsync_effective(p->diagnostics)?"true":"false",
        p->display.mailbox_capable?"true":"false",p->scanout_count,(unsigned long long)(p->scanout_count==4u?STREAM_SCANOUT_BYTES:0u));
    fprintf(f,",\"mailbox\":{\"ready_replaced\":%llu,\"old_outputs_discarded\":%llu,\"late_opportunities\":%llu,\"late_observed_flips\":%llu,\"anchors\":%llu,\"invalid_anchors\":%llu,\"residual_samples\":%llu,\"residual_sum_us\":%llu,\"residual_max_us\":%llu,\"period_estimate_us\":%llu,\"margin_us\":%llu,\"warmup_submissions\":%llu,\"clock_scope\":\"nominal 60Hz seed; validated VSYNC FlipStatus.ptime phase/deltas; untyped GetVblankStatus unused; unknown or stale phase falls back to immediate VSYNC; estimate is not a hard real-time guarantee\"}",
        (unsigned long long)p->display.mailbox_replaced,(unsigned long long)p->display.mailbox_discarded_old,
        (unsigned long long)p->mailbox_clock.missed_opportunities,(unsigned long long)p->mailbox_late_flips,
        (unsigned long long)p->mailbox_clock.anchors,(unsigned long long)p->mailbox_clock.invalid_anchors,
        (unsigned long long)p->mailbox_clock.residual_samples,(unsigned long long)p->mailbox_clock.residual_sum_us,
        (unsigned long long)p->mailbox_clock.residual_max_us,(unsigned long long)p->mailbox_clock.period_us,
        (unsigned long long)p->mailbox_clock.margin_us,(unsigned long long)p->mailbox_clock.warmup_submissions);
    fprintf(f,",\"cpu\":{\"policy_version\":2,\"variant\":%u,\"partially_applied\":%s,\"status_es\":\"%s\",\"native_decoder\":{\"adapter_start_attempted\":%s,\"start_attempted\":%s,\"priority\":%d,\"affinity\":%llu,\"application_result_scope\":\"start_attempted means the same config reached native Query; Query/Create returns are under decoder_start, early adapter rejection is separate\"},\"decode_caller\":",
        (unsigned)p->request.cpu_variant,cpu_partial(p)?"true":"false",cpu_partial(p)?"parcialmente aplicada":"sin fallos de ajuste observados",p->native_start_attempted?"true":"false",
        p->native_start_attempted&&p->native_info.decoder_cpu_priority!=0&&p->native_info.query_result!=INT32_MIN?"true":"false",
        p->native_info.decoder_cpu_priority,(unsigned long long)p->native_info.decoder_cpu_affinity);
    x4_stream_cpu_write(f,&p->decode_cpu_report);fputs(",\"converters\":[",f);
    for(unsigned i=0;i<2;++i){if(i)fputc(',',f);x4_stream_cpu_write(f,&p->converters[i].cpu_report);}
    fputs("],\"assembly\":",f);if(p->ingress_cpu_recorded)x4_stream_cpu_write(f,&p->ingress_cpu_report);else fputs("null",f);
    fputs(",\"starvation_observation\":",f);write_cpu_starvation(f,p);fputs("}",f);
    fprintf(f,",\"main_polling\":{\"in_flight_timeout_us\":1000,\"idle_timeout_us\":5000,\"state_owner\":\"main\",\"scope\":\"conversion or pending flip uses 1ms; idle uses 5ms; controller_read_interval measures starts of actual main-loop pad reads during the live session\"},\"flip_timing\":{\"valid_native_timestamps\":%llu,\"invalid_native_timestamps\":%llu,\"unmatched_previous_flips\":%llu,\"last_raw_ptime\":%llu,\"last_submitted_us\":%llu,\"last_observed_us\":%llu,\"scope\":\"SDK ptime accepted only between local submit and observation process timestamps; units are corroborated by a pinned emulator, not documented by the SDK; invalid native timestamps are omitted, never replaced with observation time; final flip without a following conversion has no sample\"}",
        (unsigned long long)p->flip_time_valid,(unsigned long long)p->flip_time_invalid,
        (unsigned long long)p->flip_to_conversion_pending_omitted,
        (unsigned long long)p->last_flip_ptime,(unsigned long long)p->last_flip_submitted_us,(unsigned long long)p->last_flip_observed_us);
    unsigned offered_as[4]={0},answer_as[4]={0};
    x4_trace_sdp_bandwidth_get(p->trace,0,offered_as);x4_trace_sdp_bandwidth_get(p->trace,1,answer_as);
    fprintf(f,",\"sdp_bandwidth\":{\"scope\":\"counts and numeric AS values only; a request is not delivered bitrate\",\"offer\":{\"video_sections\":%u,\"AS_lines\":%u,\"first_AS_kbps\":%u,\"malformed_AS_lines\":%u},\"answer\":{\"video_sections\":%u,\"AS_lines\":%u,\"first_AS_kbps\":%u,\"malformed_AS_lines\":%u}}",
        offered_as[0],offered_as[1],offered_as[2],offered_as[3],answer_as[0],answer_as[1],answer_as[2],answer_as[3]);
    fprintf(f,",\"shutdown_waits\":{\"stop_requested_us\":%llu,\"halt_begin_us\":%llu,\"halt_end_us\":%llu,\"decoder_join_wait_us\":%llu,\"native_stop_call_us\":%llu,\"native_decode_after_stop_us\":%llu,\"native_decode_started_after_stop\":%llu,\"scope\":\"blocking wait cancellation is bounded to 20ms; native_decode_after_stop_us uses the adapter timestamps exactly around the SCE Decode call, excluding input copy/validation and branches without a SCE call; overlap starts at the first explicit pipeline stop request; Delete is native_stop_call_us; neither native call can be interrupted and buffers remain owned until actual joins and Delete\"}",
        (unsigned long long)atomic_load_explicit(&p->stop_requested_us,memory_order_acquire),
        (unsigned long long)p->halt_begin_us,(unsigned long long)p->halt_end_us,
        (unsigned long long)p->decoder_join_wait_us,(unsigned long long)p->native_stop_us,
        (unsigned long long)p->native_decode_after_stop_us,(unsigned long long)p->native_decode_started_after_stop);
    uint64_t rejected=atomic_load(&p->callback_rejected),full=atomic_load(&p->callback_full_design_failures);
    uint64_t stall_recoveries=atomic_load(&p->ingress_stall_recoveries);
    fprintf(f,",\"rtp_callback_admission\":{\"rejected\":%llu,\"full_design_failures\":%llu,\"ingress_stall_recoveries\":%llu,\"design_failure\":%s,\"scope\":\"callback queue FULL is a separate design failure, never an AU queue policy drop; other rejects remain distinct\"},",
        (unsigned long long)rejected,(unsigned long long)full,(unsigned long long)stall_recoveries,full?"true":"false");
    fprintf(f,"\"ingress_stall_recoveries\":%llu,", (unsigned long long)stall_recoveries);
    X4InboundRtpSummary summary = {0};
    x4_rtc_receive_diag_summary(&summary);
    fprintf(f, "\"inbound_rtp\":[");
    for (unsigned i = 0; i < summary.stream_count; ++i) {
        if (i) fputc(',', f);
        fprintf(f, "{"
#if !X4_PUBLIC
            "\"ssrc\":%u,"
#endif
            "\"payload_type\":%u,\"packets\":%llu,\"first_us\":%llu,\"last_us\":%llu}",
#if !X4_PUBLIC
            summary.streams[i].ssrc,
#endif
            summary.streams[i].payload_type,
            (unsigned long long)summary.streams[i].packets,
            (unsigned long long)summary.streams[i].first_us,
            (unsigned long long)summary.streams[i].last_us);
    }
    fprintf(f, "],\"srtp_decrypt_failures\":%llu,\"unknown_ssrc_packets\":%llu,\"unknown_pt_packets\":%llu,",
        (unsigned long long)summary.srtp_decrypt_failures,
        (unsigned long long)summary.unknown_ssrc_packets,
        (unsigned long long)summary.unknown_pt_packets);
    fprintf(f, "\"post_halt_video_packets\":%llu,\"first_post_halt_video_us\":%llu,",
        (unsigned long long)atomic_load_explicit(&p->post_halt_video_packets, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->first_post_halt_video_us, memory_order_relaxed));
    fprintf(f, "\"sdp_video\":{"
#if !X4_PUBLIC
        "\"ssrc\":%u,"
#endif
        "\"payload_type\":%u,\"mid\":\"%s\"},",
#if !X4_PUBLIC
        summary.sdp_video_ssrc,
#endif
        summary.sdp_video_pt, summary.sdp_video_mid[0] ? summary.sdp_video_mid : "");
    uint64_t alias_sent = x4_stream_startup_alias_sent_us(&p->startup);
    uint64_t first_rtp = atomic_load_explicit(&p->first_video_rtp_us, memory_order_acquire);
    uint64_t initial_delay = (first_rtp && alias_sent && first_rtp >= alias_sent) ? (first_rtp - alias_sent) : 0;
    bool session_started = p->startup.admitted;
    bool session_expired = p->startup.timed_out;
    fprintf(f,"\"initial_video_delay_us\":%llu,\"stream_startup\":{\"initial_video_delay_us\":%llu,\"alias_resends\":%u,\"started\":%s,\"expired\":%s,\"alias_sent_us\":%llu,\"first_video_rtp_us\":%llu,\"request_mode\":%u,\"alias_sends\":%u,\"last_alias_send_us\":%llu},",
        (unsigned long long)initial_delay,
        (unsigned long long)initial_delay,
        (unsigned)p->startup.alias_retries,
        session_started ? "true" : "false",
        session_expired ? "true" : "false",
        (unsigned long long)alias_sent,
        (unsigned long long)first_rtp,
        (unsigned)p->startup.request_mode,
        (unsigned)atomic_load_explicit(&p->startup.alias_sends, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->startup.last_alias_send_us, memory_order_relaxed));
    x4_server_settings_write_json(f);
    x4_rtc_diag_write_ice_json(f);
    x4_rtc_diag_write_rtt_json(f);
#if defined(X4_OPENORBIS) && X4_PUBLIC
    x4_rtc_diag_write_udp_json(f);
#endif
#if X4_PUBLIC && !X4_VIDEO_PROBE && (defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST))
    write_remb(f,&remb);
#endif
    fprintf(f,"\"requested\":{\"width\":%u,\"height\":%u,\"fps\":%u,\"bitrate_kbps\":%u,\"os_name\":\"%s\",\"display_info\":%s},",
        p->request.width,p->request.height,p->request.fps,p->request.bitrate_kbps,p->request.os_name,p->request.display_info?"true":"false");
    fprintf(f,"\"request_profile\":{\"id\":%u,\"label\":\"%s\",\"color_preference\":%u},",
        p->request.profile,p->request.label,p->request.color_preference);
    fprintf(f,"\"status\":{\"error\":%d,\"retained\":%s,\"display_detached\":%s,\"accepted_AU\":%llu,\"consumed_AU\":%llu,\"native_outputs\":%llu,\"converted_outputs\":%llu,\"presentations\":%llu,\"width\":%u,\"height\":%u,\"native_width\":%u,\"native_height\":%u,\"pitch\":%u,\"rejected_width\":%u,\"rejected_height\":%u,\"decoder_close\":%d},",
        atomic_load(&p->error),p->retained?"true":"false",p->view.display_detached?"true":"false",(unsigned long long)atomic_load(&p->submitted),(unsigned long long)atomic_load(&p->consumed),(unsigned long long)p->view.outputs,(unsigned long long)p->view.conversions,(unsigned long long)p->view.presentations,p->view.width,p->view.height,p->view.native_width,p->view.native_height,p->view.pitch,p->view.rejected_width,p->view.rejected_height,p->decoder_close_result);
    const X4StreamSpsDiagnostics *d=&v->sps_diagnostics;
    fputs("\"delivered_sps\":",f);write_sps(f,&v->sps,d);
    fputs(",\"sps_diagnostics\":",f);write_sps_diagnostics(f,d,p->native_start_attempted);fputc(',',f);
    fputs("\"parameter_sets\":{\"scope\":\"first AU submitted to native startup; prefix retains original Annex-B framing\",\"sps\":",f);write_parameter(f,p->sps_prefix,p->sps_size);
    fputs(",\"pps\":",f);write_parameter(f,p->pps_prefix,p->pps_size);fputs("},\"sdp_profiles\":{\"offered\":",f);write_sdp_profiles(f,p->trace,0);
    fputs(",\"accepted_answer\":",f);write_sdp_profiles(f,p->trace,1);fputs("},",f);
    fputs("\"decoder_start\":",f);write_decoder_start(f,v);fputs(",\"latest_observed_sps\":",f);write_sps(f,&p->observed_sps,&p->observed_diagnostics);
    fputs(",\"latest_sps_diagnostics\":",f);write_sps_diagnostics(f,&p->observed_diagnostics,p->observed_sps_size!=0);
    fputs(",\"latest_parameter_sets\":{\"sps\":",f);write_parameter(f,p->observed_sps_prefix,p->observed_sps_size);fputs(",\"pps\":",f);write_parameter(f,p->observed_pps_prefix,p->observed_pps_size);fputs("},\"recovery\":",f);write_recovery(f,p);fputc(',',f);
    fprintf(f,"\"color\":{\"active\":%s,\"received_matrix_coefficients\":%u,\"matrix_coefficients\":%u,\"preference\":%u,\"selected_matrix\":\"%s\",\"full_range\":%s,\"full_range_implemented\":true},\"dequeued_AU\":%llu,\"geometry_warnings\":%llu,",
        p->view.native_ready?"true":"false",p->view.received_matrix,p->color.matrix_coefficients,p->request.color_preference,p->color.matrix_coefficients==1?"BT709":"BT601",p->color.full_range?"true":"false",(unsigned long long)atomic_load(&p->dequeued),(unsigned long long)v->geometry_warnings);
    fprintf(f,"\"conversion_path\":{\"observed\":%s,\"last\":\"%s\",\"scalar_outputs\":%llu},",p->conversion_path_seen?"true":"false",p->conversion_path==X4_PIPELINE_CONVERT_SSE2?"SSE2":"scalar",(unsigned long long)p->scalar_outputs);
    fputs("\"gpu_conversion\":",f);(void)x4_stream_diagnostics_write_gpu(f,&p->gpu_metrics);fputc(',',f);
    uint64_t conv_p50=0, conv_p95=0, conv_max=0;
    const char *sharp_names[]={"No","Baja","Media"};
    (void)x4_stream_diagnostics_stage_percentile(p->diagnostics,X4_STREAM_CONVERSION,50,&conv_p50,&conv_max);
    (void)x4_stream_diagnostics_stage_percentile(p->diagnostics,X4_STREAM_CONVERSION,95,&conv_p95,NULL);
    fprintf(f,"\"conversion_tuning\":{\"mode\":\"%s\",\"sharpness\":\"%s\",\"sharpness_level\":%u,\"timing_us\":{\"p50_upper_us\":%llu,\"p95_upper_us\":%llu,\"maximum_us\":%llu}},",
        p->color.convert_mode?"Suave":"Original",
        sharp_names[p->color.sharpness<=2?p->color.sharpness:0],
        (unsigned)p->color.sharpness,
        (unsigned long long)conv_p50,(unsigned long long)conv_p95,(unsigned long long)conv_max);
    fprintf(f,"\"decoder_depth\":{\"selection\":%u,\"requested\":%u,\"active\":%u,\"fallback\":%s,\"input_buffers\":%u,\"output_buffers\":%u,\"calls_with_output\":%llu,\"calls_without_output\":%llu,\"calls_with_native_error\":%llu,\"output_epochs_with_picture\":%llu,\"first_output_us\":%llu,\"last_output_us\":%llu,\"native_budget_bytes\":%llu,\"auxiliary_budget_bytes\":%llu,\"held_output_mask_after_close\":%u,\"live_inputs_after_close\":%u,\"input_retirement_posterior_calls\":%u,\"input_contract_SDK_verified\":false,\"flush_on_close\":false,\"output_interval_scope\":\"validated native outputs within each decoder epoch; caller pacing and IDR waits included; no PTS association\"},",
        p->request.decoder_depth,v->requested_depth,v->active_depth,v->depth_fallback_used?"true":"false",
        v->input_count,v->output_count,(unsigned long long)p->calls_with_output,(unsigned long long)p->calls_without_output,(unsigned long long)p->calls_with_error,
        (unsigned long long)p->native_output_epochs,(unsigned long long)p->first_native_output_us,(unsigned long long)p->last_valid_native_output_us,
        (unsigned long long)v->native_budget_bytes,(unsigned long long)v->budget_auxiliary_bytes,v->held_output_mask,v->live_input_count,v->active_depth==2?3:1);
    fprintf(f,"\"native\":{\"attempted\":%s,\"planned_bytes\":%llu,\"queried_compute\":%llu,\"queried_cpu\":%llu,\"queried_gpu\":%llu,\"queried_shared\":%llu,\"queried_frame\":%llu,\"compute_query\":%d,\"allocate_queue\":%d,\"query\":%d,\"create\":%d,\"last_decode\":%d,\"delete\":%d,\"release_queue\":%d,\"close\":%d,\"retained\":%s,\"creation_uncertain\":%s},",
        p->native_start_attempted?"true":"false",(unsigned long long)v->planned_native_bytes,(unsigned long long)v->queried_compute_bytes,(unsigned long long)v->queried_cpu_bytes,(unsigned long long)v->queried_gpu_bytes,(unsigned long long)v->queried_shared_bytes,(unsigned long long)v->queried_frame_bytes,v->compute_query_result,v->allocate_queue_result,v->query_result,v->create_result,p->native_decode_result,v->delete_decoder_result,v->release_queue_result,v->close_result,v->retained?"true":"false",v->creation_uncertain?"true":"false");
    fprintf(f,"\"ring\":{\"discarded_latency\":%llu,\"discarded_ring_full\":%llu,\"cancelled_leases\":%llu,\"rejected\":%llu,\"peak_occupied\":%u,\"peak_ready\":%u,\"final_occupied\":%u},\"scanout\":{\"peak_occupied\":%u,\"peak_in_flight\":%u,\"allocate\":%d,\"map\":%d,\"register\":%d,\"rate\":%d,\"close\":%d,\"unmap\":%d,\"release\":%d,\"gpu_ready_discarded_verification\":%llu},",
        (unsigned long long)p->ring.discarded_latency,(unsigned long long)p->ring.discarded_ready,(unsigned long long)p->ring.cancelled,(unsigned long long)p->ring.rejected,p->ring.peak_occupied,p->ring.peak_ready,p->ring.occupied,p->display.peak_occupied,p->display.peak_in_flight,p->display_allocate,p->display_map,p->display_register,p->display_rate,p->display_close,p->display_unmap,p->display_release,(unsigned long long)p->gpu_ready_discarded_verification);
    fprintf(f,"\"diagnostic_record_cost\":{\"calls\":%llu,\"total_us\":%llu,\"max_observed_lower_bound_us\":%llu,\"scope\":\"histogram updates only; excludes export, Trace and callback counters; maximum CAS may omit contention\"},\"stacks\":[",
        (unsigned long long)atomic_load(&p->diagnostic_calls),(unsigned long long)atomic_load(&p->diagnostic_us),(unsigned long long)atomic_load(&p->diagnostic_max_us));
    write_stack(f,ingress_thread);fputc(',',f);write_stack(f,&p->decoder_thread.report);for(unsigned i=0;i<2;++i){fputc(',',f);write_stack(f,&p->converters[i].thread.report);}fputs("],\"metrics\":",f);
    if(p->idr_recovery.waiting)x4_stream_diagnostics_evidence(p->diagnostics,X4_STREAM_PAUSE_WAIT_IDR);
    int first=x4_stream_diagnostics_write(f,p->diagnostics,p->ended)?EIO:0;fputs("}\n",f);
    if(ferror(f)&&!first)first=errno?errno:EIO;
    errno=0;if(fflush(f)&&!first)first=errno?errno:EIO;
    errno=0;int descriptor=fileno(f),sync_result=X4_PROBE_STACK_NOT_RUN;
    if(descriptor<0){if(!first)first=errno?errno:EIO;}
    else {
        errno=0;sync_result=sceKernelFsync(descriptor);
        if(sync_result&&!first)first=((uint32_t)sync_result&0xffff0000u)==0x80020000u?(int)((uint32_t)sync_result&0xffffu):EIO;
    }
    errno=0;if(fclose(f)&&!first)first=errno?errno:EIO;
#if X4_PUBLIC
    /* Same-directory POSIX rename replaces only the last public report.
     * A failed write keeps the previous complete report; old /data reports
     * from earlier versions are deliberately not touched. */
    if(!first){errno=0;if(rename(temporary,path))first=errno?errno:EIO;}
    if(first)(void)remove(temporary);
#endif
    p->exported=true;printf("XCloud4 stream: export path=%s result=%d errno=%d native_fsync=%d\n",path,first?-1:0,first,sync_result);
    return first?(first<0?first:-first):0;
}
int x4_stream_pipeline_free(X4StreamPipeline *p)
{
    if(!p)return 0;
    if(!p->closed||p->retained)return X4_PIPELINE_STATE;
    if(p->gpu){
        gpu_status(p);
        if(!x4_gpu_convert_close(&p->gpu,now_us())){gpu_status(p);return retain(p,X4_PROBE_STACK_RETAINED);}
        /* Successful close proves every owned native map/allocation retired.
         * Keep NOT_RUN for resources that were never acquired or mapped. */
        X4StreamGpuDiagnostics *g=&p->gpu_metrics;
        if(g->quality_arena_bytes){
            if(!g->quality_map_result)g->quality_unmap_result=0;
            if(!g->quality_reserve_result)g->quality_release_result=0;
        }
        if(g->quality_intermediate_bytes){
            if(!g->intermediate_map_result)g->intermediate_unmap_result=0;
            if(!g->intermediate_reserve_result)g->intermediate_release_result=0;
        }
    }
    free(p->notice_backing);p->notice_backing=NULL;
    free(p->verification_scratch);p->verification_scratch=NULL;
    x4_stream_au_queue_stop(p->au_queue);
    if(!x4_stream_au_queue_destroy(p->au_queue)){
        int rc=retain(p,X4_STREAM_AU_FATAL);(void)fail_named(p,rc,"AU queue retained during free");
        printf("XCloud4: stream AU queue free result=%d retained=1; parent must remain alive\n",rc);return rc;
    }
    p->au_queue=NULL;
    for(unsigned i=0;i<2;++i){int destroyed=x4_stream_wait_destroy(&p->converters[i].work_wait);if(destroyed)return retain(p,destroyed);}
    int destroyed=x4_stream_wait_destroy(&p->verification_wait);if(destroyed)return retain(p,destroyed);
    destroyed=x4_stream_wait_destroy(&p->decode_wait);if(destroyed)return retain(p,destroyed);
    destroyed=x4_stream_wait_destroy(&p->present_wait);if(destroyed)return retain(p,destroyed);
    /* Keep the status gate alive while any preceding destroy may need to
     * publish retention. A failed native destroy leaves its object owned. */
    destroyed=x4_stream_mutex_destroy(&p->ring_gate);if(destroyed)return retain(p,destroyed);
    p->ring_mutex_initialized=false;
    destroyed=x4_stream_mutex_destroy(&p->status_gate);if(destroyed)return retain(p,destroyed);
    p->status_mutex_initialized=false;
    x4_stream_diagnostics_free(p->diagnostics);free(p);return 0;
}
#endif
