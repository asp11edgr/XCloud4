/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { X4_LIVE_WIDTH = 1280, X4_LIVE_HEIGHT = 720, X4_LIVE_AU_MAX = 2 * 1024 * 1024 };
typedef struct {
    void *state;
    uint32_t *pixels;
    unsigned width, height;
    uint64_t frames;
    /* Video-owner cumulative timings in sceKernelGetProcessTime microseconds. */
    uint64_t decode_calls, decoded_frames, no_picture_calls, decode_us, decode_max_us;
    uint64_t convert_us, convert_max_us;
    uint64_t copy_calls, copy_us, copy_max_us, copy_bytes;
    uint64_t last_picture_time_us, picture_gap_max_us;
    int error;
    char stage[48];
} X4LiveVideo;
/* All calls run on one video-owner thread. The caller retains the bounded
 * RGB destination until native teardown succeeds; the decoder never frees it. */
int x4_live_video_start(X4LiveVideo *, uint32_t *pixels, size_t pixel_capacity);
/* Select an unpublished destination before a batch. No reader may own it. */
int x4_live_video_set_target(X4LiveVideo *, uint32_t *pixels, size_t pixel_capacity);
int x4_live_video_feed(X4LiveVideo *, const uint8_t *, size_t, uint64_t pts);
/* Publish the last validated native picture of this batch. A feed converts
 * first if its output reservation would overwrite that pending picture. */
int x4_live_video_convert_pending(X4LiveVideo *);
int x4_live_video_stop(X4LiveVideo *);
