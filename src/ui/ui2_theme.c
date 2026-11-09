/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_theme.h"
#include "font.h"
#include <math.h>

enum {
    FOCUS_RADIUS_MAX = 32,
    FOCUS_STAMP = FOCUS_RADIUS_MAX + X4_UI2_FOCUS_SPREAD,
    FOCUS_EDGE = X4_UI2_FOCUS_SPREAD + 4,
    CLOUD_W = 96, CLOUD_H = 60,
};
typedef struct {
    bool ready;
    uint32_t corner[FOCUS_STAMP][FOCUS_STAMP];
} FocusStamp;

/* Rendering and cache creation belong to the same UI owner as apple_ui. */
static FocusStamp focus_stamps[FOCUS_RADIUS_MAX + 1];
static uint32_t focus_edge[FOCUS_EDGE];
static bool edge_ready;
static unsigned char cloud_alpha[CLOUD_H][CLOUD_W];
static bool cloud_ready;

static void blend(uint32_t *pixels, int x, int y, uint32_t color)
{
    if ((color >> 24) && (unsigned)x < X4_UI_WIDTH && (unsigned)y < X4_UI_HEIGHT)
        x4_apple_blend_pixel(pixels + (size_t)y * X4_UI_WIDTH + x, color);
}

static uint32_t focus_color(float d)
{
    unsigned opacity = 0;
    if (d >= -2.55f && d <= 0.25f) opacity = 255;
    else if (d > -3.5f && d < -2.55f) opacity = 110;
    else if (d > 0.25f && d < 1.25f) opacity = 130;
    else if (d >= 1.25f && d < X4_UI2_FOCUS_SPREAD) {
        float t = (X4_UI2_FOCUS_SPREAD - d) / (X4_UI2_FOCUS_SPREAD - 1.25f);
        opacity = (unsigned)(54.0f * t * t);
    }
    return (X4_UI2_ACCENT & 0x00ffffffu) | (opacity << 24);
}

static const FocusStamp *focus_stamp(int radius)
{
    FocusStamp *stamp = &focus_stamps[radius];
    if (!stamp->ready) {
        int size = radius + X4_UI2_FOCUS_SPREAD;
        for (int y = 0; y < size; ++y) {
            float dy = size - y - 0.5f;
            for (int x = 0; x < size; ++x) {
                float dx = size - x - 0.5f;
                stamp->corner[y][x] = focus_color(sqrtf(dx * dx + dy * dy) - radius);
            }
        }
        stamp->ready = true;
    }
    if (!edge_ready) {
        for (int i = 0; i < FOCUS_EDGE; ++i)
            focus_edge[i] = focus_color(X4_UI2_FOCUS_SPREAD - i - 0.5f);
        edge_ready = true;
    }
    return stamp;
}

void x4_ui2_focus(uint32_t *pixels, int x, int y, int w, int h, int radius)
{
    if (!pixels || w < 4 || h < 4) return;
    if (radius < 2) radius = 2;
    if (radius > FOCUS_RADIUS_MAX) radius = FOCUS_RADIUS_MAX;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    const FocusStamp *stamp = focus_stamp(radius);
    const int spread = X4_UI2_FOCUS_SPREAD;
    for (int sy = 0; sy < radius + spread; ++sy) {
        for (int sx = 0; sx < radius + spread; ++sx) {
            uint32_t color = stamp->corner[sy][sx];
            if (!(color >> 24)) continue;
            int left = x - spread + sx, right = x + w + spread - 1 - sx;
            int top = y - spread + sy, bottom = y + h + spread - 1 - sy;
            blend(pixels, left, top, color);
            blend(pixels, right, top, color);
            /* Keep the soft lower elevation under the green ring. */
            if (sy < X4_UI2_FOCUS_SPREAD && (color >> 24) < 75) {
                blend(pixels, left, bottom, X4_RGBA(1, 6, 9, 46));
                blend(pixels, right, bottom, X4_RGBA(1, 6, 9, 46));
            }
            blend(pixels, left, bottom, color);
            blend(pixels, right, bottom, color);
        }
    }
    for (int i = 0; i < FOCUS_EDGE; ++i) {
        uint32_t color = focus_edge[i];
        if (!(color >> 24)) continue;
        int offset = i - spread;
        for (int at = x + radius; at < x + w - radius; ++at) {
            blend(pixels, at, y + offset, color);
            if (offset < -1) blend(pixels, at, y + h - 1 - offset, X4_RGBA(1, 6, 9, 46));
            blend(pixels, at, y + h - 1 - offset, color);
        }
        for (int at = y + radius; at < y + h - radius; ++at) {
            blend(pixels, x + offset, at, color);
            blend(pixels, x + w - 1 - offset, at, color);
        }
    }
}

