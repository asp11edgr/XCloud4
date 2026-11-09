/* SPDX-License-Identifier: GPL-3.0-only */
/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
#include "rtc_receive_diag.h"
#include "receiver_nack_state.h"
#include "../media/live_trace.h"
#if defined(X4_OPENORBIS) || defined(X4_MIC_HOST_TEST)
#include "../audio/mic_rtp.h"
#endif
#include <errno.h>
#if !X4_PUBLIC
#include <arpa/inet.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifndef X4_RECEIVE_DIAG_HOST_TEST
#include <orbis/libkernel.h>
/* Already used by rtc_pthread.c's native syscall(20) adapter. */
extern int32_t scePthreadGetthreadid(void);
#else
extern uint64_t x4_receive_test_clock(void);
extern void x4_receive_test_sleep(unsigned);
extern uint64_t x4_receive_test_threadid(void);
#endif

/* XCloud4 owns exactly one RTC session at a time. A process-lifetime bridge
 * observes only that session. Clearing the sink then draining entered readers
 * prevents diagnostic storage from being freed under an upstream producer.
 * New producers after detach see NULL. Producers never spin or sleep. */
static _Atomic(X4Trace *) sink;
static atomic_uint producers;
/* Compiler TLS is not assumed to work in an OpenOrbis SELF. These fixed,
 * process-lifetime cells use the existing native pthread identity API.
 * Each identity owns its timestamps; an exhausted lookup returns unknown.
 * No allocation, lock, wait or unbounded retry occurs on a receive path. */
enum { RECEIVE_THREAD_CAP = 32 };
typedef struct {
    atomic_uint_fast64_t key, socket_time, delivery_time;
    /* One native thread writes/reads its own ticket. Different threads own
     * disjoint cells; no ticket is borrowed across a callback boundary. */
    X4NackPacket nack;
} ReceiveThread;
static ReceiveThread thread_slots[RECEIVE_THREAD_CAP];
static atomic_uint_fast64_t thread_omissions;
static atomic_uint_fast64_t binding_count;
static atomic_uint_fast64_t nack_ticket_omissions;

enum { INBOUND_STREAM_CAP = 16 };
typedef struct {
    atomic_uint_fast64_t key; /* (1ull<<63) | ((uint64_t)ssrc << 32) | (pt & 0xffffffffu) */
    atomic_uint_fast64_t packets;
    atomic_uint_fast64_t first_us;
    atomic_uint_fast64_t last_us;
} InboundStreamSlot;

static InboundStreamSlot inbound_streams[INBOUND_STREAM_CAP];
static atomic_uint_fast64_t srtp_decrypt_failures;
static atomic_uint_fast64_t unknown_ssrc_packets;
static atomic_uint_fast64_t unknown_pt_packets;
static atomic_uint_fast64_t expected_video_ssrc;
static atomic_uint_fast32_t expected_video_pt;
static atomic_bool unexpected_video_logged;
static char expected_video_mid[32];

static atomic_flag ice_diag_lock = ATOMIC_FLAG_INIT;
static X4IceConnectionSummary s_ice_summary;
static unsigned s_rtt_count = 0;
static int s_rtt_samples[X4_RTT_MAX_SAMPLES];
/* Recent samples adapt past the capacity of the historical report. The
 * session worker updates them once per second; media reads only the p50. */
enum { VIDEO_RTT_SAMPLES = 31 };
static int video_rtt_samples[VIDEO_RTT_SAMPLES];
static unsigned video_rtt_count, video_rtt_cursor;
static atomic_int video_rtt_p50 = -1;
static atomic_uint video_grace_min[] = { 80, 60 }, video_grace_max[] = { 80, 60 };
static atomic_uint_fast64_t video_first_returns;
/* The RTT and NACK owners record a bounded whole-ms grace without locks. */
static atomic_uint_fast64_t video_grace46b_seen[2];
static atomic_uint_fast64_t startup_first_video;

#if !X4_PUBLIC
enum { STARTUP_RANGES = 32, STARTUP_PLIS = 128 };
typedef struct {
    uint64_t identity, detected, nack, retry, returned;
} StartupRange;
static StartupRange startup_ranges[STARTUP_RANGES];
static unsigned startup_range_count, startup_range_overflow, startup_pli_count, startup_pli_overflow;
static uint64_t startup_plis[STARTUP_PLIS];
static atomic_flag startup_records = ATOMIC_FLAG_INIT;
static atomic_uint_fast64_t startup_udp_last, startup_udp_max;
static atomic_uint_fast64_t startup_video_last, startup_video_max;
static atomic_uint_fast64_t startup_releases[3], startup_record_busy;
static atomic_bool startup_reported;
#endif
static X4ReconnectSummary reconnect_summary;

static inline void lock_ice(void) {
    while (atomic_flag_test_and_set_explicit(&ice_diag_lock, memory_order_acquire)) {}
}
static inline void unlock_ice(void) {
    atomic_flag_clear_explicit(&ice_diag_lock, memory_order_release);
}
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2 &&
    ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
    "Receive diagnostics require lock-free bridge atomics");
_Static_assert((int)X4_RD_APP == (int)X4_MON_T_APP_CALLBACK &&
    (int)X4_RD_RESERVED == (int)X4_MON_T_RESERVED,
    "Receive stage ABI must match the monitor");

static ReceiveThread *thread_slot(void)
{
    int saved = errno;
#ifndef X4_RECEIVE_DIAG_HOST_TEST
    int32_t native_id = scePthreadGetthreadid();
    uint64_t key = native_id > 0 ? (uint32_t)native_id : 0;
#else
    uint64_t key = x4_receive_test_threadid();
#endif
    errno = saved;
    if (key) for (unsigned i = 0; i < RECEIVE_THREAD_CAP; ++i) {
        uint64_t observed = atomic_load_explicit(&thread_slots[i].key, memory_order_relaxed);
        if (!observed) {
            uint64_t empty = 0;
            if (atomic_compare_exchange_strong_explicit(&thread_slots[i].key, &empty,
                key, memory_order_relaxed, memory_order_relaxed)) observed = key;
            else observed = empty;
        }
        if (observed == key) return &thread_slots[i];
    }
    atomic_fetch_add_explicit(&thread_omissions, 1, memory_order_relaxed);
    return NULL;
}

uint64_t x4_rtc_receive_clock(void)
{
    int saved = errno;
#ifndef X4_RECEIVE_DIAG_HOST_TEST
    uint64_t now = sceKernelGetProcessTime();
#else
    uint64_t now = x4_receive_test_clock();
#endif
    errno = saved;
    return now;
}

/* Only fixed RTP header fields are inspected. Header-extension and padding
 * bytes may still be encrypted in SRTP: no payload, extension, OSN or key is
 * read here. A pre-authentication tuple is explicitly untrusted metadata. */
static bool tuple(const void *packet, size_t size, uint32_t *ssrc,
    uint16_t *seq, unsigned *pt, uint32_t *flags)
{
    const uint8_t *p = packet;
    if (!p || size < 2 || (p[0] >> 6) != 2) return false;
    if (p[1] >= 192 && p[1] <= 223) {
        *flags |= X4_RD_RTCP_FLAG;
        return false;
    }
    if (size < 12 || size < 12u + (p[0] & 15u) * 4u) return false;
    *seq = ((uint16_t)p[2] << 8) | p[3];
    *ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
        ((uint32_t)p[10] << 8) | p[11];
    *pt = p[1] & 127u;
    return true;
}

