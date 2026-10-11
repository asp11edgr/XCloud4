/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    X4_NACK_GRACE_US = 45000,
    X4_NACK_GRACE_WINDOW = 128,
    X4_NACK_GRACE_ACTIVE_MAX = 16,
    X4_NACK_GRACE_REQUEST_MAX = 64
};

enum X4NackGraceRelease {
    X4_GRACE_RELEASE_NONE,
    X4_GRACE_RELEASE_ACCEPTED,
    X4_GRACE_RELEASE_DEADLINE,
    X4_GRACE_RELEASE_CAPACITY,
    X4_GRACE_RELEASE_PASSED,
    X4_GRACE_RELEASE_SOURCE,
    X4_GRACE_RELEASE_CLOSE,
    X4_GRACE_RELEASE_TRANSPORT,
    X4_GRACE_RELEASE_CLOCK,
    X4_GRACE_RELEASE_EXPLICIT
};
enum X4NackGraceOffer {
    X4_GRACE_OFFER_ADMITTED,
    X4_GRACE_OFFER_DISABLED,
    X4_GRACE_OFFER_STALE_SESSION,
    X4_GRACE_OFFER_CLOSED,
    X4_GRACE_OFFER_INVALID,
    X4_GRACE_OFFER_DUPLICATE,
    X4_GRACE_OFFER_STALE_SOURCE,
    X4_GRACE_OFFER_EXPIRED,
    X4_GRACE_OFFER_ACTIVE_CAP,
    X4_GRACE_OFFER_OVERLAP,
    X4_GRACE_OFFER_CLOCK,
    X4_GRACE_OFFER_DEADLINE_OVERFLOW
};
enum X4NackGraceDecisionReason {
    X4_GRACE_NONE,
    X4_GRACE_HOLD,
    X4_GRACE_DEADLINE,
    X4_GRACE_CAPACITY,
    X4_GRACE_CLOCK,
    X4_GRACE_CLOSED
};

typedef struct {
    uint64_t session_id, identity, first_extended, missing_mask, detected_us;
    uint32_t ssrc, sequence_epoch;
} X4NackGraceRequest;

/* Bits identify request ordinals1..64. A release is not AU/decoder utility.
 * Read the retained range's release_reason and clocks while worker-owned. */
typedef struct {
    uint64_t completed, deadline, capacity, passed, invalidated;
} X4NackGraceEffects;

typedef struct {
    uint64_t identity, first_extended, original_mask, granted_mask;
    uint64_t pending_mask, accepted_mask, detected_us, deadline_us;
    uint32_t ssrc, sequence_epoch, release_reason;
    bool seen, active;
} X4NackGraceRange;

typedef struct {
    uint64_t offers, admitted, duplicate_offers, invalid_offers;
    uint64_t stale_session_offers, stale_source_offers, disabled_offers;
    uint64_t closed_offers, expired_offers, active_cap_offers;
    uint64_t overlap_positions, overlap_only_offers, deadline_overflow_offers;
    uint64_t accepted_positions, passed_positions, completed_ranges;
    uint64_t deadline_ranges, capacity_ranges, passed_ranges;
    uint64_t source_retired_ranges, close_retired_ranges;
    uint64_t transport_retired_ranges, explicit_retired_ranges;
    uint64_t clock_regressions, clock_retired_ranges;
    uint64_t pending_positions, active_peak, hold_queries;
} X4NackGraceCounters;

typedef struct {
    X4NackGraceRange ranges[X4_NACK_GRACE_REQUEST_MAX];
    X4NackGraceCounters counters;
    uint64_t session_id, seen_request_ids, last_now_us;
    uint32_t ssrc, sequence_epoch, active_count;
    bool bound, enabled, closed;
} X4NackGraceState;

typedef struct {
    enum X4NackGraceDecisionReason reason;
    uint64_t identity, deadline_us, pending_mask;
    X4NackGraceEffects effects;
} X4NackGraceDecision;

