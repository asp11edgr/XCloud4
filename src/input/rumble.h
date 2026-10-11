/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Original bounded Xbox FourMotorRumble decoder. No hardware API is called
 * here. The data-channel producer and the main-thread poller never wait. */
typedef struct X4Rumble X4Rumble;
typedef struct {
    uint8_t gamepad_index, left_motor, right_motor, left_trigger, right_trigger;
    uint16_t duration_ms, delay_ms;
    uint8_t repeat;
} X4RumbleEvent;
typedef struct {
    uint8_t lgMotor, smMotor;
    uint64_t epoch, sequence;
} X4RumbleOutput;
typedef struct {
    uint64_t binary_messages, reports, valid, accepted, malformed, unsupported;
    uint64_t contention_dropped, inactive_dropped, superseded;
    uint64_t epoch;
    bool enabled;
} X4RumbleSnapshot;
enum {
    X4_RUMBLE_OTHER = 0, X4_RUMBLE_VALID = 1,
    X4_RUMBLE_MALFORMED = -1, X4_RUMBLE_UNSUPPORTED = -2,
    X4_RUMBLE_INACTIVE = -3, X4_RUMBLE_CONTENDED = -4
};
#define X4_RUMBLE_MAX_DURATION_MS 5000u

/* Exact modern13 / legacy12 layouts, type128/subtype0/index0, percentages
 * <=100. Unknown/combined report types are not guessed. */
int x4_rumble_parse(const void *data, size_t size, X4RumbleEvent *event);
uint8_t x4_rumble_motor_byte(uint8_t percent);
X4Rumble *x4_rumble_new(void);
/* Main/session lifecycle only, after all previous RTC producers joined.
 * A fresh nonzero epoch is assigned; cannot begin while already enabled. */
bool x4_rumble_begin(X4Rumble *);
/* May run concurrently with receive/poll. Poll immediately returns zero.
 * The caller must also physically zero the controller on every close path. */
void x4_rumble_disable(X4Rumble *);
/* Optional borrowed mailbox outlives every joined RTC producer. Callback
 * receives bytes only; no allocation, print, sleep, or hardware operation. */
int x4_rumble_receive(X4Rumble *, const void *data, size_t size, uint64_t now_us);
/* MAIN ONLY. Always initializes out (including NULL/disabled). Returns true
 * if the desired two motor bytes changed since the previous poll. Latest
 * event wins; one pulse, capped5000ms, duration0 stops. Delay/repeat/trigger
 * fields are decoded but deliberately not applied in this first version. */
bool x4_rumble_poll(X4Rumble *, uint64_t now_us, X4RumbleOutput *out);
void x4_rumble_snapshot(const X4Rumble *, X4RumbleSnapshot *out);
/* Requires disable + externally joined producers/poller. false if enabled;
 * this is a lifecycle contract, not proof of external callback quiescence. */
bool x4_rumble_free(X4Rumble *);
#ifdef X4_RUMBLE_TEST_HOOKS
/* Host-test rendezvous only; absent from production. */
void x4_rumble_test_after_gate(X4Rumble *);
#endif
