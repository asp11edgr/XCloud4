/* SPDX-License-Identifier: GPL-3.0-only */
#include "rumble.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct X4Rumble {
    atomic_flag gate;
    atomic_bool enabled;
    atomic_uint_fast64_t epoch;
    atomic_uint_fast64_t binary_messages, reports, valid, accepted, malformed;
    atomic_uint_fast64_t unsupported, contention_dropped, inactive_dropped, superseded;
    /* gate-owned incoming slot; overwritten only as a complete value. */
    X4RumbleEvent pending_event;
    uint64_t received_us, sequence;
    bool pending;
    /* Main poller only; no producer reads or writes these fields. */
    X4RumbleEvent active_event;
    uint64_t active_since, active_until, active_sequence;
    uint8_t last_large, last_small;
};

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int x4_rumble_parse(const void *data, size_t size, X4RumbleEvent *event)
{
    if (event) memset(event, 0, sizeof(*event));
    if (!data || !size || !event) return X4_RUMBLE_MALFORMED;
    const uint8_t *p = data;
    if (p[0] != 128) return X4_RUMBLE_OTHER;
    if (size != 12 && size != 13) return X4_RUMBLE_MALFORMED;
    size_t at = size == 13 ? 2 : 1;
    /* A combined report has no verified layout; do not interpret it. */
    if (size == 13 && le16(p) != 128) return X4_RUMBLE_UNSUPPORTED;
    if (p[at] != 0 || p[at + 1] != 0) return X4_RUMBLE_UNSUPPORTED;
    for (size_t i = at + 2; i < at + 6; ++i)
        if (p[i] > 100) return X4_RUMBLE_MALFORMED;
    event->gamepad_index = p[at + 1];
    event->left_motor = p[at + 2];
    event->right_motor = p[at + 3];
    event->left_trigger = p[at + 4];
    event->right_trigger = p[at + 5];
    event->duration_ms = le16(p + at + 6);
    event->delay_ms = le16(p + at + 8);
    event->repeat = p[at + 10];
    return X4_RUMBLE_VALID;
}

uint8_t x4_rumble_motor_byte(uint8_t percent)
{
    if (percent > 100) percent = 100;
    return (uint8_t)(((unsigned)percent * 255u + 50u) / 100u);
}

X4Rumble *x4_rumble_new(void)
{
    X4Rumble *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->gate = (atomic_flag)ATOMIC_FLAG_INIT;
    atomic_init(&r->enabled, false);
    atomic_init(&r->epoch, 0);
#define INIT(field) atomic_init(&r->field, 0)
    INIT(binary_messages); INIT(reports); INIT(valid); INIT(accepted);
    INIT(malformed); INIT(unsupported); INIT(contention_dropped);
    INIT(inactive_dropped); INIT(superseded);
#undef INIT
    return r;
}

bool x4_rumble_begin(X4Rumble *r)
{
    if (!r || atomic_load_explicit(&r->enabled, memory_order_acquire)) return false;
    /* Lifecycle owner has joined old producers and poller. A fresh epoch
     * prevents a previous session's pending value from becoming actionable. */
    uint64_t epoch = atomic_load_explicit(&r->epoch, memory_order_relaxed);
    if (epoch == UINT64_MAX) return false;
    memset(&r->pending_event, 0, sizeof(r->pending_event));
    memset(&r->active_event, 0, sizeof(r->active_event));
    r->pending = false;
    r->received_us = r->sequence = r->active_since = r->active_until = r->active_sequence = 0;
    r->last_large = r->last_small = 0;
    atomic_flag_clear_explicit(&r->gate, memory_order_release);
#define RESET(field) atomic_store_explicit(&r->field, 0, memory_order_relaxed)
    RESET(binary_messages); RESET(reports); RESET(valid); RESET(accepted);
    RESET(malformed); RESET(unsupported); RESET(contention_dropped);
    RESET(inactive_dropped); RESET(superseded);
#undef RESET
    atomic_store_explicit(&r->epoch, epoch + 1, memory_order_release);
    atomic_store_explicit(&r->enabled, true, memory_order_release);
    return true;
}

void x4_rumble_disable(X4Rumble *r)
{
    if (r) atomic_store_explicit(&r->enabled, false, memory_order_release);
}

