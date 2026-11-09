/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded observations for a single original-video receiver NACK experiment. */
#pragma once
#include "nack_sequence.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    X4_RECEIVER_NACK_VIDEO_PT = 102,
    X4_RECEIVER_NACK_RANGE_MAX = 64,
    X4_RECEIVER_NACK_WATCH_SLOTS = 16,
    X4_RECEIVER_NACK_SESSION_REQUEST_MAX = 64,
    X4_RECEIVER_NACK_ADAPTIVE_MAX = 512,
    X4_RECEIVER_NACK_MASK_WORDS = 8,
    X4_RECEIVER_NACK_ADAPTIVE_SLOTS = 64,
    X4_RECEIVER_NACK_TOKEN_US = 1000,
    X4_NACK_STARTUP_GRACE_MS = 250,
    X4_NACK_STARTUP_WINDOW_US = 10000000,
    X4_NACK_RETRY_HISTOGRAM = 128,
    X4_NACK_FIRST_RETURN_WINDOW = 256,
    X4_NACK_FIRST_RETURN_MINIMUM = 32
};

enum X4ReceiverNackObservationFlags {
    X4_NACK_OBS_INITIAL = 1u,
    X4_NACK_OBS_FORWARD = 2u,
    X4_NACK_OBS_BACKWARD = 4u,
    X4_NACK_OBS_DUPLICATE = 8u,
    X4_NACK_OBS_AMBIGUOUS = 16u,
    X4_NACK_OBS_BEFORE_EPOCH = 32u,
    X4_NACK_OBS_SSRC_CHANGE = 64u,
    X4_NACK_OBS_CLOSED = 128u,
    X4_NACK_OBS_WRONG_PT = 256u
};

enum X4ReceiverNackSuppression {
    X4_NACK_REQUEST_RESERVED = 0,
    X4_NACK_SUPPRESSED_DISABLED = 1,
    X4_NACK_SUPPRESSED_RANGE = 2,
    X4_NACK_SUPPRESSED_BUDGET = 3,
    X4_NACK_SUPPRESSED_SLOTS = 9
};

typedef struct {
    bool present;
    uint32_t ssrc;
    uint32_t sequence_epoch;
    uint64_t gap_id;
    /* Nonzero only when one session attempt was irrevocably reserved. */
    uint64_t request_id;
    uint64_t first_extended;
    uint64_t last_extended;
    uint64_t received_extended;
    uint64_t detected_us;
    uint64_t requested_us;
    uint32_t missing_count;
    uint32_t suppression;
} X4ReceiverNackGap;

typedef struct {
    bool present;
    bool first_observation;
    bool duplicate_observation;
    bool range_complete;
    uint32_t ssrc;
    uint32_t sequence_epoch;
    uint64_t gap_id;
    uint64_t request_id;
    uint64_t extended_sequence;
    uint64_t detected_us;
    uint64_t requested_us;
    uint64_t observed_us;
} X4ReceiverNackArrival;

typedef struct {
    uint32_t flags;
    uint32_t sequence_epoch;
    uint64_t extended_sequence;
    X4ReceiverNackGap gap;
    X4ReceiverNackArrival arrival;
    /* Immutable anchor for the current observed epoch, not rebind authority.
     * Transport epoch and anchor RTP timestamp belong to the caller. */
    X4NackSequenceTag sequence_tag;
} X4ReceiverNackObservation;

typedef struct {
    uint64_t observations;
    uint64_t wrong_payload_type;
    uint64_t observations_after_close;
    uint64_t sources_started;
    uint64_t ssrc_changes;
    uint64_t forward_packets;
    uint64_t backward_packets;
    uint64_t duplicate_packets;
    uint64_t backward_untracked;
    uint64_t before_epoch;
    uint64_t ambiguous_halfspan;
    uint64_t extended_overflow;
    uint64_t gap_ranges;
    uint64_t missing_positions;
    uint64_t requests_reserved;
    uint64_t disabled_ranges;
    uint64_t disabled_positions;
    uint64_t oversized_ranges;
    uint64_t oversized_positions;
    uint64_t budget_suppressed_ranges;
    uint64_t budget_suppressed_positions;
    uint64_t watched_ranges_evicted;
    uint64_t pending_ranges_evicted;
    uint64_t pending_positions_evicted;
    uint64_t matched_arrivals;
    uint64_t first_arrivals;
    uint64_t duplicate_arrivals;
    uint64_t completed_ranges;
    uint64_t pending_positions;
    uint64_t ssrc_retired_ranges;
    uint64_t ssrc_retired_positions;
    uint64_t ambiguity_retired_ranges;
    uint64_t ambiguity_retired_positions;
    uint64_t close_retired_ranges;
    uint64_t close_retired_positions;
    uint64_t observer_discontinuities;
    uint64_t omission_retired_ranges;
    uint64_t omission_retired_positions;
    uint64_t clock_regressions;
} X4ReceiverNackCounters;

