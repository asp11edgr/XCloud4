/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded observations for a single original-video receiver NACK experiment. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    X4_RECEIVER_NACK_VIDEO_PT = 102,
    X4_RECEIVER_NACK_RANGE_MAX = 64,
    X4_RECEIVER_NACK_WATCH_SLOTS = 16,
    X4_RECEIVER_NACK_SESSION_REQUEST_MAX = 64
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
    X4_NACK_SUPPRESSED_BUDGET = 3
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
    uint64_t pending;
    uint64_t arrived;
    uint64_t detected_us;
    uint64_t requested_us;
    uint32_t count;
    bool used;
} X4ReceiverNackRange;

typedef struct {
    X4ReceiverNackCounters counters;
    X4ReceiverNackRange ranges[X4_RECEIVER_NACK_WATCH_SLOTS];
    uint64_t high_extended;
    uint64_t recent_seen;
    uint32_t ssrc;
    uint32_t sequence_epoch;
    uint32_t next_range;
    bool enabled;
    bool bound;
    bool closed;
} X4ReceiverNackState;

/* One context per fresh transport session, owned/serialized by exactly one
 * original-video MediaHandler. The caller supplies authenticated, fully
 * validated original RTP only. The engine neither parses RTP nor sends RTCP.
 * It does not establish handler order or authentication by itself.
 *
 * No allocations, locks, sleeps, logging, clock calls or network calls occur.
 * observe() checks at most 16 ranges of at most 64 positions; snapshot access
 * to counters is safe only under the same external ownership/serialization.
 *
 * A request_id reserves one of 64 total session attempts BEFORE transport.
 * There are no retries. Local send failure must not refund that reservation.
 * SSRC change clears watched ranges but does not reset the session budget.
 * close() retires pending positions; only init() starts a fresh session/budget.
 * Disabled mode observes gaps and suppression but creates no pending requests.
 *
 * Sequence numbers are extended relative to the first observed wire sequence.
 * An exact 32768-position difference has no ordering information: it retires
 * watches and starts a separately numbered sequence_epoch without a request.
 * Backwards observations before that epoch are marked unknown, never wrapped
 * into a fictitious future. Normal forward 65535 -> 0 wrap remains continuous.
 *
 * A matched later arrival is an authenticated observation, NOT proof that a
 * NACK reached the server or that the packet was retransmitted rather than
 * delayed. request/dispatch/SRTCP/local-send times and reorder/AU utility must
 * be observed independently by the integration layer. */
void x4_receiver_nack_init(X4ReceiverNackState *state, bool enabled);
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
/* Read only while holding the same external serialization. The original
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
