/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../media/live_media.h"
typedef struct X4LiveAudio X4LiveAudio;
typedef struct {
    uint64_t packets, frames, dropped, lost, bad_frames, underflows;
    int error;
    bool playing;
} X4LiveAudioSnapshot;
/* payload_type is fixed before the worker starts; -1 locks the first RTP. */
X4LiveAudio *x4_live_audio_start(int *error, int payload_type);
void x4_live_audio_receive(X4LiveAudio *, const uint8_t *, size_t);
void x4_live_audio_snapshot(const X4LiveAudio *, X4LiveAudioSnapshot *);
void x4_live_audio_mute(X4LiveAudio *, bool);
int x4_live_audio_stop(X4LiveAudio *);
