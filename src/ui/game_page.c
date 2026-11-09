/* SPDX-License-Identifier: GPL-3.0-only */
#include "game_page.h"
#include "apple_ui.h"
#include "canvas.h"
#include "font.h"
#include "image_area.h"
#include "ui2_theme.h"
#ifdef X4_UI48_CAPTURE
#include "catalog_gui.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GAME_W = X4_UI_WIDTH, GAME_H = X4_UI_HEIGHT, GAME_X = X4_UI2_CONTENT_X,
    GAME_RIGHT = X4_UI_SAFE_RIGHT, GAME_FOOT_Y = X4_UI_FOOTER_Y,
    GAME_HEAD_Y = 126, GAME_TITLE_Y = 134, GAME_TITLE_STEP = 52,
    GAME_HEADER_INK_LEFT = 16, GAME_TITLE_INSET = 4,
    GAME_DESCRIPTION_INK_LEFT = 16,
    GAME_HEADER_GAP = 10, GAME_BADGE_H = 35,
    GAME_SUMMARY_H = X4_UI2_CAPTION + 4, GAME_SUMMARY_STEP = 26,
    GAME_ACTION_GAP = 40, GAME_PLAY_H = 60, GAME_ACTION_RADIUS = X4_UI2_RADIUS_CONTROL,
    GAME_TABS_Y = 481, GAME_TAB_H = 43, GAME_TAB_W = 176, GAME_TAB_GAP = 16,
    GAME_PANEL_Y = 546, GAME_PANEL_H = 335,
    GAME_TEXT_W = 832, GAME_DESC_Y = GAME_PANEL_Y + 53,
    GAME_DESC_SIZE = X4_UI2_BODY, GAME_DESC_STEP = 37,
    GAME_DESC_VISIBLE = 6, GAME_LINE_BYTES = 384,
    GAME_DESCRIPTION_RESTORE_Y = GAME_PANEL_Y + 10,
    GAME_TILE_X = GAME_X + 8, GAME_TILE_Y = GAME_PANEL_Y + 55,
    GAME_DETAIL_X = GAME_X + 25,
    GAME_TILE_W = 368, GAME_TILE_H = 207, GAME_TILE_GAP = 16,
    GAME_TILE_VISIBLE = 4, GAME_TILE_RADIUS = X4_UI2_RADIUS_CARD, GAME_SIMILAR_TILE_H = GAME_TILE_H + 44,
    GAME_FOCUS_PAD = X4_UI2_FOCUS_SPREAD,
};

_Static_assert(GAME_TILE_W * 9 == GAME_TILE_H * 16, "Game tiles keep the whole 16:9 image");
_Static_assert(GAME_TILE_X + GAME_TILE_VISIBLE * (GAME_TILE_W + GAME_TILE_GAP) -
    GAME_TILE_GAP + GAME_FOCUS_PAD <= GAME_RIGHT, "Game tiles must fit in the safe area");
/* Two title lines allow one summary line. The other layout adds only 26 px
 * of summary instead of a second 52 px title line. */
_Static_assert(GAME_TITLE_Y + 2 * GAME_TITLE_STEP + 3 * GAME_HEADER_GAP +
    X4_UI2_BODY + 4 + GAME_BADGE_H + GAME_SUMMARY_H +
    2 * GAME_HEADER_GAP + GAME_SUMMARY_H + GAME_PLAY_H + 2 * GAME_FOCUS_PAD <= GAME_TABS_Y,
    "Game action and tab focus must fit without overlapping");

#define GAME_WHITE X4_UI2_TEXT
#define GAME_SOFT X4_UI2_MUTED
#define GAME_GREEN X4_UI2_ACCENT

#ifdef X4_UI46_CAPTURE
#define CONTRAST(p, role, x, y, w, h, color) \
    x4_ui46_capture_contrast((p), (role), (x), (y), (w), (h), (color))
#else
#define CONTRAST(p, role, x, y, w, h, color) ((void)0)
#endif

static const char *text_or(const char *text, const char *fallback)
{
    return text && text[0] ? text : fallback;
}

static unsigned screenshot_count(const X4GamePageModel *model)
{
    const unsigned count = model->details ? model->details->screenshot_count : 0;
    return count <= X4_GAME_SCREENSHOT_MAX ? count : X4_GAME_SCREENSHOT_MAX;
}

typedef struct {
    uintptr_t address;
    unsigned width, height;
} GameArtKey;

static GameArtKey art_key(const X4CoverRaster *image)
{
    return image && image->rgba && image->width && image->height ?
        (GameArtKey){(uintptr_t)image->rgba, image->width, image->height} : (GameArtKey){0};
}

static bool same_art(GameArtKey a, GameArtKey b)
{
    return a.address == b.address && a.width == b.width && a.height == b.height;
}

typedef struct {
    GameArtKey art;
    bool chart;
} ShotClassification;

/* UI thread only. The worker publishes immutable, already-decoded thumbnails.
 * The key identifies a raster while the current product is selected. */
static struct {
    const X4GamePage *owner;
    char product[X4_PRODUCT_ID_CAP];
    ShotClassification shots[X4_GAME_SCREENSHOT_MAX];
    uint8_t order[X4_GAME_SCREENSHOT_MAX], position[X4_GAME_SCREENSHOT_MAX];
    unsigned count;
} shot_order;

static bool screenshot_looks_like_chart(const X4CoverRaster *image)
{
    enum { COLS = 12, ROWS = 8, SAMPLES = COLS * ROWS };
    unsigned light = 0, light_below = 0, neutral = 0;
    for (unsigned y = 0; y < ROWS; ++y) {
        const unsigned yy = image->height * (2u * y + 1u) / (2u * ROWS);
        const uint32_t *row = image->rgba + (size_t)yy * image->width;
        for (unsigned x = 0; x < COLS; ++x) {
            const uint32_t pixel = row[image->width * (2u * x + 1u) / (2u * COLS)];
            const unsigned r = pixel & 255u, g = (pixel >> 8) & 255u, b = (pixel >> 16) & 255u;
            const unsigned high_rg = r > g ? r : g, low_rg = r < g ? r : g;
            const unsigned high = high_rg > b ? high_rg : b, low = low_rg < b ? low_rg : b;
            if (high - low > 28u) continue;
            ++neutral;
            if (low >= 185u) {
                ++light;
                if (y >= ROWS / 2) ++light_below;
            }
        }
    }
    /* Look for a light neutral background across the tile. The bottom-half
     * threshold keeps bright skies with darker terrain in the gameplay group. */
    return 3u * neutral >= 2u * SAMPLES && 4u * light >= SAMPLES &&
        8u * light_below >= SAMPLES;
}

static void prepare_shot_order(const X4GamePage *page, const X4GamePageModel *model)
{
    const char *product = text_or(model->product_id, "");
    bool changed = shot_order.owner != page || strcmp(shot_order.product, product);
    if (changed) {
        memset(&shot_order, 0, sizeof(shot_order));
        shot_order.owner = page;
        snprintf(shot_order.product, sizeof(shot_order.product), "%s", product);
    }
    const unsigned count = screenshot_count(model);
    changed |= shot_order.count != count;
    shot_order.count = count;
    for (unsigned i = 0; i < count; ++i) {
        const X4CoverRaster *image = model->screenshots[i];
        const GameArtKey key = art_key(image);
        ShotClassification *shot = &shot_order.shots[i];
        if (!key.address || same_art(shot->art, key)) continue;
        shot->art = key;
        shot->chart = key.width <= 2048 && key.height <= 2048 && screenshot_looks_like_chart(image);
        changed = true;
    }
    if (!changed) return;
    unsigned position = 0;
    for (unsigned chart = 0; chart < 2; ++chart) {
        for (unsigned i = 0; i < count; ++i) {
            if (shot_order.shots[i].chart != (chart != 0)) continue;
            shot_order.order[position] = (uint8_t)i;
            shot_order.position[i] = (uint8_t)position++;
        }
    }
}

static unsigned screenshot_position(const X4GamePage *page)
{
    return page->screenshot_index < shot_order.count ? shot_order.position[page->screenshot_index] : 0;
}

