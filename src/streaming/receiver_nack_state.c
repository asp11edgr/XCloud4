/* SPDX-License-Identifier: GPL-3.0-only */
#include "receiver_nack_state.h"
#include <string.h>

static uint32_t population(uint64_t bits)
{
    uint32_t n = 0;
    while (bits) { bits &= bits - 1u; ++n; }
    return n;
}

enum RetireReason { RETIRE_SSRC, RETIRE_AMBIGUITY, RETIRE_CLOSE, RETIRE_OMISSION };

static void retire_ranges(X4ReceiverNackState *state, enum RetireReason reason)
{
    uint64_t ranges = 0, positions = 0;
    for (unsigned i = 0; i < X4_RECEIVER_NACK_WATCH_SLOTS; ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (range->used && range->pending) {
            ++ranges;
            positions += population(range->pending);
        }
    }
    if (reason == RETIRE_SSRC) {
        state->counters.ssrc_retired_ranges += ranges;
        state->counters.ssrc_retired_positions += positions;
    } else if (reason == RETIRE_AMBIGUITY) {
        state->counters.ambiguity_retired_ranges += ranges;
        state->counters.ambiguity_retired_positions += positions;
    } else if (reason == RETIRE_CLOSE) {
        state->counters.close_retired_ranges += ranges;
        state->counters.close_retired_positions += positions;
    } else {
        state->counters.omission_retired_ranges += ranges;
        state->counters.omission_retired_positions += positions;
    }
    state->counters.pending_positions = 0;
    state->next_range = 0;
    memset(state->ranges, 0, sizeof(state->ranges));
}

static void begin_sequence_epoch(X4ReceiverNackState *state, uint16_t sequence)
{
    ++state->sequence_epoch;
    state->high_extended = sequence;
    state->recent_seen = 1;
}

void x4_receiver_nack_init(X4ReceiverNackState *state, bool enabled)
{
    memset(state, 0, sizeof(*state));
    state->enabled = enabled;
}

void x4_receiver_nack_close(X4ReceiverNackState *state)
{
    if (state->closed) return;
    retire_ranges(state, RETIRE_CLOSE);
    state->closed = true;
}

void x4_receiver_nack_discontinuity(X4ReceiverNackState *state)
{
    if (state->closed) return;
    ++state->counters.observer_discontinuities;
    retire_ranges(state, RETIRE_OMISSION);
    state->bound = false;
    state->high_extended = 0;
    state->recent_seen = 0;
}

uint64_t x4_receiver_nack_pending(const X4ReceiverNackState *state,
    uint64_t request_id, uint64_t *first_extended)
{
    *first_extended = 0;
    if (state->closed || !request_id) return 0;
    for (unsigned i = 0; i < X4_RECEIVER_NACK_WATCH_SLOTS; ++i) {
        const X4ReceiverNackRange *range = &state->ranges[i];
        if (range->used && range->request_id == request_id) {
            *first_extended = range->first_extended;
            return range->pending;
        }
    }
    return 0;
}

static bool observe_arrival(X4ReceiverNackState *state, uint64_t extended,
    uint64_t now, X4ReceiverNackObservation *observation)
{
    for (unsigned i = 0; i < X4_RECEIVER_NACK_WATCH_SLOTS; ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (!range->used || extended < range->first_extended ||
            extended - range->first_extended >= range->count) continue;
        uint64_t bit = UINT64_C(1) << (extended - range->first_extended);
        bool first = (range->pending & bit) != 0;
        bool duplicate = (range->arrived & bit) != 0;
        /* All watched positions start pending and are canceled once. */
        if (first) {
            range->pending &= ~bit;
            range->arrived |= bit;
            --state->counters.pending_positions;
            ++state->counters.first_arrivals;
            if (!range->pending) ++state->counters.completed_ranges;
        } else if (duplicate) {
            ++state->counters.duplicate_arrivals;
        }
        ++state->counters.matched_arrivals;
        if (now < range->requested_us) ++state->counters.clock_regressions;
        observation->arrival = (X4ReceiverNackArrival){
            .present = true,
            .first_observation = first,
            .duplicate_observation = duplicate,
            .range_complete = first && !range->pending,
            .ssrc = state->ssrc,
            .sequence_epoch = state->sequence_epoch,
            .gap_id = range->gap_id,
            .request_id = range->request_id,
            .extended_sequence = extended,
            .detected_us = range->detected_us,
            .requested_us = range->requested_us,
            .observed_us = now
        };
        return duplicate;
    }
    return false;
}

