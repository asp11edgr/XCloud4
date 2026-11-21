/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "apple_ui.h"

#define X4_UI2_BACKGROUND X4_RGB(13, 22, 18)
#define X4_UI2_SURFACE    X4_RGB(25, 35, 30)
#define X4_UI2_BORDER     X4_RGB(58, 73, 64)
#define X4_UI2_TEXT       X4_RGB(248, 250, 249)
#define X4_UI2_MUTED      X4_RGB(179, 194, 185)
#define X4_UI2_ACCENT     X4_RGB(123, 218, 77)
#define X4_UI2_INK        X4_RGB(12, 21, 15)
#define X4_UI2_ACTIVE     X4_RGB(42, 66, 46)
#define X4_UI2_ELEVATED   X4_RGB(34, 47, 40)
#define X4_UI2_ACCENT_SOFT X4_RGB(145, 231, 96)
#define X4_UI2_TINT(opacity) X4_RGBA(13, 22, 18, opacity)

/* TV typography in px: page title 48, section title 32, subtitle 26,
 * metadata 24, support 20. These match the prepared font sizes. */
enum {
    X4_UI2_CAPTION = 20, X4_UI2_BODY = 24, X4_UI2_LABEL = 26,
    X4_UI2_HEADING = 32, X4_UI2_TITLE = 48,
    X4_UI2_RADIUS_CONTROL = 8, X4_UI2_RADIUS_CARD = 10,
    X4_UI2_CONTENT_X = X4_UI_SAFE_LEFT, X4_UI2_HEADER_H = 108, X4_UI2_FOCUS_SPREAD = 7,
};

/* Leave the label empty to group this glyph with the following hint. */
typedef struct { enum X4PadGlyph glyph; const char *label; } X4Ui2Hint;

void x4_ui2_heading(uint32_t *pixels, int x, int y, const char *title);
int x4_ui2_pad_glyph(uint32_t *pixels, int x, int y, enum X4PadGlyph glyph);
void x4_ui2_footer(uint32_t *pixels, const X4Ui2Hint *hints, size_t count);
void x4_ui2_footer_left(uint32_t *pixels, const X4Ui2Hint *hints, size_t count);
void x4_ui2_play(uint32_t *pixels, int x, int y, uint32_t color);
void x4_ui2_card(uint32_t *pixels, int x, int y, int w, int h, int radius);
/* Cached glow falls within X4_UI2_FOCUS_SPREAD px of the box. */
void x4_ui2_focus(uint32_t *pixels, int x, int y, int w, int h, int radius);
void x4_ui2_chip(uint32_t *pixels, int x, int y, int w, int h,
    const char *label, bool accent);
void x4_ui2_button(uint32_t *pixels, int x, int y, int w, int h,
    const char *label, bool primary, bool focused, enum X4TablerIconId icon);
/* Wordmark with the interface accent on the cloud. */
int x4_ui2_brand_width(int size);
int x4_ui2_brand(uint32_t *pixels, int x, int y, int size);
int x4_ui2_cloud_width(int height);
void x4_ui2_cloud(uint32_t *pixels, int x, int y, int height, uint32_t color);
void x4_ui2_brand_cloud(uint32_t *pixels, int x, int y, int height);
bool x4_ui2_draw_qr(uint32_t *pixels, int center_x, int center_y, int edge, const char *url);
void x4_ui2_qr_frame(uint32_t *pixels, int x, int y, int edge);