static const char *description(const X4GamePageModel *model)
{
    return text_or(model->details ? model->details->description : NULL,
        text_or(model->description, ""));
}

static bool has_store(const X4GamePageModel *model)
{
    return !model->entitled && x4_product_id_valid(model->product_id);
}

static bool can_play(const X4GamePageModel *model)
{
    return model->available && !model->busy;
}

static const char *store_label(const X4GamePageModel *model)
{
    return model->free_in_store ? x4_tr(X4_TR_50_SCREEN_GAME_FREE_GET) :
        x4_tr(X4_TR_50_SCREEN_GAME_STORE);
}

static const char *access_label(const X4GamePageModel *model)
{
    if (!model->available) return x4_tr(X4_TR_CAT_NOT_AVAILABLE);
    if (model->free_in_store) return x4_tr(X4_TR_APPLE_BADGE_FREE);
    return x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED);
}

bool x4_game_page_store_url(const char *product_id, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!x4_product_id_valid(product_id)) return false;
    int length = snprintf(out, capacity, "https://www.xbox.com/games/store/p/%s", product_id);
    if (length < 0 || (size_t)length >= capacity) {
        out[0] = 0;
        return false;
    }
    return true;
}

/* The measured width of a single-line prefix is monotone: the font
 * retains the maximum ink extent and advances the pen by nonnegative
 * amounts. Search exact font measurements only at complete codepoints. */
static const char *wrapped_line(const char *text, char line[GAME_LINE_BYTES],
    int size, int weight)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') ++text;
    uint16_t ends[GAME_LINE_BYTES];
    ends[0] = 0;
    unsigned count = 0;
    const char *at = text;
    while (*at && *at != '\n') {
        const char *next = at;
        (void)x4_utf8_next(&next);
        if (next - text >= GAME_LINE_BYTES) break;
        ends[++count] = (uint16_t)(next - text);
        at = next;
    }
    memcpy(line, text, ends[count]);
    line[ends[count]] = 0;

    unsigned lower = 0, upper = count;
    while (lower < upper) {
        unsigned middle = lower + (upper - lower + 1) / 2;
        char saved = line[ends[middle]];
        line[ends[middle]] = 0;
        int width = x4_text_width_px(size, weight, line);
        line[ends[middle]] = saved;
        if (width <= GAME_TEXT_W) lower = middle;
        else upper = middle - 1;
    }
    at = text + ends[count && !lower ? 1u : lower];
    if (lower < count || (*at && *at != '\n' && at - text >= GAME_LINE_BYTES - 4)) {
        for (const char *word = at; word > text; --word) {
            if (word[-1] == ' ' || word[-1] == '\t') {
                at = word - 1;
                break;
            }
        }
    }
    size_t length = (size_t)(at - text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\r' ||
        text[length - 1] == '\t')) --length;
    line[length] = 0;
    while (*at == ' ' || *at == '\r' || *at == '\t') ++at;
    if (*at == '\n') ++at;
    return at;
}

enum {
    GAME_DESC_CAP = sizeof(((X4GameDetails *)0)->description),
    GAME_DESC_CHECKPOINT_STEP = 16,
    GAME_DESC_CHECKPOINTS = 1 + (GAME_DESC_CAP - 1) / GAME_DESC_CHECKPOINT_STEP,
};
_Static_assert(GAME_DESC_CAP <= UINT16_MAX, "Description offsets must fit uint16_t");

/* Only the foreground UI owns this cache. Keeping a bounded copy makes
 * reuse exact even when the same model address receives a new snapshot.
 * Checkpoints bound scroll work without one offset per possible newline. */
static struct {
    char text[GAME_DESC_CAP];
    uint16_t checkpoints[GAME_DESC_CHECKPOINTS];
    size_t bytes;
    unsigned line_count;
    uint64_t revision;
} game_description;

static void prepare_description(const char *text)
{
    size_t bytes = 0;
    while (bytes + 1 < GAME_DESC_CAP && text[bytes]) ++bytes;
    if (bytes == game_description.bytes && !memcmp(text, game_description.text, bytes)) return;
    memcpy(game_description.text, text, bytes);
    game_description.text[bytes] = 0;
    game_description.bytes = bytes;
    game_description.line_count = 0;
    ++game_description.revision;
    char line[GAME_LINE_BYTES];
    const char *at = game_description.text;
    while (*at) {
        unsigned index = game_description.line_count++;
        if (index % GAME_DESC_CHECKPOINT_STEP == 0)
            game_description.checkpoints[index / GAME_DESC_CHECKPOINT_STEP] =
                (uint16_t)(at - game_description.text);
        at = wrapped_line(at, line, GAME_DESC_SIZE, X4_FONT_REGULAR);
    }
}

static unsigned description_extent(const char *text, unsigned *offset,
    char lines[GAME_DESC_VISIBLE][GAME_LINE_BYTES])
{
    prepare_description(text);
    const unsigned count = game_description.line_count;
    const unsigned max = count > GAME_DESC_VISIBLE ? count - GAME_DESC_VISIBLE : 0;
    if (*offset > max) *offset = max;
    if (lines) {
        const unsigned block = *offset / GAME_DESC_CHECKPOINT_STEP;
        const char *at = game_description.text;
        if (count) at += game_description.checkpoints[block];
        for (unsigned i = block * GAME_DESC_CHECKPOINT_STEP; i < *offset; ++i)
            at = wrapped_line(at, lines[0], GAME_DESC_SIZE, X4_FONT_REGULAR);
        for (unsigned i = 0; i < GAME_DESC_VISIBLE; ++i) {
            lines[i][0] = 0;
            if (*at) at = wrapped_line(at, lines[i], GAME_DESC_SIZE, X4_FONT_REGULAR);
        }
    }
    return max;
}

static void draw_notice(uint32_t *p, const char *notice)
{
    if (!notice || !notice[0]) return;
    enum { X = 1328, Y = 126, W = GAME_RIGHT - X, H = 122 };
    x4_apple_fill_round_rect(p, X, Y, W, H, 24, X4_RGBA(12, 20, 34, 237));
    x4_apple_stroke_round_rect(p, X, Y, W, H, 24, 1.0f, X4_RGBA(233, 242, 249, 75));
    x4_apple_draw_icon(p, X + 24, Y + 24, X4_ICON_INFO, GAME_WHITE);
    x4_apple_text_wrap(p, X + 62, Y + 22, 23, X4_FONT_REGULAR,
        notice, W - 86, 32, 3, GAME_SOFT);
}

static void draw_store(uint32_t *p, const X4GamePageModel *model)
{
    char url[X4_GAME_STORE_URL_CAP];
    if (!x4_game_page_store_url(model->product_id, url, sizeof(url))) return;
    x4_apple_fill_round_rect(p, 0, 0, GAME_W, GAME_H, 0, X4_RGBA(2, 7, 14, 187));
    x4_apple_fill_round_rect(p, 356, 202, 1208, 647, 38, X4_RGB(15, 27, 42));
    x4_apple_stroke_round_rect(p, 356, 202, 1208, 647, 38, 1.0f, X4_RGBA(222, 234, 250, 82));
    x4_text_ellipsis_px(p, 418, 253, 46, X4_FONT_BOLD, store_label(model), 1080, GAME_WHITE);
    x4_apple_text_wrap(p, 418, 346, 25, X4_FONT_REGULAR,
        x4_tr(X4_TR_50_SCREEN_GAME_STORE_QR),
        500, 39, 4, GAME_SOFT);
    x4_apple_draw_public_qr(p, 1225, 484, 310, url);
    const char *short_url = url + sizeof("https://") - 1;
    int width = x4_text_width_px(18, X4_FONT_REGULAR, short_url);
    x4_text_ellipsis_px(p, 1225 - width / 2, 654, 18, X4_FONT_REGULAR, short_url, 540, GAME_SOFT);
    x4_apple_pill_button(p, 418, 689, 385, 63,
        x4_tr(X4_TR_50_SCREEN_GAME_STORE_REFRESH), true, true, X4_ICON_NONE);
    x4_apple_draw_footer_hint(p, 418, 774, X4_PAD_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK));
}


