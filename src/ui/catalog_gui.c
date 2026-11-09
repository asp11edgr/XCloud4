/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "catalog_gui.h"
#include "cloud_time_ui.h"
#include "apple_ui.h"
#include "canvas.h"
#include "ui2_theme.h"
#include "play_history.h"
#include "font.h"
#include "lang.h"
#include "net_hint.h"
#include "server_settings.h"
#include "stream_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#else
#include <time.h>
#endif

#define W 1920
#define H 1080

enum { CONTENT_LEFT = X4_UI2_CONTENT_X };
static bool sidebar_focused;

void x4_catalog_gui_sidebar_focused(bool focused) { sidebar_focused = focused; }

static const uint32_t C_WHITE = X4_UI2_TEXT;
static const uint32_t C_MUTED = X4_UI2_MUTED;
static const uint32_t C_SOFT  = X4_RGB(0xC5, 0xD0, 0xE0);
static const uint32_t C_GREEN = X4_UI2_ACCENT;

static uint32_t *s_keyboard_backdrop;
static bool s_keyboard_backdrop_valid = false;
static const X4CatalogBrowser *s_keyboard_owner;
static uint32_t *s_keyboard_canvas;
static enum X4Lang s_keyboard_language;
static X4CatalogKeyboardCacheStats keyboard_cache_stats;

void x4_catalog_gui_keyboard_cache_invalidate(void)
{
    free(s_keyboard_backdrop);s_keyboard_backdrop=NULL;
    s_keyboard_backdrop_valid=false;s_keyboard_owner=NULL;s_keyboard_canvas=NULL;
    keyboard_cache_stats.ready=false;keyboard_cache_stats.cache_bytes=0;
}

bool x4_catalog_gui_keyboard_cache_ready(const X4CatalogBrowser *b)
{
    return b&&b->editing&&s_keyboard_backdrop_valid&&s_keyboard_backdrop&&s_keyboard_owner==b&&
        s_keyboard_language==x4_lang_current();
}

void x4_catalog_gui_keyboard_cache_snapshot(X4CatalogKeyboardCacheStats *out)
{
    if(out)*out=keyboard_cache_stats;
}

static void txt(uint32_t *p, int x, int y, int size, int weight, const char *s, int width, uint32_t c)
{
    x4_text_ellipsis_px(p, x, y, size, weight, s ? s : "", width, c);
}

/* Limited word wrapping on UTF-8 boundaries, with width measured by the
 * actual atlas. Final line uses safe ellipsis; no byte-substring clipping. */
static int wrapped(uint32_t *p, int x, int y, int size, int weight, const char *s, int width, int lines, uint32_t c)
{
    int line = 0;
    const char *at = s ? s : "";
    int line_step = size + 8;
    while (*at && line < lines) {
        if (line == lines - 1) {
            txt(p, x, y + line * line_step, size, weight, at, width, c);
            ++line;
            break;
        }
        char out[512];
        size_t n = 0, last_space = 0;
        const char *end = at;
        while (*end && *end != '\n') {
            const char *next = end;
            x4_utf8_next(&next);
            size_t bytes = (size_t)(next - end);
            if (n + bytes >= sizeof(out)) break;
            memcpy(out + n, end, bytes);
            out[n + bytes] = 0;
            if (x4_text_width_px(size, weight, out) > width) break;
            n += bytes;
            if (*end == ' ') last_space = n;
            end = next;
        }
        if (!n) {
            txt(p, x, y + line * line_step, size, weight, at, width, c);
            ++line;
            break;
        }
        if (*end && *end != '\n' && last_space) n = last_space;
        memcpy(out, at, n);
        out[n] = 0;
        while (n && out[n - 1] == ' ') out[--n] = 0;
        txt(p, x, y + line * line_step, size, weight, out, width, c);
        ++line;
        at += n;
        while (*at == ' ' || *at == '\n') ++at;
    }
    return line;
}

static void sanitize_single_line(const char *src, char *dst, size_t cap)
{
    if (!dst || !cap) return;
    dst[0] = '\0';
    if (!src) return;
    size_t j = 0;
    bool prev_space = true;
    for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            if (!prev_space) {
                dst[j++] = ' ';
                prev_space = true;
            }
        } else {
            dst[j++] = (char)ch;
            prev_space = false;
        }
    }
    while (j > 0 && dst[j - 1] == ' ') --j;
    dst[j] = '\0';
}

const char *x4_catalog_gui_asset_status(enum X4CatalogAssetState state)
{
    switch (state) {
    case X4_ASSET_DOWNLOADING: return x4_tr(X4_TR_ASSET_DOWNLOADING);
    case X4_ASSET_READY: return x4_tr(X4_TR_ASSET_READY);
    case X4_ASSET_TEMPORARY_FAILURE: return x4_tr(X4_TR_ASSET_RETRY_PENDING);
    case X4_ASSET_MISSING: return x4_tr(X4_TR_ASSET_MISSING);
    case X4_ASSET_PENDING:
    default: return x4_tr(X4_TR_ASSET_PENDING);
    }
}

static void draw_placeholder_art(uint32_t *p, int x, int y, int w, int h, int r,
    unsigned variant, const char *id, bool small, enum X4CatalogAssetState state,
    int status_box_x, int status_box_y)
{
    x4_apple_procedural_art(p, x, y, w, h, r, variant, X4_GRAD_BOTTOM_DARK);
    const char *label = x4_catalog_gui_asset_status(state);
    if (state == X4_ASSET_READY) label = x4_tr(X4_TR_ASSET_MISSING);
    if (small) {
        if (state == X4_ASSET_DOWNLOADING) label = x4_tr(X4_TR_ASSET_SMALL_LOADING);
        else if (state == X4_ASSET_TEMPORARY_FAILURE) label = x4_tr(X4_TR_ASSET_SMALL_RETRY);
    }
    int bx = status_box_x > 0 ? status_box_x : (x + 20);
    int by = status_box_y > 0 ? status_box_y : (y + 20);
    int font_sz = small ? 18 : 22;
    int lw = x4_text_width_px(font_sz, X4_FONT_REGULAR, label) + 28;
    if (lw > w - 32) lw = w - 32;
    if (lw < 80) lw = 80;
    x4_apple_glass_panel(p, bx, by, lw, small ? 32 : 38, small ? 16 : 19, 28u, 52u);
    txt(p, bx + 14, by + (small ? 5 : 6), font_sz, X4_FONT_REGULAR, label, lw - 28, C_SOFT);
    if (!small && id && id[0] && status_box_x > 0) {
        txt(p, bx + 4, by + 46, 20, X4_FONT_REGULAR, id, w - (bx - x) - 24, C_MUTED);
    }
}

static void draw_cover_rounded(uint32_t *p, X4CatalogAssets *a, const char *product,
    int preferred_surface, int x, int y, int w, int h, int r, unsigned variant, const char *id)
{
    X4CatalogAssetView v;
    if (a && product && product[0] && x4_catalog_assets_acquire(a, product, &v)) {
        if (!x4_apple_blit_image_exact(p, x, y, w, h, r, W, &v.surface[preferred_surface])) {
            draw_placeholder_art(p, x, y, w, h, r, variant, id,
                preferred_surface == X4_COVER_RECENT, v.state, 0, 0);
        }
        x4_catalog_assets_release(a, &v);
    } else {
        draw_placeholder_art(p, x, y, w, h, r, variant, id,
            preferred_surface == X4_COVER_RECENT,
            (product && product[0]) ? X4_ASSET_PENDING : X4_ASSET_MISSING, 0, 0);
    }
}

static void draw_top_bar(uint32_t *p, const X4CatalogBrowser *b)
{
    const char *title = b->active_tab == X4_CAT_TAB_DISCOVER ? x4_tr(X4_TR_APPLE_TAB_DISCOVER) :
        b->active_tab == X4_CAT_TAB_CATALOG ? x4_tr(X4_TR_APPLE_TAB_CATALOG) :
        b->active_tab == X4_CAT_TAB_SEARCH ? x4_tr(X4_TR_U49_SEARCH_LABEL) :
        x4_tr(X4_TR_U49_MY_GAMES);
    if (b->active_tab == X4_CAT_TAB_RECENTS && b->library.bound) {
        x4_apple_draw_icon(p, CONTENT_LEFT, 63, X4_ICON_DEVICE_GAMEPAD, C_WHITE);
        x4_ui2_heading(p, CONTENT_LEFT + 43, 48, title);
    } else x4_ui2_heading(p, CONTENT_LEFT, 48, title);
}

static void draw_non_ready_state(uint32_t *p, const X4CatalogSnapshot *c)
{
    const int x = CONTENT_LEFT, y = 132, w = X4_UI_SAFE_RIGHT - CONTENT_LEFT, h = 768, r = 24;
    x4_apple_glass_panel(p, x, y, w, h, r, 20u, 40u);

    if (!c || c->state == X4_CATALOG_IDLE) {
        txt(p, x + 56, y + 340, 32, X4_FONT_SEMIBOLD, x4_tr(X4_TR_CAT_OPEN_ACCOUNT), w - 112, C_MUTED);
        return;
    }
    if (c->state == X4_CATALOG_LOADING) {
        X4AccessCountryCardPhase access_phase;
        x4_access_country_card_get(NULL, &access_phase);
        if (access_phase == X4_ACCESS_CARD_TRYING) {
            x4_apple_draw_access_card(p, x + 56, y + 72, w - 112, h - 144);
            return;
        }
        const char *label = x4_server_settings_awaiting_first_choice()
            ? x4_tr(X4_TR_CAT_CHOOSE_SERVER_WAIT)
            : x4_tr(X4_TR_CAT_LOADING);
        x4_apple_spinner(p, x + 56, y + 336, x4_apple_get_animation_phase());
        txt(p, x + 110, y + 340, 32, X4_FONT_SEMIBOLD, label, w - 166, C_WHITE);
        return;
    }
    if (c->state == X4_CATALOG_ERROR || c->state == X4_CATALOG_CANCELLED) {
        const char *msg = x4_tr(X4_TR_CAT_ERROR);
        char custom_xerr[256];
        bool unavailable_country = false;
        if (c->state == X4_CATALOG_ERROR) {
            const char *spec = x4_lang_catalog_error_msg(c->error, c->http_status, c->xerr,
                c->offering, c->stage, custom_xerr, sizeof(custom_xerr));
            if (spec) msg = spec;
            unavailable_country = x4_lang_is_cloud_unavailable(
                c->error, c->http_status, c->xerr, c->offering, c->stage);
            if (unavailable_country) msg = x4_tr(X4_TR_ACCESS_TITLE);
        }
        int lines = 0;
        if (!unavailable_country) {
            x4_apple_draw_icon(p, x + 56, y + 60, X4_ICON_INFO, X4_RGB(255,196,112));
            lines = wrapped(p, x + 104, y + 54, 28, X4_FONT_SEMIBOLD, msg, w - 160, 3, C_WHITE);
        }
        int cur_y = y + 54 + lines * 36 + 14;
        if (c->error || c->http_status >= 400 || c->xerr != 0) {
            char det[96];
            if (c->error == 0 && c->xerr != 0) {
                snprintf(det, sizeof(det), x4_tr(X4_TR_DETAIL_XBOX_HTTP_FMT), c->xerr, c->http_status);
            } else {
                snprintf(det, sizeof(det), x4_tr(X4_TR_DETAIL_HTTP_FMT), (unsigned)c->error, c->http_status);
            }
            txt(p, x + 56, cur_y, 24, X4_FONT_REGULAR, det, w - 112, C_MUTED);
            cur_y += 36;
        }
        const char *hint = x4_net_hint(c->error);
        if (hint) {
            int hlines = wrapped(p, x + 56, cur_y, 22, X4_FONT_REGULAR, hint, w - 112, 2, C_MUTED);
            cur_y += hlines * 30 + 16;
        }
        if (unavailable_country) {
            if (cur_y < y + 150) cur_y = y + 150;
            x4_apple_draw_access_card(p, x + 56, cur_y, w - 112, y + h - cur_y - 28);
        }
    }
}

static bool access_card_visible(const X4CatalogSnapshot *catalog)
{
    if (!catalog) return false;
    X4AccessCountryCardPhase phase;
    x4_access_country_card_get(NULL,&phase);
    if (catalog->state == X4_CATALOG_LOADING && phase == X4_ACCESS_CARD_TRYING) return true;
    return catalog->state == X4_CATALOG_ERROR && x4_lang_is_cloud_unavailable(
        catalog->error,catalog->http_status,catalog->xerr,catalog->offering,catalog->stage);
}

static void draw_discover_tab(uint32_t *p, const X4CatalogSnapshot *c,
    const X4CatalogBrowser *b, const X4RecentGames *r,
    X4CatalogAssets *a, const X4CatalogGuiStatus *s)
{
    const X4CatalogBrowser *featured = b;
    if (!b->featured_count) {
        /* The legacy fallback and this renderer share the main UI owner.
         * Rebuild only featured state, without copying the catalog model. */
        static X4CatalogBrowser scratch;
        scratch.featured_index = b->featured_index;
        x4_catalog_browser_rebuild_featured(&scratch, c, r);
        featured = &scratch;
    }

    /* Panoramic Hero Card */
    const int hx = CONTENT_LEFT, hy = 118, hw = X4_UI_SAFE_RIGHT - CONTENT_LEFT, hh = 416, hr = 24;
    unsigned f_idx = featured->featured_index < featured->featured_count ? featured->featured_index : 0;
    const X4CatalogTitle *hero_t = featured->featured_count > 0 && featured->featured_sources[f_idx] < c->count
        ? &c->titles[featured->featured_sources[f_idx]]
        : (c->count > 0 ? &c->titles[0] : NULL);

    const char *hero_prod = hero_t ? hero_t->product_id : "";
    const char *hero_id   = hero_t ? hero_t->id : "";
    X4CatalogAssetView view;
    int acquired = (a && hero_prod[0] && x4_catalog_assets_acquire(a, hero_prod, &view));

    if (acquired && (view.surface[X4_COVER_DETAIL].rgba || view.surface[X4_COVER_GRID].rgba)) {
        const X4CoverRaster *surf = view.surface[X4_COVER_DETAIL].rgba
            ? &view.surface[X4_COVER_DETAIL]
            : &view.surface[X4_COVER_GRID];
        x4_apple_draw_hero_banner(p, hx, hy, hw, hh, hr, surf);
    } else {
        /* Place status badge inside x=1284..1796, y=230..518 so host capture
         * cover-state fingerprint checks observe every state distinctly. */
        enum X4CatalogAssetState st = acquired ? view.state : (hero_prod[0] ? X4_ASSET_PENDING : X4_ASSET_MISSING);
        draw_placeholder_art(p, hx, hy, hw, hh, hr, f_idx, hero_id, false, st, 1340, 260);
    }
    x4_apple_stroke_round_rect(p, hx, hy, hw, hh, hr, 1.5f, 0x33FFFFFFu);

    if (hero_t) {
        /* "DESTACADO" / "FEATURED" badge */
        const char *badge = x4_tr(X4_TR_APPLE_FEATURED_BADGE);
        int bw = x4_text_width_px(18, X4_FONT_BOLD, badge) + 28;
        x4_apple_glass_panel(p, hx + 48, hy + 44, bw, 34, 17, 28u, 60u);
        txt(p, hx + 62, hy + 50, 18, X4_FONT_BOLD, badge, bw - 28, C_SOFT);

        /* Hero Game Title */
        const char *hero_name = hero_t->name[0] ? hero_t->name : hero_t->id;
        if (acquired && view.metadata.name[0]) hero_name = view.metadata.name;
        int max_title_w = 980;
        int sub_y = hy + 206;
        if (x4_text_width_px(54, X4_FONT_BOLD, hero_name) <= max_title_w) {
            txt(p, hx + 48, hy + 104, 54, X4_FONT_BOLD, hero_name, max_title_w, C_WHITE);
            sub_y = hy + 182;
        } else {
            int tlines = wrapped(p, hx + 48, hy + 96, 40, X4_FONT_BOLD, hero_name, max_title_w, 2, C_WHITE);
            sub_y = hy + 96 + tlines * 48 + 10;
        }

        /* Single-line subtitle (short description or category/access) */
        char sub_line[256];
        if (acquired && view.metadata.description[0]) {
            sanitize_single_line(view.metadata.description, sub_line, sizeof(sub_line));
        } else if (acquired && view.metadata.category[0]) {
            sanitize_single_line(view.metadata.category, sub_line, sizeof(sub_line));
        } else {
            snprintf(sub_line, sizeof(sub_line), "%s",
                hero_t->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED));
        }
        txt(p, hx + 48, sub_y, 24, X4_FONT_REGULAR, sub_line, 940, C_SOFT);

        /* Primary game-page action. */
        const char *play_lbl = (s && s->pending) ? x4_tr(X4_TR_CAT_PREPARING) :
            (s && s->busy) ? x4_tr(X4_TR_CAT_QUERYING) : x4_tr(X4_TR_APPLE_HERO_PLAY_NOW);
        int btn_w = x4_text_width_px(24, X4_FONT_SEMIBOLD, play_lbl) + 64;
        if (btn_w < 216) btn_w = 216;
        if (btn_w > 340) btn_w = 340;
        x4_apple_pill_button(p, hx + 48, hy + 292, btn_w, 56, play_lbl, true, false, X4_ICON_NONE);

        /* Featured indicator dots at bottom center */
        if (featured->featured_count > 1) {
            int total_dots_w = (int)featured->featured_count * 10 + 16 + ((int)featured->featured_count - 1) * 10;
            int dx = hx + (hw - total_dots_w) / 2;
            int dy = hy + hh - 26;
            for (unsigned d = 0; d < featured->featured_count; ++d) {
                int dw = (d == f_idx) ? 26 : 8;
                uint32_t dc = (d == f_idx) ? 0xFFF5F7FAu : 0x66FFFFFFu;
                x4_apple_fill_round_rect(p, dx, dy, dw, 8, 4, dc);
                dx += dw + 10;
            }
        }
    }
    if (acquired) x4_catalog_assets_release(a, &view);

    if (!b->top_bar_focused && b->hero_focused) {
        x4_apple_focus_ring(p, hx, hy, hw, hh, hr);
    }

    /* Bottom Horizontal Row: "Continua donde lo dejaste" or "Populares" */
    bool has_recents = (x4_recent_count(r) > 0);
    const char *row_title = has_recents
        ? x4_tr(X4_TR_APPLE_CONTINUE_PLAYING)
        : x4_tr(X4_TR_APPLE_POPULAR_TITLES);
    txt(p, CONTENT_LEFT, 558, 34, X4_FONT_BOLD, row_title, 900, C_WHITE);

    unsigned row_count = x4_catalog_browser_discover_row_count(b, c, r);
    unsigned row_sel = (b->discover_row_selected < row_count) ? b->discover_row_selected : 0;

    /* Show focused row item metadata summary on the right of the section header */
    if (!b->hero_focused && row_count > 0) {
        unsigned sel_src = 0;
        if (x4_catalog_browser_discover_row_source(b, c, r, row_sel, &sel_src) && sel_src < c->count) {
            const X4CatalogTitle *st = &c->titles[sel_src];
            const char *acc = st->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED);
            int aw = x4_text_width_px(20, X4_FONT_SEMIBOLD, acc) + 32;
            x4_apple_glass_panel(p, 1836 - aw, 560, aw, 38, 19, 22u, 48u);
            txt(p, 1836 - aw + 16, 567, 20, X4_FONT_SEMIBOLD, acc, aw - 32, st->entitled ? C_GREEN : C_SOFT);
        }
    }

    if (row_count == 0) {
        x4_apple_glass_panel(p, CONTENT_LEFT, 616, X4_UI_SAFE_RIGHT - CONTENT_LEFT, 272, 20, 18u, 36u);
        txt(p, CONTENT_LEFT + 48, 734, 28, X4_FONT_REGULAR, x4_tr(X4_TR_CAT_NO_MATCH), 1300, C_MUTED);
        return;
    }

    unsigned win_start = 0;
    if (row_sel >= X4_CATALOG_DISCOVER_ROW_VISIBLE) {
        win_start = row_sel - (X4_CATALOG_DISCOVER_ROW_VISIBLE - 1);
    }

    for (unsigned slot = 0; slot < X4_CATALOG_DISCOVER_ROW_VISIBLE && (win_start + slot) < row_count; ++slot) {
        unsigned idx = win_start + slot;
        unsigned src = 0;
        const X4CatalogTitle *ct = NULL;
        if (x4_catalog_browser_discover_row_source(b, c, r, idx, &src) && src < c->count) {
            ct = &c->titles[src];
        }
        const X4RecentGame *rg = has_recents ? x4_recent_get(r, idx) : NULL;
        const char *prod = ct ? ct->product_id : (rg ? rg->product_id : "");
        const char *id   = ct ? ct->id : (rg ? rg->id : "");
        const char *name = ct ? (ct->name[0] ? ct->name : ct->id) : (rg ? (rg->name[0] ? rg->name : rg->id) : "");

        bool focused = (!sidebar_focused && !b->top_bar_focused && !b->hero_focused && idx == row_sel);
        int cx = CONTENT_LEFT + 8 + (int)slot * 288;
        int cy = 612;
        int cw = 256;
        int ch = 144;
        int cr = 12;

        draw_cover_rounded(p, a, prod, X4_COVER_GRID, cx, cy, cw, ch, cr, idx + 1u, id);
        x4_apple_stroke_round_rect(p, cx, cy, cw, ch, cr, 1.2f, 0x2EFFFFFFu);
        txt(p, cx, cy + ch + 10, 22, X4_FONT_SEMIBOLD, name, cw, C_WHITE);
        const char *acc_txt = ct ? (ct->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED)) : "";
        txt(p, cx, cy + ch + 36, 18, X4_FONT_REGULAR, acc_txt, cw, (ct && ct->entitled) ? C_GREEN : C_MUTED);

        if (focused) {
            x4_apple_focus_ring(p, cx, cy, cw, ch, cr);
        }
    }
}

