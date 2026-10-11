/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable policy tests; no console, SRTP, transport, reorder or decoder. */
#include "nack_grace.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t checks;
static unsigned cases;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define CASE(x) do { x(); ++cases; printf("PASS %s\n", #x); } while (0)

static unsigned bits(uint64_t mask)
{ unsigned n = 0; while (mask) { mask &= mask - 1; ++n; } return n; }
static X4NackGraceRequest request(unsigned ordinal, uint64_t first,
    uint64_t mask, uint64_t detected)
{
    /* Transport identity epoch1 deliberately differs from sequence epoch9. */
    return (X4NackGraceRequest){.session_id = 77,
        .identity = (UINT64_C(1) << 32) | ordinal, .first_extended = first,
        .missing_mask = mask, .detected_us = detected, .ssrc = 23,
        .sequence_epoch = 9};
}
static void init(X4NackGraceState *state, bool enabled)
{
    x4_nack_grace_init(state, 77, enabled);
    X4NackGraceEffects e = x4_nack_grace_bind_source(state, 23, 9);
    CHECK(!e.invalidated);
}
static int offer(X4NackGraceState *state, unsigned ordinal, uint64_t first,
    uint64_t mask, uint64_t detected, uint64_t now)
{
    X4NackGraceRequest r = request(ordinal, first, mask, detected);
    return x4_nack_grace_offer(state, &r, now, NULL);
}
static X4NackGraceDecision query(X4NackGraceState *state, uint16_t expected,
    uint64_t now)
{ return x4_nack_grace_query(state, expected, 32, 63, now); }
static X4NackGraceEffects accepted(X4NackGraceState *state,
    uint16_t sequence, uint64_t now)
{ return x4_nack_grace_accepted_wire(state, 23, 9, sequence, now); }
static void invariant(const X4NackGraceState *state)
{
    unsigned active = 0;
    uint64_t pending = 0;
    for (unsigned i = 0; i < X4_NACK_GRACE_REQUEST_MAX; ++i) {
        const X4NackGraceRange *r = &state->ranges[i];
        CHECK(!(r->pending_mask & ~r->granted_mask));
        CHECK(!(r->accepted_mask & ~r->granted_mask));
        CHECK(!(r->pending_mask & r->accepted_mask));
        if (!r->active) { CHECK(!r->pending_mask); continue; }
        ++active; pending += bits(r->pending_mask);
        CHECK(r->seen && r->pending_mask);
        CHECK(r->deadline_us == r->detected_us + X4_NACK_GRACE_US);
        CHECK(state->seen_request_ids & (UINT64_C(1) << i));
        CHECK(r->ssrc == state->ssrc && r->sequence_epoch == state->sequence_epoch);
        CHECK(r->release_reason == X4_GRACE_RELEASE_NONE);
    }
    CHECK(active == state->active_count && active <= X4_NACK_GRACE_ACTIVE_MAX);
    CHECK(pending == state->counters.pending_positions);
}

static void disabled_reference(void)
{
    X4NackGraceState state; init(&state, false);
    CHECK(offer(&state, 1, 100, 7, 1000, 1001) == X4_GRACE_OFFER_DISABLED);
    CHECK(query(&state, 100, 1002).reason == X4_GRACE_NONE);
    CHECK(!state.active_count && state.counters.disabled_offers == 1);
    CHECK(!accepted(&state, 100, 1003).completed);
    invariant(&state);
}
static void explicit_source_binding(void)
{
    X4NackGraceState state; x4_nack_grace_init(&state, 77, true);
    CHECK(offer(&state, 1, 100, 7, 100, 100) == X4_GRACE_OFFER_STALE_SOURCE);
    CHECK(!state.bound && !state.seen_request_ids);
    x4_nack_grace_bind_source(&state, 23, 9);
    CHECK(offer(&state, 1, 100, 7, 100, 101) == X4_GRACE_OFFER_ADMITTED);
    CHECK(state.ssrc == 23 && state.sequence_epoch == 9);
    invariant(&state);
}
static void absolute_deadline_without_new_packets(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 7, 1000, 1001) == X4_GRACE_OFFER_ADMITTED);
    X4NackGraceDecision d = query(&state, 100, 45999);
    CHECK(d.reason == X4_GRACE_HOLD && d.deadline_us == 46000);
    /* Receipt/AUTH alone is not an engine call and cannot complete a range. */
    CHECK(state.ranges[0].pending_mask == 7 && !state.counters.completed_ranges);
    d = query(&state, 100, 46000);
    CHECK(d.reason == X4_GRACE_DEADLINE && d.effects.deadline == 1);
    CHECK(!state.active_count && state.ranges[0].release_reason == X4_GRACE_RELEASE_DEADLINE);
    CHECK(query(&state, 100, 90000).reason == X4_GRACE_NONE);
    CHECK(state.counters.deadline_ranges == 1);
    invariant(&state);
}
static void worker_acceptance_partial_and_complete(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 7, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(!accepted(&state, 102, 2).completed);
    CHECK(state.ranges[0].pending_mask == 3 && state.ranges[0].accepted_mask == 4);
    CHECK(!accepted(&state, 102, 3).completed && state.counters.accepted_positions == 1);
    CHECK(query(&state, 100, 4).reason == X4_GRACE_HOLD);
    CHECK(!accepted(&state, 100, 5).completed);
    X4NackGraceDecision d = query(&state, 101, 6);
    CHECK(d.reason == X4_GRACE_HOLD && d.deadline_us == 45000);
    CHECK(accepted(&state, 101, 7).completed == 1);
    CHECK(state.counters.accepted_positions == 3 && state.counters.completed_ranges == 1);
    CHECK(query(&state, 101, 8).reason == X4_GRACE_NONE);
    invariant(&state);
}
static void wrong_source_acceptance_is_ignored(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(!x4_nack_grace_accepted_wire(&state, 24, 9, 100, 2).completed);
    CHECK(!x4_nack_grace_accepted_wire(&state, 23, 8, 100, 2).completed);
    CHECK(state.ranges[0].pending_mask == 1);
    CHECK(accepted(&state, 100, 3).completed == 1);
    invariant(&state);
}
static void accepted_after_deadline_is_not_grace_completion(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 100, 101) == X4_GRACE_OFFER_ADMITTED);
    X4NackGraceEffects e = accepted(&state, 100, 45100);
    CHECK(e.deadline == 1 && !e.completed);
    CHECK(!state.counters.accepted_positions && state.counters.deadline_ranges == 1);
    invariant(&state);
}
static void duplicate_offer_cannot_extend_deadline(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 3, 100, 101) == X4_GRACE_OFFER_ADMITTED);
    CHECK(offer(&state, 1, 100, 3, 30000, 30001) == X4_GRACE_OFFER_DUPLICATE);
    CHECK(state.ranges[0].detected_us == 100 && state.ranges[0].deadline_us == 45100);
    CHECK(query(&state, 100, 45100).reason == X4_GRACE_DEADLINE);
    CHECK(offer(&state, 1, 100, 3, 50000, 50001) == X4_GRACE_OFFER_DUPLICATE);
    invariant(&state);
}
static void overlapping_range_cannot_rearm_old_positions(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 7, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(offer(&state, 2, 101, 7, 20000, 20001) == X4_GRACE_OFFER_ADMITTED);
    CHECK(state.ranges[1].original_mask == 7 && state.ranges[1].granted_mask == 4);
    CHECK(state.counters.overlap_positions == 2);
    X4NackGraceDecision d = query(&state, 101, 44999);
    CHECK(d.reason == X4_GRACE_HOLD && (uint32_t)d.identity == 1 && d.deadline_us == 45000);
    d = query(&state, 101, 45000);
    CHECK(d.reason == X4_GRACE_DEADLINE && d.effects.deadline == 1);
    CHECK(query(&state, 103, 45001).reason == X4_GRACE_HOLD);
    CHECK(offer(&state, 3, 100, 7, 45001, 45002) == X4_GRACE_OFFER_OVERLAP);
    CHECK(query(&state, 100, 45003).reason == X4_GRACE_NONE);
    CHECK(state.ranges[0].deadline_us == 45000 && state.ranges[1].deadline_us == 65000);
    invariant(&state);
}
static void completed_overlap_and_slot_reconciliation(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 7, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    /* Caller reconciles already-STORED slots after a delayed control. */
    accepted(&state, 101, 1); accepted(&state, 102, 1);
    CHECK(state.ranges[0].pending_mask == 1);
    CHECK(accepted(&state, 100, 2).completed == 1);
    CHECK(offer(&state, 2, 100, 7, 100, 101) == X4_GRACE_OFFER_OVERLAP);
    CHECK(!state.active_count);
    invariant(&state);
}
static void ordinary_hole_does_not_receive_grace(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(query(&state, 90, 2).reason == X4_GRACE_NONE);
    CHECK(state.ranges[0].pending_mask == 1);
    CHECK(query(&state, 100, 3).reason == X4_GRACE_HOLD);
    CHECK(query(&state, 101, 4).reason == X4_GRACE_NONE);
    CHECK(state.counters.passed_ranges == 1 && state.counters.passed_positions == 1);
    invariant(&state);
}
static void partial_abandonment_is_not_complete_acceptance(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 7, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(query(&state, 101, 2).reason == X4_GRACE_HOLD);
    CHECK(state.ranges[0].pending_mask == 6 && state.counters.passed_positions == 1);
    accepted(&state, 101, 3);
    X4NackGraceEffects e = accepted(&state, 102, 4);
    CHECK(!e.completed && e.passed == 1);
    CHECK(!state.counters.completed_ranges && state.counters.accepted_positions == 2);
    invariant(&state);
}
static void capacity_fallback_before_insert(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 3, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    X4NackGraceDecision d = x4_nack_grace_query(&state, 100, 127, 127, 2);
    CHECK(d.reason == X4_GRACE_HOLD);
    d = x4_nack_grace_query(&state, 100, 127, 128, 3);
    CHECK(d.reason == X4_GRACE_CAPACITY && d.effects.capacity == 1);
    CHECK(!state.active_count && state.counters.capacity_ranges == 1);
    CHECK(offer(&state, 2, 100, 3, 4, 5) == X4_GRACE_OFFER_OVERLAP);
    init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(x4_nack_grace_query(&state, 100, 128, 127, 2).reason == X4_GRACE_CAPACITY);
    init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(x4_nack_grace_query(&state, 100, 0, UINT32_MAX, 2).reason == X4_GRACE_CAPACITY);
    invariant(&state);
}
static void source_change_does_not_refund_request_identity(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(x4_nack_grace_bind_source(&state, 24, 10).invalidated == 1);
    CHECK(offer(&state, 2, 101, 1, 2, 3) == X4_GRACE_OFFER_STALE_SOURCE);
    X4NackGraceRequest r = request(1, 100, 1, 3); r.ssrc = 24; r.sequence_epoch = 10;
    CHECK(x4_nack_grace_offer(&state, &r, 3, NULL) == X4_GRACE_OFFER_DUPLICATE);
    r.identity = (UINT64_C(1) << 32) | 2;
    CHECK(x4_nack_grace_offer(&state, &r, 4, NULL) == X4_GRACE_OFFER_ADMITTED);
    CHECK(!accepted(&state, 100, 5).completed);
    CHECK(x4_nack_grace_accepted_wire(&state, 24, 10, 100, 5).completed == 2);
    invariant(&state);
}
static void epoch_change_with_same_source_retires_old_ranges(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(x4_nack_grace_bind_source(&state, 23, 10).invalidated == 1);
    CHECK(offer(&state, 2, 100, 1, 2, 3) == X4_GRACE_OFFER_STALE_SOURCE);
    CHECK(state.counters.source_retired_ranges == 1);
    invariant(&state);
}
static void wrap_and_sparse_mask(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 65534, 11, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(query(&state, 65534, 2).reason == X4_GRACE_HOLD);
    accepted(&state, 65535, 3);
    CHECK(query(&state, 0, 4).reason == X4_GRACE_NONE);
    CHECK(state.ranges[0].pending_mask == 8); /*65534 passed;65535 accepted*/
    CHECK(!accepted(&state, 0, 5).completed);
    CHECK(query(&state, 1, 6).reason == X4_GRACE_HOLD);
    CHECK(accepted(&state, 1, 7).passed == 1);
    CHECK(state.counters.accepted_positions == 2 && !state.counters.completed_ranges);
    invariant(&state);
}
static void all_wire_start_sequences_wrap_correctly(void)
{
    for (uint32_t start = 0; start < 65536; ++start) {
        X4NackGraceState state; init(&state, true);
        CHECK(offer(&state, 1, start, 7, 0, 1) == X4_GRACE_OFFER_ADMITTED);
        CHECK(query(&state, (uint16_t)start, 2).reason == X4_GRACE_HOLD);
        CHECK(!accepted(&state, (uint16_t)(start + 2), 3).completed);
        CHECK(!accepted(&state, (uint16_t)start, 4).completed);
        CHECK(query(&state, (uint16_t)(start + 1), 5).reason == X4_GRACE_HOLD);
        CHECK(accepted(&state, (uint16_t)(start + 1), 6).completed == 1);
        CHECK(!state.active_count && !state.counters.pending_positions);
    }
}
static void maximum_bitmap_and_request_ordinals(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 64, 65520, UINT64_MAX, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(state.seen_request_ids == (UINT64_C(1) << 63));
    for (unsigned i = 0; i < 63; ++i) CHECK(!accepted(&state, (uint16_t)(65520 + i), 2 + i).completed);
    CHECK(accepted(&state, (uint16_t)(65520 + 63), 65).completed == (UINT64_C(1) << 63));
    CHECK(state.counters.accepted_positions == 64);
    invariant(&state);
}
static void simultaneous_active_cap_and_history_retention(void)
{
    X4NackGraceState state; init(&state, true);
    for (unsigned i = 1; i <= 16; ++i)
        CHECK(offer(&state, i, i * 128, 1, 0, i) == X4_GRACE_OFFER_ADMITTED);
    CHECK(offer(&state, 17, 17 * 128, 1, 0, 17) == X4_GRACE_OFFER_ACTIVE_CAP);
    CHECK(accepted(&state, 128, 18).completed == 1);
    CHECK(offer(&state, 17, 17 * 128, 1, 18, 19) == X4_GRACE_OFFER_DUPLICATE);
    CHECK(offer(&state, 18, 18 * 128, 1, 19, 20) == X4_GRACE_OFFER_ADMITTED);
    CHECK(state.counters.active_peak == 16 && state.counters.active_cap_offers == 1);
    invariant(&state);
    x4_nack_grace_close_reason(&state, X4_GRACE_RELEASE_CLOSE);
    invariant(&state);
    init(&state, true);
    for (unsigned i = 1; i <= 64; ++i) {
        CHECK(offer(&state, i, i * 128, 1, i * 10, i * 10 + 1) == X4_GRACE_OFFER_ADMITTED);
        CHECK(accepted(&state, (uint16_t)(i * 128), i * 10 + 2).completed == (UINT64_C(1) << (i - 1)));
    }
    CHECK(state.seen_request_ids == UINT64_MAX && state.counters.completed_ranges == 64);
    CHECK(offer(&state, 1, 128, 1, 700, 701) == X4_GRACE_OFFER_DUPLICATE);
    invariant(&state);
}
static void expired_offer_and_deadline_overflow(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 45000) == X4_GRACE_OFFER_EXPIRED);
    CHECK(!state.active_count);
    CHECK(offer(&state, 1, 100, 1, 45000, 45001) == X4_GRACE_OFFER_DUPLICATE);
    init(&state, true);
    CHECK(offer(&state, 1, 100, 1, UINT64_MAX - 44999, UINT64_MAX - 44998) == X4_GRACE_OFFER_DEADLINE_OVERFLOW);
    CHECK(!state.active_count && state.counters.deadline_overflow_offers == 1);
    invariant(&state);
}
static void invalid_inputs_and_stale_session(void)
{
    X4NackGraceState state; init(&state, true);
    X4NackGraceRequest r = request(1, 100, 1, 0);
    r.session_id = 76;
    CHECK(x4_nack_grace_offer(&state, &r, 1, NULL) == X4_GRACE_OFFER_STALE_SESSION);
    CHECK(!state.seen_request_ids);
    r = request(0, 100, 1, 0);
    CHECK(x4_nack_grace_offer(&state, &r, 2, NULL) == X4_GRACE_OFFER_INVALID);
    r = request(65, 100, 1, 0);
    CHECK(x4_nack_grace_offer(&state, &r, 3, NULL) == X4_GRACE_OFFER_INVALID);
    r = request(1, 100, 0, 0);
    CHECK(x4_nack_grace_offer(&state, &r, 4, NULL) == X4_GRACE_OFFER_INVALID);
    r = request(1, UINT64_MAX, 2, 0);
    CHECK(x4_nack_grace_offer(&state, &r, 5, NULL) == X4_GRACE_OFFER_INVALID);
    r = request(1, 100, 1, 7);
    CHECK(x4_nack_grace_offer(&state, &r, 6, NULL) == X4_GRACE_OFFER_INVALID);
    CHECK(!state.seen_request_ids);
    CHECK(x4_nack_grace_offer(NULL, &r, 6, NULL) == X4_GRACE_OFFER_INVALID);
    CHECK(x4_nack_grace_offer(&state, NULL, 6, NULL) == X4_GRACE_OFFER_INVALID);
    invariant(&state);
}
static void clock_regression_retires_active_without_rearming(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 100, 101) == X4_GRACE_OFFER_ADMITTED);
    X4NackGraceDecision d = query(&state, 100, 100);
    CHECK(d.reason == X4_GRACE_CLOCK && d.effects.invalidated == 1);
    CHECK(state.counters.clock_regressions == 1 && state.counters.clock_retired_ranges == 1);
    CHECK(offer(&state, 1, 100, 1, 102, 103) == X4_GRACE_OFFER_DUPLICATE);
    invariant(&state);
}
static void halfspan_does_not_fabricate_pending_future(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(query(&state, (uint16_t)(100 + 32768), 2).reason == X4_GRACE_NONE);
    CHECK(state.counters.passed_ranges == 1 && !state.active_count);
    invariant(&state);
}
static void transport_failure_cancel_and_close_are_irreversible(void)
{
    X4NackGraceState state; init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(x4_nack_grace_cancel(&state, (UINT64_C(1) << 32) | 1, X4_GRACE_RELEASE_TRANSPORT).invalidated == 1);
    CHECK(query(&state, 100, 2).reason == X4_GRACE_NONE);
    CHECK(offer(&state, 1, 100, 1, 2, 3) == X4_GRACE_OFFER_DUPLICATE);
    CHECK(offer(&state, 2, 200, 1, 3, 4) == X4_GRACE_OFFER_ADMITTED);
    CHECK(x4_nack_grace_close_reason(&state, X4_GRACE_RELEASE_EXPLICIT).invalidated == 2);
    CHECK(state.closed && !state.enabled && state.counters.explicit_retired_ranges == 1);
    CHECK(query(&state, 200, 50000).reason == X4_GRACE_CLOSED);
    CHECK(offer(&state, 3, 300, 1, 50000, 50001) == X4_GRACE_OFFER_CLOSED);
    CHECK(!accepted(&state, 200, 50002).completed);
    CHECK(!x4_nack_grace_close_reason(&state, X4_GRACE_RELEASE_CLOSE).invalidated);
    invariant(&state);
    init(&state, true);
    CHECK(offer(&state, 1, 100, 1, 0, 1) == X4_GRACE_OFFER_ADMITTED);
    CHECK(accepted(&state, 100, 2).completed == 1);
    invariant(&state);
}

