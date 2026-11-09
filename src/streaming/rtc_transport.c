/* SPDX-License-Identifier: GPL-3.0-only */
/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
#include "rtc_transport.h"
#include "rtc_native.h"
#include "rtc_receive_diag.h"
#include "nack_grace_inbox.h"
#include "stream_profile.h"
#include "stream_sdp_bandwidth.h"
#include "stream_startup.h"
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include "keyframe_limiter.h"
#endif
#include "../input/rumble.h"
#include "../media/live_trace.h"
#include "../auth/json.h"
#if defined(X4_OPENORBIS) || defined(X4_MIC_HOST_TEST)
#define X4_MIC_TRANSPORT 1
#include "../audio/mic_capture.h"
#include "../audio/mic_rtp.h"
#include <time.h>
#else
#define X4_MIC_TRANSPORT 0
#endif
#include "../core/x4_log.h"
#if defined(X4_OPENORBIS) || defined(X4_RECOVERY43_TRANSPORT_TEST)
#define X4_RECOVERY43_TRANSPORT 1
#include "recovery43_bridge.h"
#include "recovery43_sdp.h"
#include "receiver_nack_state.h"
#else
#define X4_RECOVERY43_TRANSPORT 0
#endif
#include <rtc/rtc.h>
#include <orbis/libkernel.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if X4_RECOVERY43_TRANSPORT && X4_STREAM_PROFILE && (defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST))
#define X4_REMB_ENABLED 1
#include "remb_estimator.h"
typedef struct {
    X4RembSettings settings;
    X4RembEstimator estimator;
    X4Recovery43Snapshot repair;
    uint64_t sampled_us, expired, attempted_us, sent, refused, decreases, increases;
    uint32_t sender_ssrc, video_ssrc, attempted_bps, sent_bps, min_sent_bps, max_sent_bps;
    uint32_t received_bps, max_received_bps, loss_ppm, max_loss_ppm;
    int32_t delay_us, max_delay_us;
    bool started, negotiated, loss_valid, have_expired, delay_valid, have_max_delay, final_reported;
} X4RembState;
static void remb44a_begin(X4Rtc *);
static void remb44a_tick(X4Rtc *, uint64_t now);
#if X4_PUBLIC
enum { X4_REMB_PUBLIC_REPORT_INTERVAL_US = 30000000 };
/* Session owner resets before starting a peer and folds its counters after
 * joining the NACK sender. Export runs after the session owner has finished. */
static X4RembSummary remb_finished_peers;
static uint64_t remb_first_peer_us, remb_next_report_us, remb_final_pipeline_us;
static const X4Trace *remb_session_trace;
static void remb44c_periodic(const X4Rtc *, uint64_t now);
static void remb44c_finish_peer(const X4Rtc *);
#else
static void remb44a_report(const X4Rtc *, bool final);
#endif
extern int x4_rtc_send_video_remb(int video_track, uint32_t sender_ssrc,
    uint32_t video_ssrc, uint32_t bitrate_bps);
#else
#define X4_REMB_ENABLED 0
#endif
#line 40
#define SDP_CAP 32768u
#define CANDIDATE_CAP 1024u
#define MID_CAP 32u
#define CANDIDATE_COUNT 64u
#define CALLBACK_SLOTS 64u
#define INPUT_INTERVAL_USEC 16667u
#ifndef X4_RECEIVER_NACK
#define X4_RECEIVER_NACK 0
#endif
#ifndef X4_RUMBLE
#define X4_RUMBLE 0
#endif
#define NACK_POLL_USEC 500u

_Static_assert(sizeof(double) == 8, "Xbox input uses a float64 timestamp");

typedef struct { char candidate[CANDIDATE_CAP], mid[MID_CAP]; } Candidate;
typedef struct { X4Rtc *context; unsigned active; } CallbackSlot;
static CallbackSlot slots[CALLBACK_SLOTS];
static unsigned slots_used;
static atomic_flag callback_gate = ATOMIC_FLAG_INIT;

enum LifetimeStep {
    LIFETIME_TYPE, LIFETIME_ID, LIFETIME_CONTENT, LIFETIME_REASON, LIFETIME_TIMER,
    LIFETIME_IDLE, LIFETIME_TERMINAL, LIFETIME_REPEAT
};

#if !X4_PUBLIC
enum { LAB_MESSAGE_LINES = 64, LAB_LIFETIME_LINES = 8 };
enum LabTarget {
    LAB_TARGET_ABSENT, LAB_TARGET_OTHER, LAB_TARGET_SYSTEM_UI, LAB_TARGET_SYSTEM_UI_CONFIG,
    LAB_TARGET_LIFETIME, LAB_TARGET_SERVER_CLOSE, LAB_TARGET_COUNT
};
enum LabIdKind { LAB_ID_ABSENT, LAB_ID_OTHER, LAB_ID_STRING, LAB_ID_NUMBER };
enum LabClosure {
    LAB_CLOSE_CHAT, LAB_CLOSE_CONTROL, LAB_CLOSE_INPUT, LAB_CLOSE_MESSAGE,
    LAB_CLOSE_SCTP, LAB_CLOSE_DTLS, LAB_CLOSE_PEER, LAB_CLOSE_LOCAL_CHAT,
    LAB_CLOSE_LOCAL_CONTROL, LAB_CLOSE_LOCAL_INPUT, LAB_CLOSE_LOCAL_MESSAGE, LAB_CLOSE_COUNT
};
typedef struct {
    atomic_bool ready;
    uint64_t at;
    unsigned arrival;
    int state_or_rc;
} LabClosureSample;
typedef struct {
    atomic_bool ready;
    const char *type; /* fixed label only */
    enum LabTarget target;
    size_t content_wire_bytes;
    bool control;
    uint8_t id_kind;
    char content[161]; /* canonical public fields only */
} LabMessageSample;
typedef struct {
    atomic_bool ready;
    enum LifetimeStep step;
    int ack_rc;
} LabLifetimeSample;
static atomic_uint lab_message_lines, lab_close_arrival, lab_close_order;
static atomic_uint_fast64_t lab_first_video;
/* The pinned native diagnostic hook has no per-peer user pointer. The
 * session owner closes and drains the current peer before opening another. */
static X4Rtc *lab_transport_context;
#endif

struct X4Rtc {
#if X4_STREAM_PROFILE
    X4StreamRequestConfig request_config;
#endif
    int pc, video, audio, channels[4];
    CallbackSlot *slot;
    atomic_int state, error;
    atomic_bool gathering_done, offer_logged;
    atomic_uint_fast64_t video_packets, audio_packets;
    atomic_flag data_gate;
    char sdp[SDP_CAP];
    Candidate candidates[CANDIDATE_COUNT];
    unsigned candidate_read, candidate_write;
    X4RtcMediaCallback media;
    void *media_user;
    X4Trace *diagnostic_trace;
    X4NackGraceInbox *grace_inbox;
#if X4_RUMBLE
    X4Rumble *rumble;
#endif
    uint64_t diagnostic_pli_ordinal; /* Session owner is the sole writer. */
    bool handshake_ack, startup_sent;
    bool reconnect_peer;
#ifdef X4_OPENORBIS
    bool udp_buffers_logged; /* Session owner only. */
#endif
    uint64_t started_at;
    X4GamepadSource input_source;
    void *input_user;
    OrbisPthread input_thread;
    bool input_thread_started;
    OrbisPthread nack_thread;
    bool nack_thread_started;
    atomic_bool nack_stop;
    uint32_t nack_epoch;
    int input_join_error;
    uint32_t input_sequence;
    atomic_bool input_stop, input_ready, input_metadata_sent, startup_complete;
    atomic_int input_error;
    atomic_uint_fast64_t input_packets, input_dropped;
    atomic_bool server_terminal_claimed, server_closed;
    atomic_uint_fast64_t server_idle_sequence;
    atomic_uint server_idle_seconds;
    /* Written once by the terminal-notice winner, published by server_closed.
     * Snapshot never reads them before that acquire; no callback spins. */
    bool server_hr_valid;
    uint32_t server_hr;
    char server_reason[41];
#if X4_STREAM_PROFILE
    X4StreamStartup *stream_startup;
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4KeyframeLimiter *keyframe_limiter;
    uint64_t keyframe_grant_consumed; /* session owner only, before any I/O */
#endif
    uint32_t sdp_video_ssrc, sdp_video_pt;
    char sdp_video_mid[32];
#if !X4_PUBLIC
    atomic_uint lab_close_mask;
    atomic_uint_fast64_t lab_target_counts[LAB_TARGET_COUNT];
    LabClosureSample lab_closures[LAB_CLOSE_COUNT];
    LabMessageSample lab_messages[LAB_MESSAGE_LINES];
    unsigned lab_next_message; /* session owner only */
    atomic_uint lab_lifetime_reserved;
    LabLifetimeSample lab_lifetime[LAB_LIFETIME_LINES];
    LabLifetimeSample lab_lifetime_terminal;
    unsigned lab_next_lifetime; /* session owner only */
    bool lab_lifetime_terminal_logged; /* session owner only */
#endif
#if X4_MIC_TRANSPORT
    uint64_t mic_generation;
    X4MicRtpClock mic_clock;
    OrbisPthread mic_thread;
    atomic_bool mic_stop, mic_done;
    bool mic_thread_started;
#endif
#if X4_RECOVERY43_TRANSPORT
    /* Append-only: all pre-existing input/audio/video offsets stay fixed. */
    unsigned recovery43_mode;
#if !X4_PUBLIC
    unsigned recovery43_loss;
    uint32_t recovery43_seed;
    uint64_t recovery43_next_poll; /* NACK owner only; final is after join. */
#endif
    bool recovery43_started, recovery43_answered, recovery43_offer_failed;
    X4Recovery43Offer recovery43_offered;
#endif
#if X4_REMB_ENABLED
    X4RembState remb;
#endif
#line 190
};

#if X4_RECOVERY43_TRANSPORT
static void recovery43_prepare(X4Rtc *rtc)
{
#if X4_STREAM_PROFILE
    rtc->recovery43_mode=x4_stream_request_recovery_mode(&rtc->request_config);
#elif X4_PUBLIC
    rtc->recovery43_mode=2;
#endif
#if X4_PUBLIC
    rtc->recovery43_started=x4_recovery43_begin(rtc->nack_epoch,rtc->recovery43_mode,0,0)==0;
    if(!rtc->recovery43_started)rtc->recovery43_mode=0;
#else
#if X4_STREAM_PROFILE
    rtc->recovery43_loss=x4_stream_request_loss_percent(&rtc->request_config);
#endif
    int random_rc=x4_native_random(&rtc->recovery43_seed,sizeof(rtc->recovery43_seed));
    if(random_rc<0||!rtc->recovery43_seed){
        x4_log_printf("XCloud4: recuperacion43 sin semilla rc=%d; Solo NACK sin perdida\n",random_rc);
        rtc->recovery43_mode=rtc->recovery43_loss=0;
        rtc->recovery43_seed=0;
    }
    int rc=x4_recovery43_begin(rtc->nack_epoch,rtc->recovery43_mode,
        rtc->recovery43_loss,rtc->recovery43_seed);
    rtc->recovery43_started=rc==0;
    if(rc){rtc->recovery43_mode=rtc->recovery43_loss=0;}
    x4_log_printf("XCloud4: recuperacion43 inicio modo=%u perdida=%u semilla=%u rc=%d\n",
        rtc->recovery43_mode,rtc->recovery43_loss,rtc->recovery43_seed,rc);
    if(!rtc->recovery43_mode&&rtc->recovery43_loss)
        x4_log_printf("XCloud4: Con Solo NACK la pérdida simulada no se puede recuperar "
            "(protección anti-repetición del SRTP); usar RTX o una red real\n");
#endif
}
static void recovery43_accept(X4Rtc *rtc,const char *sdp)
{
    if(rtc->recovery43_answered)return;
    if(!rtc->recovery43_started){
#if X4_PUBLIC
        rtc->recovery43_answered=true;
        x4_log_printf("XCloud4: recuperacion43 negociacion rtx=0 fec=0\n");
#endif
        return;
    }
    X4Recovery43Negotiation answer={0};
    unsigned mode=rtc->recovery43_offer_failed?0:rtc->recovery43_mode;
    int parse_rc=x4_recovery43_sdp_answer(sdp,strlen(sdp),mode,&answer);
#if X4_PUBLIC
    /* Only activate repair if the server accepted both offered mappings. */
    if(mode&&(!parse_rc)&&(!answer.rtx_accepted||!answer.fec_accepted)){
        mode=0;
        parse_rc=x4_recovery43_sdp_answer(sdp,strlen(sdp),mode,&answer);
    }
#endif
    X4VideoRepair43Config config={.mode=mode,
#if !X4_PUBLIC
        .loss_percent=rtc->recovery43_loss,.seed=rtc->recovery43_seed,
#endif
        .media_ssrc=answer.media_ssrc,
        .rtx_ssrc=answer.rtx_ssrc,.fec_ssrc=answer.fec_ssrc,
        .media_pt=answer.media_pt,.rtx_pt=answer.rtx_pt,.fec_pt=answer.fec_pt,
        .rtx_accepted=answer.rtx_accepted,.fec_accepted=answer.fec_accepted,
        .mutable_extensions=answer.mutable_extensions};
    int rc=parse_rc?parse_rc:x4_recovery43_answer(rtc->nack_epoch,&config);
    rtc->recovery43_answered=true;
#if X4_PUBLIC
    x4_log_printf("XCloud4: recuperacion43 negociacion rtx=%u fec=%u\n",
        !rc&&answer.rtx_accepted?1u:0u,!rc&&answer.fec_accepted?1u:0u);
#else
    unsigned applied=rc?0u:answer.fec_accepted?2u:answer.rtx_accepted?1u:0u;
    x4_log_printf("XCloud4: recuperacion43 respuesta solicitado=%u aplicado=%u "
        "rtx=%s fec=%s pt_video=%u pt_rtx=%u pt_fec=%u ext_mutable=%u "
        "perdida_solicitada=%u perdida_aplicada=%u parse_rc=%d setup_rc=%d\n",
        rtc->recovery43_mode,applied,!rc&&answer.rtx_accepted?"aceptado":"no",
        !rc&&answer.fec_accepted?"aceptado":"no",answer.media_pt,answer.rtx_pt,answer.fec_pt,
        answer.mutable_extensions?1u:0u,rtc->recovery43_loss,rc?0u:rtc->recovery43_loss,parse_rc,rc);
#endif
}
#endif

static void lock(atomic_flag *flag) { while (atomic_flag_test_and_set_explicit(flag, memory_order_acquire)) {} }
static void unlock(atomic_flag *flag) { atomic_flag_clear_explicit(flag, memory_order_release); }

static X4Rtc *enter(void *pointer)
{
    CallbackSlot *slot = pointer;
    if (!slot) return NULL;
    lock(&callback_gate);
    X4Rtc *context = slot->context;
    if (context) ++slot->active;
    unlock(&callback_gate);
    return context;
}
static void leave(CallbackSlot *slot)
{
    lock(&callback_gate);
    --slot->active;
    unlock(&callback_gate);
}
#if !X4_PUBLIC
static void lab_peer_diagnostics_init(X4Rtc *rtc)
{
    atomic_init(&rtc->lab_close_mask, 0);
    for (unsigned i = 0; i < LAB_TARGET_COUNT; ++i) atomic_init(&rtc->lab_target_counts[i], 0);
    for (unsigned i = 0; i < LAB_CLOSE_COUNT; ++i) atomic_init(&rtc->lab_closures[i].ready, false);
    for (unsigned i = 0; i < LAB_MESSAGE_LINES; ++i) atomic_init(&rtc->lab_messages[i].ready, false);
    rtc->lab_next_message = atomic_load_explicit(&lab_message_lines, memory_order_relaxed);
    atomic_init(&rtc->lab_lifetime_reserved, 0);
    for (unsigned i = 0; i < LAB_LIFETIME_LINES; ++i) atomic_init(&rtc->lab_lifetime[i].ready, false);
    atomic_init(&rtc->lab_lifetime_terminal.ready, false);
    rtc->lab_next_lifetime = 0;
    rtc->lab_lifetime_terminal_logged = false;
}

static void lab_observe_close(X4Rtc *rtc, enum LabClosure event, int state_or_rc)
{
    uint64_t at = sceKernelGetProcessTime();
    if (atomic_fetch_or_explicit(&rtc->lab_close_mask, 1u << event, memory_order_relaxed) & (1u << event))
        return;
    LabClosureSample *sample = &rtc->lab_closures[event];
    sample->at = at;
    sample->arrival = atomic_fetch_add_explicit(&lab_close_arrival, 1, memory_order_relaxed);
    sample->state_or_rc = state_or_rc;
    atomic_store_explicit(&sample->ready, true, memory_order_release);
}

/* Session owner only, after all callbacks have drained. Report by monotonic
 * timestamp, with capture order as the tie breaker. No native callback logs. */
static void lab_emit_closures(const X4Rtc *rtc)
{
    static const char *const labels[LAB_CLOSE_COUNT] = {
        "chat", "control", "input", "message", "SCTP", "DTLS", "peer",
        "chat", "control", "input", "message"
    };
    unsigned pending = 0;
    for (unsigned i = 0; i < LAB_CLOSE_COUNT; ++i)
        if (atomic_load_explicit(&rtc->lab_closures[i].ready, memory_order_acquire))
            pending |= 1u << i;
    uint64_t first = atomic_load_explicit(&lab_first_video, memory_order_relaxed);
    while (pending) {
        unsigned event = LAB_CLOSE_COUNT;
        for (unsigned i = 0; i < LAB_CLOSE_COUNT; ++i) {
            if (!(pending & (1u << i))) continue;
            const LabClosureSample *sample = &rtc->lab_closures[i];
            if (event == LAB_CLOSE_COUNT || sample->at < rtc->lab_closures[event].at ||
                (sample->at == rtc->lab_closures[event].at && sample->arrival < rtc->lab_closures[event].arrival))
                event = i;
        }
        pending &= ~(1u << event);
        const LabClosureSample *sample = &rtc->lab_closures[event];
        unsigned order = atomic_fetch_add_explicit(&lab_close_order, 1, memory_order_relaxed) + 1u;
        int64_t ms = first && sample->at >= first ? (int64_t)((sample->at - first) / 1000u) : -1;
        x4_log_printf("XCloud4: RTC cierre orden=%u peer=%u desde_video_ms=%lld parte=%s origen=%s estado_rc=%d\n",
            order, rtc->nack_epoch, (long long)ms, labels[event],
            event >= LAB_CLOSE_LOCAL_CHAT ? "liberacion_local" : "observado", sample->state_or_rc);
    }
}

