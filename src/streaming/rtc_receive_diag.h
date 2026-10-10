/* SPDX-License-Identifier: GPL-3.0-only */
/* Numeric, optional observations only. Also copied into the vendor overlay. */
#pragma once
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
#ifdef __cplusplus
}
#endif
