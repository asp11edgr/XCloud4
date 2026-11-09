/* SPDX-License-Identifier: GPL-3.0-only */
/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* Numeric observations plus an explicit bounded repair-control bridge.
 * Also copied into the vendor overlay; control does not depend on tracing. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "nack_sequence.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct X4Trace X4Trace;
enum {
    X4_RD_UDP, X4_RD_ICE, X4_RD_DTLS_ACCEPT, X4_RD_DTLS_POP,
    X4_RD_SRTP_INPUT, X4_RD_SRTP_VALID, X4_RD_PEER, X4_RD_TRACK,
    X4_RD_TRACK_QUEUE, X4_RD_TRACK_POP, X4_RD_APP, X4_RD_RTX,
    X4_RD_TCP, X4_RD_PEER_OUT, X4_RD_RTCP, X4_RD_RESERVED,
    X4_RD_TURN = 15
};
enum {
    X4_RD_UNAUTHENTICATED = 1, X4_RD_AUTHENTICATED = 2,
    X4_RD_RETRANSMISSION = 4, X4_RD_RTX_NORMALIZED = 8,
    X4_RD_TCP_FLAG = 16, X4_RD_RTCP_FLAG = 32,
    /* Trusted app-owned recovery before ReceivingSession, never wire flags. */
    X4_RD_RECOVERY43 = 64, X4_RD_FEC_RECOVERED = 128
};
/* Reason IDs describe the actual branch. Result retains native numeric code. */
enum {
    X4_RD_SOCKET_ERROR = 1, X4_RD_EMPTY_DATAGRAM, X4_RD_SOCKET_POLL_ERROR,
    X4_RD_ICE_INACTIVE, X4_RD_STUN_PARSE, X4_RD_ICE_UNKNOWN_SOURCE,
    X4_RD_ICE_UNEXPECTED, X4_RD_DTLS_QUEUE_FULL, X4_RD_MEDIA_SHORT,
    X4_RD_SRTP_ERROR, X4_RD_SRTCP_ERROR, X4_RD_DEMUX_UNKNOWN,
    X4_RD_PEER_HANDLER_EXCEPTION, X4_RD_NO_TRACK, X4_RD_TRACK_DIRECTION,
    X4_RD_TRACK_HANDLER_EXCEPTION, X4_RD_TRACK_QUEUE_FULL,
    X4_RD_RTP_SHORT, X4_RD_RTP_VERSION, X4_RD_RTP_PAYLOAD,
    X4_RD_RTX_UNWRAP_UNAVAILABLE, X4_RD_APP_INVALID,
    X4_RD_APP_NO_SINK, X4_RD_ICE_CALLBACK_EXCEPTION,
    X4_RD_SOCKET_WOULD_BLOCK, X4_RD_SOCKET_FAIRNESS,
    X4_RD_TURN_INVALID, X4_RD_SRTP_AUTH_FAIL,
    X4_RD_SRTP_REPLAY_FAIL, X4_RD_SRTP_REPLAY_OLD
};
uint64_t x4_rtc_receive_clock(void);
/* Header inspection is bounded, read-only and never retains packet bytes. */
uint64_t x4_rtc_receive_packet(unsigned stage, const void *packet, size_t bytes,
    uint64_t preceding_time, uint32_t flags);
void x4_rtc_receive_reject(unsigned stage, unsigned reason, int result,
    const void *packet, size_t bytes);
typedef struct {
    uint32_t ssrc;
    uint32_t payload_type;
    uint64_t packets;
    uint64_t first_us;
    uint64_t last_us;
} X4InboundRtpStream;
typedef struct {
    uint64_t srtp_decrypt_failures;
    uint64_t unknown_ssrc_packets;
    uint64_t unknown_pt_packets;
    uint32_t sdp_video_ssrc;
    uint32_t sdp_video_pt;
    char sdp_video_mid[32];
    unsigned stream_count;
    X4InboundRtpStream streams[16];
} X4InboundRtpSummary;
void x4_rtc_receive_diag_reset(void);
void x4_rtc_receive_diag_summary(X4InboundRtpSummary *out);
void x4_rtc_receive_diag_set_expected_video(uint32_t ssrc, uint32_t pt, const char *mid);
/* Rare numeric configuration/correspondence records, never SDP or payloads. */
void x4_rtc_receive_setting(unsigned setting, uint64_t a, uint64_t b, uint64_t c);
/* Explicit XCloud4 vendor extension, NOT an upstream libdatachannel API.
 * Session owner only, after setup/accepted description, never per packet. */
