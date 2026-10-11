/* SPDX-License-Identifier: GPL-3.0-only */
/* Numeric observations plus an explicit bounded repair-control bridge.
 * Also copied into the vendor overlay; control does not depend on tracing. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
    X4_RD_TCP_FLAG = 16, X4_RD_RTCP_FLAG = 32
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
int x4_rtc_configure_receiver_nack(int track, int enabled, uint32_t epoch);
int x4_rtc_receiver_nack_accept(int track, int peer_connection);
int x4_rtc_poll_receiver_nack(int track);
int x4_rtc_close_receiver_nack(int track);
void x4_rtc_nack_event(unsigned event, unsigned flags, uint64_t identity,
    uint64_t source, uint64_t a, uint64_t b, uint64_t c);
/* Track-pop thread owns this numeric ticket until its next delivery.
 * authenticated_us observes the first watched sequence recognized by the
 * generator after authenticated provenance, ReceivingSession and full RTP
 * validation. It is not an exact socket-read/unprotect completion timestamp.
 * Zero means no recognized first return on this Message, not no reception. */
typedef struct {
    uint64_t identity, source, extended_sequence, rtp_timestamp;
    uint64_t authenticated_us;
} X4NackPacket;
void x4_rtc_nack_delivery(uint64_t identity, uint64_t source,
    uint64_t extended_sequence, uint32_t rtp_timestamp, uint64_t authenticated_us);
X4NackPacket x4_rtc_nack_delivery_ticket(void);
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
bool x4_rtc_nack_grace_control(const X4NackGraceControl *control);
#ifdef __cplusplus
}
#endif