void x4_rtc_lab_native_transport_state(int source, int state)
{
    /* prepare_port.py assigns native event 80 to DTLS and 87 to SCTP.
     * libdatachannel's pinned Transport::State uses Disconnected=0, Failed=4.
     * Never enable the dependency's textual logger. */
    if (state != 0 && state != 4) return;
    enum LabClosure event;
    if (source == 80) event = LAB_CLOSE_DTLS;
    else if (source == 87) event = LAB_CLOSE_SCTP;
    else return;
    lock(&callback_gate);
    X4Rtc *rtc = lab_transport_context;
    if (rtc) ++rtc->slot->active;
    unlock(&callback_gate);
    if (rtc) {
        lab_observe_close(rtc, event, state);
        leave(rtc->slot);
    }
}

static void lab_channel_closed(int channel, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    for (unsigned i = 0; i < 4; ++i) {
        if (rtc->channels[i] != channel) continue;
        lab_observe_close(rtc, (enum LabClosure)(LAB_CLOSE_CHAT + i), 0);
        break;
    }
    leave(pointer);
}

static void lab_mark_first_video(void)
{
    uint_fast64_t absent = 0;
    (void)atomic_compare_exchange_strong_explicit(&lab_first_video, &absent,
        sceKernelGetProcessTime(), memory_order_relaxed, memory_order_relaxed);
}
#endif

static void fail(X4Rtc *rtc, int error)
{
    atomic_store(&rtc->error, error);
    atomic_store(&rtc->state, X4_RTC_FAILED);
}
/* Identical observer/dispatcher scheduling in ON and OFF builds. Reception
 * only creates bounded numeric requests; existing protected transport sends
 * happen here, never while the receive state gate is held. A 500us sleep is
 * a requested cadence, not a promised dispatch deadline. */
#if X4_RECOVERY43_TRANSPORT && !X4_PUBLIC && defined(X4_OPENORBIS) && X4_STREAM_PROFILE
static void recovery43b_report(const X4Rtc *rtc,bool final)
{
    if(rtc->video<0)return;
    X4ReceiverNackMetrics nack={0};
    rtcX4VideoRrSnapshot rr={0};
    int nack_rc=x4_rtc_receiver_nack_metrics(rtc->video,&nack);
    int rr_rc=rtcX4GetVideoRrSnapshot(rtc->video,&rr);
    const char *const nack_names[]={"T43c","T43","T43b","T46b"};
    const char *const rr_names[]={"cero","reparados","edge"};
    X4StreamNackPolicy policy=x4_stream_request_nack_policy(&rtc->request_config);
    X4StreamRrMode rr_mode=x4_stream_request_rr_mode(&rtc->request_config);
    unsigned grace_ms,min_ms,max_ms;
    x4_rtc_diag_video_grace_stats(policy,sceKernelGetProcessTime(),&grace_ms,&min_ms,&max_ms);
    unsigned current_us=policy==X4_NACK_POLICY_T43B&&!nack_rc?nack.grace_us:grace_ms*1000u;
    x4_log_printf("XCloud4: recuperacion43b fase=%s nack=%s rr=%s gracia_us=%u "
        "gracia_min_ms=%u gracia_max_ms=%u rtt_p50_ms=%d "
        "peticiones=%llu reintentos=%llu posiciones=%llu suprimidas_presupuesto=%llu "
        "vencidos_sin_primero=%llu vencidos_reintento=%llu vencidos_reintento_sin_retorno=%llu "
        "vencidos_sin_envio=%llu retorno_reintento_p50_us=%llu retorno_reintento_p95_us=%llu "
        "retorno_primero_p95_us=%u retorno_primero_muestras=%u "
        "liberaciones_arranque=%llu fci=%llu rtcp_nack=%llu nack_rc=%d "
        "rr_enviados=%llu fraccion=%u fraccion_max=%u rr_fraccion_positiva=%llu acumulada=%d "
        "seq_ext=%u jitter90k=%u lsr=%u dlsr=%u rr_rc=%d\n",
        final?"fin":"periodico",nack_names[policy],rr_names[rr_mode],
        current_us,min_ms,max_ms,x4_rtc_diag_rtt_p50_ms(),
        (unsigned long long)nack.initial_requests,(unsigned long long)nack.retries,
        (unsigned long long)nack.requested_positions,(unsigned long long)nack.budget_suppressed_positions,
        (unsigned long long)nack.expired_no_first,(unsigned long long)nack.expired_retry_late,
        (unsigned long long)nack.expired_retry_no_response,(unsigned long long)nack.expired_unsent,
        (unsigned long long)nack.retry_return_p50_us,(unsigned long long)nack.retry_return_p95_us,
        nack.first_return_p95_us,nack.first_return_samples,
        (unsigned long long)nack.startup_releases,
        (unsigned long long)nack.fci_total,(unsigned long long)nack.rtcp_packets,nack_rc,
        (unsigned long long)rr.reports,(unsigned)rr.fractionLost,(unsigned)rr.maxFractionLost,
        (unsigned long long)rr.reportsWithLoss,(int)rr.cumulativeLost,
        (unsigned)rr.extendedSequence,(unsigned)rr.jitter,(unsigned)rr.lsr,(unsigned)rr.dlsr,rr_rc);
}
#endif
static void *nack_sender(void *pointer)
{
    X4Rtc *rtc = pointer;
    while (!atomic_load_explicit(&rtc->nack_stop, memory_order_acquire)) {
        (void)x4_rtc_poll_receiver_nack(rtc->video);
#if X4_RECOVERY43_TRANSPORT && !X4_PUBLIC
        uint64_t now=sceKernelGetProcessTime();
#if X4_REMB_ENABLED
        remb44a_tick(rtc,now);
#endif
#line 443
#if defined(X4_OPENORBIS) && X4_STREAM_PROFILE
        x4_rtc_diag_startup_report(now,false);
#endif
        if(now>=rtc->recovery43_next_poll){
            if(rtc->recovery43_started)x4_recovery43_poll(rtc->nack_epoch,now,false);
#if defined(X4_OPENORBIS) && X4_STREAM_PROFILE
            recovery43b_report(rtc,false);
#endif
#if X4_REMB_ENABLED
            remb44a_report(rtc,false);
#endif
#line 451
            rtc->recovery43_next_poll=now+10000000u;
        }
#endif
#if X4_REMB_ENABLED && X4_PUBLIC
        const uint64_t now = sceKernelGetProcessTime();
        remb44a_tick(rtc, now);
        remb44c_periodic(rtc, now);
#endif
#line 454
        sceKernelUsleep(NACK_POLL_USEC);
    }
    return NULL;
}

/* Structural diagnostics only. Never print input strings: the SDP contains
 * addresses, credentials, certificate fingerprints and private identifiers. */
typedef struct { const char *p; size_t n; } SdpSpan;
typedef struct {
    unsigned kind, rtp, pt_count, pt_overflow, pts[16];
    unsigned mid_type, mid, direction, setup, trickle, feedback;
    unsigned fb_nack, fb_pli, fb_fir, fb_remb, profile_found, profile;
    int port, h264_pt, opus_pt, sctp_port;
    SdpSpan mid_span;
} SdpShape;