static void draw_search_keyboard_modal_frame(uint32_t *p)
{
    x4_apple_fill_round_rect(p, 0, 0, W, H, 0, X4_RGBA(0x06, 0x0A, 0x14, 0xA8));
    const int mx = 420, my = 200, mw = 1080, mh = 680, mr = 28;
    x4_apple_fill_round_rect(p, mx, my, mw, mh, mr, X4_RGBA(0x10, 0x18, 0x2C, 0xF6));
    x4_apple_stroke_round_rect(p, mx, my, mw, mh, mr, 1.5f, 0x44FFFFFFu);
    txt(p, mx + 44, my + 32, 34, X4_FONT_BOLD, x4_tr(X4_TR_KB_TITLE), mw - 88, C_WHITE);
    txt(p, mx + 44, my + 566, 22, X4_FONT_REGULAR, x4_tr(X4_TR_KB_HELP1), mw - 88, C_WHITE);
#if X4_PUBLIC
    txt(p, mx + 44, my + 608, 22, X4_FONT_REGULAR, x4_tr(X4_TR_KB_HELP2_PUBLIC), mw - 88, C_MUTED);
#else
    txt(p, mx + 44, my + 608, 22, X4_FONT_REGULAR, x4_tr(X4_TR_KB_HELP2_LAB), mw - 88, C_MUTED);
#endif
}

static void draw_search_keyboard_interactive(uint32_t *p, const X4CatalogBrowser *b)
{
    const int mx = 420, my = 200, mw = 1080;

    x4_apple_fill_round_rect(p, mx + 44, my + 92, mw - 88, 60, 18, X4_RGBA(0xFF, 0xFF, 0xFF, 0x18));
    x4_apple_stroke_round_rect(p, mx + 44, my + 92, mw - 88, 60, 18, 1.0f, 0x33FFFFFFu);
    x4_apple_draw_icon(p, mx + 64, my + 110, X4_ICON_SEARCH, b->query[0] ? C_GREEN : C_MUTED);
    txt(p, mx + 102, my + 106, 26, X4_FONT_REGULAR,
        b->query[0] ? b->query : x4_tr(X4_TR_KB_PLACEHOLDER),
        mw - 160, b->query[0] ? C_WHITE : C_MUTED);

    static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (unsigned i = 0; i < 36; ++i) {
        int kx = mx + 44 + (int)(i % 6) * 168;
        int ky = my + 176 + (int)(i / 6) * 62;
        char label[2] = { keys[i], 0 };
        bool active = (i == b->keyboard_cursor);
        if (active) {
            x4_apple_fill_round_rect(p, kx, ky, 152, 50, 14, X4_RGB(0xF5, 0xF7, 0xFA));
            x4_apple_focus_ring(p, kx, ky, 152, 50, 14);
        } else {
            x4_apple_fill_round_rect(p, kx, ky, 152, 50, 14, X4_RGBA(0xFF, 0xFF, 0xFF, 0x14));
            x4_apple_stroke_round_rect(p, kx, ky, 152, 50, 14, 1.0f, 0x2AFFFFFFu);
        }
        int lw = x4_text_width_px(26, X4_FONT_BOLD, label);
        txt(p, kx + (152 - lw) / 2, ky + 9, 26, X4_FONT_BOLD, label, 80,
            active ? X4_RGB(0x0B, 0x10, 0x20) : C_WHITE);
    }
}

static void draw_footer(uint32_t *p, const X4CatalogGuiStatus *s, bool access)
{
    if (access) {
        if (s && s->notice && s->notice[0])
            txt(p, CONTENT_LEFT, X4_UI_FOOTER_Y + 2, 20, X4_FONT_REGULAR, s->notice, 600, C_WHITE);
        return;
    }
    const X4Ui2Hint hints[] = {
        {X4_GLYPH_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)},
        {X4_GLYPH_TRIANGLE, x4_tr(X4_TR_APPLE_HINT_SEARCH)},
        {X4_GLYPH_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)},
        {X4_GLYPH_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)},
    };
    x4_ui2_footer(p, hints, sizeof(hints) / sizeof(hints[0]));
    const char *label = x4_tr(s && s->connected ? X4_TR_CAT_DS4_CONNECTED : X4_TR_CAT_DS4_CONNECT);
    txt(p, CONTENT_LEFT, X4_UI_FOOTER_Y + 2, 20, X4_FONT_REGULAR, label, 510, C_MUTED);
}

enum {
    HOME_LEFT = CONTENT_LEFT, HOME_RIGHT = X4_UI_SAFE_RIGHT, HOME_CARD_W = 384,
    HOME_FOCUS_PAD = 8, DISCOVER_CONTENT_LIFT = 28, HOME_PLAY_Y = 317 - DISCOVER_CONTENT_LIFT, HOME_PLAY_H = 50,
    DISCOVER_GENRE_Y = 376 - DISCOVER_CONTENT_LIFT, DISCOVER_GENRE_H = 36,
    HOME_PLAY_W = 226, HOME_FAVORITE_GAP = 20, HOME_FAVORITE_W = 342,
    HOME_PLAY_TILE_W = HOME_PLAY_W + HOME_FAVORITE_GAP + HOME_FAVORITE_W + 2 * HOME_FOCUS_PAD,
    HOME_PLAY_TILE_H = HOME_PLAY_H + 2 * HOME_FOCUS_PAD,
    DISCOVER_HERO_BOTTOM = 413, DISCOVER_ART_W = W,
    DISCOVER_FADE_STEP_US = 60000, DISCOVER_FADE_DURATION_US = 120000,
    HOME_ROW_LABEL_Y = DISCOVER_HERO_BOTTOM, HOME_ROW_CARD_Y = 461, HOME_ROW_PITCH = 270,
    DISCOVER_COLLECTION_PITCH = 251, DISCOVER_COLLECTION_W = 224,
    DISCOVER_COLLECTION_CARD_H = 126,
    HOME_FEATURE_X = HOME_LEFT - 8, HOME_FEATURE_Y = 112,
    HOME_FEATURE_W = HOME_RIGHT - HOME_LEFT + 14, HOME_FEATURE_H = 326,
};
_Static_assert(HOME_ROW_LABEL_Y - (HOME_PLAY_Y + HOME_PLAY_H + HOME_FOCUS_PAD) >= 24,
    "Play focus must clear first row by 24 pixels");
_Static_assert(HOME_ROW_LABEL_Y - (DISCOVER_GENRE_Y + DISCOVER_GENRE_H) >= 28,
    "Discover genre chips clear the first row heading");

typedef struct {
    const uint32_t *canvas;
    const X4CatalogBrowser *browser;
    X4CatalogAssets *assets;
    X4CatalogBackgroundView background;
    uint64_t model_revision, asset_revision;
    unsigned first, row, column[X4_HOME_ROW_COUNT];
    uint64_t tab_rows[2], tools_key, collection_key;
    enum X4CatalogTab tab;
    bool secondary, redraw_after_keyboard, selector_popup;
    bool library3, library_shelf, library_card_actions;
    bool discover, discover_art, favorite;
    unsigned discover_art_index, discover_fade_marker;
    uint64_t discover_fade_started_us;
    unsigned button_focus, featured_index, featured_count;
    int busy, pending, connected;
    enum X4Lang language;
    X4AccessCountryCardPhase access;
    char hero_id[sizeof(((X4CatalogTitle *)0)->id)], notice[160];
    X4CloudTime cloud_time;
    bool cloud_damage;
    size_t count;
    X4UiRect rects[5];
    X4UiRect play_tile, row_focus[2];
    X4HomeDamageKind kind;
} HomeCanvas;

static HomeCanvas home = { .count = SIZE_MAX };
static uint32_t home_play_pixels[HOME_PLAY_TILE_W * HOME_PLAY_TILE_H];
/* A scene is composed once when the ready hero changes. Only its upper
 * rectangle is cached and copied for carousel steps; rows stay independent. */
static uint32_t discover_art[2][DISCOVER_ART_W * DISCOVER_HERO_BOTTOM];
static uint64_t discover_now_us, discover_fade_elapsed_us;
static unsigned discover_fade_step;
static bool discover_fade_disabled;
static unsigned short discover_column_weight[DISCOVER_ART_W];
static bool discover_column_weight_ready;
static const X4UiRect discover_hero_region = {
    0, 0, DISCOVER_ART_W, DISCOVER_HERO_BOTTOM
};

static uint64_t discover_clock_us(void)
{
#ifdef X4_OPENORBIS
    return sceKernelGetProcessTime();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / 1000u;
#endif
}

unsigned x4_catalog_gui_discover_fade_step(uint64_t *elapsed_us)
{
    if (elapsed_us) *elapsed_us = discover_fade_elapsed_us;
    return discover_fade_step;
}

void x4_catalog_gui_discover_disable_fade(void)
{
    discover_fade_disabled = true;
    home.discover_fade_started_us = 0;
}

unsigned x4_catalog_gui_discover_clock(uint64_t now_us)
{
    discover_now_us = now_us;
    if (!home.discover_fade_started_us || now_us < home.discover_fade_started_us)
        return 0;
    const uint64_t elapsed = now_us - home.discover_fade_started_us;
    return elapsed < DISCOVER_FADE_DURATION_US ?
        1u + (unsigned)(elapsed / DISCOVER_FADE_STEP_US) : 3u;
}
typedef struct {
    unsigned title_lines;
    bool ellipsis;
} HomeHeroText;
static X4CloudTime cloud_home_value;
static const X4UiRect cloud_region = {1376, 126, 460, 86};

void x4_cloud_time_home_set(X4CloudTime value) { cloud_home_value = value; }

bool x4_cloud_time_home_damage(X4UiRect *rect)
{
    if (!home.cloud_damage) return false;
    if (rect) *rect = cloud_region;
    return true;
}

static bool cloud_home_changed(X4CloudTime before, X4CloudTime after)
{
    if ((before.source == X4_CLOUD_TIME_NONE) != (after.source == X4_CLOUD_TIME_NONE) ||
        before.unlimited != after.unlimited) return true;
    return !after.unlimited && (before.remaining_s != after.remaining_s || before.max_s != after.max_s);
}

static void cloud_home_draw(uint32_t *p, X4CloudTime value)
{
    const int x = cloud_region.x, y = cloud_region.y, width = cloud_region.w, height = cloud_region.h;
    char text[128];
    x4_cloud_time_label(value, text, sizeof(text));
    uint32_t accent = value.unlimited || value.remaining_s >= 7200u ? C_GREEN :
        value.remaining_s ? X4_RGB(0xFF, 0xB4, 0x5B) : X4_RGB(0xFF, 0x68, 0x7A);
    x4_apple_fill_round_rect(p, x, y, width, height, 20, X4_RGBA(11, 16, 29, 236));
    x4_apple_stroke_round_rect(p, x, y, width, height, 20, 1.5f, X4_RGBA(255, 255, 255, 65));
    x4_text_ellipsis_px(p, x + 22, y + (value.unlimited ? 30 : 17), 24, X4_FONT_SEMIBOLD,
        text, width - 44, C_WHITE);
    if (value.unlimited) return;
    const int track = width - 44;
    x4_apple_fill_round_rect(p, x + 22, y + 56, track, 10, 5, X4_RGB(0x29, 0x35, 0x49));
    int fill = value.max_s && value.remaining_s < value.max_s ?
        (int)((double)value.remaining_s / (double)value.max_s * track) : track;
    if (fill < 10) fill = 10; /* A red marker remains visible when the balance reaches zero. */
    x4_apple_fill_round_rect(p, x + 22, y + 56, fill, 10, 5, accent);
}

static bool home_clear_background(void)
{
    if (!home.background.raster.rgba) return false;
    x4_catalog_assets_release_background(home.assets, &home.background);
    memset(&home.background, 0, sizeof(home.background));
    return true;
}

void x4_catalog_gui_home_invalidate(void)
{
    home_clear_background();
    memset(&home, 0, sizeof(home));
    home.count = SIZE_MAX;
}

size_t x4_catalog_gui_home_damage(X4UiRect rects[5], X4HomeDamageKind *kind)
{
    if (kind) *kind = home.kind;
    if (rects && home.count != SIZE_MAX) memcpy(rects, home.rects, home.count * sizeof(*rects));
    return home.count;
}