static unsigned similar_count(const X4GamePageModel *model)
{
    if (!model->similar) return 0;
    return model->similar_count <= X4_GAME_SIMILAR_MAX ? model->similar_count : X4_GAME_SIMILAR_MAX;
}

static unsigned selected_index(unsigned index, unsigned count)
{
    return index < count ? index : count ? count - 1 : 0;
}

static unsigned first_tile(unsigned index)
{
    return index >= GAME_TILE_VISIBLE ? index - GAME_TILE_VISIBLE + 1 : 0;
}

static X4GamePageFocus content_focus(X4GamePageTab tab)
{
    switch (tab) {
    case X4_GAME_TAB_DETAILS: return X4_GAME_FOCUS_DETAILS;
    case X4_GAME_TAB_SIMILAR: return X4_GAME_FOCUS_SIMILAR;
    default: return X4_GAME_FOCUS_SCREENSHOTS;
    }
}

static bool action_focus(X4GamePageFocus focus)
{
    return focus == X4_GAME_FOCUS_PLAY || focus == X4_GAME_FOCUS_FAVORITE || focus == X4_GAME_FOCUS_STORE;
}

static void move_tab(X4GamePage *page, bool next)
{
    const unsigned step = next ? 1u : X4_GAME_TAB_COUNT - 1u;
    page->tab = (X4GamePageTab)((page->tab + step) % X4_GAME_TAB_COUNT);
    page->focus = X4_GAME_FOCUS_TABS;
}

static void scroll_description(X4GamePage *page, const X4GamePageModel *model, bool down)
{
    unsigned offset = page->description_scroll;
    unsigned max = description_extent(description(model), &offset, NULL);
    if (down && offset < max) ++offset;
    else if (!down && offset) --offset;
    page->description_scroll = offset;
}

void x4_game_page_init(X4GamePage *page)
{
    if (!page) return;
    *page = (X4GamePage){.focus = X4_GAME_FOCUS_PLAY, .screenshot_index = X4_GAME_SCREENSHOT_MAX};
    if (shot_order.owner == page) shot_order.owner = NULL;
}

int x4_game_page_fullscreen_index(const X4GamePage *page)
{
    return page && page->fullscreen_open && page->screenshot_index < X4_GAME_SCREENSHOT_MAX ?
        (int)page->screenshot_index : -1;
}

enum X4GamePageAction x4_game_page_update(X4GamePage *page,
    const X4GamePageModel *model, unsigned input)
{
    if (!page || !model) return X4_GAME_ACTION_NONE;
    if (input & X4_GAME_INPUT_BACK) {
        if (page->fullscreen_open || page->qr_open) {
            page->fullscreen_open = false;
            page->qr_open = false;
            return X4_GAME_ACTION_NONE;
        }
        return X4_GAME_ACTION_BACK;
    }
    if (page->qr_open) {
        if ((input & X4_GAME_INPUT_SELECT) && !model->busy) {
            page->qr_open = false;
            return X4_GAME_ACTION_REFRESH;
        }
        return X4_GAME_ACTION_NONE;
    }
    if (page->fullscreen_open) return X4_GAME_ACTION_NONE;

    const bool favorite = x4_product_id_valid(model->product_id);
    const bool store = has_store(model);
    const unsigned shots = screenshot_count(model), similar = similar_count(model);
    prepare_shot_order(page, model);
    page->similar_index = selected_index(page->similar_index, similar);
    if ((page->focus == X4_GAME_FOCUS_STORE && !store) ||
        (page->focus == X4_GAME_FOCUS_FAVORITE && !favorite))
        page->focus = X4_GAME_FOCUS_PLAY;
    if ((page->focus == X4_GAME_FOCUS_SCREENSHOTS && !shots) ||
        (page->focus == X4_GAME_FOCUS_SIMILAR && !similar))
        page->focus = X4_GAME_FOCUS_TABS;

    if (input & (X4_GAME_INPUT_PREVIOUS_TAB | X4_GAME_INPUT_NEXT_TAB)) {
        move_tab(page, (input & X4_GAME_INPUT_NEXT_TAB) != 0);
        return X4_GAME_ACTION_NONE;
    }
    if (page->tab == X4_GAME_TAB_DETAILS &&
        (input & (X4_GAME_INPUT_SCROLL_UP | X4_GAME_INPUT_SCROLL_DOWN)))
        scroll_description(page, model, (input & X4_GAME_INPUT_SCROLL_DOWN) != 0);

    const unsigned direction = input & (X4_GAME_INPUT_UP | X4_GAME_INPUT_DOWN |
        X4_GAME_INPUT_LEFT | X4_GAME_INPUT_RIGHT);
    if (direction) {
        if (action_focus(page->focus)) {
            if (direction & X4_GAME_INPUT_DOWN) page->focus = X4_GAME_FOCUS_TABS;
            else if (direction & X4_GAME_INPUT_RIGHT) {
                if (page->focus == X4_GAME_FOCUS_PLAY && favorite) page->focus = X4_GAME_FOCUS_FAVORITE;
                else if (store) page->focus = X4_GAME_FOCUS_STORE;
            } else if (direction & X4_GAME_INPUT_LEFT) {
                page->focus = page->focus == X4_GAME_FOCUS_STORE && favorite ?
                    X4_GAME_FOCUS_FAVORITE : X4_GAME_FOCUS_PLAY;
            }
        } else if (page->focus == X4_GAME_FOCUS_TABS) {
            if (direction & X4_GAME_INPUT_UP) page->focus = X4_GAME_FOCUS_PLAY;
            else if (direction & X4_GAME_INPUT_LEFT) move_tab(page, false);
            else if (direction & X4_GAME_INPUT_RIGHT) move_tab(page, true);
            else if ((page->tab != X4_GAME_TAB_SCREENSHOTS || shots) &&
                (page->tab != X4_GAME_TAB_SIMILAR || similar)) page->focus = content_focus(page->tab);
        } else if (direction & X4_GAME_INPUT_UP) {
            if (page->focus == X4_GAME_FOCUS_DETAILS && page->description_scroll)
                scroll_description(page, model, false);
            else page->focus = X4_GAME_FOCUS_TABS;
        } else if (page->focus == X4_GAME_FOCUS_DETAILS) {
            if (direction & X4_GAME_INPUT_DOWN) scroll_description(page, model, true);
        } else if (page->focus == X4_GAME_FOCUS_SIMILAR) {
            if ((direction & X4_GAME_INPUT_LEFT) && page->similar_index) --page->similar_index;
            if ((direction & X4_GAME_INPUT_RIGHT) && page->similar_index + 1 < similar) ++page->similar_index;
        } else if (page->focus == X4_GAME_FOCUS_SCREENSHOTS) {
            unsigned position = screenshot_position(page);
            if ((direction & X4_GAME_INPUT_LEFT) && position) --position;
            if ((direction & X4_GAME_INPUT_RIGHT) && position + 1 < shots) ++position;
            page->screenshot_index = shot_order.order[position];
        }
        return X4_GAME_ACTION_NONE;
    }

    if (!(input & X4_GAME_INPUT_SELECT)) return X4_GAME_ACTION_NONE;
    switch (page->focus) {
    case X4_GAME_FOCUS_PLAY: return can_play(model) ? X4_GAME_ACTION_PLAY : X4_GAME_ACTION_NONE;
    case X4_GAME_FOCUS_FAVORITE: return X4_GAME_ACTION_FAVORITE;
    case X4_GAME_FOCUS_SIMILAR: return X4_GAME_ACTION_SIMILAR;
    case X4_GAME_FOCUS_STORE: page->qr_open = true; break;
    case X4_GAME_FOCUS_SCREENSHOTS:
        page->screenshot_index = shot_order.order[screenshot_position(page)];
        page->fullscreen_open = true;
        break;
    default: break;
    }
    return X4_GAME_ACTION_NONE;
}

static void draw_art(uint32_t *p, const X4CoverRaster *art)
{
    if (art && art->rgba && art->width && art->height) {
        if (x4_apple_blit_image_exact(p, 0, 0, GAME_W, GAME_H, 0, GAME_W, art)) return;
        x4_apple_blit_image_rounded(p, 0, 0, GAME_W, GAME_H, 0, art, X4_GRAD_NONE);
    } else x4_apple_bg_draw(p);
}