typedef struct {
    uint64_t gap_id;
    uint64_t request_id;
    uint64_t first_extended;
    uint64_t pending[X4_RECEIVER_NACK_MASK_WORDS];
    uint64_t arrived[X4_RECEIVER_NACK_MASK_WORDS];
    uint64_t deadline_us, send_due_us;
    unsigned retry_delay_us, sends;
    uint64_t detected_us;
    uint64_t requested_us, initial_us, retry_us;
    uint32_t count;
    bool used, startup, expired, suppressed;
} X4ReceiverNackRange;

typedef struct X4ReceiverNackMetrics {
    uint64_t initial_requests, retries, requested_positions, budget_suppressed_positions;
    uint64_t fci_total, rtcp_packets, grace_us;
    uint64_t expired_no_first, expired_retry_late, expired_retry_no_response, expired_unsent;
    uint64_t retry_return_p50_us, retry_return_p95_us, startup_releases;
    uint32_t first_return_p95_us, first_return_samples;
} X4ReceiverNackMetrics;

typedef struct {
    X4ReceiverNackCounters counters;
    X4ReceiverNackMetrics metrics;
    X4ReceiverNackRange ranges[X4_RECEIVER_NACK_ADAPTIVE_SLOTS];
    uint64_t high_extended;
    uint64_t recent_seen;
    uint32_t ssrc;
    uint32_t sequence_epoch;
    uint32_t next_range, request_ordinal;
    uint64_t token_clock_us, next_send_us;
    unsigned tokens;
    unsigned grace_us, retry_delay_us;
    bool token_clock_started, legacy, grace43c, startup, policy46b;
    uint32_t retry_returns[X4_NACK_RETRY_HISTOGRAM];
    uint32_t first_returns[X4_NACK_FIRST_RETURN_WINDOW];
    unsigned first_return_cursor;
    bool enabled;
    bool bound;
    bool closed;
    X4NackSequenceTag sequence_tag;
    /* A local omission invalidates continuity but preserves the last raw
     * origin for the next authenticated epoch descriptor. Repeated omissions
     * before that next packet must not overwrite this snapshot with zero. */
    uint64_t rebase_high_extended;
    uint32_t rebase_previous_epoch;
    uint32_t rebase_previous_ssrc;
    bool rebase_pending;
} X4ReceiverNackState;

/* One context per fresh transport session, owned/serialized by exactly one
 * original-video MediaHandler. The caller supplies authenticated, fully
 * validated original RTP only. The engine neither parses RTP nor sends RTCP.
 * It does not establish handler order or authentication by itself.
 *
 * No allocations, locks, sleeps, logging, clock calls or network calls occur.
 * Legacy observes up to 16 ranges of at most 64 positions with 64 attempts
 * per session and no retries. Adaptive mode owns 64 reusable slots with up to
 * 512 positions per range. Each dispatch spends one token per position, with
 * a 512-token burst and replenishment of 1 token/ms. A gap may dispatch once
 * and retry once before its original deadline. Local send failure does not
 * refund tokens. Snapshot access needs the same external serialization.
 * SSRC change clears watched ranges without replenishing the token budget.
 * Disabled mode observes gaps and suppression but creates no pending requests.
 *
 * Sequence numbers are extended relative to the first observed wire sequence.
 * An exact 32768-position difference has no ordering information: it retires
 * watches and starts a separately numbered sequence_epoch without a request.
 * Backwards observations before that epoch are marked unknown, never wrapped
 * into a fictitious future. Normal forward 65535 -> 0 wrap remains continuous.
 * The output sequence_tag repeats the immutable first-observation anchor for
 * every valid packet in the epoch, including backwards observations. It has
 * the caller-supplied monotonic anchor time, prior epoch/raw high and reason;
 * transport_epoch and anchor_rtp_timestamp remain zero for the handler to
 * supply. INITIAL, OMISSION, SOURCE_CHANGE, AMBIGUOUS and OVERFLOW describe
 * observation state only. None authorizes a worker rebind, canonical mapping
 * or renewed request/grace deadline. Wrong-PT and closed outputs have no tag.
 *
 * A matched later arrival is an authenticated observation, NOT proof that a
 * NACK reached the server or that the packet was retransmitted rather than
 * delayed. request/dispatch/SRTCP/local-send times and reorder/AU utility must
 * be observed independently by the integration layer. */
void x4_receiver_nack_init(X4ReceiverNackState *state, bool enabled);
void x4_receiver_nack_init_mode(X4ReceiverNackState *state, bool enabled, bool legacy);
void x4_receiver_nack_init_policy(X4ReceiverNackState *state, bool enabled,
    bool legacy, bool grace43c);