void x4_catalog_gui_home_prewarm(uint32_t *p)
{
    if (!p) return;
    /* A one-pixel restore initializes the procedural cache without replacing
     * the splash on the caller's canvas. The font masks are already in the atlas. */
    uint32_t pixel;
    x4_apple_bg_restore(&pixel, 0, 0, 1, 1);
    static const struct { int size, weight; } fonts[] = {
        {18, X4_FONT_REGULAR}, {20, X4_FONT_SEMIBOLD}, {22, X4_FONT_REGULAR},
        {24, X4_FONT_SEMIBOLD}, {28, X4_FONT_SEMIBOLD}, {32, X4_FONT_SEMIBOLD},
        {32, X4_FONT_BOLD}, {40, X4_FONT_SEMIBOLD},
        {44, X4_FONT_BOLD}
    };
    const char *glyphs = "XCloud4 Descubrir Catálogo Recientes · Ver juego · View game · Ñáéíóúñ… 0123456789 /";
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i)
        (void)x4_text_width_px(fonts[i].size, fonts[i].weight, glyphs);
}

static bool home_update_background(X4CatalogAssets *assets, const char *product)
{
    if (!assets || !product[0]) {
        home_clear_background();
        return home.discover_art;
    }
    /* Keep the composed art while the next product is pending. Release the
     * old source so the worker can reuse the slot without waiting. */
    if (home.background.raster.rgba && strcmp(home.background.product_id, product))
        home_clear_background();
    X4CatalogBackgroundView view;
    if (x4_catalog_assets_acquire_background(assets, &view)) {
        const bool ready = view.raster.width == W && view.raster.height == H &&
            view.raster.rgba && !strcmp(view.product_id, product);
        const bool same = view.slot == home.background.slot &&
            view.raster.rgba == home.background.raster.rgba &&
            view.generation == home.background.generation;
        if (ready && !same) {
            home_clear_background();
            home.background = view;
            return true;
        }
        x4_catalog_assets_release_background(assets, &view);
    }
    enum X4CatalogAssetState state;
    return home.discover_art && !home.background.raster.rgba &&
        x4_catalog_assets_background_status(assets, product, &state) &&
        (state == X4_ASSET_MISSING || state == X4_ASSET_TEMPORARY_FAILURE);
}

static uint32_t discover_mix(uint32_t base, uint32_t art, unsigned weight)
{
    unsigned inverse = 255u - weight;
    unsigned rb = ((base & 0x00ff00ffu) * inverse + (art & 0x00ff00ffu) * weight +
        0x00800080u) >> 8u;
    unsigned g = ((base & 0x0000ff00u) * inverse + (art & 0x0000ff00u) * weight +
        0x00008000u) >> 8u;
    return UINT32_C(0xff000000) | (rb & 0x00ff00ffu) | (g & 0x0000ff00u);
}

static void discover_prepare_art(bool transition)
{
    const uint32_t *source = home.background.raster.rgba;
    home.discover_fade_started_us = transition && !discover_fade_disabled && home.discover_art && source ?
        discover_now_us : 0;
    home.discover_art = source != NULL;
    if (!source) return;
    home.discover_art_index ^= 1u;
    uint32_t *art = discover_art[home.discover_art_index];
    if (!discover_column_weight_ready) {
        /* Continue the dark left side of the hero under the sidebar. Keep
         * column weights on an 0..256 scale for the per-pixel multiply. */
        const unsigned text_shade = 64u;
        const unsigned shade_start = X4_UI2_SIDEBAR_W + 736u;
        const unsigned shade_end = X4_UI2_SIDEBAR_W + 1100u;
        for (unsigned x = 0; x < DISCOVER_ART_W; ++x) {
            const unsigned shade = x < shade_start ? text_shade :
                x < shade_end ? text_shade + (x - shade_start) * (255u - text_shade) /
                    (shade_end - shade_start) : 255u;
            discover_column_weight[x] = (unsigned short)((shade * 256u + 127u) / 255u);
        }
        discover_column_weight_ready = true;
    }
    const unsigned fade_start = 292, fade_span = DISCOVER_HERO_BOTTOM - fade_start - 1;
    for (unsigned y = 0; y < DISCOVER_HERO_BOTTOM; ++y) {
        unsigned vertical = y <= fade_start ? 255u :
            (DISCOVER_HERO_BOTTOM - 1 - y) * 255u / fade_span;
        /* Ease the bottom fade into the background of the shelves. */
        vertical = vertical * vertical * (765u - 2u * vertical) / 65025u;
        /* The status clock sits directly on the art. Fade the upper
         * background itself so light artwork needs no status plate. */
        const unsigned upper = y <= 108u ? 96u :
            y < 212u ? 96u + (y - 108u) * 159u / 104u : 255u;
        vertical = vertical * upper / 255u;
        const uint32_t *row = source + (size_t)(188 + y * 5 / 4) * W;
        uint32_t *out = art + (size_t)y * DISCOVER_ART_W;
        for (unsigned x = 0; x < DISCOVER_ART_W; ++x) {
            const unsigned strength = (vertical * discover_column_weight[x] + 128u) >> 8u;
            out[x] = strength ? discover_mix(X4_UI2_BACKGROUND, row[x], strength) : X4_UI2_BACKGROUND;
        }
    }
}

static void home_restore(uint32_t *p, X4UiRect r)
{
    if (!home.discover) {
        if (r.w == W && r.h == H) x4_apple_bg_draw(p);
        else x4_apple_bg_restore(p, r.x, r.y, r.w, r.h);
        return;
    }
    for (int y = r.y; y < r.y + r.h; ++y) {
        uint32_t *dst = p + (size_t)y * W;
        int left = r.x, right = r.x + r.w;
        if (home.discover_art && y < DISCOVER_HERO_BOTTOM) {
            const size_t offset = (size_t)y * DISCOVER_ART_W + left;
            const uint32_t *art = discover_art[home.discover_art_index] + offset;
            if (home.discover_fade_marker) {
                const uint32_t *previous = discover_art[home.discover_art_index ^ 1u] + offset;
                const unsigned weight = home.discover_fade_marker * 85u;
                for (int x = left; x < right; ++x)
                    dst[x] = discover_mix(*previous++, *art++, weight);
            } else memcpy(dst + left, art, (size_t)(right - left) * sizeof(*p));
        } else {
            for (int x = left; x < right; ++x) dst[x] = X4_UI2_BACKGROUND;
        }
    }
}

/* Focus moves change overlays on an otherwise identical canvas. Save just
 * their bounded base pixels so a move never repeats the hero text/shading
 * or copies and rescales an entire row of covers. */
static void home_tile_save(uint32_t *tile, const uint32_t *p, X4UiRect r)
{
    for (int row = 0; row < r.h; ++row)
        memcpy(tile + (size_t)row * r.w, p + (size_t)(r.y + row) * W + r.x,
            (size_t)r.w * sizeof(*tile));
}

static void home_tile_restore(uint32_t *p, const uint32_t *tile, X4UiRect r)
{
    for (int row = 0; row < r.h; ++row)
        memcpy(p + (size_t)(r.y + row) * W + r.x, tile + (size_t)row * r.w,
            (size_t)r.w * sizeof(*tile));
}

/* Fit the longest whole-word prefix, reserving room for the marker when the
 * final line omits text. Descubrir descriptions are already on one line. */
static const char *home_word_line(char out[512], const char *s, int size, int weight,
    int width, bool last)
{
    const char *word = s, *fit = s;
    const char ellipsis[] = "…";
    while (*word) {
        const char *end = strchr(word, ' ');
        if (!end) end = word + strlen(word);
        const size_t bytes = (size_t)(end - s);
        const bool mark = last && *end;
        if (bytes + (mark ? sizeof(ellipsis) : 1u) > 512u) break;
        memcpy(out, s, bytes);
        if (mark) memcpy(out + bytes, ellipsis, sizeof(ellipsis));
        else out[bytes] = 0;
        if (x4_text_width_px(size, weight, out) > width) break;
        fit = end;
        word = end;
        while (*word == ' ') ++word;
    }
    const size_t bytes = (size_t)(fit - s);
    if (last && *fit) memcpy(out + bytes, ellipsis, sizeof(ellipsis));
    else out[bytes] = 0;
    while (*fit == ' ') ++fit;
    return fit;
}

static unsigned home_lines(char out[2][512], const char *s, int size, int weight, int width)
{
    if (!s || !s[0]) return 0;
    const char *rest = home_word_line(out[0], s, size, weight, width, false);
    if (!out[0][0]) {
        (void)home_word_line(out[0], s, size, weight, width, true);
        return 1;
    }
    if (!*rest) return 1;
    (void)home_word_line(out[1], rest, size, weight, width, true);
    return 2;
}

#ifdef X4_CATALOG_GUI_TEST
static bool home_test_full, home_description_ellipsis;
void x4_catalog_gui_home_test_force_full(void) { home_test_full = true; }
static unsigned home_title_lines;
bool x4_catalog_gui_home_test_description(unsigned *title_lines)
{
    if (title_lines) *title_lines = home_title_lines;
    return home_description_ellipsis;
}
#endif

typedef struct {
    const char *id, *product_id, *name;
    bool available, entitled;
} HomeCard;

static HomeCard home_title_card(const X4CatalogTitle *title)
{
    if (!title) return (HomeCard){0};
    return (HomeCard){title->id, title->product_id, title->name, true, title->entitled};
}

enum {
    TAB_COLUMNS = X4_CATALOG_UI2_COLUMNS, TAB_PAGE = X4_CATALOG_UI2_PAGE_SIZE,
    HOME_SOURCE_CARD_H = 216,
    HOME_FULL_CARD_X = HOME_LEFT + 8, HOME_FULL_CARD_W = 352, HOME_FULL_CARD_H = 198,
    HOME_TAB_CARD_W = 288, HOME_TAB_CARD_H = 162,
    HOME_DISCOVER_CARD_PITCH = 398, TAB_CARD_PITCH = 315,
    CATALOG_FIRST_Y = 346, CATALOG_SECOND_Y = 614,
    SEARCH_FIRST_Y = 316, SEARCH_SECOND_Y = 590,
    LIBRARY_FIRST_Y = 328, LIBRARY_SECOND_Y = 594,
    LIBRARY_SHELF_Y = 302, LIBRARY_GRID_Y = 645,
    LIBRARY_SHELF_SPLIT = 548, LIBRARY_ROW_SPLIT = 573,
};
_Static_assert(HOME_ROW_CARD_Y - (HOME_ROW_LABEL_Y + X4_UI2_HEADING) >= 16,
    "Discover row heading clears the cards by 16 pixels");
_Static_assert(HOME_ROW_LABEL_Y + HOME_ROW_PITCH - (HOME_ROW_CARD_Y + HOME_FULL_CARD_H) >= 24,
    "Discover cards clear the next heading by 24 pixels");
_Static_assert(HOME_ROW_CARD_Y + HOME_ROW_PITCH + HOME_FULL_CARD_H <= X4_UI_FOOTER_Y - 24,
    "Discover cards clear the footer by 24 pixels");
_Static_assert(HOME_FULL_CARD_W * 9 == HOME_FULL_CARD_H * 16 &&
    HOME_TAB_CARD_W * 9 == HOME_TAB_CARD_H * 16, "uncropped covers stay 16:9");
_Static_assert(HOME_FULL_CARD_X + (X4_CATALOG_UI2_DISCOVER_VISIBLE - 1) *
    HOME_DISCOVER_CARD_PITCH + HOME_FULL_CARD_W + 8 <= HOME_RIGHT, "Discover safe width");
_Static_assert(HOME_FULL_CARD_X + (TAB_COLUMNS - 1) * TAB_CARD_PITCH +
    HOME_TAB_CARD_W + 8 <= HOME_RIGHT, "Catalog safe width");
_Static_assert(CATALOG_SECOND_Y + HOME_TAB_CARD_H + 40 < X4_UI_FOOTER_Y &&
    HOME_ROW_CARD_Y + HOME_ROW_PITCH + HOME_FULL_CARD_H + HOME_FOCUS_PAD < X4_UI_FOOTER_Y,
    "full art clears the footer");
_Static_assert(HOME_FULL_CARD_X + 5 * DISCOVER_COLLECTION_PITCH + DISCOVER_COLLECTION_W +
    HOME_FOCUS_PAD <= HOME_RIGHT, "collection cards stay in the safe area");

static bool discover_has_collections(const X4CatalogBrowser *b)
{
    return b->ui2_collection_count > 0;
}

static int discover_row_y(unsigned row, bool art)
{
    return (art ? HOME_ROW_CARD_Y : HOME_ROW_LABEL_Y) + (int)row * HOME_ROW_PITCH;
}

static bool discover_collection_row(const X4CatalogBrowser *b, unsigned row)
{
    return row == b->home_rows.count && discover_has_collections(b);
}

enum {
    HOME_TAB_FOCUS_W = HOME_TAB_CARD_W + 2 * HOME_FOCUS_PAD,
    HOME_TAB_FOCUS_H = HOME_TAB_CARD_H + 2 * HOME_FOCUS_PAD,
    HOME_ROW_FOCUS_W = HOME_FULL_CARD_W + 2 * HOME_FOCUS_PAD,
    HOME_ROW_FOCUS_H = HOME_FULL_CARD_H + 2 * HOME_FOCUS_PAD,
};
static uint32_t home_row_backdrop[2][HOME_ROW_FOCUS_W * HOME_ROW_FOCUS_H];

static bool home_discover_focus_rect(const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    unsigned visual, X4UiRect *rect)
{
    unsigned index = b->home_first_row + visual;
    if (index >= b->home_rows.count) return false;
    const X4HomeRow *row = &b->home_rows.rows[index];
    unsigned selected = b->home_columns[row->kind];
    unsigned first = x4_catalog_browser_home_row_start(b, index);
    if (selected >= row->count || selected < first ||
        selected - first >= X4_CATALOG_UI2_DISCOVER_VISIBLE ||
        row->indices[selected] >= c->count) return false;
    *rect = (X4UiRect){
        HOME_FULL_CARD_X + (int)(selected - first) * HOME_DISCOVER_CARD_PITCH - HOME_FOCUS_PAD,
        discover_row_y(visual, true) - HOME_FOCUS_PAD,
        HOME_FULL_CARD_W + 2 * HOME_FOCUS_PAD, HOME_FULL_CARD_H + 2 * HOME_FOCUS_PAD,
    };
    return true;
}

static void home_draw_row_focus(uint32_t *p, unsigned visual, X4UiRect rect)
{
    home_tile_save(home_row_backdrop[visual], p, rect);
    home.row_focus[visual] = rect;
    x4_ui2_focus(p, rect.x + HOME_FOCUS_PAD, rect.y + HOME_FOCUS_PAD,
        rect.w - 2 * HOME_FOCUS_PAD, rect.h - 2 * HOME_FOCUS_PAD, 12);
}

static X4UiRect home_move_row_focus(uint32_t *p, unsigned visual, X4UiRect rect)
{
    X4UiRect old = home.row_focus[visual];
    if (old.w) home_tile_restore(p, home_row_backdrop[visual], old);
    if (!rect.w) {
        home.row_focus[visual] = (X4UiRect){0};
        return old;
    }
    home_draw_row_focus(p, visual, rect);
    if (!old.w) return rect;
    /* Both rings are in the same visual row. Keep the existing one-damage-
     * rectangle-per-row bound while reporting both modified positions. */
    int left = old.x < rect.x ? old.x : rect.x;
    int right = old.x + old.w > rect.x + rect.w ? old.x + old.w : rect.x + rect.w;
    return (X4UiRect){left, rect.y, right - left, rect.h};
}

typedef struct {
    unsigned count[2], first[2], selected[2], active_row, columns, page;
    enum X4RecentState recent_state;
    bool library, search, favorite_cards, library3, shelf;
} HomeTab;

static HomeTab home_tab(const X4CatalogSnapshot *c, const X4CatalogBrowser *b, const X4RecentGames *r)
{
    HomeTab tab = {
        .library = b->active_tab == X4_CAT_TAB_RECENTS,
        .search = b->active_tab == X4_CAT_TAB_SEARCH,
        .recent_state = r ? r->state : X4_RECENT_IDLE,
        .columns = b->ui2_enabled ? TAB_COLUMNS : X4_CATALOG_TAB_COLUMNS,
        .page = b->ui2_enabled ? TAB_PAGE : X4_CATALOG_TAB_PAGE_SIZE,
    };
    if (tab.library) {
        if (b->library.bound) {
            const X4Ui2Library *l = &b->library;
            tab.library3 = true;
            tab.shelf = l->tab == X4_UI2_LIBRARY_ALL && l->recent_count;
            tab.favorite_cards = l->tab == X4_UI2_LIBRARY_FAVORITES;
            tab.active_row = l->focus == X4_UI2_LIBRARY_CONTINUE ? 0 :
                tab.shelf ? 1 : (l->grid_selected / tab.columns) % 2u;
            const unsigned page = tab.shelf ? tab.columns : tab.page;
            const unsigned first = l->grid_selected / page * page;
            tab.count[0] = tab.shelf ? (l->recent_count < tab.columns ? l->recent_count : tab.columns) :
                l->grid_count;
            tab.count[1] = l->grid_count;
            tab.first[0] = tab.shelf ? 0 : first;
            tab.first[1] = tab.shelf ? first : first + tab.columns;
            tab.selected[0] = tab.shelf ? l->continue_selected : l->grid_selected;
            tab.selected[1] = l->grid_selected;
            return tab;
        }
        tab.favorite_cards = b->ui2_favorites;
        unsigned count = tab.favorite_cards ?
            (b->favorites && b->favorites->count <= X4_FAVORITES_MAX ? b->favorites->count : 0) :
            x4_recent_count(r);
        unsigned selected = tab.favorite_cards ? b->ui2_favorite_selected : b->recent_selected;
        if (selected >= count) selected = 0;
        tab.active_row = (selected / tab.columns) % 2u;
        tab.count[0] = tab.count[1] = count;
        tab.selected[0] = tab.selected[1] = selected;
        tab.first[0] = selected / tab.page * tab.page;
        tab.first[1] = tab.first[0] + tab.columns;
        return tab;
    }
    const unsigned count = c->count <= X4_CATALOG_MAX && !b->criteria_dirty &&
        b->filtered_count <= c->count ? b->filtered_count : 0;
    const unsigned selected = b->selected < count ? b->selected : 0;
    tab.active_row = (selected / tab.columns) % 2u;
    tab.count[0] = tab.count[1] = count;
    tab.selected[0] = tab.selected[1] = selected;
    tab.first[0] = selected / tab.page * tab.page;
    tab.first[1] = tab.first[0] + tab.columns;
    return tab;
}

