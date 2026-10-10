/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only state tests. No RTP transport, SRTP, console or media decoding. */
#include "receiver_nack_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t checks;
static unsigned cases;

static void fail(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}
#define CHECK(expression) do { ++checks; if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (0)
#define CASE(test) do { test(); ++cases; printf("PASS %s\n", #test); } while (0)

static X4ReceiverNackObservation observe(X4ReceiverNackState *state,
    uint32_t ssrc, uint8_t pt, uint16_t sequence, uint64_t now)
{
    X4ReceiverNackObservation result;
    memset(&result, 0xa5, sizeof(result));
    x4_receiver_nack_observe(state, ssrc, pt, sequence, now, &result);
    return result;
}

static X4ReceiverNackObservation video(X4ReceiverNackState *state,
    uint16_t sequence, uint64_t now)
{ return observe(state, 10, X4_RECEIVER_NACK_VIDEO_PT, sequence, now); }

static void initial_and_contiguous(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    CHECK(sizeof(state) < 4096);
    X4ReceiverNackObservation r = video(&state, 100, 1000);
    CHECK(r.flags == X4_NACK_OBS_INITIAL);
    CHECK(r.extended_sequence == 100 && r.sequence_epoch == 1);
    CHECK(!r.gap.present && !r.arrival.present);
    r = video(&state, 101, 1010);
    CHECK(r.flags == X4_NACK_OBS_FORWARD && r.extended_sequence == 101);
    CHECK(!r.gap.present && !r.arrival.present);
    CHECK(state.counters.observations == 2 && state.counters.forward_packets == 1);
    CHECK(state.counters.requests_reserved == 0 && state.counters.gap_ranges == 0);
}

static void single_gap_and_cancel(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    X4ReceiverNackObservation r = video(&state, 104, 20);
    CHECK(r.gap.present && r.gap.request_id == 1 && r.gap.gap_id == 1);
    CHECK(r.gap.ssrc == 10 && r.gap.sequence_epoch == 1);
    CHECK(r.gap.first_extended == 101 && r.gap.last_extended == 103);
    CHECK(r.gap.received_extended == 104 && r.gap.missing_count == 3);
    CHECK(r.gap.detected_us == 20 && r.gap.requested_us == 20);
    CHECK(r.gap.suppression == X4_NACK_REQUEST_RESERVED);
    CHECK(state.counters.pending_positions == 3);
    r = video(&state, 102, 30);
    CHECK(r.flags == X4_NACK_OBS_BACKWARD);
    CHECK(r.arrival.present && r.arrival.first_observation && !r.arrival.duplicate_observation);
    CHECK(r.arrival.extended_sequence == 102 && r.arrival.request_id == 1);
    CHECK(r.arrival.requested_us == 20 && r.arrival.observed_us == 30);
    CHECK(!r.arrival.range_complete && state.counters.pending_positions == 2);
    r = video(&state, 101, 40);
    CHECK(r.arrival.first_observation && !r.arrival.range_complete);
    r = video(&state, 103, 50);
    CHECK(r.arrival.first_observation && r.arrival.range_complete);
    CHECK(state.counters.completed_ranges == 1 && state.counters.pending_positions == 0);
    CHECK(state.counters.first_arrivals == 3 && state.counters.requests_reserved == 1);
}

static void duplicate_arrival_never_retries(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 102, 20);
    X4ReceiverNackObservation r = video(&state, 101, 30);
    CHECK(r.arrival.first_observation && r.arrival.range_complete);
    r = video(&state, 101, 40);
    CHECK(r.arrival.present && !r.arrival.first_observation && r.arrival.duplicate_observation);
    CHECK(!r.arrival.range_complete && !r.gap.present);
    CHECK((r.flags & X4_NACK_OBS_DUPLICATE) != 0);
    CHECK(state.counters.pending_positions == 0 && state.counters.completed_ranges == 1);
    CHECK(state.counters.first_arrivals == 1 && state.counters.duplicate_arrivals == 1);
    CHECK(state.counters.requests_reserved == 1);
    for (unsigned i = 0; i < 1000; ++i) {
        r = video(&state, 102, 50 + i);
        CHECK(!r.gap.present && !r.arrival.present);
    }
    CHECK(state.counters.requests_reserved == 1 && state.counters.gap_ranges == 1);
}