void x4_ui2_card(uint32_t *pixels, int x, int y, int w, int h, int radius)
{
    x4_apple_fill_round_rect(pixels, x, y, w, h, radius, X4_UI2_SURFACE);
    x4_apple_stroke_round_rect(pixels, x, y, w, h, radius, 1.0f, X4_UI2_BORDER);
}

void x4_ui2_heading(uint32_t *pixels, int x, int y, const char *title)
{
    x4_text_px(pixels, x, y, X4_UI2_TITLE, X4_FONT_BOLD, title, X4_UI2_TEXT);
    x4_apple_fill_round_rect(pixels, x, y + 60, 72, 3, 1, X4_UI2_ACCENT);
}

void x4_ui2_play(uint32_t *pixels, int x, int y, uint32_t color)
{
    for (int row = 3; row < 21; ++row) {
        float edge = 20.0f - fabsf(row + 0.5f - 12.0f) * (14.0f / 9.0f);
        for (int col = 6; col < 20 && col < edge; ++col) {
            float coverage = edge - col;
            unsigned alpha = coverage >= 1.0f ? color >> 24 :
                (unsigned)(coverage * (color >> 24));
            blend(pixels, x + col, y + row, (color & 0x00ffffffu) | (alpha << 24));
        }
    }
}

static void glyph_line(uint32_t *pixels, int x, int y, int ax, int ay,
    int bx, int by, uint32_t color)
{
    const float vx = (float)(bx - ax), vy = (float)(by - ay);
    const float inv = 1.0f / (vx * vx + vy * vy);
    for (int row = 6; row < 22; ++row) {
        for (int col = 6; col < 22; ++col) {
            float dx = col + 0.5f - ax, dy = row + 0.5f - ay;
            float t = (dx * vx + dy * vy) * inv;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            dx -= t * vx;
            dy -= t * vy;
            float a = 1.6f - sqrtf(dx * dx + dy * dy);
            if (a > 1.0f) a = 1.0f;
            if (a > 0.0f)
                blend(pixels, x + col, y + row,
                    (color & 0x00ffffffu) | ((uint32_t)(255.0f * a) << 24));
        }
    }
}

static const char *const pad_words[] = {"OPTIONS", "L1", "R1", "L2", "R2"};

static int glyph_width(enum X4PadGlyph glyph)
{
    return glyph <= X4_GLYPH_SQUARE ? 28 :
        x4_text_width_px(18, X4_FONT_SEMIBOLD, pad_words[glyph - X4_GLYPH_OPTIONS]) + 20;
}

int x4_ui2_pad_glyph(uint32_t *pixels, int x, int y, enum X4PadGlyph glyph)
{
    static const uint32_t colors[] = {
        X4_RGB(121, 194, 255), X4_RGB(255, 137, 152), X4_RGB(101, 241, 166),
        X4_RGB(244, 153, 226),
    };
    if ((unsigned)glyph > X4_GLYPH_R2) return 0;
    if (!pixels) return glyph_width(glyph);
    if (glyph > X4_GLYPH_SQUARE) return x4_apple_draw_pad_glyph(pixels, x, y, glyph);
    const uint32_t color = colors[glyph];
#ifdef X4_UI48_CAPTURE
    x4_ui46_capture_contrast(pixels, "Glifo del pie", x, y, 28, 28, color);
#endif
    x4_apple_stroke_round_rect(pixels, x, y, 28, 28, 14, 1.6f, color);
    if (glyph == X4_GLYPH_CROSS) {
        glyph_line(pixels, x, y, 9, 9, 19, 19, color);
        glyph_line(pixels, x, y, 19, 9, 9, 19, color);
    } else if (glyph == X4_GLYPH_CIRCLE) {
        x4_apple_stroke_round_rect(pixels, x + 7, y + 7, 14, 14, 7, 2.0f, color);
    } else if (glyph == X4_GLYPH_TRIANGLE) {
        glyph_line(pixels, x, y, 14, 8, 20, 19, color);
        glyph_line(pixels, x, y, 20, 19, 8, 19, color);
        glyph_line(pixels, x, y, 8, 19, 14, 8, color);
    } else {
        x4_apple_stroke_round_rect(pixels, x + 8, y + 8, 12, 12, 2, 2.0f, color);
    }
    return 28;
}