int x4_rumble_receive(X4Rumble *r, const void *data, size_t size, uint64_t now_us)
{
    if (!r) return X4_RUMBLE_INACTIVE;
    atomic_fetch_add_explicit(&r->binary_messages, 1, memory_order_relaxed);
    X4RumbleEvent event;
    int parsed = x4_rumble_parse(data, size, &event);
    if (parsed == X4_RUMBLE_OTHER) return parsed;
    atomic_fetch_add_explicit(&r->reports, 1, memory_order_relaxed);
    if (parsed < 0) {
        if (parsed == X4_RUMBLE_UNSUPPORTED)
            atomic_fetch_add_explicit(&r->unsupported, 1, memory_order_relaxed);
        else atomic_fetch_add_explicit(&r->malformed, 1, memory_order_relaxed);
        return parsed;
    }
    atomic_fetch_add_explicit(&r->valid, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&r->enabled, memory_order_acquire)) {
        atomic_fetch_add_explicit(&r->inactive_dropped, 1, memory_order_relaxed);
        return X4_RUMBLE_INACTIVE;
    }
    if (atomic_flag_test_and_set_explicit(&r->gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&r->contention_dropped, 1, memory_order_relaxed);
        return X4_RUMBLE_CONTENDED;
    }
#ifdef X4_RUMBLE_TEST_HOOKS
    x4_rumble_test_after_gate(r);
#endif
    int result = X4_RUMBLE_VALID;
    /* Disable can race an admitted producer; recheck while holding the
     * slot gate. Poll also rechecks enabled, so stale values never act. */
    if (!atomic_load_explicit(&r->enabled, memory_order_acquire)) {
        atomic_fetch_add_explicit(&r->inactive_dropped, 1, memory_order_relaxed);
        result = X4_RUMBLE_INACTIVE;
    } else {
        if (r->pending) atomic_fetch_add_explicit(&r->superseded, 1, memory_order_relaxed);
        r->pending_event = event;
        r->received_us = now_us;
        if (r->sequence != UINT64_MAX) ++r->sequence;
        r->pending = true;
        atomic_fetch_add_explicit(&r->accepted, 1, memory_order_relaxed);
    }
    atomic_flag_clear_explicit(&r->gate, memory_order_release);
    return result;
}

bool x4_rumble_poll(X4Rumble *r, uint64_t now_us, X4RumbleOutput *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!r) return false;
    out->epoch = atomic_load_explicit(&r->epoch, memory_order_acquire);
    if (atomic_load_explicit(&r->enabled, memory_order_acquire) &&
        !atomic_flag_test_and_set_explicit(&r->gate, memory_order_acquire)) {
        /* The producer may have received this report after the caller took
         * now_us, even with a fresh clock at the poll call. Leave that whole
         * pending value intact until a later poll reaches its receipt time. */
        if (r->pending && r->received_us <= now_us) {
            r->active_event = r->pending_event;
            r->active_since = r->received_us;
            r->active_sequence = r->sequence;
            r->pending = false;
            uint64_t ms = r->active_event.duration_ms;
            if (ms > X4_RUMBLE_MAX_DURATION_MS) ms = X4_RUMBLE_MAX_DURATION_MS;
            uint64_t usec = ms * 1000u;
            r->active_until = usec && r->active_since <= UINT64_MAX - usec
                ? r->active_since + usec : 0;
        }
        atomic_flag_clear_explicit(&r->gate, memory_order_release);
    }
    if (atomic_load_explicit(&r->enabled, memory_order_acquire) &&
        r->active_until && now_us >= r->active_since && now_us < r->active_until) {
        out->lgMotor = x4_rumble_motor_byte(r->active_event.left_motor);
        out->smMotor = x4_rumble_motor_byte(r->active_event.right_motor);
    } else r->active_until = 0; /* Expired, disabled, or backward local clock. */
    out->sequence = r->active_sequence;
    bool changed = out->lgMotor != r->last_large || out->smMotor != r->last_small;
    r->last_large = out->lgMotor;
    r->last_small = out->smMotor;
    return changed;
}

void x4_rumble_snapshot(const X4Rumble *r, X4RumbleSnapshot *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!r) return;
#define READ(field) out->field = atomic_load_explicit(&r->field, memory_order_relaxed)
    READ(binary_messages); READ(reports); READ(valid); READ(accepted);
    READ(malformed); READ(unsupported); READ(contention_dropped);
    READ(inactive_dropped); READ(superseded); READ(epoch);
#undef READ
    out->enabled = atomic_load_explicit(&r->enabled, memory_order_acquire);
}

bool x4_rumble_free(X4Rumble *r)
{
    if (!r) return true;
    if (atomic_load_explicit(&r->enabled, memory_order_acquire)) return false;
    /* External join precondition applies even though admission is disabled. */
    free(r);
    return true;
}
