/* SPDX-License-Identifier: GPL-3.0-only */
#include "nack_grace.h"
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(X4NackGraceState) <= 8192, "bounded worker grace state");

static unsigned population(uint64_t mask)
{
    unsigned n = 0;
    while (mask) { mask &= mask - 1; ++n; }
    return n;
}
static uint64_t bit_for(const X4NackGraceRange *range)
{ return UINT64_C(1) << ((uint32_t)range->identity - 1); }

static void retire(X4NackGraceState *state, X4NackGraceRange *range,
    enum X4NackGraceRelease reason, X4NackGraceEffects *effects)
{
    if (!range->active) return;
    uint64_t bit = bit_for(range);
    state->counters.pending_positions -= population(range->pending_mask);
    range->pending_mask = 0;
    range->active = false;
    range->release_reason = reason;
    --state->active_count;
    switch (reason) {
    case X4_GRACE_RELEASE_ACCEPTED:
        ++state->counters.completed_ranges; effects->completed |= bit; break;
    case X4_GRACE_RELEASE_DEADLINE:
        ++state->counters.deadline_ranges; effects->deadline |= bit; break;
    case X4_GRACE_RELEASE_CAPACITY:
        ++state->counters.capacity_ranges; effects->capacity |= bit; break;
    case X4_GRACE_RELEASE_PASSED:
        ++state->counters.passed_ranges; effects->passed |= bit; break;
    case X4_GRACE_RELEASE_SOURCE:
        ++state->counters.source_retired_ranges; effects->invalidated |= bit; break;
    case X4_GRACE_RELEASE_CLOSE:
        ++state->counters.close_retired_ranges; effects->invalidated |= bit; break;
    case X4_GRACE_RELEASE_TRANSPORT:
        ++state->counters.transport_retired_ranges; effects->invalidated |= bit; break;
    case X4_GRACE_RELEASE_CLOCK:
        ++state->counters.clock_retired_ranges; effects->invalidated |= bit; break;
    default:
        ++state->counters.explicit_retired_ranges; effects->invalidated |= bit; break;
    }
}
static void retire_all(X4NackGraceState *state, enum X4NackGraceRelease reason,
    X4NackGraceEffects *effects)
{
    if (!state->active_count) return;
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i)
        retire(state, &state->ranges[i], reason, effects);
}
static bool time_ok(X4NackGraceState *state, uint64_t now,
    X4NackGraceEffects *effects)
{
    if (now < state->last_now_us) {
        ++state->counters.clock_regressions;
        retire_all(state, X4_GRACE_RELEASE_CLOCK, effects);
        return false;
    }
    state->last_now_us = now;
    return true;
}
static void expire(X4NackGraceState *state, uint64_t now,
    X4NackGraceEffects *effects)
{
    if (!state->active_count) return;
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
        X4NackGraceRange *range = &state->ranges[i];
        if (range->active && now >= range->deadline_us)
            retire(state, range, X4_GRACE_RELEASE_DEADLINE, effects);
    }
}
static bool pending_at(const X4NackGraceRange *range, uint16_t sequence)
{
    uint16_t offset = (uint16_t)(sequence - (uint16_t)range->first_extended);
    return offset < 64 && (range->pending_mask & (UINT64_C(1) << offset));
}

void x4_nack_grace_init(X4NackGraceState *state, uint64_t session_id, bool enabled)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->session_id = session_id;
    state->enabled = enabled;
}

X4NackGraceEffects x4_nack_grace_bind_source(X4NackGraceState *state,
    uint32_t ssrc, uint32_t sequence_epoch)
{
    X4NackGraceEffects effects = {0};
    if (!state || state->closed || !sequence_epoch) return effects;
    if (state->bound && (state->ssrc != ssrc || state->sequence_epoch != sequence_epoch))
        retire_all(state, X4_GRACE_RELEASE_SOURCE, &effects);
    state->ssrc = ssrc; state->sequence_epoch = sequence_epoch; state->bound = true;
    return effects;
}