static void draw_footer(uint32_t *p, bool modal)
{
    const X4Ui2Hint hints[] = {
        {X4_PAD_L1, ""},
        {X4_PAD_R1, x4_tr(X4_TR_50_SCREEN_GAME_CHANGE_TAB)},
        {X4_PAD_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)},
    };
    const size_t count = sizeof(hints) / sizeof(hints[0]);
    x4_ui2_footer(p, modal ? &hints[count - 1] : hints, modal ? 1 : count);
}

static const X4CoverRaster *fullscreen_art(const X4GamePageModel *model, int index)
{
    return model->fullscreen && model->fullscreen->rgba ? model->fullscreen : model->screenshots[index];
}

static void draw_fullscreen(uint32_t *p, const X4GamePage *page, const X4GamePageModel *model)
{
    const int index = x4_game_page_fullscreen_index(page);
    draw_art(p, fullscreen_art(model, index));
    x4_ui2_card(p, GAME_X, 126, 340, 52, 26);
    char label[96];
    snprintf(label, sizeof(label), x4_tr(X4_TR_50_SCREEN_GAME_SCREENSHOT_FMT),
        screenshot_position(page) + 1, screenshot_count(model));
    x4_text_ellipsis_px(p, GAME_X + 20, 140, X4_UI2_BODY, X4_FONT_SEMIBOLD, label, 300, GAME_WHITE);
    x4_ui2_card(p, GAME_RIGHT - 190, GAME_FOOT_Y - 10, 204, 51, 24);
    draw_footer(p, true);
}

static void draw_backdrop(uint32_t *p, const X4CoverRaster *background)
{
    draw_art(p, background);
    x4_apple_shade_text_zone(p, 0, 0, 1150, 551, 0, 460, 196, 226);
    x4_apple_shade_text_zone(p, 0, 0, GAME_W, 110, 0, 0, 104, 164);
    x4_apple_shade_text_zone(p, 0, 430, GAME_W, GAME_H - 430, 160, 0, 0, 237);
    x4_rect(p, GAME_X, GAME_TABS_Y + GAME_TAB_H + 9, GAME_RIGHT - GAME_X, 1, X4_UI2_BORDER);
}

static void append_metadata(char *line, size_t capacity, const char *value)
{
    if (!value || !value[0]) return;
    size_t used = strlen(line);
    if (used >= capacity - 1) return;
    (void)snprintf(line + used, capacity - used, "%s%s", used ? " · " : "", value);
}

typedef struct {
    char title[128], metadata[320], summary[GAME_LINE_BYTES], age_rating[32];
    char last_session[48], played_time[48];
    unsigned attributes;
    bool game_pass, free_in_store, available, entitled;
} GameHeader;

static void header_data(GameHeader *header, const X4GamePageModel *model)
{
    memset(header, 0, sizeof(*header));
    const X4GameDetails *details = model->details;
    const char *title = text_or(details ? details->title : NULL,
        text_or(model->title, x4_tr(X4_TR_50_SCREEN_GAME_DEFAULT)));
    const char *category = text_or(details ? details->category : NULL, model->category);
    const char *studio = text_or(details ? details->publisher : NULL, model->studio);
    snprintf(header->title, sizeof(header->title), "%s", title);
    append_metadata(header->metadata, sizeof(header->metadata), studio);
    if (category && category[0])
        append_metadata(header->metadata, sizeof(header->metadata), x4_lang_store_category(category));
    if (details) {
        if (details->subcategory[0] && (!category || strcmp(category, details->subcategory)))
            append_metadata(header->metadata, sizeof(header->metadata),
                x4_lang_store_category(details->subcategory));
        append_metadata(header->metadata, sizeof(header->metadata), details->year);
        snprintf(header->age_rating, sizeof(header->age_rating), "%s", details->age_rating);
        header->attributes = details->attributes;
    }
    const char *short_text = text_or(model->description, description(model));
    snprintf(header->summary, sizeof(header->summary), "%.*s", GAME_LINE_BYTES - 1, short_text);
    snprintf(header->last_session, sizeof(header->last_session), "%s", text_or(model->last_session, ""));
    snprintf(header->played_time, sizeof(header->played_time), "%s", text_or(model->played_time, ""));
    header->game_pass = model->available && model->entitled && !model->free_in_store;
    header->free_in_store = model->free_in_store;
    header->available = model->available;
    header->entitled = model->entitled;
}

static void game_text_line(uint32_t *p, const char *role, int x, int y, int size,
    int weight, const char *text, int width, uint32_t color)
{
    if (!text || !text[0]) return;
    char fit[GAME_LINE_BYTES];
    x4_text_fit_utf8(fit, sizeof(fit), size, weight, text, width);
    (void)role;
    CONTRAST(p, role, x, y, x4_text_width_px(size, weight, fit), size + 4, color);
    x4_text_px(p, x, y, size, weight, fit, color);
}

static int draw_badge(uint32_t *p, int x, int y, const char *label, bool accent, bool icon)
{
    if (!label || !label[0]) return x;
    int w = x4_text_width_px(X4_UI2_CAPTION, X4_FONT_SEMIBOLD, label) + 26 + (icon ? 31 : 0);
    if (w > GAME_RIGHT - x) return x;
    const uint32_t ink = accent ? GAME_GREEN : GAME_WHITE;
    if (accent) x4_apple_fill_round_rect(p, x, y, w, GAME_BADGE_H, 17, X4_UI2_ACTIVE);
    else x4_ui2_card(p, x, y, w, GAME_BADGE_H, 17);
    if (icon) x4_apple_draw_icon(p, x + 11, y + 5, X4_ICON_DEVICE_GAMEPAD, ink);
    game_text_line(p, "game badge", x + (icon ? 42 : 13), y + 7, X4_UI2_CAPTION,
        X4_FONT_SEMIBOLD, label, w - 25 - (icon ? 30 : 0), ink);
    return x + w + 10;
}

static void draw_history(uint32_t *p, int y, const GameHeader *header)
{
    char text[224] = {0};
    char played[112];
    if (header->last_session[0])
        snprintf(text, sizeof(text), x4_tr(X4_TR_50_SCREEN_GAME_LAST_SESSION_FMT),
            header->last_session);
    if (header->played_time[0]) {
        snprintf(played, sizeof(played), x4_tr(X4_TR_50_SCREEN_GAME_TIME_PLAYED_FMT),
            header->played_time);
        append_metadata(text, sizeof(text), played);
    }
    x4_apple_stroke_round_rect(p, GAME_X, y + 4, 18, 18, 9, 2, GAME_SOFT);
    x4_rect(p, GAME_X + 8, y + 8, 2, 6, GAME_SOFT);
    x4_rect(p, GAME_X + 8, y + 12, 5, 2, GAME_SOFT);
    game_text_line(p, "game history", GAME_X + 28, y, X4_UI2_CAPTION, X4_FONT_REGULAR,
        text, GAME_TEXT_W - 28, GAME_SOFT);
}

