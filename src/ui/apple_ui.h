/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lang.h"
#include "product_metadata.h"
#include "tabler_icons_atlas.h"

#define X4_RGBA(r, g, b, a) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define X4_RGB(r, g, b) X4_RGBA((r), (g), (b), 0xffu)

#define X4_ICON_NONE ((enum X4TablerIconId)-1)

enum X4PadGlyph {
    X4_GLYPH_CROSS = 0,
    X4_GLYPH_CIRCLE = 1,
    X4_GLYPH_TRIANGLE = 2,
    X4_GLYPH_SQUARE = 3,
    X4_GLYPH_OPTIONS = 4,
    X4_GLYPH_L1 = 5,
    X4_GLYPH_R1 = 6,
    X4_GLYPH_L2 = 7,
    X4_GLYPH_R2 = 8,

    X4_PAD_CROSS = X4_GLYPH_CROSS,
    X4_PAD_CIRCLE = X4_GLYPH_CIRCLE,
    X4_PAD_TRIANGLE = X4_GLYPH_TRIANGLE,
    X4_PAD_SQUARE = X4_GLYPH_SQUARE,
    X4_PAD_OPTIONS = X4_GLYPH_OPTIONS,
    X4_PAD_L1 = X4_GLYPH_L1,
    X4_PAD_R1 = X4_GLYPH_R1,
    X4_PAD_L2 = X4_GLYPH_L2,
    X4_PAD_R2 = X4_GLYPH_R2
};

enum X4ImageGradientMode {
    X4_GRAD_NONE = 0,
    X4_GRAD_CARD_BOTTOM = 1,
    X4_GRAD_BOTTOM_DARK = X4_GRAD_CARD_BOTTOM,
    X4_GRAD_HERO_LEFT_BOTTOM = 2
};

/* Precomputed midnight-blue original background + pre-blurred glass backing. */
void x4_apple_bg_draw(uint32_t *pixels);
/* UI-owner-only animation clock; no renderer I/O, sleeps or threads. */
void x4_apple_set_animation_phase(unsigned phase);
unsigned x4_apple_get_animation_phase(void);
void x4_apple_spinner(uint32_t *pixels, int x, int y, unsigned phase);
int x4_apple_text_wrap(uint32_t *pixels, int x, int y, int size, int weight,
    const char *text, int width, int line_height, int max_lines, uint32_t color);

/* Core 2D antialiased primitives. */
void x4_apple_blend_pixel(uint32_t *dst, uint32_t src_rgba);
void x4_apple_fill_round_rect(uint32_t *pixels, int x, int y, int w, int h,
                              int radius, uint32_t rgba);
void x4_apple_stroke_round_rect(uint32_t *pixels, int x, int y, int w, int h,
                                int radius, float border_px, uint32_t rgba);
void x4_apple_outer_glow(uint32_t *pixels, int x, int y, int w, int h,
                         int radius, int spread_px, uint32_t rgba);
void x4_apple_vertical_gradient_round_rect(uint32_t *pixels, int x, int y, int w, int h,
                                           int radius, uint32_t top_rgba, uint32_t bot_rgba);

/* High-level Apple dark glass panels, focus ring, and pill buttons. */
void x4_apple_glass_panel(uint32_t *pixels, int x, int y, int w, int h,
                          int radius, unsigned fill_alpha, unsigned border_alpha);
void x4_apple_focus_ring(uint32_t *pixels, int x, int y, int w, int h, int radius);
void x4_apple_pill_button(uint32_t *pixels, int x, int y, int w, int h,
                          const char *label, bool primary, bool focused,
                          enum X4TablerIconId icon);

/* Bilinearly scaled image with rounded-rect clipping and darkening vignette. */
void x4_apple_blit_image_rounded(uint32_t *pixels, int dst_x, int dst_y, int dst_w, int dst_h,
                                 int radius, const X4CoverRaster *raster,
                                 enum X4ImageGradientMode grad_mode);
/* Home cards only: up to 10% crop; covers/detail retain the 5% entry point. */
void x4_apple_blit_card_rounded(uint32_t *pixels, int x, int y, int w, int h,
    int radius, const X4CoverRaster *raster, enum X4ImageGradientMode gradient);
void x4_apple_draw_hero_banner(uint32_t *pixels, int x, int y, int w, int h, int radius,
                               const X4CoverRaster *raster);

/* Original procedural card/hero artwork (no copyrighted game art). */
void x4_apple_procedural_art(uint32_t *pixels, int dst_x, int dst_y, int dst_w, int dst_h,
                             int radius, unsigned variant, enum X4ImageGradientMode grad_mode);

/* Pre-rasterized Tabler Icons and drawn DualShock 4 controller glyphs. */
void x4_apple_draw_icon(uint32_t *pixels, int x, int y,
                        enum X4TablerIconId icon_id, uint32_t color);
void x4_apple_draw_user_large(uint32_t *pixels, int x, int y, uint32_t color);
int x4_apple_draw_pad_glyph(uint32_t *pixels, int x, int y,
                            enum X4PadGlyph glyph);
int x4_apple_draw_footer_hint(uint32_t *pixels, int x, int y,
                              enum X4PadGlyph glyph, const char *label);

/* Top-left "XCloud4/" brand mark with green slash and top-right account/gear pills. */
void x4_apple_draw_brand_logo(uint32_t *pixels, int x, int y);
void x4_apple_draw_account_pill(uint32_t *pixels, int x, int y, int h,
                                const char *label, bool signed_in, bool focused);

/* Region unavailability QR block (TAREA 19 Section E). */
const char *x4_region_qr_url(enum X4Lang lang);
const char *x4_region_qr_short_label(enum X4Lang lang);
const char *x4_region_qr_caption(enum X4Lang lang);
void x4_apple_draw_region_qr_block(uint32_t *pixels, int x, int y, int w, int h,
                                   enum X4Lang lang);

/* Optional access-country card and OPTIONS content. Renderer-only: no I/O. */
#include "access_country.h"
void x4_apple_draw_access_controls(uint32_t *pixels, int x, int y, int w, int h,
    const X4AccessCountryUi *ui, X4AccessCountryCardPhase phase, bool settings);
void x4_apple_draw_access_card(uint32_t *pixels, int x, int y, int w, int h);
void x4_apple_draw_access_success(uint32_t *pixels, int x, int y, int w);

typedef struct {
    bool open, shown;
    char version[64], url[256];
} X4UpdateNotice;
/* Called by the Home UI owner only after an AVAILABLE result. Eligibility
 * excludes other pages/modals; offers made while ineligible are not consumed.
 * Test notices do not consume the once-per-startup real notice. */
bool x4_apple_update_notice_offer(X4UpdateNotice *notice,const char *version,
    const char *validated_tag,bool eligible,bool is_test);
/* Returns true while the modal owns input. Only Circle closes it. */
bool x4_apple_update_notice_handle(X4UpdateNotice *notice,bool circle);
void x4_apple_update_notice_draw(const X4UpdateNotice *notice,uint32_t *pixels);
void x4_apple_draw_update_test_action(uint32_t *pixels,int x,int y);