X4NackGraceEffects x4_nack_grace_unbind_source(X4NackGraceState *state)
{
    X4NackGraceEffects effects = {0};
    if (!state) return effects;
    retire_all(state, X4_GRACE_RELEASE_SOURCE, &effects);
    state->bound = false;
    return effects;
}

int x4_nack_grace_offer(X4NackGraceState *state, const X4NackGraceRequest *request,
    uint64_t now_us, X4NackGraceEffects *effects_out)
{
    X4NackGraceEffects effects = {0};
    if (effects_out) *effects_out = effects;
    if (!state || !request) return X4_GRACE_OFFER_INVALID;
    ++state->counters.offers;
    if (request->session_id != state->session_id) {
        ++state->counters.stale_session_offers; return X4_GRACE_OFFER_STALE_SESSION;
    }
    if (state->closed) { ++state->counters.closed_offers; return X4_GRACE_OFFER_CLOSED; }
    if (!time_ok(state, now_us, &effects)) {
        if (effects_out) *effects_out = effects;
        return X4_GRACE_OFFER_CLOCK;
    }
    expire(state, now_us, &effects);
    if (effects_out) *effects_out = effects;
    uint32_t ordinal = (uint32_t)request->identity;
    unsigned highest = 0;
    for (uint64_t mask = request->missing_mask; mask > 1; mask >>= 1) ++highest;
    if (!request->identity || !ordinal || ordinal > X4_NACK_GRACE_REQUEST_MAX ||
        !request->missing_mask || !request->sequence_epoch || request->detected_us > now_us ||
        request->first_extended > UINT64_MAX - highest) {
        ++state->counters.invalid_offers; return X4_GRACE_OFFER_INVALID;
    }
    if (!state->bound || state->ssrc != request->ssrc ||
        state->sequence_epoch != request->sequence_epoch) {
        ++state->counters.stale_source_offers; return X4_GRACE_OFFER_STALE_SOURCE;
    }
    uint64_t request_bit = UINT64_C(1) << (ordinal - 1);
    if (state->seen_request_ids & request_bit) {
        ++state->counters.duplicate_offers; return X4_GRACE_OFFER_DUPLICATE;
    }
    state->seen_request_ids |= request_bit;
    X4NackGraceRange *range = &state->ranges[ordinal - 1];
    *range = (X4NackGraceRange){.identity = request->identity,
        .first_extended = request->first_extended, .original_mask = request->missing_mask,
        .detected_us = request->detected_us, .ssrc = request->ssrc,
        .sequence_epoch = request->sequence_epoch, .seen = true};
    if (request->detected_us > UINT64_MAX - X4_NACK_GRACE_US) {
        ++state->counters.deadline_overflow_offers; return X4_GRACE_OFFER_DEADLINE_OVERFLOW;
    }
    range->deadline_us = request->detected_us + X4_NACK_GRACE_US;
    if (!state->enabled) { ++state->counters.disabled_offers; return X4_GRACE_OFFER_DISABLED; }
    if (now_us >= range->deadline_us) {
        range->release_reason = X4_GRACE_RELEASE_DEADLINE;
        ++state->counters.expired_offers; return X4_GRACE_OFFER_EXPIRED;
    }
    if (state->active_count >= X4_NACK_GRACE_ACTIVE_MAX) {
        ++state->counters.active_cap_offers; return X4_GRACE_OFFER_ACTIVE_CAP;
    }
    uint64_t grant = request->missing_mask;
    /* Fixed64 histories x fixed64 bits; no scanning a sequence-sized gap.
     * Positions are canonical across separately proven same-source epochs.
     * A rebase cannot renew a tombstoned position by changing epoch alone. */
    for (unsigned bit = 0; bit < 64; ++bit) if (grant & (UINT64_C(1) << bit)) {
        uint64_t position = request->first_extended + bit;
        for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
            const X4NackGraceRange *old = &state->ranges[i];
            if (old == range || !old->seen || old->ssrc != request->ssrc || position < old->first_extended)
                continue;
            uint64_t offset = position - old->first_extended;
            if (offset < 64 && (old->original_mask & (UINT64_C(1) << offset))) {
                grant &= ~(UINT64_C(1) << bit); ++state->counters.overlap_positions; break;
            }
        }
    }
    range->granted_mask = range->pending_mask = grant;
    if (!grant) { ++state->counters.overlap_only_offers; return X4_GRACE_OFFER_OVERLAP; }
    range->active = true;
    ++state->active_count; ++state->counters.admitted;
    state->counters.pending_positions += population(grant);
    if (state->active_count > state->counters.active_peak)
        state->counters.active_peak = state->active_count;
    return X4_GRACE_OFFER_ADMITTED;
}