static X4Trace *enter(void)
{
    /* seq_cst is intentional: detach's null publication and count drain must
     * order against both the producer's count and its subsequent sink load. */
    atomic_fetch_add_explicit(&producers, 1, memory_order_seq_cst);
    return atomic_load_explicit(&sink, memory_order_seq_cst);
}
static void leave(void)
{ atomic_fetch_sub_explicit(&producers, 1, memory_order_seq_cst); }

#if !X4_PUBLIC
static void startup_spacing(atomic_uint_fast64_t *last, atomic_uint_fast64_t *maximum,
    uint64_t now)
{
    uint_fast64_t previous = atomic_load_explicit(last, memory_order_relaxed);
    for (unsigned attempt = 0; previous < now && attempt < 3u; ++attempt) {
        if (!atomic_compare_exchange_weak_explicit(last, &previous, now,
                memory_order_relaxed, memory_order_relaxed)) continue;
        if (!previous) return;
        uint64_t spacing = now - previous;
        uint_fast64_t max = atomic_load_explicit(maximum, memory_order_relaxed);
        for (unsigned update = 0; spacing > max && update < 3u; ++update)
            if (atomic_compare_exchange_weak_explicit(maximum, &max, spacing,
                    memory_order_relaxed, memory_order_relaxed)) break;
        return;
    }
}
#endif

#if defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST)
static void remb_receive_observe(const uint8_t *, size_t, unsigned, uint32_t, uint16_t, uint32_t, uint64_t);
#endif
#line 196
uint64_t x4_rtc_receive_packet(unsigned stage, const void *packet, size_t bytes,
    uint64_t preceding_time, uint32_t flags)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
#if defined(X4_OPENORBIS) || defined(X4_MIC_HOST_TEST)
    if (stage == X4_RD_RTCP && (flags & X4_RD_AUTHENTICATED))
        x4_mic_rr_observe(packet, bytes);
#endif
    if (stage == X4_RD_UDP || stage == X4_RD_TCP || stage == X4_RD_TRACK_POP) {
        ReceiveThread *slot = thread_slot();
        if (slot) atomic_store_explicit(stage == X4_RD_TRACK_POP ?
            &slot->delivery_time : &slot->socket_time, now, memory_order_relaxed);
    }
    uint32_t ssrc = 0; uint16_t seq = 0; unsigned pt = 0;
    bool valid = tuple(packet, bytes, &ssrc, &seq, &pt, &flags);
#if defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST)
    if (valid && stage == X4_RD_PEER_OUT)
        remb_receive_observe(packet, bytes, pt, ssrc, seq, flags,
            preceding_time ? preceding_time : now);
#endif
#line 212
#if !X4_PUBLIC
    if ((stage == X4_RD_UDP || stage == X4_RD_TRACK) && x4_rtc_diag_startup_active(now)) {
        if (stage == X4_RD_UDP) {
            startup_spacing(&startup_udp_last, &startup_udp_max, now);
        } else if (valid && (flags & X4_RD_AUTHENTICATED)) {
            unsigned video_pt = atomic_load_explicit(&expected_video_pt, memory_order_relaxed);
            if (pt == (video_pt ? video_pt : X4_RECEIVER_NACK_VIDEO_PT))
                startup_spacing(&startup_video_last, &startup_video_max, now);
        }
    }
#endif
    if (stage == X4_RD_SRTP_VALID && valid) {
        uint64_t key = (1ull << 63) | ((uint64_t)ssrc << 32) | pt;
        for (unsigned i = 0; i < INBOUND_STREAM_CAP; ++i) {
            uint64_t observed = atomic_load_explicit(&inbound_streams[i].key, memory_order_relaxed);
            if (!observed) {
                uint64_t empty = 0;
                if (atomic_compare_exchange_strong_explicit(&inbound_streams[i].key, &empty,
                    key, memory_order_relaxed, memory_order_relaxed)) {
                    observed = key;
                } else {
                    observed = empty;
                }
            }
            if (observed == key) {
                atomic_fetch_add_explicit(&inbound_streams[i].packets, 1, memory_order_relaxed);
                uint64_t unset = 0;
                (void)atomic_compare_exchange_strong_explicit(&inbound_streams[i].first_us, &unset, now,
                    memory_order_relaxed, memory_order_relaxed);
                atomic_store_explicit(&inbound_streams[i].last_us, now, memory_order_relaxed);
                break;
            }
        }
        uint64_t exp_ssrc = atomic_load_explicit(&expected_video_ssrc, memory_order_relaxed);
        uint32_t exp_pt = atomic_load_explicit(&expected_video_pt, memory_order_relaxed);
        if (exp_ssrc && (pt == exp_pt || pt == 102) && ssrc != (uint32_t)exp_ssrc) {
            if (!atomic_exchange_explicit(&unexpected_video_logged, true, memory_order_relaxed)) {
#if X4_PUBLIC
                printf("XCloud4: RTP video recibido con identificador de flujo no coincidente\n");
#else
                printf("XCloud4: RTP video recibido con SSRC no coincidente: recibido=0x%08x pt=%u (esperado=0x%08x pt=%u)\n",
                    ssrc, pt, (uint32_t)exp_ssrc, exp_pt);
#endif
            }
        }
    }
    X4Trace *trace = enter();
    if (trace) {
        x4_trace_monitor_transport_packet(trace, stage, valid, ssrc, seq, pt,
            bytes, now, preceding_time, flags);
    }
    leave();
    errno = saved;
    return now;
}

void x4_rtc_receive_reject(unsigned stage, unsigned reason, int result,
    const void *packet, size_t bytes)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
    if (stage == X4_RD_SRTP_INPUT && (reason == X4_RD_SRTP_ERROR || reason == X4_RD_SRTP_AUTH_FAIL || reason == X4_RD_SRTP_REPLAY_FAIL)) {
        atomic_fetch_add_explicit(&srtp_decrypt_failures, 1, memory_order_relaxed);
    } else if (stage == X4_RD_SRTP_INPUT && reason == X4_RD_DEMUX_UNKNOWN) {
        atomic_fetch_add_explicit(&unknown_ssrc_packets, 1, memory_order_relaxed);
    } else if ((stage == X4_RD_PEER && reason == X4_RD_NO_TRACK) || (stage == X4_RD_TRACK && reason == X4_RD_RTP_PAYLOAD)) {
        atomic_fetch_add_explicit(&unknown_pt_packets, 1, memory_order_relaxed);
    }
    X4Trace *trace = enter();
    if (trace) {
        uint32_t ssrc = 0, flags = 0; uint16_t seq = 0; unsigned pt = 0;
        bool valid = tuple(packet, bytes, &ssrc, &seq, &pt, &flags);
        x4_trace_monitor_transport_reject(trace, stage, reason, result,
            valid, ssrc, seq, now);
    }
    leave();
    errno = saved;
}

