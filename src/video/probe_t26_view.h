/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#if defined(X4_VIDEO_PROBE) && X4_VIDEO_PROBE && \
    defined(X4_PROBE_T26) && X4_PROBE_T26

#include <stdbool.h>
#include <stdint.h>
#include "probe_t26_plan.h"

#define X4_T26_VIEW_TITLE "Banco de decodificador T26: no apagues la consola"
enum { X4_T26_VIEW_WIDTH = 1920, X4_T26_VIEW_HEIGHT = 1080,
    X4_T26_VIEW_ROWS = 12, X4_T26_VIEW_TEXT = 160,
    X4_T26_VIEW_PREPARING = X4_T26_MODE_COUNT };

/* Plain snapshot captured by value by the owner; no clocks, native calls,
 * shared state, input handling or measurements belong to this renderer.
 * cell is the zero-based current cell. total=0 uses the 43-cell plan.
 * remaining_us=0 while running means the estimate is not available yet.
 * Rows/status are trusted app text, copied with bounds even if not NUL-ended.
 */
typedef struct {
    unsigned cell, total, mode;
    uint64_t remaining_us;
    bool done, retained, host_example;
    char status[X4_T26_VIEW_TEXT];
    unsigned row_count;
    char rows[X4_T26_VIEW_ROWS][X4_T26_VIEW_TEXT];
} X4T26ViewSnapshot;

typedef struct {
    unsigned text_lines, rows_drawn, ellipsized_lines, bounds_failures;
    unsigned minimum_font_px;
    int maximum_text_right, maximum_text_bottom;
} X4T26ViewMetrics;

/* pixels owns a complete 1920x1080 RGBA buffer (red in the low byte).
 * No allocation or file access. Draw only outside the timed measurement.
 * The owner is responsible for presentation and snapshot synchronization.
 */
int x4_t26_view_draw(uint32_t *pixels, const X4T26ViewSnapshot *,
    X4T26ViewMetrics *optional_metrics);

#endif