static int draw_header(uint32_t *p, const GameHeader *header, const X4GamePageModel *model)
{
    const char *title = header->title;
    int y = GAME_TITLE_Y;
    unsigned title_lines = 0;
    while (*title && title_lines < 2) {
        char line[GAME_LINE_BYTES];
        if (title_lines == 0) title = wrapped_line(title, line, X4_UI2_TITLE, X4_FONT_BOLD);
        else snprintf(line, sizeof(line), "%s", title);
        game_text_line(p, "game title", GAME_X + GAME_TITLE_INSET, y, X4_UI2_TITLE,
            X4_FONT_BOLD, line, GAME_TEXT_W - GAME_TITLE_INSET, GAME_WHITE);
        y += GAME_TITLE_STEP;
        ++title_lines;
    }
    y += GAME_HEADER_GAP;
    if (header->metadata[0]) {
        game_text_line(p, "game metadata", GAME_X, y, X4_UI2_BODY, X4_FONT_SEMIBOLD,
            header->metadata, GAME_TEXT_W, GAME_SOFT);
        y += X4_UI2_BODY + 4 + GAME_HEADER_GAP;
    }
    int x = GAME_X;
    if (header->game_pass) x = draw_badge(p, x, y, "Game Pass", true, true);
    if (header->free_in_store)
        x = draw_badge(p, x, y, x4_tr(X4_TR_U49_FREE), true, false);
    if (!header->available || (!header->entitled && !header->free_in_store))
        x = draw_badge(p, x, y, access_label(model), false, false);
    if (header->attributes & X4_GAME_ATTR_SINGLE_PLAYER)
        x = draw_badge(p, x, y, x4_tr(X4_TR_50_SCREEN_GAME_SINGLE_PLAYER), false, false);
    if (header->attributes & X4_GAME_ATTR_ONLINE_MULTIPLAYER)
        x = draw_badge(p, x, y, x4_tr(X4_TR_50_SCREEN_GAME_ONLINE_MULTI), false, false);
    if (header->attributes & X4_GAME_ATTR_COOP)
        x = draw_badge(p, x, y, x4_tr(X4_TR_50_SCREEN_GAME_COOP), false, false);
    x = draw_badge(p, x, y, header->age_rating, false, false);
    if (x > GAME_X) y += GAME_BADGE_H + GAME_HEADER_GAP;

    const char *at = header->summary;
    const unsigned summary_lines = title_lines == 2 ? 1u : 2u;
    int bottom = y - GAME_HEADER_GAP;
    for (unsigned i = 0; *at && i < summary_lines; ++i) {
        char line[GAME_LINE_BYTES];
        if (i + 1 < summary_lines) at = wrapped_line(at, line, X4_UI2_CAPTION, X4_FONT_REGULAR);
        else snprintf(line, sizeof(line), "%s", at);
        game_text_line(p, "game summary", GAME_X, y, X4_UI2_CAPTION,
            X4_FONT_REGULAR, line, GAME_TEXT_W, GAME_WHITE);
        bottom = y + GAME_SUMMARY_H;
        y += GAME_SUMMARY_STEP;
    }
    int action_y = bottom + GAME_ACTION_GAP;
    if (header->last_session[0] || header->played_time[0]) {
        draw_history(p, bottom + GAME_HEADER_GAP, header);
        const int after_history = bottom + 2 * GAME_HEADER_GAP + GAME_SUMMARY_H;
        if (after_history > action_y) action_y = after_history;
    }
    return action_y;
}

static X4UiRect action_rect(X4GamePageFocus focus, const X4GamePageModel *model, int y)
{
    int x = GAME_X;
    if (focus == X4_GAME_FOCUS_PLAY) return (X4UiRect){x, y, 284, GAME_PLAY_H};
    x += 300;
    if (x4_product_id_valid(model->product_id)) {
        if (focus == X4_GAME_FOCUS_FAVORITE) return (X4UiRect){x, y, 324, GAME_PLAY_H};
        x += 340;
    }
    if (focus == X4_GAME_FOCUS_STORE && has_store(model))
        return (X4UiRect){x, y, 298, GAME_PLAY_H};
    return (X4UiRect){0};
}

static void draw_button(uint32_t *p, X4UiRect rect, const char *label,
    bool primary, enum X4TablerIconId icon, bool play)
{
    const int radius = GAME_ACTION_RADIUS;
    if (!rect.w) return;
    if (primary) x4_apple_fill_round_rect(p, rect.x, rect.y, rect.w, rect.h, radius, GAME_GREEN);
    else x4_ui2_card(p, rect.x, rect.y, rect.w, rect.h, radius);
    const uint32_t ink = primary ? X4_UI2_INK : GAME_WHITE;
    const int icon_space = play || icon != X4_ICON_NONE ? 34 : 0;
    int size = X4_UI2_BODY;
    int text_width = x4_text_width_px(size, X4_FONT_BOLD, label);
    const enum X4Lang language = x4_lang_current();
    if (language != X4_LANG_ES && language != X4_LANG_EN &&
        text_width > rect.w - 26 - icon_space) {
        size = X4_UI2_CAPTION;
        text_width = x4_text_width_px(size, X4_FONT_BOLD, label);
    }
    int inset = (rect.w - text_width - icon_space) / 2;
    if (inset < 13) inset = 13;
    if (play) x4_ui2_play(p, rect.x + inset, rect.y + (rect.h - 24) / 2, ink);
    else if (icon_space)
        x4_apple_draw_icon(p, rect.x + inset, rect.y + (rect.h - 24) / 2, icon, ink);
    game_text_line(p, "game action", rect.x + inset + icon_space, rect.y + (rect.h - size) / 2,
        size, X4_FONT_BOLD, label, rect.w - 2 * inset - icon_space, ink);
}

static void draw_actions(uint32_t *p, const X4GamePageModel *model, int y)
{
    draw_button(p, action_rect(X4_GAME_FOCUS_PLAY, model, y),
        model->busy ? x4_tr(X4_TR_CAT_QUERYING) : x4_tr(X4_TR_50_SCREEN_GAME_CLOUD_PLAY),
        can_play(model), X4_ICON_NONE, true);
    draw_button(p, action_rect(X4_GAME_FOCUS_FAVORITE, model, y), model->favorite ?
        x4_tr(X4_TR_U49_REMOVE_FROM_FAVORITES) :
        x4_tr(X4_TR_U49_ADD_TO_FAVORITES), false, X4_ICON_HEART, false);
    draw_button(p, action_rect(X4_GAME_FOCUS_STORE, model, y), store_label(model), false, X4_ICON_NONE, false);
}

static X4UiRect tab_rect(unsigned index)
{
    return (X4UiRect){GAME_X + (int)index * (GAME_TAB_W + GAME_TAB_GAP), GAME_TABS_Y,
        GAME_TAB_W, GAME_TAB_H};
}

static const char *tab_label(X4GamePageTab tab)
{
    switch (tab) {
    case X4_GAME_TAB_DETAILS: return x4_tr(X4_TR_50_SCREEN_GAME_DETAILS);
    case X4_GAME_TAB_SIMILAR: return x4_tr(X4_TR_50_SCREEN_GAME_SIMILAR);
    default: return x4_tr(X4_TR_50_SCREEN_GAME_SCREENSHOTS);
    }
}

static void draw_tabs(uint32_t *p, X4GamePageTab selected)
{
    for (unsigned i = 0; i < X4_GAME_TAB_COUNT; ++i) {
        const X4UiRect r = tab_rect(i);
        const bool active = i == (unsigned)selected;
        const char *label = tab_label((X4GamePageTab)i);
        const int weight = active ? X4_FONT_BOLD : X4_FONT_SEMIBOLD;
        const int width = x4_text_width_px(X4_UI2_CAPTION, weight, label);
        const int offset = (r.w - width) / 2;
        game_text_line(p, "game tab", r.x + offset, r.y + 9, X4_UI2_CAPTION, weight,
            label, r.w - offset - 12, active ? GAME_WHITE : GAME_SOFT);
        if (active)
            x4_apple_fill_round_rect(p, r.x + offset - 5, r.y + r.h - 7, width + 10, 4, 2, GAME_GREEN);
    }
}

typedef struct {
    X4GamePageTab tab;
    unsigned count, first, selected, offset, attributes;
    uint64_t description_revision;
    GameArtKey art[GAME_TILE_VISIBLE];
    uint8_t source[GAME_TILE_VISIBLE];
    char name[GAME_TILE_VISIBLE][128], publisher[128], developer[128], release_date[11];
} GamePanel;