static bool sdp_equal(SdpSpan s, const char *literal)
{
    size_t n = strlen(literal);
    return s.n == n && !memcmp(s.p, literal, n);
}
static bool sdp_prefix(SdpSpan s, const char *literal)
{
    size_t n = strlen(literal);
    return s.n >= n && !memcmp(s.p, literal, n);
}
static SdpSpan sdp_after(SdpSpan s, size_t n)
{
    return n <= s.n ? (SdpSpan){s.p + n, s.n - n} : (SdpSpan){s.p, 0};
}
static SdpSpan sdp_token(SdpSpan *rest)
{
    size_t first = 0, end;
    while (first < rest->n && (rest->p[first] == ' ' || rest->p[first] == '\t')) ++first;
    end = first;
    while (end < rest->n && rest->p[end] != ' ' && rest->p[end] != '\t') ++end;
    SdpSpan token = {rest->p + first, end - first};
    *rest = sdp_after(*rest, end);
    return token;
}
static bool sdp_number(SdpSpan token, unsigned maximum, unsigned *number)
{
    if (!token.n) return false;
    unsigned value = 0;
    for (size_t i = 0; i < token.n; ++i) {
        unsigned digit = (unsigned char)token.p[i] - (unsigned)'0';
        if (digit > 9 || digit > maximum || value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *number = value;
    return true;
}
static unsigned sdp_setup(SdpSpan value)
{
    return sdp_equal(value, "actpass") ? 1 : sdp_equal(value, "active") ? 2 :
        sdp_equal(value, "passive") ? 3 : sdp_equal(value, "holdconn") ? 4 : 5;
}
static bool sdp_trickle(SdpSpan rest)
{
    while (rest.n) if (sdp_equal(sdp_token(&rest), "trickle")) return true;
    return false;
}
static void sdp_profile(SdpShape *m, SdpSpan line)
{
    static const char key[] = "profile-level-id=";
    for (size_t i = 0; i + sizeof(key) - 1 + 6 <= line.n; ++i) {
        if (i && line.p[i - 1] != ';' && line.p[i - 1] != ' ' && line.p[i - 1] != '\t') continue;
        if (memcmp(line.p + i, key, sizeof(key) - 1)) continue;
        size_t start = i + sizeof(key) - 1;
        unsigned value = 0;
        bool valid = true;
        for (size_t j = 0; j < 6; ++j) {
            unsigned char c = (unsigned char)line.p[start + j];
            unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit > 15) { valid = false; break; }
            value = value * 16 + digit;
        }
        size_t end = start + 6;
        if (end < line.n && line.p[end] != ';' && line.p[end] != ' ' && line.p[end] != '\t') valid = false;
        if (valid) { m->profile_found = 1; m->profile = value; }
    }
}
static void sdp_shape_log(const char *sdp, size_t length)
{
    SdpShape media[8] = {0};
    SdpSpan bundle[8] = {{0}};
    unsigned count = 0, stored = 0, candidates = 0, bundle_count = 0;
    unsigned ssrc_lines = 0, bare_ssrc = 0;
    unsigned session_setup = 0, session_trickle = 0, bundle_unique = 1, bundle_matched = 0;
    SdpShape *m = NULL;
    for (size_t offset = 0; offset < length;) {
        size_t end = offset;
        while (end < length && sdp[end] != '\n') ++end;
        size_t line_end = end;
        if (line_end > offset && sdp[line_end - 1] == '\r') --line_end;
        SdpSpan line = {sdp + offset, line_end - offset};
        offset = end < length ? end + 1 : end;
        if (sdp_prefix(line, "m=")) {
            ++count;
            m = stored < 8 ? &media[stored++] : NULL;
            if (!m) continue;
            *m = (SdpShape){.port = -1, .h264_pt = -1, .opus_pt = -1, .sctp_port = -1,
                .setup = session_setup, .trickle = session_trickle};
            SdpSpan rest = sdp_after(line, 2), type = sdp_token(&rest);
            m->kind = sdp_equal(type, "video") ? 1 : sdp_equal(type, "audio") ? 2 :
                sdp_equal(type, "application") ? 3 : 4;
            unsigned number;
            if (sdp_number(sdp_token(&rest), 65535, &number)) m->port = (int)number;
            SdpSpan protocol = sdp_token(&rest);
            m->rtp = sdp_equal(protocol, "UDP/TLS/RTP/SAVPF") || sdp_equal(protocol, "RTP/SAVPF");
            if (m->rtp) while (rest.n) {
                SdpSpan pt = sdp_token(&rest);
                if (!pt.n) break;
                if (!sdp_number(pt, 127, &number)) { m->pt_overflow = 1; continue; }
                if (m->pt_count < 16) m->pts[m->pt_count++] = number;
                else m->pt_overflow = 1;
            }
            continue;
        }
        if (sdp_prefix(line, "a=candidate:")) { ++candidates; continue; }
        if (sdp_prefix(line, "a=ssrc:")) {
            ++ssrc_lines;
            SdpSpan rest = sdp_after(line, 7);
            (void)sdp_token(&rest); /* The source ID stays private. */
            if (!sdp_token(&rest).n) ++bare_ssrc;
            continue;
        }
        if (sdp_prefix(line, "a=group:BUNDLE")) {
            SdpSpan rest = sdp_after(line, 14);
            while (rest.n) {
                SdpSpan token = sdp_token(&rest);
                if (!token.n) break;
                for (unsigned i = 0; i < bundle_count && i < 8; ++i)
                    if (bundle[i].n == token.n && !memcmp(bundle[i].p, token.p, token.n)) bundle_unique = 0;
                if (bundle_count < 8) bundle[bundle_count] = token;
                ++bundle_count;
            }
            continue;
        }
        if (sdp_prefix(line, "a=setup:")) {
            unsigned setup = sdp_setup(sdp_after(line, 8));
            if (m) m->setup = setup; else if (!count) session_setup = setup;
        } else if (sdp_prefix(line, "a=ice-options:")) {
            unsigned trickle = sdp_trickle(sdp_after(line, 14));
            if (m) m->trickle = trickle; else if (!count) session_trickle = trickle;
        } else if (m) {
            unsigned number;
            if (sdp_prefix(line, "a=mid:")) {
                m->mid_span = sdp_after(line, 6);
                m->mid_type = sdp_number(m->mid_span, 65535, &m->mid) ? 1 : 2;
            } else if (sdp_equal(line, "a=recvonly")) m->direction = 1;
            else if (sdp_equal(line, "a=sendonly")) m->direction = 2;
            else if (sdp_equal(line, "a=sendrecv")) m->direction = 3;
            else if (sdp_equal(line, "a=inactive")) m->direction = 4;
            else if (sdp_prefix(line, "a=rtpmap:")) {
                SdpSpan rest = sdp_after(line, 9), pt = sdp_token(&rest), codec = sdp_token(&rest);
                if (sdp_number(pt, 127, &number)) {
                    if (sdp_equal(codec, "H264/90000")) m->h264_pt = (int)number;
                    if (sdp_equal(codec, "opus/48000/2")) m->opus_pt = (int)number;
                }
            } else if (sdp_prefix(line, "a=rtcp-fb:")) {
                ++m->feedback;
                SdpSpan rest = sdp_after(line, 10);
                (void)sdp_token(&rest);
                while (rest.n && (rest.p[0] == ' ' || rest.p[0] == '\t')) rest = sdp_after(rest, 1);
                m->fb_nack |= sdp_equal(rest, "nack"); m->fb_pli |= sdp_equal(rest, "nack pli");
                m->fb_fir |= sdp_equal(rest, "ccm fir"); m->fb_remb |= sdp_equal(rest, "goog-remb");
            } else if (sdp_prefix(line, "a=fmtp:")) sdp_profile(m, line);
            else if (sdp_prefix(line, "a=sctp-port:") && sdp_number(sdp_after(line, 12), 65535, &number))
                m->sctp_port = (int)number;
        }
    }
    if (bundle_count > 8) bundle_unique = 2; /* Unknown when the diagnostic bound is exceeded. */
    for (unsigned i = 0; i < bundle_count && i < 8; ++i)
        for (unsigned j = 0; j < stored; ++j)
            if (media[j].mid_span.n == bundle[i].n && !memcmp(media[j].mid_span.p, bundle[i].p, bundle[i].n)) {
                ++bundle_matched; break;
            }
    printf("XCloud4: SDP shape bytes=%zu media=%u stored=%u candidates=%u bundle=%u unique=%u matched=%u setup=%u trickle=%u ssrc=%u bare_ssrc=%u first_kind=%u second_kind=%u\n",
        length, count, stored, candidates, bundle_count, bundle_unique, bundle_matched, session_setup, session_trickle,
        ssrc_lines, bare_ssrc, stored ? media[0].kind : 0, stored > 1 ? media[1].kind : 0);
    for (unsigned i = 0; i < stored; ++i) {
        m = &media[i];
        printf("XCloud4: SDP media index=%u kind=%u port_present=%u midtype=%u direction=%u rtp=%u pts=%u overflow=%u setup=%u trickle=%u sctp_present=%u\n",
            i, m->kind, (unsigned)(m->port > 0), m->mid_type, m->direction, m->rtp, m->pt_count, m->pt_overflow,
            m->setup, m->trickle, (unsigned)(m->sctp_port > 0));
        for (unsigned j = 0; j < m->pt_count; ++j)
            printf("XCloud4: SDP payload media=%u index=%u pt=%u\n", i, j, m->pts[j]);
        printf("XCloud4: SDP codec media=%u h264=%d opus=%d feedback=%u nack=%u pli=%u fir=%u remb=%u profile_present=%u profile=%06X known31=%u known32=%u\n",
            i, m->h264_pt, m->opus_pt, m->feedback, m->fb_nack, m->fb_pli, m->fb_fir, m->fb_remb,
            m->profile_found, m->profile, m->profile_found && m->profile == 0x42e01f,
            m->profile_found && m->profile == 0x42e020);
    }
}

/* Rare, bounded numeric session metadata. The original SDP stays in memory;
 * credentials, candidates, fingerprints, names and opaque text are excluded. */
static void sdp_receive_shape(X4Rtc *rtc, const char *sdp, size_t length, unsigned scope)
{
    unsigned media = 0, records = 0;
    bool have_media = false;
    bool is_video = false;
    printf("XCloud4: SDP receive scope=%u (0=offer,1=accepted answer)\n", scope);
    for (size_t offset = 0; offset < length;) {
        size_t end = offset;
        while (end < length && sdp[end] != '\n') ++end;
        size_t line_end = end;
        if (line_end > offset && sdp[line_end-1] == '\r') --line_end;
        SdpSpan line = {sdp + offset, line_end - offset};
        offset = end < length ? end + 1 : end;
        if (sdp_prefix(line, "m=")) {
            if (have_media) ++media;
            have_media = true;
            is_video = sdp_prefix(line, "m=video");
            continue;
        }
        if (!have_media || media >= 8 || records >= 128) continue;
        if (is_video && rtc && scope == 1) {
            if (sdp_prefix(line, "a=mid:")) {
                SdpSpan mid_span = sdp_after(line, 6);
                unsigned number;
                /* This copy feeds the klog and metrics, never negotiation. */
                const char *label = !mid_span.n ? "" : sdp_equal(mid_span, "video") ? "video" :
                    sdp_number(mid_span, 999999u, &number) ? "numerico" : "oculto";
                snprintf(rtc->sdp_video_mid, sizeof(rtc->sdp_video_mid), "%s", label);
            } else if (sdp_prefix(line, "a=rtpmap:")) {
                SdpSpan rest = sdp_after(line, 9), type = sdp_token(&rest), codec = sdp_token(&rest);
                unsigned pt = 0;
                if (sdp_number(type, 127, &pt) && sdp_equal(codec, "H264/90000")) {
                    rtc->sdp_video_pt = pt;
                }
            } else if (sdp_prefix(line, "a=ssrc:")) {
                SdpSpan rest = sdp_after(line, 7);
                uint64_t val = 0;
                size_t i = 0;
                while (i < rest.n && rest.p[i] >= '0' && rest.p[i] <= '9') {
                    val = val * 10 + (rest.p[i] - '0');
                    ++i;
                }
                if (val && !rtc->sdp_video_ssrc) {
                    rtc->sdp_video_ssrc = (uint32_t)val;
                    x4_rtc_receive_diag_set_expected_video(rtc->sdp_video_ssrc, rtc->sdp_video_pt ? rtc->sdp_video_pt : 102, rtc->sdp_video_mid);
                }
            }
        }
        uint64_t key = ((uint64_t)scope << 32) | ((uint64_t)media << 16);
        unsigned pt, number;
        if (sdp_prefix(line, "a=rtpmap:")) {
            SdpSpan rest = sdp_after(line, 9), type = sdp_token(&rest), codec = sdp_token(&rest);
            if (!sdp_number(type, 127, &pt)) continue;
            unsigned kind = sdp_equal(codec, "H264/90000") ? 1 :
                sdp_equal(codec, "opus/48000/2") ? 2 :
                (sdp_equal(codec, "rtx/90000") || sdp_equal(codec, "RTX/90000")) ? 3 : 0;
            x4_rtc_receive_setting(10, key | pt, kind, 0);
            printf("XCloud4: SDP receive payload scope=%u media=%u pt=%u codec=%u\n",scope,media,pt,kind);
            ++records;
        } else if (sdp_prefix(line, "a=rtcp-fb:")) {
            SdpSpan rest = sdp_after(line, 10), target = sdp_token(&rest);
            if (sdp_equal(target, "*")) pt = 128;
            else if (!sdp_number(target, 127, &pt)) continue;
            while (rest.n && (rest.p[0]==' ' || rest.p[0]=='\t')) rest = sdp_after(rest,1);
            unsigned mask = sdp_equal(rest,"nack") ? 1 : sdp_equal(rest,"nack pli") ? 2 :
                sdp_equal(rest,"ccm fir") ? 4 : sdp_equal(rest,"goog-remb") ? 8 : 0;
#if X4_REMB_ENABLED
            if (rtc && scope == 1 && !rtc->nack_thread_started && is_video && mask == 8 &&
                (pt == 128 || pt == X4_RECEIVER_NACK_VIDEO_PT)) rtc->remb.negotiated = true;
#endif
#line 718
            x4_rtc_receive_setting(13, key | pt, mask, 0);
            printf("XCloud4: SDP receive feedback scope=%u media=%u pt=%u mask=%u\n",scope,media,pt,mask);
            ++records;
        } else if (sdp_prefix(line, "a=fmtp:")) {
            SdpSpan rest = sdp_after(line,7), target = sdp_token(&rest);
            if (!sdp_number(target,127,&pt)) continue;
            while (rest.n && records < 128) {
                while (rest.n && (rest.p[0]==' ' || rest.p[0]=='\t' || rest.p[0]==';')) rest=sdp_after(rest,1);
                size_t n=0; while (n<rest.n && rest.p[n]!=';') ++n;
                SdpSpan parameter={rest.p,n};
                rest=sdp_after(rest,n);
#if X4_STREAM_PROFILE
                if(sdp_prefix(parameter,"profile-level-id=")){
                    SdpShape shape={0};sdp_profile(&shape,parameter);
                    if(shape.profile_found){
                        x4_rtc_receive_setting(14,key|pt,shape.profile,0);
                        printf("XCloud4: SDP receive H264 profile scope=%u media=%u pt=%u profile_level=%06X\n",scope,media,pt,shape.profile);
                        ++records;
                    }
                }
#endif
                if (sdp_prefix(parameter,"apt=") && sdp_number(sdp_after(parameter,4),127,&number)) {
                    x4_rtc_receive_setting(11,key|pt,number,0);
                    printf("XCloud4: SDP receive apt scope=%u media=%u rtx_pt=%u primary_pt=%u\n",scope,media,pt,number);
                    ++records;
                }
            }
        } else if (sdp_prefix(line,"a=ssrc-group:FID ")) {
            SdpSpan rest=sdp_after(line,17);
            unsigned primary, retransmission;
            if (sdp_number(sdp_token(&rest),UINT32_MAX,&primary) &&
                sdp_number(sdp_token(&rest),UINT32_MAX,&retransmission)) {
                x4_rtc_receive_setting(12,key,primary,retransmission);
                /* SSRCs stay in the private numeric trace, not text logs. */
                printf("XCloud4: SDP receive FID scope=%u media=%u present=1\n",scope,media);
                ++records;
            }
        }
    }
    x4_rtc_receive_setting(19,scope,records,records>=128);
}

static void description_callback(int pc, const char *sdp, const char *type, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    size_t length = sdp ? strnlen(sdp, SDP_CAP) : SDP_CAP;
    if (length == SDP_CAP || !type || strcmp(type, "offer")) fail(rtc, -40);
    else {
        lock(&rtc->data_gate);
        memcpy(rtc->sdp, sdp, length + 1);
        unlock(&rtc->data_gate);
    }
    leave(pointer);
}
static void candidate_callback(int pc, const char *candidate, const char *mid, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    size_t length = candidate ? strnlen(candidate, CANDIDATE_CAP) : CANDIDATE_CAP;
    size_t mid_length = mid ? strnlen(mid, MID_CAP) : MID_CAP;
    lock(&rtc->data_gate);
    if (length == CANDIDATE_CAP || mid_length == MID_CAP ||
        rtc->candidate_write - rtc->candidate_read == CANDIDATE_COUNT) fail(rtc, -41);
    else {
        Candidate *item = &rtc->candidates[rtc->candidate_write++ % CANDIDATE_COUNT];
        memcpy(item->candidate, candidate, length + 1);
        memcpy(item->mid, mid, mid_length + 1);
    }
    unlock(&rtc->data_gate);
    leave(pointer);
}
static void state_callback(int pc, rtcState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    if (!atomic_load(&rtc->error)) atomic_store(&rtc->state, state);
    printf("XCloud4: RTC estado=%d\n", (int)state);
#if !X4_PUBLIC
    if (state == RTC_CLOSED || state == RTC_FAILED)
        lab_observe_close(rtc, LAB_CLOSE_PEER, (int)state);
#endif
    leave(pointer);
}
static void ice_state_callback(int pc, rtcIceState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    printf("XCloud4: RTC ICE estado=%d\n", (int)state);
    leave(pointer);
}
static void gathering_callback(int pc, rtcGatheringState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    printf("XCloud4: RTC gathering state=%d\n", (int)state);
    if (state == RTC_GATHERING_COMPLETE) atomic_store(&rtc->gathering_done, true);
    leave(pointer);
}

static void rtp_callback(int track, const char *packet, int size, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    /* RtcpReceivingSession consumes control packets and leaves RTP intact. */
    if (size >= 12 && packet && (((const uint8_t *)packet)[0] >> 6) == 2) {
        x4_rtc_receive_packet(X4_RD_APP, packet, (size_t)size,
            x4_rtc_receive_delivery_time(), X4_RD_AUTHENTICATED);
        int kind = track == rtc->video ? X4_RTC_VIDEO : X4_RTC_AUDIO;
        if (kind == X4_RTC_VIDEO) {
#if !X4_PUBLIC
            if (atomic_fetch_add(&rtc->video_packets, 1) == 0) lab_mark_first_video();
#else
            atomic_fetch_add(&rtc->video_packets, 1);
#endif
        } else atomic_fetch_add(&rtc->audio_packets, 1);
        lock(&rtc->data_gate);
        X4RtcMediaCallback callback = rtc->media;
        void *user = rtc->media_user;
        unlock(&rtc->data_gate);
        if (callback) callback(user, kind, (const uint8_t *)packet, (size_t)size);
        else x4_rtc_receive_reject(X4_RD_APP, X4_RD_APP_NO_SINK, 0, packet, (size_t)size);
    } else {
        x4_rtc_receive_reject(X4_RD_APP, X4_RD_APP_INVALID, size, packet,
            size > 0 ? (size_t)size : 0);
    }
    leave(pointer);
}

static void uuid(char output[37])
{
    unsigned char bytes[16];
    x4_native_random_or_exit(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    snprintf(output, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
             bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
}
static int send_text(int channel, const char *text) { return rtcSendMessage(channel, text, -1); }

static void input_u16(uint8_t *p, uint16_t value)
{ p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void input_u32(uint8_t *p, uint32_t value)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i)); }

static void input_header(X4Rtc *rtc, uint8_t *packet, uint16_t type)
{
    lock(&rtc->data_gate);
    uint32_t sequence = ++rtc->input_sequence; /* Defined uint32 wrap. */
    unlock(&rtc->data_gate);
    uint64_t t=sceKernelGetProcessTime();
    double milliseconds=t>=rtc->started_at ? (double)(t-rtc->started_at)/1000.0 : 0.0;
    uint64_t bits;
    memcpy(&bits,&milliseconds,sizeof(bits));
    input_u16(packet,type);input_u32(packet+2,sequence);
    for(unsigned i=0;i<8;++i)packet[6+i]=(uint8_t)(bits>>(8*i));
}

typedef struct {
    bool connected;
    uint16_t buttons;
    int16_t axes[4];
    uint16_t triggers[2];
} InputMappedState;
static InputMappedState input_mapped_state(const X4GamepadFrame *frame)
{
    InputMappedState state={.connected=frame->connected,.buttons=frame->buttons&0xfffeu,
        .axes={frame->left_x,frame->left_y,frame->right_x,frame->right_y},
        .triggers={frame->left_trigger,frame->right_trigger}};
    for(unsigned i=0;i<4;++i)if(state.axes[i]<-32767)state.axes[i]=-32767;
    return state;
}
static bool input_same_state(const InputMappedState *a,const InputMappedState *b)
{
    if(a->connected!=b->connected || a->buttons!=b->buttons)return false;
    for(unsigned i=0;i<4;++i)if(a->axes[i]!=b->axes[i])return false;
    return a->triggers[0]==b->triggers[0] && a->triggers[1]==b->triggers[1];
}
/* input 1.0, with one 23-byte block per announced controller. The mixed
 * physicality endianness is part of the wire format. */
enum { INPUT_GAMEPAD_PREFIX = 15, INPUT_GAMEPAD_BLOCK = 23,
       INPUT_GAMEPAD_MAX = INPUT_GAMEPAD_PREFIX + X4_GAMEPAD_COUNT * INPUT_GAMEPAD_BLOCK };
static void input_gamepad_block(uint8_t *block, const InputMappedState *state, uint8_t index)
{
    block[0]=index;
    input_u16(block+1,state->buttons);
    for(unsigned i=0;i<4;++i)input_u16(block+3+2*i,(uint16_t)state->axes[i]);
    input_u16(block+11,state->triggers[0]);input_u16(block+13,state->triggers[1]);
    input_u32(block+15,1);
    block[19]=block[20]=block[21]=0;block[22]=1; /* uint32 big endian. */
}
static size_t input_gamepads(X4Rtc *rtc, const InputMappedState pads[X4_GAMEPAD_COUNT],
    unsigned mask, uint8_t packet[INPUT_GAMEPAD_MAX])
{
    input_header(rtc,packet,2);
    /* P1 always occupies the first block. Reuse the mapped values used by
     * change detection, so the single-pad path neither scans nor maps twice. */
    input_gamepad_block(packet+INPUT_GAMEPAD_PREFIX,&pads[0],0);
    size_t end=INPUT_GAMEPAD_PREFIX+INPUT_GAMEPAD_BLOCK;
    unsigned count=1;
    for(unsigned pending=mask&~1u;pending;pending&=pending-1u) {
        unsigned i=(unsigned)__builtin_ctz(pending);
        input_gamepad_block(packet+end,&pads[i],(uint8_t)i);
        end+=INPUT_GAMEPAD_BLOCK;
        ++count;
    }
    packet[14]=(uint8_t)count;
    return end;
}

static void input_error(X4Rtc *rtc,int error)
{
    int previous=atomic_exchange(&rtc->input_error,error);
    if(error && error!=previous)printf("XCloud4: RTC input error=0x%08x\n",(unsigned)error);
}

/* Input-only observation storage: one sender owns it until its join. The
 * session owner resets before starting RTC and exports after joining it. The
 * nonblocking claim makes an accidental live reset/export fail closed, rather
 * than reading mutable histogram storage. It never delays the input loop. */
#define INPUT_POLL_USEC 2000u
#define INPUT_MINIMUM_USEC 4000u
#define INPUT_METRIC_BINS 256u
#define INPUT_AGE_QUANTUM_USEC 1000u
#define INPUT_SEND_QUANTUM_USEC 100u
typedef struct {
    uint64_t count, total_us, maximum_us, bins[INPUT_METRIC_BINS];
} InputTiming;
typedef struct {
    bool available, legacy;
    int priority_return;
    uint64_t first_ready_us, ended_us, successful, changes, keepalives;
    uint64_t cap_delays, busy_deferrals, buffer_query_errors, send_errors, age_unavailable, clock_errors;
    uint64_t second_origin_us, second_index, second_packets, maximum_per_second;
    InputTiming state_age, send_duration;
    bool rumble_available;
    X4RumbleSnapshot rumble;
} InputMetrics;
static InputMetrics input_metrics;
static atomic_flag input_metrics_claim = ATOMIC_FLAG_INIT;

static void input_timing_record(InputTiming *timing,uint64_t us,unsigned quantum)
{
    ++timing->count;timing->total_us+=us;
    if(us>timing->maximum_us)timing->maximum_us=us;
    uint64_t bin=us/quantum;
    ++timing->bins[bin<INPUT_METRIC_BINS-1u?bin:INPUT_METRIC_BINS-1u];
}
static bool input_metrics_begin_mode(bool legacy,int priority_return,bool preserve)
{
    if(atomic_flag_test_and_set_explicit(&input_metrics_claim,memory_order_acquire))return false;
    if(!preserve||!input_metrics.available)memset(&input_metrics,0,sizeof(input_metrics));
    input_metrics.available=true;input_metrics.legacy=legacy;
    input_metrics.priority_return=priority_return;input_metrics.ended_us=0;
    return true;
}
static bool input_metrics_begin(bool legacy,int priority_return)
{return input_metrics_begin_mode(legacy,priority_return,false);}
static void input_metrics_ready(bool own,uint64_t now)
{
    if(own && !input_metrics.first_ready_us) {
        input_metrics.first_ready_us=now;input_metrics.second_origin_us=now;
    }
}
static void input_metrics_send(bool own,const X4GamepadFrame *frame,bool changed,
    uint64_t begin,uint64_t end,int rc)
{
    if(!own)return;
    if(end>=begin)input_timing_record(&input_metrics.send_duration,end-begin,INPUT_SEND_QUANTUM_USEC);
    else ++input_metrics.clock_errors;
    if(rc<0){++input_metrics.send_errors;return;}
    ++input_metrics.successful;
    if(changed)++input_metrics.changes;else ++input_metrics.keepalives;
    /* This is the physical sample timestamp carried by the source, never the
     * mailbox poll time. Expired/disconnected/unknown states have no age. */
    if(frame->connected && frame->sampled_us && frame->sampled_us<=begin)
        input_timing_record(&input_metrics.state_age,begin-frame->sampled_us,INPUT_AGE_QUANTUM_USEC);
    else ++input_metrics.age_unavailable;
    if(begin>=input_metrics.second_origin_us) {
        uint64_t second=(begin-input_metrics.second_origin_us)/1000000u;
        if(second!=input_metrics.second_index) {
            if(input_metrics.second_packets>input_metrics.maximum_per_second)
                input_metrics.maximum_per_second=input_metrics.second_packets;
            input_metrics.second_index=second;input_metrics.second_packets=0;
        }
        ++input_metrics.second_packets;
    } else ++input_metrics.clock_errors;
}
static void input_metrics_end(bool own)
{
    if(!own)return;
    input_metrics.ended_us=sceKernelGetProcessTime();
    if(input_metrics.second_packets>input_metrics.maximum_per_second)
        input_metrics.maximum_per_second=input_metrics.second_packets;
    atomic_flag_clear_explicit(&input_metrics_claim,memory_order_release);
}
void x4_rtc_input_metrics_reset(void)
{
    if(atomic_flag_test_and_set_explicit(&input_metrics_claim,memory_order_acquire))return;
    memset(&input_metrics,0,sizeof(input_metrics));
    atomic_flag_clear_explicit(&input_metrics_claim,memory_order_release);
}
static void input_timing_write(FILE *f,const InputTiming *timing,unsigned quantum)
{
    uint64_t targets[2]={(timing->count*50u+99u)/100u,(timing->count*95u+99u)/100u};
    unsigned percentiles[2]={INPUT_METRIC_BINS,INPUT_METRIC_BINS};
    for(unsigned q=0;q<2;++q)if(timing->count) {
        uint64_t cumulative=0;
        for(unsigned i=0;i<INPUT_METRIC_BINS;++i) {
            cumulative+=timing->bins[i];if(cumulative>=targets[q]){percentiles[q]=i;break;}
        }
    }
    fprintf(f,"{\"n\":%llu,\"quantum_us\":%u,\"mean_us\":",
        (unsigned long long)timing->count,quantum);
    if(timing->count)fprintf(f,"%.3f",(double)timing->total_us/(double)timing->count);else fputs("null",f);
    for(unsigned q=0;q<2;++q) {
        fprintf(f,",\"p%u_upper_us\":",q?95u:50u);
        if(percentiles[q]<INPUT_METRIC_BINS-1u)fprintf(f,"%u",(percentiles[q]+1u)*quantum);
        else fputs("null",f);
    }
    fprintf(f,",\"maximum_us\":%llu,\"overflow\":%llu}",
        (unsigned long long)timing->maximum_us,(unsigned long long)timing->bins[INPUT_METRIC_BINS-1u]);
}
void x4_rtc_input_metrics_write(FILE *f)
{
    if(!f)return;
    if(atomic_flag_test_and_set_explicit(&input_metrics_claim,memory_order_acquire)) {
        fputs("{\"available\":false,\"reason\":\"sender_not_joined\"}",f);return;
    }
    if(!input_metrics.available)fputs("{\"available\":false,\"reason\":\"sender_not_started\"}",f);
    else {
        uint64_t duration=input_metrics.ended_us>=input_metrics.first_ready_us && input_metrics.first_ready_us?
            input_metrics.ended_us-input_metrics.first_ready_us:0;
        fprintf(f,"{\"available\":true,\"mode\":\"%s\",\"priority_requested\":400,\"priority_return\":%d,"
            "\"packets\":%llu,\"changes\":%llu,\"keepalives\":%llu,\"cap_delay_polls\":%llu,"
            "\"busy_buffer_polls\":%llu,\"buffer_query_errors\":%llu,\"send_errors\":%llu,\"unknown_state_age\":%llu,"
            "\"clock_errors\":%llu,\"ready_to_join_duration_us\":%llu,\"mean_packets_per_second\":",
            input_metrics.legacy?"legacy_60hz":"immediate",input_metrics.priority_return,
            (unsigned long long)input_metrics.successful,(unsigned long long)input_metrics.changes,
            (unsigned long long)input_metrics.keepalives,(unsigned long long)input_metrics.cap_delays,
            (unsigned long long)input_metrics.busy_deferrals,(unsigned long long)input_metrics.buffer_query_errors,
            (unsigned long long)input_metrics.send_errors,
            (unsigned long long)input_metrics.age_unavailable,(unsigned long long)input_metrics.clock_errors,
            (unsigned long long)duration);
        if(duration)fprintf(f,"%.6f",(double)input_metrics.successful*1000000.0/(double)duration);else fputs("null",f);
        fprintf(f,",\"maximum_packets_per_second\":%llu,\"rate_scope\":\"successful sends; one-second bins anchored at first ready poll; final partial bin included in maximum; mean first-ready to join\",\"state_age\":",
            (unsigned long long)input_metrics.maximum_per_second);
        input_timing_write(f,&input_metrics.state_age,INPUT_AGE_QUANTUM_USEC);
        fputs(",\"send_duration\":",f);input_timing_write(f,&input_metrics.send_duration,INPUT_SEND_QUANTUM_USEC);
        fputs(",\"state_age_scope\":\"physical sampled_us to successful local send start; disconnected/zero/future timestamps excluded as unknown\","
            "\"send_duration_scope\":\"rtcSendMessage call duration; successful and failed attempts\"",f);
        fprintf(f,",\"send_success_scope\":\"local rtcSendMessage return only; no Xbox acknowledgement\",\"rumble_available\":%s",
            input_metrics.rumble_available?"true":"false");
        if(input_metrics.rumble_available) {
            const X4RumbleSnapshot *r=&input_metrics.rumble;
            fprintf(f,",\"rumble\":{\"binary_messages\":%llu,\"reports\":%llu,\"valid\":%llu,\"accepted\":%llu,"
                "\"malformed\":%llu,\"unsupported\":%llu,\"contention_dropped\":%llu,\"inactive_dropped\":%llu,"
                "\"superseded\":%llu,\"scope\":\"mailbox snapshot at input sender stop; later close callbacks excluded\"}",
                (unsigned long long)r->binary_messages,(unsigned long long)r->reports,
                (unsigned long long)r->valid,(unsigned long long)r->accepted,
                (unsigned long long)r->malformed,(unsigned long long)r->unsupported,
                (unsigned long long)r->contention_dropped,(unsigned long long)r->inactive_dropped,
                (unsigned long long)r->superseded);
        }
        fputs("}",f);
    }
    atomic_flag_clear_explicit(&input_metrics_claim,memory_order_release);
}
typedef struct {
    unsigned announced;
    unsigned absence_active;
    uint64_t absence_since[X4_GAMEPAD_COUNT], absence_generation[X4_GAMEPAD_COUNT];
#if X4_RUMBLE
    bool rumble_targets_set;
    unsigned rumble_targets;
    uint64_t rumble_generation[X4_GAMEPAD_COUNT];
#endif
#if !X4_PUBLIC
    bool saw_presence;
    unsigned presence;
    uint64_t reports[X4_GAMEPAD_COUNT], neutral[X4_GAMEPAD_COUNT];
    uint64_t adds[X4_GAMEPAD_COUNT], removes[X4_GAMEPAD_COUNT], control_errors[X4_GAMEPAD_COUNT];
#endif
} InputGamepadTracker;

#define INPUT_REMOVE_DEBOUNCE_USEC 2000000u

static inline bool input_topology_pending(const X4Rtc *rtc,const InputGamepadTracker *tracker,
    const X4GamepadRoster *roster,unsigned present)
{
#if !X4_PUBLIC
    if(!tracker->saw_presence || tracker->presence!=present)return true;
#endif
    if(!roster->multiplayer)return false;
    if((present|tracker->announced)&~1u)return true;
#if X4_RUMBLE
    return rtc->rumble && (!tracker->rumble_targets_set || tracker->rumble_targets!=present ||
        tracker->rumble_generation[0]!=roster->generation[0]);
#else
    (void)rtc;
    return false;
#endif
}

static void input_reconcile_gamepads(X4Rtc *rtc, InputGamepadTracker *tracker,
    const X4GamepadRoster *roster, unsigned present, uint64_t now)
{
#if !X4_PUBLIC
    unsigned observed=tracker->saw_presence?tracker->presence^present:present|1u;
    for(;observed;observed&=observed-1u) {
        unsigned i=(unsigned)__builtin_ctz(observed);
        printf("XCloud4: RTC mandos presencia indice=%u conectado=%u generacion=%llu\n",
            i,(present&(1u<<i))?1u:0u,(unsigned long long)roster->generation[i]);
    }
    tracker->saw_presence=true;tracker->presence=present;
#endif
    if(!roster->multiplayer)return;
    /* P1 is permanently announced in start_channels. For any other slot a
     * fresh generation restarts the absence window, even if the intervening
     * reconnect was too short for the sender to observe directly. */
    for(unsigned pending=(present|tracker->announced)&~1u;pending;pending&=pending-1u) {
        unsigned i=(unsigned)__builtin_ctz(pending),bit=1u<<i;
        bool added=(present&bit)!=0, announced=(tracker->announced&bit)!=0;
        if(added || !announced)tracker->absence_active&=~bit;
        if(added==announced)continue;
        if(!added) {
            bool reversed=(tracker->absence_active&bit) && now<tracker->absence_since[i];
            if(!(tracker->absence_active&bit) || reversed ||
               tracker->absence_generation[i]!=roster->generation[i]) {
                tracker->absence_active|=bit;
                tracker->absence_generation[i]=roster->generation[i];
                uint64_t changed=roster->changed_us[i];
                tracker->absence_since[i]=!reversed && changed && changed<=now?changed:now;
            }
            if(now-tracker->absence_since[i]<INPUT_REMOVE_DEBOUNCE_USEC)continue;
        }
        if(!rtcIsOpen(rtc->channels[1]))continue;
        char message[70];
        int length=snprintf(message,sizeof(message),
            "{\"message\":\"gamepadChanged\",\"gamepadIndex\":%u,\"wasAdded\":%s}",
            i,added?"true":"false");
        if(length<=0 || (size_t)length>=sizeof(message))continue;
        int rc=send_text(rtc->channels[1],message);
        if(rc<0) {
#if !X4_PUBLIC
            ++tracker->control_errors[i];
#endif
            continue;
        }
        if(added)tracker->announced|=bit;
        else {tracker->announced&=~bit;tracker->absence_active&=~bit;}
#if !X4_PUBLIC
        if(added)++tracker->adds[i];else ++tracker->removes[i];
        printf("XCloud4: RTC mandos aviso indice=%u alta=%u rc=%d\n",i,added?1u:0u,rc);
#endif
    }
#if X4_RUMBLE
    /* A new slot is actionable only after its local registration succeeds.
     * A physically missing slot is ineligible even during the wire grace. */
    unsigned targets=present&tracker->announced;
    if(rtc->rumble && (!tracker->rumble_targets_set || tracker->rumble_targets!=targets ||
        memcmp(tracker->rumble_generation,roster->generation,sizeof(tracker->rumble_generation)))) {
        if(x4_rumble_set_targets(rtc->rumble,(uint8_t)targets,roster->generation)) {
            tracker->rumble_targets_set=true;tracker->rumble_targets=targets;
            memcpy(tracker->rumble_generation,roster->generation,sizeof(tracker->rumble_generation));
        }
    }
#endif
}

static void *input_sender(void *pointer)
{
    X4Rtc *rtc=pointer;
    int priority_return=scePthreadSetprio(scePthreadSelf(),400);
    bool legacy=true;
#if X4_STREAM_PROFILE
    legacy=(rtc->request_config.interaction_modes&X4_INTERACTION_INPUT_60HZ)!=0;
#endif
    bool own_metrics=rtc->reconnect_peer?input_metrics_begin_mode(legacy,priority_return,true):
        input_metrics_begin(legacy,priority_return);
    printf("XCloud4: RTC input mode=%s priority=400 return=0x%08x\n",
        legacy?"legacy_60hz":"immediate",(unsigned)priority_return);
    InputGamepadTracker tracker={.announced=1u};
    InputMappedState last_success[X4_GAMEPAD_COUNT]={0};
    unsigned last_mask=0;
    bool have_attempt=false;
    uint64_t last_attempt_us=0,last_success_us=0;
    while(!atomic_load(&rtc->input_stop)) {
        uint64_t until=sceKernelGetProcessTime()+(legacy?INPUT_INTERVAL_USEC:INPUT_POLL_USEC);
        lock(&rtc->data_gate);
        X4GamepadSource source=rtc->input_source;void *user=rtc->input_user;
        unlock(&rtc->data_gate);
        bool ready=source && atomic_load(&rtc->input_metadata_sent) &&
            atomic_load(&rtc->startup_complete) && !atomic_load(&rtc->error) &&
            atomic_load(&rtc->state)==X4_RTC_CONNECTED && rtcIsOpen(rtc->channels[2]);
        atomic_store(&rtc->input_ready,ready);
        if(ready) {
            X4GamepadRoster roster;
            bool fresh=source(user,&roster);
            uint64_t now=sceKernelGetProcessTime();
            input_metrics_ready(own_metrics,now);
            unsigned present=fresh ? (roster.connected_mask & (roster.multiplayer?15u:1u)) : 0u;
            if(fresh && input_topology_pending(rtc,&tracker,&roster,present) &&
                !atomic_load(&rtc->input_stop))
                input_reconcile_gamepads(rtc,&tracker,&roster,present,now);
            unsigned mask=tracker.announced,extra=mask&~1u;
            static const X4GamepadFrame neutral={0};
            const X4GamepadFrame *primary=(present&1u) && roster.pads[0].connected?&roster.pads[0]:&neutral;
            InputMappedState current[X4_GAMEPAD_COUNT];
            current[0]=input_mapped_state(primary);
            bool changed=last_mask!=mask || !input_same_state(&current[0],&last_success[0]);
            for(unsigned pending=extra;pending;pending&=pending-1u) {
                unsigned i=(unsigned)__builtin_ctz(pending);
                const X4GamepadFrame *frame=(present&(1u<<i)) && roster.pads[i].connected?&roster.pads[i]:&neutral;
                current[i]=input_mapped_state(frame);
                if(!changed && !input_same_state(&current[i],&last_success[i]))changed=true;
            }
            bool keepalive=last_mask && now>=last_success_us && now-last_success_us>=INPUT_INTERVAL_USEC;
            bool capped=!legacy && have_attempt && now>=last_attempt_us &&
                now-last_attempt_us<INPUT_MINIMUM_USEC;
            if((legacy || changed || keepalive) && !atomic_load(&rtc->input_stop)) {
                if(capped){if(own_metrics)++input_metrics.cap_delays;}
                else {
                    int buffered=rtcGetBufferedAmount(rtc->channels[2]);
                    if(buffered) {
                        atomic_fetch_add(&rtc->input_dropped,1);
                        if(own_metrics) {
                            if(buffered>0)++input_metrics.busy_deferrals;
                            else ++input_metrics.buffer_query_errors;
                        }
                        if(buffered<0)input_error(rtc,buffered);
                    } else {
                        uint8_t packet[INPUT_GAMEPAD_MAX];
                        size_t size=input_gamepads(rtc,current,mask,packet);
                        uint64_t begin=sceKernelGetProcessTime();last_attempt_us=begin;have_attempt=true;
                        int rc=rtcSendMessage(rtc->channels[2],(const char *)packet,(int)size);
                        uint64_t end=sceKernelGetProcessTime();
                        input_metrics_send(own_metrics,primary,changed,begin,end,rc);
                        if(rc<0){atomic_fetch_add(&rtc->input_dropped,1);input_error(rtc,rc);}
                        else {
                            atomic_fetch_add(&rtc->input_packets,1);input_error(rtc,0);
                            last_success[0]=current[0];
#if !X4_PUBLIC
                            ++tracker.reports[0];
                            if(!primary->connected)++tracker.neutral[0];
#endif
                            for(unsigned pending=extra;pending;pending&=pending-1u) {
                                unsigned i=(unsigned)__builtin_ctz(pending);
                                last_success[i]=current[i];
#if !X4_PUBLIC
                                ++tracker.reports[i];
                                if(!current[i].connected)++tracker.neutral[i];
#endif
                            }
                            last_mask=mask;last_success_us=end;
                        }
                    }
                }
            }
        }
        /* Fresh poll deadline, no catch-up queue. The latest sample wins. */
        while(!atomic_load(&rtc->input_stop)) {
            uint64_t now=sceKernelGetProcessTime();if(now>=until)break;
            sceKernelUsleep((unsigned)(until-now));
        }
    }
    atomic_store(&rtc->input_ready,false);
#if X4_RUMBLE
    if(own_metrics && rtc->rumble) {
        x4_rumble_snapshot(rtc->rumble,&input_metrics.rumble);
        input_metrics.rumble_available=true;
    }
#endif
#if !X4_PUBLIC
    X4RumbleSnapshot vibration={0};
#if X4_RUMBLE
    if(rtc->rumble) {
        if(own_metrics)vibration=input_metrics.rumble;
        else x4_rumble_snapshot(rtc->rumble,&vibration);
    }
#endif
    tracker.adds[0]=atomic_load(&rtc->startup_complete)?1u:0u;
    for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i) {
        printf("XCloud4: RTC mandos resumen indice=%u informes=%llu neutros=%llu altas=%llu bajas=%llu vibracion_recibida=%llu fallos_aviso=%llu\n",
            i,(unsigned long long)tracker.reports[i],(unsigned long long)tracker.neutral[i],
            (unsigned long long)tracker.adds[i],(unsigned long long)tracker.removes[i],
            (unsigned long long)vibration.received_by_index[i],(unsigned long long)tracker.control_errors[i]);
    }
#endif
    input_metrics_end(own_metrics);
    return NULL;
}