int x4_rtc_receive_track_config(int track);
/* Bounded native-thread keyed timestamps; zero if the boundary/slot is unknown.
 * RTC_SETTING22 records cumulative slot omissions and cells used at attach/end.
 * RTC_SETTING23 records process binding count / global sink scope1 / epoch0.
 * A fresh process and one session are needed for unambiguous test attribution;
 * asynchronous old transport teardown is not bound to a separate owner epoch. */
uint64_t x4_rtc_receive_socket_time(void);
uint64_t x4_rtc_receive_delivery_time(void);
void x4_rtc_receive_attach(X4Trace *);
/* Session owner only. Waits only at shutdown, never in a receive producer. */
void x4_rtc_receive_detach(X4Trace *);
/* Explicit XCloud4 vendor experiment extensions. No upstream API implied. */
int x4_rtc_configure_receiver_nack(int track, int enabled, uint32_t epoch, int legacy);
typedef struct X4ReceiverNackMetrics X4ReceiverNackMetrics;
int x4_rtc_receiver_nack_metrics(int track, X4ReceiverNackMetrics *out);
int x4_rtc_receiver_nack_accept(int track, int peer_connection);
int x4_rtc_poll_receiver_nack(int track);
int x4_rtc_close_receiver_nack(int track);
void x4_rtc_nack_event(unsigned event, unsigned flags, uint64_t identity,
    uint64_t source, uint64_t a, uint64_t b, uint64_t c);
/* For event31 vendor sequence observations only, flags encodes
 * low8=subkind13..16 and high bits=record flags. Other events unchanged. */
/* Track-pop thread owns this numeric ticket until its next delivery.
 * authenticated_us observes generator recognition after authenticated
 * provenance, ReceivingSession and full RTP validation. Ordinary packets now
 * carry sequence provenance too; identity!=0 still means a recognized first
 * watched return. This is not the exact socket-read/unprotect completion.
 * A zero tag means no usable observer provenance, not no reception. */
typedef struct {
    uint64_t identity, source, extended_sequence, rtp_timestamp;
    uint64_t authenticated_us;
    X4NackSequenceTag sequence;
} X4NackPacket;
void x4_rtc_nack_delivery(uint64_t identity, uint64_t source,
    uint64_t extended_sequence, uint32_t rtp_timestamp, uint64_t authenticated_us);
X4NackPacket x4_rtc_nack_delivery_ticket(void);
/* Track-pop provenance for ALL validated original-video messages, independent
 * of a watched NACK return. A missing tag never manufactures authentication.
 * Called together with the existing delivery ticket before the app callback.
 */
void x4_rtc_nack_sequence_delivery(const X4NackSequenceTag *sequence);
/* Functional control for the existing original-video receiver generator.
 * The application binds an independently bounded inbox to the current
 * transport epoch. Producers never wait, allocate or print; false reports
 * local inbox omission, not failure of NACK dispatch or server support.
 *
 * Request preserves the ORIGINAL missing mask even after authentication
 * clears the vendor watch bitmap. Only worker admission may complete grace.
 * Deadline is absolute detection+45000us, never renewed by a duplicate offer.
 * Zero deadline represents an invalid/overflowing clock and must fail closed.
 *
 * attempt_start_us precedes packet construction and the transport call. It
 * is NOT a socket-send start or server receipt. local_return_us observes the
 * bool/exception return from that call or the earlier local cancellation.
 * A true local_result does not establish remote delivery. */
enum {
    X4_NACK_GRACE_REQUEST = 1, X4_NACK_GRACE_OUTCOME = 2,
    X4_NACK_GRACE_SEQUENCE = 3,
    X4_NACK_GRACE_MAX_US = 45000,
    X4_NACK_GRACE_NOT_ATTEMPTED = -2,
    X4_NACK_GRACE_EXCEPTION = -1,
    X4_NACK_GRACE_ATTEMPT = 0,
    X4_NACK_GRACE_CANCEL_AUTH_COMPLETE = 4,
    X4_NACK_GRACE_CANCEL_RETIRED = 5,
    X4_NACK_GRACE_CANCEL_CLOSING = 7
};
typedef struct {
    uint64_t identity, source, first_extended, original_mask, selected_mask;
    uint64_t detected_us, deadline_us, attempt_start_us, local_return_us;
    uint32_t sequence_epoch, kind;
    int32_t local_result;
    uint32_t reason;
} X4NackGraceControl;
/* SEQUENCE reuses the fixed control cell, never a REQUEST:
 * identity = owner transport_epoch<<32 | new sequence_epoch;
 * source = packed original PT102/SSRC;
 * sequence_epoch = new; first_extended = anchor raw extended;
 * original_mask = previous raw high; selected_mask = previous epoch;
 * detected_us = anchor clock; attempt_start_us = anchor RTP timestamp;
 * deadline_us/local_return_us =0; local_result=0; reason=sequence reason.
 * It is an announcement only. Media provenance and actual worker STORED must
 * confirm it before any binding change. A transition never renews deadlines.
 */