X4NackGraceEffects x4_nack_grace_accepted_wire(X4NackGraceState *state,
    uint32_t ssrc, uint32_t sequence_epoch, uint16_t sequence, uint64_t now_us)
{
    X4NackGraceEffects effects = {0};
    if (!state || state->closed || !state->bound || state->ssrc != ssrc ||
        state->sequence_epoch != sequence_epoch) return effects;
    if (!time_ok(state, now_us, &effects)) return effects;
    expire(state, now_us, &effects);
    if (!state->active_count) return effects;
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
        X4NackGraceRange *range = &state->ranges[i];
        if (!range->active || !pending_at(range, sequence)) continue;
        uint16_t offset = (uint16_t)(sequence - (uint16_t)range->first_extended);
        uint64_t bit = UINT64_C(1) << offset;
        range->pending_mask &= ~bit; range->accepted_mask |= bit;
        ++state->counters.accepted_positions; --state->counters.pending_positions;
        if (!range->pending_mask) retire(state, range,
            range->accepted_mask == range->granted_mask ? X4_GRACE_RELEASE_ACCEPTED :
            X4_GRACE_RELEASE_PASSED, &effects);
    }
    return effects;
}

X4NackGraceEffects x4_nack_grace_accepted_extended(X4NackGraceState *state,
    uint32_t ssrc, uint32_t sequence_epoch, uint64_t sequence, uint64_t now_us)
{
    X4NackGraceEffects effects = {0};
    if (!state || state->closed || !state->bound || state->ssrc != ssrc ||
        state->sequence_epoch != sequence_epoch) return effects;
    if (!time_ok(state, now_us, &effects)) return effects;
    expire(state, now_us, &effects);
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
        X4NackGraceRange *range = &state->ranges[i];
        if (!range->active || sequence < range->first_extended) continue;
        uint64_t offset = sequence - range->first_extended;
        if (offset >= 64 || !(range->pending_mask & (UINT64_C(1) << offset))) continue;
        uint64_t bit = UINT64_C(1) << offset;
        range->pending_mask &= ~bit; range->accepted_mask |= bit;
        ++state->counters.accepted_positions; --state->counters.pending_positions;
        if (!range->pending_mask) retire(state, range,
            range->accepted_mask == range->granted_mask ? X4_GRACE_RELEASE_ACCEPTED :
            X4_GRACE_RELEASE_PASSED, &effects);
    }
    return effects;
}