static int startup_message(int channel, const char *target, const char *content)
{
    char escaped[1536], message[2048], id[37];
    size_t used = 0;
    for (const char *p = content; *p; ++p) {
        if (used + 3 >= sizeof(escaped)) return -1;
        if (*p == '"' || *p == '\\') escaped[used++] = '\\';
        escaped[used++] = *p;
    }
    escaped[used] = 0;
    uuid(id);
    int count = snprintf(message, sizeof(message), "{\"type\":\"Message\",\"content\":\"%s\",\"id\":\"%s\",\"target\":\"%s\",\"cv\":\"\"}", escaped, id, target);
    return count > 0 && (unsigned)count < sizeof(message) ? send_text(channel, message) : -1;
}

static void start_channels(X4Rtc *rtc)
{
    lock(&rtc->data_gate);
    bool ready = rtc->handshake_ack && !rtc->startup_sent &&
                 rtcIsOpen(rtc->channels[1]) && rtcIsOpen(rtc->channels[3]);
    if (ready) rtc->startup_sent = true;
    unlock(&rtc->data_gate);
    if (!ready) return;
    int control = rtc->channels[1], message = rtc->channels[3];
    /* Provider protocol constants, matching pinned GreenVita/Greenlight. */
    int rc = send_text(control, "{\"message\":\"authorizationRequest\",\"accessKey\":\"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E\"}");
    rc |= send_text(control, "{\"message\":\"gamepadChanged\",\"gamepadIndex\":0,\"wasAdded\":true}");
    rc |= startup_message(message, "/streaming/systemUi/configuration", "{\"version\":[0,2,0],\"systemUis\":[]}");
    rc |= startup_message(message, "/streaming/properties/clientappinstallidchanged", "{\"clientAppInstallId\":\"f9ac8684-1032-4131-bb47-d2f58da9bb93\"}");
    rc |= startup_message(message, "/streaming/characteristics/orientationchanged", "{\"orientation\":0}");
    rc |= startup_message(message, "/streaming/characteristics/touchinputenabledchanged", "{\"touchInputEnabled\":false}");
#if X4_STREAM_PROFILE
    const X4StreamRequestConfig *c=&rtc->request_config;
    char content[512];
    printf("XCloud4: RTC video request width=%u height=%u max_fps=%u bitrate_kbps=%u profile=%s os=%s display_info=%u (requested, not delivered)\n",
        c->width,c->height,c->fps,c->bitrate_kbps,c->label,c->os_name,(unsigned)c->display_info);
    if (!x4_stream_request_capabilities(c,content,sizeof(content))) rc=-1;
    else rc|=startup_message(message,"/streaming/characteristics/clientdevicecapabilities",content);
    if (!x4_stream_request_dimensions(c,content,sizeof(content))) rc=-1;
    else rc|=startup_message(message,"/streaming/characteristics/dimensionschanged",content);
    /* The official client requests its resolution on control, after the
     * message-channel startup characteristics. This is a local send result,
     * not an acknowledgement or evidence of the resolution delivered. */
    const char *alias=x4_stream_request_resolution_alias(c);
    if (rc>=0 && alias) {
        char resolution[96];
        int count=snprintf(resolution,sizeof(resolution),
            "{\"message\":\"userRequestedResolutionUpdate\",\"resolutionAlias\":\"%s\"}",alias);
        int resolution_rc=count>0 && (size_t)count<sizeof(resolution) ? send_text(control,resolution) : -1;
        x4_stream_startup_note_alias(rtc->stream_startup,resolution_rc,sceKernelGetProcessTime());
        printf("XCloud4: RTC resolution request alias=%s rc=%d (requested, not delivered)\n",alias,resolution_rc);
        rc|=resolution_rc;
    }
#else
    printf("XCloud4: RTC video request width=%u height=%u max_fps=%u bitrate_kbps=%u profile=%s (requested, not delivered)\n",
        X4_STREAM_WIDTH,X4_STREAM_HEIGHT,X4_STREAM_FPS,X4_STREAM_BITRATE_KBPS,X4_STREAM_LABEL);
#define X4_TEXT_I(x) #x
#define X4_TEXT(x) X4_TEXT_I(x)
    rc |= startup_message(message, "/streaming/characteristics/clientdevicecapabilities",
        "{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":" X4_TEXT(X4_STREAM_FPS)
        ",\"maxWidth\":" X4_TEXT(X4_STREAM_WIDTH) ",\"maxHeight\":" X4_TEXT(X4_STREAM_HEIGHT) ",\"maxBitrateKbps\":" X4_TEXT(X4_STREAM_BITRATE_KBPS)
        ",\"video\":{\"width\":" X4_TEXT(X4_STREAM_WIDTH) ",\"height\":" X4_TEXT(X4_STREAM_HEIGHT)
        ",\"maxWidth\":" X4_TEXT(X4_STREAM_WIDTH) ",\"maxHeight\":" X4_TEXT(X4_STREAM_HEIGHT) ",\"maxBitrateKbps\":" X4_TEXT(X4_STREAM_BITRATE_KBPS) "}}");
    rc |= startup_message(message, "/streaming/characteristics/dimensionschanged",
        "{\"horizontal\":" X4_TEXT(X4_STREAM_WIDTH) ",\"vertical\":" X4_TEXT(X4_STREAM_HEIGHT)
        ",\"preferredWidth\":" X4_TEXT(X4_STREAM_WIDTH) ",\"preferredHeight\":" X4_TEXT(X4_STREAM_HEIGHT)
        ",\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":" X4_TEXT(X4_STREAM_WIDTH) ",\"safeAreaBottom\":" X4_TEXT(X4_STREAM_HEIGHT) ",\"supportsCustomResolution\":true}");
#undef X4_TEXT
#undef X4_TEXT_I
#endif
    if (rc < 0) fail(rtc, -42);
    else {atomic_store(&rtc->startup_complete,true);printf("XCloud4: RTC canales Xbox preparados\n");}
}
static void channel_open(int channel, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    if (channel == rtc->channels[3]) {
        char id[37], message[192];
        uuid(id);
        snprintf(message, sizeof(message), "{\"type\":\"Handshake\",\"version\":\"messageV1\",\"id\":\"%s\",\"cv\":\"0\"}", id);
        if (send_text(channel, message) < 0) fail(rtc, -43);
    }
    if (channel == rtc->channels[2]) {
        /* ClientMetadata report: u16 type8, shared u32 sequence, f64 elapsed ms,
         * u8 supported touch points0; all fields are little endian. */
        unsigned char metadata[15] = {0};
        atomic_store(&rtc->input_metadata_sent,false);
        input_header(rtc,metadata,8);
        if (rtcSendMessage(channel, (const char *)metadata, sizeof(metadata)) < 0) fail(rtc, -43);
        else atomic_store(&rtc->input_metadata_sent,true);
    }
    start_channels(rtc);
    leave(pointer);
}
static bool lifetime_reason(const char *reason)
{
    size_t n=strlen(reason);
    if(!n||n>40||!((reason[0]>='A'&&reason[0]<='Z')||(reason[0]>='a'&&reason[0]<='z')))return false;
    for(size_t i=1;i<n;++i)if(!((reason[i]>='A'&&reason[i]<='Z')||
        (reason[i]>='a'&&reason[i]<='z')||(reason[i]>='0'&&reason[i]<='9')))return false;
    return true;
}
static bool lifetime_hr(X4JsonSpan value,uint32_t *out)
{
    if(!x4_json_uint32(value,out))return true;
    /* HRESULT may be a signed JSON int32, or an explicit hexadecimal string.
     * Bound both representations; never forward arbitrary display text. */
    if(x4_json_type(value)==X4_JSON_T_NUMBER&&value.length>1&&value.length<=11&&value.data[0]=='-'){
        uint64_t n=0;
        for(size_t i=1;i<value.length;++i){unsigned d=(unsigned char)value.data[i]-'0';if(d>9)return false;n=n*10+d;}
        if(n>2147483648ull)return false;
        *out=0u-(uint32_t)n;return true;
    }
    char text[11];
    if(x4_json_token(value,text,sizeof(text))||strlen(text)<3||text[0]!='0'||(text[1]!='x'&&text[1]!='X'))return false;
    uint32_t n=0;
    for(size_t i=2;text[i];++i){unsigned d;
        if(text[i]>='0'&&text[i]<='9')d=(unsigned)(text[i]-'0');
        else if(text[i]>='a'&&text[i]<='f')d=(unsigned)(text[i]-'a')+10;
        else if(text[i]>='A'&&text[i]<='F')d=(unsigned)(text[i]-'A')+10;
        else return false;
        n=(n<<4)|d;
    }
    *out=n;return true;
}
static bool lifetime_id(X4JsonSpan value)
{
    /* The enclosing JSON is already validated. An opaque string can contain
     * punctuation or escapes. Echo the original span rather than decoding it
     * and imposing an alphabet or changing its JSON type. */
    X4JsonType type=x4_json_type(value);
    return (type==X4_JSON_T_STRING&&value.length>2)||
        (type==X4_JSON_T_NUMBER&&value.length<68);
}