static void panel_data(GamePanel *panel, const X4GamePage *page, const X4GamePageModel *model)
{
    memset(panel, 0, sizeof(*panel));
    panel->tab = page->tab;
    if (page->tab == X4_GAME_TAB_DETAILS) {
        panel->offset = page->description_scroll;
        (void)description_extent(description(model), &panel->offset, NULL);
        panel->description_revision = game_description.revision;
        if (model->details) {
            snprintf(panel->publisher, sizeof(panel->publisher), "%s", model->details->publisher);
            snprintf(panel->developer, sizeof(panel->developer), "%s", model->details->developer);
            snprintf(panel->release_date, sizeof(panel->release_date), "%s", model->details->release_date);
            panel->attributes = model->details->attributes;
        }
        return;
    }
    const bool similar = page->tab == X4_GAME_TAB_SIMILAR;
    panel->count = similar ? similar_count(model) : screenshot_count(model);
    panel->selected = similar ? selected_index(page->similar_index, panel->count) : screenshot_position(page);
    panel->first = first_tile(panel->selected);
    for (unsigned i = 0; i < GAME_TILE_VISIBLE && panel->first + i < panel->count; ++i) {
        const unsigned index = panel->first + i;
        panel->source[i] = (uint8_t)(similar ? index : shot_order.order[index]);
        panel->art[i] = art_key(similar ? model->similar[index].image : model->screenshots[panel->source[i]]);
        if (similar)
            snprintf(panel->name[i], sizeof(panel->name[i]), "%s", text_or(model->similar[index].title, ""));
    }
}

static int tile_x(unsigned slot)
{
    return GAME_TILE_X + (int)slot * (GAME_TILE_W + GAME_TILE_GAP);
}

typedef struct {
    GameArtKey key;
    bool ready;
    uint32_t pixels[GAME_TILE_W * GAME_TILE_H];
} GameTile;

typedef struct {
    GameTile screenshots[X4_GAME_SCREENSHOT_MAX], similar[X4_GAME_SIMILAR_MAX];
} GameTiles;

static GameTile direct_tile; /* UI thread only, used when a retained canvas is unavailable. */

static void prepare_tile(GameTile *tile, const X4CoverRaster *image)
{
    const GameArtKey key = art_key(image);
    if (key.address && same_art(tile->key, key)) return;
    tile->key = key;
    tile->ready = false;
    if (!key.address || key.width > 2048 || key.height > 2048) return;

    unsigned w = GAME_TILE_W, h = GAME_TILE_H;
    if ((uint64_t)key.width * GAME_TILE_H > (uint64_t)key.height * GAME_TILE_W)
        h = GAME_TILE_W * key.height / key.width;
    else w = GAME_TILE_H * key.width / key.height;
    if (!w) w = 1;
    if (!h) h = 1;
    if (w != GAME_TILE_W || h != GAME_TILE_H)
        for (size_t i = 0; i < GAME_TILE_W * GAME_TILE_H; ++i) tile->pixels[i] = X4_UI2_SURFACE;
    size_t start = (GAME_TILE_H - h) / 2 * GAME_TILE_W + (GAME_TILE_W - w) / 2;
    tile->ready = x4_image_area((const unsigned char *)image->rgba,
        (size_t)key.width * key.height * sizeof(*image->rgba), key.width, key.height,
        tile->pixels + start, GAME_TILE_W * GAME_TILE_H - start, w, h, GAME_TILE_W) == 0;
}

static void draw_fit_art(uint32_t *p, int x, const X4CoverRaster *image, GameTile *tile)
{
#ifdef X4_UI48_CAPTURE
    if (image && (uint64_t)image->width * 9u == (uint64_t)image->height * 16u)
        x4_ui48_capture_cover((X4UiRect){x, GAME_TILE_Y, GAME_TILE_W, GAME_TILE_H}, image);
#endif
    if (x4_apple_blit_image_exact(p, x, GAME_TILE_Y, GAME_TILE_W, GAME_TILE_H,
            GAME_TILE_RADIUS, GAME_RIGHT, image)) return;
    if (!tile) {
        tile = &direct_tile;
        tile->key = (GameArtKey){0};
        tile->ready = false;
    }
    prepare_tile(tile, image);
    if (tile->ready) {
        X4CoverRaster scaled = {GAME_TILE_W, GAME_TILE_H, tile->pixels};
        (void)x4_apple_blit_image_exact(p, x, GAME_TILE_Y, GAME_TILE_W, GAME_TILE_H,
            GAME_TILE_RADIUS, GAME_RIGHT, &scaled);
        return;
    }
    x4_ui2_card(p, x, GAME_TILE_Y, GAME_TILE_W, GAME_TILE_H, GAME_TILE_RADIUS);
    x4_apple_draw_icon(p, x + (GAME_TILE_W - 24) / 2, GAME_TILE_Y + (GAME_TILE_H - 24) / 2,
        X4_ICON_DEVICE_DESKTOP, GAME_SOFT);
}

static void draw_shelf(uint32_t *p, const GamePanel *panel, const X4GamePageModel *model, GameTiles *tiles)
{
    const bool similar = panel->tab == X4_GAME_TAB_SIMILAR;
    if (similar)
        game_text_line(p, "game panel title", GAME_TILE_X, GAME_PANEL_Y + 14, X4_UI2_LABEL, X4_FONT_BOLD,
            x4_tr(X4_TR_50_SCREEN_GAME_SAME_GENRE), GAME_TEXT_W, GAME_WHITE);
    if (!panel->count) {
        game_text_line(p, "game panel empty", GAME_TILE_X, GAME_TILE_Y + 25, X4_UI2_BODY,
            X4_FONT_REGULAR, similar ? x4_tr(X4_TR_50_SCREEN_GAME_NO_SIMILAR) :
                x4_tr(X4_TR_50_SCREEN_GAME_NO_SCREENSHOTS), GAME_TEXT_W, GAME_SOFT);
        return;
    }
    char position[48];
    unsigned end = panel->first + GAME_TILE_VISIBLE;
    if (end > panel->count) end = panel->count;
    snprintf(position, sizeof(position), "%u–%u / %u", panel->first + 1, end, panel->count);
    const int width = x4_text_width_px(X4_UI2_CAPTION, X4_FONT_REGULAR, position) + 6;
    game_text_line(p, "game panel position", GAME_RIGHT - 24 - width, GAME_PANEL_Y + 20,
        X4_UI2_CAPTION, X4_FONT_REGULAR, position, width, GAME_SOFT);
    for (unsigned i = 0; i < GAME_TILE_VISIBLE && panel->first + i < panel->count; ++i) {
        const unsigned index = panel->source[i];
        const int x = tile_x(i);
        const X4CoverRaster *image = similar ? model->similar[index].image : model->screenshots[index];
        if (similar)
            x4_ui2_card(p, x, GAME_TILE_Y, GAME_TILE_W, GAME_SIMILAR_TILE_H, GAME_TILE_RADIUS);
        GameTile *tile = tiles ? (similar ? &tiles->similar[index] : &tiles->screenshots[index]) : NULL;
        draw_fit_art(p, x, image, tile);
        if (similar) {
            game_text_line(p, "game similar title", x + 12, GAME_TILE_Y + GAME_TILE_H + 10,
                X4_UI2_CAPTION, X4_FONT_SEMIBOLD, panel->name[i], GAME_TILE_W - 24, GAME_WHITE);
        }
    }
}

static int draw_fact(uint32_t *p, int y, const char *label, const char *value)
{
    if (!value[0]) return y;
    enum { X = GAME_X + 900, W = GAME_RIGHT - X - 20 };
    game_text_line(p, "game detail label", X, y, X4_UI2_CAPTION, X4_FONT_SEMIBOLD,
        label, W, GAME_SOFT);
    game_text_line(p, "game detail value", X, y + 23, X4_UI2_BODY, X4_FONT_SEMIBOLD,
        value, W, GAME_WHITE);
    return y + 57;
}

