/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../media/live_media.h"
#include <stdio.h>
typedef struct X4LiveAudio X4LiveAudio;
typedef struct {
    uint64_t packets, bytes, frames, dropped, lost, bad_frames, underflows;
    int error;
    bool playing;
    bool previous, detailed_metrics_available, fec_implemented, worker_joined;
    uint64_t plc_samples, fec_packets, pcm_overflows, decode_errors, trimmed_samples, restarts;
    uint64_t underflow_episodes, underflow_interval_count, underflow_interval_sum_us;
    uint64_t underflow_interval_min_us, underflow_interval_max_us;
    uint64_t buffer_observations, buffer_p5_us, buffer_p50_us, buffer_p95_us, buffer_max_us;
    uint64_t packet_duration_count, packet_duration_p50_us, packet_duration_p95_us, packet_duration_max_us;
    uint64_t target_observations, target_p50_us, target_max_us, target_adjustments;
    unsigned target_current_samples, output_block_samples;
    int open_512_rc, open_1024_rc;
    bool open_512_attempted, open_1024_attempted;
    uint64_t trim_events;
} X4LiveAudioSnapshot;
/* payload_type is fixed before the worker starts; -1 locks the first RTP. */
X4LiveAudio *x4_live_audio_start(int *error, int payload_type);
/* previous=true preserves the 0.7.47 worker; false selects adaptive 50 ms/PLC/FEC. */
X4LiveAudio *x4_live_audio_start_mode(int *error, int payload_type, bool previous);
void x4_live_audio_receive(X4LiveAudio *, const uint8_t *, size_t);
/* RTC closed/joined: owner resets RTP reception only; never reopens AudioOut. */
int x4_live_audio_begin_epoch(X4LiveAudio *);
/* Only media bridge with an immutable per-peer generation. */
void x4_live_audio_receive_for_epoch(X4LiveAudio *,const uint8_t *,size_t,uint64_t);
void x4_live_audio_snapshot(const X4LiveAudio *, X4LiveAudioSnapshot *);
void x4_live_audio_metrics_write(FILE *, const X4LiveAudioSnapshot *);
void x4_live_audio_mute(X4LiveAudio *, bool);
int x4_live_audio_stop(X4LiveAudio *);
/* Final metrics are taken after a successful worker join and before teardown.
 * On a join error the context is retained and worker_joined remains false. */
int x4_live_audio_stop_report(X4LiveAudio *, X4LiveAudioSnapshot *);