static void create_gap(X4ReceiverNackState *state, uint64_t expected,
    uint64_t received, uint32_t count, uint64_t now,
    X4ReceiverNackObservation *observation)
{
    X4ReceiverNackCounters *counters = &state->counters;
    X4ReceiverNackGap gap = {
        .present = true,
        .ssrc = state->ssrc,
        .sequence_epoch = state->sequence_epoch,
        .gap_id = ++counters->gap_ranges,
        .first_extended = expected,
        .last_extended = received - 1u,
        .received_extended = received,
        .detected_us = now,
        .missing_count = count
    };
    counters->missing_positions += count;
    if (count > X4_RECEIVER_NACK_RANGE_MAX) {
        ++counters->oversized_ranges;
        counters->oversized_positions += count;
    }
    if (!state->enabled) {
        gap.suppression = X4_NACK_SUPPRESSED_DISABLED;
        ++counters->disabled_ranges;
        counters->disabled_positions += count;
    } else if (count > X4_RECEIVER_NACK_RANGE_MAX) {
        gap.suppression = X4_NACK_SUPPRESSED_RANGE;
    } else if (counters->requests_reserved >= X4_RECEIVER_NACK_SESSION_REQUEST_MAX) {
        gap.suppression = X4_NACK_SUPPRESSED_BUDGET;
        ++counters->budget_suppressed_ranges;
        counters->budget_suppressed_positions += count;
    } else {
        gap.request_id = ++counters->requests_reserved;
        gap.requested_us = now;
        gap.suppression = X4_NACK_REQUEST_RESERVED;
        X4ReceiverNackRange *range = &state->ranges[state->next_range];
        if (range->used) {
            ++counters->watched_ranges_evicted;
            uint32_t pending = population(range->pending);
            if (pending) ++counters->pending_ranges_evicted;
            counters->pending_positions_evicted += pending;
            counters->pending_positions -= pending;
        }
        *range = (X4ReceiverNackRange){
            .gap_id = gap.gap_id,
            .request_id = gap.request_id,
            .first_extended = expected,
            .pending = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1u,
            .detected_us = now,
            .requested_us = now,
            .count = count,
            .used = true
        };
        counters->pending_positions += count;
        state->next_range = (state->next_range + 1u) % X4_RECEIVER_NACK_WATCH_SLOTS;
    }
    observation->gap = gap;
}

void x4_receiver_nack_observe(X4ReceiverNackState *state, uint32_t ssrc,
    uint8_t payload_type, uint16_t sequence, uint64_t monotonic_us,
    X4ReceiverNackObservation *observation)
{
    memset(observation, 0, sizeof(*observation));
    ++state->counters.observations;
    if (state->closed) {
        ++state->counters.observations_after_close;
        observation->flags = X4_NACK_OBS_CLOSED;
        return;
    }
    if (payload_type != X4_RECEIVER_NACK_VIDEO_PT) {
        ++state->counters.wrong_payload_type;
        observation->flags = X4_NACK_OBS_WRONG_PT;
        return;
    }
    if (!state->bound || state->ssrc != ssrc) {
        if (state->bound) {
            ++state->counters.ssrc_changes;
            retire_ranges(state, RETIRE_SSRC);
            observation->flags |= X4_NACK_OBS_SSRC_CHANGE;
        }
        state->bound = true;
        state->ssrc = ssrc;
        ++state->counters.sources_started;
        begin_sequence_epoch(state, sequence);
        observation->flags |= X4_NACK_OBS_INITIAL;
        observation->sequence_epoch = state->sequence_epoch;
        observation->extended_sequence = state->high_extended;
        return;
    }

    uint16_t high_wire = (uint16_t)state->high_extended;
    uint32_t forward = (uint16_t)(sequence - high_wire);
    if (forward == 32768u || (forward < 32768u &&
        UINT64_MAX - state->high_extended < forward)) {
        if (forward == 32768u) ++state->counters.ambiguous_halfspan;
        else ++state->counters.extended_overflow;
        retire_ranges(state, RETIRE_AMBIGUITY);
        begin_sequence_epoch(state, sequence);
        observation->flags = X4_NACK_OBS_AMBIGUOUS;
        observation->sequence_epoch = state->sequence_epoch;
        observation->extended_sequence = state->high_extended;
        return;
    }

    observation->sequence_epoch = state->sequence_epoch;
    if (!forward) {
        ++state->counters.duplicate_packets;
        observation->flags = X4_NACK_OBS_DUPLICATE;
        observation->extended_sequence = state->high_extended;
    } else if (forward < 32768u) {
        uint64_t received = state->high_extended + forward;
        ++state->counters.forward_packets;
        observation->flags = X4_NACK_OBS_FORWARD;
        observation->extended_sequence = received;
        if (forward > 1u)
            create_gap(state, state->high_extended + 1u, received,
                forward - 1u, monotonic_us, observation);
        state->recent_seen = forward < 64u ? (state->recent_seen << forward) | 1u : 1u;
        state->high_extended = received;
    } else {
        uint32_t backwards = 65536u - forward;
        ++state->counters.backward_packets;
        observation->flags = X4_NACK_OBS_BACKWARD;
        if (backwards > state->high_extended) {
            ++state->counters.before_epoch;
            observation->flags |= X4_NACK_OBS_BEFORE_EPOCH;
            return;
        }
        uint64_t extended = state->high_extended - backwards;
        observation->extended_sequence = extended;
        bool duplicate = backwards < 64u &&
            (state->recent_seen & (UINT64_C(1) << backwards)) != 0;
        if (backwards < 64u) state->recent_seen |= UINT64_C(1) << backwards;
        bool watched_duplicate = observe_arrival(state, extended,
            monotonic_us, observation);
        if (duplicate || watched_duplicate) {
            ++state->counters.duplicate_packets;
            observation->flags |= X4_NACK_OBS_DUPLICATE;
        }
        if (!observation->arrival.present) ++state->counters.backward_untracked;
    }
}