static void draw_details_description(uint32_t *p, const GamePanel *panel,
    const X4GamePageModel *model)
{
    game_text_line(p, "game panel title", GAME_DETAIL_X, GAME_PANEL_Y + 14, X4_UI2_LABEL, X4_FONT_BOLD,
        x4_tr(X4_TR_50_SCREEN_GAME_ABOUT), GAME_TEXT_W, GAME_WHITE);
    char lines[GAME_DESC_VISIBLE][GAME_LINE_BYTES];
    unsigned offset = panel->offset;
    const unsigned max = description_extent(description(model), &offset, lines);
    for (unsigned i = 0; i < GAME_DESC_VISIBLE; ++i)
        game_text_line(p, "game description", GAME_DETAIL_X, GAME_DESC_Y + (int)i * GAME_DESC_STEP,
            GAME_DESC_SIZE, X4_FONT_REGULAR, lines[i], GAME_TEXT_W, GAME_WHITE);
    if (!game_description.line_count)
        game_text_line(p, "game panel empty", GAME_DETAIL_X, GAME_DESC_Y, GAME_DESC_SIZE, X4_FONT_REGULAR,
            x4_tr(X4_TR_50_SCREEN_GAME_NO_DESC), GAME_TEXT_W, GAME_SOFT);
    if (max) {
        const int height = GAME_DESC_STEP * GAME_DESC_VISIBLE;
        const int w = GAME_DETAIL_X + GAME_TEXT_W + 15;
        int thumb = height * GAME_DESC_VISIBLE / (int)(max + GAME_DESC_VISIBLE);
        if (thumb < 12) thumb = 12;
        x4_apple_fill_round_rect(p, w, GAME_DESC_Y, 4, height, 2, X4_UI2_BORDER);
        x4_apple_fill_round_rect(p, w, GAME_DESC_Y + (height - thumb) * (int)offset / (int)max,
            4, thumb, 2, GAME_GREEN);
    }
}

static void draw_details(uint32_t *p, const GamePanel *panel, const X4GamePageModel *model)
{
    draw_details_description(p, panel, model);
    int y = GAME_PANEL_Y + 22;
    y = draw_fact(p, y, x4_tr(X4_TR_50_SCREEN_GAME_PUBLISHER), panel->publisher);
    y = draw_fact(p, y, x4_tr(X4_TR_U49_ABOUT_DEVELOPER), panel->developer);
    y = draw_fact(p, y, x4_tr(X4_TR_50_SCREEN_GAME_RELEASE_DATE), panel->release_date);
    char controls[128] = {0};
    if (panel->attributes & X4_GAME_ATTR_GAMEPAD)
        append_metadata(controls, sizeof(controls), x4_tr(X4_TR_50_SCREEN_GAME_CONTROLLER));
    if (panel->attributes & X4_GAME_ATTR_KEYBOARD_MOUSE)
        append_metadata(controls, sizeof(controls), x4_tr(X4_TR_50_SCREEN_GAME_KEYBOARD_MOUSE));
    if (panel->attributes & X4_GAME_ATTR_TOUCH)
        append_metadata(controls, sizeof(controls), x4_tr(X4_TR_50_SCREEN_GAME_TOUCH));
    (void)draw_fact(p, y, x4_tr(X4_TR_50_SCREEN_GAME_INPUT), controls);
    if (game_description.line_count > GAME_DESC_VISIBLE)
        game_text_line(p, "game detail scroll", GAME_DETAIL_X, GAME_PANEL_Y + GAME_PANEL_H - 40,
            X4_UI2_CAPTION, X4_FONT_REGULAR,
            x4_tr(X4_TR_50_SCREEN_GAME_DETAILS_SCROLL),
            GAME_TEXT_W, GAME_SOFT);
}

/* The pen is not the edge of its ink: the Noto capital J alone can
 * start 4 px to its left. Include the packaged fonts' negative bearings.
 * The title inset keeps even the widest bearing clear of the sidebar. */
static const X4UiRect head_rect = {GAME_X - GAME_HEADER_INK_LEFT, GAME_HEAD_Y,
    GAME_RIGHT - GAME_X + GAME_HEADER_INK_LEFT, GAME_TABS_Y - GAME_HEAD_Y - GAME_FOCUS_PAD};
static const X4UiRect tabs_rect = {GAME_X, GAME_TABS_Y,
    X4_GAME_TAB_COUNT * (GAME_TAB_W + GAME_TAB_GAP) - GAME_TAB_GAP, GAME_TAB_H};
static const X4UiRect panel_rect = {GAME_X, GAME_PANEL_Y, GAME_RIGHT - GAME_X, GAME_PANEL_H};
/* The heading descenders and the first line's top bearing can overlap
 * the scroll damage. Restore and compose both on the opaque panel interior. */
static const X4UiRect description_rect = {
    GAME_DETAIL_X - GAME_DESCRIPTION_INK_LEFT, GAME_DESCRIPTION_RESTORE_Y,
    GAME_TEXT_W + 20 + GAME_DESCRIPTION_INK_LEFT,
    GAME_DESC_Y + GAME_DESC_STEP * GAME_DESC_VISIBLE - GAME_DESCRIPTION_RESTORE_Y,
};
static const X4UiRect notice_rect = {1328, 126, GAME_RIGHT - 1328, 122};

static void draw_panel(uint32_t *p, const GamePanel *panel, const X4GamePageModel *model, GameTiles *tiles)
{
    if (panel->tab == X4_GAME_TAB_DETAILS) {
        x4_ui2_card(p, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h, 24);
        draw_details(p, panel, model);
    } else draw_shelf(p, panel, model, tiles);
}

static X4UiRect focus_rect(const X4GamePage *page, const X4GamePageModel *model,
    const GamePanel *panel, int action_y, int *radius)
{
    X4UiRect rect = {0};
    if (action_focus(page->focus)) {
        rect = action_rect(page->focus, model, action_y);
        *radius = GAME_ACTION_RADIUS;
    } else if (page->focus == X4_GAME_FOCUS_TABS) {
        rect = tab_rect(page->tab);
        *radius = 8;
    } else if (page->focus == X4_GAME_FOCUS_DETAILS) {
        rect = (X4UiRect){GAME_DETAIL_X - 9, GAME_DESC_Y - 9, GAME_TEXT_W + 38,
            GAME_DESC_STEP * GAME_DESC_VISIBLE + 18};
        *radius = 18;
    } else if (panel->count) {
        const bool similar = panel->tab == X4_GAME_TAB_SIMILAR;
        rect = (X4UiRect){tile_x(panel->selected - panel->first), GAME_TILE_Y, GAME_TILE_W,
            similar ? GAME_SIMILAR_TILE_H : GAME_TILE_H};
        *radius = GAME_TILE_RADIUS;
    }
    return rect;
}

static bool same_rect(X4UiRect a, X4UiRect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static X4UiRect padded(X4UiRect rect)
{
    return (X4UiRect){rect.x - GAME_FOCUS_PAD, rect.y - GAME_FOCUS_PAD,
        rect.w + 2 * GAME_FOCUS_PAD, rect.h + 2 * GAME_FOCUS_PAD};
}

static int draw_frame(uint32_t *p, const X4GamePage *page, const X4GamePageModel *model,
    const GameHeader *header, const GamePanel *panel, GameTiles *tiles)
{
    const int action_y = draw_header(p, header, model);
    draw_actions(p, model, action_y);
    draw_tabs(p, page->tab);
    draw_panel(p, panel, model, tiles);
    draw_notice(p, model->notice);
    draw_footer(p, false);
    return action_y;
}

void x4_game_page_draw(const X4GamePage *page, const X4GamePageModel *model, uint32_t *p)
{
    if (!page || !model || !p) return;
    prepare_shot_order(page, model);
    if (x4_game_page_fullscreen_index(page) >= 0) {
        draw_fullscreen(p, page, model);
        return;
    }
    GameHeader header;
    GamePanel panel;
    header_data(&header, model);
    panel_data(&panel, page, model);
    draw_backdrop(p, model->background);
    const int action_y = draw_frame(p, page, model, &header, &panel, NULL);
    int radius = 0;
    const X4UiRect focus = focus_rect(page, model, &panel, action_y, &radius);
    if (focus.w) x4_ui2_focus(p, focus.x, focus.y, focus.w, focus.h, radius);
    if (page->qr_open) draw_store(p, model);
}

typedef struct {
    uint32_t backdrop[GAME_W * GAME_H];
    GameTiles tiles;
    uint32_t underlay[(GAME_TEXT_W + 38 + 2 * GAME_FOCUS_PAD) *
        (GAME_DESC_STEP * GAME_DESC_VISIBLE + 18 + 2 * GAME_FOCUS_PAD)];
} GamePixels;

/* Composition belongs to the UI thread. The backdrop and focus underlay
 * hold owned pixels; saved asset keys are compared but never dereferenced. */
static struct {
    GamePixels *pixels;
    const X4GamePage *owner;
    uint32_t *canvas;
    GameArtKey background, full_art;
    X4UiRect focus;
    uint64_t generation;
    enum X4Lang lang, backdrop_lang;
    GameHeader header;
    GamePanel panel;
    char product[X4_PRODUCT_ID_CAP], notice[512];
    int fullscreen, action_y;
    bool backdrop_valid, qr_open, busy, favorite, store;
} game_cache;

void x4_game_page_cache_invalidate(void)
{
    game_cache.canvas = NULL;
}

void x4_game_page_cache_dispose(void)
{
    free(game_cache.pixels);
    memset(&game_cache, 0, sizeof(game_cache));
    memset(&shot_order, 0, sizeof(shot_order));
}

static void restore_region(uint32_t *p, X4UiRect rect, X4UiRect *damage, size_t *length)
{
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        const size_t start = (size_t)y * GAME_W + rect.x;
        memcpy(p + start, game_cache.pixels->backdrop + start, (size_t)rect.w * sizeof(*p));
    }
    damage[(*length)++] = rect;
}

