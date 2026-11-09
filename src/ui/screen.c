/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include "cloud_time_ui.h"
#include "apple_ui.h"
#include "canvas.h"
#include "font.h"
#include "lang.h"
#include "net_hud.h"
#include "net_hint.h"
#include "server_settings.h"
#include "picture_settings.h"
#include "../auth/auth_profile.h"
#include "../core/app_identity.h"
#include "stream_video_status.h"
#include "../../third_party/qrcodegen/qrcodegen.h"
#include "../auth/release_check.h"
#ifdef X4_UPDATE51_ENABLED
#include "../update/update_job51.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <orbis/libkernel.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

static X4HomeDrawContext s_home_ctx;

size_t x4_screen_cover_demand(int page, const char *featured_id,
    const char *ids[2], uint32_t masks[2])
{
    if (page != 0 && page != 8) return 0;
    ids[0] = X4_FORTNITE_PRODUCT_ID;
    if (page == 8) {
        masks[0] = 1u << X4_COVER_FORTNITE_POSTER;
        return 1;
    }

    masks[0] = 1u << X4_COVER_HOME_FORTNITE;
    if (!x4_product_id_valid(featured_id) || !strcmp(featured_id, X4_FORTNITE_PRODUCT_ID)) {
        masks[0] |= 1u << X4_COVER_HOME_FEATURE;
        return 1;
    }
    ids[1] = featured_id;
    masks[1] = 1u << X4_COVER_HOME_FEATURE;
    return 2;
}

void x4_screen_set_home_context(const X4HomeDrawContext *ctx)
{
    if (ctx) {
        s_home_ctx = *ctx;
    } else {
        memset(&s_home_ctx, 0, sizeof(s_home_ctx));
    }
}

static bool auth_qr_code_valid(const char *code)
{
    if (!code || !code[0]) return false;
    for (const char *p = code; *p; ++p) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static bool auth_qr_uri_valid(const char *uri)
{
    if (!uri) return false;
    static const char *const allowed[] = {
        "https://microsoft.com/devicelogin",
        "https://www.microsoft.com/link",
        "https://www.microsoft.com/devicelogin",
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (!strcmp(uri, allowed[i])) return true;
    }
    return false;
}

bool x4_auth_qr_payload(const char *uri, const char *code, char *payload_out, size_t cap)
{
    if (!auth_qr_uri_valid(uri) || !auth_qr_code_valid(code)) return false;
    if (payload_out && cap) {
        snprintf(payload_out, cap, "%s?otc=%s", uri, code);
    }
    return true;
}

static const uint32_t BG = 0xff1b1510, PANEL = 0xff30261c;
static const uint32_t WHITE = 0xfff5f1eb, MUTED = 0xffb6a899, GREEN = 0xff72d86e;

static void update_home_menu(X4Screen *s, uint32_t pressed)
{
    int sel = s->selected;
    if (sel < 0 || sel > 7) sel = 0;
    if (sel == 3) sel = 4;

    if (sel <= 2) {
        /* Middle row: Explore, Fortnite, Continue or Account. */
        if (pressed & ORBIS_PAD_BUTTON_LEFT) sel = (sel + 2) % 3;
        if (pressed & ORBIS_PAD_BUTTON_RIGHT) sel = (sel + 1) % 3;
        if (pressed & ORBIS_PAD_BUTTON_DOWN) sel = sel == 0 ? 4 : 5;
        if (pressed & ORBIS_PAD_BUTTON_UP) sel = sel == 2 ? 7 : 6;
    } else if (sel <= 5) {
        /* Bottom row: About and Support. */
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT))
            sel = sel == 4 ? 5 : 4;
        if (pressed & ORBIS_PAD_BUTTON_UP) sel = sel == 4 ? 0 : 1;
    } else {
        /* Top bar: 6=Your account pill, 7=Gear icon */
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT))
            sel = (sel == 6) ? 7 : 6;
        if (pressed & ORBIS_PAD_BUTTON_DOWN)
            sel = (sel == 7) ? 2 : 1;
    }
    s->selected = sel;

    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
        switch (sel) {
        case 0: /* Explorar catalogo */
            s->explore_catalog_requested = 1;
            if (s->signed_in) {
                s->catalog_from_home = 1;
                s->page = 5;
            } else {
                s->auto_catalog_after_login = 1;
                s->page = 4;
            }
            break;
        case 1: /* Fortnite */
            s->page = 8;
            break;
        case 2: /* Continue or the signed-out Account card. */
            if (s->signed_in) {
                s->open_recent_requested = 1;
                break;
            }
            /* fall through */
        case 6: /* Tu cuenta top pill */
            s->auto_catalog_after_login = 0;
            s->page = 4;
            break;
        case 7: /* Gear icon */
            s->open_settings_requested = 1;
            break;
        case 4: /* Acerca de */
            s->page = 2;
            break;
        case 5: /* Apoyar */
            s->page = 7;
            break;
        default:
            break;
        }
    }
}

void x4_screen_update(X4Screen *s, uint32_t pressed, uint32_t held)
{
    /* Main waits for the session worker to close the remote session. */
    if (s->page == 6) return;
#ifdef X4_UPDATE51_ENABLED
    if (s->page == 2 && pressed == ORBIS_PAD_BUTTON_SQUARE) {
        (void)x4_update51_cancel_pending();
        return;
    }
#endif
#if X4_PUBLIC
    (void)held;
    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
        if (s->page == 5) {
            if (s->catalog_from_home) {
                s->catalog_from_home = 0;
                s->page = 0;
            } else {
                s->page = 4;
            }
        } else {
            s->auto_catalog_after_login = 0;
            s->page = 0;
        }
        return;
    }
    if (s->page == 0) {
        update_home_menu(s, pressed);
    } else if (s->page == 8) {
        if (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_LEFT)) {
            if (s->selected > 0) s->selected--;
        }
        if (pressed & (ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_RIGHT)) {
            if (s->selected + 1 < (int)X4_PIC_QUALITY_COUNT) s->selected++;
        }
    }
#else
    if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && (s->page != 1 || (held & ORBIS_PAD_BUTTON_L1))) {
        if (s->page == 5) {
            if (s->catalog_from_home) {
                s->catalog_from_home = 0;
                s->page = 0;
            } else {
                s->page = 4;
            }
        } else {
            s->auto_catalog_after_login = 0;
            s->page = 0;
        }
        return;
    }
    if (s->page == 0) {
        update_home_menu(s, pressed);
    } else if (s->page == 8) {
        if (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_LEFT)) {
            if (s->selected > 0) s->selected--;
        }
        if (pressed & (ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_RIGHT)) {
            if (s->selected < 2) s->selected++;
        }
    }
#endif
}

static void header(uint32_t *p, const X4Controller *c)
{
    x4_rect(p, 0, 0, X4_WIDTH, X4_HEIGHT, BG);
    x4_rect(p, 84, 88, 12, 78, GREEN);
    x4_text(p, 122, 92, 9, "XCloud4", WHITE);
    x4_text(p, 1420, 110, 3, X4_PRODUCT_VERSION, MUTED);
    x4_rect(p, 84, 205, 1752, 2, PANEL);
    x4_text(p, 84, 963, 3, c->data.connected ? x4_tr(X4_TR_DS4_CONNECTED) : x4_tr(X4_TR_DS4_CONNECT), c->data.connected ? GREEN : MUTED);
    if (c->error) {
        char error[50];
        snprintf(error, sizeof(error), x4_tr(X4_TR_CONTROLLER_ERR_FMT), (unsigned)c->error);
        x4_text(p, 1110, 963, 3, error, MUTED);
    }
}

enum { HOME_CARDS = 3, HOME_ELEMENTS = 8, HOME_FOCUS_HALO = 14 };
#define HOME_ELEMENT(i) (1u << (unsigned)(i))
#define HOME_CARD_BITS (HOME_ELEMENT(HOME_CARDS) - 1u)
#define HOME_STATIC HOME_ELEMENT(HOME_ELEMENTS)
#define HOME_ALL (HOME_STATIC * 2u - 1u)

typedef struct {
    enum X4Lang lang;
    bool signed_in, server_hint;
    char gamertag[32], update[32], continue_name[128];
    X4CoverRaster featured, fortnite, continue_cover;
    uint64_t asset_revision;
} HomeMenuKey;

static struct {
    uint32_t *canvas;
    HomeMenuKey key;
    int selected;
    size_t count;
    X4UiRect rects[2];
} s_menu = {.count = SIZE_MAX};
static uint64_t s_menu_asset_revision;
/* Cache the unfocused card and its neutral halo as one patch. The UI owns
 * these pixels, independently of the lifetime of the borrowed rasters. */
static struct {
    uint32_t *pixels;
    bool attempted, valid;
} s_home_cards[HOME_CARDS];

static X4UiRect home_element(int index, X4UiRect account)
{
    static const X4UiRect rects[HOME_ELEMENTS] = {
        {84,304,852,448}, {966,304,420,448}, {1416,304,420,448},
        {0,0,0,0}, {84,790,252,58}, {356,790,232,58},
        {0,0,0,0}, {1784,48,52,52}
    };
    return index == 6 ? account : rects[index];
}

static X4UiRect home_account_rect(const char *label)
{
    int width = x4_text_width_px(22, X4_FONT_SEMIBOLD, label) + 74;
    if (width < 190) width = 190;
    if (width > 340) width = 340;
    return (X4UiRect){1784 - 16 - width, 48, width, 52};
}

static void home_card_art(uint32_t *p, X4UiRect r, int radius,
    const X4CoverRaster *art, unsigned variant)
{
    if (x4_apple_blit_image_exact(p, r.x, r.y, r.w, r.h, radius, X4_WIDTH, art)) return;
    if (art->rgba)
        x4_apple_blit_card_rounded(p, r.x, r.y, r.w, r.h, radius, art, X4_GRAD_BOTTOM_DARK);
    else
        x4_apple_procedural_art(p, r.x, r.y, r.w, r.h, radius, variant, X4_GRAD_BOTTOM_DARK);
}

static X4UiRect home_element_damage(X4UiRect r)
{
    return (X4UiRect){r.x - HOME_FOCUS_HALO, r.y - HOME_FOCUS_HALO,
        r.w + 2 * HOME_FOCUS_HALO, r.h + 2 * HOME_FOCUS_HALO};
}

void x4_screen_home_invalidate(void)
{
    s_menu.canvas = NULL;
    s_menu.count = SIZE_MAX;
}

void x4_screen_home_asset_revision(uint64_t revision)
{
    s_menu_asset_revision = revision;
}

size_t x4_screen_home_damage(X4UiRect rects[2])
{
    if (s_menu.count != SIZE_MAX && rects)
        memcpy(rects, s_menu.rects, s_menu.count * sizeof(*rects));
    return s_menu.count;
}

