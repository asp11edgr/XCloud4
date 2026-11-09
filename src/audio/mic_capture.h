/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { X4_MIC_PAYLOAD_CAP = 1180 };

typedef enum {
    X4_MIC_OFF, X4_MIC_WAITING, X4_MIC_ACTIVE, X4_MIC_MUTED,
    X4_MIC_UNAVAILABLE, X4_MIC_REJECTED
} X4MicStatus;

typedef struct {
    bool enabled, muted, active;
    X4MicStatus status;
    unsigned level, rate, frame_ms;
} X4MicSnapshot;

typedef struct {
    uint64_t generation;
    unsigned frame_ms;
} X4MicRequest;

typedef struct {
    uint64_t captured_us, sample_us, epoch;
    uint16_t samples, size;
    uint8_t data[X4_MIC_PAYLOAD_CAP];
} X4MicFrame;

/* Main arms only the initial signed-in user. The next RTC peer snapshots
 * this intent, and separately binds capture only after the answer opens it. */
uint64_t x4_mic_session_begin(bool enabled, int32_t initial_user, unsigned frame_ms);
void x4_mic_session_end(void);
void x4_mic_toggle_mute(void);
X4MicSnapshot x4_mic_snapshot(void);
X4MicRequest x4_mic_request(void);

/* `peer` is a nonzero, increasing transport slot within the generation. */
bool x4_mic_capture_attach(uint64_t generation, uint64_t peer);
void x4_mic_capture_detach(uint64_t generation, uint64_t peer);
void x4_mic_capture_reject(uint64_t generation);
/* Sender-only. `now_us`, `captured_us` and `sample_us` share the monotonic
 * process-time clock. The returned bytes are owned by the caller. */
bool x4_mic_capture_take(uint64_t generation, uint64_t peer, uint64_t now_us, X4MicFrame *frame);
bool x4_mic_capture_send_allowed(uint64_t generation, uint64_t peer, uint64_t epoch);

#ifdef X4_MIC_CAPTURE_TEST
int x4_mic_test_audio_open(int32_t user, unsigned rate, unsigned port, unsigned samples);
int x4_mic_test_audio_read(int handle, int16_t *pcm);
int x4_mic_test_audio_state(int handle);
int x4_mic_test_audio_close(int handle);
bool x4_mic_capture_test_idle(void);
unsigned x4_mic_capture_test_pending(void);
#endif