static void reset_video_rtt_locked(void)
{
    video_rtt_count = video_rtt_cursor = 0;
    atomic_store_explicit(&video_rtt_p50, -1, memory_order_release);
    atomic_store_explicit(&video_first_returns, 0, memory_order_release);
    atomic_store_explicit(&video_grace46b_seen[0], 1, memory_order_relaxed);
    atomic_store_explicit(&video_grace46b_seen[1], 0, memory_order_relaxed);
}

void x4_rtc_receive_diag_reset(void)
{
    /* The previous RTC owner has closed before initial session reset. */
    atomic_store_explicit(&startup_first_video, 0, memory_order_relaxed);
    for (unsigned i = 0; i < 2; ++i) {
        unsigned grace = x4_nack_grace_ms(-1, i == 0);
        atomic_store_explicit(&video_grace_min[i], grace, memory_order_relaxed);
        atomic_store_explicit(&video_grace_max[i], grace, memory_order_relaxed);
    }
#if !X4_PUBLIC
    memset(startup_ranges, 0, sizeof(startup_ranges));
    startup_range_count = startup_range_overflow = startup_pli_count = startup_pli_overflow = 0;
    atomic_store_explicit(&startup_udp_last, 0, memory_order_relaxed);
    atomic_store_explicit(&startup_udp_max, 0, memory_order_relaxed);
    atomic_store_explicit(&startup_video_last, 0, memory_order_relaxed);
    atomic_store_explicit(&startup_video_max, 0, memory_order_relaxed);
    atomic_store_explicit(&startup_record_busy, 0, memory_order_relaxed);
    atomic_store_explicit(&startup_reported, false, memory_order_relaxed);
    for (unsigned i = 0; i < 3; ++i)
        atomic_store_explicit(&startup_releases[i], 0, memory_order_relaxed);
#endif
    atomic_store_explicit(&srtp_decrypt_failures, 0, memory_order_relaxed);
    atomic_store_explicit(&unknown_ssrc_packets, 0, memory_order_relaxed);
    atomic_store_explicit(&unknown_pt_packets, 0, memory_order_relaxed);
    atomic_store_explicit(&expected_video_ssrc, 0, memory_order_relaxed);
    atomic_store_explicit(&expected_video_pt, 0, memory_order_relaxed);
    atomic_store_explicit(&unexpected_video_logged, false, memory_order_relaxed);
    expected_video_mid[0] = '\0';
    for (unsigned i = 0; i < INBOUND_STREAM_CAP; ++i) {
        atomic_store_explicit(&inbound_streams[i].key, 0, memory_order_relaxed);
        atomic_store_explicit(&inbound_streams[i].packets, 0, memory_order_relaxed);
        atomic_store_explicit(&inbound_streams[i].first_us, 0, memory_order_relaxed);
        atomic_store_explicit(&inbound_streams[i].last_us, 0, memory_order_relaxed);
    }
    lock_ice();
    memset(&s_ice_summary, 0, sizeof(s_ice_summary));
    s_rtt_count = 0;
    memset(s_rtt_samples, 0, sizeof(s_rtt_samples));
    reset_video_rtt_locked();
    memset(&reconnect_summary,0,sizeof(reconnect_summary));
    unlock_ice();
}

void x4_rtc_receive_diag_set_expected_video(uint32_t ssrc, uint32_t pt, const char *mid)
{
    atomic_store_explicit(&expected_video_ssrc, ssrc, memory_order_relaxed);
    atomic_store_explicit(&expected_video_pt, pt, memory_order_relaxed);
    if (mid) {
        size_t len = strlen(mid);
        if (len >= sizeof(expected_video_mid)) len = sizeof(expected_video_mid) - 1;
        memcpy(expected_video_mid, mid, len);
        expected_video_mid[len] = '\0';
    } else {
        expected_video_mid[0] = '\0';
    }
}