#if !X4_PUBLIC
static void lifetime_note(X4Rtc *rtc,enum LifetimeStep step,int ack_rc)
{
    /* Keep a separate slot for the first terminal notice even after the
     * bounded diagnostic history has filled. Only the terminal winner uses it. */
    LabLifetimeSample *sample=&rtc->lab_lifetime_terminal;
    if(step!=LIFETIME_TERMINAL){
        unsigned i=atomic_load_explicit(&rtc->lab_lifetime_reserved,memory_order_relaxed);
        do {
            if(i>=LAB_LIFETIME_LINES)return;
        } while(!atomic_compare_exchange_weak_explicit(&rtc->lab_lifetime_reserved,&i,i+1u,
            memory_order_relaxed,memory_order_relaxed));
        sample=&rtc->lab_lifetime[i];
    }
    sample->step=step;
    sample->ack_rc=ack_rc;
    atomic_store_explicit(&sample->ready,true,memory_order_release);
}

static void lifetime_emit(const LabLifetimeSample *sample)
{
    static const char *const fields[]={
        [LIFETIME_TYPE]="resultado=rechazado paso=tipo motivo=ausente_o_inesperado",
        [LIFETIME_ID]="resultado=rechazado paso=id motivo=ausente_o_invalido",
        [LIFETIME_CONTENT]="resultado=rechazado paso=contenido motivo=ausente_o_invalido",
        [LIFETIME_REASON]="resultado=rechazado paso=motivo motivo=ausente_o_invalido",
        [LIFETIME_TIMER]="resultado=rechazado paso=temporizador motivo=invalido",
        [LIFETIME_IDLE]="resultado=aceptado paso=aviso motivo=inactividad",
        [LIFETIME_TERMINAL]="resultado=aceptado paso=aviso motivo=terminal",
        [LIFETIME_REPEAT]="resultado=aceptado paso=aviso motivo=terminal_repetido",
    };
    x4_log_printf("XCloud4: RTC lifetime %s\n",fields[sample->step]);
    if(sample->step>=LIFETIME_IDLE)
        x4_log_printf("XCloud4: RTC lifetime paso=ack rc=%d\n",sample->ack_rc);
}
#else
#define lifetime_note(rtc,step,ack_rc) ((void)(rtc),(void)(step),(void)(ack_rc))
#endif

static void lifetime_message(X4Rtc *rtc,int channel,const char *data,size_t length)
{
    X4JsonSpan root,type,target,content,id,body,reason,value;
    char kind[32],destination[96],buffer[4097],label[41]={0};
    if(x4_json_parse(data,length,&root)||
        x4_json_member(root,"target",&target)!=1||x4_json_token(target,destination,sizeof(destination))||
        strcmp(destination,"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect"))return;
    int ack_rc=0;
    enum LifetimeStep step=LIFETIME_TYPE;
    if(x4_json_member(root,"type",&type)!=1||x4_json_token(type,kind,sizeof(kind))||
        (strcmp(kind,"TransactionStart")&&strcmp(kind,"Message")))goto done;
    step=LIFETIME_ID;
    static const char prefix[]="{\"type\":\"TransactionComplete\",\"content\":\"\\\"\\\"\",\"id\":";
    static const char suffix[]=",\"cv\":\"\"}";
    if(x4_json_member(root,"id",&id)!=1||!lifetime_id(id)||
        id.length>sizeof(buffer)-(sizeof(prefix)-1)-sizeof(suffix))goto done;
    step=LIFETIME_CONTENT;
    if(x4_json_member(root,"content",&content)!=1)goto done;
    body=content;
    if(x4_json_type(content)==X4_JSON_T_STRING&&
        (x4_json_token(content,buffer,sizeof(buffer))||x4_json_parse(buffer,strlen(buffer),&body)))goto done;
    if(x4_json_type(body)!=X4_JSON_T_OBJECT)goto done;
    step=LIFETIME_REASON;
    if(x4_json_member(body,"reason",&reason)!=1||x4_json_type(reason)!=X4_JSON_T_STRING)goto done;
    /* A syntactically valid but unsafe/oversized reason still closes the
     * session, with no reason exposed; it is not an RTC/network failure. */
    if(x4_json_token(reason,label,sizeof(label))||!lifetime_reason(label))label[0]='\0';
    if(!strcmp(label,"WarningForBeingIdle")){
        step=LIFETIME_TIMER;
        uint32_t seconds=120;
        int found=x4_json_member(body,"secondsUntilKick",&value);
        if(found<0||(found==1&&(x4_json_uint32(value,&seconds)||seconds<1||seconds>3600)))goto done;
        atomic_store_explicit(&rtc->server_idle_seconds,seconds,memory_order_relaxed);
        atomic_fetch_add_explicit(&rtc->server_idle_sequence,1,memory_order_release);
        step=LIFETIME_IDLE;
    }else if(!atomic_exchange_explicit(&rtc->server_terminal_claimed,true,memory_order_acq_rel)){
        /* Only an idle warning needs a valid timer. Optional diagnostic
         * HRESULT metadata must not discard an explicit terminal reason. */
        uint32_t hr=0;
        rtc->server_hr_valid=x4_json_member(body,"hr",&value)==1&&lifetime_hr(value,&hr);
        rtc->server_hr=hr;
        memcpy(rtc->server_reason,label,sizeof(label));
        atomic_store_explicit(&rtc->server_closed,true,memory_order_release);
        step=LIFETIME_TERMINAL;
    }else step=LIFETIME_REPEAT;
    /* A peer can close the channel while we acknowledge the transaction.
     * Reuse the decoded content buffer now that no body fields are read. */
    memcpy(buffer,prefix,sizeof(prefix)-1);
    memcpy(buffer+sizeof(prefix)-1,id.data,id.length);
    memcpy(buffer+sizeof(prefix)-1+id.length,suffix,sizeof(suffix));
    ack_rc=send_text(channel,buffer);
done:
    lifetime_note(rtc,step,ack_rc);
}
#if !X4_PUBLIC
static const char *const lab_targets[LAB_TARGET_COUNT] = {
    "ausente", "oculto", "/streaming/systemUi/*", "/streaming/systemUi/configuration",
    "/streaming/sessionLifetimeManagement/*",
    "/streaming/sessionLifetimeManagement/serverInitiatedDisconnect"
};

static const char *lab_safe_word(X4JsonSpan value, const char *const *words, size_t count)
{
    char decoded[64];
    if (x4_json_token(value, decoded, sizeof(decoded))) return NULL;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(decoded, words[i])) return words[i];
    return NULL;
}

static enum LabTarget lab_target(X4JsonSpan value)
{
    static const char system_ui[] = "/streaming/systemUi/";
    static const char lifetime[] = "/streaming/sessionLifetimeManagement/";
    char decoded[96];
    if (x4_json_token(value, decoded, sizeof(decoded))) return LAB_TARGET_OTHER;
    if (!strcmp(decoded, lab_targets[LAB_TARGET_SYSTEM_UI_CONFIG])) return LAB_TARGET_SYSTEM_UI_CONFIG;
    if (!strcmp(decoded, lab_targets[LAB_TARGET_SERVER_CLOSE])) return LAB_TARGET_SERVER_CLOSE;
    if (!strncmp(decoded, system_ui, sizeof(system_ui)-1)) return LAB_TARGET_SYSTEM_UI;
    if (!strncmp(decoded, lifetime, sizeof(lifetime)-1)) return LAB_TARGET_LIFETIME;
    return LAB_TARGET_OTHER;
}

static void lab_safe_content(X4JsonSpan content, enum LabTarget target, char output[161])
{
    static const char *const reasons[] = {
        "KickForClosedGame", "WarningForBeingIdle", "SessionExpired",
        "SessionTimeLimitExceeded", "UserRequestedSessionEnd", "GameSessionEnded",
        "KickForOutOfStreamingTime"
    };
    static const char *const ui_types[] = {"ShowMessage", "OpenDialog", "CloseDialog", "Notify", "Configuration"};
    X4JsonSpan inner = content, value;
    char decoded[512];
    memcpy(output, "oculto", sizeof("oculto"));
    if (x4_json_type(content) == X4_JSON_T_STRING &&
        (x4_json_token(content, decoded, sizeof(decoded)) ||
        x4_json_parse(decoded, strlen(decoded), &inner))) return;
    if (target == LAB_TARGET_LIFETIME || target == LAB_TARGET_SERVER_CLOSE) {
        const char *reason = NULL;
        uint32_t seconds;
        if (x4_json_member(inner, "reason", &value) == 1)
            reason = lab_safe_word(value, reasons, sizeof(reasons)/sizeof(reasons[0]));
        bool seconds_valid = x4_json_member(inner, "secondsUntilKick", &value) == 1 &&
            !x4_json_uint32(value, &seconds) && seconds >= 1 && seconds <= 3600;
        if (reason && seconds_valid)
            (void)snprintf(output, 161, "reason=%s secondsUntilKick=%u", reason, (unsigned)seconds);
        else if (reason) (void)snprintf(output, 161, "reason=%s", reason);
        else if (seconds_valid) (void)snprintf(output, 161, "secondsUntilKick=%u", (unsigned)seconds);
    } else if (x4_json_member(inner, "type", &value) == 1) {
        const char *type = lab_safe_word(value, ui_types, sizeof(ui_types)/sizeof(ui_types[0]));
        if (type) (void)snprintf(output, 161, "type=%s", type);
    }
}

static void lab_queue_message(X4Rtc *rtc, bool control, const char *data, size_t length)
{
    static const char *const types[] = {
        "HandshakeAck", "Handshake", "Message", "TransactionStart", "TransactionComplete",
        "TransactionCancel", "TransactionError", "TransactionProgress", "MessageAck",
        "authorizationResponse", "authorizationAccepted", "authorizationFailed",
        "videoKeyframeRequested", "gamepadChanged", "userRequestedResolutionUpdate"
    };
    X4JsonSpan root, field, content = {0};
    const char *type = "no_json";
    enum LabTarget target = LAB_TARGET_ABSENT;
    uint8_t id_kind = LAB_ID_ABSENT;
    if (data && length && length <= 4096 && !x4_json_parse(data, length, &root) &&
        x4_json_type(root) == X4_JSON_T_OBJECT) {
        int found = x4_json_member(root, "type", &field);
        if (found == 0 && control) found = x4_json_member(root, "message", &field);
        const char *safe_type = found == 1 ? lab_safe_word(field, types, sizeof(types)/sizeof(types[0])) : NULL;
        type = safe_type ? safe_type : found ? "oculto" : "ausente";
        found = x4_json_member(root, "target", &field);
        target = found == 1 ? lab_target(field) : found ? LAB_TARGET_OTHER : LAB_TARGET_ABSENT;
        found = x4_json_member(root, "id", &field);
        if (found) id_kind = LAB_ID_OTHER;
        if (found == 1 && (target == LAB_TARGET_LIFETIME || target == LAB_TARGET_SERVER_CLOSE)) {
            X4JsonType kind = x4_json_type(field);
            if (kind == X4_JSON_T_STRING) id_kind = LAB_ID_STRING;
            else if (kind == X4_JSON_T_NUMBER) id_kind = LAB_ID_NUMBER;
        }
        if (x4_json_member(root, "content", &content) != 1) content = (X4JsonSpan){0};
    }
    atomic_fetch_add_explicit(&rtc->lab_target_counts[target], 1, memory_order_relaxed);
    unsigned line = atomic_load_explicit(&lab_message_lines, memory_order_relaxed);
    do {
        if (line >= LAB_MESSAGE_LINES) return;
    } while (!atomic_compare_exchange_weak_explicit(&lab_message_lines, &line, line + 1u,
        memory_order_relaxed, memory_order_relaxed));
    LabMessageSample *sample = &rtc->lab_messages[line];
    sample->type = type;
    sample->target = target;
    sample->control = control;
    sample->id_kind = id_kind;
    sample->content_wire_bytes = content.length;
    memcpy(sample->content, "-", sizeof("-"));
    if (content.length && target >= LAB_TARGET_SYSTEM_UI)
        lab_safe_content(content, target, sample->content);
    atomic_store_explicit(&sample->ready, true, memory_order_release);
}

/* A later callback may publish first. Emit only the contiguous prefix so the
 * session owner's log preserves reservation order without waiting for writers. */
static void lab_emit_messages(X4Rtc *rtc)
{
    static const char *const id_labels[] = {
        [LAB_ID_ABSENT] = "id_tipo=ausente", [LAB_ID_OTHER] = "id_tipo=otro",
        [LAB_ID_STRING] = "id_tipo=string", [LAB_ID_NUMBER] = "id_tipo=numero"
    };
    for (; rtc->lab_next_message < LAB_MESSAGE_LINES; ++rtc->lab_next_message) {
        const LabMessageSample *sample = &rtc->lab_messages[rtc->lab_next_message];
        if (!atomic_load_explicit(&sample->ready, memory_order_acquire)) break;
        bool lifetime = sample->target == LAB_TARGET_LIFETIME || sample->target == LAB_TARGET_SERVER_CLOSE;
        const char *id_label = lifetime ? id_labels[sample->id_kind] :
            (sample->id_kind == LAB_ID_ABSENT ? "id=0" : "id=1");
        x4_log_printf("XCloud4: RTC mensaje orden=%u canal=%s type=%s target=%s %s content_wire_bytes=%zu content=%.160s\n",
            rtc->lab_next_message + 1u, sample->control ? "control" : "message",
            sample->type, lab_targets[sample->target], id_label,
            sample->content_wire_bytes, sample->content);
    }
    for(;rtc->lab_next_lifetime<LAB_LIFETIME_LINES;++rtc->lab_next_lifetime){
        const LabLifetimeSample *sample=&rtc->lab_lifetime[rtc->lab_next_lifetime];
        if(!atomic_load_explicit(&sample->ready,memory_order_acquire))break;
        lifetime_emit(sample);
    }
    if(!rtc->lab_lifetime_terminal_logged&&
        atomic_load_explicit(&rtc->lab_lifetime_terminal.ready,memory_order_acquire)){
        lifetime_emit(&rtc->lab_lifetime_terminal);
        rtc->lab_lifetime_terminal_logged=true;
    }
}