/* Pure bounded policy, exclusively owned by the video worker. No atomics,
 * allocation, locks, I/O, clock calls or callbacks. The integration owns a
 * separately bounded, session-safe control inbox; this is NOT an inbox.
 *
 * init() starts a fresh session. identity is an opaque nonzero request ID
 * with its global session attempt ordinal1..64 in the low32bits. The upper
 * bits are NOT assumed to be sequence_epoch; the real bridge uses a separate
 * transport epoch. Every request supplies the matching session_id explicitly.
 * bind_source() is mandatory and driven by accepted worker track/epoch
 * changes, never an unsolicited control record. Unbound/old-source controls
 * fail closed; offer() cannot change the accepted worker source.
 *
 * A granted position has a fixed detected_us+45000 deadline. It never resets
 * when expected advances, a duplicate arrives, another overlapping request
 * is offered, AUTH finishes, or dispatch occurs. All64 request histories are
 * retained until init(), even after completion, so overlap cannot rearm old
 * positions. At most16 ranges may be active; saturation suppresses grace.
 *
 * AUTH is deliberately absent from this API. accepted_wire() is called only
 * after a packet was actually STORED in the worker reorder. A JUMP requires
 * invalidating grace together with the worker's track/reorder reset instead.
 * After admitting a delayed control, reconcile matching slots already STORED
 * in the current reorder with accepted_wire(); a callback is not acceptance.
 * The sequence_epoch argument must belong to that worker/source, not simply
 * be copied from an untrusted/stale packet. No payload or decoder PTS is used.
 *
 * query() receives the REAL monotonic time even if the existing reorder call
 * uses now=0 to suppress ordinary expiry. Call it also when no packets arrive,
 * so deadline expiry cannot depend on new reception. HOLD applies only to a
 * currently pending expected position and supersedes BOTH depth32 and25ms
 * expiry for that range until its deadline; ordinary gaps keep their policy.
 *
 * furthest_forward_offset is the maximum modular forward offset relative to
 * expected among buffered packets AND the proposed next insert. An offset
 * >=128, or buffered>=128, releases the matching grace before a window jump
 * or aliasing collision. UINT32_MAX means unknown: fail closed. Query before
 * such an insert as well as before next(). Never enlarge the128-slot reorder.
 * This policy does not promise that128 slots can cover the observed return
 * delay. Integration must diagnose fallback/headroom and AU utility.
 *
 * All now_us values must share one monotonic clock. A regression retires all
 * active ranges; deadline arithmetic overflow rejects the offer. Clocks do
 * not establish NACK server receipt, retransmission causality or picture age.
 * Fields/counters may be read only by the same worker or after its join. */
void x4_nack_grace_init(X4NackGraceState *state, uint64_t session_id, bool enabled);
X4NackGraceEffects x4_nack_grace_bind_source(X4NackGraceState *state,
    uint32_t ssrc, uint32_t sequence_epoch);
int x4_nack_grace_offer(X4NackGraceState *state, const X4NackGraceRequest *request,
    uint64_t now_us, X4NackGraceEffects *effects);
X4NackGraceEffects x4_nack_grace_accepted_wire(X4NackGraceState *state,
    uint32_t ssrc, uint32_t sequence_epoch, uint16_t sequence, uint64_t now_us);
X4NackGraceDecision x4_nack_grace_query(X4NackGraceState *state,
    uint16_t expected, uint32_t buffered, uint32_t furthest_forward_offset,
    uint64_t now_us);
/* Only expiration/capacity release forces abandonment. A transport failure,
 * omission or source/clock invalidation removes grace and restores32/25. */
bool x4_nack_grace_release_forces_gap(enum X4NackGraceRelease reason);
/* After query() with the real clock, return the maximum safe modular advance
 * before the nearest still-active pending position. The endpoint remains
 * protected; zero prohibits advance. No clock or mutation occurs here.
 * Use for BOTH ordinary-ready and explicit-forced advances, not only force.
 * Without active grace, default_limit is returned unchanged. */
uint32_t x4_nack_grace_advance_limit(const X4NackGraceState *state,
    uint16_t expected, uint32_t default_limit);
X4NackGraceEffects x4_nack_grace_cancel(X4NackGraceState *state,
    uint64_t identity, enum X4NackGraceRelease reason);
/* Close is irreversible until init(). Use EXPLICIT for an inbox omission;
 * CLOSE for normal close. Other release reasons are normalized to EXPLICIT.
 * Thus a saturated/unknown control history disables grace for the session. */
X4NackGraceEffects x4_nack_grace_close_reason(X4NackGraceState *state,
    enum X4NackGraceRelease reason);

#ifdef __cplusplus
}
#endif
