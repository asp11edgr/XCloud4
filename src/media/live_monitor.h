/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent, optional diagnostics. This module never controls playback. */
#pragma once
#include "video_ingress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct X4Monitor X4Monitor;
enum {
    X4_MON_ACTOR_WORKER, X4_MON_ACTOR_MAIN, X4_MON_ACTOR_DISPLAY,
    X4_MON_ACTOR_HELPER0, X4_MON_ACTOR_HELPER1, X4_MON_ACTOR_HELPER2,
    X4_MON_ACTOR_RTC, X4_MON_ACTOR_SESSION, X4_MON_ACTORS
};
enum {
    X4_MON_PHASE_UNKNOWN, X4_MON_PHASE_WAIT_DATA, X4_MON_PHASE_ASSEMBLE,
    X4_MON_PHASE_DECODE, X4_MON_PHASE_COPY, X4_MON_PHASE_CONVERT,
    X4_MON_PHASE_WAIT_SURFACE, X4_MON_PHASE_COPY_WAIT, X4_MON_PHASE_DRAW,
    X4_MON_PHASE_FLIP_SUBMIT, X4_MON_PHASE_FLIP_WAIT, X4_MON_PHASE_MAILBOX,
    X4_MON_PHASE_STARTUP, X4_MON_PHASE_STOPPING, X4_MON_PHASE_MAIN_POLL,
    X4_MON_PHASE_MAIN_STATUS, X4_MON_PHASE_MAIN_IDLE
};
enum { X4_MON_EPOCH_UNKNOWN, X4_MON_EPOCH_LOADING,
       X4_MON_EPOCH_STABLE, X4_MON_EPOCH_TRANSITION };
enum { X4_MON_RX_VIDEO, X4_MON_RX_AUDIO, X4_MON_RX_OTHER, X4_MON_RX_KINDS };
enum {
    X4_MON_Q_VALID = 1, X4_MON_Q_EMPTY = 2, X4_MON_Q_UNPUBLISHED = 4,
    X4_MON_Q_UNSTABLE = 8, X4_MON_Q_UNBOUND = 16, X4_MON_Q_CLOCK = 32,
    X4_MON_Q_RANGE = 64
};
enum {
    X4_MON_S_CADENCE_STABLE = 1, X4_MON_S_RX_STABLE = 2,
    X4_MON_S_QUEUE_STABLE = 4, X4_MON_S_HISTORY_STABLE = 8,
    X4_MON_S_ACTOR_SHIFT = 8
};
enum {
    X4_MON_EVENT_SAMPLER_START = 0x610, X4_MON_EVENT_SAMPLER_SAMPLE,
    X4_MON_EVENT_GAP_BEGIN, X4_MON_EVENT_GAP_END, X4_MON_EVENT_EPOCH,
    X4_MON_EVENT_ACTIVE, X4_MON_EVENT_SAMPLER_STOP,
    X4_MON_EVENT_WINDOW_CLOSE, X4_MON_EVENT_DISABLED, X4_MON_EVENT_PHASE_CHANGE,
    X4_MON_EVENT_RX_OBSERVATION
};
enum {
    X4_MON_PERIOD_US = 100000, X4_MON_GAP_US = 100000,
    X4_MON_SAMPLE_CAP = 12000, X4_MON_CADENCE_CAP = 65536,
    X4_MON_HISTORY_CAP = 8192, X4_MON_WINDOW_CAP = 8,
    X4_MON_WINDOW_RECORD_CAP = 4096, X4_MON_EVENT_CAP = 512,
    X4_MON_RESET_CAP = 64, X4_MON_RX_SOURCE_CAP = 16,
    X4_MON_HIST_CAP = 32, X4_MON_HEADER_WORDS = 128,
    X4_MON_SAMPLE_WORDS = 80, X4_MON_WINDOW_HEADER_WORDS = 16,
    X4_MON_RX_WORDS = 16, X4_MON_CONFIG_WORDS = 16, X4_MON_EPOCH_CAP = 64
};
/* Stage names describe observations, not proof of arrival/delivery over the
 * network. The bridge may leave inaccessible stages entirely unobserved. */