void x4_rtc_receive_diag_summary(X4InboundRtpSummary *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->srtp_decrypt_failures = atomic_load_explicit(&srtp_decrypt_failures, memory_order_relaxed);
    out->unknown_ssrc_packets = atomic_load_explicit(&unknown_ssrc_packets, memory_order_relaxed);
    out->unknown_pt_packets = atomic_load_explicit(&unknown_pt_packets, memory_order_relaxed);
    out->sdp_video_ssrc = (uint32_t)atomic_load_explicit(&expected_video_ssrc, memory_order_relaxed);
    out->sdp_video_pt = atomic_load_explicit(&expected_video_pt, memory_order_relaxed);
    memcpy(out->sdp_video_mid, expected_video_mid, sizeof(out->sdp_video_mid));
    unsigned count = 0;
    for (unsigned i = 0; i < INBOUND_STREAM_CAP; ++i) {
        uint64_t key = atomic_load_explicit(&inbound_streams[i].key, memory_order_relaxed);
        if (key & (1ull << 63)) {
            out->streams[count].ssrc = (uint32_t)(key >> 32);
            out->streams[count].payload_type = (uint32_t)(key & 0xffffffffu);
            out->streams[count].packets = atomic_load_explicit(&inbound_streams[i].packets, memory_order_relaxed);
            out->streams[count].first_us = atomic_load_explicit(&inbound_streams[i].first_us, memory_order_relaxed);
            out->streams[count].last_us = atomic_load_explicit(&inbound_streams[i].last_us, memory_order_relaxed);
            ++count;
        }
    }
    out->stream_count = count;
}
uint64_t x4_rtc_receive_socket_time(void)
{
    ReceiveThread *slot = thread_slot();
    return slot ? atomic_load_explicit(&slot->socket_time,memory_order_relaxed) : 0;
}
uint64_t x4_rtc_receive_delivery_time(void)
{
    ReceiveThread *slot = thread_slot();
    return slot ? atomic_load_explicit(&slot->delivery_time,memory_order_relaxed) : 0;
}
void x4_rtc_receive_setting(unsigned setting, uint64_t a, uint64_t b, uint64_t c)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
    X4Trace *trace = enter();
    if (trace) {
#if X4_STREAM_PROFILE
        if(setting==14 && (a>>32)<2 && (a&0xffffu)<128 && b<=0xffffffu)
            x4_trace_sdp_profile_record(trace,(unsigned)(a>>32),(unsigned)(a&0xffffu),(uint32_t)b);
#endif
        x4_trace_monitor_critical(trace, now, X4_MON_CR_RTC_SETTING,
            0xffffu, 0, setting, 0, a, b, c);
    }
    leave();
    errno = saved;
}
void x4_rtc_nack_event(unsigned event, unsigned flags, uint64_t identity,
    uint64_t source, uint64_t a, uint64_t b, uint64_t c)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
    X4Trace *trace = enter();
    /* The newly used vendor sequence records share critical31 with worker
     * grace records. Low byte selects subkind13..16, remaining bits are its
     * flags. No extra critical-counter slot or record capacity is allocated. */
    unsigned stage = event == X4_MON_CR_NACK_GRACE ? flags & 255u : 0xffffu;
    unsigned record_flags = event == X4_MON_CR_NACK_GRACE ? flags >> 8 : flags;
    if (trace) x4_trace_monitor_critical(trace, now, event, stage, record_flags,
        identity, source, a, b, c);
    leave(); errno = saved;
}
void x4_rtc_nack_delivery(uint64_t identity, uint64_t source,
    uint64_t extended_sequence, uint32_t rtp_timestamp, uint64_t authenticated_us)
{
    ReceiveThread *slot = thread_slot();
    if (slot) slot->nack = (X4NackPacket){.identity=identity, .source=source,
        .extended_sequence=extended_sequence, .rtp_timestamp=rtp_timestamp,
        .authenticated_us=authenticated_us};
    else atomic_fetch_add_explicit(&nack_ticket_omissions,1,memory_order_relaxed);
}
void x4_rtc_nack_sequence_delivery(const X4NackSequenceTag *sequence)
{
    ReceiveThread *slot = thread_slot();
    if (slot) slot->nack.sequence = sequence ? *sequence : (X4NackSequenceTag){0};
    else atomic_fetch_add_explicit(&nack_ticket_omissions,1,memory_order_relaxed);
}
X4NackPacket x4_rtc_nack_delivery_ticket(void)
{
    ReceiveThread *slot = thread_slot();
    if (!slot) atomic_fetch_add_explicit(&nack_ticket_omissions,1,memory_order_relaxed);
    return slot ? slot->nack : (X4NackPacket){0};
}
void x4_rtc_receive_attach(X4Trace *trace)
{
    uint64_t bindings = atomic_fetch_add_explicit(&binding_count,1,memory_order_relaxed)+1;
    atomic_store_explicit(&sink, trace, memory_order_seq_cst);
    unsigned used=0;
    for(unsigned i=0;i<RECEIVE_THREAD_CAP;++i)
        if(atomic_load_explicit(&thread_slots[i].key,memory_order_relaxed))++used;
    x4_rtc_receive_setting(22,atomic_load_explicit(&thread_omissions,memory_order_relaxed),used,0);
    x4_rtc_receive_setting(23,bindings,1,0);
}
void x4_rtc_receive_detach(X4Trace *trace)
{
    X4Trace *expected = trace;
    if (!atomic_compare_exchange_strong_explicit(&sink, &expected, NULL,
        memory_order_seq_cst, memory_order_seq_cst)) return;
    while (atomic_load_explicit(&producers, memory_order_seq_cst)) {
#ifndef X4_RECEIVE_DIAG_HOST_TEST
        sceKernelUsleep(1000);
#else
        x4_receive_test_sleep(1000);
#endif
    }
    /* Owner still owns trace after draining entered hooks. Record final
     * coverage directly; never temporarily republish a detached sink.
     * The bridge has global scope, not an old/new transport epoch binding. */
    if (trace) {
        unsigned used=0;
        for(unsigned i=0;i<RECEIVE_THREAD_CAP;++i)
            if(atomic_load_explicit(&thread_slots[i].key,memory_order_relaxed))++used;
        uint64_t now=x4_rtc_receive_clock();
        x4_trace_monitor_critical(trace,now,X4_MON_CR_RTC_SETTING,0xffffu,0,22,0,
            atomic_load_explicit(&thread_omissions,memory_order_relaxed),used,1);
        x4_trace_monitor_critical(trace,now,X4_MON_CR_RTC_SETTING,0xffffu,0,23,0,
            atomic_load_explicit(&binding_count,memory_order_relaxed),1,0);
        x4_trace_monitor_critical(trace,now,X4_MON_CR_NACK_COUNTER,0xffffu,0,0,0,
            109,atomic_load_explicit(&nack_ticket_omissions,memory_order_relaxed),0);
    }
}


static const char *const ice_types[] = {"host", "srflx", "relay", "prflx", "unknown"};
enum { ICE_UNKNOWN = 4 };
typedef struct {
    char address[48];
    uint16_t port;
    unsigned type;
    const char *protocol;
} IceEndpoint;

static unsigned ice_type(const char *word)
{
    for (unsigned i = 0; i < ICE_UNKNOWN; ++i)
        if (!strcmp(ice_types[i], word)) return i;
    return ICE_UNKNOWN;
}

static const char *ice_protocol(const char *word)
{
    if (!strcmp(word, "UDP") || !strcmp(word, "udp")) return "udp";
    if (!strcmp(word, "TCP") || !strcmp(word, "tcp")) return "tcp";
    return "unknown";
}

static uint16_t ice_port(const char *word)
{
    unsigned port = 0;
    for (; *word; ++word) {
        if (*word < '0' || *word > '9') return 0;
        port = port * 10 + (unsigned)(*word - '0');
        if (port > UINT16_MAX) return 0;
    }
    return (uint16_t)port;
}

static IceEndpoint parse_candidate_line(const char *cand)
{
    IceEndpoint endpoint = {.type = ICE_UNKNOWN, .protocol = "unknown"};
    if (!cand || !cand[0]) return endpoint;
    static const char whitespace[] = " \t\r\n";
    const char *p = cand;
    p += strspn(p, whitespace);
    if (!strncmp(p, "a=", 2)) p += 2;
    p += strspn(p, whitespace);
    bool has_prefix = !strncmp(p, "candidate:", 10);
    if (has_prefix || strpbrk(p, whitespace)) {
        if (has_prefix) p += 10;
        char tokens[8][64];
        for (unsigned i = 0; i < 8; ++i) {
            p += strspn(p, whitespace);
            size_t length = strcspn(p, whitespace);
            if (!length || length >= sizeof(tokens[i])) return endpoint;
            memcpy(tokens[i], p, length);
            tokens[i][length] = '\0';
            p += length;
        }
        /* Foundation, component, protocol, priority, address, port, typ, type. */
        if (strcmp(tokens[6], "typ") || strlen(tokens[4]) >= sizeof(endpoint.address))
            return endpoint;
        endpoint.port = ice_port(tokens[5]);
        if (!endpoint.port) return endpoint;
        memcpy(endpoint.address, tokens[4], strlen(tokens[4]) + 1);
        endpoint.type = ice_type(tokens[7]);
        endpoint.protocol = ice_protocol(tokens[2]);
        return endpoint;
    }

    /* Some backends supply only an endpoint, with no type or protocol. */
    const char *colon = strrchr(p, ':');
    if (colon && colon != p) {
        size_t length = (size_t)(colon - p);
        if (length > 2 && p[0] == '[' && p[length - 1] == ']') {
            ++p;
            length -= 2;
        }
        if (length >= sizeof(endpoint.address)) return endpoint;
        endpoint.port = ice_port(colon + 1);
        if (endpoint.port) memcpy(endpoint.address, p, length);
    }
    return endpoint;
}

/* Caller holds the ICE lock. Older RTC backends return just IP:port for the
 * selected pair. Recover the remote labels from the corresponding candidate. */
static IceEndpoint selected_remote_locked(const char *candidate)
{
    IceEndpoint endpoint = parse_candidate_line(candidate);
    if (!endpoint.port) return endpoint;
    for (unsigned i = 0; i < s_ice_summary.remote_candidate_count; ++i) {
        const X4RemoteIceCandidate *cached = &s_ice_summary.remote_candidates[i];
        if (endpoint.port != cached->port || strcmp(endpoint.address, cached->ip)) continue;
        if (endpoint.type == ICE_UNKNOWN) endpoint.type = ice_type(cached->type);
        if (!strcmp(endpoint.protocol, "unknown")) endpoint.protocol = ice_protocol(cached->transport);
        break;
    }
    return endpoint;
}