static HomeCard home_tab_card(const HomeTab *tab, const X4CatalogSnapshot *c,
    const X4CatalogBrowser *b, const X4RecentGames *r, unsigned row, unsigned ordinal)
{
    if (ordinal >= tab->count[row]) return (HomeCard){0};
    if (!tab->library) {
        const unsigned src = b->indices[ordinal];
        return home_title_card(src < c->count ? &c->titles[src] : NULL);
    }
    if (tab->library3) {
        const X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_card(b, tab->shelf && !row, ordinal);
        if (!card.title) return (HomeCard){0};
        const char *id = card.title_id[0] ? card.title_id : card.product_id;
        return (HomeCard){id, card.product_id, card.title, card.source < c->count, false};
    }
    if (tab->favorite_cards) {
        const X4Favorite *favorite = &b->favorites->items[ordinal];
        return (HomeCard){favorite->product_id, favorite->product_id, favorite->name, false, false};
    }
    const X4RecentGame *game = x4_recent_get(r, ordinal);
    if (!game) return (HomeCard){0};
    unsigned src;
    if (x4_recent_catalog_index(game, c, &src)) {
        HomeCard card = home_title_card(&c->titles[src]);
        if (!card.product_id[0]) card.product_id = game->product_id;
        return card;
    }
    return (HomeCard){game->id, game->product_id, game->name, false, false};
}

static int home_tab_card_y(const HomeTab *tab, unsigned row)
{
    if (tab->shelf) return row ? LIBRARY_GRID_Y : LIBRARY_SHELF_Y;
    if (tab->library) return row ? LIBRARY_SECOND_Y : LIBRARY_FIRST_Y;
    if (tab->search) return row ? SEARCH_SECOND_Y : SEARCH_FIRST_Y;
    return row ? CATALOG_SECOND_Y : CATALOG_FIRST_Y;
}

static X4UiRect home_tab_focus_rect(const HomeTab *tab, unsigned row)
{
    if (tab->selected[row] >= tab->count[row] ||
        tab->selected[row] < tab->first[row] ||
        tab->selected[row] - tab->first[row] >= tab->columns) return (X4UiRect){0};
    const unsigned column = tab->selected[row] - tab->first[row];
    return (X4UiRect){
        HOME_FULL_CARD_X + (int)column * TAB_CARD_PITCH - HOME_FOCUS_PAD,
        home_tab_card_y(tab, row) - HOME_FOCUS_PAD,
        HOME_TAB_FOCUS_W, HOME_TAB_FOCUS_H,
    };
}

static void catalog_text(uint32_t *p, const char *role, int x, int y, int size, int weight,
    const char *label, int width, uint32_t color);

static void discover_action_icon(uint32_t *p, int x, int y, bool primary, bool favorite,
    uint32_t color)
{
    if (primary) {
        x4_ui2_play(p, x - 3, y - 2, color);
    } else {
        x4_apple_fill_round_rect(p, x, y + 9, 18, 2, 1, color);
        if (!favorite) x4_apple_fill_round_rect(p, x + 8, y + 1, 2, 18, 1, color);
    }
}

static int discover_button_x(bool favorite_button)
{
    return HOME_LEFT + (favorite_button ? HOME_PLAY_W + HOME_FAVORITE_GAP : 0);
}

static int discover_button_width(bool favorite_button)
{
    return favorite_button ? HOME_FAVORITE_W : HOME_PLAY_W;
}

static void discover_button_focus(uint32_t *p, unsigned focus)
{
    if (!focus) return;
    x4_ui2_focus(p, discover_button_x(focus == 2), HOME_PLAY_Y,
        discover_button_width(focus == 2), HOME_PLAY_H, X4_UI2_RADIUS_CONTROL);
}

static void discover_draw_button(uint32_t *p, const char *label, bool secondary,
    bool favorite, bool waiting)
{
    const int x = discover_button_x(secondary), width = discover_button_width(secondary);
    const bool green = !secondary && !waiting;
    const uint32_t color = green ? X4_UI2_INK : X4_UI2_TEXT;
    if (green)
        x4_apple_fill_round_rect(p, x, HOME_PLAY_Y, width, HOME_PLAY_H, X4_UI2_RADIUS_CONTROL, C_GREEN);
    else x4_ui2_card(p, x, HOME_PLAY_Y, width, HOME_PLAY_H, X4_UI2_RADIUS_CONTROL);
    if (waiting && !secondary) x4_apple_spinner(p, x + 20, HOME_PLAY_Y + 11, 3);
    else discover_action_icon(p, x + 21, HOME_PLAY_Y + 15, !secondary, favorite, color);
    catalog_text(p, green ? "discover-play" : "discover-favorite", x + 54, HOME_PLAY_Y + 11, X4_UI2_BODY,
        X4_FONT_SEMIBOLD, label, width - 72, color);
}

static HomeHeroText home_draw_hero_text(uint32_t *p, const char *name, const char *meta,
    const char *desc)
{
    char description[2][512];
    const unsigned nd = home_lines(description, desc, X4_UI2_BODY, X4_FONT_REGULAR, 700);
    const int title_y = (meta[0] ? 158 : 182) - DISCOVER_CONTENT_LIFT;
    catalog_text(p, "hero-title", HOME_LEFT, title_y, X4_UI2_TITLE, X4_FONT_BOLD, name, 968, C_WHITE);
    if (meta[0])
        catalog_text(p, "hero-publisher", HOME_LEFT, 218 - DISCOVER_CONTENT_LIFT, X4_UI2_BODY, X4_FONT_SEMIBOLD,
            meta, 700, C_MUTED);
    for (unsigned i = 0; i < nd; ++i)
        catalog_text(p, "hero-description", HOME_LEFT, 250 - DISCOVER_CONTENT_LIFT + 27 * (int)i, X4_UI2_BODY,
            X4_FONT_REGULAR, description[i], 700, C_SOFT);
    const size_t last = nd == 2 ? strlen(description[1]) : 0;
    return (HomeHeroText){
        .title_lines = name[0] != 0,
        .ellipsis = last >= 3 && !strcmp(description[1] + last - 3, "…"),
    };
}

typedef struct {
    uint32_t pixels[HOME_FULL_CARD_W * HOME_FULL_CARD_H];
    char product[X4_PRODUCT_ID_CAP];
    const uint32_t *source;
    uint64_t revision;
    bool tab;
} Ui2CatalogTile;
_Static_assert((int)TAB_COLUMNS >= (int)X4_CATALOG_UI2_DISCOVER_VISIBLE,
    "art tile cache covers both row widths");
static Ui2CatalogTile tile_cache[2 * TAB_COLUMNS];
static unsigned tile_next;
static uint64_t tile_revision;

static bool home_blit_full_card(uint32_t *p, int x, int y, const X4CoverRaster *card,
    const char *product, bool tab)
{
    if (!card->rgba || card->width != HOME_CARD_W || card->height != HOME_SOURCE_CARD_H ||
        !product || !product[0]) return false;
    const int width = tab ? HOME_TAB_CARD_W : HOME_FULL_CARD_W;
    const int height = tab ? HOME_TAB_CARD_H : HOME_FULL_CARD_H;
    Ui2CatalogTile *tile = NULL;
    for (size_t i = 0; i < sizeof(tile_cache) / sizeof(tile_cache[0]); ++i) {
        Ui2CatalogTile *candidate = &tile_cache[i];
        if (candidate->source == card->rgba && candidate->tab == tab &&
            candidate->revision == tile_revision && !strcmp(candidate->product, product)) {
            tile = candidate;
            break;
        }
    }
    if (!tile) {
        tile = &tile_cache[tile_next++ % (sizeof(tile_cache) / sizeof(tile_cache[0]))];
        snprintf(tile->product, sizeof(tile->product), "%s", product);
        tile->source = card->rgba;
        tile->revision = tile_revision;
        tile->tab = tab;
        for (int row = 0; row < height; ++row) {
            int source_y = row * (HOME_SOURCE_CARD_H - 1) / (height - 1);
            const uint32_t *src = card->rgba + (size_t)source_y * HOME_CARD_W;
            uint32_t *dst = tile->pixels + (size_t)row * width;
            for (int col = 0; col < width; ++col)
                dst[col] = src[col * (HOME_CARD_W - 1) / (width - 1)];
        }
    }
#ifdef X4_UI48_CAPTURE
    x4_ui48_capture_cover((X4UiRect){x, y, width, height}, card);
#endif
    const X4CoverRaster scaled = {(unsigned)width, (unsigned)height, tile->pixels};
    return x4_apple_blit_image_exact(p, x, y, width, height, 12, W, &scaled);
}

static void home_draw_card(uint32_t *p, const HomeCard *t, X4CatalogAssets *a,
    int cy, unsigned i, int pitch, bool tab)
{
    if (!t->id) return;
    int x = HOME_FULL_CARD_X + (int)i * pitch;
    int width = tab ? HOME_TAB_CARD_W : HOME_FULL_CARD_W;
    int height = tab ? HOME_TAB_CARD_H : HOME_FULL_CARD_H;
    X4CatalogAssetView view;
    bool acquired = a && t->product_id[0] && x4_catalog_assets_acquire(a, t->product_id, &view);
    bool drawn = acquired && home_blit_full_card(p, x, cy, &view.surface[X4_COVER_ROW],
        t->product_id, tab);
    if (!drawn) {
        x4_apple_fill_round_rect(p, x, cy, width, height, 12, X4_RGB(23, 32, 42));
        enum X4CatalogAssetState state = acquired ? view.state :
            t->product_id[0] ? X4_ASSET_PENDING : X4_ASSET_MISSING;
        if (state == X4_ASSET_READY) state = X4_ASSET_MISSING;
        if (state == X4_ASSET_PENDING || state == X4_ASSET_DOWNLOADING)
            x4_apple_spinner(p, x + 24, cy + 82, i + 2);
        txt(p, x + 60, cy + 82, 22, X4_FONT_REGULAR,
            x4_catalog_gui_asset_status(state), width - 84, C_MUTED);
        txt(p, x + 60, cy + 115, 22, X4_FONT_REGULAR,
            t->name[0] ? t->name : t->id, width - 84, C_WHITE);
    }
    if (acquired) x4_catalog_assets_release(a, &view);
}

static void catalog_text(uint32_t *p, const char *role, int x, int y, int size, int weight,
    const char *label, int width, uint32_t color)
{
#ifdef X4_UI46_CAPTURE
    const int ink = x4_text_width_px(size, weight, label);
    x4_ui46_capture_contrast(p, role, x, y, ink < width ? ink : width, size + 1, color);
#else
    (void)role;
#endif
    txt(p, x, y, size, weight, label, width, color);
}

static void discover_continue_history(uint32_t *p, const X4History *history,
    const char *product, uint64_t now, int x, int y)
{
    const X4HistoryEntry *entry = x4_history_find(history, product);
    if (!entry) return;
    const bool english = x4_lang_current() == X4_LANG_EN;
    char last[X4_HISTORY_LABEL], played[X4_HISTORY_LABEL], labels[2][80];
    x4_history_text(entry, now, english, last, played);
    unsigned count = 0;
    if (*last)
        snprintf(labels[count++], sizeof(labels[0]), "%s: %s",
            x4_tr(X4_TR_U49_LAST_SESSION), last);
    if (*played)
        snprintf(labels[count++], sizeof(labels[0]), "%s: %s",
            x4_tr(X4_TR_U49_PLAY_TIME), played);
    if (!count) return;

    const int inset = 10, pad = 10, size = X4_UI2_CAPTION, pitch = 24;
    int width = 0;
    for (unsigned i = 0; i < count; ++i) {
        int ink = x4_text_width_px(size, X4_FONT_SEMIBOLD, labels[i]);
        if (ink > width) width = ink;
    }
    const int cap = HOME_FULL_CARD_W - 2 * (inset + pad);
    if (width > cap) width = cap;
    const int height = 12 + (int)count * pitch;
    const int top = y + HOME_FULL_CARD_H - height - inset;
    x4_apple_fill_round_rect(p, x + inset, top, width + 2 * pad, height, X4_UI2_RADIUS_CONTROL,
        X4_RGBA(7, 14, 24, 228));
    for (unsigned i = 0; i < count; ++i)
        catalog_text(p, "discover-history", x + inset + pad, top + 5 + (int)i * pitch,
            size, X4_FONT_SEMIBOLD, labels[i], width, C_WHITE);
}

static void tile_title(uint32_t *p, int x, int y, const char *name, unsigned size)
{
    char lines[2][512];
    unsigned count = home_lines(lines, name, (int)size, X4_FONT_SEMIBOLD, HOME_TAB_CARD_W - 4);
    for (unsigned i = 0; i < count; ++i)
        catalog_text(p, "grid-game-caption", x + 2, y + (int)i * ((int)size + 4), (int)size,
            X4_FONT_SEMIBOLD, lines[i], HOME_TAB_CARD_W - 4, C_WHITE);
}

static void library_history_badge(uint32_t *p, const X4Ui2LibraryCard *card,
    int x, int y, uint64_t now, bool prefer_played)
{
    if (!card->history) return;
    char last[X4_HISTORY_LABEL], played[X4_HISTORY_LABEL], label[92];
    x4_history_text(card->history, now, x4_lang_current() == X4_LANG_EN, last, played);
    if (!last[0] && !played[0]) return;
    const bool show_played = played[0] && (prefer_played || !last[0]);
    snprintf(label, sizeof(label), "%s: %s",
        show_played ? x4_tr(X4_TR_U49_PLAY_TIME) :
            x4_tr(X4_TR_U49_LAST_SESSION), show_played ? played : last);
    const int width = x4_text_width_px(18, X4_FONT_SEMIBOLD, label);
    int w = width + 24;
    if (w > HOME_TAB_CARD_W - 62) w = HOME_TAB_CARD_W - 62;
    x4_apple_fill_round_rect(p, x + 10, y + HOME_TAB_CARD_H - 43, w, 33, 10, X4_RGB(12, 22, 34));
    catalog_text(p, "library-history", x + 22, y + HOME_TAB_CARD_H - 37, 18,
        X4_FONT_SEMIBOLD, label, w - 24, C_WHITE);
}

static void library_heart(uint32_t *p, int x, int y, bool favorite)
{
    if (favorite) {
        /* Interior spans of the existing 24 px outline. */
        static const unsigned char inset[] = {5, 4, 4, 5, 5, 6, 7, 8, 9, 10, 11};
        x4_rect(p, x + 7, y + 5, 2, 1, C_GREEN);
        x4_rect(p, x + 15, y + 5, 2, 1, C_GREEN);
        x4_rect(p, x + 5, y + 6, 6, 1, C_GREEN);
        x4_rect(p, x + 13, y + 6, 6, 1, C_GREEN);
        for (unsigned row = 0; row < sizeof(inset); ++row)
            x4_rect(p, x + inset[row], y + 7 + (int)row, 24 - 2 * inset[row], 1, C_GREEN);
    }
    x4_apple_draw_icon(p, x, y, X4_ICON_HEART, favorite ? C_GREEN : C_WHITE);
}