static void focus_underlay(uint32_t *p, X4UiRect rect, bool save)
{
    uint32_t *underlay = game_cache.pixels->underlay;
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        uint32_t *row = p + (size_t)y * GAME_W + rect.x;
        if (save) memcpy(underlay, row, (size_t)rect.w * sizeof(*p));
        else memcpy(row, underlay, (size_t)rect.w * sizeof(*p));
        underlay += rect.w;
    }
}

size_t x4_game_page_draw_cached(const X4GamePage *page, const X4GamePageModel *model,
    uint64_t background_generation, uint32_t *p, bool persistent,
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX])
{
    if (!page || !model || !p || !damage) return 0;
    if (game_cache.owner != page) {
        x4_game_page_cache_dispose();
        game_cache.owner = page;
    }
    prepare_shot_order(page, model);
    const enum X4Lang lang = x4_lang_current();
    const int fullscreen = x4_game_page_fullscreen_index(page);
    bool full = !persistent || game_cache.canvas != p || game_cache.lang != lang ||
        game_cache.fullscreen != fullscreen || game_cache.qr_open != page->qr_open;
    game_cache.canvas = persistent ? p : NULL;
    game_cache.lang = lang;
    game_cache.fullscreen = fullscreen;
    game_cache.qr_open = page->qr_open;
    if (fullscreen >= 0) {
        const GameArtKey art = art_key(fullscreen_art(model, fullscreen));
        const unsigned count = screenshot_count(model);
        full |= !same_art(art, game_cache.full_art) || game_cache.panel.count != count;
        game_cache.full_art = art;
        game_cache.panel.count = count;
        if (full) draw_fullscreen(p, page, model);
        return full ? SIZE_MAX : 0;
    }

    if (!game_cache.pixels) game_cache.pixels = calloc(1, sizeof(*game_cache.pixels));
    const char *product = text_or(model->product_id, ""), *notice = text_or(model->notice, "");
    if (!game_cache.pixels || strlen(product) >= sizeof(game_cache.product) ||
        strlen(notice) >= sizeof(game_cache.notice)) {
        x4_game_page_draw(page, model, p);
        game_cache.canvas = NULL;
        return SIZE_MAX;
    }
    const GameArtKey background = art_key(model->background);
    if (!game_cache.backdrop_valid || !same_art(game_cache.background, background) ||
        game_cache.generation != background_generation || game_cache.backdrop_lang != lang) {
        draw_backdrop(game_cache.pixels->backdrop, model->background);
        game_cache.backdrop_valid = true;
        game_cache.backdrop_lang = lang;
        game_cache.background = background;
        game_cache.generation = background_generation;
        full = true;
    }
    if (strcmp(game_cache.product, product)) {
        for (unsigned i = 0; i < X4_GAME_SCREENSHOT_MAX; ++i)
            game_cache.pixels->tiles.screenshots[i].key = (GameArtKey){0};
        for (unsigned i = 0; i < X4_GAME_SIMILAR_MAX; ++i)
            game_cache.pixels->tiles.similar[i].key = (GameArtKey){0};
        full = true;
    }

    GameHeader header;
    GamePanel panel;
    header_data(&header, model);
    panel_data(&panel, page, model);
    int action_y = game_cache.action_y;
    int radius = 0;
    X4UiRect raw_focus = focus_rect(page, model, &panel, action_y, &radius);
    X4UiRect focus = raw_focus.w ? padded(raw_focus) : (X4UiRect){0};

    const bool header_changed = memcmp(&header, &game_cache.header, sizeof(header)) != 0;
    const bool store = has_store(model);
    const bool actions_changed = game_cache.busy != model->busy ||
        game_cache.favorite != model->favorite || game_cache.store != store;
    const bool tab_changed = panel.tab != game_cache.panel.tab;
    const bool notice_changed = strcmp(game_cache.notice, notice) != 0;
    /* Focus movement inside a shelf affects only its two focus rectangles.
     * The visible range is part of the key, the position inside it is not. */
    GamePanel visual = panel;
    visual.selected = game_cache.panel.selected;
    const bool panel_changed = memcmp(&visual, &game_cache.panel, sizeof(visual)) != 0;
    visual.offset = game_cache.panel.offset;
    const bool description_only = panel.tab == X4_GAME_TAB_DETAILS && panel_changed &&
        !memcmp(&visual, &game_cache.panel, sizeof(visual));
    const bool focus_changed = !same_rect(game_cache.focus, focus);
    if (!full && !header_changed && !actions_changed && !tab_changed && !notice_changed &&
        !panel_changed && !focus_changed) return 0;
    if (page->qr_open) full = true;

    size_t length = 0;
    if (full) {
        memcpy(p, game_cache.pixels->backdrop, sizeof(game_cache.pixels->backdrop));
        action_y = draw_frame(p, page, model, &header, &panel, &game_cache.pixels->tiles);
    } else {
        if (game_cache.focus.w) {
            focus_underlay(p, game_cache.focus, false);
            damage[length++] = game_cache.focus;
        }
        if (header_changed) {
            restore_region(p, head_rect, damage, &length);
            action_y = draw_header(p, &header, model);
            draw_actions(p, model, action_y);
        } else if (actions_changed) {
            restore_region(p, (X4UiRect){GAME_X, action_y, GAME_RIGHT - GAME_X, GAME_PLAY_H},
                damage, &length);
            draw_actions(p, model, action_y);
        }
        if (tab_changed) {
            restore_region(p, tabs_rect, damage, &length);
            draw_tabs(p, page->tab);
        }
        if (description_only) {
            x4_rect(p, description_rect.x, description_rect.y, description_rect.w,
                description_rect.h, X4_UI2_SURFACE);
            damage[length++] = description_rect;
            draw_details_description(p, &panel, model);
        } else if (panel_changed) {
            restore_region(p, panel_rect, damage, &length);
            draw_panel(p, &panel, model, &game_cache.pixels->tiles);
        }
        if (notice_changed || (header_changed && notice[0])) {
            restore_region(p, notice_rect, damage, &length);
            draw_notice(p, notice);
        }
    }
    if (action_y != game_cache.action_y) {
        raw_focus = focus_rect(page, model, &panel, action_y, &radius);
        focus = raw_focus.w ? padded(raw_focus) : (X4UiRect){0};
        game_cache.action_y = action_y;
    }
    if (focus.w) {
        focus_underlay(p, focus, true);
        x4_ui2_focus(p, raw_focus.x, raw_focus.y, raw_focus.w, raw_focus.h, radius);
        if (!full) damage[length++] = focus;
    }
    if (page->qr_open) draw_store(p, model);
    game_cache.focus = focus;
    game_cache.header = header;
    game_cache.panel = panel;
    game_cache.busy = model->busy;
    game_cache.favorite = model->favorite;
    game_cache.store = store;
    strcpy(game_cache.notice, notice);
    strcpy(game_cache.product, product);
    return full ? SIZE_MAX : length;
}