#if !X4_PUBLIC
static void ice_address_prefix(const char *address, char prefix[24])
{
    unsigned char bytes[16];
    if (inet_pton(AF_INET, address, bytes) == 1) {
        snprintf(prefix, 24, "%u.%u.x.x", (unsigned)bytes[0], (unsigned)bytes[1]);
    } else if (inet_pton(AF_INET6, address, bytes) == 1) {
        snprintf(prefix, 24, "%x:%x::/32", ((unsigned)bytes[0] << 8) | bytes[1],
            ((unsigned)bytes[2] << 8) | bytes[3]);
    } else {
        strcpy(prefix, "-");
    }
}
#endif

void x4_rtc_diag_record_remote_candidate(const char *cand_str, const char *mid)
{
    IceEndpoint endpoint = parse_candidate_line(cand_str);
    if (!endpoint.port) return;

    lock_ice();
    for (unsigned i = 0; i < s_ice_summary.remote_candidate_count; ++i) {
        if (!strcmp(s_ice_summary.remote_candidates[i].ip, endpoint.address) &&
            s_ice_summary.remote_candidates[i].port == endpoint.port) {
            unlock_ice();
            return; /* already recorded */
        }
    }

    if (s_ice_summary.remote_candidate_count < 32) {
        unsigned idx = s_ice_summary.remote_candidate_count++;
        X4RemoteIceCandidate *cached = &s_ice_summary.remote_candidates[idx];
        memcpy(cached->ip, endpoint.address, sizeof(cached->ip));
        cached->port = endpoint.port;
        snprintf(cached->type, sizeof(cached->type), "%s", ice_types[endpoint.type]);
        snprintf(cached->mid, sizeof(cached->mid), "%s", mid ? mid : "0");
        snprintf(cached->transport, sizeof(cached->transport), "%s", endpoint.protocol);
    }
    unlock_ice();

#if X4_PUBLIC
    printf("XCloud4: ICE candidato remoto: remote=%s/%s\n", ice_types[endpoint.type], endpoint.protocol);
#else
    char prefix[24];
    ice_address_prefix(endpoint.address, prefix);
    printf("XCloud4: ICE candidato remoto: remote=%s/%s remote_prefix=%s\n",
        ice_types[endpoint.type], endpoint.protocol, prefix);
#endif
}

void x4_rtc_diag_record_selected_pair(const char *local, const char *remote)
{
    IceEndpoint left = parse_candidate_line(local);
    lock_ice();
    IceEndpoint right = selected_remote_locked(remote);
    snprintf(s_ice_summary.selected_local, sizeof(s_ice_summary.selected_local), "%s", local ? local : "");
    snprintf(s_ice_summary.selected_remote, sizeof(s_ice_summary.selected_remote), "%s", remote ? remote : "");
    memcpy(s_ice_summary.game_server_ip, right.address, sizeof(s_ice_summary.game_server_ip));
    s_ice_summary.game_server_port = right.port;
    s_ice_summary.has_selected_pair = true;
    unlock_ice();

#if X4_PUBLIC
    printf("XCloud4: ICE par seleccionado: local=%s/%s remote=%s/%s\n",
        ice_types[left.type], left.protocol, ice_types[right.type], right.protocol);
#else
    char local_prefix[24], remote_prefix[24];
    ice_address_prefix(left.address, local_prefix);
    ice_address_prefix(right.address, remote_prefix);
    printf("XCloud4: ICE par seleccionado: local=%s/%s remote=%s/%s local_prefix=%s remote_prefix=%s\n",
        ice_types[left.type], left.protocol, ice_types[right.type], right.protocol, local_prefix, remote_prefix);
#endif
}

void x4_rtc_diag_get_ice_summary(X4IceConnectionSummary *out)
{
    if (!out) return;
    lock_ice();
    *out = s_ice_summary;
    unlock_ice();
}

void x4_rtc_diag_write_ice_json(FILE *f)
{
    if (!f) return;
    lock_ice();
#if X4_PUBLIC
    /* Public diagnostics retain only bounded classifications. Raw ICE
     * strings, addresses, ports and mids never enter this JSON branch. */
    unsigned counts[5]={0};
    for(unsigned i=0;i<s_ice_summary.remote_candidate_count;++i){
        ++counts[ice_type(s_ice_summary.remote_candidates[i].type)];
    }
    fprintf(f,"\"ice\":{\"remote_candidate_counts\":{\"host\":%u,\"srflx\":%u,\"relay\":%u,\"prflx\":%u,\"unknown\":%u},\"selected_pair_classification\":",
        counts[0],counts[1],counts[2],counts[3],counts[4]);
    if(s_ice_summary.has_selected_pair) {
        IceEndpoint selected = selected_remote_locked(s_ice_summary.selected_remote);
        const char *protocol = !strcmp(selected.protocol, "udp") ? "UDP" :
            !strcmp(selected.protocol, "tcp") ? "TCP" : "unknown";
        fprintf(f,"{\"remote_type\":\"%s\",\"transport\":\"%s\"}",ice_types[selected.type],protocol);
    }
    else fputs("null",f);
    fputs("},",f);
#else
    fprintf(f, "\"ice\":{\"remote_candidates\":[");
    for (unsigned i = 0; i < s_ice_summary.remote_candidate_count; ++i) {
        if (i) fputc(',', f);
        const X4RemoteIceCandidate *candidate = &s_ice_summary.remote_candidates[i];
        char prefix[24];
        ice_address_prefix(candidate->ip, prefix);
        fprintf(f, "{\"ip_prefix\":\"%s\",\"type\":\"%s\",\"transport\":\"%s\"}",
            prefix, ice_types[ice_type(candidate->type)], ice_protocol(candidate->transport));
    }
    fprintf(f, "],\"selected_candidate_pair\":");
    if (s_ice_summary.has_selected_pair) {
        IceEndpoint local = parse_candidate_line(s_ice_summary.selected_local);
        IceEndpoint remote = selected_remote_locked(s_ice_summary.selected_remote);
        char local_prefix[24], remote_prefix[24];
        ice_address_prefix(local.address, local_prefix);
        ice_address_prefix(remote.address, remote_prefix);
        fprintf(f, "{\"local\":\"%s/%s\",\"remote\":\"%s/%s\",\"local_prefix\":\"%s\",\"remote_prefix\":\"%s\"}",
            ice_types[local.type], local.protocol, ice_types[remote.type], remote.protocol,
            local_prefix, remote_prefix);
    } else {
        fprintf(f, "null");
    }
    fprintf(f, "},");
#endif
    unlock_ice();
}


