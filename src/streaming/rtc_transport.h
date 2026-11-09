/* SPDX-License-Identifier: GPL-3.0-only */
/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../input/gamepad.h"
#include "stream_profile.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct X4Rtc X4Rtc;
typedef struct X4Trace X4Trace;
typedef struct X4Rumble X4Rumble;
typedef struct X4NackGraceInbox X4NackGraceInbox;
typedef struct X4StreamStartup X4StreamStartup;
enum { X4_RTC_VIDEO = 0, X4_RTC_AUDIO = 1 };
typedef enum {
    X4_RTC_NEW = 0, X4_RTC_CONNECTING, X4_RTC_CONNECTED,
    X4_RTC_DISCONNECTED, X4_RTC_FAILED, X4_RTC_CLOSED
} X4RtcState;
typedef struct {
    X4RtcState state;
    int error;
    bool gathering_done;
    uint64_t video_packets, audio_packets;
    bool input_ready;
    int input_error;
    /* Native send calls that succeeded, and superseded/error drops. These
     * do not acknowledge Xbox consumption of controller reports. */
    uint64_t input_packets, input_dropped;
    /* Lifetime notices are data-channel notifications, not RTC failures.
     * The first terminal notice is immutable; idle sequence changes once per
     * accepted warning so the worker can log/display it exactly once. */
    bool server_closed, server_hr_valid;
    uint32_t server_hr;
    char server_reason[41];
    uint64_t server_idle_sequence;
    unsigned server_idle_seconds;
} X4RtcSnapshot;
/* Called on a transport thread, with an authenticated/decrypted RTP packet.
 * The pointer is borrowed for this call only. Copy into a bounded queue;
 * never decode, block or render on this callback. */
typedef void (*X4RtcMediaCallback)(void *user, int kind, const uint8_t *rtp, size_t size);
X4Rtc *x4_rtc_open(int *error);
/* Captures public request values by copy before any RTC callbacks run. */
X4Rtc *x4_rtc_open_request(int *error, const X4StreamRequestConfig *);
/* Same Xbox session, fresh peer. Call only after the prior close succeeds;
 * preserves cumulative receive/reconnection diagnostics. */
X4Rtc *x4_rtc_open_reconnect_request(int *error, const X4StreamRequestConfig *);
void x4_rtc_set_media_callback(X4Rtc *, X4RtcMediaCallback, void *user);
/* Session owner only, before dispatch starts. Trace outlives joined RTC. */
void x4_rtc_set_diagnostic_trace(X4Rtc *, X4Trace *);
/* Optional borrowed storage, bound by owner before remote SDP/callbacks.
 * Both objects outlive all producers and a successful RTC close. */
void x4_rtc_set_rumble_mailbox(X4Rtc *, X4Rumble *);
void x4_rtc_set_nack_grace_inbox(X4Rtc *, X4NackGraceInbox *);
/* Bind before signaling/open callbacks; borrowed until RTC close succeeds.
 * A late bind is rejected rather than inventing an alias-send timestamp. */
int x4_rtc_set_stream_startup(X4Rtc *, X4StreamStartup *);
typedef struct X4KeyframeLimiter X4KeyframeLimiter;
/* Bind before startup sends anything. Streaming dispatch requires an unused
 * reservation from this exact limiter; binding burns older reservations. */
int x4_rtc_set_keyframe_limiter(X4Rtc *, X4KeyframeLimiter *);
/* Owner thread only, once after open. Starts a bounded independent sender;
 * a native thread-creation error is returned without failing media/RTC.
 * Source/context must outlive a successful close. */
int x4_rtc_set_gamepad_source(X4Rtc *, X4GamepadSource, void *user);
/* Returns 1 when copied, 0 while pending, negative on failure. */
int x4_rtc_local_description(X4Rtc *, char *sdp, size_t capacity);
int x4_rtc_next_local_candidate(X4Rtc *, char *candidate, size_t capacity,
                                char *mid, size_t mid_capacity);
int x4_rtc_set_remote_description(X4Rtc *, const char *sdp);
int x4_rtc_add_remote_candidate(X4Rtc *, const char *candidate, const char *mid);
int x4_rtc_get_selected_candidate_pair(X4Rtc *, char *local, size_t local_cap, char *remote, size_t remote_cap);
int x4_rtc_get_rtt_ms(X4Rtc *);
void x4_rtc_get_sdp_video(const X4Rtc *, uint32_t *ssrc, uint32_t *pt, char *mid, size_t mid_cap);
void x4_rtc_snapshot(X4Rtc *, X4RtcSnapshot *);
/* Preserve a queued keyframe request until startup has locally sent its
 * resolution alias. This does not acknowledge the resolution delivered. */
bool x4_rtc_keyframe_startup_ready(X4Rtc *);
/* Streaming owner only, once after a successful limiter take. Failed native
 * calls still consume that reservation; control/PLI local returns are counted
 * separately and do not acknowledge remote keyframe generation. */
int x4_rtc_request_keyframe(X4Rtc *);
/* Joins callbacks before releasing the context. Call only from the owner
 * worker, never from a callback. */
/* A join error retains the complete RTC/provider context. The owner must
 * keep it alive and retry; no callback/channel/context is freed on error. */
int x4_rtc_close(X4Rtc *);
/* Same close, with a final snapshot only after all callbacks have joined.
 * On nonzero return, context is retained and output is not final. This keeps
 * a late server terminal notice from being lost when replacing a peer. */
int x4_rtc_close_snapshot(X4Rtc *, X4RtcSnapshot *final);
#ifdef __cplusplus
}
#endif