static bool home_art_equal(X4CoverRaster a, X4CoverRaster b)
{
    return a.rgba == b.rgba && a.width == b.width && a.height == b.height;
}

static bool home_key_equal(const HomeMenuKey *a, const HomeMenuKey *b)
{
    return a->lang == b->lang && a->signed_in == b->signed_in &&
        a->server_hint == b->server_hint && !strcmp(a->gamertag, b->gamertag) &&
        !strcmp(a->update, b->update) && !strcmp(a->continue_name, b->continue_name) &&
        home_art_equal(a->featured, b->featured) && home_art_equal(a->fortnite, b->fortnite) &&
        home_art_equal(a->continue_cover, b->continue_cover) &&
        (!(a->featured.rgba || a->fortnite.rgba || a->continue_cover.rgba) ||
            a->asset_revision == b->asset_revision);
}

static bool home_art_changed(X4CoverRaster before, X4CoverRaster now, bool revision)
{
    return !home_art_equal(before, now) || (now.rgba && revision);
}

static void home_invalidate_changed_cards(const HomeMenuKey *key)
{
    const HomeMenuKey *prior = &s_menu.key;
    bool language = prior->lang != key->lang;
    bool revision = prior->asset_revision != key->asset_revision;
    if (language || home_art_changed(prior->featured, key->featured, revision))
        s_home_cards[0].valid = false;
    if (language || home_art_changed(prior->fortnite, key->fortnite, revision))
        s_home_cards[1].valid = false;
    if (language || prior->signed_in != key->signed_in ||
        strcmp(prior->continue_name, key->continue_name) ||
        (key->signed_in && key->continue_name[0] &&
            home_art_changed(prior->continue_cover, key->continue_cover, revision)))
        s_home_cards[2].valid = false;
}

static void home_card_caption(uint32_t *p, const char *role, int x, int y,
    int size, int weight, const char *label, int width, uint32_t color)
{
#ifdef X4_UI46_CAPTURE
    int measured = x4_text_width_px(size, weight, label);
    x4_ui46_capture_contrast(p, role, x, y, measured < width ? measured : width, size + 4, color);
#else
    (void)role;
#endif
    x4_text_ellipsis_px(p, x, y, size, weight, label, width, color);
}

static void home_draw_elements(uint32_t *p, const HomeMenuKey *key, const char *acct_pill_label,
    X4UiRect account, int selected, unsigned elements)
{
    bool signed_in = key->signed_in;
    if (elements & HOME_STATIC) {
        /* Top bar: brand logo on left, account pill + gear button on right */
        x4_apple_draw_brand_logo(p, 84, 52);

        if (key->update[0]) {
            char banner[128];
            snprintf(banner, sizeof(banner), x4_tr(X4_TR_HOME_UPDATE_AVAILABLE_FMT), key->update);
            int bw = x4_text_width_px(22, X4_FONT_SEMIBOLD, banner) + 36;
            if (bw > 640) bw = 640;
            x4_apple_glass_panel(p, 350, 50, bw, 44, 22, 22u, 48u);
            x4_text_ellipsis_px(p, 368, 59, 22, X4_FONT_SEMIBOLD, banner, bw - 36, X4_RGB(0x4A, 0xDE, 0x80));
        }
    }

    if (elements & HOME_ELEMENT(6)) {
        x4_apple_draw_account_pill(p, account.x, account.y, account.h,
            acct_pill_label, signed_in, selected == 6);
    }

    if (elements & HOME_ELEMENT(7)) {
        const int gear_x = 1784, gear_y = 48;
        x4_apple_glass_panel(p, gear_x, gear_y, 52, 52, 26,
            selected == 7 ? 38u : 22u, selected == 7 ? 96u : 42u);
        if (selected == 7) {
            x4_apple_focus_ring(p, gear_x, gear_y, 52, 52, 26);
        }
        x4_apple_draw_icon(p, gear_x + 14, gear_y + 14, X4_ICON_SETTINGS, X4_RGB(0xF5, 0xF7, 0xFA));
    }

    if (elements & HOME_STATIC) {
        /* Main heading and subtitle */
        x4_text_ellipsis_px(p, 84, 146, 64, X4_FONT_BOLD,
            x4_tr(X4_TR_APPLE_HOME_TITLE), 1160, X4_RGB(0xF5, 0xF7, 0xFA));
        x4_text_ellipsis_px(p, 84, 228, 28, X4_FONT_REGULAR,
            x4_tr(X4_TR_APPLE_HOME_SUBTITLE), 820, X4_RGB(0xA3, 0xB1, 0xC6));

        if (key->server_hint) {
            const char *srv_hint = x4_tr(X4_TR_APPLE_SERVER_FIRST_HINT);
            int hw = x4_text_width_px(22, X4_FONT_REGULAR, srv_hint);
            int hx = 1836 - hw;
            if (hx < 920) hx = 920;
            x4_text_ellipsis_px(p, hx, 234, 22, X4_FONT_REGULAR, srv_hint, 1836 - hx, X4_RGB(0x8F, 0xA2, 0xB8));
        }
    }

    if (elements & HOME_ELEMENT(0)) {
        /* Card 1: Explorar catalogo (large panoramic card) */
        X4UiRect rect = home_element(0, account);
        const int c1_x = rect.x, c1_y = rect.y, c1_w = rect.w, c1_h = rect.h, c1_r = 24;
        home_card_art(p, rect, 24, &key->featured, 0);
        x4_apple_vertical_gradient_round_rect(p, c1_x, c1_y + 92, c1_w, c1_h - 92, c1_r,
            X4_RGBA(0, 0, 0, 0), X4_RGBA(0, 0, 0, 235));
        x4_apple_stroke_round_rect(p, c1_x, c1_y, c1_w, c1_h, c1_r, 1.5f, X4_RGBA(0xFF, 0xFF, 0xFF, 0x2E));
        home_card_caption(p, "explore-title", c1_x + 40, c1_y + 236, 40, X4_FONT_BOLD,
            x4_tr(X4_TR_APPLE_EXPLORE_TITLE), c1_w - 80, X4_RGB(0xF5, 0xF7, 0xFA));
        home_card_caption(p, "explore-subtitle", c1_x + 40, c1_y + 290, 24, X4_FONT_REGULAR,
            x4_tr(X4_TR_APPLE_EXPLORE_SUB), c1_w - 80, X4_RGB(0xC5, 0xD0, 0xE0));
        x4_apple_pill_button(p, c1_x + 40, c1_y + 348, 212, 56,
            x4_tr(X4_TR_APPLE_ENTER_BTN), true, false, X4_ICON_NONE);
    }

    if (elements & HOME_ELEMENT(1)) {
        /* Card 2: Fortnite (vertical cover card) */
        X4UiRect rect = home_element(1, account);
        const int c2_x = rect.x, c2_y = rect.y, c2_w = rect.w, c2_h = rect.h, c2_r = 24;
        home_card_art(p, rect, 24, &key->fortnite, 1);
        x4_apple_vertical_gradient_round_rect(p, c2_x, c2_y + 132, c2_w, c2_h - 132, c2_r,
            X4_RGBA(0, 0, 0, 0), X4_RGBA(0, 0, 0, 235));
        x4_apple_stroke_round_rect(p, c2_x, c2_y, c2_w, c2_h, c2_r, 1.5f, X4_RGBA(0xFF, 0xFF, 0xFF, 0x2E));

        /* "Gratis" / "Free" badge */
        const char *free_badge = x4_tr(X4_TR_APPLE_BADGE_FREE);
        int fbw = x4_text_width_px(18, X4_FONT_BOLD, free_badge) + 24;
        x4_apple_fill_round_rect(p, c2_x + c2_w - fbw - 24, c2_y + 24, fbw, 32, 16,
            X4_RGBA(4, 12, 20, 226));
        x4_text_px(p, c2_x + c2_w - fbw - 12, c2_y + 29, 18, X4_FONT_BOLD, free_badge, X4_RGB(0x4A, 0xDE, 0x80));

        home_card_caption(p, "fortnite-title", c2_x + 36, c2_y + 300, 34, X4_FONT_BOLD,
            x4_tr(X4_TR_APPLE_FORTNITE_TITLE), c2_w - 72, X4_RGB(0xF5, 0xF7, 0xFA));
        home_card_caption(p, "fortnite-subtitle", c2_x + 36, c2_y + 348, 22, X4_FONT_SEMIBOLD,
            x4_tr(X4_TR_APPLE_FORTNITE_SUB1), c2_w - 72, X4_RGB(0xC5, 0xD0, 0xE0));
        home_card_caption(p, "fortnite-caption", c2_x + 36, c2_y + 378, 20, X4_FONT_REGULAR,
            x4_tr(X4_TR_APPLE_FORTNITE_SUB2), c2_w - 72, X4_RGB(0x94, 0xA3, 0xB8));
    }

    if (elements & HOME_ELEMENT(2)) {
        X4UiRect r = home_element(2, account);
        const bool recent = signed_in && key->continue_name[0];
        const uint32_t title_color = X4_RGB(0xF5, 0xF7, 0xFA);
        const uint32_t caption_color = X4_RGB(0xC5, 0xD0, 0xE0);
        x4_apple_glass_panel(p, r.x, r.y, r.w, r.h, 24, 20u, 38u);
        if (recent) {
            const X4CoverRaster *art = &key->continue_cover;
            x4_text_ellipsis_px(p, r.x + 36, r.y + 26, 30, X4_FONT_BOLD,
                x4_tr(X4_TR_50_SCREEN_HOME_CONTINUE), r.w - 72, title_color);
            if (!x4_apple_blit_image_exact(p, r.x + 18, r.y + 75, 384, 216, 16, r.x + r.w - 18, art)) {
                if (art->rgba && art->width && art->height)
                    x4_apple_blit_image_rounded(p, r.x + 18, r.y + 75, 384, 216, 16, art, X4_GRAD_NONE);
                else
                    x4_apple_fill_round_rect(p, r.x + 18, r.y + 75, 384, 216, 16, X4_RGB(26, 38, 52));
            }
            x4_apple_vertical_gradient_round_rect(p, r.x + 18, r.y + 210, 384, 81, 16,
                X4_RGBA(0, 0, 0, 0), X4_RGBA(0, 0, 0, 125));
#ifdef X4_UI46_CAPTURE
            x4_ui46_capture_contrast(p, "continue-title", r.x + 36, r.y + 300,
                r.w - 72, 62, title_color);
#endif
            x4_apple_text_wrap(p, r.x + 36, r.y + 300, 27, X4_FONT_BOLD,
                key->continue_name, r.w - 72, 31, 2, title_color);
            x4_apple_pill_button(p, r.x + 36, r.y + 376, 220, 56,
                x4_tr(X4_TR_APPLE_HERO_PLAY_NOW), true, false, X4_ICON_NONE);
        } else {
            int av_cx = r.x + r.w / 2, av_cy = r.y + 178;
            x4_apple_fill_round_rect(p, av_cx - 62, av_cy - 62, 124, 124, 62,
                X4_RGBA(0xFF, 0xFF, 0xFF, 0x18));
            x4_apple_stroke_round_rect(p, av_cx - 62, av_cy - 62, 124, 124, 62, 1.5f,
                X4_RGBA(0xFF, 0xFF, 0xFF, 0x33));
            if (signed_in) {
                x4_apple_draw_icon(p, av_cx - 12, av_cy - 12, X4_ICON_DEVICE_GAMEPAD,
                    X4_RGB(0xD8, 0xE2, 0xF0));
                x4_text_ellipsis_px(p, r.x + 36, r.y + 324, 34, X4_FONT_BOLD,
                    x4_tr(X4_TR_APPLE_TAB_RECENTS), r.w - 72, title_color);
            } else {
                x4_apple_draw_user_large(p, av_cx - 28, av_cy - 28, X4_RGB(0xD8, 0xE2, 0xF0));
                x4_text_ellipsis_px(p, r.x + 36, r.y + 324, 34, X4_FONT_BOLD,
                    x4_tr(X4_TR_APPLE_ACCOUNT_TITLE), r.w - 72, title_color);
                x4_text_ellipsis_px(p, r.x + 36, r.y + 372, 24, X4_FONT_REGULAR,
                    x4_tr(X4_TR_APPLE_ACCOUNT_SUB), r.w - 72, caption_color);
            }
        }
    }

    static const enum X4TextId labels[] = {X4_TR_APPLE_PILL_ABOUT, X4_TR_APPLE_PILL_SUPPORT};
    static const enum X4TablerIconId icons[] = {X4_ICON_INFO, X4_ICON_HEART};
    for (int i = 4; i <= 5; ++i) {
        if (!(elements & HOME_ELEMENT(i))) continue;
        X4UiRect r = home_element(i, account);
        x4_apple_pill_button(p, r.x, r.y, r.w, r.h, x4_tr(labels[i - 4]),
            false, selected == i, icons[i - 4]);
    }

    if (elements & HOME_STATIC) {
        /* Footer */
        x4_apple_fill_round_rect(p, 84, 926, 1752, 1, 0, X4_RGBA(0xFF, 0xFF, 0xFF, 0x22));
        int fx = 84;
        fx += x4_apple_draw_footer_hint(p, fx, X4_UI_FOOTER_Y, X4_PAD_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)) + 36;
        fx += x4_apple_draw_footer_hint(p, fx, X4_UI_FOOTER_Y, X4_PAD_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)) + 36;