void x4_ui2_footer(uint32_t *pixels, const X4Ui2Hint *hints, size_t count)
{
    enum { GAP = 32, PAIR_GAP = 6, INSET = 10, SIZE = 22 };
    if (!count) return;
    int width = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *label = hints[i].label;
        const bool has_label = label && label[0];
        width += glyph_width(hints[i].glyph);
        if (has_label) width += INSET + x4_text_width_px(SIZE, X4_FONT_REGULAR, label);
        if (i + 1 < count) width += has_label ? GAP : PAIR_GAP;
    }
    int x = X4_UI_SAFE_RIGHT - width;
    const int y = X4_UI_FOOTER_Y;
    for (size_t i = 0; i < count; ++i) {
        const int glyph_w = glyph_width(hints[i].glyph);
        const char *label = hints[i].label;
        const bool has_label = label && label[0];
        const int text_w = has_label ? x4_text_width_px(SIZE, X4_FONT_REGULAR, label) : 0;
        const int w = glyph_w + (has_label ? INSET + text_w : 0);
#ifdef X4_UI46_CAPTURE
        x4_ui46_capture_footer(pixels, x, y, w, 32, false);
#endif
        (void)x4_ui2_pad_glyph(pixels, x, y, hints[i].glyph);
        if (has_label) {
#ifdef X4_UI48_CAPTURE
            x4_ui46_capture_contrast(pixels, "Texto del pie", x + glyph_w + INSET,
                y + 2, text_w, SIZE, X4_UI2_TEXT);
#endif
            x4_text_px(pixels, x + glyph_w + INSET, y + 2, SIZE, X4_FONT_REGULAR,
                label, X4_UI2_TEXT);
        }
#ifdef X4_UI46_CAPTURE
        x4_ui46_capture_footer(pixels, x, y, w, 32, true);
#endif
        x += w + (has_label ? GAP : PAIR_GAP);
    }
}

void x4_ui2_chip(uint32_t *pixels, int x, int y, int w, int h,
    const char *label, bool accent)
{
    if (w < 20 || h < 24 || !label) return;
    if (accent)
        x4_apple_fill_round_rect(pixels, x, y, w, h, h / 2, X4_UI2_ACTIVE);
    else
        x4_ui2_card(pixels, x, y, w, h, h / 2);
    int text_w = x4_text_width_px(X4_UI2_CAPTION, X4_FONT_SEMIBOLD, label);
    int inset = text_w < w - 20 ? (w - text_w) / 2 : 10;
    x4_text_ellipsis_px(pixels, x + inset, y + (h - X4_UI2_CAPTION) / 2,
        X4_UI2_CAPTION, X4_FONT_SEMIBOLD, label, w - 2 * inset,
        accent ? X4_UI2_ACCENT : X4_UI2_TEXT);
}

void x4_ui2_button(uint32_t *pixels, int x, int y, int w, int h,
    const char *label, bool primary, bool focused, enum X4TablerIconId icon)
{
    if (w < 36 || h < 30 || !label) return;
    const int radius = h / 2;
    if (primary)
        x4_apple_fill_round_rect(pixels, x, y, w, h, radius, X4_UI2_ACCENT);
    else
        x4_ui2_card(pixels, x, y, w, h, radius);
    if (focused) x4_ui2_focus(pixels, x, y, w, h, radius);
    const uint32_t color = primary ? X4_UI2_INK : X4_UI2_TEXT;
    const bool has_icon = icon >= 0 && icon < X4_ICON_COUNT;
    const int icon_space = has_icon ? 34 : 0;
    int text_w = x4_text_width_px(X4_UI2_BODY, X4_FONT_BOLD, label) + icon_space;
    int inset = text_w < w - 24 ? (w - text_w) / 2 : 12;
    if (has_icon)
        x4_apple_draw_icon(pixels, x + inset, y + (h - 24) / 2, icon, color);
    x4_text_ellipsis_px(pixels, x + inset + icon_space, y + (h - X4_UI2_BODY) / 2,
        X4_UI2_BODY, X4_FONT_BOLD, label, w - 2 * inset - icon_space, color);
}

typedef struct { float x, y; } Point;
typedef struct { Point a; float vx, vy, inv_length; } CloudSegment;

static Point bezier(Point a, Point b, Point c, Point d, float t)
{
    float u = 1.0f - t;
    return (Point){
        u*u*u*a.x + 3*u*u*t*b.x + 3*u*t*t*c.x + t*t*t*d.x,
        u*u*u*a.y + 3*u*u*t*b.y + 3*u*t*t*c.y + t*t*t*d.y,
    };
}

