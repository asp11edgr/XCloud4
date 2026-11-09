/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_info.h"
#include "../core/app_identity.h"
#include "canvas.h"
#include "screen.h"
#include "ui2_modal.h"

#include <stdio.h>

enum {
    X = X4_UI2_CONTENT_X, R = X4_UI_SAFE_RIGHT, W = R - X, Y = 151,
    ABOUT_META_W = 610, ABOUT_HELP_X = X + ABOUT_META_W + 26,
};

static void info_text(uint32_t *p, const char *role, int x, int y, int width,
    int size, int weight, const char *text, int lines, uint32_t color)
{
#ifdef X4_UI46_CAPTURE
    x4_ui46_capture_contrast(p, role, x, y, width, size, color);
#else
    (void)role;
#endif
    (void)x4_ui2_text_bounded(p, x, y, size, weight, text, width, size + 8, lines, color);
}

static void begin_page(uint32_t *p, enum X4TextId title)
{
    x4_rect(p, 0, 0, X4_UI_WIDTH, X4_UI_HEIGHT, X4_UI2_BACKGROUND);
    x4_ui2_heading(p, X, 56, x4_tr(title));
}

static void end_page(uint32_t *p)
{
    const X4Ui2Hint hints[] = {
        {X4_PAD_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)},
    };
    x4_ui2_footer(p, hints, sizeof(hints) / sizeof(*hints));
}

static void about_metadata(uint32_t *p)
{
    const enum X4TextId labels[] = {X4_TR_U49_ABOUT_VERSION, X4_TR_U49_ABOUT_DEVELOPER,
        X4_TR_U49_ABOUT_LICENSE, X4_TR_U49_ABOUT_SOURCE};
    const char *values[] = {X4_PRODUCT_VERSION, "ASAP_EDGR", "GPL-3.0", "github.com/asp11edgr/XCloud4"};
    x4_ui2_card(p, X, 342, ABOUT_META_W, 358, X4_UI2_RADIUS_CARD);
    for (unsigned i = 0; i < sizeof(labels) / sizeof(*labels); ++i) {
        const int y = 364 + (int)i * 82;
        info_text(p, "about metadata label", X + 27, y, ABOUT_META_W - 54,
            X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(labels[i]), 1, X4_UI2_MUTED);
        info_text(p, "about metadata value", X + 27, y + 28, ABOUT_META_W - 54,
            X4_UI2_BODY, X4_FONT_SEMIBOLD, values[i], 1, X4_UI2_TEXT);
        if (i < 3) x4_rect(p, X + 27, y + 70, ABOUT_META_W - 54, 1, X4_UI2_BORDER);
    }
}

static void about_help(uint32_t *p)
{
    const int x = ABOUT_HELP_X + 27, w = R - x - 27;
    x4_ui2_card(p, ABOUT_HELP_X, 342, R - ABOUT_HELP_X, 537, X4_UI2_RADIUS_CARD);
    info_text(p, "about requirements heading", x, 367, w,
        X4_UI2_LABEL, X4_FONT_BOLD, x4_tr(X4_TR_U49_ABOUT_REQUIREMENTS), 1, X4_UI2_TEXT);
    info_text(p, "about requirements", x, 410, w,
        X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_ABOUT_LINE2), 3, X4_UI2_MUTED);
    x4_rect(p, x, 509, w, 1, X4_UI2_BORDER);
    info_text(p, "about credits heading", x, 527, w,
        X4_UI2_LABEL, X4_FONT_BOLD, x4_tr(X4_TR_U49_ABOUT_CREDITS), 1, X4_UI2_TEXT);
    info_text(p, "about credits", x, 569, w,
        X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_ABOUT_LINE6), 3, X4_UI2_MUTED);
    x4_rect(p, x, 669, w, 1, X4_UI2_BORDER);
    info_text(p, "about controls heading", x, 687, w,
        X4_UI2_LABEL, X4_FONT_BOLD, x4_tr(X4_TR_U49_ABOUT_HELP), 1, X4_UI2_TEXT);
    info_text(p, "about touchpad controls", x, 732, w,
        X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_ABOUT_LINE7), 2, X4_UI2_MUTED);
    info_text(p, "about exit controls", x, 803, w,
        X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_ABOUT_LINE8), 2, X4_UI2_MUTED);
}

