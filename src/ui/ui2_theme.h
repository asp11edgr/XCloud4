/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "apple_ui.h"

#define X4_UI2_BACKGROUND X4_RGB(7, 14, 24)
#define X4_UI2_SURFACE    X4_RGB(14, 27, 42)
#define X4_UI2_BORDER     X4_RGB(42, 65, 82)
#define X4_UI2_TEXT       X4_RGB(246, 249, 253)
#define X4_UI2_MUTED      X4_RGB(177, 193, 210)
#define X4_UI2_ACCENT     X4_RGB(50, 243, 139)
#define X4_UI2_INK        X4_RGB(5, 27, 18)
#define X4_UI2_ACTIVE     X4_RGB(13, 54, 43)

/* TV typography in px: page title 48, section title 32, subtitle 26,
 * metadata 24, support 20. These match the prepared font sizes. */
enum {
    X4_UI2_CAPTION = 20, X4_UI2_BODY = 24, X4_UI2_LABEL = 26,
    X4_UI2_HEADING = 32, X4_UI2_TITLE = 48,
    X4_UI2_RADIUS_CONTROL = 13, X4_UI2_RADIUS_CARD = 14,
    X4_UI2_SIDEBAR_W = 236, X4_UI2_CONTENT_X = 272, X4_UI2_FOCUS_SPREAD = 7,
};

/* Leave the label empty to group this glyph with the following hint. */
typedef struct { enum X4PadGlyph glyph; const char *label; } X4Ui2Hint;

void x4_ui2_heading(uint32_t *pixels, int x, int y, const char *title);
int x4_ui2_pad_glyph(uint32_t *pixels, int x, int y, enum X4PadGlyph glyph);
void x4_ui2_footer(uint32_t *pixels, const X4Ui2Hint *hints, size_t count);
void x4_ui2_play(uint32_t *pixels, int x, int y, uint32_t color);
void x4_ui2_card(uint32_t *pixels, int x, int y, int w, int h, int radius);
/* Cached glow falls within X4_UI2_FOCUS_SPREAD px of the box. */
void x4_ui2_focus(uint32_t *pixels, int x, int y, int w, int h, int radius);
void x4_ui2_chip(uint32_t *pixels, int x, int y, int w, int h,
    const char *label, bool accent);
void x4_ui2_button(uint32_t *pixels, int x, int y, int w, int h,
    const char *label, bool primary, bool focused, enum X4TablerIconId icon);
/* Original wordmark, with the accent on the cloud and the final 4. */
int x4_ui2_brand_width(int size);
int x4_ui2_brand(uint32_t *pixels, int x, int y, int size);
int x4_ui2_cloud_width(int height);
void x4_ui2_cloud(uint32_t *pixels, int x, int y, int height, uint32_t color);
void x4_ui2_brand_cloud(uint32_t *pixels, int x, int y, int height);
