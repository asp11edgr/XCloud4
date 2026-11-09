/* SPDX-License-Identifier: GPL-3.0-only */
#include "remb_estimator.h"
#include <assert.h>
#include <stdio.h>

static void sample(X4RembEstimator *estimator, uint64_t now_us,
    uint32_t rate, uint32_t loss, int32_t delay, uint32_t expected)
{
    assert(x4_remb_estimator_update(estimator, now_us, rate, loss, delay));
    assert(estimator->target_bps == expected);
    assert(estimator->last_update_us == now_us);
}

static void stable_at_cap(void)
{
    const uint32_t caps[] = {10000000, 12000000, 20000000, 30000000};
    for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
        X4RembEstimator estimator;
        x4_remb_estimator_init(&estimator, caps[i], 0);
        assert(estimator.target_bps == caps[i]);
        for (unsigned second = 1; second <= 8; ++second)
            sample(&estimator, second * UINT64_C(1000000), caps[i], 0, -12000, caps[i]);
        assert(estimator.stable_seconds == 3);
    }
}

static void loss_delay_and_recovery(void)
{
    X4RembEstimator estimator;
    x4_remb_estimator_init(&estimator, 20000000, 0);
    /* Three percent residual loss triggers the drop on the first sample. */
    sample(&estimator, 1000000, 16000000, 30000, 0, 13600000);
    assert(estimator.stable_seconds == 0);
    for (uint64_t now = 1000000; now < 2000000; now += 1000) {
        assert(!x4_remb_estimator_update(&estimator, now, 1000000, 40000, 60000));
        assert(estimator.target_bps == 13600000);
    }
    /* Thresholds are strict, and the third stable window raises A by 8%. */
    sample(&estimator, 2000000, 16000000, 20000, 30000, 13600000);
    sample(&estimator, 3000000, 16000000, 10000, -10000, 13600000);
    sample(&estimator, 4000000, 16000000, 0, 0, 14688000);
    sample(&estimator, 5000000, 16000000, 0, 0, 15863040);
    /* Delay on its own also reduces the target and restarts the streak. */
    sample(&estimator, 6000000, 12000000, 0, 40000, 10200000);
    sample(&estimator, 7000000, 12000000, 0, 0, 10200000);
    sample(&estimator, 8000000, 12000000, 0, 0, 10200000);
    sample(&estimator, 9000000, 12000000, 0, 0, 11016000);
}

static void missed_and_empty_windows(void)
{
    X4RembEstimator estimator;
    x4_remb_estimator_init(&estimator, 20000000, 0);
    sample(&estimator, 1000000, 3000000, 30000, 0, 3000000);
    sample(&estimator, 2000000, 4000000, 0, 0, 3000000);
    sample(&estimator, 3000000, 4000000, 0, 0, 3000000);
    sample(&estimator, 8000000, 4000000, 0, 0, 3000000);
    assert(estimator.stable_seconds == 1);
    sample(&estimator, 9000000, 4000000, 0, 0, 3000000);
    sample(&estimator, 10000000, 0, 0, 0, 3000000);
    assert(estimator.stable_seconds == 0);
    sample(&estimator, 11000000, 4000000, 0, 0, 3000000);
    sample(&estimator, 12000000, 4000000, 0, 0, 3000000);
    sample(&estimator, 13000000, 4000000, 0, 0, 3240000);
    x4_remb_estimator_init(&estimator, 12000000, 16000000);
    assert(estimator.target_bps == 12000000 && estimator.stable_seconds == 0);
    assert(!x4_remb_estimator_update(&estimator, 15999999, 0, 0, 0));
    assert(!x4_remb_estimator_update(&estimator, 16999999, 0, 0, 0));
}

static void wide_arithmetic_and_bounds(void)
{
    X4RembEstimator estimator;
    const uint64_t last_start = UINT64_MAX - 1000000;
    x4_remb_estimator_init(&estimator, 0, last_start);
    assert(estimator.target_bps == X4_REMB_ESTIMATOR_MIN_BPS);
    sample(&estimator, UINT64_MAX, 0, UINT32_MAX, INT32_MIN, 3000000);
    assert(!x4_remb_estimator_update(&estimator, 0, 0, 0, 0));

    x4_remb_estimator_init(&estimator, UINT32_MAX, 0);
    for (unsigned second = 1; second <= 3; ++second)
        sample(&estimator, second * UINT64_C(1000000), UINT32_MAX, 0, 0, UINT32_MAX);
    sample(&estimator, 4000000, UINT32_MAX, 0, INT32_MAX, UINT32_C(3650722200));
    x4_remb_estimator_init(&estimator, 10000000, 0);
    sample(&estimator, 1000000, UINT32_MAX, UINT32_MAX, 0, 10000000);
}

static void send_cadence(void)
{
    const uint32_t fixed = 20000000;
    assert(!x4_remb_send_due(0, 0, 0, 0));
    assert(x4_remb_send_due(0, 0, 0, fixed));
    assert(!x4_remb_send_due(0, 0, fixed, fixed));
    assert(!x4_remb_send_due(999999, 0, fixed, fixed));
    assert(!x4_remb_send_due(999999, 0, fixed, 21000000));
    assert(x4_remb_send_due(1000000, 0, fixed, fixed));
    assert(x4_remb_send_due(1000000, 0, fixed, 21000000));
    assert(!x4_remb_send_due(1000, 0, fixed, 19000000)); /* exactly 5% */
    assert(x4_remb_send_due(1000, 0, fixed, 18999999));
    assert(!x4_remb_send_due(999, 1000, fixed, 3000000));
    assert(x4_remb_send_due(1000, 1000, UINT32_MAX, 3000000));
    assert(!x4_remb_send_due(1000, 1000, UINT32_MAX, UINT32_MAX));
    /* The urgent request was attempted and rejected. No busy retry, but
     * the unchanged target may be attempted again after the interval. */
    assert(!x4_remb_send_due(1001, 1000, 18999999, 18999999));
    assert(!x4_remb_send_due(1000999, 1000, 18999999, 18999999));
    assert(x4_remb_send_due(1001000, 1000, 18999999, 18999999));
}

int main(void)
{
    stable_at_cap();
    loss_delay_and_recovery();
    missed_and_empty_windows();
    wide_arithmetic_and_bounds();
    send_cadence();
    puts("PASS REMB estimator: stable, residual loss, queue, recovery, pacing, reset and bounds");
    return 0;
}