X4NackGraceDecision x4_nack_grace_query(X4NackGraceState *state,
    uint16_t expected, uint32_t buffered, uint32_t furthest_forward_offset,
    uint64_t now_us)
{
    X4NackGraceDecision decision = {0};
    if (!state) return decision;
    if (state->closed) { decision.reason = X4_GRACE_CLOSED; return decision; }
    if (!time_ok(state, now_us, &decision.effects)) {
        decision.reason = X4_GRACE_CLOCK; return decision;
    }
    if (!state->enabled || !state->active_count) return decision;
    bool matching_expired = false;
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
        X4NackGraceRange *range = &state->ranges[i];
        if (!range->active) continue;
        if (now_us >= range->deadline_us) {
            if (pending_at(range, expected)) matching_expired = true;
            retire(state, range, X4_GRACE_RELEASE_DEADLINE, &decision.effects);
            continue;
        }
        /* A forward worker advance can abandon only a prefix of this range.
         * It is not acceptance. Use modular halfspan ordering, never signed
         * overflow or an unbounded loop over skipped sequence positions. */
        uint16_t advanced = (uint16_t)(expected - (uint16_t)range->first_extended);
        uint64_t passed = advanced && advanced < 64 ? (UINT64_C(1) << advanced) - 1 :
            advanced >= 64 && advanced < 32768 ? UINT64_MAX : 0;
        passed &= range->pending_mask;
        range->pending_mask &= ~passed;
        state->counters.passed_positions += population(passed);
        state->counters.pending_positions -= population(passed);
        if (advanced == 32768 || !range->pending_mask) {
            retire(state, range, X4_GRACE_RELEASE_PASSED, &decision.effects);
            continue;
        }
        if (!pending_at(range, expected)) continue;
        if (buffered >= X4_NACK_GRACE_WINDOW || furthest_forward_offset >= X4_NACK_GRACE_WINDOW) {
            retire(state, range, X4_GRACE_RELEASE_CAPACITY, &decision.effects);
            decision.reason = X4_GRACE_CAPACITY;
            decision.identity = range->identity; decision.deadline_us = range->deadline_us;
            continue;
        }
        decision.reason = X4_GRACE_HOLD;
        decision.identity = range->identity; decision.deadline_us = range->deadline_us;
        decision.pending_mask = range->pending_mask;
    }
    if (decision.reason == X4_GRACE_HOLD) ++state->counters.hold_queries;
    else if (decision.reason == X4_GRACE_NONE && matching_expired) decision.reason = X4_GRACE_DEADLINE;
    return decision;
}

bool x4_nack_grace_release_forces_gap(enum X4NackGraceRelease reason)
{
    return reason == X4_GRACE_RELEASE_DEADLINE || reason == X4_GRACE_RELEASE_CAPACITY;
}

uint32_t x4_nack_grace_advance_limit(const X4NackGraceState *state,
    uint16_t expected, uint32_t default_limit)
{
    if (!state || !state->enabled || state->closed || !state->active_count)
        return default_limit;
    uint32_t limit = default_limit;
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
        const X4NackGraceRange *range = &state->ranges[i];
        if (!range->active) continue;
        for (unsigned bit = 0; bit < 64; ++bit) {
            if (!(range->pending_mask & (UINT64_C(1) << bit))) continue;
            uint16_t sequence = (uint16_t)(range->first_extended + bit);
            uint32_t offset = (uint16_t)(sequence - expected);
            if (offset < limit) limit = offset;
        }
    }
    return limit;
}

X4NackGraceEffects x4_nack_grace_cancel(X4NackGraceState *state,
    uint64_t identity, enum X4NackGraceRelease reason)
{
    X4NackGraceEffects effects = {0};
    uint32_t ordinal = (uint32_t)identity;
    if (!state || !ordinal || ordinal > X4_NACK_GRACE_REQUEST_MAX) return effects;
    X4NackGraceRange *range = &state->ranges[ordinal - 1];
    if (range->identity != identity) return effects;
    if (reason != X4_GRACE_RELEASE_TRANSPORT && reason != X4_GRACE_RELEASE_CLOSE &&
        reason != X4_GRACE_RELEASE_SOURCE && reason != X4_GRACE_RELEASE_CAPACITY)
        reason = X4_GRACE_RELEASE_EXPLICIT;
    retire(state, range, reason, &effects);
    return effects;
}

X4NackGraceEffects x4_nack_grace_close_reason(X4NackGraceState *state,
    enum X4NackGraceRelease reason)
{
    X4NackGraceEffects effects = {0};
    if (!state) return effects;
    if (reason != X4_GRACE_RELEASE_CLOSE) reason = X4_GRACE_RELEASE_EXPLICIT;
    retire_all(state, reason, &effects);
    state->closed = true; state->enabled = false;
    return effects;
}