bool x4_rtc_nack_grace_control(const X4NackGraceControl *control);

typedef struct {
    char ip[48];
    uint16_t port;
    char type[16];
    char mid[16];
    char transport[8];
} X4RemoteIceCandidate;

typedef struct {
    unsigned remote_candidate_count;
    X4RemoteIceCandidate remote_candidates[32];
    char selected_local[160];
    char selected_remote[160];
    char game_server_ip[48];
    uint16_t game_server_port;
    bool has_selected_pair;
} X4IceConnectionSummary;

void x4_rtc_diag_record_remote_candidate(const char *cand_str, const char *mid);
void x4_rtc_diag_record_selected_pair(const char *local, const char *remote);
void x4_rtc_diag_get_ice_summary(X4IceConnectionSummary *out);
void x4_rtc_diag_write_ice_json(FILE *f);

#define X4_RTT_MAX_SAMPLES 600

typedef struct {
    int p50;
    int p95;
    unsigned sample_count;
    int samples[X4_RTT_MAX_SAMPLES];
} X4RttSummary;

void x4_rtc_diag_record_rtt_sample(int rtt_ms);
/* Separate from the historical series: reset recent RTT on peer replacement. */
void x4_rtc_diag_reset_video_rtt(void);
int x4_rtc_diag_rtt_p50_ms(void);
unsigned x4_rtc_diag_video_grace_ms(void);
unsigned x4_rtc_diag_video_grace43c_ms(void);
void x4_rtc_diag_video_grace_stats(unsigned policy, uint64_t now_us,
    unsigned *current_ms, unsigned *min_ms, unsigned *max_ms);
void x4_rtc_diag_video_presented(uint64_t now_us);
bool x4_rtc_diag_startup_active(uint64_t now_us);
enum {
    X4_STARTUP_DETECT, X4_STARTUP_NACK, X4_STARTUP_RETRY, X4_STARTUP_RETURN
};
enum {
    X4_STARTUP_RELEASE_REPAIRED, X4_STARTUP_RELEASE_EXPIRED, X4_STARTUP_RELEASE_CAPACITY
};
void x4_rtc_diag_startup_range(unsigned kind, uint64_t identity,
    uint64_t detected_us, uint64_t now_us);
void x4_rtc_diag_startup_release(unsigned reason);
uint64_t x4_rtc_diag_startup_release_count(void);
void x4_rtc_diag_startup_pli(uint64_t now_us);
void x4_rtc_diag_startup_report(uint64_t now_us, bool final);
void x4_rtc_diag_get_rtt_summary(X4RttSummary *out);
void x4_rtc_diag_write_rtt_json(FILE *f);

enum {
    X4_RECONNECT_FAILED = 1, X4_RECONNECT_DISCONNECTED,
    X4_RECONNECT_NO_VIDEO, X4_RECONNECT_MANUAL, X4_RECONNECT_KEEPALIVE
};
enum { X4_RECONNECT_DIAG_CAP = 64 };
typedef struct {
    unsigned cause;
    bool recovered, completed;
    uint64_t first_video_ms;
} X4ReconnectRecord;
typedef struct {
    uint64_t attempts, reconnects, omitted;
    unsigned record_count;
    X4ReconnectRecord records[X4_RECONNECT_DIAG_CAP];
} X4ReconnectSummary;
/* Single session owner, never an RTP callback. Initial open resets these;
 * reconnect opens deliberately retain them and all T29 receive counters. */
void x4_rtc_diag_reconnect_attempt(unsigned cause);
void x4_rtc_diag_reconnect_result(unsigned cause, bool recovered, uint64_t first_video_ms);
void x4_rtc_diag_get_reconnect_summary(X4ReconnectSummary *out);


#if defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST)
/* The NACK owner samples once per second; window boundaries never change
 * receiver state, repair or RR. The SSRC is numeric and never logged. */
typedef struct {
    uint64_t bytes, packets;
    uint32_t video_ssrc;
    int32_t delay_us;
    bool delay_valid;
} X4RembReceiveWindow;
void x4_rtc_diag_remb_begin(unsigned epoch, uint32_t expected_ssrc, unsigned payload_type);
void x4_rtc_diag_remb_end(unsigned epoch);
bool x4_rtc_diag_remb_take(unsigned epoch, X4RembReceiveWindow *out);
#endif
#ifdef __cplusplus
}
#endif
