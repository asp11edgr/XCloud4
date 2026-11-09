/* SPDX-License-Identifier: GPL-3.0-only */
#include "receiver_nack_state.h"
#include <string.h>

_Static_assert(sizeof(X4ReceiverNackState) <= 16u * 1024u, "bounded receiver state");

static uint32_t population(uint64_t bits)
{
    uint32_t n = 0;
    while (bits) { bits &= bits - 1u; ++n; }
    return n;
}


static unsigned range_slots(const X4ReceiverNackState *state)
{
    return state->legacy ? X4_RECEIVER_NACK_WATCH_SLOTS : X4_RECEIVER_NACK_ADAPTIVE_SLOTS;
}

static uint32_t range_pending(const X4ReceiverNackRange *range)
{
    uint32_t count = 0;
    unsigned words = (range->count + 63u) / 64u;
    for (unsigned i = 0; i < words; ++i) count += population(range->pending[i]);
    return count;
}

static uint64_t after_us(uint64_t now, unsigned delay)
{
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}

static void mark_expiry(X4ReceiverNackState *state, X4ReceiverNackRange *range, uint64_t now)
{
    if (range->expired || now < range->deadline_us) return;
    range->expired = true;
    uint32_t pending = range_pending(range);
    if (range->retry_us) state->metrics.expired_retry_no_response += pending;
    else if (range->sends) state->metrics.expired_no_first += pending;
    else state->metrics.expired_unsent += pending;
}

static void expire_range(X4ReceiverNackState *state, X4ReceiverNackRange *range)
{
    state->counters.pending_positions -= range_pending(range);
    range->used = false;
}

static void set_next_send(X4ReceiverNackState *state, uint64_t now)
{
    state->next_send_us = 0;
    for (unsigned i = 0; i < range_slots(state); ++i) {
        const X4ReceiverNackRange *range = &state->ranges[i];
        if (!range->used || range->expired || !range_pending(range)) continue;
        uint64_t due = range->send_due_us > now ? range->send_due_us : now;
        if (range->sends >= 2 || range->suppressed || due >= range->deadline_us) {
            if (!state->grace43c) continue;
            due = range->deadline_us;
        }
        if (!state->next_send_us || due < state->next_send_us)
            state->next_send_us = due ? due : 1u;
    }
}

enum RetireReason { RETIRE_SSRC, RETIRE_AMBIGUITY, RETIRE_CLOSE, RETIRE_OMISSION };