static void prepare_cloud(void)
{
    if (cloud_ready) return;
    /* Original closed contour. The short flat base anchors the wordmark. */
    static const Point controls[] = {
        {24,48}, {13,48}, {7,42}, {7,34},
        {7,26}, {13,21}, {22,20},
        {25,11}, {34,6}, {45,6},
        {57,6}, {67,13}, {70,23},
        {81,22}, {89,28}, {89,36},
        {89,44}, {83,48}, {73,48},
    };
    enum { CURVES = 6, SUBDIVISIONS = 8, SEGMENTS = CURVES * SUBDIVISIONS + 1 };
    CloudSegment segments[SEGMENTS];
    unsigned n = 0;
    for (int i = 0; i < CURVES; ++i) {
        const Point *p = &controls[i * 3];
        Point last = p[0];
        for (int j = 1; j <= SUBDIVISIONS; ++j) {
            Point at = bezier(p[0], p[1], p[2], p[3], (float)j / SUBDIVISIONS);
            float vx = at.x - last.x, vy = at.y - last.y;
            segments[n++] = (CloudSegment){last, vx, vy, 1.0f / (vx*vx + vy*vy)};
            last = at;
        }
    }
    segments[n++] = (CloudSegment){{73,48}, -49, 0, 1.0f / (49.0f * 49.0f)};
    for (int y = 0; y < CLOUD_H; ++y) {
        for (int x = 0; x < CLOUD_W; ++x) {
            float best = 100000.0f;
            for (unsigned i = 0; i < n; ++i) {
                const CloudSegment *segment = &segments[i];
                float px = x + 0.5f - segment->a.x, py = y + 0.5f - segment->a.y;
                float t = (px * segment->vx + py * segment->vy) * segment->inv_length;
                if (t < 0.0f) t = 0.0f;
                else if (t > 1.0f) t = 1.0f;
                float dx = px - t * segment->vx, dy = py - t * segment->vy;
                float d = dx * dx + dy * dy;
                if (d < best) best = d;
            }
            float alpha = 2.8f - sqrtf(best);
            if (alpha > 1.0f) alpha = 1.0f;
            cloud_alpha[y][x] = alpha > 0.0f ? (unsigned char)(255.0f * alpha + 0.5f) : 0;
        }
    }
    cloud_ready = true;
}

static int cloud_height(int height)
{
    if (height < 8) return 8;
    return height > 128 ? 128 : height;
}

int x4_ui2_cloud_width(int height)
{
    return cloud_height(height) * CLOUD_W / CLOUD_H;
}

static void draw_cloud(uint32_t *pixels, int x, int y, int height, uint32_t color, bool brand)
{
    if (!pixels) return;
    height = cloud_height(height);
    int width = x4_ui2_cloud_width(height);
    unsigned opacity = color >> 24;
    color &= 0x00ffffffu;
    prepare_cloud();
    for (int row = 0; row < height; ++row) {
        int sy = ((2 * row + 1) * CLOUD_H) / (2 * height);
        for (int col = 0; col < width; ++col) {
            int sx = ((2 * col + 1) * CLOUD_W) / (2 * width);
            unsigned alpha = (cloud_alpha[sy][sx] * opacity + 127u) / 255u;
            if (alpha) {
                uint32_t tint = color;
                if (brand) {
                    const unsigned t = (unsigned)(255 * col / (width - 1));
                    tint = X4_RGB(34 * t / 255, 212 + 15 * t / 255, 255 - 145 * t / 255)
                        & 0x00ffffffu;
                }
                blend(pixels, x + col, y + row, tint | (alpha << 24));
            }
        }
    }
}

void x4_ui2_cloud(uint32_t *pixels, int x, int y, int height, uint32_t color)
{
    draw_cloud(pixels, x, y, height, color, false);
}

void x4_ui2_brand_cloud(uint32_t *pixels, int x, int y, int height)
{
    draw_cloud(pixels, x, y, height, X4_RGB(0, 212, 255), true);
}

static int brand_size(int size)
{
    if (size < 16) return 16;
    return size > 80 ? 80 : size;
}

int x4_ui2_brand_width(int size)
{
    size = brand_size(size);
    return x4_ui2_cloud_width(size) + size / 4 +
        x4_text_width_px(size, X4_FONT_BOLD, "XCloud") +
        x4_text_width_px(size, X4_FONT_BOLD, "4");
}

int x4_ui2_brand(uint32_t *pixels, int x, int y, int size)
{
    size = brand_size(size);
    x4_ui2_brand_cloud(pixels, x, y, size);
    int text_x = x + x4_ui2_cloud_width(size) + size / 4;
    x4_text_px(pixels, text_x, y, size, X4_FONT_BOLD, "XCloud", X4_UI2_TEXT);
    text_x += x4_text_width_px(size, X4_FONT_BOLD, "XCloud");
    x4_text_px(pixels, text_x, y, size, X4_FONT_BOLD, "4", X4_RGB(34, 227, 110));
    return text_x - x + x4_text_width_px(size, X4_FONT_BOLD, "4");
}
