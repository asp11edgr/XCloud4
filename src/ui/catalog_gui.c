/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_gui.h"
#include "apple_ui.h"
#include "font.h"
#include "lang.h"
#include "net_hint.h"
#include "server_settings.h"
#include "../core/build_identity.h"
#include "stream_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1920
#define H 1080

static const uint32_t C_WHITE = X4_RGB(0xF5, 0xF7, 0xFA);
static const uint32_t C_MUTED = X4_RGB(0xA3, 0xB1, 0xC6);
static const uint32_t C_SOFT  = X4_RGB(0xC5, 0xD0, 0xE0);
static const uint32_t C_GREEN = X4_RGB(0x4A, 0xDE, 0x80);

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

typedef struct {
    char text[256];
} X4WrappedLine;

static int wrap_text_to_lines(const char *src, int font_sz, int font_weight, int max_w,
                              X4WrappedLine *lines, int max_lines)
{
    if (!src || !lines || max_lines <= 0) return 0;
    int count = 0;
    const char *at = src;
    while (*at == ' ' || *at == '\r' || *at == '\n' || *at == '\t') ++at;

    while (*at && count < max_lines) {
        char line_buf[256];
        line_buf[0] = '\0';
        size_t line_len = 0;

        while (*at && *at != '\n') {
            while (*at == ' ' || *at == '\t') ++at;
            if (!*at || *at == '\n') break;

            const char *word_start = at;
            while (*at && *at != ' ' && *at != '\t' && *at != '\n') ++at;
            size_t word_bytes = (size_t)(at - word_start);

            char test_buf[256];
            if (line_len == 0) {
                if (word_bytes >= sizeof(test_buf)) word_bytes = sizeof(test_buf) - 1;
                memcpy(test_buf, word_start, word_bytes);
                test_buf[word_bytes] = '\0';
            } else {
                if (line_len + 1 + word_bytes >= sizeof(test_buf)) break;
                memcpy(test_buf, line_buf, line_len);
                test_buf[line_len] = ' ';
                memcpy(test_buf + line_len + 1, word_start, word_bytes);
                test_buf[line_len + 1 + word_bytes] = '\0';
            }

            if (line_len > 0 && x4_text_width_px(font_sz, font_weight, test_buf) > max_w) {
                at = word_start;
                break;
            }

            size_t test_len = strlen(test_buf);
            memcpy(line_buf, test_buf, test_len + 1);
            line_len = test_len;
        }

        if (line_len > 0) {
            snprintf(lines[count].text, sizeof(lines[count].text), "%s", line_buf);
            ++count;
        } else if (*at && *at != '\n') {
            snprintf(lines[count].text, sizeof(lines[count].text), "%.*s", 32, at);
            at += strlen(lines[count].text);
            ++count;
        }
        if (*at == '\n') ++at;
        while (*at == ' ' || *at == '\r' || *at == '\t') ++at;
    }
    return count;
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
        const X4CoverRaster *surf = &v.surface[preferred_surface];
        if (!surf->rgba && preferred_surface != X4_COVER_GRID) {
            surf = &v.surface[X4_COVER_GRID];
        }
        if (!surf->rgba && preferred_surface != X4_COVER_DETAIL) {
            surf = &v.surface[X4_COVER_DETAIL];
        }
        if (surf && surf->rgba) {
            x4_apple_blit_image_rounded(p, x, y, w, h, r, surf, X4_GRAD_BOTTOM_DARK);
        } else {
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

static void draw_top_bar(uint32_t *p, const X4CatalogSnapshot *c, const X4CatalogBrowser *b)
{
    x4_apple_draw_brand_logo(p, 84, 48);

    /* L1 / R1 hints flanking the 3-tab center bar */
    const int bar_x = 660, bar_y = 42, bar_w = 600, bar_h = 56, bar_r = 28;
    x4_apple_draw_pad_glyph(p, bar_x - 60, 52, X4_PAD_L1);
    x4_apple_glass_panel(p, bar_x, bar_y, bar_w, bar_h, bar_r, 18u, 36u);
    x4_apple_draw_pad_glyph(p, bar_x + bar_w + 16, 52, X4_PAD_R1);

    const enum X4TextId tab_ids[3] = {
        X4_TR_APPLE_TAB_DISCOVER,
        X4_TR_APPLE_TAB_CATALOG,
        X4_TR_APPLE_TAB_RECENTS
    };

    for (unsigned t = 0; t < 3; ++t) {
        int tx = bar_x + 6 + (int)t * 196;
        int ty = 47;
        int tw = 192;
        int th = 46;
        bool active = ((unsigned)b->active_tab == t);
        bool focused = (b->top_bar_focused && b->top_bar_index == t);
        if (active) {
            x4_apple_fill_round_rect(p, tx, ty, tw, th, 23, 0x2EFFFFFFu);
            x4_apple_stroke_round_rect(p, tx, ty, tw, th, 23, 1.2f, 0x55FFFFFFu);
        }
        if (focused) {
            x4_apple_focus_ring(p, tx, ty, tw, th, 23);
        }
        const char *label = x4_tr(tab_ids[t]);
        int weight = active ? X4_FONT_SEMIBOLD : X4_FONT_REGULAR;
        int lw = x4_text_width_px(22, weight, label);
        int lx = tx + (tw - lw) / 2;
        if (lx < tx + 10) lx = tx + 10;
        txt(p, lx, ty + 10, 22, weight, label, tw - 20, active ? C_WHITE : C_MUTED);
    }

    /* Search icon button (index 3) */
    int search_x = 1444, search_y = 44;
    bool search_focus = (b->top_bar_focused && b->top_bar_index == 3);
    x4_apple_glass_panel(p, search_x, search_y, 52, 52, 26,
        (search_focus || b->query[0]) ? 36u : 20u,
        (search_focus || b->query[0]) ? 88u : 38u);
    if (search_focus) x4_apple_focus_ring(p, search_x, search_y, 52, 52, 26);
    x4_apple_draw_icon(p, search_x + 14, search_y + 14, X4_ICON_SEARCH,
        b->query[0] ? C_GREEN : C_WHITE);

    /* Settings gear icon button (index 4) */
    int gear_x = 1510, gear_y = 44;
    bool gear_focus = (b->top_bar_focused && b->top_bar_index == 4);
    x4_apple_glass_panel(p, gear_x, gear_y, 52, 52, 26,
        gear_focus ? 36u : 20u, gear_focus ? 88u : 38u);
    if (gear_focus) x4_apple_focus_ring(p, gear_x, gear_y, 52, 52, 26);
    x4_apple_draw_icon(p, gear_x + 14, gear_y + 14, X4_ICON_SETTINGS, C_WHITE);

    /* Account pill (index 5) */
    const char *acct_label = (c && c->gamertag[0]) ? c->gamertag : x4_tr(X4_TR_APPLE_YOUR_ACCOUNT);
    bool acct_focus = (b->top_bar_focused && b->top_bar_index == 5);
    int pill_w = x4_text_width_px(22, X4_FONT_SEMIBOLD, acct_label) + 74;
    if (pill_w < 190) pill_w = 190;
    if (pill_w > 260) pill_w = 260;
    int pill_x = 1836 - pill_w;
    if (pill_x < 1574) pill_x = 1574;
    x4_apple_draw_account_pill(p, pill_x, 44, 52, acct_label, true, acct_focus);
}

static void draw_non_ready_state(uint32_t *p, const X4CatalogSnapshot *c)
{
    const int x = 84, y = 132, w = 1752, h = 768, r = 24;
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
    X4CatalogBrowser feat = *b;
    if (feat.featured_count == 0) {
        x4_catalog_browser_rebuild_featured(&feat, c, r);
    }

    /* Panoramic Hero Card */
    const int hx = 84, hy = 118, hw = 1752, hh = 416, hr = 24;
    unsigned f_idx = (feat.featured_index < feat.featured_count) ? feat.featured_index : 0;
    const X4CatalogTitle *hero_t = (feat.featured_count > 0 && feat.featured_sources[f_idx] < c->count)
        ? &c->titles[feat.featured_sources[f_idx]]
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

        /* Primary white pill button "Jugar ahora" */
        const char *play_lbl = (s && s->pending) ? x4_tr(X4_TR_CAT_PREPARING) :
            (s && s->busy) ? x4_tr(X4_TR_CAT_QUERYING) : x4_tr(X4_TR_APPLE_PLAY_NOW_BTN);
        int btn_w = x4_text_width_px(24, X4_FONT_SEMIBOLD, play_lbl) + 64;
        if (btn_w < 216) btn_w = 216;
        if (btn_w > 340) btn_w = 340;
        x4_apple_pill_button(p, hx + 48, hy + 292, btn_w, 56, play_lbl, true, false, X4_ICON_NONE);

        /* Featured indicator dots at bottom center */
        if (feat.featured_count > 1) {
            int total_dots_w = (int)feat.featured_count * 10 + 16 + ((int)feat.featured_count - 1) * 10;
            int dx = hx + (hw - total_dots_w) / 2;
            int dy = hy + hh - 26;
            for (unsigned d = 0; d < feat.featured_count; ++d) {
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
    txt(p, 84, 558, 34, X4_FONT_BOLD, row_title, 900, C_WHITE);

    unsigned row_count = x4_catalog_browser_discover_row_count(&feat, c, r);
    unsigned row_sel = (feat.discover_row_selected < row_count) ? feat.discover_row_selected : 0;

    /* Show focused row item metadata summary on the right of the section header */
    if (!b->hero_focused && row_count > 0) {
        unsigned sel_src = 0;
        if (x4_catalog_browser_discover_row_source(&feat, c, r, row_sel, &sel_src) && sel_src < c->count) {
            const X4CatalogTitle *st = &c->titles[sel_src];
            const char *acc = st->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED);
            int aw = x4_text_width_px(20, X4_FONT_SEMIBOLD, acc) + 32;
            x4_apple_glass_panel(p, 1836 - aw, 560, aw, 38, 19, 22u, 48u);
            txt(p, 1836 - aw + 16, 567, 20, X4_FONT_SEMIBOLD, acc, aw - 32, st->entitled ? C_GREEN : C_SOFT);
        }
    }

    if (row_count == 0) {
        x4_apple_glass_panel(p, 84, 616, 1752, 272, 20, 18u, 36u);
        txt(p, 132, 734, 28, X4_FONT_REGULAR, x4_tr(X4_TR_CAT_NO_MATCH), 1656, C_MUTED);
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
        if (x4_catalog_browser_discover_row_source(&feat, c, r, idx, &src) && src < c->count) {
            ct = &c->titles[src];
        }
        const X4RecentGame *rg = has_recents ? x4_recent_get(r, idx) : NULL;
        const char *prod = ct ? ct->product_id : (rg ? rg->product_id : "");
        const char *id   = ct ? ct->id : (rg ? rg->id : "");
        const char *name = ct ? (ct->name[0] ? ct->name : ct->id) : (rg ? (rg->name[0] ? rg->name : rg->id) : "");

        bool focused = (!b->top_bar_focused && !b->hero_focused && idx == row_sel);
        int cx = 84 + (int)slot * 356;
        int cy = 612;
        int cw = 328;
        int ch = 185;
        int cr = 18;
        if (focused) {
            cx -= 4;
            cy -= 4;
            cw += 8;
            ch += 8;
        }

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

static void draw_grid_and_detail_tab(uint32_t *p, const X4CatalogSnapshot *c,
    const X4CatalogBrowser *b, const X4RecentGames *r,
    X4CatalogAssets *a, const X4CatalogGuiStatus *s, bool is_recents_tab)
{
    unsigned count = (c && c->count <= X4_CATALOG_MAX && !b->criteria_dirty && b->filtered_count <= c->count)
        ? b->filtered_count : 0;
    unsigned start = (count && b->selected < count) ? x4_catalog_browser_page_start(b) : 0;

    /* Sub-header: filters + search on Catalog tab, or title on Recents tab */
    if (!is_recents_tab) {
        x4_apple_draw_pad_glyph(p, 84, 124, X4_PAD_L2);
        const enum X4TextId f_ids[3] = {
            X4_TR_CAT_FILTER_ALL,
            X4_TR_CAT_FILTER_ENTITLED,
            X4_TR_CAT_FILTER_UNCONFIRMED
        };
        int fx = 148;
        for (unsigned fi = 0; fi < 3; ++fi) {
            const char *flbl = x4_tr(f_ids[fi]);
            bool active = ((unsigned)b->filter == fi);
            int fw = x4_text_width_px(20, active ? X4_FONT_SEMIBOLD : X4_FONT_REGULAR, flbl) + 34;
            x4_apple_glass_panel(p, fx, 118, fw, 42, 21, active ? 36u : 16u, active ? 84u : 32u);
            txt(p, fx + 17, 126, 20, active ? X4_FONT_SEMIBOLD : X4_FONT_REGULAR,
                flbl, fw - 34, active ? C_WHITE : C_MUTED);
            fx += fw + 12;
        }
        x4_apple_draw_pad_glyph(p, fx + 4, 124, X4_PAD_R2);

        /* Search query box */
        int sx = fx + 74;
        int sw = 1212 - sx;
        if (sw >= 180) {
            x4_apple_glass_panel(p, sx, 118, sw, 42, 21, 18u, 36u);
            x4_apple_draw_icon(p, sx + 14, 127, X4_ICON_SEARCH, b->query[0] ? C_GREEN : C_MUTED);
            const char *q_lbl = b->query[0] ? b->query : x4_tr(X4_TR_APPLE_HINT_SEARCH);
            txt(p, sx + 46, 126, 20, X4_FONT_REGULAR, q_lbl, sw - 60, b->query[0] ? C_WHITE : C_MUTED);
        }
    } else {
        txt(p, 84, 120, 32, X4_FONT_BOLD, x4_tr(X4_TR_CAT_RECENT_TITLE), 760, C_WHITE);
    }

    /* Right side of sub-header: results/page summary */
    char summary[256];
    if (s && s->pending) {
        snprintf(summary, sizeof(summary), "%s", x4_tr(X4_TR_CAT_FINISHING_COVERS));
    } else if (s && s->notice && s->notice[0]) {
        snprintf(summary, sizeof(summary), "%s", s->notice);
    } else if (is_recents_tab) {
        unsigned rcount = x4_recent_count(r);
        unsigned rstart = (b->recent_selected / 6) * 6;
        snprintf(summary, sizeof(summary), x4_tr(X4_TR_CAT_RESULTS_FMT),
            rcount, rcount ? rstart / 6 + 1 : 0, (rcount + 5) / 6, X4_PRODUCT_VERSION);
    } else {
        snprintf(summary, sizeof(summary), x4_tr(X4_TR_CAT_RESULTS_FMT),
            count, count ? start / 6 + 1 : 0, (count + 5) / 6, X4_PRODUCT_VERSION);
    }
    txt(p, 1256, 126, 20, X4_FONT_REGULAR, summary, 580, C_MUTED);

    /* Left 3x2 Grid of Cards (x = 84..1212, y = 180..896) */
    if (!is_recents_tab) {
        if (count > 0) {
            for (unsigned i = 0; i < 6 && (start + i) < count; ++i) {
                unsigned index = b->indices[start + i];
                if (index >= c->count || index >= X4_CATALOG_MAX) continue;
                const X4CatalogTitle *t = &c->titles[index];
                int cx = 84 + (int)(i % 3) * 384;
                int cy = 176 + (int)(i / 3) * 286;
                int cw = 356, ch = 200, cr = 18;
                bool focused = (!b->top_bar_focused && b->selected == (start + i));
                draw_cover_rounded(p, a, t->product_id, X4_COVER_GRID, cx, cy, cw, ch, cr, i, t->id);
                x4_apple_stroke_round_rect(p, cx, cy, cw, ch, cr, 1.2f, 0x2EFFFFFFu);
                txt(p, cx, cy + ch + 10, 22, X4_FONT_SEMIBOLD,
                    t->name[0] ? t->name : t->id, cw, C_WHITE);
                const char *acc = t->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED);
                txt(p, cx, cy + ch + 36, 18, X4_FONT_REGULAR, acc, cw, t->entitled ? C_GREEN : C_MUTED);
                if (focused) {
                    x4_apple_focus_ring(p, cx, cy, cw, ch, cr);
                }
            }
        } else {
            x4_apple_glass_panel(p, 84, 180, 1128, 700, 24, 18u, 36u);
            txt(p, 132, 500, 28, X4_FONT_REGULAR, x4_tr(X4_TR_CAT_NO_MATCH), 1032, C_MUTED);
        }
    } else {
        unsigned nr = x4_recent_count(r);
        unsigned rstart = (b->recent_selected / 6) * 6;
        if (nr > 0) {
            for (unsigned i = 0; i < 6 && (rstart + i) < nr; ++i) {
                const X4RecentGame *g = x4_recent_get(r, rstart + i);
                const X4CatalogTitle *ct = NULL;
                unsigned index = 0;
                if (x4_recent_catalog_index(g, c, &index) && index < c->count) ct = &c->titles[index];
                int cx = 84 + (int)(i % 3) * 384;
                int cy = 176 + (int)(i / 3) * 286;
                int cw = 356, ch = 200, cr = 18;
                bool focused = (!b->top_bar_focused && b->recent_selected == (rstart + i));
                draw_cover_rounded(p, a, g->product_id, X4_COVER_GRID, cx, cy, cw, ch, cr, i, g->id);
                x4_apple_stroke_round_rect(p, cx, cy, cw, ch, cr, 1.2f, 0x2EFFFFFFu);
                const char *rname = ct ? (ct->name[0] ? ct->name : ct->id) : (g->name[0] ? g->name : g->id);
                txt(p, cx, cy + ch + 10, 22, X4_FONT_SEMIBOLD, rname, cw, C_WHITE);
                const char *acc = ct ? (ct->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED)) : "";
                txt(p, cx, cy + ch + 36, 18, X4_FONT_REGULAR, acc, cw, (ct && ct->entitled) ? C_GREEN : C_MUTED);
                if (focused) {
                    x4_apple_focus_ring(p, cx, cy, cw, ch, cr);
                }
            }
        } else {
            x4_apple_glass_panel(p, 84, 180, 1128, 700, 24, 18u, 36u);
            const char *label = x4_tr(X4_TR_CAT_RECENT_EMPTY);
            if (r && r->state == X4_RECENT_LOADING) label = x4_tr(X4_TR_CAT_RECENT_LOADING);
            else if (r && r->state == X4_RECENT_ERROR) label = x4_tr(X4_TR_CAT_RECENT_ERROR);
            txt(p, 132, 480, 28, X4_FONT_REGULAR, label, 1032, C_MUTED);
            if (r && r->state == X4_RECENT_ERROR) {
                txt(p, 132, 530, 24, X4_FONT_REGULAR, x4_tr(X4_TR_CAT_RECENT_RETRY), 1032, C_WHITE);
            }
        }
    }

    /* Right Glass Detail Card (x = 1256, y = 180, w = 580, h = 716) */
    x4_apple_glass_panel(p, 1256, 180, 580, 716, 24, 20u, 38u);
    const X4CatalogTitle *selected = x4_catalog_browser_selected_title(b, c, r);
    const X4RecentGame *historical = (is_recents_tab || b->section == X4_CATALOG_SECTION_RECENT)
        ? x4_recent_get(r, b->recent_selected) : NULL;
    const char *product = selected ? selected->product_id : (historical ? historical->product_id : "");
    const char *id      = selected ? selected->id : (historical ? historical->id : "");

    X4CatalogAssetView view;
    int acquired = (a && product[0] && x4_catalog_assets_acquire(a, product, &view));
    if (selected || historical) {
        /* Detail cover sits at x=1284, y=206, w=524, h=295 (16:9) */
        if (acquired && view.surface[X4_COVER_DETAIL].rgba) {
            x4_apple_blit_image_rounded(p, 1284, 206, 524, 295, 18,
                &view.surface[X4_COVER_DETAIL], X4_GRAD_NONE);
        } else {
            enum X4CatalogAssetState st = acquired ? view.state : (product[0] ? X4_ASSET_PENDING : X4_ASSET_MISSING);
            draw_placeholder_art(p, 1284, 206, 524, 295, 18, 2u, id, false, st, 1310, 246);
        }
        x4_apple_stroke_round_rect(p, 1284, 206, 524, 295, 18, 1.2f, 0x2EFFFFFFu);

        const char *name = selected
            ? (selected->name[0] ? selected->name : selected->id)
            : (historical->name[0] ? historical->name : historical->id);
        if (acquired && view.metadata.name[0]) name = view.metadata.name;

        int lines = wrapped(p, 1284, 514, 30, X4_FONT_BOLD, name, 524, 2, C_WHITE);
        int cur_y = 514 + lines * 38 + 6;

        if (acquired && (view.metadata.category[0] || view.metadata.publisher[0])) {
            char meta_line[256];
            if (view.metadata.category[0] && view.metadata.publisher[0]) {
                snprintf(meta_line, sizeof(meta_line), "%s · %s",
                    view.metadata.category, view.metadata.publisher);
            } else {
                snprintf(meta_line, sizeof(meta_line), "%s",
                    view.metadata.category[0] ? view.metadata.category : view.metadata.publisher);
            }
            txt(p, 1284, cur_y, 20, X4_FONT_REGULAR, meta_line, 524, C_MUTED);
            cur_y += 30;
        }

        /* Description wrapping and scrolling */
        if (acquired && view.metadata.description[0]) {
            int desc_top = cur_y + 4;
            int desc_bottom = 744;
            int avail_h = desc_bottom - desc_top;
            if (avail_h >= 50) {
                X4WrappedLine dlines[64];
                int total_dlines = wrap_text_to_lines(view.metadata.description, 20, X4_FONT_REGULAR, 524, dlines, 64);
                bool can_scroll = (total_dlines * 26 > avail_h);
                int vis_lines = can_scroll ? ((avail_h - 24) / 26) : (avail_h / 26);
                if (vis_lines < 1) vis_lines = 1;

                int max_scroll = total_dlines > vis_lines ? (total_dlines - vis_lines) : 0;
                int scroll = (int)b->detail_scroll_offset;
                if (scroll > max_scroll) scroll = max_scroll;
                if (scroll < 0) scroll = 0;

                for (int li = 0; li < vis_lines && (scroll + li) < total_dlines; ++li) {
                    txt(p, 1284, desc_top + li * 26, 20, X4_FONT_REGULAR, dlines[scroll + li].text, 524, C_SOFT);
                }

                if (can_scroll) {
                    char hint_buf[128];
                    const char *arrow_ind = (scroll > 0 && scroll < max_scroll) ? "▲▼ " :
                                            (scroll > 0) ? "▲ " : "▼ ";
                    snprintf(hint_buf, sizeof(hint_buf), "%s%s", arrow_ind, x4_tr(X4_TR_APPLE_HINT_RIGHT_STICK_READ_MORE));
                    txt(p, 1284, desc_bottom - 20, 18, X4_FONT_REGULAR, hint_buf, 524, C_MUTED);
                }
            }
        }

        const char *acc_lbl = selected
            ? (selected->entitled ? x4_tr(X4_TR_CAT_FILTER_ENTITLED) : x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED))
            : x4_tr(X4_TR_CAT_NOT_AVAILABLE);
        int aw = x4_text_width_px(20, X4_FONT_SEMIBOLD, acc_lbl) + 32;
        x4_apple_glass_panel(p, 1284, 756, aw, 38, 19, 22u, 44u);
        txt(p, 1300, 763, 20, X4_FONT_SEMIBOLD, acc_lbl, aw - 32,
            (selected && selected->entitled) ? C_GREEN : C_MUTED);

        bool can_start = selected && !(s && (s->busy || s->pending));
        const char *play_label = !selected ? x4_tr(X4_TR_CAT_REFRESH_PROMPT) :
            (s && s->pending) ? x4_tr(X4_TR_CAT_PREPARING) :
            (s && s->busy) ? x4_tr(X4_TR_CAT_QUERYING) : x4_tr(X4_TR_APPLE_PLAY_NOW_BTN);
        x4_apple_pill_button(p, 1284, 814, 524, 56, play_label, can_start, false, X4_ICON_NONE);
    } else {
        txt(p, 1284, 520, 26, X4_FONT_REGULAR, x4_tr(X4_TR_CAT_CHOOSE_GAME), 524, C_MUTED);
    }
    if (acquired) x4_catalog_assets_release(a, &view);
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
    (void)s;
    x4_apple_fill_round_rect(p, 84, 926, 1752, 1, 0, X4_RGBA(0xFF, 0xFF, 0xFF, 0x22));
    int fx = 84;
    if (!access) {
        fx += x4_apple_draw_footer_hint(p, fx, 954, X4_PAD_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)) + 34;
        fx += x4_apple_draw_footer_hint(p, fx, 954, X4_PAD_TRIANGLE, x4_tr(X4_TR_APPLE_HINT_SEARCH)) + 34;
        fx += x4_apple_draw_footer_hint(p, fx, 954, X4_PAD_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)) + 34;
        fx += x4_apple_draw_footer_hint(p, fx, 954, X4_PAD_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)) + 34;
    }
    int notice_right;

