/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_boot.h"
#include "font.h"
#include <string.h>

enum { BOOT_PROGRESS_W = 570, BOOT_PROGRESS_Y = 617, BOOT_LANE_Y = 631 };
static const X4UiRect boot_progress = {
    (X4_UI_WIDTH - BOOT_PROGRESS_W) / 2, BOOT_PROGRESS_Y, BOOT_PROGRESS_W, 120,
};
static const X4UiRect boot_title = {
    (X4_UI_WIDTH - 1050) / 2, 400, 1050, 150,
};
static struct {
    uint32_t *destination;
    enum X4Lang language;
    X4Ui2BootStep step;
    bool fallback;
    size_t damage;
    X4UiRect rect;
} boot;

static void centered(uint32_t *pixels, int y, int size, int weight,
    const char *text, uint32_t color)
{
    int width = x4_text_width_px(size, weight, text);
    x4_text_px(pixels, (X4_UI_WIDTH - width) / 2, y, size, weight, text, color);
}

static void background_rows(uint32_t *pixels, X4UiRect rect)
{
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        unsigned light = (unsigned)(y < 560 ? y / 70 : (X4_UI_HEIGHT - y) / 65);
        uint32_t color = X4_RGB(6 + light, 14 + light, 24 + light);
        uint32_t *row = pixels + (size_t)y * X4_UI_WIDTH + rect.x;
        for (int x = 0; x < rect.w; ++x) row[x] = color;
    }
}

static void backdrop(uint32_t *pixels)
{
    /* The background and green rim are built only on a fresh boot canvas. */
    background_rows(pixels, (X4UiRect){0, 0, X4_UI_WIDTH, X4_UI_HEIGHT});
    for (int x = 0; x < X4_UI_WIDTH; ++x) {
        int d = x - 1536;
        int curve = 982 - d * d / 14200;
        uint32_t *column = pixels + (size_t)curve * X4_UI_WIDTH + x;
        for (int row = 0; row < 3; ++row) column[row * X4_UI_WIDTH] = X4_RGB(11, 50, 45);
        for (int row = 3; row < 9; ++row) column[row * X4_UI_WIDTH] = X4_RGB(10, 38, 36);
        for (int row = 9; row < 21; ++row) column[row * X4_UI_WIDTH] = X4_RGB(9, 29, 32);
    }
    x4_ui2_brand_cloud(pixels, (X4_UI_WIDTH - x4_ui2_cloud_width(116)) / 2, 271, 116);
}

static void title(uint32_t *pixels, bool fallback)
{
    const int logo_size = 64;
    const int word_width = x4_text_width_px(logo_size, X4_FONT_BOLD, "XCloud");
    const int digit_width = x4_text_width_px(logo_size, X4_FONT_BOLD, "4");
    int logo_x = (X4_UI_WIDTH - word_width - digit_width) / 2;
    x4_text_px(pixels, logo_x, 401, logo_size, X4_FONT_BOLD, "XCloud", X4_UI2_TEXT);
    x4_text_px(pixels, logo_x + word_width, 401, logo_size, X4_FONT_BOLD, "4", X4_RGB(34, 227, 110));
    centered(pixels, 496, fallback ? 32 : 26, X4_FONT_REGULAR,
        x4_tr(X4_TR_50_BOOT_TAGLINE), X4_UI2_MUTED);
}

bool x4_ui2_boot_draw(uint32_t *pixels, X4Ui2BootStep step)
{
    if (!pixels || (unsigned)step > X4_UI2_BOOT_READY) return false;
    enum X4Lang language = x4_lang_current();
    bool fallback = x4_font_is_fallback() != 0;
    bool full = pixels != boot.destination;
    bool text_changed = language != boot.language || fallback != boot.fallback;
    if (!full && !text_changed && step == boot.step) {
        boot.damage = 0;
        return false;
    }
    if (full) backdrop(pixels);
    else if (text_changed) background_rows(pixels, boot_title);
    if (full || text_changed) title(pixels, fallback);
    const X4UiRect r = boot_progress;
    if (!full) background_rows(pixels, r);
    x4_apple_fill_round_rect(pixels, r.x + 6, BOOT_LANE_Y, r.w - 12, 10, 5, X4_UI2_BORDER);
    int fill = (r.w - 12) * (int)step / X4_UI2_BOOT_READY;
    if (fill) x4_apple_fill_round_rect(pixels, r.x + 6, BOOT_LANE_Y, fill, 10, 5, X4_UI2_ACCENT);
    static const enum X4TextId labels[] = {
        X4_TR_50_BOOT_FONTS, X4_TR_50_BOOT_CACHE, X4_TR_50_BOOT_SESSION,
        X4_TR_50_BOOT_CATALOG, X4_TR_50_BOOT_READY,
    };
    /* Before the external atlas opens, the embedded face is Regular 32. */
    centered(pixels, 672, fallback ? 32 : 28, fallback ? X4_FONT_REGULAR : X4_FONT_SEMIBOLD,
        x4_tr(labels[step]), X4_UI2_TEXT);
    boot.destination = pixels;
    boot.language = language;
    boot.step = step;
    boot.fallback = fallback;
    boot.damage = full ? SIZE_MAX : 1;
    boot.rect = text_changed ? boot_title : r;
    boot.rect.h = r.y + r.h - boot.rect.y;
    return true;
}

void x4_ui2_boot_reset(void)
{
    memset(&boot, 0, sizeof(boot));
}

size_t x4_ui2_boot_damage(X4UiRect *rect)
{
    if (rect && boot.damage == 1) *rect = boot.rect;
    return boot.damage;
}