static void lab_close_summary(X4Rtc *rtc)
{
    for (unsigned i = 0; i < LAB_TARGET_COUNT; ++i) {
        uint64_t total = atomic_load_explicit(&rtc->lab_target_counts[i], memory_order_relaxed);
        if (total) x4_log_printf("XCloud4: RTC mensajes resumen peer=%u target=%s total=%llu\n",
            rtc->nack_epoch, lab_targets[i], (unsigned long long)total);
    }
}
#endif

static void channel_message(int channel, const char *data, int size, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    size_t length = size < 0 ? (size_t)(-(int64_t)size - 1) : (size_t)size;
#if !X4_PUBLIC
    if (channel == rtc->channels[1] || channel == rtc->channels[3])
        lab_queue_message(rtc, channel == rtc->channels[1], data, length);
#endif
#if X4_RUMBLE
    /* libdatachannel's nonnegative size denotes binary. Input report bytes
     * are parsed into an optional latest slot; hardware stays on main. */
    if (channel == rtc->channels[2] && size >= 0 && data && rtc->rumble)
        (void)x4_rumble_receive(rtc->rumble, data, length, sceKernelGetProcessTime());
#endif
    if (channel == rtc->channels[3] && data && length && length <= 4096) {
        char type[32];
        X4JsonField field = {.name="type", .kind=X4_JSON_STRING, .text=type, .capacity=sizeof(type)};
        if (x4_json_fields(data, length, &field, 1) == 0 && field.found && !strcmp(type,"HandshakeAck")) {
            lock(&rtc->data_gate);
            rtc->handshake_ack = true;
            unlock(&rtc->data_gate);
            start_channels(rtc);
        }else lifetime_message(rtc,channel,data,length);
    }
    leave(pointer);
}

#if X4_MIC_TRANSPORT
_Static_assert(CALLBACK_SLOTS == X4_MIC_RR_SLOTS, "RTC and microphone report slot bounds must match");

static void microphone_offer(X4Rtc *rtc, rtcTrackInit *audio)
{
    X4MicRequest request = x4_mic_request();
    if (!request.generation) return;
    if (!x4_mic_rtp_seed(&rtc->mic_clock.seed, x4_native_random)) {
        x4_mic_capture_reject(request.generation);
#if !X4_PUBLIC
        x4_log_printf("XCloud4: microphone offer seed unavailable\n");
#endif
        return;
    }
    rtc->mic_generation = request.generation;
#if X4_PUBLIC
    rtc->mic_clock.frame_samples = 480;
#else
    rtc->mic_clock.frame_samples = request.frame_ms == 20 ? 960 : 480;
#endif
    atomic_init(&rtc->mic_stop, false);
    atomic_init(&rtc->mic_done, false);
    audio->direction = RTC_DIRECTION_SENDRECV;
    audio->ssrc = rtc->mic_clock.seed.ssrc;
    audio->name = "audio";
#if !X4_PUBLIC
    x4_log_printf("XCloud4: microphone offer sendrecv=1 frame_ms=%u\n", rtc->mic_clock.frame_samples / 48u);
#endif
}

static bool microphone_ntp(uint64_t *ntp)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return false;
    uint32_t seconds = (uint32_t)((uint64_t)now.tv_sec + UINT64_C(2208988800));
    *ntp = ((uint64_t)seconds << 32) | (((uint64_t)now.tv_nsec << 32) / UINT64_C(1000000000));
    return true;
}

static void microphone_wipe(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size--) *bytes++ = 0;
}

#if !X4_PUBLIC
typedef struct {
    uint32_t bins[42];
    uint64_t samples, maximum;
    unsigned errors, sender_reports;
} MicSendStats;

static void microphone_latency(MicSendStats *stats, uint64_t lag)
{
    size_t bin = lag / 1000u < 41 ? (size_t)(lag / 1000u) : 41;
    ++stats->bins[bin];
    ++stats->samples;
    if (lag > stats->maximum) stats->maximum = lag;
}

static unsigned microphone_latency_percentile(const MicSendStats *stats, unsigned percentile)
{
    if (!stats->samples) return 0;
    uint64_t rank = (stats->samples * percentile + 99u) / 100u, total = 0;
    for (unsigned i = 0; i < 42; ++i) {
        total += stats->bins[i];
        if (total >= rank) return i;
    }
    return 41;
}

static void microphone_log_sender(unsigned slot, uint32_t packets, uint32_t bytes, const MicSendStats *stats)
{
    X4MicReceiverStats rr = x4_mic_rr_snapshot(slot);
    x4_log_printf("XCloud4: microphone tx_ok=%u tx_error=%u opus_bytes=%u sr=%u rr=%u "
        "rr_fraction_q8=%u rr_lost=%d rr_jitter_48khz=%u rr_ext_seq=%u "
        "capture_send_p50_ms_bucket=%u capture_send_p95_ms_bucket=%u capture_send_max_us=%llu\n",
        packets, stats->errors, bytes, stats->sender_reports, rr.reports, rr.fraction_lost,
        (int)rr.total_lost, rr.jitter, rr.last_sequence,
        microphone_latency_percentile(stats, 50), microphone_latency_percentile(stats, 95),
        (unsigned long long)stats->maximum);
}
#endif

static void *microphone_sender(void *argument)
{
    X4Rtc *rtc = argument;
    uint64_t generation = rtc->mic_generation, peer = rtc->nack_epoch;
    unsigned slot = rtc->nack_epoch - 1u;
    uint64_t now = sceKernelGetProcessTime();
    uint64_t open_deadline = now + 10000000u;
    while (!atomic_load_explicit(&rtc->mic_stop, memory_order_acquire) && !rtcIsOpen(rtc->audio)) {
        int state = atomic_load_explicit(&rtc->state, memory_order_acquire);
        if (state == X4_RTC_FAILED || state == X4_RTC_CLOSED) goto done;
        if (sceKernelGetProcessTime() >= open_deadline) {
            x4_mic_capture_reject(generation);
            goto done;
        }
        sceKernelUsleep(10000);
    }
    if (atomic_load_explicit(&rtc->mic_stop, memory_order_acquire)) goto done;
    if (!x4_mic_capture_attach(generation, peer)) {
        x4_mic_capture_reject(generation);
        goto done;
    }
    if (atomic_load_explicit(&rtc->mic_stop, memory_order_acquire)) {
        x4_mic_capture_detach(generation, peer);
        goto done;
    }
    X4MicRtpClock clock = rtc->mic_clock;
    if (!x4_mic_rr_bind(slot, clock.seed.ssrc)) {
        x4_mic_capture_detach(generation, peer);
        x4_mic_capture_reject(generation);
        goto done;
    }
    uint32_t packets = 0, payload_bytes = 0;
    uint64_t last_epoch = 0, next_sr = 0;
    X4MicFrame frame;
    uint8_t packet[X4_MIC_RTP_HEADER + sizeof(frame.data)];
#if !X4_PUBLIC
    MicSendStats stats = {0};
    uint64_t next_log = now + 5000000u;
#endif
    while (!atomic_load_explicit(&rtc->mic_stop, memory_order_acquire)) {
        int state = atomic_load_explicit(&rtc->state, memory_order_acquire);
        if (state == X4_RTC_FAILED || state == X4_RTC_CLOSED) break;
        now = sceKernelGetProcessTime();
        bool have_frame = x4_mic_capture_take(generation, peer, now, &frame);
        if (have_frame && frame.size && frame.size <= sizeof(frame.data) && frame.samples == clock.frame_samples) {
            memcpy(packet + X4_MIC_RTP_HEADER, frame.data, frame.size);
            X4MicRtpClock next = clock;
            if (x4_mic_rtp_header(&next, frame.sample_us, frame.epoch, packet) &&
                !atomic_load_explicit(&rtc->mic_stop, memory_order_acquire) &&
                x4_mic_capture_send_allowed(generation, peer, frame.epoch)) {
                clock = next;
                int result = rtcSendMessage(rtc->audio, (const char *)packet, X4_MIC_RTP_HEADER + frame.size);
                if (result >= 0) {
                    ++packets;
                    payload_bytes += frame.size;
                    last_epoch = frame.epoch;
                    if (!next_sr) next_sr = now + 4500000u + clock.seed.timestamp % 1000000u;
#if !X4_PUBLIC
                    uint64_t after = sceKernelGetProcessTime();
                    if (after >= frame.captured_us) microphone_latency(&stats, after - frame.captured_us);
#endif
                }
#if !X4_PUBLIC
                else ++stats.errors;
#endif
            }
        }
        if (last_epoch && next_sr && now >= next_sr &&
            !atomic_load_explicit(&rtc->mic_stop, memory_order_acquire) &&
            x4_mic_capture_send_allowed(generation, peer, last_epoch)) {
            uint64_t ntp;
            if (microphone_ntp(&ntp)) {
                uint8_t report[X4_MIC_RTCP_SR_BYTES];
                x4_mic_rtcp_sender_report(report, clock.seed.ssrc, ntp,
                    x4_mic_rtp_time_at(&clock, sceKernelGetProcessTime()), packets, payload_bytes);
                int result = rtcSendMessage(rtc->audio, (const char *)report, sizeof(report));
#if !X4_PUBLIC
                if (result >= 0) ++stats.sender_reports;
#else
                (void)result;
#endif
            }
            next_sr = now + 4500000u + (clock.seed.timestamp ^ packets) % 1000000u;
        }
#if !X4_PUBLIC
        if (now >= next_log) {
            microphone_log_sender(slot, packets, payload_bytes, &stats);
            next_log = now + 5000000u;
        }
#endif
        if (!have_frame) sceKernelUsleep(2000);
    }
    microphone_wipe(&frame, sizeof(frame));
    microphone_wipe(packet, sizeof(packet));
    x4_mic_rr_unbind(slot);
    x4_mic_capture_detach(generation, peer);
#if !X4_PUBLIC
    microphone_log_sender(slot, packets, payload_bytes, &stats);
#endif
done:
    atomic_store_explicit(&rtc->mic_done, true, memory_order_release);
    return NULL;
}

static void microphone_answer(X4Rtc *rtc, const char *sdp, int native_rc)
{
    if (!rtc->mic_generation) return;
    bool accepted = native_rc >= 0 && x4_mic_sdp_can_send(sdp, strlen(sdp));
    int sender_rc = 0;
    if (accepted && !rtc->mic_thread_started) {
        OrbisPthreadAttr attr;
        sender_rc = scePthreadAttrInit(&attr);
        if (!sender_rc) {
            sender_rc = scePthreadAttrSetstacksize(&attr, 256u * 1024u);
            if (!sender_rc) sender_rc = scePthreadCreate(&rtc->mic_thread, &attr, microphone_sender, rtc, "x4-mic-rtp");
            (void)scePthreadAttrDestroy(&attr);
        }
        rtc->mic_thread_started = sender_rc == 0;
    }
    if (!accepted || sender_rc) x4_mic_capture_reject(rtc->mic_generation);
#if !X4_PUBLIC
    x4_log_printf("XCloud4: microphone answer accepted=%u native_rc=%d sender_rc=%d\n",
        accepted ? 1u : 0u, native_rc, sender_rc);
#endif
}

static int microphone_close(X4Rtc *rtc)
{
    if (!rtc->mic_thread_started) return 0;
    atomic_store_explicit(&rtc->mic_stop, true, memory_order_release);
    x4_mic_capture_detach(rtc->mic_generation, rtc->nack_epoch);
    x4_mic_rr_unbind(rtc->nack_epoch - 1u);
    uint64_t deadline = sceKernelGetProcessTime() + 100000u;
    while (!atomic_load_explicit(&rtc->mic_done, memory_order_acquire)) {
        if (sceKernelGetProcessTime() >= deadline) return RTC_ERR_NOT_AVAIL;
        sceKernelUsleep(1000);
    }
    int rc = scePthreadJoin(rtc->mic_thread, NULL);
    if (!rc) rtc->mic_thread_started = false;
    return rc;
}
#endif

#ifdef X4_OPENORBIS
typedef struct {
    int rcvbuf, sndbuf, requested, attempts;
    bool available;
} X4UdpBuffers;

extern int x4_rtc_native_udp_buffers(int pc, int *rcvbuf, int *sndbuf,
    int *requested, int *attempts);

#if X4_PUBLIC
static atomic_flag udp_buffers_gate = ATOMIC_FLAG_INIT;
static X4UdpBuffers udp_buffers_last;

void x4_rtc_diag_write_udp_json(FILE *f)
{
    lock(&udp_buffers_gate);
    X4UdpBuffers values = udp_buffers_last;
    unlock(&udp_buffers_gate);
    if (!values.available) {
        fputs("\"udp_buffers\":null,", f);
        return;
    }
    fprintf(f, "\"udp_buffers\":{\"rcvbuf_bytes\":%d,\"sndbuf_bytes\":%d,"
        "\"requested_rcvbuf_bytes\":%d,\"receive_attempts\":%d},",
        values.rcvbuf, values.sndbuf, values.requested, values.attempts);
}
#endif

static void report_udp_buffers(X4Rtc *rtc)
{
    if (rtc->udp_buffers_logged || rtc->pc < 0) return;
    X4UdpBuffers values = {0};
    if (x4_rtc_native_udp_buffers(rtc->pc, &values.rcvbuf, &values.sndbuf,
            &values.requested, &values.attempts) != 0) return;
    rtc->udp_buffers_logged = true;
#if X4_PUBLIC
    values.available = true;
    lock(&udp_buffers_gate);
    udp_buffers_last = values;
    unlock(&udp_buffers_gate);
#endif
    x4_log_printf("XCloud4: red udp rcvbuf=%d sndbuf=%d pedido=%d intentos=%d\n",
        values.rcvbuf, values.sndbuf, values.requested, values.attempts);
}
#endif

static void open_diagnostics(bool reconnect)
{
    if (reconnect) return;
#if defined(X4_OPENORBIS) && X4_PUBLIC
    lock(&udp_buffers_gate);
    udp_buffers_last = (X4UdpBuffers){0};
    unlock(&udp_buffers_gate);
#endif
#if X4_REMB_ENABLED && X4_PUBLIC
    remb_first_peer_us = sceKernelGetProcessTime();
    remb_next_report_us = remb_first_peer_us + X4_REMB_PUBLIC_REPORT_INTERVAL_US;
    remb_session_trace = NULL;
    remb_finished_peers = (X4RembSummary){0};
#endif
#line 2004
    x4_rtc_receive_diag_reset();
#if !X4_PUBLIC
    atomic_store_explicit(&lab_message_lines, 0, memory_order_relaxed);
    atomic_store_explicit(&lab_close_arrival, 0, memory_order_relaxed);
    atomic_store_explicit(&lab_close_order, 0, memory_order_relaxed);
    atomic_store_explicit(&lab_first_video, 0, memory_order_relaxed);
#endif
}
static X4Rtc *open_request_mode(int *error,const X4StreamRequestConfig *request,bool reconnect)
{
    int rc = -44;
#if X4_STREAM_PROFILE
    if (!request || (unsigned)request->profile>X4_REQUEST_1080P_C ||
        (unsigned)request->color_preference>X4_COLOR_FORCE_601 || request->cpu_variant>X4_CPU_3) goto fail_no_context;
#else
    (void)request;
#endif
    if (x4_native_entropy_prepare() < 0 || x4_native_net_prepare() < 0) goto fail_no_context;
    X4Rtc *rtc = calloc(1, sizeof(*rtc));
    if (!rtc) goto fail_no_context;
#if X4_STREAM_PROFILE
    rtc->request_config=x4_stream_request_capture(request);
#endif
    rtc->pc = rtc->video = rtc->audio = -1;
    rtc->reconnect_peer=reconnect;
    rtc->started_at = sceKernelGetProcessTime();
    open_diagnostics(reconnect);
    for (unsigned i=0;i<4;++i) rtc->channels[i]=-1;
    atomic_init(&rtc->state, X4_RTC_NEW);
    atomic_init(&rtc->error, 0);
    atomic_init(&rtc->gathering_done, false);
    atomic_init(&rtc->offer_logged, false);
    atomic_init(&rtc->video_packets, 0);
    atomic_init(&rtc->audio_packets, 0);
    atomic_init(&rtc->input_stop,false);atomic_init(&rtc->input_ready,false);
    atomic_init(&rtc->nack_stop,false);
    atomic_init(&rtc->input_metadata_sent,false);atomic_init(&rtc->startup_complete,false);
    atomic_init(&rtc->input_error,0);
    atomic_init(&rtc->input_packets,0);atomic_init(&rtc->input_dropped,0);
    atomic_init(&rtc->server_terminal_claimed,false);atomic_init(&rtc->server_closed,false);
    atomic_init(&rtc->server_idle_sequence,0);atomic_init(&rtc->server_idle_seconds,120);
    atomic_flag_clear(&rtc->data_gate);
    lock(&callback_gate);
    if (slots_used < CALLBACK_SLOTS) { rtc->slot=&slots[slots_used++]; rtc->slot->context=rtc; }
    unlock(&callback_gate);
    if (!rtc->slot) { free(rtc); rc=-45; goto fail_no_context; }
    rtc->nack_epoch = (uint32_t)(rtc->slot - slots) + 1u;
#if X4_RECOVERY43_TRANSPORT
    recovery43_prepare(rtc);
#endif
#if !X4_PUBLIC
    lab_peer_diagnostics_init(rtc);
    lock(&callback_gate);
    lab_transport_context = rtc;
    unlock(&callback_gate);
#endif
    rtcInitLogger(RTC_LOG_NONE, NULL); /* Raw dependency logs can contain ICE credentials. */
    /* Explicit application policy: four RTC workers. The SDK's precompiled
     * hardware_concurrency probe reported200112 workers in the actual0.7.1
     * PS4 kernel capture, so it cannot define this application's pool. */
    printf("XCloud4: RTC begin rtcSetThreadPoolSize(4)\n");
    rc=rtcSetThreadPoolSize(4);
    printf("XCloud4: RTC rtcSetThreadPoolSize(4) rc=%d\n",rc);
    if(rc<0)goto fail_context;
    const char *servers[] = {"stun:stun.l.google.com:19302"};
    rtcConfiguration configuration = {.iceServers=servers,.iceServersCount=1,
        .certificateType=RTC_CERTIFICATE_ECDSA,.disableAutoNegotiation=true,
        .forceMediaTransport=true,.mtu=1280,.maxMessageSize=65536,
        .disableFingerprintVerification=false};
    printf("XCloud4: RTC begin rtcCreatePeerConnection\n");
    rc=rtc->pc=rtcCreatePeerConnection(&configuration);
    printf("XCloud4: RTC rtcCreatePeerConnection rc=%d\n",rc);
    if (rc<0) goto fail_context;
    rtcSetUserPointer(rtc->pc,rtc->slot);
#define CHECK(call) do { \
    printf("XCloud4: RTC begin %s\n",#call); \
    rc=(call); \
    printf("XCloud4: RTC %s rc=%d\n",#call,rc); \
    if(rc<0)goto fail_context; \
}while(0)
    CHECK(rtcSetLocalDescriptionCallback(rtc->pc,description_callback));
    CHECK(rtcSetLocalCandidateCallback(rtc->pc,candidate_callback));
    CHECK(rtcSetStateChangeCallback(rtc->pc,state_callback));
    CHECK(rtcSetIceStateChangeCallback(rtc->pc,ice_state_callback));
    CHECK(rtcSetGatheringStateChangeCallback(rtc->pc,gathering_callback));
    /* Keep media routing keyed by track handle when the offer order changes. */
    rtcTrackInit audio={.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_OPUS,.payloadType=111,.mid="0",
        .profile="minptime=10;useinbandfec=1;stereo=1"};
    rtcTrackInit video={.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_H264,.payloadType=102,.mid="1",
#if X4_STREAM_PROFILE
        .profile=rtc->request_config.h264_fmtp};
#else
        .profile=X4_STREAM_H264_FMTP};
#endif
#if X4_MIC_TRANSPORT
    microphone_offer(rtc, &audio);
#endif
    CHECK(rtc->audio=rtcAddTrackEx(rtc->pc,&audio));
    CHECK(rtc->video=rtcAddTrackEx(rtc->pc,&video));
    int tracks[]={rtc->audio,rtc->video};
    for(unsigned i=0;i<2;++i) {
        printf("XCloud4: RTC configure track index=%u\n",i);
        rtcSetUserPointer(tracks[i],rtc->slot);
        CHECK(rtcChainRtcpReceivingSession(tracks[i]));
        CHECK(rtcSetMessageCallback(tracks[i],rtp_callback));
    }
#if X4_STREAM_PROFILE
    const X4StreamNackPolicy nack_policy=x4_stream_request_nack_policy(&rtc->request_config);
    x4_rtc_diag_reset_video_rtt();
#ifdef X4_OPENORBIS
    CHECK(rtcX4ConfigureVideoRr(rtc->video,(rtcX4VideoRrMode)x4_stream_request_rr_mode(&rtc->request_config),
        (uint8_t)video.payloadType));
#endif
#else
    const X4StreamNackPolicy nack_policy=X4_NACK_POLICY_T43;
#endif
    CHECK(x4_rtc_configure_receiver_nack(rtc->video,X4_RECEIVER_NACK,rtc->nack_epoch,nack_policy));
    const char *labels[]={"chat","control","input","message"};
    const char *protocols[]={"chatV1","controlV1","1.0","messageV1"};
    for(unsigned i=0;i<4;++i) {
        printf("XCloud4: RTC configure channel index=%u\n",i);
        rtcDataChannelInit config={.protocol=protocols[i]};
        if(i==2) {config.reliability.unordered=true;config.reliability.unreliable=true;config.reliability.maxRetransmits=0;}
        CHECK(rtc->channels[i]=rtcCreateDataChannelEx(rtc->pc,labels[i],&config));
        rtcSetUserPointer(rtc->channels[i],rtc->slot);
        CHECK(rtcSetOpenCallback(rtc->channels[i],channel_open));
        CHECK(rtcSetMessageCallback(rtc->channels[i],channel_message));
    }
#if !X4_PUBLIC
    /* Register after the channel array is complete and before negotiation.
     * A diagnostic hook failure must not affect the session. */
    for (unsigned i = 0; i < 4; ++i) {
        int diagnostic_rc = rtcSetClosedCallback(rtc->channels[i], lab_channel_closed);
        if (diagnostic_rc < 0)
            printf("XCloud4: RTC observador cierre canal=%u rc=%d\n", i, diagnostic_rc);
    }
#endif
    CHECK(rtcSetLocalDescription(rtc->pc,"offer"));
#undef CHECK
    if(error)*error=0;
    return rtc;
fail_context:
    printf("XCloud4: RTC open failed rc=%d\n",rc);
    x4_rtc_close(rtc);
fail_no_context:
    if(error)*error=rc;
    return NULL;
}