enum {
    X4_MON_T_DATAGRAM, X4_MON_T_ICE_DELIVER, X4_MON_T_DTLS_QUEUE_ACCEPT,
    X4_MON_T_DTLS_QUEUE_POP, X4_MON_T_SRTP_INPUT, X4_MON_T_SRTP_VALID,
    X4_MON_T_PEER_DISPATCH, X4_MON_T_TRACK_INCOMING, X4_MON_T_TRACK_QUEUED,
    X4_MON_T_TRACK_DELIVERED, X4_MON_T_APP_CALLBACK, X4_MON_T_RTX_NORMALIZED,
    X4_MON_T_TCP_FRAMED, X4_MON_T_PEER_HANDLER_OUT, X4_MON_T_RTCP_VALID,
    X4_MON_T_TURN_DECAP, X4_MON_T_STAGES,
    X4_MON_T_RESERVED = X4_MON_T_TURN_DECAP,
    X4_MON_T_TRACK_QUEUE_ATTEMPT = X4_MON_T_TRACK_QUEUED
};
enum {
    X4_MON_CR_SEQUENCE_GAP = 1, X4_MON_CR_LATE_POSITION, X4_MON_CR_REJECT,
    X4_MON_CR_RECOVERY_BEGIN, X4_MON_CR_RECOVERY_END, X4_MON_CR_PLI_LOCAL,
    X4_MON_CR_PLI_ATTEMPT, X4_MON_CR_PLI_RESULT, X4_MON_CR_AU_FILTER,
    X4_MON_CR_IDR_SEEN, X4_MON_CR_AU_PARAMETERS, X4_MON_CR_PRESENT_GAP_BEGIN,
    X4_MON_CR_PRESENT_GAP_END, X4_MON_CR_SEQUENCE_AMBIGUOUS,
    X4_MON_CR_AU_DISCARD, X4_MON_CR_TRANSPORT_DELAY, X4_MON_CR_RTC_SETTING,
    X4_MON_CR_NACK_DETECT, X4_MON_CR_NACK_REQUEST, X4_MON_CR_NACK_DISPATCH,
    X4_MON_CR_NACK_SRTCP_BEGIN, X4_MON_CR_NACK_SRTCP_RESULT,
    X4_MON_CR_NACK_LOCAL_SEND_RESULT, X4_MON_CR_NACK_AUTH_RETURN,
    X4_MON_CR_NACK_REORDER_RESULT, X4_MON_CR_NACK_AU_CONTRIBUTION,
    X4_MON_CR_NACK_STATE, X4_MON_CR_NACK_SUPPRESS, X4_MON_CR_NACK_COUNTER,
    X4_MON_CR_NACK_DISPATCH_MASK, X4_MON_CR_NACK_GRACE
};
enum {
    X4_MON_CR_CAP = 4096, X4_MON_CR_EVENT_CAP = 32,
    X4_MON_T_SOURCE_CAP = 16, X4_MON_T_SOURCE_WORDS = 24,
    X4_MON_T_COUNTER_WORDS = 16, X4_MON_CR_HEADER_WORDS = 32,
    X4_MON_T_RANGE_CAP = 16, X4_MON_WINDOW_PRE_CAP = 512,
    X4_MON_WINDOW_BODY_CAP = 3072, X4_MON_WINDOW_END_CAP = 1024
};
/* Numeric only: time, identity, type/stage/flags, source metadata, three
 * event-specific numeric fields, and independent critical ordinal. */
typedef struct { uint64_t w[8]; } X4MonitorCritical;
_Static_assert(sizeof(X4MonitorCritical) == 64, "critical diagnostic ABI");
_Static_assert(X4_MON_CR_NACK_DETECT == 18 && X4_MON_CR_NACK_COUNTER == 29 &&
    X4_MON_CR_NACK_DISPATCH_MASK == 30 &&
    X4_MON_CR_NACK_GRACE == 31 &&
    (int)X4_MON_CR_NACK_GRACE < (int)X4_MON_CR_EVENT_CAP,
    "NACK uses existing v2 event slots");