#if !X4_PUBLIC
        x4_text_px(p, fx, 956, 22, X4_FONT_REGULAR, "R3: Lab", X4_RGB(0x94, 0xA3, 0xB8));
#endif
        const char *ps_hint = x4_tr(X4_TR_APPLE_PS_EXIT_HINT);
        int ps_w = x4_text_width_px(22, X4_FONT_REGULAR, ps_hint);
        int ps_x = 1836 - ps_w;
        if (ps_x < fx + 24) ps_x = fx + 24;
        x4_text_ellipsis_px(p, ps_x, 956, 22, X4_FONT_REGULAR, ps_hint, 1836 - ps_x, X4_RGB(0x94, 0xA3, 0xB8));
    }

}

static void home_draw_cached(uint32_t *p, const HomeMenuKey *key, const char *label,
    X4UiRect account, int selected, unsigned elements)
{
    home_draw_elements(p, key, label, account, selected, elements & ~HOME_CARD_BITS);
    for (int i = 0; i < HOME_CARDS; ++i) {
        if (!(elements & HOME_ELEMENT(i))) continue;
        X4UiRect card = home_element(i, account);
        X4UiRect r = home_element_damage(card);
        size_t row_bytes = (size_t)r.w * sizeof(*p);
        uint32_t *canvas = p + (size_t)r.y * X4_WIDTH + r.x;
        uint32_t *cached = s_home_cards[i].pixels;
        if (s_home_cards[i].valid) {
            for (int y = 0; y < r.h; ++y)
                memcpy(canvas + (size_t)y * X4_WIDTH, cached + (size_t)y * r.w, row_bytes);
        } else {
            home_draw_elements(p, key, label, account, selected, HOME_ELEMENT(i));
            if (!s_home_cards[i].attempted) {
                s_home_cards[i].attempted = true;
                cached = malloc(row_bytes * (size_t)r.h);
                s_home_cards[i].pixels = cached;
            }
            if (cached) {
                for (int y = 0; y < r.h; ++y)
                    memcpy(cached + (size_t)y * r.w, canvas + (size_t)y * X4_WIDTH, row_bytes);
                s_home_cards[i].valid = true;
            }
        }
        if (selected == i) x4_apple_focus_ring(p, card.x, card.y, card.w, card.h, 24);
    }
}

void x4_screen_home_prepare(uint32_t *p)
{
    if (!p) return;
    x4_screen_home_invalidate();
    HomeMenuKey key = {.lang = x4_lang_current()};
    home_invalidate_changed_cards(&key);
    for (int i = 0; i < HOME_CARDS; ++i) {
        X4UiRect r = home_element_damage(home_element(i, (X4UiRect){0}));
        x4_apple_bg_restore(p, r.x, r.y, r.w, r.h);
    }
    home_draw_cached(p, &key, "", (X4UiRect){0}, -1, HOME_CARD_BITS);
    s_menu.key = key;
}

static void home(uint32_t *p, const X4Screen *s)
{
    HomeMenuKey key = {.lang = x4_lang_current(),
        .signed_in = s->signed_in || s_home_ctx.signed_in,
        .asset_revision = s_menu_asset_revision};
    key.server_hint = !key.signed_in && !x4_server_settings_has_saved_choice();
    memcpy(key.gamertag, s_home_ctx.gamertag, sizeof(key.gamertag));
    if (x4_release_check_status(key.update, sizeof(key.update)) != X4_UPDATE_AVAILABLE ||
        x4_apple_update_is_current(key.update))
        key.update[0] = 0;
    if (s_home_ctx.featured_hero) key.featured = *s_home_ctx.featured_hero;
    if (s_home_ctx.fortnite_cover) key.fortnite = *s_home_ctx.fortnite_cover;
    if (s_home_ctx.continue_cover) key.continue_cover = *s_home_ctx.continue_cover;
    if (s_home_ctx.continue_name)
        snprintf(key.continue_name, sizeof(key.continue_name), "%s", s_home_ctx.continue_name);
    const char *label = key.signed_in && key.gamertag[0] ?
        key.gamertag : x4_tr(X4_TR_APPLE_YOUR_ACCOUNT);

    if (s_menu.canvas != p || !home_key_equal(&s_menu.key, &key)) {
        home_invalidate_changed_cards(&key);
        x4_apple_bg_draw(p);
        home_draw_cached(p, &key, label, home_account_rect(label), s->selected, HOME_ALL);
        s_menu.count = SIZE_MAX;
    } else {
        s_menu.count = 0;
        if (s_menu.selected == s->selected) return;
        int changed[] = {s_menu.selected, s->selected};
        X4UiRect account = home_account_rect(label);
        unsigned elements = 0;
        /* Neither halo reaches any neighbouring element's content. Restore
         * both regions before drawing, since the halo extents can overlap. */
        for (unsigned i = 0; i < sizeof(changed) / sizeof(changed[0]); ++i) {
            int index = changed[i];
            if (index < 0 || index >= HOME_ELEMENTS || index == 3) continue;
            X4UiRect r = home_element_damage(home_element(index, account));
            /* A cached card includes its unfocused halo. Other elements
             * still need the background restored before either is drawn. */
            if (index >= HOME_CARDS || !s_home_cards[index].valid)
                x4_apple_bg_restore(p, r.x, r.y, r.w, r.h);
            s_menu.rects[s_menu.count++] = r;
            elements |= HOME_ELEMENT(index);
        }
        home_draw_cached(p, &key, label, account, s->selected, elements);
    }
    s_menu.canvas = p;
    s_menu.key = key;
    s_menu.selected = s->selected;
}

#if !X4_PUBLIC
static void button(uint32_t *p, int x, int y, const char *label, uint32_t mask, uint32_t buttons)
{
    int active = (buttons & mask) != 0;
    x4_rect(p, x, y, 230, 70, active ? GREEN : PANEL);
    x4_text(p, x + 18, y + 23, 3, label, active ? BG : WHITE);
}
static void stick_panel(uint32_t *p, int x, int y, const char *label, stick value)
{
    x4_text(p, x, y, 3, label, WHITE);
    x4_rect(p, x, y + 42, 230, 150, PANEL);
    x4_rect(p, x + 114, y + 50, 2, 134, MUTED);
    x4_rect(p, x + 8, y + 115, 214, 2, MUTED);
    x4_rect(p, x + 8 + (int)value.x * 204 / 255, y + 50 + (int)value.y * 124 / 255, 10, 10, GREEN);
    char text[32];
    snprintf(text, sizeof(text), "X %03u  Y %03u", (unsigned)value.x, (unsigned)value.y);
    x4_text(p, x, y + 215, 3, text, MUTED);
}
static void controller_page(uint32_t *p, const X4Controller *c)
{
    x4_text(p, 84, 267, 5, x4_tr(X4_TR_50_SCREEN_PAD_TEST_TITLE), WHITE);
    x4_text(p, 84, 332, 3, x4_tr(X4_TR_50_SCREEN_PAD_TEST_GUIDE), MUTED);
    const char *labels[] = {x4_tr(X4_TR_50_SCREEN_PAD_UP), x4_tr(X4_TR_50_SCREEN_PAD_RIGHT), x4_tr(X4_TR_50_SCREEN_PAD_DOWN), x4_tr(X4_TR_50_SCREEN_PAD_LEFT), x4_tr(X4_TR_50_SCREEN_PAD_TRIANGLE), x4_tr(X4_TR_50_SCREEN_PAD_CIRCLE), "X", x4_tr(X4_TR_50_SCREEN_PAD_SQUARE), "L1", "R1", "L3", "R3", "L2", "R2", "TOUCHPAD"};
    const uint32_t masks[] = {ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_RIGHT, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_LEFT,
        ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1, ORBIS_PAD_BUTTON_L3, ORBIS_PAD_BUTTON_R3,
        ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2, ORBIS_PAD_BUTTON_TOUCH_PAD};
    for (int i = 0; i < 15; ++i)
        button(p, 84 + (i % 4) * 250, 410 + (i / 4) * 91, labels[i], masks[i], c->data.buttons);
    stick_panel(p, 1160, 410, x4_tr(X4_TR_50_SCREEN_PAD_LEFT), c->data.leftStick);
    stick_panel(p, 1500, 410, x4_tr(X4_TR_50_SCREEN_PAD_RIGHT), c->data.rightStick);
    char triggers[48];
    snprintf(triggers, sizeof(triggers), "L2 %03u     R2 %03u", (unsigned)c->data.analogButtons.l2, (unsigned)c->data.analogButtons.r2);
    x4_text(p, 1160, 713, 3, triggers, WHITE);
    x4_rect(p, 1160, 758, 255, 12, PANEL);
    x4_rect(p, 1160, 758, c->data.analogButtons.l2, 12, GREEN);
    x4_rect(p, 1500, 758, 255, 12, PANEL);
    x4_rect(p, 1500, 758, c->data.analogButtons.r2, 12, GREEN);
    x4_text(p, 84, 894, 3, x4_tr(X4_TR_50_SCREEN_PAD_TEST_BACK), WHITE);
}
#endif