static void about_update(uint32_t *p, enum X4UpdateStatus status, const char *version)
{
    bool retry_local = false;
#ifdef X4_UPDATE51_ENABLED
    const bool retry = x4_update51_retry_available(&retry_local);
    const bool pending = retry || x4_update51_pending();
#else
    const bool retry = false, pending = false;
#endif
    if (status == X4_UPDATE_AVAILABLE && version && x4_apple_update_is_current(version))
        status = X4_UPDATE_LATEST;
    char text[192];
    const char *label;
    switch (status) {
    case X4_UPDATE_CHECKING: label = x4_tr(X4_TR_U49_ABOUT_CHECKING); break;
    case X4_UPDATE_LATEST: label = x4_tr(X4_TR_U49_ABOUT_LATEST); break;
    case X4_UPDATE_AVAILABLE:
        snprintf(text, sizeof(text), x4_tr(X4_TR_U49_ABOUT_UPDATE_FMT), version ? version : "");
        label = text;
        break;
    default: label = x4_tr(X4_TR_U49_ABOUT_FAILED); break;
    }
    if (retry && status != X4_UPDATE_CHECKING) label = x4_tr(X4_TR_51E_UPDATE_RETRY_STATE);
    x4_ui2_card(p, X, 725, ABOUT_META_W, pending ? 193 : 154, X4_UI2_RADIUS_CARD);
    x4_apple_draw_icon(p, X + 26, 754, X4_ICON_INFO, X4_UI2_ACCENT);
    info_text(p, "about update heading", X + 65, 752, ABOUT_META_W - 92,
        X4_UI2_LABEL, X4_FONT_BOLD, x4_tr(X4_TR_U49_ABOUT_UPDATE), 1, X4_UI2_TEXT);
    info_text(p, "about update state", X + 27, 793, ABOUT_META_W - 54,
        X4_UI2_CAPTION, X4_FONT_REGULAR, label, 2, X4_UI2_MUTED);
    if (retry && retry_local)
        info_text(p, "about local update retry", X + 27, 846, ABOUT_META_W - 54,
            X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_51E_UPDATE_RETRY_LAB_HINT), 1, X4_UI2_MUTED);
    else if (status != X4_UPDATE_CHECKING)
        x4_apple_draw_footer_hint(p, X + 27, 846, X4_PAD_CROSS,
            x4_tr(retry ? X4_TR_50_SCREEN_RETRY : X4_TR_51_UPDATE_CHECK));
    if (pending)
        x4_apple_draw_footer_hint(p, X + 27, 883, X4_PAD_SQUARE, x4_tr(X4_TR_51B_CANCEL_PENDING));
}

void x4_ui2_about_draw(uint32_t *p, const X4CoverRaster *app_icon,
    enum X4UpdateStatus status, const char *version)
{
    if (!p) return;
    begin_page(p, X4_TR_U49_ABOUT_TITLE);
    x4_ui2_card(p, X, Y, W, 166, X4_UI2_RADIUS_CARD);
    int text_x = X + 28;
    if (app_icon && app_icon->rgba) {
        x4_apple_blit_image_rounded(p, X + 28, Y + 29, 108, 108, 18, app_icon, X4_GRAD_NONE);
        text_x += 132;
    }
    x4_ui2_brand(p, text_x, Y + 25, X4_UI2_HEADING);
#if !X4_PUBLIC && defined(X4_UPDATE_TEST_MARKER)
    info_text(p, "installation test marker", R - 180, Y + 29, 148,
        X4_UI2_BODY, X4_FONT_SEMIBOLD, X4_UPDATE_TEST_MARKER, 1, X4_UI2_ACCENT);
#endif
    info_text(p, "about product summary", text_x, Y + 73, R - text_x - 24,
        X4_UI2_BODY, X4_FONT_SEMIBOLD, x4_tr(X4_TR_U49_ABOUT_DESCRIPTION), 1, X4_UI2_TEXT);
    info_text(p, "about independent project", text_x, Y + 111, R - text_x - 24,
        X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_ABOUT_LINE3), 2, X4_UI2_MUTED);
    about_metadata(p);
    about_help(p);
    about_update(p, status, version);
    end_page(p);
}

void x4_ui2_support_draw(uint32_t *p)
{
    if (!p) return;
    begin_page(p, X4_TR_U49_SUPPORT_TITLE);
    const int left = X + 48;
    x4_ui2_card(p, X, Y, W, 727, X4_UI2_RADIUS_CARD);
    x4_apple_fill_round_rect(p, left, 225, 64, 64, 24, X4_UI2_ACTIVE);
    x4_apple_draw_icon(p, left + 20, 245, X4_ICON_HEART, X4_UI2_ACCENT);
    info_text(p, "support headline", left, 324, 800, X4_UI2_TITLE,
        X4_FONT_BOLD, x4_tr(X4_TR_U49_SUPPORT_HEADLINE), 2, X4_UI2_TEXT);
    info_text(p, "support voluntary text", left, 472, 815, X4_UI2_LABEL,
        X4_FONT_REGULAR, x4_tr(X4_TR_U49_SUPPORT_VOLUNTARY), 3, X4_UI2_MUTED);
    const char *url = x4_support_qr_url();
    const char *site = url + sizeof("https://") - 1;
    x4_ui2_chip(p, left, 592, 360, 46, site, true);
    x4_rect(p, 1236, 224, 1, 465, X4_UI2_BORDER);
    (void)x4_apple_draw_public_qr(p, 1518, 431, 346, url);
    const int caption_w = x4_text_width_px(X4_UI2_CAPTION, X4_FONT_REGULAR, x4_tr(X4_TR_U49_SUPPORT_SCAN));
    const int caption_x = 1518 - (caption_w < 460 ? caption_w : 460) / 2;
    info_text(p, "support scan help", caption_x, 635, 460, X4_UI2_CAPTION,
        X4_FONT_REGULAR, x4_tr(X4_TR_U49_SUPPORT_SCAN), 2, X4_UI2_MUTED);
    x4_rect(p, left, 741, W - 96, 1, X4_UI2_BORDER);
    x4_apple_draw_icon(p, left, 781, X4_ICON_INFO, X4_UI2_ACCENT);
    info_text(p, "support donations notice", left + 40, 779, W - 136,
        X4_UI2_BODY, X4_FONT_REGULAR, x4_tr(X4_TR_U49_SUPPORT_NOTICE), 2, X4_UI2_MUTED);
    end_page(p);
}
