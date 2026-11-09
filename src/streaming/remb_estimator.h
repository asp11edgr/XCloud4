/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    X4_REMB_ESTIMATOR_INTERVAL_US = 1000000,
    X4_REMB_ESTIMATOR_MIN_BPS = 3000000
};

/* Owned by the session's control loop, not the RTP callbacks. */
typedef struct {
    uint64_t last_update_us;
    uint32_t cap_bps;
    uint32_t target_bps;
    uint8_t stable_seconds;
} X4RembEstimator;

static inline void x4_remb_estimator_init(X4RembEstimator *estimator,
    uint32_t cap_bps, uint64_t now_us)
{
    if (cap_bps < X4_REMB_ESTIMATOR_MIN_BPS)
        cap_bps = X4_REMB_ESTIMATOR_MIN_BPS;
    *estimator = (X4RembEstimator){
        .last_update_us = now_us,
        .cap_bps = cap_bps,
        .target_bps = cap_bps
    };
}

/* Returns true when a new measurement was consumed. Apply a changed target
 * immediately after a true result. A call before the next interval cannot
 * change the target, even if the same bad measurements are supplied again. */
static inline bool x4_remb_estimator_update(X4RembEstimator *estimator,
    uint64_t now_us, uint32_t received_bps, uint32_t loss_ppm, int32_t filtered_delay_us)
{
    if (now_us < estimator->last_update_us)
        return false;
    const uint64_t elapsed_us = now_us - estimator->last_update_us;
    if (elapsed_us < X4_REMB_ESTIMATOR_INTERVAL_US)
        return false;
    estimator->last_update_us = now_us;
    /* A delayed poll can represent only one observed window. */
    if (elapsed_us >= 2 * X4_REMB_ESTIMATOR_INTERVAL_US)
        estimator->stable_seconds = 0;

    if (loss_ppm > 20000 || filtered_delay_us > 30000) {
        estimator->stable_seconds = 0;
        const uint32_t reduced = (uint32_t)((uint64_t)received_bps * 85 / 100);
        const uint32_t target = reduced < X4_REMB_ESTIMATOR_MIN_BPS ?
            X4_REMB_ESTIMATOR_MIN_BPS : reduced;
        estimator->target_bps = target > estimator->cap_bps ? estimator->cap_bps : target;
    } else if (received_bps == 0) {
        /* An interval with no video does not establish a stable link. */
        estimator->stable_seconds = 0;
    } else {
        if (estimator->stable_seconds < 3)
            ++estimator->stable_seconds;
        if (estimator->stable_seconds == 3) {
            const uint64_t raised = (uint64_t)estimator->target_bps * 108 / 100;
            estimator->target_bps = raised > estimator->cap_bps ?
                estimator->cap_bps : (uint32_t)raised;
        }
    }
    return true;
}

/* A failed send still consumes its attempt. A zero previous bitrate means
 * no attempt yet, including when a new peer's clock starts at zero. */
static inline bool x4_remb_send_due(uint64_t now_us, uint64_t last_attempt_us,
    uint32_t last_attempt_bps, uint32_t target_bps)
{
    if (!target_bps || now_us < last_attempt_us)
        return false;
    if (!last_attempt_bps)
        return true;
    if ((uint64_t)target_bps * 100 < (uint64_t)last_attempt_bps * 95)
        return true;
    return now_us - last_attempt_us >= X4_REMB_ESTIMATOR_INTERVAL_US;
}

#if X4_PUBLIC
/* Only numeric, session-wide totals; the receive and send identifiers stay
 * private to the transport. A negative observed delay peak is valid. */
typedef struct {
    uint64_t sent, decreases, increases;
    uint32_t sent_bps, min_sent_bps, max_sent_bps, received_bps, max_loss_ppm;
    int32_t max_delay_us;
    bool have_max_delay;
} X4RembSummary;

struct X4Trace;
/* Called by the public exporter after all RTC owners have joined. */
void x4_rtc_remb_finish_summary(const struct X4Trace *trace, uint64_t pipeline_started_us,
    X4RembSummary *out);
#endif