static const uint32_t INK = X4_RGB(245,247,250);
static const uint32_t SOFT = X4_RGB(167,181,202);

static void phase2_base(uint32_t *p, const char *crumb)
{
    x4_apple_bg_draw(p);
    x4_apple_draw_brand_logo(p,84,52);
    x4_text_ellipsis_px(p,370,70,24,X4_FONT_REGULAR,crumb,1450,SOFT);
}

static void phase2_back(uint32_t *p, const char *label)
{
    x4_apple_pill_button(p,84,X4_UI_FOOTER_Y - 15,232,58,label,false,true,X4_ICON_NONE);
    x4_apple_draw_pad_glyph(p,108,X4_UI_FOOTER_Y,X4_PAD_CIRCLE);
}

static void phase2_warning(uint32_t *p, int x, int y)
{
    /* Generic warning symbol, drawn locally; no trademark or network asset. */
    for (int row=0;row<30;++row) {
        int half=(row+2)/2;
        x4_rect(p,x+16-half,y+row,half*2+1,1,X4_RGB(252,201,112));
    }
    x4_rect(p,x+15,y+10,3,10,X4_RGB(26,30,42));
    x4_rect(p,x+15,y+23,3,3,X4_RGB(26,30,42));
}

static void phase2_qr(uint32_t *p, const uint8_t *qr, int x, int y, int target)
{
    int n=qrcodegen_getSize(qr), margin=4;
    int scale=target/(n+2*margin);
    if (scale<1) return;
    int box=(n+2*margin)*scale;
    x+=(target-box)/2; y+=(target-box)/2;
    x4_rect(p,x,y,box,box,0xffffffffu);
    for (int my=0;my<n;++my) for(int mx=0;mx<n;++mx)
        if(qrcodegen_getModule(qr,mx,my))
            x4_rect(p,x+(margin+mx)*scale,y+(margin+my)*scale,scale,scale,0xff000000u);
}

static void phase2_update_status(uint32_t *p, int x, int y)
{
    char ver[32]={0},line[128];
    enum X4UpdateStatus st=x4_release_check_status(ver,sizeof(ver));
    if (st == X4_UPDATE_AVAILABLE && x4_apple_update_is_current(ver)) st = X4_UPDATE_LATEST;
    switch(st) {
    case X4_UPDATE_CHECKING: snprintf(line,sizeof(line),"%s",x4_tr(X4_TR_U49_ABOUT_CHECKING));break;
    case X4_UPDATE_LATEST: snprintf(line,sizeof(line),"%s",x4_tr(X4_TR_U49_ABOUT_LATEST));break;
    case X4_UPDATE_AVAILABLE: snprintf(line,sizeof(line),x4_tr(X4_TR_U49_ABOUT_UPDATE_FMT),ver);break;
    default: snprintf(line,sizeof(line),"%s",x4_tr(X4_TR_U49_ABOUT_FAILED));break;
    }
    x4_text_ellipsis_px(p,x,y,22,X4_FONT_REGULAR,line,1120,SOFT);
}

static void project_page(uint32_t *p)
{
    phase2_base(p,x4_tr(X4_TR_50_SCREEN_ABOUT_CRUMB));
    x4_apple_glass_panel(p,270,150,1380,782,28,24,46);
    if(s_home_ctx.app_icon && s_home_ctx.app_icon->rgba)
        x4_apple_blit_image_rounded(p,430,196,128,128,24,s_home_ctx.app_icon,X4_GRAD_NONE);
    x4_text_px(p,606,194,60,X4_FONT_BOLD,"XCloud4",INK);
    x4_text_px(p,608,270,30,X4_FONT_SEMIBOLD,x4_tr(X4_TR_50_SCREEN_ABOUT_TAGLINE),INK);
    x4_text_px(p,430,348,24,X4_FONT_REGULAR,x4_tr(X4_TR_U49_ABOUT_DESCRIPTION),SOFT);
    x4_apple_fill_round_rect(p,430,392,1060,1,0,X4_RGBA(255,255,255,38));
    const char *labels[]={x4_tr(X4_TR_U49_ABOUT_VERSION),x4_tr(X4_TR_U49_ABOUT_DEVELOPER),x4_tr(X4_TR_U49_ABOUT_LICENSE),x4_tr(X4_TR_U49_ABOUT_SOURCE)};
    const char *values[]={X4_PRODUCT_VERSION,"ASAP_EDGR","GPL-3.0","github.com/asp11edgr/XCloud4"};
    for(unsigned i=0;i<4;++i) {
        int y=416+(int)i*36;
        x4_text_px(p,430,y,24,X4_FONT_REGULAR,labels[i],SOFT);
        x4_text_px(p,750,y,24,X4_FONT_SEMIBOLD,values[i],INK);
    }
    x4_apple_fill_round_rect(p,430,574,1060,1,0,X4_RGBA(255,255,255,38));
    x4_apple_text_wrap(p,430,594,23,X4_FONT_REGULAR,x4_tr(X4_TR_ABOUT_LINE3),1060,29,2,INK);
    x4_apple_text_wrap(p,430,660,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ABOUT_REQUIREMENTS),1060,27,2,SOFT);
    x4_apple_text_wrap(p,430,724,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ABOUT_CREDITS),1060,27,2,SOFT);
    x4_apple_text_wrap(p,430,786,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_GAME_TOUCH_LONG),1060,27,2,INK);
    x4_text_px(p,430,848,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_GAME_EXIT_LONG),SOFT);
    phase2_update_status(p,430,886);
    phase2_back(p,x4_tr(X4_TR_APPLE_HINT_BACK));
}

const char *x4_support_qr_url(void) { return "https://ko-fi.com/asapedgr"; }

static void support_page(uint32_t *p)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX],temp[qrcodegen_BUFFER_LEN_MAX];
    static bool ready=false,valid=false;
    if(!ready) {
        valid=qrcodegen_encodeText(x4_support_qr_url(),temp,qr,qrcodegen_Ecc_MEDIUM,
            qrcodegen_VERSION_MIN,qrcodegen_VERSION_MAX,qrcodegen_Mask_AUTO,false);
        ready=true;
    }
    phase2_base(p,x4_tr(X4_TR_50_SCREEN_SUPPORT_CRUMB));
    x4_apple_glass_panel(p,200,246,1520,620,28,24,46);
    x4_apple_glass_panel(p,278,300,88,88,24,24,48);
    x4_apple_draw_icon(p,310,330,X4_ICON_HEART,INK);
    x4_text_px(p,280,420,30,X4_FONT_SEMIBOLD,x4_tr(X4_TR_U49_SUPPORT_HEADLINE),SOFT);
    x4_apple_text_wrap(p,280,470,48,X4_FONT_BOLD,x4_tr(X4_TR_50_SCREEN_SUPPORT_TAGLINE),860,60,2,INK);
    x4_apple_text_wrap(p,280,600,25,X4_FONT_REGULAR,x4_tr(X4_TR_U49_SUPPORT_VOLUNTARY),860,34,2,SOFT);
    x4_apple_pill_button(p,280,694,630,70,"ko-fi.com/asapedgr",false,false,X4_ICON_HEART);
    x4_apple_fill_round_rect(p,1180,300,1,450,0,X4_RGBA(255,255,255,38));
    if(valid) phase2_qr(p,qr,1260,386,330);
    x4_apple_fill_round_rect(p,280,794,1360,1,0,X4_RGBA(255,255,255,38));
    x4_apple_draw_icon(p,280,814,X4_ICON_INFO,SOFT);
    x4_text_px(p,324,814,22,X4_FONT_REGULAR,x4_tr(X4_TR_U49_SUPPORT_NOTICE),SOFT);
    phase2_back(p,x4_tr(X4_TR_APPLE_HINT_BACK));
}

static void phase2_catalog_error(uint32_t *p, const X4CatalogSnapshot *cat)
{
    char message[256],detail[128];
    const char *msg=x4_lang_catalog_error_msg(cat->error,cat->http_status,cat->xerr,
        cat->offering,cat->stage,message,sizeof(message));
    bool country=x4_lang_is_cloud_unavailable(cat->error,cat->http_status,cat->xerr,cat->offering,cat->stage);
    if (country) {
        x4_apple_glass_panel(p,190,180,1540,760,28,24,46);
        snprintf(detail,sizeof(detail),"0x%08x · HTTP %d · Xbox %u",(unsigned)cat->error,cat->http_status,cat->xerr);
        x4_text_px(p,252,222,20,X4_FONT_REGULAR,detail,SOFT);
        x4_apple_draw_access_card(p,252,288,1408,586);
        return;
    }
    if(cat->error==X4_AUTH_E_FORTNITE_NOT_FOUND) msg=x4_tr(X4_TR_FORTNITE_UNAVAILABLE);
    x4_apple_glass_panel(p,190,250,1540,660,28,24,46);
    phase2_warning(p,252,310);
    x4_text_px(p,310,302,38,X4_FONT_BOLD,x4_tr(X4_TR_50_SCREEN_ERROR_GENERIC),INK);
    x4_apple_text_wrap(p,252,380,26,X4_FONT_REGULAR,msg?msg:x4_tr(X4_TR_50_SCREEN_ERROR_XBOX),1410,36,4,INK);
    snprintf(detail,sizeof(detail),"0x%08x · HTTP %d · Xbox %u",(unsigned)cat->error,cat->http_status,cat->xerr);
    x4_text_px(p,252,552,20,X4_FONT_REGULAR,detail,SOFT);
    {
        const char *hint=x4_net_hint(cat->error);
        x4_apple_text_wrap(p,252,604,24,X4_FONT_REGULAR,hint?hint:x4_tr_stage(cat->stage),1410,34,5,SOFT);
        x4_apple_draw_footer_hint(p,252,806,X4_PAD_CROSS,x4_tr(X4_TR_50_SCREEN_RETRY));
        x4_apple_draw_footer_hint(p,520,806,X4_PAD_OPTIONS,x4_tr(X4_TR_U49_ACCOUNT_SERVER));
    }
    phase2_back(p,x4_tr(X4_TR_APPLE_HINT_BACK));
}