void x4_receiver_nack_init_46b(X4ReceiverNackState *state, bool enabled);
void x4_receiver_nack_set_rtt(X4ReceiverNackState *state, int rtt_p50_ms);
void x4_receiver_nack_refresh(X4ReceiverNackState *state, int rtt_p50_ms,
    uint64_t now_us, bool startup);
void x4_receiver_nack_snapshot(const X4ReceiverNackState *state, X4ReceiverNackMetrics *out);

/* Keep the same whole-ms deadline in the reorder and the NACK owner. */
static inline unsigned x4_nack_retry_delay_us(int rtt_ms)
{
    if (rtt_ms < 14) return 20000;
    if (rtt_ms > 1000) return 1500000;
    return (unsigned)rtt_ms * 1500u;
}

static inline unsigned x4_nack_grace_ms(int rtt_ms, bool grace43c)
{
    if (rtt_ms < 0) return grace43c ? 80u : 60u;
    if (rtt_ms > 1000) return grace43c ? 120u : 100u;
    unsigned rtt = (unsigned)rtt_ms;
    unsigned grace = 2u * rtt + 15u;
    if (grace43c) {
        unsigned for_retry = (x4_nack_retry_delay_us(rtt_ms) + 999u) / 1000u + rtt + 30u;
        if (grace < for_retry) grace = for_retry;
        if (grace < 60u) return 60u;
        return grace > 120u ? 120u : grace;
    }
    if (grace < 45u) return 45u;
    return grace > 100u ? 100u : grace;
}

static inline unsigned x4_nack_retry46b_ms(int rtt_ms)
{
    unsigned rtt = rtt_ms < 0 ? 0u : rtt_ms > 1000 ? 1000u : (unsigned)rtt_ms;
    unsigned delay = 2u * rtt + 10u;
    return delay < 30u ? 30u : delay;
}

static inline unsigned x4_nack_grace46b_ms(int rtt_ms, uint32_t p95_us,
    unsigned samples)
{
    unsigned rtt = rtt_ms < 0 ? 0u : rtt_ms > 1000 ? 1000u : (unsigned)rtt_ms;
    unsigned grace = 2u * rtt + 15u;
    unsigned for_retry = x4_nack_retry46b_ms(rtt_ms) + rtt + 30u;
    if (grace < for_retry) grace = for_retry;
    if (samples >= X4_NACK_FIRST_RETURN_MINIMUM) {
        unsigned observed = p95_us / 1000u + (p95_us % 1000u != 0u) + 15u;
        if (grace < observed) grace = observed;
    }
    if (grace < 80u) return 80u;
    return grace > 150u ? 150u : grace;
}

/* Only the serialized receiver publishes a changed window for the worker. */
void x4_rtc_diag_first_return_publish(uint32_t p95_us, unsigned samples);
unsigned x4_rtc_diag_video_grace46b_ms(void);

/* Adaptive owner-only. A false result may still move next_send_us; zero
 * denotes no work. The returned selection is copied before unlocking. */
bool x4_receiver_nack_next(X4ReceiverNackState *state, uint64_t now_us,
    X4ReceiverNackGap *gap, uint64_t pending[X4_RECEIVER_NACK_MASK_WORDS], bool *retry);
/* T46b anchors its retry to local send entry after constructing the RTCP
 * packet. A selection without a send attempt cannot open the retry window. */
void x4_receiver_nack_sent_first(X4ReceiverNackState *state, uint64_t request_id,
    uint64_t dispatch_us);
void x4_receiver_nack_close(X4ReceiverNackState *state);
/* The owner calls this after a locally omitted observation, while serialized.
 * It retires watches/cancels queued positions and discards continuity so the
 * next observed packet cannot manufacture a network gap from that omission.
 * That next packet starts a new sequence_epoch. The 64-attempt session budget
 * and all cumulative counters survive. Integration separately counts the
 * number of omitted packets; one call may cover more than one omission. */
void x4_receiver_nack_discontinuity(X4ReceiverNackState *state);
void x4_receiver_nack_observe(X4ReceiverNackState *state, uint32_t ssrc,
    uint8_t payload_type, uint16_t sequence, uint64_t monotonic_us,
    X4ReceiverNackObservation *observation);
void x4_receiver_nack_observe_return(X4ReceiverNackState *state, uint32_t ssrc,
    uint8_t payload_type, uint16_t sequence, uint64_t monotonic_us,
    bool measure_first_return, X4ReceiverNackObservation *observation);
/* Legacy only; read under the same external serialization. The original
 * request reservation remains spent even if every position was canceled
 * before dispatch. Zero means closed/retired/evicted/unknown/complete; no
 * current position may be requested. A nonzero bitmap addresses positions
 * first_extended + bit index and permits one later dispatcher to omit packets
 * already observed while its request waited. No retry is thereby authorized. */
uint64_t x4_receiver_nack_pending(const X4ReceiverNackState *state,
    uint64_t request_id, uint64_t *first_extended);

#ifdef __cplusplus
}
#endif