static void retire_ranges(X4ReceiverNackState *state, enum RetireReason reason)
{
    uint64_t ranges = 0, positions = 0;
    for (unsigned i = 0; i < range_slots(state); ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (range->used) {
            uint32_t pending = range_pending(range);
            if (pending) ++ranges;
            positions += pending;
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
    state->next_send_us = 0;
    memset(state->ranges, 0, sizeof(state->ranges));
}

static void begin_sequence_epoch(X4ReceiverNackState *state, uint16_t sequence,
    uint64_t monotonic_us, uint32_t reason, uint32_t previous_epoch,
    uint64_t previous_high_extended)
{
    ++state->sequence_epoch;
    state->high_extended = sequence;
    state->recent_seen = 1;
    state->sequence_tag = (X4NackSequenceTag){
        .anchor_extended = sequence,
        .previous_high_extended = previous_high_extended,
        .anchor_us = monotonic_us,
        .sequence_epoch = state->sequence_epoch,
        .previous_epoch = previous_epoch,
        .reason = reason
    };
    state->rebase_pending = false;
}

void x4_receiver_nack_set_rtt(X4ReceiverNackState *state, int rtt_p50_ms)
{
    if (state->legacy) {
        state->grace_us = 45000;
        return;
    }
    if (state->policy46b) {
        state->grace_us = 1000u * x4_nack_grace46b_ms(rtt_p50_ms,
            state->metrics.first_return_p95_us, state->metrics.first_return_samples);
        state->retry_delay_us = 1000u * x4_nack_retry46b_ms(rtt_p50_ms);
    } else {
        state->grace_us = 1000u * x4_nack_grace_ms(rtt_p50_ms, state->grace43c);
        state->retry_delay_us = x4_nack_retry_delay_us(rtt_p50_ms);
    }
}

void x4_receiver_nack_refresh(X4ReceiverNackState *state, int rtt_p50_ms,
    uint64_t now, bool startup)
{
    if (!state->grace43c) return;
    state->startup = startup;
    unsigned old = state->grace_us, old_retry = state->retry_delay_us;
    x4_receiver_nack_set_rtt(state, rtt_p50_ms);
    state->metrics.grace_us = startup ? 1000u * X4_NACK_STARTUP_GRACE_MS : state->grace_us;
    if (old == state->grace_us && old_retry == state->retry_delay_us) return;
    for (unsigned i = 0; i < range_slots(state); ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (!range->used || range->expired) continue;
        if (!range->startup) range->deadline_us = after_us(range->detected_us, state->grace_us);
        mark_expiry(state, range, now);
        if (range->sends == 1 && !range->suppressed &&
            (!state->policy46b || range->initial_us))
            range->send_due_us = after_us(range->initial_us, state->retry_delay_us);
        range->retry_delay_us = state->retry_delay_us;
    }
    set_next_send(state, now);
}

void x4_receiver_nack_snapshot(const X4ReceiverNackState *state, X4ReceiverNackMetrics *out)
{
    *out = state->metrics;
    uint64_t count = 0;
    for (unsigned i = 0; i < X4_NACK_RETRY_HISTOGRAM; ++i)
        count += state->retry_returns[i];
    if (!count) return;
    uint64_t p50 = (count + 1u) / 2u, p95 = (count * 95u + 99u) / 100u;
    uint64_t cumulative = 0;
    for (unsigned i = 0; i < X4_NACK_RETRY_HISTOGRAM; ++i) {
        cumulative += state->retry_returns[i];
        /* 1-ms bins through 100 ms; 6-ms bins for the startup tail. */
        uint64_t upper_us = i < 100u ? (i + 1u) * 1000u : 100000u + (i - 99u) * 6000u;
        if (p50 && cumulative >= p50) {
            out->retry_return_p50_us = upper_us;
            p50 = 0;
        }
        if (cumulative >= p95) {
            out->retry_return_p95_us = upper_us;
            break;
        }
    }
}

void x4_receiver_nack_init_policy(X4ReceiverNackState *state, bool enabled, bool legacy,
    bool grace43c)
{
    memset(state, 0, sizeof(*state));
    state->enabled = enabled;
    state->legacy = legacy;
    state->grace43c = !legacy && grace43c;
    state->tokens = X4_RECEIVER_NACK_ADAPTIVE_MAX;
    x4_receiver_nack_set_rtt(state, -1);
    state->metrics.grace_us = state->grace_us;
}

void x4_receiver_nack_init_mode(X4ReceiverNackState *state, bool enabled, bool legacy)
{
    x4_receiver_nack_init_policy(state, enabled, legacy, false);
}

void x4_receiver_nack_init_46b(X4ReceiverNackState *state, bool enabled)
{
    x4_receiver_nack_init_policy(state, enabled, false, true);
    state->policy46b = true;
    x4_receiver_nack_set_rtt(state, -1);
    state->metrics.grace_us = state->grace_us;
}

void x4_receiver_nack_init(X4ReceiverNackState *state, bool enabled)
{
    x4_receiver_nack_init_mode(state, enabled, true);
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
    if (state->bound) {
        state->rebase_high_extended = state->high_extended;
        state->rebase_previous_epoch = state->sequence_epoch;
        state->rebase_previous_ssrc = state->ssrc;
        state->rebase_pending = true;
    }
    retire_ranges(state, RETIRE_OMISSION);
    state->bound = false;
    state->high_extended = 0;
    state->recent_seen = 0;
    memset(&state->sequence_tag, 0, sizeof(state->sequence_tag));
}

uint64_t x4_receiver_nack_pending(const X4ReceiverNackState *state,
    uint64_t request_id, uint64_t *first_extended)
{
    *first_extended = 0;
    if (!state->legacy || state->closed || !request_id) return 0;
    for (unsigned i = 0; i < range_slots(state); ++i) {
        const X4ReceiverNackRange *range = &state->ranges[i];
        if (range->used && range->request_id == request_id) {
            *first_extended = range->first_extended;
            return range->pending[0];
        }
    }
    return 0;
}

/* Four bounded radix passes select an exact nearest-rank sample in place.
 * The ring retains arrival order for eviction. The histogram uses stack
 * storage and the sample path neither allocates nor sorts the whole window. */
static uint32_t first_return_percentile(const uint32_t *samples, unsigned count)
{
    unsigned rank = (count * 95u + 99u) / 100u;
    uint32_t mask = 0, prefix = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint16_t buckets[256] = {0};
        for (unsigned i = 0; i < count; ++i)
            if ((samples[i] & mask) == prefix)
                ++buckets[(samples[i] >> shift) & 255u];
        unsigned selected = 0;
        while (rank > buckets[selected]) rank -= buckets[selected++];
        prefix |= (uint32_t)selected << shift;
        mask |= UINT32_C(255) << shift;
    }
    return prefix;
}

static void record_first_return(X4ReceiverNackState *state, uint64_t delay)
{
    uint32_t samples = state->metrics.first_return_samples;
    state->first_returns[state->first_return_cursor] =
        delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
    state->first_return_cursor =
        (state->first_return_cursor + 1u) % X4_NACK_FIRST_RETURN_WINDOW;
    if (samples < X4_NACK_FIRST_RETURN_WINDOW) ++samples;
    state->metrics.first_return_samples = samples;
    if (samples >= X4_NACK_FIRST_RETURN_MINIMUM)
        state->metrics.first_return_p95_us = first_return_percentile(state->first_returns, samples);
}

static bool observe_arrival(X4ReceiverNackState *state, uint64_t extended,
    uint64_t now, bool measure_first_return, X4ReceiverNackObservation *observation)
{
    for (unsigned i = 0; i < range_slots(state); ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (!range->used || extended < range->first_extended ||
            extended - range->first_extended >= range->count) continue;
        unsigned offset = (unsigned)(extended - range->first_extended);
        unsigned word = offset / 64u;
        uint64_t bit = UINT64_C(1) << (offset % 64u);
        bool first = (range->pending[word] & bit) != 0;
        bool duplicate = (range->arrived[word] & bit) != 0;
        /* All watched positions start pending and are canceled once. */
        if (first) {
            if (!state->legacy) {
                mark_expiry(state, range, now);
                /* RTP and RTX identify the lost position, not the request
                 * that caused the return. Overlapping attempts are ambiguous. */
                if (state->policy46b && measure_first_return && range->initial_us &&
                    !range->retry_us && now >= range->initial_us)
                    record_first_return(state, now - range->initial_us);
                if (range->retry_us && now >= range->retry_us) {
                    uint64_t delay = now - range->retry_us;
                    uint64_t bucket = delay < 100000u ? delay / 1000u :
                        100u + (delay - 100000u) / 6000u;
                    if (bucket >= X4_NACK_RETRY_HISTOGRAM) bucket = X4_NACK_RETRY_HISTOGRAM - 1u;
                    if (state->retry_returns[bucket] != UINT32_MAX) ++state->retry_returns[bucket];
                    if (range->expired) {
                        --state->metrics.expired_retry_no_response;
                        ++state->metrics.expired_retry_late;
                    }
                }
            }
            range->pending[word] &= ~bit;
            range->arrived[word] |= bit;
            --state->counters.pending_positions;
            ++state->counters.first_arrivals;
            if (!range_pending(range)) ++state->counters.completed_ranges;
        } else if (duplicate) {
            ++state->counters.duplicate_arrivals;
        }
        ++state->counters.matched_arrivals;
        if (now < range->requested_us) ++state->counters.clock_regressions;
        observation->arrival = (X4ReceiverNackArrival){
            .present = true,
            .first_observation = first,
            .duplicate_observation = duplicate,
            .range_complete = first && !range_pending(range),
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
    unsigned maximum = state->legacy ? X4_RECEIVER_NACK_RANGE_MAX : X4_RECEIVER_NACK_ADAPTIVE_MAX;
    if (count > maximum) {
        ++counters->oversized_ranges;
        counters->oversized_positions += count;
    }
    if (!state->enabled) {
        gap.suppression = X4_NACK_SUPPRESSED_DISABLED;
        ++counters->disabled_ranges;
        counters->disabled_positions += count;
    } else if (count > maximum) {
        gap.suppression = X4_NACK_SUPPRESSED_RANGE;
    } else if (state->legacy && counters->requests_reserved >= X4_RECEIVER_NACK_SESSION_REQUEST_MAX) {
        gap.suppression = X4_NACK_SUPPRESSED_BUDGET;
        ++counters->budget_suppressed_ranges;
        counters->budget_suppressed_positions += count;
    } else if (state->legacy || now <= UINT64_MAX - state->grace_us) {
        unsigned slot = state->next_range;
        if (!state->legacy) {
            slot = range_slots(state);
            for (unsigned step = 0; step < range_slots(state); ++step) {
                unsigned at = (state->next_range + step) % range_slots(state);
                X4ReceiverNackRange *range = &state->ranges[at];
                if (range->used && now >= range->deadline_us) {
                    mark_expiry(state, range, now);
                    expire_range(state, range);
                }
                if (!range->used || !range_pending(range)) { slot = at; break; }
            }
        }
        if (slot == range_slots(state)) {
            gap.suppression = X4_NACK_SUPPRESSED_SLOTS;
            ++counters->pending_ranges_evicted;
            counters->pending_positions_evicted += count;
            observation->gap = gap;
            return;
        }
        ++counters->requests_reserved;
        if (!++state->request_ordinal) ++state->request_ordinal;
        gap.request_id = state->request_ordinal;
        gap.requested_us = now;
        gap.suppression = X4_NACK_REQUEST_RESERVED;
        X4ReceiverNackRange *range = &state->ranges[slot];
        if (range->used && state->legacy) {
            ++counters->watched_ranges_evicted;
            uint32_t pending = range_pending(range);
            if (pending) ++counters->pending_ranges_evicted;
            counters->pending_positions_evicted += pending;
            counters->pending_positions -= pending;
        }
        *range = (X4ReceiverNackRange){
            .gap_id = gap.gap_id,
            .request_id = gap.request_id,
            .first_extended = expected,
            .detected_us = now,
            .requested_us = now,
            .count = count,
            .used = true
        };
        for (unsigned word = 0; word * 64u < count; ++word) {
            unsigned remaining = count - word * 64u;
            range->pending[word] = remaining >= 64u ? UINT64_MAX : (UINT64_C(1) << remaining) - 1u;
        }
        counters->pending_positions += count;
        if (state->legacy) {
            state->next_range = (state->next_range + 1u) % X4_RECEIVER_NACK_WATCH_SLOTS;
        } else {
            state->next_range = (slot + 1u) % X4_RECEIVER_NACK_ADAPTIVE_SLOTS;
            range->startup = state->startup;
            unsigned grace_us = range->startup ? 1000u * X4_NACK_STARTUP_GRACE_MS : state->grace_us;
            range->deadline_us = after_us(now, grace_us);
            range->send_due_us = now;
            range->retry_delay_us = state->retry_delay_us;
            state->metrics.grace_us = grace_us;
            if (!state->next_send_us || now < state->next_send_us)
                state->next_send_us = now ? now : 1u;
        }
    } else {
        gap.suppression = X4_NACK_SUPPRESSED_RANGE;
    }
    observation->gap = gap;
}

void x4_receiver_nack_observe_return(X4ReceiverNackState *state, uint32_t ssrc,
    uint8_t payload_type, uint16_t sequence, uint64_t monotonic_us,
    bool measure_first_return, X4ReceiverNackObservation *observation)
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
        uint32_t previous_epoch = state->bound ? state->sequence_epoch :
            state->rebase_pending ? state->rebase_previous_epoch : 0;
        uint64_t previous_high = state->bound ? state->high_extended :
            state->rebase_pending ? state->rebase_high_extended : 0;
        uint32_t previous_ssrc = state->bound ? state->ssrc :
            state->rebase_previous_ssrc;
        uint32_t reason = !previous_epoch ? X4_NACK_SEQUENCE_INITIAL :
            previous_ssrc != ssrc ? X4_NACK_SEQUENCE_SOURCE_CHANGE :
            X4_NACK_SEQUENCE_OMISSION;
        if (state->bound) {
            ++state->counters.ssrc_changes;
            retire_ranges(state, RETIRE_SSRC);
            observation->flags |= X4_NACK_OBS_SSRC_CHANGE;
        }
        state->bound = true;
        state->ssrc = ssrc;
        ++state->counters.sources_started;
        begin_sequence_epoch(state, sequence, monotonic_us, reason,
            previous_epoch, previous_high);
        observation->flags |= X4_NACK_OBS_INITIAL;
        observation->sequence_epoch = state->sequence_epoch;
        observation->extended_sequence = state->high_extended;
        observation->sequence_tag = state->sequence_tag;
        return;
    }

    uint16_t high_wire = (uint16_t)state->high_extended;
    uint32_t forward = (uint16_t)(sequence - high_wire);
    if (forward == 32768u || (forward < 32768u &&
        UINT64_MAX - state->high_extended < forward)) {
        if (forward == 32768u) ++state->counters.ambiguous_halfspan;
        else ++state->counters.extended_overflow;
        retire_ranges(state, RETIRE_AMBIGUITY);
        begin_sequence_epoch(state, sequence, monotonic_us,
            forward == 32768u ? X4_NACK_SEQUENCE_AMBIGUOUS : X4_NACK_SEQUENCE_OVERFLOW,
            state->sequence_epoch, state->high_extended);
        observation->flags = X4_NACK_OBS_AMBIGUOUS;
        observation->sequence_epoch = state->sequence_epoch;
        observation->extended_sequence = state->high_extended;
        observation->sequence_tag = state->sequence_tag;
        return;
    }

    observation->sequence_epoch = state->sequence_epoch;
    observation->sequence_tag = state->sequence_tag;
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
            monotonic_us, measure_first_return, observation);
        if (duplicate || watched_duplicate) {
            ++state->counters.duplicate_packets;
            observation->flags |= X4_NACK_OBS_DUPLICATE;
        }
        if (!observation->arrival.present) ++state->counters.backward_untracked;
    }
}