static void library_draw_row(uint32_t *p, const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    const X4RecentGames *r, X4CatalogAssets *a, const HomeTab *tab, unsigned visual)
{
    const X4Ui2Library *l = &b->library;
    const bool shelf = tab->shelf && !visual;
    const int y = home_tab_card_y(tab, visual);
    const unsigned first = tab->first[visual], count = tab->count[visual];
    const uint64_t now = x4_history_now();
    if (!count) {
        if (visual && !tab->shelf) return;
        const char *message = l->tab == X4_UI2_LIBRARY_FAVORITES ?
            x4_tr(X4_TR_U49_NO_FAVORITES) :
            l->tab == X4_UI2_LIBRARY_RECENT && tab->recent_state == X4_RECENT_LOADING ?
            x4_tr(X4_TR_CAT_RECENT_LOADING) :
            l->tab == X4_UI2_LIBRARY_RECENT && tab->recent_state == X4_RECENT_ERROR ?
            x4_tr(X4_TR_U49_RECENTS_LOAD_FAILED) :
            x4_tr(X4_TR_U49_NO_GAMES_IN_VIEW);
        x4_ui2_card(p, HOME_FULL_CARD_X, y, HOME_RIGHT - HOME_FULL_CARD_X, 83, 16);
        catalog_text(p, "library-empty", HOME_FULL_CARD_X + 24, y + 26, 24, X4_FONT_REGULAR,
            message, HOME_RIGHT - HOME_FULL_CARD_X - 48, C_MUTED);
        return;
    }
    for (unsigned i = 0; i < tab->columns && first + i < count; ++i) {
        const X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_card(b, shelf, first + i);
        const HomeCard tile = home_tab_card(tab, c, b, r, visual, first + i);
        if (!card.title || !tile.id) continue;
        const int x = HOME_FULL_CARD_X + (int)i * TAB_CARD_PITCH;
        home_draw_card(p, &tile, a, y, i, TAB_CARD_PITCH, true);
        tile_title(p, x, y + HOME_TAB_CARD_H + 8, card.title, 20);
        if (shelf) {
            library_history_badge(p, &card, x, y, now, false);
        } else {
            const int icon_x = x + HOME_TAB_CARD_W - 44;
            x4_apple_fill_round_rect(p, icon_x, y + 10, 34, 34, 17, X4_RGB(12, 22, 34));
            library_heart(p, icon_x + 5, y + 15, card.favorite);
            if (card.source < c->count) {
                const int bottom = y + HOME_TAB_CARD_H - 42;
                x4_apple_fill_round_rect(p, x + 10, bottom, 42, 32, 13, X4_RGB(12, 22, 34));
                x4_ui2_cloud(p, x + 17, bottom + 8, 17, C_WHITE);
            }
            library_history_badge(p, &card, x + 42, y, now, true);
        }
    }
}

static void home_draw_row(uint32_t *p, const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    const X4RecentGames *r, X4CatalogAssets *a, const HomeTab *tab, unsigned visual)
{
    if (tab) {
        if (tab->library3) {
            library_draw_row(p, c, b, r, a, tab, visual);
            return;
        }
        const int y = home_tab_card_y(tab, visual);
        const unsigned first = tab->first[visual], count = tab->count[visual];
        if (tab->library && !visual && !count) {
            const char *message = tab->favorite_cards ?
                x4_tr(X4_TR_U49_FAVORITE_FROM_GAME) :
                x4_tr(X4_TR_U49_RECENTS_WILL_APPEAR);
            if (!tab->favorite_cards && tab->recent_state == X4_RECENT_LOADING)
                message = x4_tr(X4_TR_CAT_RECENT_LOADING);
            if (!tab->favorite_cards && tab->recent_state == X4_RECENT_ERROR)
                message = x4_tr(X4_TR_CAT_RECENT_ERROR);
            x4_ui2_card(p, HOME_FULL_CARD_X, y, HOME_RIGHT - HOME_FULL_CARD_X, 104, 18);
            catalog_text(p, "library-empty", HOME_FULL_CARD_X + 24, y + 27, 24, X4_FONT_REGULAR,
                message, HOME_RIGHT - HOME_FULL_CARD_X - 48, C_MUTED);
            if (!tab->favorite_cards && tab->recent_state == X4_RECENT_ERROR) {
                x4_apple_draw_pad_glyph(p, HOME_FULL_CARD_X + 24, y + 59, X4_PAD_SQUARE);
                catalog_text(p, "library-retry", HOME_FULL_CARD_X + 68, y + 61, 20,
                    X4_FONT_REGULAR, x4_tr(X4_TR_CAT_RECENT_RETRY),
                    HOME_RIGHT - HOME_FULL_CARD_X - 90, C_SOFT);
            }
        }
        for (unsigned i = 0; i < tab->columns && first + i < count; ++i) {
            const HomeCard card = home_tab_card(tab, c, b, r, visual, first + i);
            home_draw_card(p, &card, a, y, i, TAB_CARD_PITCH, true);
            if (card.id) {
                const int x = HOME_FULL_CARD_X + (int)i * TAB_CARD_PITCH;
                if (tab->favorite_cards) {
                    const int heart = x + HOME_TAB_CARD_W - 52;
                    x4_apple_fill_round_rect(p, heart, y + 12, 40, 40, 12, X4_UI2_BACKGROUND);
                    library_heart(p, heart + 8, y + 20, true);
                }
                tile_title(p, x, y + HOME_TAB_CARD_H + 10, card.name[0] ? card.name : card.id, 22);
            }
        }
        return;
    }

    const unsigned index = b->home_first_row + visual;
    if (index >= b->home_rows.count) return;
    static const enum X4TextId labels[X4_HOME_ROW_COUNT] = {
        X4_TR_CAT_HOME_CONTINUE, X4_TR_CAT_HOME_MINE, X4_TR_CAT_HOME_FREE,
        X4_TR_CAT_HOME_NEW, X4_TR_CAT_HOME_POPULAR, X4_TR_CAT_HOME_LEAVING
    };
    const X4HomeRow *row = &b->home_rows.rows[index];
    const unsigned first = x4_catalog_browser_home_row_start(b, index);
    const int hy = discover_row_y(visual, false), cy = discover_row_y(visual, true);
    catalog_text(p, "discover-row-heading", HOME_LEFT, hy, X4_UI2_HEADING, X4_FONT_SEMIBOLD,
        row->kind == X4_HOME_CONTINUE ? x4_tr(X4_TR_U49_HOME_CONTINUE) :
        row->kind == X4_HOME_POPULAR ?
            x4_tr(X4_TR_U49_POPULAR_CLOUD) : x4_tr(labels[row->kind]),
        1150, C_WHITE);
    if (first + X4_CATALOG_UI2_DISCOVER_VISIBLE < row->count)
        x4_apple_draw_icon(p, HOME_RIGHT - 34, hy, X4_ICON_CHEVRON_RIGHT, C_MUTED);
    const bool with_history = row->kind == X4_HOME_CONTINUE && b->history;
    const uint64_t now = with_history ? x4_history_now() : 0;
    for (unsigned i = 0; i < X4_CATALOG_UI2_DISCOVER_VISIBLE && first + i < row->count; ++i) {
        const unsigned source = row->indices[first + i];
        const HomeCard card = home_title_card(source < c->count ? &c->titles[source] : NULL);
        home_draw_card(p, &card, a, cy, i, HOME_DISCOVER_CARD_PITCH, false);
        if (with_history && card.product_id && *card.product_id)
            discover_continue_history(p, b->history, card.product_id, now,
                HOME_FULL_CARD_X + (int)i * HOME_DISCOVER_CARD_PITCH, cy);
    }
}

typedef struct {
    enum X4TextId name, caption;
    enum X4TablerIconId icon;
    uint32_t surface;
} CollectionStyle;

static const CollectionStyle collection_styles[X4_UI2_COLLECTION_COUNT] = {
    [X4_UI2_COLLECTION_MULTIPLAYER] = {X4_TR_U49_COLLECTION_MULTIPLAYER_NAME, X4_TR_U49_COLLECTION_MULTIPLAYER_CAPTION, X4_ICON_DEVICE_GAMEPAD, X4_RGB(59, 39, 88)},
    [X4_UI2_COLLECTION_OPEN_WORLD] = {X4_TR_U49_COLLECTION_OPEN_WORLD_NAME, X4_TR_U49_COLLECTION_OPEN_WORLD_CAPTION, X4_ICON_WORLD, X4_RGB(19, 63, 54)},
    [X4_UI2_COLLECTION_SPORTS] = {X4_TR_U49_COLLECTION_SPORTS_NAME, X4_TR_U49_COLLECTION_SPORTS_CAPTION, X4_ICON_DEVICE_GAMEPAD, X4_RGB(82, 51, 31)},
    [X4_UI2_COLLECTION_RACING] = {X4_TR_U49_COLLECTION_RACING_NAME, X4_TR_U49_COLLECTION_RACING_CAPTION, X4_ICON_DEVICE_GAMEPAD, X4_RGB(30, 55, 86)},
    [X4_UI2_COLLECTION_SHOOTER] = {X4_TR_U49_COLLECTION_SHOOTER_NAME, X4_TR_U49_COLLECTION_SHOOTER_CAPTION, X4_ICON_DEVICE_GAMEPAD, X4_RGB(83, 41, 63)},
    [X4_UI2_COLLECTION_INDIE] = {X4_TR_U49_COLLECTION_INDIE_NAME, X4_TR_U49_COLLECTION_INDIE_CAPTION, X4_ICON_HEART, X4_RGB(48, 46, 87)},
};

static void discover_collection_icon(uint32_t *p, int x, int y, X4Ui2CollectionKind kind, uint32_t color)
{
    if (kind == X4_UI2_COLLECTION_RACING) {
        x4_rect(p, x + 4, y + 3, 2, 18, color);
        x4_apple_stroke_round_rect(p, x + 6, y + 3, 15, 12, 1, 1.0f, color);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                if (!((row + col) & 1)) x4_rect(p, x + 7 + 4 * col, y + 4 + 5 * row, 4, 5, color);
    } else if (kind == X4_UI2_COLLECTION_SHOOTER) {
        x4_apple_stroke_round_rect(p, x + 4, y + 4, 16, 16, 8, 1.5f, color);
        x4_rect(p, x + 11, y + 2, 2, 7, color);
        x4_rect(p, x + 11, y + 15, 2, 7, color);
        x4_rect(p, x + 2, y + 11, 7, 2, color);
        x4_rect(p, x + 15, y + 11, 7, 2, color);
    } else if (kind == X4_UI2_COLLECTION_SPORTS) {
        x4_apple_stroke_round_rect(p, x + 3, y + 3, 18, 18, 9, 1.5f, color);
        x4_apple_fill_round_rect(p, x + 9, y + 9, 6, 6, 3, color);
        x4_rect(p, x + 11, y + 4, 2, 5, color);
        x4_rect(p, x + 11, y + 15, 2, 5, color);
        x4_rect(p, x + 4, y + 11, 5, 2, color);
        x4_rect(p, x + 15, y + 11, 5, 2, color);
    } else {
        enum X4TablerIconId icon = kind > X4_UI2_COLLECTION_NONE && kind < X4_UI2_COLLECTION_COUNT ?
            collection_styles[kind].icon : X4_ICON_DEVICE_GAMEPAD;
        x4_apple_draw_icon(p, x, y, icon, color);
    }
}

static const char *collection_name(X4Ui2CollectionKind kind)
{
    if (kind <= X4_UI2_COLLECTION_NONE || kind >= X4_UI2_COLLECTION_COUNT) return "";
    const CollectionStyle *style = &collection_styles[kind];
    return x4_tr(style->name);
}

typedef struct {
    uint32_t pixels[DISCOVER_COLLECTION_W * DISCOVER_COLLECTION_CARD_H];
    const uint32_t *source;
    uint64_t revision;
    char product[X4_PRODUCT_ID_CAP];
} CollectionArt;

static CollectionArt collection_art[X4_UI2_COLLECTION_COUNT - 1];

static bool draw_collection_art(uint32_t *p, int x, int y, X4Ui2CollectionKind kind,
    const X4CatalogSnapshot *c, unsigned source, X4CatalogAssets *assets)
{
    if (!assets || source >= c->count || !c->titles[source].product_id[0]) return false;
    const char *id = c->titles[source].product_id;
    X4CatalogAssetView view;
    if (!x4_catalog_assets_acquire(assets, id, &view)) return false;
    const X4CoverRaster *raster = &view.surface[X4_COVER_ROW];
    const bool ready = raster->rgba && raster->width == HOME_CARD_W &&
        raster->height == HOME_SOURCE_CARD_H;
    bool drawn = false;
    if (ready) {
        CollectionArt *entry = &collection_art[(unsigned)kind - 1];
        if (entry->revision != tile_revision || entry->source != raster->rgba ||
            strcmp(entry->product, id)) {
            const uint32_t color = collection_styles[kind].surface;
            for (unsigned row = 0; row < DISCOVER_COLLECTION_CARD_H; ++row) {
                const uint32_t *from = raster->rgba + (size_t)(row * HOME_SOURCE_CARD_H /
                    DISCOVER_COLLECTION_CARD_H) * HOME_CARD_W;
                uint32_t *to = entry->pixels + (size_t)row * DISCOVER_COLLECTION_W;
                for (unsigned col = 0; col < DISCOVER_COLLECTION_W; ++col) {
                    /* Color and stronger bottom shading are baked into the
                     * retained thumbnail. Sampling always uses the full art. */
                    unsigned opacity = row < 54u ? 148u + 90u * row / 54u : 238u;
                    to[col] = discover_mix(color, from[col * HOME_CARD_W /
                        DISCOVER_COLLECTION_W], 255u - opacity);
                }
            }
            entry->revision = tile_revision;
            entry->source = raster->rgba;
            snprintf(entry->product, sizeof(entry->product), "%s", id);
        }
#ifdef X4_UI48_CAPTURE
        x4_ui48_capture_cover((X4UiRect){x, y, DISCOVER_COLLECTION_W, DISCOVER_COLLECTION_CARD_H}, raster);
#endif
        const X4CoverRaster fitted = {DISCOVER_COLLECTION_W, DISCOVER_COLLECTION_CARD_H, entry->pixels};
        drawn = x4_apple_blit_image_exact(p, x, y, DISCOVER_COLLECTION_W,
            DISCOVER_COLLECTION_CARD_H, 12, W, &fitted);
    }
    x4_catalog_assets_release(assets, &view);
    return drawn;
}

static void draw_collections(uint32_t *p, const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    X4CatalogAssets *assets, unsigned visual)
{
    catalog_text(p, "collections-heading", HOME_LEFT, discover_row_y(visual, false), 26,
        X4_FONT_SEMIBOLD, x4_tr(X4_TR_U49_COLLECTIONS), 1120, C_WHITE);
    for (unsigned i = 0; i < b->ui2_collection_count && i < X4_UI2_COLLECTION_COUNT - 1u; ++i) {
        const X4Ui2Collection collection = b->ui2_collections[i];
        const X4Ui2CollectionKind kind = collection.kind;
        if (kind <= X4_UI2_COLLECTION_NONE || kind >= X4_UI2_COLLECTION_COUNT) continue;
        const CollectionStyle *style = &collection_styles[kind];
        const int x = HOME_FULL_CARD_X + (int)i * DISCOVER_COLLECTION_PITCH;
        const int y = discover_row_y(visual, true);
        const int width = DISCOVER_COLLECTION_W, height = DISCOVER_COLLECTION_CARD_H;
        if (!draw_collection_art(p, x, y, kind, c, collection.source, assets))
            x4_apple_fill_round_rect(p, x, y, width, height, 12, style->surface);
        x4_apple_stroke_round_rect(p, x, y, width, height, 12, 1.0f, X4_RGBA(255, 255, 255, 58));
        discover_collection_icon(p, x + 15, y + 15, kind, C_SOFT);
        catalog_text(p, "collection-title", x + 15, y + 54, 22, X4_FONT_SEMIBOLD,
            collection_name(kind), width - 30, C_WHITE);
        catalog_text(p, "collection-subtitle", x + 15, y + 84, 18, X4_FONT_REGULAR,
            x4_tr(style->caption), width - 30, C_SOFT);
    }
}

static X4UiRect collection_focus_rect(const X4CatalogBrowser *b, unsigned visual)
{
    if (b->ui2_collection_selected >= b->ui2_collection_count ||
        b->ui2_collection_selected >= X4_UI2_COLLECTION_COUNT - 1u) return (X4UiRect){0};
    return (X4UiRect){
        HOME_FULL_CARD_X + (int)b->ui2_collection_selected * DISCOVER_COLLECTION_PITCH - HOME_FOCUS_PAD,
        discover_row_y(visual, true) - HOME_FOCUS_PAD,
        DISCOVER_COLLECTION_W + 2 * HOME_FOCUS_PAD, DISCOVER_COLLECTION_CARD_H + 2 * HOME_FOCUS_PAD,
    };
}

enum {
    TOOLS_LEFT = HOME_FULL_CARD_X,
    SELECTOR_Y = 241, SELECTOR_W = 338, SELECTOR_GAP = 20, SELECTOR_H = 56,
    SELECTOR_POP_Y = SELECTOR_Y + SELECTOR_H + 9, SELECTOR_ITEM_H = 33,
    SELECTOR_VISIBLE = 4,
    LIBRARY_TAB_Y = 172, LIBRARY_TAB_W = 206, LIBRARY_TAB_H = 48,
    LIBRARY_CONTROL_H = 42, LIBRARY_SORT_X = HOME_RIGHT - 702,
    LIBRARY_SORT_W = 340, LIBRARY_GENRE_X = HOME_RIGHT - 342, LIBRARY_GENRE_W = 342,
};
_Static_assert(SELECTOR_POP_Y + SELECTOR_VISIBLE * SELECTOR_ITEM_H + 12 <
    CATALOG_SECOND_Y - HOME_FOCUS_PAD, "drop-down spans at most the first row");