static void sort_rtt_samples(int *arr, unsigned n)
{
    for (unsigned i = 0; i < n - 1; ++i) {
        for (unsigned j = i + 1; j < n; ++j) {
            if (arr[j] < arr[i]) {
                int tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
    }
}

static void update_grace_bounds_locked(int rtt)
{
    for (unsigned i = 0; i < 2; ++i) {
        unsigned grace = x4_nack_grace_ms(rtt, i == 0);
        if (grace < atomic_load_explicit(&video_grace_min[i], memory_order_relaxed))
            atomic_store_explicit(&video_grace_min[i], grace, memory_order_relaxed);
        if (grace > atomic_load_explicit(&video_grace_max[i], memory_order_relaxed))
            atomic_store_explicit(&video_grace_max[i], grace, memory_order_relaxed);
    }
}

static unsigned grace46b_with_returns(int rtt, uint64_t returns)
{
    return x4_nack_grace46b_ms(rtt, (uint32_t)returns, (unsigned)(returns >> 32));
}

static void record_grace46b(unsigned grace)
{
    unsigned bit = grace - 80u;
    atomic_fetch_or_explicit(&video_grace46b_seen[bit / 64u],
        UINT64_C(1) << (bit % 64u), memory_order_relaxed);
}

void x4_rtc_diag_record_rtt_sample(int rtt_ms)
{
    if (rtt_ms < 0) return;
    lock_ice();
    if (s_rtt_count < X4_RTT_MAX_SAMPLES) {
        s_rtt_samples[s_rtt_count++] = rtt_ms;
    }
    video_rtt_samples[video_rtt_cursor] = rtt_ms;
    video_rtt_cursor = (video_rtt_cursor + 1u) % VIDEO_RTT_SAMPLES;
    if (video_rtt_count < VIDEO_RTT_SAMPLES) ++video_rtt_count;
    int sorted[VIDEO_RTT_SAMPLES];
    memcpy(sorted, video_rtt_samples, video_rtt_count * sizeof(*sorted));
    sort_rtt_samples(sorted, video_rtt_count);
    int median = sorted[video_rtt_count / 2u];
    update_grace_bounds_locked(median);
    atomic_store_explicit(&video_rtt_p50, median, memory_order_release);
    record_grace46b(grace46b_with_returns(median,
        atomic_load_explicit(&video_first_returns, memory_order_acquire)));
    unlock_ice();
}

void x4_rtc_diag_reset_video_rtt(void)
{
    lock_ice();
    reset_video_rtt_locked();
    unlock_ice();
}

int x4_rtc_diag_rtt_p50_ms(void)
{
    return atomic_load_explicit(&video_rtt_p50, memory_order_acquire);
}

unsigned x4_rtc_diag_video_grace_ms(void)
{
    return x4_nack_grace_ms(x4_rtc_diag_rtt_p50_ms(), false);
}

unsigned x4_rtc_diag_video_grace43c_ms(void)
{
    return x4_nack_grace_ms(x4_rtc_diag_rtt_p50_ms(), true);
}

void x4_rtc_diag_first_return_publish(uint32_t p95_us, unsigned samples)
{
    uint64_t returns = ((uint64_t)samples << 32) | p95_us;
    atomic_store_explicit(&video_first_returns, returns, memory_order_release);
    record_grace46b(grace46b_with_returns(x4_rtc_diag_rtt_p50_ms(), returns));
}

unsigned x4_rtc_diag_video_grace46b_ms(void)
{
    return grace46b_with_returns(x4_rtc_diag_rtt_p50_ms(),
        atomic_load_explicit(&video_first_returns, memory_order_acquire));
}

void x4_rtc_diag_video_presented(uint64_t now)
{
    uint_fast64_t unstarted = 0;
    if (now) (void)atomic_compare_exchange_strong_explicit(&startup_first_video, &unstarted,
        now, memory_order_release, memory_order_relaxed);
}

bool x4_rtc_diag_startup_active(uint64_t now)
{
    uint64_t first = atomic_load_explicit(&startup_first_video, memory_order_acquire);
    return first && now >= first && now - first < X4_NACK_STARTUP_WINDOW_US;
}

void x4_rtc_diag_video_grace_stats(unsigned policy, uint64_t now,
    unsigned *current, unsigned *min, unsigned *max)
{
    if (policy == 1) {
        *current = *min = *max = 45;
        return;
    }
    if (policy == 3u) {
        uint64_t low = atomic_load_explicit(&video_grace46b_seen[0], memory_order_relaxed);
        uint64_t high = atomic_load_explicit(&video_grace46b_seen[1], memory_order_relaxed);
        *current = x4_rtc_diag_video_grace46b_ms();
        *min = low ? 80u + (unsigned)__builtin_ctzll(low) : *current;
        *max = high ? 207u - (unsigned)__builtin_clzll(high) :
            low ? 143u - (unsigned)__builtin_clzll(low) : *current;
        if (atomic_load_explicit(&startup_first_video, memory_order_acquire)) {
            *max = X4_NACK_STARTUP_GRACE_MS;
            if (x4_rtc_diag_startup_active(now)) *current = X4_NACK_STARTUP_GRACE_MS;
        }
        return;
    }
    unsigned i = policy == 0 ? 0 : 1;
    *current = x4_nack_grace_ms(x4_rtc_diag_rtt_p50_ms(), i == 0);
    *min = atomic_load_explicit(&video_grace_min[i], memory_order_relaxed);
    *max = atomic_load_explicit(&video_grace_max[i], memory_order_relaxed);
    if (i == 0 && atomic_load_explicit(&startup_first_video, memory_order_acquire)) {
        *max = X4_NACK_STARTUP_GRACE_MS;
        if (x4_rtc_diag_startup_active(now)) *current = X4_NACK_STARTUP_GRACE_MS;
    }
}

#if !X4_PUBLIC
static bool startup_begin_record(uint64_t now)
{
    if (!x4_rtc_diag_startup_active(now) ||
        atomic_load_explicit(&startup_reported, memory_order_relaxed)) return false;
    if (atomic_flag_test_and_set_explicit(&startup_records, memory_order_acquire)) {
        atomic_fetch_add_explicit(&startup_record_busy, 1, memory_order_relaxed);
        return false;
    }
    if (!atomic_load_explicit(&startup_reported, memory_order_relaxed)) return true;
    atomic_flag_clear_explicit(&startup_records, memory_order_release);
    return false;
}

static long long startup_delta_ms(uint64_t detected, uint64_t event)
{
    return !event || event < detected ? -1 : (long long)((event - detected) / 1000u);
}
#endif

void x4_rtc_diag_startup_range(unsigned kind, uint64_t identity,
    uint64_t detected, uint64_t now)
{
#if !X4_PUBLIC
    if (!startup_begin_record(now)) return;
    if (kind == X4_STARTUP_DETECT) {
        if (startup_range_count < STARTUP_RANGES)
            startup_ranges[startup_range_count++] = (StartupRange){ .identity = identity,
                .detected = detected };
        else ++startup_range_overflow;
    } else {
        for (unsigned i = 0; i < startup_range_count; ++i) {
            StartupRange *range = &startup_ranges[i];
            if (range->identity != identity || range->detected != detected) continue;
            if (kind == X4_STARTUP_NACK && !range->nack) range->nack = now;
            else if (kind == X4_STARTUP_RETRY && !range->retry) range->retry = now;
            else if (kind == X4_STARTUP_RETURN && !range->returned) range->returned = now;
            break;
        }
    }
    atomic_flag_clear_explicit(&startup_records, memory_order_release);
#else
    (void)kind; (void)identity; (void)detected; (void)now;
#endif
}

void x4_rtc_diag_startup_release(unsigned reason)
{
#if !X4_PUBLIC
    if (reason < 3)
        atomic_fetch_add_explicit(&startup_releases[reason], 1, memory_order_relaxed);
#else
    (void)reason;
#endif
}

uint64_t x4_rtc_diag_startup_release_count(void)
{
    uint64_t total = 0;
#if !X4_PUBLIC
    for (unsigned i = 0; i < 3; ++i)
        total += atomic_load_explicit(&startup_releases[i], memory_order_relaxed);
#endif
    return total;
}

void x4_rtc_diag_startup_pli(uint64_t now)
{
#if !X4_PUBLIC
    if (!startup_begin_record(now)) return;
    if (startup_pli_count < STARTUP_PLIS) startup_plis[startup_pli_count++] = now;
    else ++startup_pli_overflow;
    atomic_flag_clear_explicit(&startup_records, memory_order_release);
#else
    (void)now;
#endif
}

void x4_rtc_diag_startup_report(uint64_t now, bool final)
{
#if !X4_PUBLIC
    uint64_t first = atomic_load_explicit(&startup_first_video, memory_order_acquire);
    if (!first || (!final && (now < first || now - first < X4_NACK_STARTUP_WINDOW_US)) ||
        atomic_load_explicit(&startup_reported, memory_order_relaxed) ||
        atomic_flag_test_and_set_explicit(&startup_records, memory_order_acquire)) return;
    if (atomic_exchange_explicit(&startup_reported, true, memory_order_relaxed)) {
        atomic_flag_clear_explicit(&startup_records, memory_order_release);
        return;
    }
    /* The gate is released before logging: producers will see reported=true
     * and stop mutating the records until the next session reset. */
    atomic_flag_clear_explicit(&startup_records, memory_order_release);
    uint64_t window = now > first ? now - first : 0;
    if (window > X4_NACK_STARTUP_WINDOW_US) window = X4_NACK_STARTUP_WINDOW_US;
    printf("XCloud4: arranque43c ventana_ms=%llu udp_max_us=%llu video_rtp_max_us=%llu rangos=%u omitidos=%u pli=%u pli_omitidos=%u ocupados=%llu liberados_reparacion=%llu liberados_plazo=%llu liberados_capacidad=%llu\n",
        (unsigned long long)(window / 1000u),
        (unsigned long long)atomic_load_explicit(&startup_udp_max, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&startup_video_max, memory_order_relaxed),
        startup_range_count, startup_range_overflow, startup_pli_count, startup_pli_overflow,
        (unsigned long long)atomic_load_explicit(&startup_record_busy, memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&startup_releases[0], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&startup_releases[1], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&startup_releases[2], memory_order_relaxed));
    for (unsigned i = 0; i < startup_range_count; ++i) {
        const StartupRange *range = &startup_ranges[i];
        printf("XCloud4: arranque43c rango=%u detectado_ms=%lld nack_ms=%lld reintento_ms=%lld retorno_ms=%lld\n",
            i + 1, startup_delta_ms(first, range->detected), startup_delta_ms(range->detected, range->nack),
            startup_delta_ms(range->detected, range->retry), startup_delta_ms(range->detected, range->returned));
    }
    for (unsigned i = 0; i < startup_pli_count; ++i)
        printf("XCloud4: arranque43c pli=%u relativo_ms=%lld\n", i + 1,
            startup_delta_ms(first, startup_plis[i]));
#else
    (void)now; (void)final;
#endif
}

void x4_rtc_diag_get_rtt_summary(X4RttSummary *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    lock_ice();
    unsigned n = s_rtt_count;
    out->sample_count = n;
    if (n == 0) {
        out->p50 = -1;
        out->p95 = -1;
        unlock_ice();
        return;
    }
    for (unsigned i = 0; i < n; ++i) {
        out->samples[i] = s_rtt_samples[i];
    }
    unlock_ice();

    int temp[X4_RTT_MAX_SAMPLES];
    memcpy(temp, out->samples, n * sizeof(int));
    sort_rtt_samples(temp, n);

    out->p50 = temp[n / 2];
    unsigned p95_idx = (n * 95) / 100;
    if (p95_idx >= n) p95_idx = n - 1;
    out->p95 = temp[p95_idx];
}

void x4_rtc_diag_write_rtt_json(FILE *f)
{
    if (!f) return;
    X4RttSummary rtt;
    x4_rtc_diag_get_rtt_summary(&rtt);
    fprintf(f, "\"rtt_ms\":{\"p50\":%d,\"p95\":%d,\"samples\":%u,\"series\":[",
        rtt.p50, rtt.p95, rtt.sample_count);
    for (unsigned i = 0; i < rtt.sample_count; ++i) {
        if (i) fputc(',', f);
        fprintf(f, "%d", rtt.samples[i]);
    }
    fprintf(f, "]},");
    /* Both public last-stream and Lab already call this writer. New fields
     * contain fixed classifications and timings, never endpoint addresses. */
    lock_ice();
    fprintf(f,"\"reconnects\":%llu,\"reconnection\":{\"attempts\":%llu,\"omitted\":%llu,\"records\":[",
        (unsigned long long)reconnect_summary.reconnects,
        (unsigned long long)reconnect_summary.attempts,(unsigned long long)reconnect_summary.omitted);
    static const char *const causes[]={"unknown","failed","disconnected","no_video","manual","keepalive"};
    for(unsigned i=0;i<reconnect_summary.record_count;++i){
        const X4ReconnectRecord *r=&reconnect_summary.records[i];
        fprintf(f,"%s{\"cause\":\"%s\",\"completed\":%s,\"recovered\":%s,\"first_video_ms\":%llu}",i?",":"",
            causes[r->cause],r->completed?"true":"false",r->recovered?"true":"false",(unsigned long long)r->first_video_ms);
    }
    fputs("]},",f);unlock_ice();
}

void x4_rtc_diag_reconnect_attempt(unsigned cause)
{
    if(cause<X4_RECONNECT_FAILED||cause>X4_RECONNECT_KEEPALIVE)return;
    lock_ice();++reconnect_summary.attempts;
    if(reconnect_summary.record_count<X4_RECONNECT_DIAG_CAP)
        reconnect_summary.records[reconnect_summary.record_count++]=(X4ReconnectRecord){.cause=cause};
    else ++reconnect_summary.omitted;
    unlock_ice();
}
void x4_rtc_diag_reconnect_result(unsigned cause,bool recovered,uint64_t first_video_ms)
{
    if(cause<X4_RECONNECT_FAILED||cause>X4_RECONNECT_KEEPALIVE)return;
    lock_ice();
    if(recovered)++reconnect_summary.reconnects;
    /* Results correspond to the immediately preceding single-owner attempt.
     * Once the bounded list is full, totals continue without overwriting it. */
    if(!reconnect_summary.omitted&&reconnect_summary.record_count){
        X4ReconnectRecord *r=&reconnect_summary.records[reconnect_summary.record_count-1];
        if(!r->completed&&r->cause==cause){r->completed=true;r->recovered=recovered;r->first_video_ms=first_video_ms;}
    }
    unlock_ice();
}
void x4_rtc_diag_get_reconnect_summary(X4ReconnectSummary *out)
{if(out){lock_ice();*out=reconnect_summary;unlock_ice();}}

#if defined(X4_OPENORBIS) || defined(X4_REMB_HOST_TEST)
/* This monitor is separate from RR and NACK. The live peer owns setup and
 * teardown. Packet observation tries the gate once and never waits. */
enum { REMB_SEQUENCE_HISTORY = 2048 };
static atomic_flag remb_receive_gate = ATOMIC_FLAG_INIT;
static atomic_uint remb_receive_epoch;
static struct {
    uint64_t seen[REMB_SEQUENCE_HISTORY];
    uint64_t high, bytes, packets, first_arrival;
    uint32_t expected_ssrc, source, first_timestamp, last_timestamp;
    unsigned payload_type;
    int32_t filtered_transit;
    bool have_transit, delay_valid;
} remb_receive;

static void remb_receive_enter(void)
{
    while (atomic_flag_test_and_set_explicit(&remb_receive_gate, memory_order_acquire)) {}
}

void x4_rtc_diag_remb_begin(unsigned epoch, uint32_t expected_ssrc, unsigned payload_type)
{
    if (!epoch || payload_type > 127) return;
    remb_receive_enter();
    memset(&remb_receive, 0, sizeof(remb_receive));
    remb_receive.expected_ssrc = expected_ssrc;
    remb_receive.payload_type = payload_type;
    atomic_store_explicit(&remb_receive_epoch, epoch, memory_order_release);
    atomic_flag_clear_explicit(&remb_receive_gate, memory_order_release);
}

void x4_rtc_diag_remb_end(unsigned epoch)
{
    unsigned current = epoch;
    (void)atomic_compare_exchange_strong_explicit(&remb_receive_epoch, &current, 0,
        memory_order_acq_rel, memory_order_relaxed);
}

static bool remb_count_sequence(uint16_t sequence)
{
    /* Leave room below the first sequence for out-of-order packets at wrap. */
    uint64_t extended = UINT64_C(65536) + sequence;
    if (remb_receive.high) {
        const unsigned distance = (uint16_t)(sequence - (uint16_t)remb_receive.high);
        if (distance >= 32768u) {
            const unsigned back = 65536u - distance;
            if (back >= REMB_SEQUENCE_HISTORY) return false;
            extended = remb_receive.high - back;
        } else {
            extended = remb_receive.high + distance;
        }
    }
    uint64_t *slot = &remb_receive.seen[extended % REMB_SEQUENCE_HISTORY];
    if (*slot == extended) return false;
    *slot = extended;
    if (extended > remb_receive.high) remb_receive.high = extended;
    return true;
}

static void remb_receive_transit(uint32_t timestamp, uint64_t arrival)
{
    if (!remb_receive.have_transit) {
        remb_receive.have_transit = true;
        remb_receive.first_timestamp = remb_receive.last_timestamp = timestamp;
        remb_receive.first_arrival = arrival;
        return;
    }
    const uint32_t advance = timestamp - remb_receive.last_timestamp;
    const uint32_t ticks = timestamp - remb_receive.first_timestamp;
    if (!advance || advance >= UINT32_C(0x80000000) ||
        ticks >= UINT32_C(0x80000000) || arrival < remb_receive.first_arrival) return;

    /* Same RTP frame keeps the earliest arrival. Filter transit differences
     * at distinct original timestamps, without measuring RTX/FEC delay. */
    const uint64_t elapsed = arrival - remb_receive.first_arrival;
    const int64_t media_us = (int64_t)ticks * 1000 / 90;
    const int64_t transit = (int64_t)(elapsed > INT32_MAX ? INT32_MAX : elapsed) - media_us;
    const int32_t clipped = transit > 2000000 ? 2000000 :
        transit < -2000000 ? -2000000 : (int32_t)transit;
    remb_receive.filtered_transit += (clipped - remb_receive.filtered_transit) / 4;
    remb_receive.last_timestamp = timestamp;
    remb_receive.delay_valid = true;
}

static size_t remb_payload_size(const uint8_t *packet, size_t bytes)
{
    size_t header = 12u + (packet[0] & 15u) * 4u;
    if (header > bytes) return 0;
    if (packet[0] & 16u) {
        if (bytes - header < 4u) return 0;
        const size_t words = (size_t)packet[header + 2] << 8 | packet[header + 3];
        const size_t extension = 4u + words * 4u;
        if (extension > bytes - header) return 0;
        header += extension;
    }
    if (packet[0] & 32u) {
        const unsigned padding = packet[bytes - 1];
        if (!padding || padding > bytes - header) return 0;
        bytes -= padding;
    }
    return bytes - header;
}

static void remb_receive_observe(const uint8_t *packet, size_t bytes, unsigned pt,
    uint32_t ssrc, uint16_t sequence, uint32_t flags, uint64_t now)
{
    const unsigned epoch = atomic_load_explicit(&remb_receive_epoch, memory_order_acquire);
    if (!epoch || !ssrc || !(flags & X4_RD_AUTHENTICATED) || (flags & X4_RD_RETRANSMISSION)) return;
    const size_t payload = remb_payload_size(packet, bytes);
    if (!payload ||
        atomic_flag_test_and_set_explicit(&remb_receive_gate, memory_order_acquire)) return;
    if (atomic_load_explicit(&remb_receive_epoch, memory_order_relaxed) == epoch &&
        pt == remb_receive.payload_type &&
        (!remb_receive.expected_ssrc || ssrc == remb_receive.expected_ssrc)) {
        const bool original = !(flags & (X4_RD_RTX_NORMALIZED | X4_RD_FEC_RECOVERED | X4_RD_RECOVERY43));
        if (!remb_receive.source && original) remb_receive.source = ssrc;
        if (ssrc == remb_receive.source && remb_count_sequence(sequence)) {
            remb_receive.bytes += payload;
            ++remb_receive.packets;
            if (original) {
                const uint32_t timestamp = (uint32_t)packet[4] << 24 | (uint32_t)packet[5] << 16 |
                    (uint32_t)packet[6] << 8 | packet[7];
                remb_receive_transit(timestamp, now);
            }
        }
    }
    atomic_flag_clear_explicit(&remb_receive_gate, memory_order_release);
}

bool x4_rtc_diag_remb_take(unsigned epoch, X4RembReceiveWindow *out)
{
    if (!epoch || !out || atomic_flag_test_and_set_explicit(&remb_receive_gate, memory_order_acquire))
        return false;
    const bool ready = atomic_load_explicit(&remb_receive_epoch, memory_order_acquire) == epoch &&
        remb_receive.source != 0;
    if (ready) {
        *out = (X4RembReceiveWindow){
            .bytes = remb_receive.bytes, .packets = remb_receive.packets,
            .video_ssrc = remb_receive.source, .delay_us = remb_receive.filtered_transit,
            .delay_valid = remb_receive.delay_valid
        };
        remb_receive.bytes = remb_receive.packets = 0;
        remb_receive.filtered_transit = 0;
        remb_receive.have_transit = remb_receive.delay_valid = false;
    }
    atomic_flag_clear_explicit(&remb_receive_gate, memory_order_release);
    return ready;
}
#endif