static void wrap_gap_and_arrivals(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 65534, 10);
    X4ReceiverNackObservation r = video(&state, 1, 20);
    CHECK(r.gap.missing_count == 2 && r.extended_sequence == 65537);
    CHECK(r.gap.first_extended == 65535 && r.gap.last_extended == 65536);
    r = video(&state, 0, 30);
    CHECK(r.arrival.first_observation && r.arrival.extended_sequence == 65536);
    CHECK(!r.arrival.range_complete);
    r = video(&state, 65535, 40);
    CHECK(r.arrival.extended_sequence == 65535 && r.arrival.range_complete);
    CHECK(state.counters.pending_positions == 0);
    r = video(&state, 2, 50);
    CHECK(r.extended_sequence == 65538 && !r.gap.present);
    CHECK(state.counters.ambiguous_halfspan == 0);
}

static void contiguous_wrap_is_not_gap(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 65535, 10);
    X4ReceiverNackObservation r = video(&state, 0, 20);
    CHECK(r.extended_sequence == 65536 && !r.gap.present);
    CHECK(state.counters.requests_reserved == 0);
    r = video(&state, 65535, 30);
    CHECK(r.extended_sequence == 65535 && (r.flags & X4_NACK_OBS_DUPLICATE));
    CHECK(!r.gap.present && !r.arrival.present);
}

static void range_64_boundary(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 1000, 10);
    X4ReceiverNackObservation r = video(&state, 1065, 20);
    CHECK(r.gap.request_id == 1 && r.gap.missing_count == 64);
    CHECK(state.ranges[0].pending == UINT64_MAX);
    for (unsigned i = 0; i < 64; ++i) {
        r = video(&state, (uint16_t)(1001 + i), 30 + i);
        CHECK(r.arrival.present && r.arrival.first_observation);
        CHECK(r.arrival.range_complete == (i == 63));
        CHECK(state.counters.pending_positions == 63 - i);
    }
    CHECK(state.counters.completed_ranges == 1 && state.counters.first_arrivals == 64);
}

static void oversized_range_does_not_iterate_or_retry(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    X4ReceiverNackObservation r = video(&state, 166, 20);
    CHECK(r.gap.present && r.gap.missing_count == 65 && !r.gap.request_id);
    CHECK(r.gap.suppression == X4_NACK_SUPPRESSED_RANGE);
    CHECK(state.counters.oversized_ranges == 1 && state.counters.oversized_positions == 65);
    CHECK(state.counters.pending_positions == 0 && state.counters.requests_reserved == 0);
    r = video(&state, 101, 30);
    CHECK(!r.gap.present && !r.arrival.present);
    r = video(&state, 32933, 40);
    CHECK(r.gap.missing_count == 32766 && r.gap.suppression == X4_NACK_SUPPRESSED_RANGE);
    CHECK(state.counters.oversized_positions == 32831);
    CHECK(state.counters.requests_reserved == 0);
}

static void disabled_reference_has_no_requests(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, false);
    video(&state, 100, 10);
    X4ReceiverNackObservation r = video(&state, 103, 20);
    CHECK(r.gap.present && !r.gap.request_id && !r.gap.requested_us);
    CHECK(r.gap.suppression == X4_NACK_SUPPRESSED_DISABLED);
    CHECK(state.counters.disabled_ranges == 1 && state.counters.disabled_positions == 2);
    r = video(&state, 101, 30);
    CHECK(!r.arrival.present && !r.gap.present);
    CHECK(state.counters.pending_positions == 0 && state.counters.requests_reserved == 0);
    r = video(&state, 169, 40);
    CHECK(r.gap.suppression == X4_NACK_SUPPRESSED_DISABLED && r.gap.missing_count == 65);
    CHECK(state.counters.oversized_ranges == 1 && state.counters.oversized_positions == 65);
}

static void dispatch_snapshot_omits_canceled_positions(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 104, 20);
    uint64_t first = UINT64_MAX;
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 7 && first == 101);
    video(&state, 102, 30);
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 5 && first == 101);
    video(&state, 101, 40);
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 4 && first == 101);
    video(&state, 103, 50);
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 0 && first == 101);
    CHECK(state.counters.requests_reserved == 1);
    CHECK(x4_receiver_nack_pending(&state, 0, &first) == 0 && first == 0);
    CHECK(x4_receiver_nack_pending(&state, 99, &first) == 0 && first == 0);
    x4_receiver_nack_close(&state);
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 0 && first == 0);
}