static void selector_value(const X4CatalogBrowser *b, X4Ui2CatalogFocus kind, unsigned value,
    char label[112])
{
    static const enum X4TextId access_labels[X4_CATALOG_FILTER_COUNT] = {
        X4_TR_CAT_FILTER_ALL, X4_TR_CAT_FILTER_ENTITLED, X4_TR_CAT_FILTER_UNCONFIRMED
    };
    if (kind == X4_UI2_CATALOG_SORT) {
        const char *name = value == X4_UI2_SORT_NEW ? x4_tr(X4_TR_U49_SORT_NEW) :
            value == X4_UI2_SORT_AZ ? "A-Z" : x4_tr(X4_TR_U49_SORT_FEATURED);
        snprintf(label, 112, "%s", name);
        return;
    }
    unsigned count;
    const char *name;
    if (kind == X4_UI2_CATALOG_ACCESS) {
        name = value == X4_CATALOG_FILTER_FAVORITES ? x4_tr(X4_TR_U49_FAVORITES) :
            x4_tr(access_labels[value]);
        count = b->ui2_access_counts[value];
    } else {
        unsigned at = 0;
        X4Ui2CollectionKind collection = x4_catalog_browser_ui2_genre_option(b, value, &at);
        if (collection != X4_UI2_COLLECTION_NONE) {
            name = collection_name(collection);
            count = collection == X4_UI2_COLLECTION_MULTIPLAYER ? b->ui2_multiplayer_count :
                b->ui2_collection_filter_count;
        } else {
            name = at ? x4_lang_store_category(b->ui2_genres[at].label) : x4_tr(X4_TR_CAT_FILTER_ALL);
            count = b->ui2_genres[at].count;
        }
    }
    snprintf(label, 112, x4_tr(X4_TR_50_CATALOG_FILTER_COUNT_FMT), name, count);
}

typedef enum { SELECTOR_NO_ICON, SELECTOR_SORT_ICON, SELECTOR_FILTER_ICON } SelectorIcon;

static void selector_box(uint32_t *p, int x, int y, int w, int h, bool focused, SelectorIcon icon)
{
    x4_ui2_card(p, x, y, w, h, X4_UI2_RADIUS_CONTROL);
    if (focused) x4_ui2_focus(p, x, y, w, h, X4_UI2_RADIUS_CONTROL);
    const int arrow_x = x + w - 28, arrow_y = y + h / 2;
    for (int step = 0; step < 5; ++step) {
        x4_rect(p, arrow_x - 4 + step, arrow_y + step, 2, 2, C_MUTED);
        x4_rect(p, arrow_x + 4 - step, arrow_y + step, 2, 2, C_MUTED);
    }
    const int ix = x + 14, iy = y + (h - 20) / 2;
    if (icon == SELECTOR_FILTER_ICON) {
        x4_rect(p, ix + 2, iy + 3, 19, 2, C_SOFT);
        for (int step = 0; step < 7; ++step) {
            x4_rect(p, ix + 2 + step, iy + 5 + step, 2, 2, C_SOFT);
            x4_rect(p, ix + 19 - step, iy + 5 + step, 2, 2, C_SOFT);
        }
        x4_rect(p, ix + 8, iy + 12, 2, 7, C_SOFT);
        x4_rect(p, ix + 13, iy + 12, 2, 5, C_SOFT);
        x4_rect(p, ix + 9, iy + 17, 5, 2, C_SOFT);
    } else if (icon == SELECTOR_SORT_ICON) {
        x4_rect(p, ix, iy + 4, 12, 2, C_SOFT);
        x4_rect(p, ix, iy + 9, 9, 2, C_SOFT);
        x4_rect(p, ix, iy + 14, 6, 2, C_SOFT);
        x4_rect(p, ix + 16, iy + 3, 2, 13, C_SOFT);
        for (int step = 0; step < 4; ++step) {
            x4_rect(p, ix + 13 + step, iy + 13 + step, 2, 2, C_SOFT);
            x4_rect(p, ix + 19 - step, iy + 13 + step, 2, 2, C_SOFT);
        }
    }
}

static void draw_catalog_selectors(uint32_t *p, const X4CatalogBrowser *b, bool top, bool popup)
{
    const char *labels[] = {
        x4_tr(X4_TR_U49_GENRE), x4_tr(X4_TR_U49_ACCESS),
        x4_tr(X4_TR_U49_SORT_BY),
    };
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        const X4Ui2CatalogFocus kind = (X4Ui2CatalogFocus)(X4_UI2_CATALOG_GENRES + i);
        const int x = TOOLS_LEFT + (int)i * (SELECTOR_W + SELECTOR_GAP);
        const unsigned count = x4_catalog_browser_ui2_selector_count(b, kind);
        unsigned selected = x4_catalog_browser_ui2_selector_value(b, kind);
        if (selected >= count) selected = 0;
        const bool focused = !top && kind == b->ui2_content_focus;
        char value[112];
        if (!popup) {
            selector_box(p, x, SELECTOR_Y, SELECTOR_W, SELECTOR_H,
                focused && !b->ui2_selector_open, SELECTOR_NO_ICON);
            catalog_text(p, "catalog-selector-label", x + 15, SELECTOR_Y + 4, 18, X4_FONT_SEMIBOLD,
                labels[i], SELECTOR_W - 65, C_MUTED);
            selector_value(b, kind, selected, value);
            catalog_text(p, "catalog-selector-value", x + 15, SELECTOR_Y + 28, 20, X4_FONT_SEMIBOLD,
                value, SELECTOR_W - 65, C_WHITE);
            continue;
        }
        if (!focused || !count) continue;
        unsigned cursor = b->ui2_selector_cursor < count ? b->ui2_selector_cursor : count - 1;
        unsigned shown = count < SELECTOR_VISIBLE ? count : SELECTOR_VISIBLE;
        unsigned first = cursor < shown ? 0 : cursor + 1 - shown;
        x4_ui2_card(p, x, SELECTOR_POP_Y, SELECTOR_W, (int)shown * SELECTOR_ITEM_H + 12, 14);
        for (unsigned n = 0; n < shown; ++n) {
            const unsigned index = first + n;
            const int y = SELECTOR_POP_Y + 6 + (int)n * SELECTOR_ITEM_H;
            if (index == cursor) {
                x4_apple_fill_round_rect(p, x + 8, y + 1, SELECTOR_W - 16,
                    SELECTOR_ITEM_H - 2, 9, X4_UI2_ACTIVE);
                x4_ui2_focus(p, x + 8, y + 1, SELECTOR_W - 16, SELECTOR_ITEM_H - 2, 9);
            }
            selector_value(b, kind, index, value);
            catalog_text(p, "catalog-dropdown-option", x + 20, y + 6, 20, X4_FONT_SEMIBOLD,
                value, SELECTOR_W - 60, index == selected ? C_GREEN : C_WHITE);
        }
    }
}

static void draw_library_tabs(uint32_t *p, const X4CatalogBrowser *b, const HomeTab *tab, bool top)
{
    const char *labels[] = {x4_tr(X4_TR_U49_LIBRARY), x4_tr(X4_TR_U49_FAVORITES)};
    for (unsigned i = 0; i < 2; ++i) {
        const bool active = tab->favorite_cards == (i == 1);
        const int x = TOOLS_LEFT + (int)i * LIBRARY_TAB_W;
        const int width = x4_text_width_px(26, X4_FONT_SEMIBOLD, labels[i]);
        if (active && !top && b->ui2_library_tabs)
            x4_ui2_focus(p, x, LIBRARY_TAB_Y, LIBRARY_TAB_W - 20, LIBRARY_TAB_H, 12);
        catalog_text(p, "library-tab", x + 12, LIBRARY_TAB_Y + 6, 26, X4_FONT_SEMIBOLD,
            labels[i], LIBRARY_TAB_W - 44, active ? C_GREEN : C_MUTED);
        if (active) x4_rect(p, x + 12, LIBRARY_TAB_Y + 41, width, 3, C_GREEN);
    }
}

static const char *library_tab_label(X4Ui2LibraryTab tab)
{
    if (tab == X4_UI2_LIBRARY_FAVORITES) return x4_tr(X4_TR_U49_FAVORITES);
    if (tab == X4_UI2_LIBRARY_RECENT) return x4_tr(X4_TR_U49_RECENTLY_PLAYED);
    return x4_tr(X4_TR_U49_LIBRARY);
}

static const char *library_sort_label(X4Ui2LibrarySort sort)
{
    switch (sort) {
    case X4_UI2_LIBRARY_ALPHABETIC: return "A-Z";
    case X4_UI2_LIBRARY_PLAYED: return x4_tr(X4_TR_U49_SORT_MOST_PLAYED);
    case X4_UI2_LIBRARY_FAVORITE_FIRST: return x4_tr(X4_TR_U49_SORT_FAVORITES_FIRST);
    default: return x4_tr(X4_TR_U49_SORT_RECENT);
    }
}

static void library_control(uint32_t *p, int x, int y, int w, const char *label, bool active,
    bool focused, enum X4TablerIconId icon)
{
    if (active)
        x4_apple_fill_round_rect(p, x, y, w, LIBRARY_CONTROL_H, X4_UI2_RADIUS_CONTROL, X4_UI2_ACTIVE);
    else x4_ui2_card(p, x, y, w, LIBRARY_CONTROL_H, X4_UI2_RADIUS_CONTROL);
    if (focused) x4_ui2_focus(p, x, y, w, LIBRARY_CONTROL_H, X4_UI2_RADIUS_CONTROL);
    if (icon != X4_ICON_NONE)
        x4_apple_draw_icon(p, x + 14, y + 9, icon, active ? C_GREEN : C_SOFT);
    const int offset = icon != X4_ICON_NONE ? 49 : 17;
    catalog_text(p, "library-control", x + offset, y + 10, 20, X4_FONT_SEMIBOLD,
        label, w - offset - 16, C_WHITE);
}

static int library_controls_y(const HomeTab *tab)
{
    return tab->shelf ? LIBRARY_SHELF_SPLIT + 19 : 241;
}

static void draw_library3_controls(uint32_t *p, const X4CatalogBrowser *b, const HomeTab *tab, bool top)
{
    const X4Ui2Library *l = &b->library;
    const int y = library_controls_y(tab);
    catalog_text(p, "library-grid-heading", TOOLS_LEFT, y + 5, X4_UI2_HEADING, X4_FONT_BOLD,
        library_tab_label(l->tab), 600, C_WHITE);
    char sort[80];
    snprintf(sort, sizeof(sort), "%s: %s", x4_tr(X4_TR_U49_SORT_BY), library_sort_label(l->sort));
    const char *genre = x4_tr(X4_TR_U49_FILTER);
    if (l->genre && l->genre < X4_UI2_GENRE_MAX && b->ui2_genre_names[l->genre][0])
        genre = x4_lang_store_category(b->ui2_genre_names[l->genre]);
    selector_box(p, LIBRARY_SORT_X, y, LIBRARY_SORT_W, LIBRARY_CONTROL_H,
        !top && !l->popup && l->focus == X4_UI2_LIBRARY_SORT, SELECTOR_SORT_ICON);
    selector_box(p, LIBRARY_GENRE_X, y, LIBRARY_GENRE_W, LIBRARY_CONTROL_H,
        !top && !l->popup && l->focus == X4_UI2_LIBRARY_GENRE, SELECTOR_FILTER_ICON);
    catalog_text(p, "library-control", LIBRARY_SORT_X + 49, y + 10, 20, X4_FONT_SEMIBOLD,
        sort, LIBRARY_SORT_W - 90, C_WHITE);
    catalog_text(p, "library-control", LIBRARY_GENRE_X + 49, y + 10, 20, X4_FONT_SEMIBOLD,
        genre, LIBRARY_GENRE_W - 90, C_WHITE);
    const unsigned page = tab->shelf ? tab->columns : tab->page;
    if (l->grid_count > page) {
        char count[64];
        snprintf(count, sizeof(count), "%s %u / %u", x4_tr(X4_TR_U49_PAGE),
            l->grid_selected / page + 1u, (l->grid_count + page - 1u) / page);
        int width = x4_text_width_px(18, X4_FONT_SEMIBOLD, count);
        catalog_text(p, "library-page", HOME_RIGHT - width, y + 50, 18, X4_FONT_SEMIBOLD,
            count, width, C_MUTED);
    }
}

static void draw_library3_tabs(uint32_t *p, const X4CatalogBrowser *b, const HomeTab *tab, bool top)
{
    const X4Ui2Library *l = &b->library;
    catalog_text(p, "library-subtitle", TOOLS_LEFT, 126, X4_UI2_LABEL, X4_FONT_REGULAR,
        x4_tr(X4_TR_U49_LIBRARY_CLOUD_SUBTITLE),
        1120, C_MUTED);
    int x = TOOLS_LEFT;
    const unsigned counts[] = {l->members, l->favorite_count, l->recent_count};
    const int widths[] = {280, 225, 354};
    const enum X4TablerIconId icons[] = {X4_ICON_DEVICE_GAMEPAD, X4_ICON_HEART, X4_ICON_NONE};
    for (unsigned i = 0; i < X4_UI2_LIBRARY_TAB_COUNT; ++i) {
        char label[72];
        snprintf(label, sizeof(label), x4_tr(X4_TR_50_LIBRARY_TAB_COUNT_FMT),
            library_tab_label((X4Ui2LibraryTab)i), counts[i]);
        bool active = (unsigned)l->tab == i;
        library_control(p, x, LIBRARY_TAB_Y, widths[i], label, active,
            active && !top && l->focus == X4_UI2_LIBRARY_TABS, icons[i]);
        x += widths[i] + 18;
    }
    if (tab->shelf) {
        catalog_text(p, "library-continue-heading", TOOLS_LEFT, 252, X4_UI2_HEADING, X4_FONT_BOLD,
            x4_tr(X4_TR_U49_HOME_CONTINUE), 720, C_WHITE);
        library_control(p, HOME_RIGHT - 166, 245, 166, x4_tr(X4_TR_U49_SEE_ALL), false,
            !top && l->focus == X4_UI2_LIBRARY_SEE_ALL, X4_ICON_CHEVRON_RIGHT);
    } else {
        draw_library3_controls(p, b, tab, top);
    }
}

static void draw_library3_popup(uint32_t *p, const X4CatalogBrowser *b, const HomeTab *tab)
{
    const X4Ui2Library *l = &b->library;
    const bool sort = l->focus == X4_UI2_LIBRARY_SORT;
    int x = sort ? LIBRARY_SORT_X : LIBRARY_GENRE_X;
    int w = sort ? LIBRARY_SORT_W : LIBRARY_GENRE_W;
    int y = library_controls_y(tab) + LIBRARY_CONTROL_H + 7;
    const X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_selection(b);
    const unsigned count = l->manage ? 1u + card.favorite :
        sort ? X4_UI2_LIBRARY_SORT_COUNT : b->ui2_genre_count;
    if (!count) return;
    const unsigned shown = count < SELECTOR_VISIBLE ? count : SELECTOR_VISIBLE;
    const unsigned cursor = l->cursor < count ? l->cursor : 0;
    const unsigned first = cursor < shown ? 0 : cursor + 1u - shown;
    int body_offset = 6;
    if (l->manage) {
        w = 620;
        x = HOME_LEFT + (HOME_RIGHT - HOME_LEFT - w) / 2;
        y = 432;
        body_offset = 77;
    }
    x4_ui2_card(p, x, y, w, body_offset + (int)shown * SELECTOR_ITEM_H + 6, 14);
    if (l->manage) {
        catalog_text(p, "library-manage-title", x + 20, y + 15, 26, X4_FONT_SEMIBOLD,
            card.title ? card.title : "", w - 40, C_WHITE);
        catalog_text(p, "library-manage-label", x + 20, y + 48, 18, X4_FONT_REGULAR,
            x4_tr(X4_TR_U49_MANAGE), w - 40, C_MUTED);
    }
    for (unsigned i = 0; i < shown; ++i) {
        const unsigned value = first + i;
        const int row = y + body_offset + (int)i * SELECTOR_ITEM_H;
        if (value == cursor) {
            x4_apple_fill_round_rect(p, x + 8, row + 1, w - 16, SELECTOR_ITEM_H - 2, 9, X4_UI2_ACTIVE);
            x4_ui2_focus(p, x + 8, row + 1, w - 16, SELECTOR_ITEM_H - 2, 9);
        }
        const char *label = l->manage ? (value == 0 ? x4_tr(X4_TR_U49_VIEW_GAME_SHORT) :
            x4_tr(X4_TR_U49_REMOVE_FROM_FAVORITES)) :
            sort ? library_sort_label((X4Ui2LibrarySort)value) :
            value ? x4_lang_store_category(b->ui2_genres[value].label) : x4_tr(X4_TR_CAT_FILTER_ALL);
        catalog_text(p, "library-popup", x + 20, row + 7, 19, X4_FONT_SEMIBOLD, label, w - 40, C_WHITE);
    }
}

