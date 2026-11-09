/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { X4_MIC_RTP_HEADER = 12, X4_MIC_RTCP_SR_BYTES = 44, X4_MIC_RR_SLOTS = 64 };

typedef struct {
    uint32_t ssrc, timestamp;
    uint16_t sequence;
} X4MicRtpSeed;

typedef struct {
    X4MicRtpSeed seed;
    uint16_t frame_samples;
    uint64_t first_sample_us, frame_index, epoch;
    bool started;
} X4MicRtpClock;

typedef struct {
    unsigned reports, fraction_lost;
    int32_t total_lost;
    uint32_t jitter, last_sequence;
} X4MicReceiverStats;

/* Bounded retry for an unusable SSRC. Each attempt uses one entropy call. */
bool x4_mic_rtp_seed(X4MicRtpSeed *seed, int (*random_bytes)(void *, size_t));
bool x4_mic_rtp_header(X4MicRtpClock *clock, uint64_t sample_us, uint64_t epoch,
    uint8_t header[X4_MIC_RTP_HEADER]);
uint32_t x4_mic_rtp_time_at(const X4MicRtpClock *clock, uint64_t now_us);
void x4_mic_rtcp_sender_report(uint8_t packet[X4_MIC_RTCP_SR_BYTES], uint32_t ssrc,
    uint64_t ntp, uint32_t rtp_timestamp, uint32_t packets, uint32_t payload_bytes);
bool x4_mic_sdp_can_send(const char *sdp, size_t size);

/* Session owner uses a process-lifetime, never-reused RTC slot. Observers
 * publish only atomic numbers, and can never dereference a heap peer. */
bool x4_mic_rr_bind(unsigned slot, uint32_t ssrc);
void x4_mic_rr_unbind(unsigned slot);
void x4_mic_rr_observe(const void *packet, size_t bytes);
X4MicReceiverStats x4_mic_rr_snapshot(unsigned slot);
