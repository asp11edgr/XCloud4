/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "ui2_modal.h"
#include "font.h"
#include "canvas.h"

static inline void x4_ui2_settings_line(uint32_t *p, int x, int y, int size, int weight,
    const char *label, int width, uint32_t color)
{
    (void)x4_ui2_text_bounded(p, x, y, size, weight, label, width, size, 1, color);
}

static inline void x4_ui2_settings_chevron(uint32_t *p, int x, int y, uint32_t color)
{
    for (int i = 0; i < 6; ++i) {
        x4_rect(p, x + i, y + i, 2, 2, color);
        x4_rect(p, x + 10 - i, y + i, 2, 2, color);
    }
}

static inline void x4_ui2_settings_row(uint32_t *p, int x, int y, int w, int h,
    const char *label, const char *value, bool focused, bool dropdown)
{
    x4_ui2_card(p, x, y, w, h, X4_UI2_RADIUS_CONTROL);
    if (focused) x4_ui2_focus(p, x, y, w, h, X4_UI2_RADIUS_CONTROL);
    const int label_w = w * 44 / 100;
    const int right = x + w - 22 - (dropdown ? 26 : 0);
    const int value_w = right - (x + 22 + label_w + 18);
    int value_x = right - x4_text_width_px(24, X4_FONT_SEMIBOLD, value);
    if (value_x < right - value_w) value_x = right - value_w;
    x4_ui2_settings_line(p, x + 22, y + (h - 24) / 2, 24, X4_FONT_SEMIBOLD, label, label_w, X4_UI2_TEXT);
    x4_ui2_settings_line(p, value_x, y + (h - 24) / 2, 24, X4_FONT_SEMIBOLD,
        value, value_w, focused ? X4_UI2_ACCENT : X4_UI2_MUTED);
    if (dropdown) x4_ui2_settings_chevron(p, x + w - 31, y + (h - 8) / 2, X4_UI2_MUTED);
}

static inline void x4_ui2_settings_toggle(uint32_t *p, int x, int y, bool enabled)
{
    x4_apple_fill_round_rect(p, x, y, 54, 28, 14, enabled ? X4_UI2_ACCENT : X4_UI2_BORDER);
    x4_apple_fill_round_rect(p, x + (enabled ? 30 : 4), y + 4, 20, 20, 10,
        enabled ? X4_UI2_INK : X4_UI2_TEXT);
}