static void home_draw_tab_tools(uint32_t *p, const X4CatalogBrowser *b, const HomeTab *tab,
    const char *notice, bool top)
{
    if (tab->library) {
        if (tab->library3) {
            draw_library3_tabs(p, b, tab, top);
            return;
        }
        catalog_text(p, "library-subtitle", TOOLS_LEFT, 126, 24, X4_FONT_REGULAR,
            x4_tr(X4_TR_U49_LIBRARY_SUBTITLE),
            1120, C_MUTED);
        draw_library_tabs(p, b, tab, top);
        if (tab->count[0] > tab->page) {
            char page[56];
            snprintf(page, sizeof(page), "%s %u / %u", x4_tr(X4_TR_U49_PAGE),
                tab->first[0] / tab->page + 1, (tab->count[0] + tab->page - 1) / tab->page);
            const int width = x4_text_width_px(20, X4_FONT_SEMIBOLD, page);
            catalog_text(p, "library-page", HOME_RIGHT - width, 261, 20, X4_FONT_SEMIBOLD,
                page, width, C_MUTED);
        }
        return;
    }

    const char *subtitle = tab->search ?
        x4_tr(X4_TR_U49_SEARCH_SUBTITLE) :
        x4_tr(X4_TR_U49_CATALOG_SUBTITLE);
    catalog_text(p, "catalog-subtitle", TOOLS_LEFT, 126, 24, X4_FONT_REGULAR, subtitle, 1120, C_MUTED);

    const int x = TOOLS_LEFT, y = 165, width = tab->search ? 1128 : HOME_RIGHT - TOOLS_LEFT;
    const int search_h = tab->search ? 48 : 56;
    x4_ui2_card(p, x, y, width, search_h, 17);
    if (!top && b->ui2_content_focus == X4_UI2_CATALOG_SEARCH)
        x4_ui2_focus(p, x, y, width, search_h, 17);
    x4_apple_draw_icon(p, x + 18, y + (search_h - 24) / 2, X4_ICON_SEARCH, C_MUTED);
    catalog_text(p, "catalog-search-field", x + 62, y + (search_h - 22) / 2, 22, X4_FONT_REGULAR,
        b->query[0] ? b->query : x4_tr(X4_TR_U49_SEARCH_PLACEHOLDER),
        width - 126, b->query[0] ? C_WHITE : C_MUTED);
    x4_apple_draw_pad_glyph(p, x + width - 47, y + (search_h - 28) / 2, X4_PAD_TRIANGLE);

    char count[160];
    if (*notice) snprintf(count, sizeof(count), "%s", notice);
    else if (tab->search) snprintf(count, sizeof(count), "%u %s", tab->count[0],
        x4_tr_count(tab->count[0], X4_TR_U49_RESULT, X4_TR_U49_RESULTS));
    else snprintf(count, sizeof(count), "%u %s", tab->count[0],
        x4_tr_count(tab->count[0], X4_TR_U49_GAME, X4_TR_U49_GAMES));
    const int ink = x4_text_width_px(22, X4_FONT_SEMIBOLD, count);
    const int aligned = ink < 378 ? ink : 378;
    catalog_text(p, "catalog-results", HOME_RIGHT - aligned, tab->search ? y + 11 : SELECTOR_Y + 15, 22,
        X4_FONT_SEMIBOLD, count, aligned, C_MUTED);
    if (!tab->search) draw_catalog_selectors(p, b, top, false);
    if (tab->count[0] > tab->page) {
        char page[56];
        snprintf(page, sizeof(page), "%s %u / %u", x4_tr(X4_TR_U49_PAGE),
            tab->first[0] / tab->page + 1, (tab->count[0] + tab->page - 1) / tab->page);
        const int text_width = x4_text_width_px(20, X4_FONT_SEMIBOLD, page);
        catalog_text(p, "catalog-page", HOME_RIGHT - text_width, tab->search ? 268 : 307, 20,
            X4_FONT_SEMIBOLD, page, text_width, C_MUTED);
    }
    if (!tab->count[0]) {
        const char *message = tab->search && !b->query[0] ?
            x4_tr(X4_TR_U49_SEARCH_HINT) :
            x4_tr(X4_TR_CAT_NO_MATCH);
        catalog_text(p, "catalog-empty", TOOLS_LEFT, tab->search ? 264 : 407, 26,
            X4_FONT_SEMIBOLD, message, 750, C_WHITE);
    }
}

static void discover_genre_chip(uint32_t *p, int x, const char *label,
    X4Ui2CollectionKind kind, int width)
{
    const int y = DISCOVER_GENRE_Y, height = DISCOVER_GENRE_H;
    x4_ui2_card(p, x, y, width, height, X4_UI2_RADIUS_CONTROL);
    discover_collection_icon(p, x + 12, y + 7, kind, C_SOFT);
    catalog_text(p, "hero-genre", x + 45, y + 8, 20, X4_FONT_SEMIBOLD,
        label, width - 55, C_WHITE);
}

static void discover_draw_genres(uint32_t *p, const X4CatalogBrowser *b, unsigned source,
    const char *fallback)
{
    const char *labels[3] = {0};
    X4Ui2CollectionKind kinds[3];
    unsigned count = 0;
    const unsigned attributes = source < X4_CATALOG_MAX ? b->ui2_attributes[source] : 0;
    const uint32_t mask = source < X4_CATALOG_MAX ? b->ui2_genre_masks[source] : 0;
    if ((attributes & X4_GAME_ATTR_OPEN_WORLD) &&
        !(mask & b->ui2_collection_genres[X4_UI2_COLLECTION_OPEN_WORLD])) {
        labels[count] = collection_name(X4_UI2_COLLECTION_OPEN_WORLD);
        kinds[count++] = X4_UI2_COLLECTION_OPEN_WORLD;
    }
    for (unsigned slot = 0; slot < b->ui2_genre_sources && slot < X4_UI2_GENRE_MAX && count < 3; ++slot) {
        const uint32_t bit = UINT32_C(1) << slot;
        if (!(mask & bit) || !b->ui2_genre_names[slot][0]) continue;
        X4Ui2CollectionKind kind = X4_UI2_COLLECTION_NONE;
        for (unsigned i = 1; i < X4_UI2_COLLECTION_COUNT; ++i)
            if (b->ui2_collection_genres[i] & bit) {
                kind = (X4Ui2CollectionKind)i;
                break;
            }
        labels[count] = kind ? collection_name(kind) : x4_lang_store_category(b->ui2_genre_names[slot]);
        kinds[count++] = kind;
    }
    if (!count && fallback[0]) {
        labels[count] = fallback;
        kinds[count++] = X4_UI2_COLLECTION_NONE;
    }
    int x = HOME_LEFT;
    for (unsigned i = 0; i < count; ++i) {
        int width = x4_text_width_px(20, X4_FONT_SEMIBOLD, labels[i]) + 58;
        if (width < 142) width = 142;
        if (width > 320) width = 320;
        discover_genre_chip(p, x, labels[i], kinds[i], width);
        x += width + 12;
    }
}

static void discover_caret(uint32_t *p, int x, int y, bool next)
{
    for (int i = -5; i <= 5; ++i) {
        const int distance = 5 - abs(i);
        x4_rect(p, x + (next ? distance : -distance), y + i, 2, 1, C_SOFT);
    }
}

static void discover_draw_dots(uint32_t *p, const X4CatalogBrowser *b)
{
    const unsigned count = b->featured_count <= X4_CATALOG_FEATURED_MAX ? b->featured_count : 0;
    if (count < 2) return;
    const int width = (int)count * 20 + 68, x = HOME_RIGHT - width;
    x4_apple_fill_round_rect(p, x, 374, width, 38, 19, X4_RGBA(9, 16, 27, 193));
    x4_apple_stroke_round_rect(p, x, 374, width, 38, 19, 1.0f, X4_UI2_BORDER);
    discover_caret(p, x + 21, 393, false);
    discover_caret(p, HOME_RIGHT - 24, 393, true);
    for (unsigned i = 0; i < count; ++i)
        x4_apple_fill_round_rect(p, x + 39 + (int)i * 20, 388, 10, 10, 5,
            i == b->featured_index ? C_GREEN : C_SOFT);
}

static void home_draw_feature(uint32_t *p, const X4CatalogBrowser *b, const HomeCard *hero,
    unsigned source, X4CatalogAssets *a, const X4CatalogGuiStatus *s, bool favorite, unsigned focus)
{
    home.play_tile = (X4UiRect){0};
    if (!hero->id) return;
    X4CatalogAssetView view;
    const bool acquired = a && hero->product_id[0] && x4_catalog_assets_acquire(a, hero->product_id, &view);
    const char *name = acquired && view.metadata.name[0] ? view.metadata.name :
        (hero->name[0] ? hero->name : hero->id);
    const char *category = acquired ? x4_lang_store_category(view.metadata.category) : "";
    const char *publisher = acquired ? view.metadata.publisher : "";
    char desc[512];
    sanitize_single_line(acquired ? view.metadata.description : "", desc, sizeof(desc));
    const HomeHeroText text = home_draw_hero_text(p, name, publisher, desc);
    const bool waiting = s && (s->busy || s->pending);
    const char *play = s && s->pending ? x4_tr(X4_TR_CAT_PREPARING) :
        waiting ? x4_tr(X4_TR_CAT_QUERYING) : x4_tr(X4_TR_APPLE_HERO_PLAY_NOW);
    discover_draw_button(p, play, false, false, waiting);
    const bool can_favorite = hero->product_id[0] != 0;
    if (can_favorite) {
        const char *label = favorite ? x4_tr(X4_TR_U49_REMOVE_FROM_MY_GAMES) :
            x4_tr(X4_TR_U49_ADD_TO_MY_GAMES);
        discover_draw_button(p, label, true, favorite, false);
    }
    home.play_tile = (X4UiRect){HOME_LEFT - HOME_FOCUS_PAD, HOME_PLAY_Y - HOME_FOCUS_PAD,
        can_favorite ? HOME_PLAY_TILE_W : HOME_PLAY_W + 2 * HOME_FOCUS_PAD, HOME_PLAY_TILE_H};
    home_tile_save(home_play_pixels, p, home.play_tile);
    discover_button_focus(p, focus != 2 || can_favorite ? focus : 0);
    discover_draw_genres(p, b, source, category);
    discover_draw_dots(p, b);
#ifdef X4_CATALOG_GUI_TEST
    home_title_lines = text.title_lines;
    home_description_ellipsis = text.ellipsis;
#else
    (void)text;
#endif
    if (acquired) x4_catalog_assets_release(a, &view);
}

static uint64_t hash_word(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    const unsigned char *byte = (const unsigned char *)text;
    do { hash = hash_word(hash, *byte); } while (*byte++);
    return hash;
}

static uint64_t home_tab_row_signature(const HomeTab *tab, const X4CatalogSnapshot *c,
    const X4CatalogBrowser *b, const X4RecentGames *r, unsigned row)
{
    uint64_t hash = hash_word(UINT64_C(14695981039346656037), tab->first[row]);
    hash = hash_word(hash, tab->count[row]);
    if (tab->library) {
        hash = hash_word(hash, tab->favorite_cards);
        if (!row && !tab->favorite_cards) hash = hash_word(hash, tab->recent_state);
    }
    if (tab->library3) {
        hash = hash_word(hash, b->library.tab);
        hash = hash_word(hash, b->library.history_revision);
        for (unsigned i = 0; i < tab->columns; ++i) {
            const X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_card(b,
                tab->shelf && !row, tab->first[row] + i);
            if (!card.title) break;
            hash = hash_text(hash, card.product_id);
            hash = hash_text(hash, card.title_id);
            hash = hash_text(hash, card.title);
            hash = hash_word(hash, card.favorite | ((unsigned)(card.source < c->count) << 1));
        }
        return hash;
    }
    for (unsigned i = 0; i < tab->columns; ++i) {
        const HomeCard card = home_tab_card(tab, c, b, r, row, tab->first[row] + i);
        if (!card.id) break;
        hash = hash_text(hash, card.id);
        hash = hash_text(hash, card.product_id);
        hash = hash_text(hash, card.name);
        hash = hash_word(hash, card.available | ((unsigned)card.entitled << 1));
    }
    return hash;
}

static uint64_t home_tab_tools_key(const HomeTab *tab, const X4CatalogBrowser *b,
    const char *notice, bool top)
{
    uint64_t hash = hash_word(UINT64_C(14695981039346656037), tab->count[0]);
    hash = hash_word(hash, tab->first[0]);
    if (tab->library) {
        if (tab->library3) {
            const X4Ui2Library *l = &b->library;
            hash = hash_word(hash, l->tab);
            hash = hash_word(hash, l->members);
            hash = hash_word(hash, l->favorite_count);
            hash = hash_word(hash, l->recent_count);
            hash = hash_word(hash, l->grid_count);
            hash = hash_word(hash, l->grid_selected / (tab->shelf ? tab->columns : tab->page));
            hash = hash_word(hash, l->sort);
            hash = hash_word(hash, l->genre);
            hash = hash_word(hash, top || l->focus == X4_UI2_LIBRARY_CONTINUE ? X4_UI2_LIBRARY_GRID : l->focus);
            hash = hash_word(hash, l->popup | ((unsigned)l->manage << 1));
            if (l->popup || l->manage) hash = hash_word(hash, l->cursor);
            return hash;
        }
        hash = hash_word(hash, tab->favorite_cards);
        return hash_word(hash, !top && b->ui2_library_tabs);
    }
    hash = hash_word(hash, top ? X4_UI2_CATALOG_GRID : b->ui2_content_focus);
    hash = hash_text(hash_text(hash, b->query), notice);
    if (tab->search) return hash;
    hash = hash_word(hash, b->filter);
    hash = hash_word(hash, b->ui2_genre);
    hash = hash_word(hash, b->ui2_sort);
    hash = hash_word(hash, b->ui2_new.count);
    hash = hash_word(hash, b->ui2_selector_open);
    if (b->ui2_selector_open) hash = hash_word(hash, b->ui2_selector_cursor);
    hash = hash_word(hash, b->ui2_collection_filter);
    hash = hash_word(hash, b->ui2_collection_filter_count);
    hash = hash_word(hash, b->ui2_multiplayer_available);
    hash = hash_word(hash, b->ui2_multiplayer_count);
    for (unsigned i = 0; i < X4_CATALOG_FILTER_COUNT; ++i)
        hash = hash_word(hash, b->ui2_enabled ? b->ui2_access_counts[i] : b->group_counts[i]);
    for (unsigned i = 0; i < b->ui2_genre_count && i < X4_UI2_GENRE_MAX; ++i) {
        hash = hash_text(hash, b->ui2_genres[i].label);
        hash = hash_word(hash, b->ui2_genres[i].count);
    }
    return hash;
}

static uint64_t collections_key(const X4CatalogBrowser *b)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < b->ui2_collection_count && i < X4_UI2_COLLECTION_COUNT - 1u; ++i) {
        hash = hash_word(hash, b->ui2_collections[i].kind);
        hash = hash_word(hash, b->ui2_collections[i].source);
    }
    return hash;
}

static void home_draw_footer(uint32_t *p, const X4CatalogBrowser *b, const X4CatalogGuiStatus *s)
{
#ifdef X4_UI46_CAPTURE
    x4_ui46_capture_footer(p, HOME_LEFT, X4_UI_FOOTER_Y - 6,
        HOME_RIGHT - HOME_LEFT, X4_UI_SAFE_BOTTOM - X4_UI_FOOTER_Y + 6, false);
#endif
    const X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_selection(b);
    const bool manage = b->active_tab == X4_CAT_TAB_RECENTS && b->library.manage;
    const bool library = b->active_tab == X4_CAT_TAB_RECENTS && card.product_id &&
        card.product_id[0] && !manage && !sidebar_focused;
    X4Ui2Hint hints[5] = {{X4_GLYPH_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)}};
    size_t count = 1;
    if (library) {
        hints[count++] = (X4Ui2Hint){X4_GLYPH_SQUARE, x4_tr(X4_TR_U49_MANAGE)};
        hints[count++] = (X4Ui2Hint){X4_GLYPH_TRIANGLE,
            card.favorite ? x4_tr(X4_TR_U49_REMOVE_FAVORITE) :
                x4_tr(X4_TR_U49_ADD_TO_FAVORITES)};
    } else if (!manage) {
        hints[count++] = (X4Ui2Hint){X4_GLYPH_TRIANGLE, x4_tr(X4_TR_APPLE_HINT_SEARCH)};
    }
    hints[count++] = (X4Ui2Hint){X4_GLYPH_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)};
    hints[count++] = (X4Ui2Hint){X4_GLYPH_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)};
    x4_ui2_footer(p, hints, count);
    const char *label = x4_tr(s && s->connected ? X4_TR_CAT_DS4_CONNECTED : X4_TR_CAT_DS4_CONNECT);
    uint32_t color = C_SOFT;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE && !X4_PUBLIC
    char diagnostic[32];
    const X4StreamRequestConfig current = x4_stream_settings_get();
    if (x4_stream_settings_conversion_backend()) label = x4_tr(X4_TR_U49_GPU);
    else {
        snprintf(diagnostic, sizeof(diagnostic), x4_tr(X4_TR_U49_CPU_FMT), current.cpu_variant);
        label = diagnostic;
    }
#endif
    X4AccessCountryUi country;
    X4AccessCountryCardPhase phase;
    x4_access_country_card_get(&country, &phase);
    char success[160];
    if (phase == X4_ACCESS_CARD_SUCCESS) {
        snprintf(success, sizeof(success), x4_tr(X4_TR_ACCESS_SUCCESS_FMT),
            x4_access_country_label(country.selected));
        label = success;
        color = C_GREEN;
    }
    txt(p, HOME_LEFT, X4_UI_FOOTER_Y + 2, 20, X4_FONT_REGULAR, label, 510, color);
#ifdef X4_UI46_CAPTURE
    x4_ui46_capture_footer(p, HOME_LEFT, X4_UI_FOOTER_Y - 6,
        HOME_RIGHT - HOME_LEFT, X4_UI_SAFE_BOTTOM - X4_UI_FOOTER_Y + 6, true);
#endif
}

static bool focus_changed(X4UiRect old, X4UiRect next)
{
    return old.x != next.x || old.y != next.y || old.w != next.w;
}