void x4_fortnite_summary_draw(const X4Screen *s, const X4CatalogSnapshot *cat, int busy, uint32_t *p)
{
    phase2_base(p,x4_tr(X4_TR_50_SCREEN_FORTNITE_CRUMB));
    X4AccessCountryCardPhase access_phase;
    x4_access_country_card_get(NULL,&access_phase);
    if (busy && access_phase == X4_ACCESS_CARD_TRYING) {
        x4_apple_glass_panel(p,190,180,1540,760,28,24,46);
        x4_apple_draw_access_card(p,252,288,1408,586);
        return;
    }
    if(cat && cat->state==X4_CATALOG_ERROR) { phase2_catalog_error(p,cat);return; }
    if(busy || (cat && cat->state==X4_CATALOG_LOADING)) {
        x4_apple_glass_panel(p,270,300,1380,500,28,24,46);
        x4_apple_spinner(p,925,440,x4_apple_get_animation_phase());
        x4_text_px(p,590,546,34,X4_FONT_SEMIBOLD,x4_tr(X4_TR_50_SCREEN_FORTNITE_FINDING),INK);
        phase2_back(p,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_CANCEL));return;
    }
    if (!x4_apple_blit_image_exact(p, 84, 170, 550, 770, 28, X4_WIDTH, s_home_ctx.fortnite_poster)) {
        if (s_home_ctx.fortnite_cover && s_home_ctx.fortnite_cover->rgba)
            x4_apple_blit_image_rounded(p,84,170,550,770,28,s_home_ctx.fortnite_cover,X4_GRAD_NONE);
        else
            x4_apple_glass_panel(p,84,170,550,770,28,22,42);
    }
    x4_text_px(p,704,170,72,X4_FONT_BOLD,"Fortnite",INK);
    x4_text_px(p,708,264,30,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_FORTNITE_CHOOSE_QUALITY),SOFT);
    const char *server=x4_server_settings_active_display_name();
    const char *raw=x4_server_settings_get_selected();
    if(!raw || !raw[0] || !strcmp(raw,"auto")) raw=x4_server_settings_default_region_name();
    int ms=raw&&raw[0]?x4_server_settings_get_region_rtt(raw):0;
    char server_line[256];
    if(ms>0) snprintf(server_line,sizeof(server_line),x4_tr(X4_TR_50_SCREEN_FORTNITE_SERVER_PING),server,ms);
    else snprintf(server_line,sizeof(server_line),x4_tr(X4_TR_50_SCREEN_FORTNITE_SERVER_UNMEASURED),server);
    X4AccessCountry country = x4_access_country_get();
    if (country != X4_ACCESS_AUTO) {
        size_t used = strlen(server_line);
        snprintf(server_line + used,sizeof(server_line) - used,x4_tr(X4_TR_ACCESS_BADGE_FMT),x4_access_country_label(country));
    }
    int server_x = country == X4_ACCESS_AUTO ? 1160 : 784;
    int server_width = country == X4_ACCESS_AUTO ? 616 : 992;
    x4_apple_glass_panel(p,server_x,118,server_width,48,24,22,44);
    x4_text_ellipsis_px(p,server_x+20,130,21,X4_FONT_REGULAR,server_line,server_width-48,SOFT);
    x4_apple_glass_panel(p,1788,118,48,48,24,22,44);
    x4_apple_draw_icon(p,1800,130,X4_ICON_SETTINGS,INK);
#if X4_PUBLIC
    const char *labels[]={x4_tr(X4_TR_50_SCREEN_QUALITY_HIGH),"1080p","720p",x4_tr(X4_TR_PIC_SETTINGS_MAX_LABEL)};
    const char *tags[]={"~10-13 Mbps","~5-8 Mbps","~7 Mbps","~15 Mbps"};
    const int quality_count=X4_PIC_QUALITY_COUNT,quality_top=316,quality_step=78,quality_height=66;
#else
    const char *labels[]={x4_tr(X4_TR_50_SCREEN_QUALITY_HIGH),"1080p","720p"};
    const char *tags[]={"~10-13 Mbps","~5-8 Mbps","~7 Mbps"};
    const int quality_count=3,quality_top=344,quality_step=94,quality_height=78;
#endif
    int sel=s->selected<0||s->selected>=quality_count?0:s->selected;
    for(int i=0;i<quality_count;++i) {
        int y=quality_top+i*quality_step;
        x4_apple_glass_panel(p,708,y,1128,quality_height,20,i==sel?38:20,i==sel?96:42);
        if(i==sel)x4_apple_focus_ring(p,708,y,1128,quality_height,20);
        x4_apple_stroke_round_rect(p,736,y+24,30,30,15,2.0f,SOFT);
        if(i==sel)x4_apple_fill_round_rect(p,744,y+32,14,14,7,INK);
        x4_text_px(p,794,y+20,30,X4_FONT_SEMIBOLD,labels[i],INK);
        if(!i) {
            x4_apple_fill_round_rect(p,1048,y+24,204,32,16,X4_RGBA(255,255,255,38));
            x4_text_px(p,1064,y+29,20,X4_FONT_SEMIBOLD,x4_tr(X4_TR_50_SCREEN_FORTNITE_RECOMMENDED),INK);
        }
        int tw=x4_text_width_px(23,X4_FONT_REGULAR,tags[i]);
        x4_text_px(p,1802-tw,y+25,23,X4_FONT_REGULAR,tags[i],SOFT);
    }
    const enum X4TextId descs[]={X4_TR_PIC_SETTINGS_DESC_1080P_ALTA,X4_TR_PIC_SETTINGS_DESC_1080P,X4_TR_PIC_SETTINGS_DESC_720P
#if X4_PUBLIC
        ,X4_TR_PIC_SETTINGS_MAX_DESC
#endif
    };
    x4_apple_text_wrap(p,708,642,23,X4_FONT_REGULAR,x4_tr(descs[sel]),1128,30,3,SOFT);
    x4_apple_text_wrap(p,708,742,22,X4_FONT_REGULAR,x4_tr(X4_TR_FORTNITE_HINT),1128,29,2,SOFT);
    x4_apple_pill_button(p,708,816,320,64,x4_tr(X4_TR_APPLE_HERO_PLAY_NOW),true,false,X4_ICON_NONE);
    x4_apple_draw_footer_hint(p,1080,832,X4_PAD_OPTIONS,x4_tr(X4_TR_50_SCREEN_FORTNITE_CHANGE_SERVER));
    x4_text_px(p,708,897,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_FORTNITE_REQUIREMENTS),SOFT);
#if X4_PUBLIC
    x4_text_ellipsis_px(p,708,921,21,X4_FONT_REGULAR,sel==X4_PIC_QUALITY_1080P_MAX?
        x4_tr(X4_TR_FORTNITE_MAX_NOTE):x4_tr(X4_TR_U49_FORTNITE_NO_PASS),1128,SOFT);
#else
    x4_text_px(p,708,921,21,X4_FONT_REGULAR,x4_tr(X4_TR_U49_FORTNITE_NO_PASS),SOFT);
#endif
    int fx=708;
    fx+=x4_apple_draw_footer_hint(p,fx,X4_UI_FOOTER_Y,X4_PAD_CROSS,x4_tr(X4_TR_APPLE_HERO_PLAY_NOW))+36;
    fx+=x4_apple_draw_footer_hint(p,fx,X4_UI_FOOTER_Y,X4_PAD_CIRCLE,x4_tr(X4_TR_APPLE_HINT_BACK))+36;
    x4_apple_draw_footer_hint(p,fx,X4_UI_FOOTER_Y,X4_PAD_OPTIONS,x4_tr(X4_TR_U49_ACCOUNT_SERVER));
    x4_apple_draw_access_success(p,84,956,550);
}

void x4_screen_draw(const X4Screen *s, const X4Controller *c, uint32_t *p)
{
    if(s->page==0) {home(p,s);return;}
    if(s->page==2) {project_page(p);return;}
    if(s->page==7) {support_page(p);return;}
    /* These pages paint their own complete frame after the main snapshot. */
    if(s->page==4 || s->page==6 || s->page==8)return;
    header(p,c);
#if X4_PUBLIC
    if(s->page==3)support_page(p);
#else
    if(s->page==1)controller_page(p,c);
#endif
}

void x4_media_draw(const X4DemoVideo *v, const X4DemoAudio *a, uint32_t *p)
{
    x4_text(p, 84, 260, 5, x4_tr(X4_TR_50_SCREEN_MEDIA_TEST_TITLE), WHITE);
    x4_rect(p, 82, 328, 964, 556, PANEL);
    if (v->pixels && v->frames) {
        for (unsigned y = 0; y < 552; ++y)
            for (unsigned x = 0; x < 960; ++x)
                p[(330 + y) * X4_WIDTH + 84 + x] = v->pixels[(y * 2 / 3) * X4_SAMPLE_WIDTH + x * 2 / 3];
    } else {
        x4_text(p, 126, 529, 3, v->error ? x4_tr(X4_TR_50_SCREEN_MEDIA_NO_VIDEO) : x4_tr(X4_TR_50_SCREEN_MEDIA_LOCAL_START), MUTED);
    }
    x4_text(p, 1110, 355, 4, "VIDEO H264", v->error ? MUTED : GREEN);
    x4_text(p, 1110, 417, 2, v->stage[0] ? v->stage : x4_tr(X4_TR_50_SCREEN_MEDIA_START), WHITE);
    char line[64];
    if (v->error) snprintf(line, sizeof(line), "ERROR 0X%08X", (unsigned)v->error);
    else snprintf(line, sizeof(line), x4_tr(X4_TR_50_SCREEN_MEDIA_FRAMES), v->frames, v->total);
    x4_text(p, 1110, 460, 3, line, MUTED);
    x4_text(p, 1110, 536, 4, x4_tr(X4_TR_50_SCREEN_MEDIA_STEREO), atomic_load(&a->error) ? MUTED : GREEN);
    int error = atomic_load(&a->error);
    if (error) snprintf(line, sizeof(line), "ERROR 0X%08X", (unsigned)error);
    else snprintf(line, sizeof(line), "%s", atomic_load(&a->muted) ? x4_tr(X4_TR_50_SCREEN_MEDIA_MUTED) : atomic_load(&a->finished) ? x4_tr(X4_TR_50_SCREEN_MEDIA_TONES_END) : x4_tr(X4_TR_50_SCREEN_MEDIA_TONES_STEREO));
    x4_text(p, 1110, 598, 2, line, WHITE);
    x4_text(p, 1110, 710, 2, x4_tr(X4_TR_50_SCREEN_MEDIA_SAMPLE), MUTED);
    x4_text(p, 84, 915, 3, x4_tr(X4_TR_50_SCREEN_MEDIA_TEST_HINT), WHITE);
}