/* NACK source: bit63 | (original payload type102 <<32) | SSRC. Request
 * identity: session epoch high32 and session request ordinal low32. The
 * extended sequence is source/epoch local, not a cross-source packet ID.
 * DETECT: identity gap, a first missing, b last missing (inclusive), c received.
 * REQUEST: a first, b last, c detected time. DISPATCH: c request time.
 * SRTCP BEGIN/RESULT and LOCAL SEND: a signed int64 status, b bytes,
 * c dispatch time; local-send status is0 failed,1 success,-1 unavailable.
 * AUTH RETURN: a extended sequence,b RTP timestamp,c request time; flags
 * first1,duplicate2,range-complete4. Later arrival need not be retransmission.
 * REORDER RESULT: a extended sequence,b existing decision,c request ordinal.
 * AU CONTRIBUTION: a AU ordinal,b stage0 in-progress,1 structural,
 * 2 accepted feed,3 recovery filter,4 discarded,c extended sequence.
 * For stage4, flags1 is shutdown discard; it is not recovery completion.
 * STATE: a enabled,b session request cap,c(watch slots<<32)|range cap.
 * SUPPRESS: a reason,b positions,c gap ordinal. COUNTER: a counter ID,
 * b exact snapshot,c session epoch. Repeated snapshots must not be summed.
 * DISPATCH MASK: a first extended,b actual pending positions bitmap64,
 * c serialized selection time. Bit0 is a; bits outside the REQUEST range
 * are invalid. Selection precedes transport dispatch and is not atomic send.
 * STATE is configuration/lifecycle only; flags8 marks closing, not disabled.
 * Local status success establishes neither delivery nor a presented image. */
/* GRACE31 is a separately session-bound functional policy's observation.
 * stage1 request(first,mask,detection);2 outcome(attempt,local-return,mask),
 * flags=(local-result+2)&255 | cancel-reason<<8;3 offer(deadline,grant,
 * occupancy<<32|result);4 release(deadline,accepted-mask,occupancy),flagsreason;
 * 5 callback(seq,AUTH,callback),flagsadmission;6 queue-pop(seq,callback,AUTH);
 * 7 worker decision(seq,expected<<32|preoccupancy,deadline),flagsreorder;
 * 8 elapsed Decode/copy/convert aggregates from worker offer processing to
 * release, not the whole detection interval or an input/output association;
 * 9 queue(oldest-age,depth,max returned dwell);10 counter(id,value,0);
 * 11 hold-begin(expected<<32|occupancy,deadline,furthest-offset);
 * 12 actual forced abandonment(expected<<32|lost,preoccupancy,span),flagsreason.
 * t_us is the event observation, not an inferred remote acknowledgement.
 * Critical-ring attempts/omissions apply independently to all subkinds. */
typedef struct {
    uint64_t t_us, a, b;
    uint32_t ordinal;
    uint16_t event, flags;
} X4MonitorRecord;
typedef struct { uint64_t end_us, generation, interval_us, epochs; } X4MonitorCadence;
typedef struct { uint64_t time_us, ordinal, epoch, active; } X4MonitorEpoch;
typedef struct { uint64_t w[X4_MON_SAMPLE_WORDS]; } X4MonitorSample;
typedef struct { uint64_t head, tail, depth, oldest_us, flags; } X4MonitorQueue;
/* Fixed numeric settings; root stamps build identity separately. */
typedef struct {
    uint64_t width, height, max_fps, bitrate_kbps, readers, budget_us,
        decode_limit, queue_capacity, reorder_capacity, reorder_depth,
        reorder_wait_us, h264_profile, max_fs, max_mbps, reserved0, reserved1;
} X4MonitorConfig;
_Static_assert(sizeof(X4MonitorRecord) == 32, "monitor record ABI");
_Static_assert(sizeof(X4MonitorCadence) == 32, "monitor cadence ABI");
_Static_assert(sizeof(X4MonitorSample) == 640, "monitor sample ABI");
_Static_assert(sizeof(X4MonitorEpoch) == 32, "monitor epoch ABI");
_Static_assert(sizeof(X4MonitorConfig) == 128, "monitor config ABI");