static void local_observer_omission_does_not_manufacture_gap(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 103, 20);
    uint64_t first = 0;
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 3);
    x4_receiver_nack_discontinuity(&state);
    CHECK(state.counters.observer_discontinuities == 1 && !state.bound);
    CHECK(state.counters.omission_retired_ranges == 1 && state.counters.omission_retired_positions == 2);
    CHECK(state.counters.pending_positions == 0 && state.counters.requests_reserved == 1);
    CHECK(x4_receiver_nack_pending(&state, 1, &first) == 0);
    X4ReceiverNackObservation r = video(&state, 110, 30);
    CHECK(r.flags == X4_NACK_OBS_INITIAL && r.sequence_epoch == 2);
    CHECK(!r.gap.present && !r.arrival.present && state.counters.gap_ranges == 1);
    r = video(&state, 112, 40);
    CHECK(r.gap.request_id == 2 && r.gap.sequence_epoch == 2);
    x4_receiver_nack_close(&state);
    x4_receiver_nack_discontinuity(&state);
    CHECK(state.counters.observer_discontinuities == 1 && state.closed);
}

static void hard_session_budget_bounds_feedback(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 0, 10);
    uint64_t reserved = 0;
    for (unsigned i = 1; i <= 10000; ++i) {
        X4ReceiverNackObservation r = video(&state, (uint16_t)(i * 2), 10 + i);
        CHECK(r.gap.present && r.gap.missing_count == 1);
        if (r.gap.request_id) {
            ++reserved;
            CHECK(r.gap.request_id == i && i <= X4_RECEIVER_NACK_SESSION_REQUEST_MAX);
        } else {
            CHECK(i > X4_RECEIVER_NACK_SESSION_REQUEST_MAX);
            CHECK(r.gap.suppression == X4_NACK_SUPPRESSED_BUDGET);
        }
    }
    CHECK(reserved == 64 && state.counters.requests_reserved == 64);
    CHECK(state.counters.budget_suppressed_ranges == 9936);
    CHECK(state.counters.budget_suppressed_positions == 9936);
    CHECK(state.counters.pending_positions <= 16);
    /* No API for refund/retry exists: a failed local send consumes its attempt. */
}

static void ssrc_change_preserves_budget_and_retires_ranges(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 103, 20);
    X4ReceiverNackObservation r = observe(&state, 99, 102, 20, 30);
    CHECK((r.flags & X4_NACK_OBS_SSRC_CHANGE) && (r.flags & X4_NACK_OBS_INITIAL));
    CHECK(r.sequence_epoch == 2 && r.extended_sequence == 20);
    CHECK(state.counters.pending_positions == 0 && state.counters.ssrc_changes == 1);
    CHECK(state.counters.ssrc_retired_ranges == 1 && state.counters.ssrc_retired_positions == 2);
    r = observe(&state, 99, 102, 22, 40);
    CHECK(r.gap.request_id == 2 && r.gap.ssrc == 99 && r.gap.sequence_epoch == 2);
    CHECK(state.counters.requests_reserved == 2);
    /* Returning to an older SSRC retires the current one; it is not concurrent
       multi-source support, nor does it create a new session budget. */
    r = video(&state, 101, 50);
    CHECK((r.flags & X4_NACK_OBS_SSRC_CHANGE) && !r.arrival.present);
    CHECK(state.counters.requests_reserved == 2 && state.counters.ssrc_changes == 2);
}

static void ssrc_changes_do_not_create_feedback_storm(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    for (unsigned i = 1; i <= 1000; ++i) {
        observe(&state, i, 102, 100, 2u * i);
        X4ReceiverNackObservation r = observe(&state, i, 102, 102, 2u * i + 1u);
        CHECK(r.gap.present);
        CHECK((r.gap.request_id != 0) == (i <= 64));
    }
    CHECK(state.counters.requests_reserved == 64);
    CHECK(state.counters.budget_suppressed_ranges == 936);
    CHECK(state.counters.sources_started == 1000 && state.counters.ssrc_changes == 999);
}