X4Rtc *x4_rtc_open(int *error)
{
#if X4_STREAM_PROFILE
    X4StreamRequestConfig c=x4_stream_settings_get();
    return open_request_mode(error,&c,false);
#else
    return open_request_mode(error,NULL,false);
#endif
}
X4Rtc *x4_rtc_open_request(int *error, const X4StreamRequestConfig *request)
{
    return open_request_mode(error,request,false);
}
X4Rtc *x4_rtc_open_reconnect_request(int *error,const X4StreamRequestConfig *request)
{return open_request_mode(error,request,true);}

void x4_rtc_set_media_callback(X4Rtc *rtc,X4RtcMediaCallback callback,void *user)
{
    if(!rtc)return;
    lock(&rtc->data_gate);rtc->media=callback;rtc->media_user=user;unlock(&rtc->data_gate);
}
void x4_rtc_set_diagnostic_trace(X4Rtc *rtc, X4Trace *trace)
{
    if (rtc) {
        rtc->diagnostic_trace = trace;
#if X4_REMB_ENABLED && X4_PUBLIC
        if (!rtc->reconnect_peer) remb_session_trace = trace;
#endif
#line 2178
        x4_rtc_receive_attach(trace);
        x4_rtc_receive_setting(20, 1, 1, 0); /* ReceivingSession, receiver generator, sender responder */
#if X4_STREAM_PROFILE
        const bool legacy=x4_stream_request_legacy_nack(&rtc->request_config);
#else
        const bool legacy=true;
#endif
        x4_rtc_nack_event(X4_MON_CR_NACK_STATE,4u|(X4_RECEIVER_NACK?1u:0u),
            (uint64_t)rtc->nack_epoch<<32,0,X4_RECEIVER_NACK,legacy?64u:512u,
            legacy?((16ull<<32)|64u):((1000ull<<32)|512u));
        (void)x4_rtc_receive_track_config(rtc->audio);
        (void)x4_rtc_receive_track_config(rtc->video);
    }
}
void x4_rtc_set_rumble_mailbox(X4Rtc *rtc, X4Rumble *rumble)
{
#if X4_RUMBLE
    if (rtc) rtc->rumble = rumble;
#else
    (void)rtc;
    (void)rumble;
#endif
}
void x4_rtc_set_nack_grace_inbox(X4Rtc *rtc, X4NackGraceInbox *inbox)
{
    if (!rtc) return;
    rtc->grace_inbox = inbox;
    x4_nack_grace_inbox_bind(inbox, rtc->nack_epoch);
}
int x4_rtc_set_stream_startup(X4Rtc *rtc,X4StreamStartup *startup)
{
    if(!rtc)return -1;
#if X4_STREAM_PROFILE
    lock(&rtc->data_gate);
    int rc=rtc->startup_sent?-1:0;
    if(!rc)rtc->stream_startup=startup;
    unlock(&rtc->data_gate);return rc;
#else
    (void)startup;return 0;
#endif
}
int x4_rtc_set_keyframe_limiter(X4Rtc *rtc,X4KeyframeLimiter *limiter)
{
    if(!rtc)return -1;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(!limiter)return RTC_ERR_NOT_AVAIL;
    lock(&rtc->data_gate);
    int rc=rtc->startup_sent?-1:0;
    if(!rc){
        X4KeyframeLimiterSnapshot state;x4_keyframe_limiter_snapshot(limiter,&state);
        rtc->keyframe_limiter=limiter;rtc->keyframe_grant_consumed=state.granted;
    }
    unlock(&rtc->data_gate);return rc;
#else
    (void)limiter;return 0;
#endif
}
int x4_rtc_set_gamepad_source(X4Rtc *rtc,X4GamepadSource source,void *user)
{
    if(!rtc || !source || rtc->input_thread_started || atomic_load(&rtc->input_stop))return -46;
    lock(&rtc->data_gate);rtc->input_source=source;rtc->input_user=user;unlock(&rtc->data_gate);
    int rc=scePthreadCreate(&rtc->input_thread,NULL,input_sender,rtc,"XCloud4Input");
    if(rc) {
        lock(&rtc->data_gate);rtc->input_source=NULL;rtc->input_user=NULL;unlock(&rtc->data_gate);
        input_error(rtc,rc);
        return rc;
    }
    rtc->input_thread_started=true;
    printf("XCloud4: RTC input sender iniciado\n");
    return 0;
}
int x4_rtc_local_description(X4Rtc *rtc,char *sdp,size_t capacity)
{
    if(!rtc||!sdp||!capacity)return -1;
    sdp[0]=0;
    if(atomic_load(&rtc->error))return atomic_load(&rtc->error);
    /* The initial description callback precedes gathering and is only a
     * snapshot. GreenVita gathers before creating its offer; use the current
     * library description after gathering instead of that stale snapshot.
     * The session worker owns this call, and its existing deadline bounds
     * the pending return. No credentials or candidates are constructed here. */
    if(!atomic_load(&rtc->gathering_done))return 0;
    if(capacity>SDP_CAP)capacity=SDP_CAP;
    int required=rtcGetLocalDescription(rtc->pc,NULL,0);
    if(required==RTC_ERR_NOT_AVAIL)return 0;
    if(required<1)return required<0?required:-40;
    /* CAPI lengths include the NUL terminator. The library copies its
     * description under its own mutex; no application callback lock is held. */
    if((size_t)required>capacity)return RTC_ERR_TOO_SMALL;
    char type[16]={0};
    int rc=rtcGetLocalDescriptionType(rtc->pc,type,sizeof(type));
    if(rc!=6||memcmp(type,"offer",6))return rc<0?rc:-40;
    rc=rtcGetLocalDescription(rtc->pc,sdp,(int)capacity);
    if(rc!=required||rc<1||(size_t)rc>capacity||sdp[rc-1]||strnlen(sdp,(size_t)rc)!=(size_t)rc-1) {
        memset(sdp,0,capacity);
        return rc<0?rc:-40;
    }
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(rtc->request_config.profile==X4_REQUEST_1080P_G8){
        int bitrate_rc=x4_stream_sdp_limit_video(sdp,capacity,8000);
        if(bitrate_rc){sdp[0]=0;return bitrate_rc;}
        rc=(int)strlen(sdp)+1;
    }
    X4SdpVideoBandwidth offer_bandwidth={0};
    if(x4_stream_sdp_video_bandwidth(sdp,(size_t)rc-1,&offer_bandwidth))
        x4_trace_sdp_bandwidth_record(rtc->diagnostic_trace,0,offer_bandwidth.video_sections,
            offer_bandwidth.as_lines,offer_bandwidth.first_as_kbps,offer_bandwidth.malformed_as_lines);
#endif
#if X4_RECOVERY43_TRANSPORT
    if(rtc->recovery43_mode){
        int recovery_rc=x4_recovery43_sdp_offer(sdp,capacity,rtc->recovery43_mode,&rtc->recovery43_offered);
        rtc->recovery43_offer_failed=recovery_rc!=0;
#if !X4_PUBLIC
        if(recovery_rc)x4_log_printf("XCloud4: recuperacion43 oferta rc=%d; sin RTX/FEC\n",recovery_rc);
#endif
        rc=(int)strlen(sdp)+1;
    }
#endif
    if(!atomic_exchange(&rtc->offer_logged,true)) {
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        printf("XCloud4: SDP offer video bandwidth profile=%s sections=%u AS_lines=%u first_AS_kbps=%u malformed=%u (requested, not delivered)\n",
            rtc->request_config.label,offer_bandwidth.video_sections,offer_bandwidth.as_lines,
            offer_bandwidth.first_as_kbps,offer_bandwidth.malformed_as_lines);
#endif
        sdp_shape_log(sdp,(size_t)rc-1);
        sdp_receive_shape(rtc,sdp,(size_t)rc-1,0);
    }
    return 1;
}
int x4_rtc_next_local_candidate(X4Rtc *rtc,char *candidate,size_t capacity,char *mid,size_t mid_capacity)
{
    if(!rtc||!candidate||!mid)return -1;
    if(atomic_load(&rtc->error))return atomic_load(&rtc->error);
    lock(&rtc->data_gate);
    int rc=0;
    if(rtc->candidate_read!=rtc->candidate_write) {
        Candidate *item=&rtc->candidates[rtc->candidate_read%CANDIDATE_COUNT];
        if(strlen(item->candidate)+1>capacity||strlen(item->mid)+1>mid_capacity)rc=-1;
        else {strcpy(candidate,item->candidate);strcpy(mid,item->mid);++rtc->candidate_read;rc=1;}
    }
    unlock(&rtc->data_gate);return rc;
}
int x4_rtc_set_remote_description(X4Rtc *rtc,const char *sdp)
{
    if(!rtc||!sdp||strnlen(sdp,SDP_CAP)>=SDP_CAP)return -1;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* Observe the server's answer even if libdatachannel rejects it. Numeric
     * AS metadata does not imply native acceptance or a delivered bitrate. */
    X4SdpVideoBandwidth bandwidth={0};
    bool observed=x4_stream_sdp_video_bandwidth(sdp,strlen(sdp),&bandwidth);
    if(observed)x4_trace_sdp_bandwidth_record(rtc->diagnostic_trace,1,bandwidth.video_sections,
        bandwidth.as_lines,bandwidth.first_as_kbps,bandwidth.malformed_as_lines);
#endif
#if X4_RECOVERY43_TRANSPORT
    /* Publish the accepted mapping before the native call can deliver RTP. */
    recovery43_accept(rtc,sdp);
#endif
    int rc=rtcSetRemoteDescription(rtc->pc,sdp,"answer");
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(observed)printf("XCloud4: SDP answer video bandwidth sections=%u AS_lines=%u first_AS_kbps=%u malformed=%u native_set_remote_rc=%d (response metadata, not measured bitrate)\n",
        bandwidth.video_sections,bandwidth.as_lines,bandwidth.first_as_kbps,bandwidth.malformed_as_lines,rc);
#endif
    if(rc<0) fail(rtc,rc);
    else {
        sdp_receive_shape(rtc,sdp,strlen(sdp),1);
        int nack_rc = x4_rtc_receiver_nack_accept(rtc->video, rtc->pc);
        /* The experiment must not turn an unavailable feedback observation
         * into a media-session failure, especially in the OFF reference.
         * A fresh generator stays disabled unless the Answer check succeeds. */
        if (nack_rc < 0)
            x4_rtc_receive_setting(24, (uint64_t)(int64_t)nack_rc, 0, 0);
        (void)x4_rtc_receive_track_config(rtc->audio);
        (void)x4_rtc_receive_track_config(rtc->video);
        if (!rtc->nack_thread_started) {
#if X4_REMB_ENABLED
            remb44a_begin(rtc);
#endif
#line 2301
            int thread_rc = scePthreadCreate(&rtc->nack_thread, NULL, nack_sender, rtc, "XCloud4Nack");
            if (thread_rc) { fail(rtc, thread_rc); return thread_rc; }
            rtc->nack_thread_started = true;
        }
    }
#if X4_MIC_TRANSPORT
    microphone_answer(rtc, sdp, rc);
#endif
    return rc;
}
int x4_rtc_add_remote_candidate(X4Rtc *rtc,const char *candidate,const char *mid)
{
    if(!rtc||!candidate||!mid||strnlen(candidate,CANDIDATE_CAP)>=CANDIDATE_CAP||strnlen(mid,MID_CAP)>=MID_CAP)return -1;
    int rc=rtcAddRemoteCandidate(rtc->pc,candidate,mid);if(rc<0)fail(rtc,rc);return rc;
}
int x4_rtc_get_selected_candidate_pair(X4Rtc *rtc, char *local, size_t local_cap, char *remote, size_t remote_cap)
{
    if (!rtc || rtc->pc < 0) return -1;
#if !defined(_WIN32)
    return rtcGetSelectedCandidatePair(rtc->pc, local, (int)local_cap, remote, (int)remote_cap);
#else
    (void)local; (void)local_cap; (void)remote; (void)remote_cap;
    return -1;
#endif
}
int x4_rtc_get_rtt_ms(X4Rtc *rtc)
{
    if (!rtc || rtc->pc < 0) return -1;
#if !defined(_WIN32)
    int rtt = rtcGetRtt(rtc->pc);
    return rtt >= 0 ? rtt : -1;
#else
    return -1;
#endif
}
void x4_rtc_get_sdp_video(const X4Rtc *rtc,uint32_t *ssrc,uint32_t *pt,char *mid,size_t mid_cap)
{
    if(ssrc)*ssrc=rtc?rtc->sdp_video_ssrc:0;
    if(pt)*pt=rtc?rtc->sdp_video_pt:0;
    if(mid&&mid_cap){
        if(rtc&&rtc->sdp_video_mid[0]){
            strncpy(mid,rtc->sdp_video_mid,mid_cap-1);
            mid[mid_cap-1]='\0';
        }else mid[0]='\0';
    }
}
void x4_rtc_snapshot(X4Rtc *rtc,X4RtcSnapshot *snapshot)
{
    if(!snapshot)return;
    *snapshot=(X4RtcSnapshot){.state=X4_RTC_CLOSED};if(!rtc)return;
#if !X4_PUBLIC
    lab_emit_messages(rtc);
#endif
    snapshot->state=(X4RtcState)atomic_load(&rtc->state);snapshot->error=atomic_load(&rtc->error);
#ifdef X4_OPENORBIS
    if (snapshot->state == X4_RTC_CONNECTED) report_udp_buffers(rtc);
#endif
    snapshot->gathering_done=atomic_load(&rtc->gathering_done);
    snapshot->video_packets=atomic_load(&rtc->video_packets);snapshot->audio_packets=atomic_load(&rtc->audio_packets);
    snapshot->input_ready=atomic_load(&rtc->input_ready);snapshot->input_error=atomic_load(&rtc->input_error);
    snapshot->input_packets=atomic_load(&rtc->input_packets);snapshot->input_dropped=atomic_load(&rtc->input_dropped);
    snapshot->server_idle_sequence=atomic_load_explicit(&rtc->server_idle_sequence,memory_order_acquire);
    snapshot->server_idle_seconds=atomic_load_explicit(&rtc->server_idle_seconds,memory_order_relaxed);
    snapshot->server_closed=atomic_load_explicit(&rtc->server_closed,memory_order_acquire);
    if(snapshot->server_closed){
        snapshot->server_hr_valid=rtc->server_hr_valid;snapshot->server_hr=rtc->server_hr;
        memcpy(snapshot->server_reason,rtc->server_reason,sizeof(snapshot->server_reason));
    }
}
bool x4_rtc_keyframe_startup_ready(X4Rtc *rtc)
{
    if(!rtc)return false;
#if X4_STREAM_PROFILE
    if(x4_stream_request_resolution_alias(&rtc->request_config))
        return atomic_load_explicit(&rtc->startup_complete,memory_order_acquire);
#endif
    return true;
}
int x4_rtc_request_keyframe(X4Rtc *rtc)
{
    if(!rtc)return -1;
    if(!x4_rtc_keyframe_startup_ready(rtc))return RTC_ERR_NOT_AVAIL;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(!rtc->keyframe_limiter){
        printf("XCloud4: keyframe rechazado: falta el limitador comun\n");
        return RTC_ERR_NOT_AVAIL;
    }
    X4KeyframeLimiterSnapshot state;x4_keyframe_limiter_snapshot(rtc->keyframe_limiter,&state);
    if(state.granted<=rtc->keyframe_grant_consumed)return RTC_ERR_NOT_AVAIL;
    rtc->keyframe_grant_consumed=state.granted;
    bool control_attempted=false;int control_result=0;
#endif
    /* Native dispatch attempts/results only, never a network delivery ACK.
     * Keep the existing control message, RTCP call, cadence and return value. */
    X4Trace *trace = rtc->diagnostic_trace;
    if(rtcIsOpen(rtc->channels[1])) {
        uint64_t ordinal = ++rtc->diagnostic_pli_ordinal;
        x4_trace_record(trace, X4_TRACE_PLI_ATTEMPT, 1, ordinal, 0);
        int control_rc = send_text(rtc->channels[1],"{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}");
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        control_attempted=true;control_result=control_rc;
#endif
        x4_trace_record(trace, X4_TRACE_PLI_RESULT, 1, ordinal, (uint32_t)control_rc);
    }
    uint64_t ordinal = ++rtc->diagnostic_pli_ordinal;
    x4_trace_record(trace, X4_TRACE_PLI_ATTEMPT, 2, ordinal, 0);
#if !X4_PUBLIC && defined(X4_OPENORBIS) && X4_STREAM_PROFILE
    x4_rtc_diag_startup_pli(sceKernelGetProcessTime());
#endif
    int rc = rtcRequestKeyframe(rtc->video);
#if X4_RECOVERY43_TRANSPORT
    if(rtc->recovery43_started)x4_recovery43_note_media(rtc->nack_epoch,0,0,1,0);
#endif
    x4_trace_record(trace, X4_TRACE_PLI_RESULT, 2, ordinal, (uint32_t)rc);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    x4_keyframe_limiter_note_transport(rtc->keyframe_limiter,control_attempted,control_result,true,rc);
#endif
    return rc;
}
int x4_rtc_close_snapshot(X4Rtc *rtc,X4RtcSnapshot *final)
{
    if(!rtc){if(final)x4_rtc_snapshot(NULL,final);return 0;}
#if X4_MIC_TRANSPORT
    /* The sender is the only new owner of the audio track. Preserve the
     * entire RTC context and retry close if an in-flight native send stalls. */
    int mic_rc = microphone_close(rtc);
    if (mic_rc) return mic_rc;
#endif
#if X4_RUMBLE
    x4_rumble_disable(rtc->rumble);
#endif
    atomic_store_explicit(&rtc->nack_stop,true,memory_order_release);
    if (rtc->nack_thread_started) {
        int rc=scePthreadJoin(rtc->nack_thread,NULL);
        if (rc) { fail(rtc,rc); return rc; } /* Preserve context on join failure. */
        rtc->nack_thread_started=false;
    }
#if X4_RECOVERY43_TRANSPORT && !X4_PUBLIC && defined(X4_OPENORBIS) && X4_STREAM_PROFILE
    recovery43b_report(rtc,true);
    x4_rtc_diag_startup_report(sceKernelGetProcessTime(),true);
#endif
#if X4_REMB_ENABLED
    if (!rtc->remb.final_reported) {
        x4_rtc_diag_remb_end(rtc->nack_epoch);
#if X4_PUBLIC
        remb44c_finish_peer(rtc);
#else
        remb44a_report(rtc,true);
#endif
        rtc->remb.final_reported = true;
    }
#endif
#line 2439
    if (rtc->video>=0) {
        uint64_t deadline=sceKernelGetProcessTime()+100000u;
        int rc;
        do {
            rc=x4_rtc_close_receiver_nack(rtc->video);
            if (!rc) break;
            if (rc != -1 || sceKernelGetProcessTime()>=deadline) { fail(rtc,rc); return rc; }
            sceKernelUsleep(50); /* Shutdown owner only; incoming never waits. */
        } while (true);
    }
    atomic_store(&rtc->input_stop,true);
    if(rtc->input_thread_started) {
        int rc=scePthreadJoin(rtc->input_thread,NULL);
        if(rc) {
            if(rtc->input_join_error!=rc)printf("XCloud4: RTC input join error=0x%08x; contexto retenido\n",(unsigned)rc);
            rtc->input_join_error=rc;
            input_error(rtc,rc);
            return rc; /* No handle, callback or provider context is freed. */
        }
        rtc->input_thread_started=false;
    }
    printf("XCloud4: RTC input resumen submitted=%llu dropped=%llu error=0x%08x\n",
        (unsigned long long)atomic_load(&rtc->input_packets),
        (unsigned long long)atomic_load(&rtc->input_dropped),(unsigned)atomic_load(&rtc->input_error));
    /* Every callback carries a process-lifetime slot. Detach the heap context
     * before deleting asynchronous library objects; stale callbacks see NULL.
     * Slots are deliberately never reused (bounded to64 sessions per process). */
    lock(&callback_gate);rtc->slot->context=NULL;unlock(&callback_gate);
    for (unsigned i = 0; i < 4; ++i) if (rtc->channels[i] >= 0) {
        rtcSetMessageCallback(rtc->channels[i], NULL);
        rtcSetOpenCallback(rtc->channels[i], NULL);
#if !X4_PUBLIC
        rtcSetClosedCallback(rtc->channels[i], NULL);
        int removed = rtcDeleteDataChannel(rtc->channels[i]);
        lab_observe_close(rtc, (enum LabClosure)(LAB_CLOSE_LOCAL_CHAT + i), removed);
#else
        rtcDeleteDataChannel(rtc->channels[i]);
#endif
    }
    if(rtc->video>=0) {rtcSetMessageCallback(rtc->video,NULL);rtcDeleteTrack(rtc->video);}
    if(rtc->audio>=0) {rtcSetMessageCallback(rtc->audio,NULL);rtcDeleteTrack(rtc->audio);}
    if(rtc->pc>=0) {rtcSetLocalDescriptionCallback(rtc->pc,NULL);rtcSetLocalCandidateCallback(rtc->pc,NULL);rtcSetStateChangeCallback(rtc->pc,NULL);rtcSetIceStateChangeCallback(rtc->pc,NULL);rtcSetGatheringStateChangeCallback(rtc->pc,NULL);rtcDeletePeerConnection(rtc->pc);
#ifdef X4_OPENORBIS
        rtc->pc=-1; /* Final snapshot cannot query a deleted peer. */
#endif
    }
#if !X4_PUBLIC
    lock(&callback_gate);
    if (lab_transport_context == rtc) lab_transport_context = NULL;
    unlock(&callback_gate);
#endif
    for(;;) {lock(&callback_gate);unsigned active=rtc->slot->active;unlock(&callback_gate);if(!active)break;sceKernelUsleep(1000);}
#if X4_RECOVERY43_TRANSPORT
    if(rtc->recovery43_started){
        /* The NACK logger is joined and application callbacks are quiescent.
         * Any native handler still unwinding retains its own recovery State. */
        x4_recovery43_end(rtc->nack_epoch);
        rtc->recovery43_started=false;
    }
#endif
#if !X4_PUBLIC
    lab_emit_messages(rtc);
    lab_emit_closures(rtc);
    lab_close_summary(rtc);
#endif
#if X4_RUMBLE
    X4RumbleSnapshot vibration={0};
    x4_rumble_snapshot(rtc->rumble,&vibration);
    printf("XCloud4: vibracion reports=%llu valid=%llu accepted=%llu malformed=%llu unsupported=%llu omitted=%llu inactive=%llu\n",
        (unsigned long long)vibration.reports,(unsigned long long)vibration.valid,
        (unsigned long long)vibration.accepted,(unsigned long long)vibration.malformed,
        (unsigned long long)vibration.unsupported,(unsigned long long)vibration.contention_dropped,
        (unsigned long long)vibration.inactive_dropped);
#endif
    x4_nack_grace_inbox_unbind(rtc->grace_inbox);
    x4_rtc_receive_detach(rtc->diagnostic_trace);
    /* Every entered callback is gone, including a terminal notification
     * racing the decision to replace this peer. The owner must inspect this
     * result before opening another peer or summing per-peer counters. */
    if(final)x4_rtc_snapshot(rtc,final);
    volatile unsigned char *wipe=(unsigned char *)rtc;for(size_t i=0;i<sizeof(*rtc);++i)wipe[i]=0;
    free(rtc);
    return 0;
}
int x4_rtc_close(X4Rtc *rtc)
{return x4_rtc_close_snapshot(rtc,NULL);}