void x4_exit_error_draw(int error, uint32_t *p)
{
    if (!error) return;
    char message[96];
#if X4_PUBLIC
    snprintf(message, sizeof(message), x4_tr(X4_TR_EXIT_ERR_PUBLIC_FMT), (unsigned)error);
#else
    snprintf(message, sizeof(message), x4_tr(X4_TR_EXIT_ERR_LAB_FMT), (unsigned)error);
#endif
    x4_rect(p, 84, 999, 1752, 54, PANEL);
    x4_text(p, 100, 1015, 3, message, WHITE);
}


void x4_auth_draw(const X4AuthSnapshot *a, int busy, int closing, uint32_t *p)
{
    phase2_base(p,x4_tr(X4_TR_50_SCREEN_ACCOUNT_CRUMB));
    const bool authorized=a->state==X4_AUTH_AUTHORIZED;
    x4_text_px(p,126,172,60,X4_FONT_BOLD,authorized?x4_tr(X4_TR_U49_ACCOUNT_TITLE):x4_tr(X4_TR_U49_HOME_SIGN_IN_TITLE),INK);
    x4_text_px(p,130,254,30,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ACCOUNT_CONNECT),SOFT);
    x4_apple_glass_panel(p,270,340,1380,470,28,24,46);
    if(a->state==X4_AUTH_WAITING && !closing) {
        const char *steps[]={x4_tr(X4_TR_50_SCREEN_ACCOUNT_SCAN_QR),x4_tr(X4_TR_50_SCREEN_ACCOUNT_OPEN_LINK),x4_tr(X4_TR_50_SCREEN_ACCOUNT_ENTER_CODE)};
        for(int i=0;i<3;++i) {
            int y=380+i*72;
            char n[4];snprintf(n,sizeof(n),"%d",i+1);
            x4_apple_fill_round_rect(p,334,y,44,44,22,X4_RGBA(255,255,255,26));
            x4_text_px(p,349,y+8,25,X4_FONT_SEMIBOLD,n,INK);
            x4_text_px(p,402,y+7,27,X4_FONT_REGULAR,steps[i],INK);
        }
        x4_apple_fill_round_rect(p,334,616,744,104,20,X4_RGBA(5,9,18,170));
        x4_apple_stroke_round_rect(p,334,616,744,104,20,1.5f,X4_RGBA(255,255,255,70));
        int code_size=64;
        while(code_size>24 && x4_text_width_px(code_size,X4_FONT_SEMIBOLD,a->user_code)>688) --code_size;
        int code_width=x4_text_width_px(code_size,X4_FONT_SEMIBOLD,a->user_code);
        x4_text_px(p,334+(744-code_width)/2,632+(64-code_size)/2,code_size,X4_FONT_SEMIBOLD,a->user_code,INK);
        char expires[64];
        snprintf(expires,sizeof(expires),x4_tr(X4_TR_AUTH_EXPIRES_FMT),a->seconds_left/60,a->seconds_left%60);
        x4_text_px(p,334,746,22,X4_FONT_REGULAR,expires,SOFT);
        static uint8_t qr[qrcodegen_BUFFER_LEN_MAX],temp[qrcodegen_BUFFER_LEN_MAX];
        static char cached_code[32],cached_uri[256];
        static bool valid=false;
        if(strcmp(cached_code,a->user_code)||strcmp(cached_uri,a->verification_uri)) {
            snprintf(cached_code,sizeof(cached_code),"%s",a->user_code);
            snprintf(cached_uri,sizeof(cached_uri),"%s",a->verification_uri);
            char payload[512];
            valid=x4_auth_qr_payload(a->verification_uri,a->user_code,payload,sizeof(payload)) &&
                qrcodegen_encodeText(payload,temp,qr,qrcodegen_Ecc_MEDIUM,qrcodegen_VERSION_MIN,
                    qrcodegen_VERSION_MAX,qrcodegen_Mask_AUTO,false);
        }
        x4_apple_fill_round_rect(p,1140,386,1,360,0,X4_RGBA(255,255,255,38));
        if(valid) phase2_qr(p,qr,1210,410,330);
        x4_apple_spinner(p,620,846,x4_apple_get_animation_phase());
        x4_text_px(p,674,850,26,X4_FONT_REGULAR,x4_tr(X4_TR_U49_ACCOUNT_WAIT),INK);
        x4_text_px(p,476,906,23,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ACCOUNT_PASSWORD),SOFT);
        phase2_back(p,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_CANCEL));
        return;
    }
    const char *title=x4_tr(X4_TR_AUTH_READY);
    switch(a->state) {
    case X4_AUTH_CONNECTING:title=x4_tr(X4_TR_AUTH_CONNECTING);break;
    case X4_AUTH_CONNECTED:title=x4_tr(X4_TR_AUTH_CONNECTED);break;
    case X4_AUTH_AUTHORIZED:title=s_home_ctx.gamertag[0]?s_home_ctx.gamertag:x4_tr(X4_TR_50_SCREEN_ACCOUNT_LINKED);break;
    case X4_AUTH_CANCELLED:title=x4_tr(X4_TR_AUTH_CANCELLED);break;
    case X4_AUTH_EXPIRED:title=x4_tr(X4_TR_50_SCREEN_ACCOUNT_EXPIRED);break;
    case X4_AUTH_DENIED:title=x4_tr(X4_TR_AUTH_DENIED);break;
    case X4_AUTH_ERROR:title=x4_tr(X4_TR_AUTH_ERROR);break;
    default:break;
    }
    if(closing) title=x4_tr(X4_TR_AUTH_CLOSING_TITLE);
    if(a->state==X4_AUTH_ERROR || a->state==X4_AUTH_DENIED)phase2_warning(p,334,404);
    else if(busy||closing)x4_apple_spinner(p,334,404,x4_apple_get_animation_phase());
    else x4_apple_draw_icon(p,334,404,X4_ICON_USER,INK);
    x4_text_ellipsis_px(p,398,400,38,X4_FONT_BOLD,title,1140,INK);
    x4_apple_text_wrap(p,334,490,25,X4_FONT_REGULAR,x4_tr_stage(a->stage),1240,34,3,SOFT);
    if(authorized) {
        x4_text_px(p,334,610,27,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ACCOUNT_READY_CATALOG),INK);
        x4_apple_draw_footer_hint(p,334,692,X4_PAD_R1,x4_tr(X4_TR_U49_ACCOUNT_CATALOG));
        x4_apple_draw_footer_hint(p,710,692,X4_PAD_TRIANGLE,x4_tr(X4_TR_U49_ACCOUNT_SIGN_OUT));
    } else if(!busy && !closing) {
        x4_apple_draw_footer_hint(p,334,650,X4_PAD_CROSS,x4_tr(X4_TR_U49_ACCOUNT_SIGN_IN));
#if !X4_PUBLIC
        x4_apple_draw_footer_hint(p,680,650,X4_PAD_SQUARE,x4_tr(X4_TR_U49_ACCOUNT_CHECK));
#endif
    }
    if(a->error || a->http_status>=400) {
        char detail[80];snprintf(detail,sizeof(detail),"0x%08x · HTTP %d",(unsigned)a->error,a->http_status);
        x4_text_px(p,334,732,21,X4_FONT_REGULAR,detail,SOFT);
        const char *hint=x4_net_hint(a->error);
        if(hint)x4_apple_text_wrap(p,334,568,23,X4_FONT_REGULAR,hint,1240,30,2,SOFT);
    }
    x4_text_px(p,334,852,22,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ACCOUNT_PASSWORD),SOFT);
#if X4_PUBLIC
    x4_apple_text_wrap(p,334,896,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ACCOUNT_HINT_PUBLIC),1320,28,2,SOFT);
#else
    x4_apple_text_wrap(p,334,896,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_ACCOUNT_HINT_LAB),1320,28,2,SOFT);
#endif
    phase2_back(p,x4_tr(X4_TR_APPLE_HINT_BACK));
}

void x4_auth_remember_draw(bool enabled, bool available, bool confirming, uint32_t *p)
{
    if (confirming) {
        x4_apple_glass_panel(p,430,280,1060,530,28,24,46);
        x4_text_px(p,486,330,38,X4_FONT_BOLD,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_TITLE),INK);
        x4_apple_text_wrap(p,486,416,27,X4_FONT_REGULAR,
            x4_tr(X4_TR_AUTH_REMEMBER_WARNING_BODY),948,38,7,INK);
        x4_apple_draw_footer_hint(p,486,728,X4_PAD_CROSS,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_ACCEPT));
        x4_apple_draw_footer_hint(p,990,728,X4_PAD_CIRCLE,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_CANCEL));
    } else {
        x4_apple_fill_round_rect(p,318,760,1274,44,12,X4_RGBA(5,9,18,230));
        x4_apple_draw_footer_hint(p,334,768,X4_PAD_L1,x4_tr(X4_TR_AUTH_REMEMBER));
        x4_text_px(p,910,768,23,X4_FONT_SEMIBOLD,
            x4_tr(!available ? X4_TR_AUTH_REMEMBER_UNAVAILABLE :
                  enabled ? X4_TR_AUTH_REMEMBER_ON : X4_TR_AUTH_REMEMBER_OFF),SOFT);
    }
}