#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE && !X4_PUBLIC
    char video_settings[192];
    x4_stream_settings_summary(video_settings,sizeof(video_settings));
    /* txt(p,650,68,28,1,video_settings,580,green); */
    int vw = x4_text_width_px(20, X4_FONT_SEMIBOLD, video_settings);
    int vx = 1836 - vw;
    if (vx < fx + 16) vx = fx + 16;
    txt(p, vx, 956, 20, X4_FONT_SEMIBOLD, video_settings, 1836 - vx, C_GREEN);
    notice_right = vx - 32;
#else
    const char *ds4 = (s && s->connected) ? x4_tr(X4_TR_CAT_DS4_CONNECTED) : x4_tr(X4_TR_CAT_DS4_CONNECT);
    int dw = x4_text_width_px(20, X4_FONT_REGULAR, ds4);
    int dx = 1836 - dw;
    if (dx < fx + 16) dx = fx + 16;
    txt(p, dx, 956, 20, X4_FONT_REGULAR, ds4, 1836 - dx, (s && s->connected) ? C_SOFT : C_MUTED);
    notice_right = dx - 32;
#endif
    if (access && s && s->notice && s->notice[0] && notice_right > 84)
        txt(p,84,956,20,X4_FONT_REGULAR,s->notice,notice_right-84,C_WHITE);
}

static void catalog_gui_draw(const X4CatalogSnapshot *c, const X4CatalogBrowser *b,
    const X4RecentGames *r, X4CatalogAssets *a, const X4CatalogGuiStatus *s, uint32_t *p,
    bool persistent_canvas)
{
    if (!p || !b) return;
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

    x4_apple_bg_draw(p);
    draw_top_bar(p, c, b);

    if (!c || c->state != X4_CATALOG_READY) {
        draw_non_ready_state(p, c);
    } else if (b->active_tab == X4_CAT_TAB_DISCOVER) {
        draw_discover_tab(p, c, b, r, a, s);
    } else if (b->active_tab == X4_CAT_TAB_RECENTS) {
        draw_grid_and_detail_tab(p, c, b, r, a, s, true);
    } else {
        draw_grid_and_detail_tab(p, c, b, r, a, s, false);
    }

    /* The access card owns X/Circle/Triangle. Do not advertise catalog
     * selection/search shortcuts while those same edges have other actions. */
    draw_footer(p,s,access_card_visible(c));
    if (c && c->state == X4_CATALOG_READY)
        x4_apple_draw_access_success(p, 550, 988, 820);
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
