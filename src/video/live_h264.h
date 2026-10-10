/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../media/live_trace.h"
enum { X4_LIVE_WIDTH = 1280, X4_LIVE_HEIGHT = 720, X4_LIVE_AU_MAX = 2 * 1024 * 1024,
    X4_LIVE_COPY_READERS = 4 };
typedef struct {
    void *state;
    uint32_t *pixels;
    unsigned width, height;
    uint64_t frames;
    /* Video-owner cumulative timings in sceKernelGetProcessTime microseconds. */
    uint64_t decode_calls, decoded_frames, no_picture_calls, decode_us, decode_max_us;
    uint64_t convert_us, convert_max_us;
    /* Selected-copy wall time includes dispatch/completion, but excludes the
     * first-copy serial reference/compare. Reader times may overlap. */
    uint64_t copy_calls, copy_us, copy_max_us, copy_bytes;
    /* helper_us is the SUM of overlapping elapsed spans across three helpers;
     * it is neither CPU time nor an additional contribution to copy wall. */
    uint64_t copy_parallel_calls, copy_serial_calls, copy_owner_us, copy_helper_us;
    uint64_t copy_wait_us, copy_wait_max_us, forced_preserve_calls;
    uint64_t copy_check_attempts, copy_check_pass, copy_check_mismatch, copy_check_not_checked;
    uint64_t copy_check_bytes, copy_check_us;
    uint64_t last_picture_time_us, picture_gap_max_us;
    int error;
    char stage[48];
    /* Session diagnostic ordinals survive decoder restart. Native output has
     * no returned PTS: output IDs never identify an input AU. */
    X4Trace *trace;
    uint64_t trace_au, trace_decode_next, trace_output_next, trace_copy_next;
    uint64_t trace_decoder_epoch, trace_converted_output;
} X4LiveVideo;
/* All calls run on one video-owner thread. Three optional persistent CPU-only
 * copy helpers finish their disjoint spans before conversion, Decode or native
 * teardown. A partial pool is stopped/joined before serial fallback. The caller
 * retains RGB storage until teardown succeeds. Any failed helper join latches
 * an error and retains the entire state, native and staging storage. */
int x4_live_video_start(X4LiveVideo *, uint32_t *pixels, size_t pixel_capacity);
/* Select an unpublished destination before a batch. No reader may own it. */
int x4_live_video_set_target(X4LiveVideo *, uint32_t *pixels, size_t pixel_capacity);
int x4_live_video_feed(X4LiveVideo *, const uint8_t *, size_t, uint64_t pts);
/* Publish the last validated native picture of this batch. A feed converts
 * first if its output reservation would overwrite that pending picture. */
int x4_live_video_convert_pending(X4LiveVideo *);
int x4_live_video_stop(X4LiveVideo *);