static void close_and_fresh_session(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 103, 20);
    x4_receiver_nack_close(&state);
    CHECK(state.closed && state.counters.close_retired_ranges == 1);
    CHECK(state.counters.close_retired_positions == 2 && state.counters.pending_positions == 0);
    x4_receiver_nack_close(&state);
    CHECK(state.counters.close_retired_ranges == 1);
    X4ReceiverNackObservation r = video(&state, 101, 30);
    CHECK(r.flags == X4_NACK_OBS_CLOSED && !r.arrival.present && !r.gap.present);
    CHECK(state.counters.observations_after_close == 1 && state.counters.requests_reserved == 1);
    x4_receiver_nack_init(&state, true);
    CHECK(!state.closed && !state.bound && state.counters.observations == 0);
    video(&state, 100, 40);
    r = video(&state, 102, 50);
    CHECK(r.gap.request_id == 1 && r.gap.gap_id == 1 && r.gap.sequence_epoch == 1);
}

static void ring_eviction_is_explicit(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    for (unsigned i = 1; i <= 17; ++i) video(&state, (uint16_t)(100 + 2 * i), 10 + i);
    CHECK(state.counters.requests_reserved == 17 && state.counters.pending_positions == 16);
    CHECK(state.counters.watched_ranges_evicted == 1 && state.counters.pending_ranges_evicted == 1);
    CHECK(state.counters.pending_positions_evicted == 1);
    X4ReceiverNackObservation r = video(&state, 101, 100);
    CHECK(!r.arrival.present && !r.gap.present);
    CHECK(state.counters.backward_untracked == 1);
    r = video(&state, 103, 110);
    CHECK(r.arrival.present && r.arrival.request_id == 2 && r.arrival.first_observation);
    CHECK(state.counters.pending_positions == 15);
}

static void completed_range_eviction_does_not_count_pending(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 102, 20);
    video(&state, 101, 30);
    for (unsigned i = 2; i <= 17; ++i) video(&state, (uint16_t)(100 + 2 * i), 30 + i);
    CHECK(state.counters.watched_ranges_evicted == 1);
    CHECK(state.counters.pending_ranges_evicted == 0 && state.counters.pending_positions_evicted == 0);
    CHECK(state.counters.pending_positions == 16 && state.counters.completed_ranges == 1);
}

static void late_arrival_stays_observable_without_reorder_deadline(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 102, 20);
    for (unsigned i = 103; i <= 1000; ++i) video(&state, (uint16_t)i, 30 + i);
    X4ReceiverNackObservation r = video(&state, 101, 500000);
    CHECK(r.arrival.present && r.arrival.first_observation && r.arrival.range_complete);
    CHECK(r.arrival.request_id == 1 && r.arrival.observed_us - r.arrival.requested_us == 499980);
    CHECK(!r.gap.present && state.counters.requests_reserved == 1);
    r = video(&state, 101, 500010);
    CHECK(r.arrival.duplicate_observation && (r.flags & X4_NACK_OBS_DUPLICATE));
    /* The engine does not call this useful or late: app reorder/AU decides. */
}

static void payload_filter_does_not_modify_source(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    X4ReceiverNackObservation r = observe(&state, 20, 111, 100, 10);
    CHECK(r.flags == X4_NACK_OBS_WRONG_PT && !state.bound);
    video(&state, 100, 20);
    r = observe(&state, 20, 103, 1000, 30);
    CHECK(r.flags == X4_NACK_OBS_WRONG_PT && state.ssrc == 10 && state.high_extended == 100);
    CHECK(state.counters.wrong_payload_type == 2 && state.counters.ssrc_changes == 0);
    r = video(&state, 101, 40);
    CHECK(!r.gap.present && state.counters.requests_reserved == 0);
}

static void halfspan_ambiguity_retires_and_resyncs(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 10);
    video(&state, 103, 20);
    X4ReceiverNackObservation r = video(&state, 32871, 30);
    CHECK(r.flags == X4_NACK_OBS_AMBIGUOUS && r.sequence_epoch == 2);
    CHECK(r.extended_sequence == 32871 && !r.gap.present && !r.arrival.present);
    CHECK(state.counters.ambiguous_halfspan == 1 && state.counters.pending_positions == 0);
    CHECK(state.counters.ambiguity_retired_ranges == 1 && state.counters.ambiguity_retired_positions == 2);
    CHECK(state.counters.requests_reserved == 1);
    r = video(&state, 32873, 40);
    CHECK(r.gap.request_id == 2 && r.gap.sequence_epoch == 2);
}