void x4_receiver_nack_observe(X4ReceiverNackState *state, uint32_t ssrc,
    uint8_t payload_type, uint16_t sequence, uint64_t monotonic_us,
    X4ReceiverNackObservation *observation)
{
    x4_receiver_nack_observe_return(state, ssrc, payload_type, sequence,
        monotonic_us, true, observation);
}

static void refill(X4ReceiverNackState *state, uint64_t now)
{
    if (!state->token_clock_started) {
        state->token_clock_started = true;
        state->token_clock_us = now;
        return;
    }
    if (now < state->token_clock_us) return;
    uint64_t added = (now - state->token_clock_us) / X4_RECEIVER_NACK_TOKEN_US;
    if (added >= X4_RECEIVER_NACK_ADAPTIVE_MAX - state->tokens) {
        state->tokens = X4_RECEIVER_NACK_ADAPTIVE_MAX;
        state->token_clock_us = now;
    } else {
        state->tokens += (unsigned)added;
        state->token_clock_us += added * X4_RECEIVER_NACK_TOKEN_US;
    }
}

bool x4_receiver_nack_next(X4ReceiverNackState *state, uint64_t now,
    X4ReceiverNackGap *gap, uint64_t pending[X4_RECEIVER_NACK_MASK_WORDS], bool *retry)
{
    if (state->legacy || state->closed || !state->enabled) {
        state->next_send_us = 0;
        return false;
    }
    refill(state, now);
    X4ReceiverNackRange *selected = NULL;
    unsigned positions = 0;
    for (unsigned i = 0; i < range_slots(state); ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (!range->used) continue;
        /* Retain the identity after expiry for late admission diagnostics,
         * until allocation replaces this physical slot. */
        mark_expiry(state, range, now);
        if (range->sends >= 2 || range->suppressed || now < range->send_due_us || range->expired) continue;
        unsigned count = range_pending(range);
        if (!count) continue;
        if (count > state->tokens) {
            uint64_t wait = (uint64_t)(count - state->tokens) * X4_RECEIVER_NACK_TOKEN_US;
            uint64_t due = state->token_clock_us > UINT64_MAX - wait ?
                UINT64_MAX : state->token_clock_us + wait;
            if (due < range->deadline_us && due > now) {
                range->send_due_us = due;
            } else {
                ++state->counters.budget_suppressed_ranges;
                state->counters.budget_suppressed_positions += count;
                state->metrics.budget_suppressed_positions += count;
                range->suppressed = true;
            }
            continue;
        }
        if (!selected || range->sends < selected->sends ||
            (range->sends == selected->sends && range->detected_us < selected->detected_us)) {
            selected = range;
            positions = count;
        }
    }
    if (selected) {
        *retry = selected->sends != 0;
        state->tokens -= positions;
        state->metrics.requested_positions += positions;
        if (*retry) {
            ++state->metrics.retries;
            selected->retry_us = now;
        } else {
            ++state->metrics.initial_requests;
            if (!state->policy46b) selected->initial_us = now;
        }
        ++selected->sends;
        if (selected->sends == 1 && state->policy46b)
            selected->send_due_us = UINT64_MAX;
        else if (selected->sends == 1 && now <= UINT64_MAX - selected->retry_delay_us)
            selected->send_due_us = now + selected->retry_delay_us;
        else selected->sends = 2;
        *gap = (X4ReceiverNackGap){
            .present = true, .ssrc = state->ssrc, .sequence_epoch = state->sequence_epoch,
            .gap_id = selected->gap_id, .request_id = selected->request_id,
            .first_extended = selected->first_extended,
            .last_extended = selected->first_extended + selected->count - 1u,
            .detected_us = selected->detected_us, .requested_us = selected->requested_us,
            .missing_count = selected->count
        };
        memcpy(pending, selected->pending, sizeof(selected->pending));
    }
    set_next_send(state, now);
    return selected != NULL;
}

void x4_receiver_nack_sent_first(X4ReceiverNackState *state, uint64_t request_id,
    uint64_t dispatch_us)
{
    if (!state->policy46b || !dispatch_us) return;
    for (unsigned i = 0; i < range_slots(state); ++i) {
        X4ReceiverNackRange *range = &state->ranges[i];
        if (!range->used || range->request_id != request_id) continue;
        if (range->sends != 1 || range->initial_us) return;
        range->initial_us = dispatch_us;
        range->send_due_us = after_us(dispatch_us, range->retry_delay_us);
        set_next_send(state, dispatch_us);
        return;
    }
}
