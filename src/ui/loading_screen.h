/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../auth/xbox_session.h"
#include "product_metadata.h"

enum X4LoadingStage {
    X4_LOADING_REQUEST, X4_LOADING_RESOURCES, X4_LOADING_AUTHORIZE,
    X4_LOADING_CONNECT, X4_LOADING_CONNECTED, X4_LOADING_FIRST_PICTURE,
    X4_LOADING_COMPLETE, X4_LOADING_ERROR
};
enum X4LoadingStep {
    X4_LOADING_STEP_SESSION, X4_LOADING_STEP_SERVER, X4_LOADING_STEP_OPTIMIZE,
    X4_LOADING_STEP_READY, X4_LOADING_STEPS, X4_LOADING_STEP_INVALID
};
/* An in-progress step, all steps done, or invalid (the error card owns UI). */
enum X4LoadingStep x4_loading_screen_step(enum X4LoadingStage stage);
typedef struct {
    enum X4LoadingStage stage;
    unsigned percent, waiting_seconds;
    bool waiting_for_resources;
    unsigned queue_elapsed_seconds;
    bool queue_eta_available;
    unsigned queue_eta_seconds;
    bool active, has_game_background, first_picture, delayed_notice;
    uint64_t background_preparations, background_us, render_calls;
    uint64_t full_buffer_initializations, region_copies, fade_frames;
    uint64_t render_max_us, frame_total_max_us, static_preparations, static_max_us;
    uint64_t first_picture_us, transition_ready_us, transition_wait_us, fade_max_us;
    uint64_t fade_seed_copies, fade_full_initializations, fade_blocks_written;
} X4LoadingView;

/* Main/UI owner only. Copies and blurs the borrowed 16:9 image during begin;
 * no asset pointer is retained. Null/non-16:9 images use original artwork.
 * Fixed maximum heap: two 1920x1080 surfaces and one 900x680 backing zone
 * (18.16 MiB), all normal cacheable memory and released at end.
 * No file/network access, worker, wait, or decoder operation occurs here. */
bool x4_loading_screen_begin(const char *title, const X4CoverRaster *game,
    uint64_t now_us);
void x4_loading_screen_end(void);
bool x4_loading_screen_active(void);
/* Actual session states only advance progress. Time interpolation is visibly
 * an estimate in the FIRST_PICTURE stage, capped at 95% until a real image. */
void x4_loading_screen_update(const X4SessionSnapshot *session,uint64_t now_us);
void x4_loading_screen_first_picture(uint64_t now_us);
/* First picture means a validated native output is available, not displayed.
 * The main owner keeps its existing UI display until this 160ms transition
 * is ready, then performs the unchanged stream-display handoff. This clock
 * deadline needs no second AU or converted-image callback. */
bool x4_loading_screen_transition_ready(uint64_t now_us);
/* Draws at most30 new animation frames/s in a320x180 zone. A new UI buffer
 * gets one full cached copy; stage/bar/percent/notice rectangles are updated
 * only when their cached state changes independently for each UI buffer.
 * Returns true when pixels were written. Error cards remain screen.c's job. */
bool x4_loading_screen_draw(uint32_t *pixels,uint64_t now_us);
/* After first_picture, draw dissolves through dark over160ms. The100% center
 * is copied once per UI buffer; later frames only store newly blackened8x8
 * blocks, with no destination reads or repeated full-screen copies. No
 * loading pixels are composed on converted video after the handoff. */
void x4_loading_screen_snapshot(X4LoadingView *out);
/* One JSON object, no comma or key. Numeric diagnostics only. */
void x4_loading_screen_metrics_write(FILE *file);