#if X4_REMB_ENABLED
static uint32_t remb44a_cap(X4StreamRequestProfile profile)
{
    switch (profile) {
    case X4_REQUEST_1080P_HQ: return 30000000u; /* Máxima */
    case X4_REQUEST_1080P_G: return 20000000u; /* Alta */
    case X4_REQUEST_1080P_G8: case X4_REQUEST_1080P_H: return 12000000u;
    default: return 10000000u; /* 720p */
    }
}

static void remb44a_begin(X4Rtc *rtc)
{
    X4RembState *state = &rtc->remb;
    state->settings = x4_stream_settings_remb();
    if (state->settings.mode != X4_REMB_OFF && state->negotiated) {
        int rc = x4_native_random(&state->sender_ssrc, sizeof(state->sender_ssrc));
        if (rc < 0) {
            state->sender_ssrc = 0;
            x4_log_printf("XCloud4: remb sin identificador local rc=%d\n", rc);
        } else if (!state->sender_ssrc) {
            state->sender_ssrc = 1;
        }
    }
    const bool mapped = x4_recovery43_snapshot(rtc->nack_epoch, &state->repair) &&
        state->repair.configured;
    const uint32_t expected = mapped ? state->repair.accepted.media_ssrc : 0;
    const unsigned pt = mapped ? state->repair.accepted.media_pt : X4_RECEIVER_NACK_VIDEO_PT;
    x4_rtc_diag_remb_begin(rtc->nack_epoch, expected, pt);
}

static uint32_t remb44a_rate(uint64_t bytes, uint64_t duration_us)
{
    if (bytes > UINT64_MAX / 8000000u) return UINT32_MAX;
    const uint64_t bps = bytes * 8000000u / duration_us;
    return bps > UINT32_MAX ? UINT32_MAX : (uint32_t)bps;
}

static void remb44a_sample(X4RembState *state, const X4RembReceiveWindow *window,
    uint64_t now, bool repair_valid, uint64_t expired)
{
    const uint64_t elapsed = now - state->sampled_us;
    state->sampled_us = now;
    state->received_bps = remb44a_rate(window->bytes, elapsed);
    if (state->received_bps > state->max_received_bps) state->max_received_bps = state->received_bps;

    /* Count final expirations once. An unresolved hole can be repaired across
     * a one-second boundary and is not loss until the existing ledger expires it. */
    const bool comparable_loss = repair_valid && state->have_expired && expired >= state->expired;
    const uint64_t lost = comparable_loss ? expired - state->expired : 0;
    const uint64_t expected = window->packets + lost;
    state->loss_ppm = expected ? (uint32_t)(lost * 1000000u / expected) : 0;
    state->expired = expired;
    state->have_expired = repair_valid;
    state->loss_valid = comparable_loss;
    if (state->loss_ppm > state->max_loss_ppm) state->max_loss_ppm = state->loss_ppm;

    state->delay_us = window->delay_us;
    state->delay_valid = window->delay_valid;
    if (state->delay_valid && (!state->have_max_delay || state->delay_us > state->max_delay_us)) {
        state->max_delay_us = state->delay_us;
        state->have_max_delay = true;
    }

    if (state->settings.mode == X4_REMB_ADAPTIVE) {
        const uint32_t before = state->estimator.target_bps;
        /* No sample cannot prove a stable interval. A bad metric can still
         * justify a decrease if the other metric is unavailable. */
        const uint32_t received = (comparable_loss && state->delay_valid) ||
            state->loss_ppm > 20000u || state->delay_us > 30000 ?
            state->received_bps : 0;
        if (x4_remb_estimator_update(&state->estimator, now, received,
                state->loss_ppm, state->delay_us)) {
            state->decreases += state->estimator.target_bps < before;
            state->increases += state->estimator.target_bps > before;
        }
    }
}

static void remb44a_tick(X4Rtc *rtc, uint64_t now)
{
    X4RembState *state = &rtc->remb;
    const bool sample_due = !state->started || (now >= state->sampled_us &&
        now - state->sampled_us >= X4_REMB_ESTIMATOR_INTERVAL_US);
    if (sample_due && atomic_load_explicit(&rtc->video_packets, memory_order_relaxed)) {
        X4RembReceiveWindow window;
        if (x4_rtc_diag_remb_take(rtc->nack_epoch, &window)) {
            const bool repair_valid = x4_recovery43_snapshot(rtc->nack_epoch, &state->repair) &&
                state->repair.configured;
            const uint64_t expired = repair_valid ? state->repair.repair.gaps_expired : 0;
            if (state->started) {
                remb44a_sample(state, &window, now, repair_valid, expired);
            } else {
                state->started = true;
                state->sampled_us = now;
                state->expired = expired;
                state->have_expired = repair_valid;
                state->video_ssrc = window.video_ssrc;
                x4_remb_estimator_init(&state->estimator, remb44a_cap(rtc->request_config.profile), now);
                if (state->sender_ssrc == state->video_ssrc) {
                    state->sender_ssrc ^= UINT32_C(0x80000000);
                    if (!state->sender_ssrc) state->sender_ssrc = 1;
                }
            }
        }
    }
    if (!state->started || state->settings.mode == X4_REMB_OFF || !state->negotiated || !state->sender_ssrc ||
        atomic_load_explicit(&rtc->state, memory_order_relaxed) != X4_RTC_CONNECTED) return;
    const uint32_t target = state->settings.mode == X4_REMB_FIXED ?
        state->settings.fixed_mbps * 1000000u : state->estimator.target_bps;
    if (!x4_remb_send_due(now, state->attempted_us, state->attempted_bps, target)) return;
    state->attempted_us = sceKernelGetProcessTime();
    state->attempted_bps = target;
    int rc = x4_rtc_send_video_remb(rtc->video, state->sender_ssrc, state->video_ssrc, target);
    if (rc != 1) {
        ++state->refused;
        return;
    }
    ++state->sent;
    state->sent_bps = target;
    if (!state->min_sent_bps || target < state->min_sent_bps) state->min_sent_bps = target;
    if (target > state->max_sent_bps) state->max_sent_bps = target;
}

#if X4_PUBLIC
static void remb44c_add_peer(X4RembSummary *total, const X4RembState *peer)
{
    total->sent += peer->sent;
    total->decreases += peer->decreases;
    total->increases += peer->increases;
    if (peer->started) total->received_bps = peer->received_bps;
    if (peer->sent) {
        total->sent_bps = peer->sent_bps;
        if (!total->min_sent_bps || peer->min_sent_bps < total->min_sent_bps)
            total->min_sent_bps = peer->min_sent_bps;
        if (peer->max_sent_bps > total->max_sent_bps)
            total->max_sent_bps = peer->max_sent_bps;
    }
    if (peer->max_loss_ppm > total->max_loss_ppm) total->max_loss_ppm = peer->max_loss_ppm;
    if (peer->have_max_delay && (!total->have_max_delay || peer->max_delay_us > total->max_delay_us)) {
        total->max_delay_us = peer->max_delay_us;
        total->have_max_delay = true;
    }
}

static void remb44c_report(const X4RembSummary *total)
{
    x4_log_printf("XCloud4: remb modo=adaptable enviados=%llu actual_mbps=%.6f min_mbps=%.6f "
        "max_mbps=%.6f bajadas=%llu subidas=%llu R_mbps=%.6f L_max_ppm=%u D_max_ms=%.3f\n",
        (unsigned long long)total->sent, total->sent_bps / 1000000.0, total->min_sent_bps / 1000000.0,
        total->max_sent_bps / 1000000.0, (unsigned long long)total->decreases,
        (unsigned long long)total->increases, total->received_bps / 1000000.0,
        total->max_loss_ppm, total->max_delay_us / 1000.0);
}

static void remb44c_periodic(const X4Rtc *rtc, uint64_t now)
{
    if (now < remb_next_report_us) return;
    remb_next_report_us = now + X4_REMB_PUBLIC_REPORT_INTERVAL_US;
    X4RembSummary total = remb_finished_peers;
    remb44c_add_peer(&total, &rtc->remb);
    remb44c_report(&total);
}

static void remb44c_finish_peer(const X4Rtc *rtc)
{
    remb44c_add_peer(&remb_finished_peers, &rtc->remb);
}

void x4_rtc_remb_finish_summary(const X4Trace *trace, uint64_t pipeline_started_us,
    X4RembSummary *out)
{
    /* If signaling never reached RTC, or a trace address has been reused,
     * the prior session's totals must not appear in this pipeline's report. */
    const bool matching = pipeline_started_us && pipeline_started_us <= remb_first_peer_us &&
        trace == remb_session_trace;
    *out = matching ? remb_finished_peers : (X4RembSummary){0};
    if (remb_final_pipeline_us != pipeline_started_us) {
        remb_final_pipeline_us = pipeline_started_us;
        remb44c_report(out);
    }
}
#else
#line 2650
static void remb44a_report(const X4Rtc *rtc, bool final)
{
    const X4RembState *state = &rtc->remb;
    const char *mode = state->settings.mode == X4_REMB_FIXED ? "fijo" :
        state->settings.mode == X4_REMB_ADAPTIVE ? "adaptable" : "no";
    x4_log_printf("XCloud4: remb fase=%s modo=%s negociado=%u actual_bps=%u min_bps=%u max_bps=%u "
        "enviados=%llu rechazados=%llu R_bps=%u R_max_bps=%u L_ppm=%u L_max_ppm=%u L_valida=%u "
        "D_us=%d D_max_us=%d D_valido=%u bajadas=%llu subidas=%llu\n",
        final ? "fin" : "periodico", mode, (unsigned)state->negotiated,
        state->sent_bps, state->min_sent_bps, state->max_sent_bps,
        (unsigned long long)state->sent, (unsigned long long)state->refused,
        state->received_bps, state->max_received_bps, state->loss_ppm, state->max_loss_ppm,
        (unsigned)state->loss_valid, state->delay_us, state->max_delay_us,
        (unsigned)state->delay_valid, (unsigned long long)state->decreases,
        (unsigned long long)state->increases);
}
#endif
#endif