static void before_epoch_is_unknown_not_future_gap(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 5, 10);
    X4ReceiverNackObservation r = video(&state, 65530, 20);
    CHECK((r.flags & X4_NACK_OBS_BEFORE_EPOCH) && (r.flags & X4_NACK_OBS_BACKWARD));
    CHECK(!r.gap.present && !r.arrival.present && state.high_extended == 5);
    CHECK(state.counters.before_epoch == 1 && state.counters.requests_reserved == 0);
    r = video(&state, 6, 30);
    CHECK(r.extended_sequence == 6 && !r.gap.present);
}

static void ordinary_backward_duplicates_use_recent_bitmap(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    for (unsigned i = 100; i <= 110; ++i) video(&state, (uint16_t)i, i);
    X4ReceiverNackObservation r = video(&state, 103, 200);
    CHECK((r.flags & X4_NACK_OBS_DUPLICATE) && (r.flags & X4_NACK_OBS_BACKWARD));
    CHECK(!r.arrival.present && !r.gap.present);
    CHECK(state.counters.duplicate_packets == 1 && state.counters.backward_untracked == 1);
}

static void clock_regression_is_counted_without_negative_duration(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 100, 100);
    video(&state, 102, 200);
    X4ReceiverNackObservation r = video(&state, 101, 199);
    CHECK(r.arrival.present && r.arrival.observed_us == 199 && r.arrival.requested_us == 200);
    CHECK(state.counters.clock_regressions == 1);
    /* Integration must guard duration subtraction instead of silently wrapping. */
}

static void extended_overflow_is_explicit_resync(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 65535, 10);
    state.high_extended = UINT64_MAX;
    X4ReceiverNackObservation r = video(&state, 0, 20);
    CHECK(r.flags == X4_NACK_OBS_AMBIGUOUS && !r.gap.present);
    CHECK(state.counters.extended_overflow == 1 && state.counters.ambiguous_halfspan == 0);
    CHECK(r.sequence_epoch == 2 && state.high_extended == 0);
}

static void repeated_wraps_preserve_highwater_and_budget(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init(&state, true);
    video(&state, 65530, 10);
    for (uint64_t sequence = 65531; sequence <= 196620; ++sequence) {
        X4ReceiverNackObservation r = video(&state, (uint16_t)sequence, sequence);
        CHECK(r.extended_sequence == sequence && !r.gap.present);
    }
    CHECK(state.high_extended == 196620 && state.sequence_epoch == 1);
    CHECK(state.counters.requests_reserved == 0 && state.counters.ambiguous_halfspan == 0);
}

int main(void)
{
    CASE(initial_and_contiguous);
    CASE(single_gap_and_cancel);
    CASE(duplicate_arrival_never_retries);
    CASE(wrap_gap_and_arrivals);
    CASE(contiguous_wrap_is_not_gap);
    CASE(range_64_boundary);
    CASE(oversized_range_does_not_iterate_or_retry);
    CASE(disabled_reference_has_no_requests);
    CASE(dispatch_snapshot_omits_canceled_positions);
    CASE(local_observer_omission_does_not_manufacture_gap);
    CASE(hard_session_budget_bounds_feedback);
    CASE(ssrc_change_preserves_budget_and_retires_ranges);
    CASE(ssrc_changes_do_not_create_feedback_storm);
    CASE(close_and_fresh_session);
    CASE(ring_eviction_is_explicit);
    CASE(completed_range_eviction_does_not_count_pending);
    CASE(late_arrival_stays_observable_without_reorder_deadline);
    CASE(payload_filter_does_not_modify_source);
    CASE(halfspan_ambiguity_retires_and_resyncs);
    CASE(before_epoch_is_unknown_not_future_gap);
    CASE(ordinary_backward_duplicates_use_recent_bitmap);
    CASE(clock_regression_is_counted_without_negative_duration);
    CASE(extended_overflow_is_explicit_resync);
    CASE(repeated_wraps_preserve_highwater_and_budget);
    printf("PASS receiver NACK state: %u cases, %" PRIu64 " checks\n", cases, checks);
    return 0;
}
