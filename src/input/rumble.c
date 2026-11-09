/* SPDX-License-Identifier: GPL-3.0-only */
#include "rumble.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    atomic_flag gate;
    atomic_uint_fast64_t target, received;
    /* Producer/poller exchange one complete pending value under gate. */
    X4RumbleEvent pending_event;
    uint64_t received_us, sequence, pending_generation;
    bool pending;
    /* Main poller only. */
    X4RumbleEvent active_event;
    uint64_t active_since, active_until, active_sequence, active_generation;
    uint8_t last_large, last_small;
} X4RumbleSlot;

struct X4Rumble {
    atomic_bool enabled, routed;
    atomic_uint_fast64_t epoch;
    atomic_uint_fast64_t binary_messages, reports, valid, accepted, malformed;
    atomic_uint_fast64_t unsupported, contention_dropped, inactive_dropped, superseded;
    X4RumbleSlot slot[X4_GAMEPAD_COUNT];
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
    if (p[at] != 0 || p[at + 1] >= X4_GAMEPAD_COUNT) return X4_RUMBLE_UNSUPPORTED;
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
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i) {
        r->slot[i].gate = (atomic_flag)ATOMIC_FLAG_INIT;
        atomic_init(&r->slot[i].target, 0);
        atomic_init(&r->slot[i].received, 0);
    }
    atomic_init(&r->routed, false);
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
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i) {
        X4RumbleSlot *slot = &r->slot[i];
        slot->pending = false;
        slot->sequence = slot->active_sequence = slot->active_until = 0;
        slot->active_generation = 0;
        slot->last_large = slot->last_small = 0;
        atomic_store_explicit(&slot->target, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->received, 0, memory_order_relaxed);
        atomic_flag_clear_explicit(&slot->gate, memory_order_release);
    }
    atomic_store_explicit(&r->routed, false, memory_order_relaxed);
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

bool x4_rumble_set_targets(X4Rumble *r, uint8_t mask,
    const uint64_t generations[X4_GAMEPAD_COUNT])
{
    if (!r || !generations || !atomic_load_explicit(&r->enabled, memory_order_acquire))
        return false;
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i)
        atomic_store_explicit(&r->slot[i].target,
            (mask & (1u << i)) ? generations[i] : 0, memory_order_release);
    atomic_store_explicit(&r->routed, true, memory_order_release);
    return true;
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
    unsigned index = event.gamepad_index;
    X4RumbleSlot *slot = &r->slot[index];
    atomic_fetch_add_explicit(&slot->received, 1, memory_order_relaxed);
    if (index && !atomic_load_explicit(&r->routed, memory_order_acquire)) {
        atomic_fetch_add_explicit(&r->unsupported, 1, memory_order_relaxed);
        return X4_RUMBLE_UNSUPPORTED;
    }
    atomic_fetch_add_explicit(&r->valid, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&r->enabled, memory_order_acquire)) {
        atomic_fetch_add_explicit(&r->inactive_dropped, 1, memory_order_relaxed);
        return X4_RUMBLE_INACTIVE;
    }
    if (atomic_flag_test_and_set_explicit(&slot->gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&r->contention_dropped, 1, memory_order_relaxed);
        return X4_RUMBLE_CONTENDED;
    }
#ifdef X4_RUMBLE_TEST_HOOKS
    x4_rumble_test_after_gate(r);
#endif
    bool routed = atomic_load_explicit(&r->routed, memory_order_acquire);
    uint64_t generation = routed ? atomic_load_explicit(&slot->target, memory_order_acquire) : 0;
    int result = X4_RUMBLE_VALID;
    /* Poll checks the tag again. Rebinding never transfers an older pulse
     * to a different pad even when a receive raced the binding update. */
    if (!atomic_load_explicit(&r->enabled, memory_order_acquire) || (routed && !generation)) {
        atomic_fetch_add_explicit(&r->inactive_dropped, 1, memory_order_relaxed);
        result = X4_RUMBLE_INACTIVE;
    } else {
        if (slot->pending) atomic_fetch_add_explicit(&r->superseded, 1, memory_order_relaxed);
        slot->pending_event = event;
        slot->received_us = now_us;
        if (slot->sequence != UINT64_MAX) ++slot->sequence;
        slot->pending_generation = generation;
        slot->pending = true;
        atomic_fetch_add_explicit(&r->accepted, 1, memory_order_relaxed);
    }
    atomic_flag_clear_explicit(&slot->gate, memory_order_release);
    return result;
}

static bool poll_slot(X4Rumble *r, X4RumbleSlot *slot, bool routed, uint64_t now_us,
    X4RumbleMotorOutput *out)
{
    uint64_t generation = routed ? atomic_load_explicit(&slot->target, memory_order_acquire) : 0;
    out->epoch = atomic_load_explicit(&r->epoch, memory_order_acquire);
    if (atomic_load_explicit(&r->enabled, memory_order_acquire) &&
        !atomic_flag_test_and_set_explicit(&slot->gate, memory_order_acquire)) {
        if (routed) generation = atomic_load_explicit(&slot->target, memory_order_acquire);
        /* The producer may receive after the caller sampled now_us. Leave
         * a current pending value intact until a later poll reaches it. */
        if (slot->pending && (slot->pending_generation != generation || (routed && !generation)))
            slot->pending = false;
        if (slot->pending && slot->received_us <= now_us) {
            slot->active_event = slot->pending_event;
            slot->active_since = slot->received_us;
            slot->active_sequence = slot->sequence;
            slot->active_generation = slot->pending_generation;
            slot->pending = false;
            uint64_t ms = slot->active_event.duration_ms;
            if (ms > X4_RUMBLE_MAX_DURATION_MS) ms = X4_RUMBLE_MAX_DURATION_MS;
            uint64_t usec = ms * 1000u;
            slot->active_until = usec && slot->active_since <= UINT64_MAX - usec
                ? slot->active_since + usec : 0;
        }
        atomic_flag_clear_explicit(&slot->gate, memory_order_release);
    }
    if (atomic_load_explicit(&r->enabled, memory_order_acquire) &&
        slot->active_generation == generation && (!routed || generation) &&
        slot->active_until && now_us >= slot->active_since && now_us < slot->active_until) {
        out->lgMotor = x4_rumble_motor_byte(slot->active_event.left_motor);
        out->smMotor = x4_rumble_motor_byte(slot->active_event.right_motor);
    } else slot->active_until = 0;
    out->sequence = slot->active_sequence;
    out->generation = generation;
    bool changed = out->lgMotor != slot->last_large || out->smMotor != slot->last_small;
    slot->last_large = out->lgMotor;
    slot->last_small = out->smMotor;
    return changed;
}

bool x4_rumble_poll(X4Rumble *r, uint64_t now_us, X4RumbleOutput *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!r) return false;
    bool routed = atomic_load_explicit(&r->routed, memory_order_acquire);
    X4RumbleMotorOutput first = {0};
    bool changed = poll_slot(r, &r->slot[0], routed, now_us, &first);
    out->lgMotor = first.lgMotor;
    out->smMotor = first.smMotor;
    out->epoch = first.epoch;
    out->sequence = first.sequence;
    out->generation = first.generation;
    if (routed)
        for (unsigned i = 1; i < X4_GAMEPAD_COUNT; ++i)
            changed |= poll_slot(r, &r->slot[i], true, now_us, &out->others[i - 1]);
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
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i)
        out->received_by_index[i] = atomic_load_explicit(&r->slot[i].received, memory_order_relaxed);
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