int main(void)
{
    CASE(disabled_reference);
    CASE(explicit_source_binding);
    CASE(absolute_deadline_without_new_packets);
    CASE(worker_acceptance_partial_and_complete);
    CASE(wrong_source_acceptance_is_ignored);
    CASE(accepted_after_deadline_is_not_grace_completion);
    CASE(duplicate_offer_cannot_extend_deadline);
    CASE(overlapping_range_cannot_rearm_old_positions);
    CASE(completed_overlap_and_slot_reconciliation);
    CASE(ordinary_hole_does_not_receive_grace);
    CASE(partial_abandonment_is_not_complete_acceptance);
    CASE(capacity_fallback_before_insert);
    CASE(source_change_does_not_refund_request_identity);
    CASE(epoch_change_with_same_source_retires_old_ranges);
    CASE(wrap_and_sparse_mask);
    CASE(all_wire_start_sequences_wrap_correctly);
    CASE(maximum_bitmap_and_request_ordinals);
    CASE(simultaneous_active_cap_and_history_retention);
    CASE(expired_offer_and_deadline_overflow);
    CASE(invalid_inputs_and_stale_session);
    CASE(clock_regression_retires_active_without_rearming);
    CASE(halfspan_does_not_fabricate_pending_future);
    CASE(transport_failure_cancel_and_close_are_irreversible);
    printf("PASS %u cases, %" PRIu64 " checks\n", cases, checks);
    return 0;
}