X4Monitor *x4_monitor_create(uint64_t session, unsigned readers);
uint64_t x4_monitor_now_us(void);
/* Bind/configure once before start and before any producers. Queue storage
 * remains immutable/alive until successful monitor stop/join. */
bool x4_monitor_bind_queue(X4Monitor *, const X4VideoIngress *);
bool x4_monitor_config(X4Monitor *, const X4MonitorConfig *);
int x4_monitor_start(X4Monitor *); /* 0 started; 1 optional diagnostic disabled */
int x4_monitor_stop(X4Monitor *);  /* 0 joined/disabled; negative: retain ALL storage */
bool x4_monitor_stopped(const X4Monitor *);
void x4_monitor_record(X4Monitor *, uint64_t time, uint16_t event,
    uint16_t flags, uint64_t a, uint64_t b);
void x4_monitor_rx(X4Monitor *, unsigned kind, bool valid, uint32_t ssrc,
    uint16_t seq, size_t bytes, uint64_t time);
/* Bounded try-only observations; no allocation, wait or I/O. A zero preceding
 * timestamp means inter-stage scheduling delay is unavailable. Flags are
 * provenance metadata only and never control RTP/RTX processing. */
void x4_monitor_transport_packet(X4Monitor *, unsigned stage, bool metadata_valid,
    uint32_t ssrc, uint16_t seq, unsigned payload_type, size_t bytes,
    uint64_t time, uint64_t prior_stage_time, uint32_t flags);
void x4_monitor_transport_reject(X4Monitor *, unsigned stage, unsigned reason,
    int result, bool metadata_valid, uint32_t ssrc, uint16_t seq, uint64_t time);
void x4_monitor_critical(X4Monitor *, uint64_t time, unsigned event,
    unsigned stage, uint32_t flags, uint64_t identity, uint64_t source,
    uint64_t a, uint64_t b, uint64_t c);
/* Optional supplied tuple is explicitly independent, not a coherent queue
 * snapshot. Direct binding is preferred; sample flags expose its witnesses. */
void x4_monitor_queue(X4Monitor *, uint32_t depth, uint64_t oldest_us, bool valid);
/* Each actor has ONE owning writer. Reader checks version once, never spins.
 * begin_us==0 uses the common monotonic clock. */
void x4_monitor_state(X4Monitor *, unsigned actor, unsigned phase, uint64_t begin_us);
/* Main/session writer only; no automatic stable-gameplay inference. */
void x4_monitor_epoch(X4Monitor *, unsigned actor, unsigned epoch);
void x4_monitor_active(X4Monitor *, bool active, uint64_t time);
void x4_monitor_present(X4Monitor *, uint64_t generation, bool fresh, uint64_t time);
/* Call only after monitor joined AND every external producer is quiescent. */
void x4_monitor_end(X4Monitor *);
int x4_monitor_dump(X4Monitor *, const char *base_path); /* appends .progress.bin; exclusive */
bool x4_monitor_free(X4Monitor *); /* false retains unless ended and joined */
void x4_monitor_observe_queue(const X4VideoIngress *, uint64_t time, X4MonitorQueue *);

#ifdef X4_MONITOR_HOST_TEST
/* Host harness supplies clock/sleep and pthread-equivalent create/join.
 * No native playback or SDK runtime is required by the diagnostic module. */
typedef uintptr_t X4MonitorThread;
uint64_t x4_monitor_host_now_us(void);
void x4_monitor_host_sleep_us(uint64_t);
int x4_monitor_host_create(X4MonitorThread *, void *(*)(void *), void *);
int x4_monitor_host_join(X4MonitorThread);
#endif

#ifdef X4_MONITOR_TEST_HOOKS
enum { X4_MON_TEST_BEFORE_SAMPLE = 1, X4_MON_TEST_AFTER_SAMPLE,
       X4_MON_TEST_QUEUE_WITNESS, X4_MON_TEST_EVENT_GATE };
void x4_monitor_test_hook(unsigned point, void *context, uint64_t a, uint64_t b);
#endif