void x4_session_draw(const X4SessionSnapshot *s, int busy, int closing, uint32_t *p)
{
    phase2_base(p,x4_tr(X4_TR_50_SCREEN_SESSION_CRUMB));
    x4_text_px(p,190,174,52,X4_FONT_BOLD,x4_tr(X4_TR_50_SCREEN_SESSION_TITLE),INK);
    x4_apple_text_wrap(p,194,254,30,X4_FONT_REGULAR,s->title_name[0]?s->title_name:x4_tr(X4_TR_50_SCREEN_SESSION_GAME),1530,40,2,SOFT);
    x4_apple_glass_panel(p,190,360,1540,550,28,24,46);
    const bool failed=s->state==X4_SESSION_ERROR || s->error || s->http_status>=400 || s->cleanup_failed;
    char title[192];
    if(closing)snprintf(title,sizeof(title),"%s",x4_tr(X4_TR_50_SCREEN_SESSION_CLOSING_APP));
    else if(failed)snprintf(title,sizeof(title),"%s",x4_tr(X4_TR_50_SCREEN_SESSION_FAILED));
    else if(s->state==X4_SESSION_CLOSED)snprintf(title,sizeof(title),"%s",x4_tr(X4_TR_50_SCREEN_SESSION_CLOSED));
    else if(s->state==X4_SESSION_CANCELLED)snprintf(title,sizeof(title),"%s",x4_tr(X4_TR_50_SCREEN_SESSION_CANCELLED));
    else if(s->state==X4_SESSION_STOPPING)snprintf(title,sizeof(title),"%s",x4_tr(X4_TR_50_SCREEN_SESSION_STOPPING));
    else snprintf(title,sizeof(title),x4_tr(X4_TR_50_SCREEN_SESSION_PREPARING_FMT),s->elapsed_seconds);
    if(failed)phase2_warning(p,252,412);
    else if(busy||closing)x4_apple_spinner(p,252,410,x4_apple_get_animation_phase());
    else x4_apple_draw_icon(p,252,412,X4_ICON_INFO,INK);
    x4_apple_text_wrap(p,316,408,36,X4_FONT_SEMIBOLD,title,1330,46,2,INK);
    if(s->queue_limit_reached)x4_apple_text_wrap(p,252,536,25,X4_FONT_REGULAR,
        x4_tr(X4_TR_50_SCREEN_SESSION_QUEUE_LIMIT),1410,34,3,SOFT);
    else if(s->waiting_for_resources) {
        char queue[160];snprintf(queue,sizeof(queue),x4_tr(X4_TR_50_SCREEN_SESSION_QUEUE_FMT),s->queue_elapsed_seconds/60,s->queue_elapsed_seconds%60);
        x4_apple_text_wrap(p,252,536,25,X4_FONT_REGULAR,queue,1410,34,2,SOFT);
        if(s->queue_eta_available){
            char eta[128];snprintf(eta,sizeof(eta),x4_tr(X4_TR_50_SCREEN_SESSION_QUEUE_ETA_FMT),s->queue_eta_seconds/60,s->queue_eta_seconds%60);
            x4_text_px(p,252,606,22,X4_FONT_REGULAR,eta,SOFT);
        }
        x4_text_px(p,252,650,23,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_SESSION_QUEUE_CANCEL),SOFT);
    } else x4_apple_text_wrap(p,252,536,25,X4_FONT_REGULAR,x4_tr_stage(s->stage),1410,34,3,SOFT);
    const char *hint=s->queue_limit_reached?NULL:x4_net_hint(s->error);
    if(hint)x4_apple_text_wrap(p,252,656,23,X4_FONT_REGULAR,hint,1410,32,2,SOFT);
    char detail[192];
    if(s->cleanup_failed)snprintf(detail,sizeof(detail),x4_tr(X4_TR_50_SCREEN_SESSION_UNCONFIRMED_FMT),(unsigned)s->cleanup_error,s->cleanup_http_status);
    else snprintf(detail,sizeof(detail),"0x%08x · HTTP %d · SDP %d · ICE %d",(unsigned)s->error,s->http_status,s->sdp_http_status,s->ice_http_status);
    x4_text_px(p,252,748,21,X4_FONT_REGULAR,detail,SOFT);
    x4_apple_draw_footer_hint(p,252,826,X4_PAD_CIRCLE,x4_tr(X4_TR_50_SCREEN_SESSION_CANCEL_CATALOG));
    x4_text_px(p,190,954,22,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_GAME_TOUCH_LONG),SOFT);
    x4_text_px(p,190,994,21,X4_FONT_REGULAR,x4_tr(X4_TR_50_SCREEN_GAME_EXIT_LONG),SOFT);
}

void x4_live_status_draw(const X4LiveMediaSnapshot *m, int error, uint32_t *p)
{
    char line[512];
    if(error || m->video_error || m->audio_error) {
        /* Replace the preparation card only; the title and exit controls stay. */
        x4_apple_glass_panel(p,220,386,1480,416,24,36,46);
        phase2_warning(p,252,412);
        int video_error=m->video_error?m->video_error:error;
#if X4_STREAM_PROFILE
        if(video_error) (void)x4_stream_video_error_text(line,sizeof(line),m->video_rejected_width,m->video_rejected_height,video_error);
        else
#endif
            snprintf(line,sizeof(line),x4_tr(X4_TR_50_SCREEN_SESSION_AUDIO_ERROR_FMT),(unsigned)m->audio_error);
        x4_apple_text_wrap(p,316,410,28,X4_FONT_SEMIBOLD,line,1300,38,5,INK);
        snprintf(line,sizeof(line),x4_tr(X4_TR_50_SCREEN_SESSION_AUDIO_VIDEO_FMT),video_error,m->audio_error?x4_tr(X4_TR_LIVE_AUDIO_UNAVAIL):m->audio_playing?x4_tr(X4_TR_LIVE_AUDIO_ACTIVE):x4_tr(X4_TR_LIVE_AUDIO_WAITING),(unsigned)m->audio_error);
        x4_text_ellipsis_px(p,252,650,22,X4_FONT_REGULAR,line,1410,SOFT);
        const char *hint=x4_net_hint(video_error);
        if(hint)x4_apple_text_wrap(p,252,702,23,X4_FONT_REGULAR,hint,1410,31,2,SOFT);
    }
#if !X4_PUBLIC
    snprintf(line,sizeof(line),x4_tr(X4_TR_50_SCREEN_SESSION_COUNTERS_FMT),
        (unsigned long long)m->video_packets,(unsigned long long)m->video_frames,(unsigned long long)m->audio_packets,(unsigned long long)m->audio_frames);
    x4_text_ellipsis_px(p,252,784,20,X4_FONT_REGULAR,line,1410,SOFT);
#endif
}

static void phase2_menu_icon(uint32_t *p, int x, int y, int item, bool muted, bool cancel)
{
    if(item==0) {x4_apple_draw_icon(p,x,y,X4_ICON_HOME,INK);return;}
    if(cancel) {
        for(int i=0;i<22;++i) {x4_rect(p,x+i,y+i,3,3,INK);x4_rect(p,x+21-i,y+i,3,3,INK);}
    } else if(item==1) {
        x4_apple_fill_round_rect(p,x+1,y+8,10,16,2,INK);
        for(int i=0;i<11;++i)x4_rect(p,x+8+i,y+8-i/2,1,16+i,INK);
        if(!muted)for(int i=0;i<24;++i)x4_rect(p,x+i,y+26-i,2,2,INK);
        else x4_apple_stroke_round_rect(p,x+23,y+7,6,18,3,2.0f,INK);
    } else if(item==2) {
        /* Generic game controller. The Xbox trademark logo is not drawn. */
        x4_apple_stroke_round_rect(p,x,y+5,32,22,8,2.0f,INK);
        x4_rect(p,x+6,y+14,9,2,INK);x4_rect(p,x+9,y+11,2,8,INK);
        x4_apple_fill_round_rect(p,x+23,y+11,3,3,1,INK);
        x4_apple_fill_round_rect(p,x+20,y+17,3,3,1,INK);
    } else x4_apple_draw_icon(p,x,y,X4_ICON_INFO,INK);
}

enum { MENU_X=72, MENU_Y=122, MENU_W=748, MENU_ROW_H=76,
       MENU_GAP=18, MENU_MAX_H=252+8*(MENU_ROW_H+MENU_GAP) };
_Static_assert(252+8*(MENU_ROW_H+6)<=MENU_MAX_H,"eight menu items must fit the cached row bounds");
_Static_assert(MENU_Y+252+8*(MENU_ROW_H+6)<=1080,"eight menu items must fit the framebuffer");

/* Ordinary BSS is cacheable CPU memory, unlike the VideoOut scanout. Existing
 * UI primitives use a fixed 1920-pixel stride, so this bounded 7.91 MiB canvas
 * keeps all their antialiasing/alpha reads here. Only the panel spans are copied
 * to scanout. There is no allocation, blur or font work per presented frame. */
static _Alignas(16) uint32_t menu_canvas[X4_WIDTH * X4_HEIGHT];
static struct {
    bool ready, was_open, is_public, muted;
    int selected, items;
    enum X4Lang language;
    unsigned short inset[MENU_MAX_H];
    int height;
    X4TouchMenuCacheStats stats;
} menu_cache;

void x4_touch_menu_cache_reset(void)
{
    memset(&menu_cache,0,sizeof(menu_cache));
}

void x4_touch_menu_cache_snapshot(X4TouchMenuCacheStats *out)
{
    if(!out)return;
    *out=menu_cache.stats;
    out->is_ready=menu_cache.ready;
    out->cache_bytes=sizeof(menu_canvas)+sizeof(menu_cache);
}

/* This leaf deliberately has no destination load, even for unaligned buffers.
 * Opaque copy is independent of prior video pixels. Keep it uninlined so both
 * the host test and the native build can audit the emitted instructions. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void x4_touch_menu_copy_opaque_row(uint32_t *restrict dst,
                                 const uint32_t *restrict src, size_t pixels)
{
#if defined(__SSE2__)
    while(pixels>=4) {
        __m128i value=_mm_loadu_si128((const __m128i *)(const void *)src);
        _mm_storeu_si128((__m128i *)(void *)dst,value);
        src+=4;dst+=4;pixels-=4;
    }
#endif
    while(pixels--) *dst++=*src++;
}

static void touch_menu_prerender(const X4TouchMenu *menu, int muted)
{
    uint32_t *p=menu_canvas;
    int items=menu->item_count;
    const int mx=MENU_X,my=MENU_Y,mw=MENU_W,row_h=MENU_ROW_H,gap=items==8?6:MENU_GAP;
    const int mh=252+items*(row_h+gap);
    /* Opaque dark backing; all further alpha blends read only this canvas. */
    x4_rect(p,mx,my,mw,mh,X4_RGB(11,16,29));
    x4_apple_fill_round_rect(p,mx,my,mw,mh,28,X4_RGB(11,16,29));
    x4_apple_stroke_round_rect(p,mx,my,mw,mh,28,1.5f,X4_RGBA(255,255,255,60));
    x4_apple_draw_brand_logo(p,mx+40,my+30);
    x4_text_px(p,mx+40,my+114,44,X4_FONT_BOLD,x4_tr(X4_TR_50_SCREEN_GAME_MENU),INK);
    for(int i=0;i<items;++i) {
        const char *text="";
        char indicators[128];
        bool cancel=false;
        if(i==0)text=x4_tr(X4_TR_TOUCH_MENU_BACK);
        else if(i==1)text=x4_tr(muted?X4_TR_TOUCH_MENU_UNMUTE:X4_TR_TOUCH_MENU_MUTE);
        else if(i==2)text=x4_tr(X4_TR_TOUCH_MENU_XBOX);
        else if(i==3)text=x4_tr(X4_TR_TOUCH_MENU_RELOAD);
        else if(i==4){
            snprintf(indicators,sizeof(indicators),"%s: %s",x4_tr(X4_TR_NET_HUD_LABEL),x4_net_hud_mode_label());
            text=indicators;
        }
        else if(menu->is_public){text=x4_tr(X4_TR_TOUCH_MENU_CANCEL);cancel=true;}
        else if(i==5)text=x4_tr(X4_TR_TOUCH_MENU_MARK_TEST);
        else if(i==6)text=x4_tr(X4_TR_TOUCH_MENU_EXIT_APP);
        else {text=x4_tr(X4_TR_TOUCH_MENU_CANCEL);cancel=true;}
        int y=my+190+i*(row_h+gap);
        x4_apple_fill_round_rect(p,mx+40,y,mw-80,row_h,20,X4_RGBA(255,255,255,i==menu->selected_index?36:14));
        x4_apple_stroke_round_rect(p,mx+40,y,mw-80,row_h,20,1.0f,X4_RGBA(255,255,255,38));
        if(i==menu->selected_index)x4_apple_focus_ring(p,mx+40,y,mw-80,row_h,20);
        phase2_menu_icon(p,mx+66,y+24,i,muted!=0,cancel);
        x4_text_ellipsis_px(p,mx+126,y+22,28,X4_FONT_SEMIBOLD,text,mw-208,INK);
    }
    int fy=my+mh-50;
    int fx=mx+40;
    fx+=x4_apple_draw_footer_hint(p,fx,fy,X4_PAD_CROSS,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_ACCEPT))+34;
    x4_apple_draw_footer_hint(p,fx,fy,X4_PAD_CIRCLE,x4_tr(X4_TR_UPDATE_NOTICE_CLOSE));
    menu_cache.height=mh;
    for(int y=0;y<mh;++y) {
        /* Pixel-centre circle spans preserve the same 28 px outer radius.
         * Edge antialiasing is flattened against the opaque backing too. */
        int edge=y<mh/2?y:mh-1-y;
        float dy=28.0f-((float)edge+0.5f);
        int inset=dy>0.0f?(int)ceilf(28.0f-sqrtf(28.0f*28.0f-dy*dy)):0;
        menu_cache.inset[y]=(unsigned short)inset;
    }
}

