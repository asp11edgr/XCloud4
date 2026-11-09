/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
enum X4UiScreen {
    X4_UI_MENU, X4_UI_CATALOG, X4_UI_ACCOUNT, X4_UI_FORTNITE,
    X4_UI_ABOUT, X4_UI_SUPPORT, X4_UI_LOADING, X4_UI_QUEUE, X4_UI_ERRORS,
    X4_UI_OPTIONS_PICTURE, X4_UI_OPTIONS_SERVER, X4_UI_FIRST_TIME,
    X4_UI_KEYBOARD, X4_UI_LAB_SETTINGS, X4_UI_DEMO, X4_UI_OTHER,
    X4_UI_DISCOVER, X4_UI_RECENTS, X4_UI_GAME_PAGE, X4_UI_BOOT,
    X4_UI_DISCOVER_FADE, X4_UI_MODAL,
    X4_UI_SCREEN_COUNT
};
typedef struct {
    uint64_t n,p50_us,p95_us,max_us,total_us,overflow;
    bool p50_overflow,p95_overflow;
} X4UiTimingView;
typedef struct {
    uint64_t presentations,copy_only,copied_bytes;
    X4UiTimingView draw,copy,present;
} X4UiDrawMetricView;
/* Main/UI owner only. Draw excludes copy/flip; copy is just NT transfer and
 * sfence; present includes transfer, native submit and the existing flip wait.
 * A buffer-coherency-only present is never counted as a zero-cost redraw. */
void x4_ui_draw_metrics_reset(void);
void x4_ui_draw_metrics_record(enum X4UiScreen screen,bool navigation,
    bool redrawn,uint64_t draw_us,uint64_t copy_us,uint64_t present_us,
    uint64_t copied_bytes,uint64_t now_us,uint64_t dirty_skips);
void x4_ui_draw_metrics_poll(uint64_t now_us,uint64_t dirty_skips);
void x4_ui_draw_metrics_flush(uint64_t now_us);
bool x4_ui_draw_metrics_snapshot(enum X4UiScreen screen,bool navigation,X4UiDrawMetricView *out);
/* Exact formatter also exercised by host tests. All values are numeric and
 * screen labels are constants; no account or session identifiers are logged. */
void x4_ui_draw_metrics_write(FILE *,enum X4UiScreen,bool,const X4UiDrawMetricView *,uint64_t dirty_skips);