static void draw_home(uint32_t *p, const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    const X4RecentGames *r, X4CatalogAssets *a, const X4CatalogGuiStatus *s, const HomeTab *tab)
{
    if (home.canvas != p || home.browser != b || home.assets != a) x4_catalog_gui_home_invalidate();
    const bool collections = !tab && discover_has_collections(b);
    const X4CatalogTitle *discover_hero = tab ? NULL : x4_catalog_browser_ui2_hero(b, c);
    if (!tab && !discover_hero) discover_hero = x4_catalog_browser_selected_title(b, c, r);
    const HomeCard hero = home_title_card(discover_hero);
    const char *hero_id = hero.product_id ? hero.product_id : "";
    const char *identity = *hero_id ? hero_id : hero.id ? hero.id : "";
    const unsigned hero_source = discover_hero ? (unsigned)(discover_hero - c->titles) : X4_CATALOG_MAX;
    const bool tab_changed = home.canvas && home.tab != b->active_tab;
    const uint64_t fade_begin = !tab && !discover_fade_disabled && home.discover_art ?
        discover_clock_us() : 0;
    const bool background_changed = tab ? home_clear_background() : home_update_background(a, hero_id);
    if (background_changed || (!home.discover && !tab)) discover_prepare_art(!tab && home.discover);
    home.discover = tab == NULL;
    const unsigned fade_marker = tab ? 0 : x4_catalog_gui_discover_clock(discover_now_us);
    const bool fade_redraw = fade_marker != home.discover_fade_marker;
    home.discover_fade_marker = fade_marker < 3u ? fade_marker : 0;
    if (!home.discover_fade_marker) home.discover_fade_started_us = 0;
    bool full = !home.canvas || home.language != x4_lang_current() ||
        tab_changed || home.secondary != (tab != NULL) || home.redraw_after_keyboard;
    const bool library3 = tab && tab->library3;
    const bool library_shelf = tab && tab->shelf;
    full |= library3 != home.library3 || library_shelf != home.library_shelf;
#ifdef X4_CATALOG_GUI_TEST
    if (home_test_full) full = true;
    home_test_full = false;
#endif
    const bool model_changed = home.model_revision != b->home_revision;
    X4CatalogAssetStats stats = {0};
    if (a) x4_catalog_assets_stats(a, &stats);
    tile_revision = stats.revision;
    const bool assets_changed = stats.revision && home.asset_revision != stats.revision;
    const bool top = b->top_bar_focused || sidebar_focused;
    const X4Ui2LibraryCard library_card = library3 ? x4_catalog_browser_ui2_library_selection(b) :
        (X4Ui2LibraryCard){0};
    const bool library_actions = !top && library_card.product_id && library_card.product_id[0];
    const bool collection_focused = collections && b->ui2_collection_focus && !top;
    const unsigned button_focus = !tab && !top && !collection_focused && b->hero_focused ?
        (b->ui2_hero_favorite && *hero_id ? 2u : 1u) : 0u;
    const bool play_changed = button_focus != home.button_focus;
    const bool favorite = *hero_id && b->favorites && x4_favorites_contains(b->favorites, hero_id);
    const unsigned first = tab ? tab->first[tab->active_row] : b->home_first_row;
    const unsigned row = tab ? tab->active_row : b->home_row;
    const bool scrolled = home.first != first, changed_row = home.row != row;
    const bool changed_column = !tab &&
        memcmp(home.column, b->home_columns, sizeof(home.column)) != 0;
    X4AccessCountryCardPhase access;
    x4_access_country_card_get(NULL, &access);
    const int busy = s ? s->busy : 0, pending = s ? s->pending : 0, connected = s ? s->connected : 0;
    const char *notice = tab && s && s->notice ? s->notice : "";

    X4UiRect regions[5] = {
        {HOME_LEFT, 42, 490, 70},
        {HOME_FEATURE_X, HOME_FEATURE_Y, HOME_FEATURE_W, HOME_FEATURE_H},
        {HOME_LEFT - 8, 445, HOME_RIGHT - HOME_LEFT + 14, 251},
        {HOME_LEFT - 8, 696, HOME_RIGHT - HOME_LEFT + 14, 248},
        {HOME_LEFT - 8, 944, HOME_RIGHT - HOME_LEFT + 14, 42}
    };
    if (!tab) {
        regions[1].h = DISCOVER_HERO_BOTTOM - regions[1].y;
        if (background_changed || fade_redraw) regions[1] = discover_hero_region;
        regions[2].y = DISCOVER_HERO_BOTTOM;
        regions[3].y = HOME_ROW_LABEL_Y + HOME_ROW_PITCH;
        regions[2].h = regions[3].y - regions[2].y;
        regions[3].h = 944 - regions[3].y;
    }
    if (tab) {
        const int row0 = tab->shelf ? 287 : tab->library ? 315 : tab->search ? 300 : 329;
        const int row1 = tab->shelf ? LIBRARY_SHELF_SPLIT :
            tab->library ? LIBRARY_ROW_SPLIT : tab->search ? 574 : 596;
        regions[1].h = row0 - regions[1].y;
        regions[2].y = row0;
        regions[2].h = row1 - row0;
        regions[3].y = row1;
        regions[3].h = 944 - row1;
    }

    const uint64_t tools = tab ? home_tab_tools_key(tab, b, notice, top) : 0;
    const uint64_t collection = collections ? collections_key(b) : 0;
    const bool collection_changed = collection != home.collection_key;
    const bool hero_content_changed = model_changed || assets_changed || background_changed || fade_redraw ||
        strcmp(home.hero_id, identity) || favorite != home.favorite ||
        b->featured_index != home.featured_index || b->featured_count != home.featured_count ||
        busy != home.busy || pending != home.pending;
    const bool play_focus_only = !full && !tab && play_changed && !hero_content_changed && home.play_tile.w > 0;
    bool paint[5] = {
        false,
        tab ? tools != home.tools_key : play_changed || hero_content_changed,
        false, false, connected != home.connected || access != home.access ||
            (tab && tab->library && (tools != home.tools_key || library_actions != home.library_card_actions))
    };
    X4UiRect row_focus[2] = {{0}};
    bool row_focus_only[2] = {false, false};
    uint64_t row_signatures[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        bool row_changed = assets_changed;
        if (tab) {
            row_signatures[i] = home_tab_row_signature(tab, c, b, r, i);
            row_changed |= row_signatures[i] != home.tab_rows[i];
            if (!top && row == i && (tab->library3 ?
                    (b->library.focus == X4_UI2_LIBRARY_CONTINUE || b->library.focus == X4_UI2_LIBRARY_GRID) :
                    tab->library ? !b->ui2_library_tabs :
                    b->ui2_content_focus == X4_UI2_CATALOG_GRID))
                row_focus[i] = home_tab_focus_rect(tab, i);
        } else {
            const unsigned index = first + i;
            uint64_t key = UINT64_C(14695981039346656037);
            if (discover_collection_row(b, index)) {
                key = hash_word(key, collection);
                if (collection_focused) row_focus[i] = collection_focus_rect(b, i);
            } else if (index < b->home_rows.count) {
                const X4HomeRow *items = &b->home_rows.rows[index];
                const unsigned start = x4_catalog_browser_home_row_start(b, index);
                key = hash_word(key, items->kind);
                key = hash_word(key, items->count);
                key = hash_word(key, start);
                for (unsigned col = 0; col < X4_CATALOG_UI2_DISCOVER_VISIBLE && start + col < items->count; ++col) {
                    unsigned at = items->indices[start + col];
                    if (at < c->count) {
                        key = hash_text(key, c->titles[at].id);
                        key = hash_text(key, c->titles[at].product_id);
                        key = hash_text(key, c->titles[at].name);
                    }
                }
                if (!top && !button_focus && !collection_focused && index == row)
                    (void)home_discover_focus_rect(c, b, i, &row_focus[i]);
            }
            row_signatures[i] = key;
            row_changed |= key != home.tab_rows[i];
        }
        const bool ring_changed = focus_changed(home.row_focus[i], row_focus[i]);
        paint[2 + i] = row_changed || ring_changed;
        row_focus_only[i] = !full && !row_changed && ring_changed;
    }

    /* The selector overlays the first row. Restore both touched regions
     * before repainting either while open, including the close transition. */
    const bool popup = tab && !tab->library && !tab->search && !top && b->ui2_selector_open;
    const bool library_popup = library3 && !top && (b->library.popup || b->library.manage);
    if (library3 && (library_popup || home.selector_popup) && (paint[1] || paint[2] || paint[3])) {
        paint[1] = paint[2] = paint[3] = true;
        row_focus_only[0] = row_focus_only[1] = false;
    } else if ((popup || home.selector_popup) && (paint[1] || paint[2])) {
        paint[1] = paint[2] = true;
        row_focus_only[0] = false;
    }
    if (library_shelf && paint[1]) {
        paint[3] = true;
        row_focus_only[1] = false;
    }
    home.count = full ? SIZE_MAX : 0;
    const X4CloudTime cloud = tab ? (X4CloudTime){0} : cloud_home_value;
    const bool cloud_changed = cloud_home_changed(home.cloud_time, cloud);
    const bool feature_redrawn = !tab && (full || (paint[1] && !play_focus_only));
    home.cloud_damage = !full && !tab && cloud_changed && !feature_redrawn;
    home.kind = tab_changed ? X4_HOME_DAMAGE_TAB : background_changed || fade_redraw ? X4_HOME_DAMAGE_BACKGROUND :
        scrolled || changed_row || play_changed ? X4_HOME_DAMAGE_VERTICAL :
        changed_column || collection_changed || row_focus_only[0] || row_focus_only[1] ||
        (tab && tools != home.tools_key) ? X4_HOME_DAMAGE_HORIZONTAL : X4_HOME_DAMAGE_OTHER;
    if (full) home_restore(p, (X4UiRect){0, 0, W, H});
    for (unsigned i = 0; i < 5; ++i) {
        if (!full && !paint[i]) continue;
        if (i == 1 && play_focus_only) {
            home_tile_restore(p, home_play_pixels, home.play_tile);
            discover_button_focus(p, button_focus);
            home.rects[home.count++] = home.play_tile;
            continue;
        }
        if ((i == 2 || i == 3) && row_focus_only[i - 2]) {
            const X4UiRect damage = home_move_row_focus(p, i - 2, row_focus[i - 2]);
            if (damage.w) home.rects[home.count++] = damage;
            continue;
        }
        if (!full) {
            home_restore(p, regions[i]);
            home.rects[home.count++] = regions[i];
        }
        switch (i) {
        case 0: draw_top_bar(p, b); break;
        case 1:
            if (tab) home_draw_tab_tools(p, b, tab, notice, top);
            else {
                if (!full && (background_changed || fade_redraw)) draw_top_bar(p, b);
                home_draw_feature(p, b, &hero, hero_source, a, s, favorite, button_focus);
            }
            break;
        case 2:
        case 3: {
            const unsigned visual = i - 2;
            home.row_focus[visual] = (X4UiRect){0};
            if (!tab && discover_collection_row(b, first + visual))
                draw_collections(p, c, b, a, visual);
            else {
                if (library_shelf && visual) draw_library3_controls(p, b, tab, top);
                home_draw_row(p, c, b, r, a, tab, visual);
            }
            if (row_focus[visual].w) home_draw_row_focus(p, visual, row_focus[visual]);
            break;
        }
        case 4: home_draw_footer(p, b, s); break;
        }
    }
    if (popup && (full || paint[1])) draw_catalog_selectors(p, b, top, true);
    if (library_popup && (full || paint[1] || paint[3])) draw_library3_popup(p, b, tab);
    home.selector_popup = popup || library_popup;
    home.library3 = library3;
    home.library_shelf = library_shelf;
    home.library_card_actions = library_actions;
    if (home.cloud_damage) home_restore(p, cloud_region);
    if ((feature_redrawn || home.cloud_damage) && cloud.source != X4_CLOUD_TIME_NONE)
        cloud_home_draw(p, cloud);
    home.cloud_time = cloud;
    home.canvas = p;
    home.browser = b;
    home.assets = a;
    home.model_revision = b->home_revision;
    if (stats.revision) home.asset_revision = stats.revision;
    home.first = first;
    home.row = row;
    memcpy(home.column, b->home_columns, sizeof(home.column));
    memcpy(home.tab_rows, row_signatures, sizeof(home.tab_rows));
    home.tools_key = tools;
    home.collection_key = collection;
    home.tab = b->active_tab;
    home.secondary = tab != NULL;
    home.redraw_after_keyboard = false;
    home.button_focus = button_focus;
    home.favorite = favorite;
    home.featured_count = b->featured_count;
    home.featured_index = b->featured_index;
    home.busy = busy;
    home.pending = pending;
    home.connected = connected;
    home.language = x4_lang_current();
    home.access = access;
    snprintf(home.hero_id, sizeof(home.hero_id), "%s", identity);
    if (home.discover_fade_marker && (full || background_changed || fade_redraw)) {
        const uint64_t now = discover_clock_us();
        discover_fade_elapsed_us = now >= fade_begin ? now - fade_begin : 0;
        discover_fade_step = home.discover_fade_marker;
    }
}

static void catalog_gui_draw(const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    const X4RecentGames *r, X4CatalogAssets *a, const X4CatalogGuiStatus *s, uint32_t *p,
    bool persistent_canvas)
{
    discover_fade_elapsed_us = 0;
    discover_fade_step = 0;
    if (!p || !b) return;
    bool discover = c && c->state == X4_CATALOG_READY &&
        b->home_configured && b->active_tab == X4_CAT_TAB_DISCOVER;
    bool secondary_tab = b->active_tab == X4_CAT_TAB_CATALOG || b->active_tab == X4_CAT_TAB_RECENTS ||
        b->active_tab == X4_CAT_TAB_SEARCH;
    bool secondary = secondary_tab && c && !access_card_visible(c) &&
        (c->state == X4_CATALOG_READY || b->active_tab == X4_CAT_TAB_RECENTS);
    if ((!discover && !secondary) || (b->editing && !secondary) || !persistent_canvas)
        x4_catalog_gui_home_invalidate();
    if (b->editing) {
        if (x4_catalog_gui_keyboard_cache_ready(b)) {
            bool full=!persistent_canvas||s_keyboard_canvas!=p;
            if(full){
                memcpy(p,s_keyboard_backdrop,(size_t)W*H*sizeof(uint32_t));
                ++keyboard_cache_stats.full_canvas_copies;
            }else{
                for(int row=200;row<880;++row){
                    size_t offset=(size_t)row*W+420;
                    memcpy(p+offset,s_keyboard_backdrop+offset,1080*sizeof(uint32_t));
                }
            }
            draw_search_keyboard_interactive(p, b);
            s_keyboard_canvas=p;
            ++keyboard_cache_stats.cached_draws;keyboard_cache_stats.last_draw_full=full;
            return;
        }
        x4_catalog_gui_keyboard_cache_invalidate();
    } else {
        x4_catalog_gui_keyboard_cache_invalidate();
    }

    if (discover || secondary) {
        HomeTab tab = {0};
        if (secondary) tab = home_tab(c, b, r);
        draw_home(p, c, b, r, a, s, secondary ? &tab : NULL);
        if ((b->editing && !secondary) || !persistent_canvas) x4_catalog_gui_home_invalidate();
        if (b->editing && secondary) home.redraw_after_keyboard = true;
        if (!b->editing) return;
    } else {
        x4_apple_bg_draw(p);
        draw_top_bar(p, b);

        if (!c || c->state != X4_CATALOG_READY) {
            draw_non_ready_state(p, c);
        } else if (b->active_tab == X4_CAT_TAB_DISCOVER) {
            draw_discover_tab(p, c, b, r, a, s);
        }

        /* The access card owns X/Circle/Triangle while visible. */
        draw_footer(p,s,access_card_visible(c));
        if (c && c->state == X4_CATALOG_READY)
            x4_apple_draw_access_success(p, 550, X4_UI_FOOTER_Y - 52, 820);
    }
    keyboard_cache_stats.last_draw_full=true;
    if (b->editing) {
        draw_search_keyboard_modal_frame(p);
        ++keyboard_cache_stats.background_preparations;
        s_keyboard_backdrop=malloc((size_t)W*H*sizeof(uint32_t));
        if(s_keyboard_backdrop){
            memcpy(s_keyboard_backdrop,p,(size_t)W*H*sizeof(uint32_t));
            s_keyboard_backdrop_valid=true;s_keyboard_owner=b;
            s_keyboard_canvas=p;s_keyboard_language=x4_lang_current();
            keyboard_cache_stats.ready=true;keyboard_cache_stats.cache_bytes=(size_t)W*H*sizeof(uint32_t);
        }else ++keyboard_cache_stats.allocation_failures;
        draw_search_keyboard_interactive(p, b);
    }
}

void x4_catalog_gui_draw(const X4CatalogSnapshot *c,const X4CatalogBrowser *b,
    const X4RecentGames *r,X4CatalogAssets *a,const X4CatalogGuiStatus *s,uint32_t *p)
{
    catalog_gui_draw(c,b,r,a,s,p,false);
}

void x4_catalog_gui_draw_cached(const X4CatalogSnapshot *c,const X4CatalogBrowser *b,
    const X4RecentGames *r,X4CatalogAssets *a,const X4CatalogGuiStatus *s,uint32_t *p)
{
    catalog_gui_draw(c,b,r,a,s,p,true);
}