bool x4_touch_menu_prepare(const X4TouchMenu *menu, int muted)
{
    if(!menu || !menu->is_open || menu->item_count<1 || menu->item_count>8) {
        menu_cache.was_open=false;
        menu_cache.ready=false;
        return false;
    }
    enum X4Lang language=x4_lang_current();
    bool changed=!menu_cache.was_open || !menu_cache.ready ||
        menu_cache.selected!=menu->selected_index || menu_cache.items!=menu->item_count ||
        menu_cache.is_public!=menu->is_public || menu_cache.muted!=(muted!=0) ||
        menu_cache.language!=language;
    if(changed) {
        touch_menu_prerender(menu,muted);
        menu_cache.selected=menu->selected_index;
        menu_cache.items=menu->item_count;
        menu_cache.is_public=menu->is_public;
        menu_cache.muted=muted!=0;
        menu_cache.language=language;
        menu_cache.ready=true;
        ++menu_cache.stats.regenerations;
    }
    menu_cache.was_open=true;
    return true;
}

void x4_touch_menu_draw(const X4TouchMenu *menu, int muted, uint32_t *p)
{
    if(!p || !x4_touch_menu_prepare(menu,muted))return;
    uint64_t bytes=0;
    for(int row=0;row<menu_cache.height;++row) {
        size_t inset=menu_cache.inset[row];
        size_t count=MENU_W-inset*2;
        size_t offset=(size_t)(MENU_Y+row)*X4_WIDTH+MENU_X+inset;
        x4_touch_menu_copy_opaque_row(p+offset,menu_canvas+offset,count);
        bytes+=count*sizeof(uint32_t);
    }
    ++menu_cache.stats.frames;
    menu_cache.stats.copied_bytes+=bytes;
}

void x4_live_overlay(const X4LiveMediaSnapshot *m, const X4SessionSnapshot *s, int muted,
                     const X4TouchMenu *touch_menu, bool show_banner, uint32_t *p)
{
    /* Fixed-size notices are UI-only; the session state remains STREAMING. */
    if (s) {
        uint64_t now=sceKernelGetProcessTime();
        char notice[192]={0};
        if(s->reconnecting)
            snprintf(notice,sizeof(notice),x4_tr(X4_TR_RECONNECTING_FMT),s->reconnect_attempt);
        else if(s->recovered_until_us && now<s->recovered_until_us)
            snprintf(notice,sizeof(notice),"%s",x4_tr(X4_TR_CONNECTION_RESTORED));
        else if(s->inactivity_until_us && now<s->inactivity_until_us) {
            unsigned seconds=(unsigned)((s->inactivity_until_us-now+999999u)/1000000u);
            snprintf(notice,sizeof(notice),x4_tr(X4_TR_INACTIVITY_FMT),seconds);
        }
        if(notice[0]) {
            int y=touch_menu&&touch_menu->is_open?30:438;
            x4_apple_fill_round_rect(p,420,y,1080,120,24,X4_RGB(11,16,29));
            x4_apple_stroke_round_rect(p,420,y,1080,120,24,1.5f,X4_RGBA(255,255,255,70));
            x4_text_ellipsis_px(p,456,y+42,30,X4_FONT_SEMIBOLD,notice,1008,INK);
        }
    }
    /* 6-second banner when video playback starts */
    if (show_banner && (!touch_menu || !touch_menu->is_open)) {
        x4_rect(p, 0, X4_HEIGHT - 44, X4_WIDTH, 44, BG);
        const char *server_disp = x4_server_settings_active_display_name();
        char banner[256];
#if X4_PUBLIC
        if (server_disp && server_disp[0]) {
            snprintf(banner, sizeof(banner), "%s · %s · %s",
                x4_picture_settings_active_quality_label(),
                server_disp,
                x4_tr(X4_TR_GAME_TOUCH_HINT));
        } else {
            snprintf(banner, sizeof(banner), "%s · %s",
                x4_picture_settings_active_quality_label(),
                x4_tr(X4_TR_GAME_TOUCH_HINT));
        }
#else
        if (server_disp && server_disp[0]) {
            snprintf(banner, sizeof(banner), "%s · %s",
                server_disp,
                x4_tr(X4_TR_GAME_TOUCH_HINT));
        } else {
            snprintf(banner, sizeof(banner), "%s",
                x4_tr(X4_TR_GAME_TOUCH_HINT));
        }
#endif
        X4AccessCountry access_country = s ? (X4AccessCountry)s->access_country : X4_ACCESS_AUTO;
        if (x4_access_country_valid(access_country) && access_country != X4_ACCESS_AUTO) {
            size_t used = strlen(banner);
            snprintf(banner + used,sizeof(banner) - used,x4_tr(X4_TR_ACCESS_BADGE_FMT),x4_access_country_label(access_country));
        }
        if (access_country != X4_ACCESS_AUTO)
            x4_text_ellipsis_px(p,48,X4_HEIGHT-34,22,X4_FONT_REGULAR,banner,X4_WIDTH-96,WHITE);
        else x4_text(p, 48, X4_HEIGHT - 34, 2, banner, WHITE);
    }
    if (m && s && (m->audio_error || s->input_error || muted)) {
        char line[96];
        const char *audio = m->audio_error ? x4_tr(X4_TR_OVERLAY_AUDIO_UNAVAIL) : muted ? x4_tr(X4_TR_OVERLAY_AUDIO_MUTED) : "";
        snprintf(line, sizeof(line), "%s%s%s", audio,
            audio[0] && s->input_error ? " / " : "",
            s->input_error ? x4_tr(X4_TR_OVERLAY_INPUT_UNAVAIL) : "");
        x4_rect(p, 20, 20, 620, 36, BG);
        x4_text(p, 32, 29, 2, line, WHITE);
    }
    if (touch_menu && touch_menu->is_open) {
        x4_touch_menu_draw(touch_menu, muted, p);
    }
}

void x4_cloud_time_game_draw(uint32_t *p, X4CloudTime value, unsigned alert, bool menu_open)
{
    if (!p || (!alert && (!menu_open || value.source == X4_CLOUD_TIME_NONE))) return;
    char label[128];
    if (alert) snprintf(label, sizeof(label), "%s", x4_tr(alert == X4_CLOUD_TIME_ALERT_5 ?
        X4_TR_CLOUD_TIME_5 : X4_TR_CLOUD_TIME_15));
    else x4_cloud_time_label(value, label, sizeof(label));
    int x = menu_open ? 852 : 72, y = menu_open ? 122 : 72;
    x4_apple_fill_round_rect(p, x, y, 680, 68, 18, X4_RGBA(11, 16, 29, 240));
    x4_apple_stroke_round_rect(p, x, y, 680, 68, 18, 1.5f,
        alert ? X4_RGB(0xFF, 0xB4, 0x5B) : X4_RGBA(255, 255, 255, 70));
    x4_text_ellipsis_px(p, x + 24, y + 21, 25, X4_FONT_SEMIBOLD, label, 632, INK);
}

void x4_cloud_time_confirm_draw(uint32_t *p)
{
    if (!p) return;
    enum { X = 420, W = 1080, PAD = 58, BODY_LINE = 45, BUTTON_H = 56 };
    const char *message = x4_tr(X4_TR_CLOUD_TIME_EMPTY);
    int lines = x4_apple_text_wrap(NULL, 0, 0, 35, X4_FONT_SEMIBOLD, message,
        W - 2 * PAD, BODY_LINE, 3, INK);
    int height = 38 + lines * BODY_LINE + 24 + BUTTON_H + 34;
    int y = (1080 - height) / 2, by = y + height - BUTTON_H - 34;
    x4_apple_fill_round_rect(p, X, y, W, height, 28, X4_RGB(11, 16, 29));
    x4_apple_stroke_round_rect(p, X, y, W, height, 28, 2.0f, X4_RGB(0xFF, 0x68, 0x7A));
    x4_apple_text_wrap(p, X + PAD, y + 36, 35, X4_FONT_SEMIBOLD, message,
        W - 2 * PAD, BODY_LINE, 3, INK);
    x4_apple_pill_button(p, X + PAD, by, 286, BUTTON_H, x4_tr(X4_TR_CLOUD_TIME_TRY),
        false, true, X4_ICON_NONE);
    x4_apple_draw_pad_glyph(p, X + PAD + 19, by + 14, X4_PAD_CROSS);
    x4_apple_pill_button(p, X + PAD + 316, by, 206, BUTTON_H, x4_tr(X4_TR_CANCEL_HINT),
        false, false, X4_ICON_NONE);
    x4_apple_draw_pad_glyph(p, X + PAD + 335, by + 14, X4_PAD_CIRCLE);
}
