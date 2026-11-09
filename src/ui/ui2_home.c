/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_home.h"
#include "ui2_theme.h"
#include "canvas.h"
#include "image_area.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CONTENT_LEFT = X4_UI2_CONTENT_X - X4_UI2_FOCUS_SPREAD - 1,
    CONTENT_RIGHT = X4_UI_SAFE_RIGHT, CONTENT_TOP = 126, CONTENT_BOTTOM = 1024,
    CONTENT_WIDTH = CONTENT_RIGHT - CONTENT_LEFT,
    CONTENT_HEIGHT = CONTENT_BOTTOM - CONTENT_TOP,
    CARD_X = X4_UI2_CONTENT_X, CARD_Y = 234, CARD_H = 244, HERO_W = 1044,
    EXPLORE_X = 1336, EXPLORE_W = 488,
    TILE_W = 224, TILE_H = 126, TILE_STEP = 263, TILE_TITLE_GAP = 10,
    RECENT_HEADING_Y = 498, RECENT_TILE_Y = 542,
    POPULAR_HEADING_Y = 726, POPULAR_TILE_Y = 770,
    HERO_ART_W = 416, HERO_ART_H = 234,
    HERO_TITLE_Y = 60, HERO_TITLE_LINE = 42, HERO_HISTORY_Y = 154, HERO_BUTTON_Y = 186,
};
_Static_assert(TILE_W * 9 == TILE_H * 16 && HERO_ART_W * 9 == HERO_ART_H * 16,
    "Home artwork uses 16:9 apertures");
_Static_assert(CARD_X - X4_UI2_FOCUS_SPREAD >= CONTENT_LEFT &&
    CARD_X + (X4_UI2_HOME_POPULAR_MAX - 1) * TILE_STEP + TILE_W + X4_UI2_FOCUS_SPREAD <= CONTENT_RIGHT,
    "Focus damage remains in Home content");
_Static_assert(POPULAR_TILE_Y + TILE_H + TILE_TITLE_GAP + X4_UI2_CAPTION + 4 <= X4_UI_FOOTER_Y - 24,
    "Home leaves at least 24 px between game names and the footer");
_Static_assert(HERO_HISTORY_Y - (HERO_TITLE_Y + HERO_TITLE_LINE + 44) >= 8 &&
    HERO_BUTTON_Y - (HERO_HISTORY_Y + 24) >= 8,
    "Home play history is clear of the longest title and the button");
_Static_assert(RECENT_HEADING_Y - (CARD_Y + CARD_H) >= 20 &&
    RECENT_TILE_Y - (RECENT_HEADING_Y + X4_UI2_HEADING) >= 12 &&
    POPULAR_TILE_Y - (POPULAR_HEADING_Y + X4_UI2_HEADING) >= 12,
    "Home cards and headings leave space between sections");
_Static_assert(POPULAR_HEADING_Y - (RECENT_TILE_Y + TILE_H + TILE_TITLE_GAP + X4_UI2_CAPTION + 4) >= 24,
    "Home row names clear the next heading");
static const char fortnite_product[] = "BT5P2X999VH2";

typedef struct {
    char product[X4_PRODUCT_ID_CAP];
    char name[128];
} HomeItem;

typedef struct {
    enum X4Lang language;
    enum X4Ui2HomeMode mode;
    unsigned count, recents, popular;
    char gamertag[32];
    char history[160];
    HomeItem items[X4_UI2_HOME_ITEM_COUNT];
} HomeKey;

typedef struct {
    uintptr_t address;
    unsigned width, height;
    uint64_t generation, signature;
} HomeArtKey;

static struct {
    HomeKey key;
    HomeArtKey art[X4_UI2_HOME_ITEM_COUNT];
    uint32_t *unfocused;
    const uint32_t *canvas;
    X4CatalogAssets *assets;
    uint64_t asset_revision;
    int focus;
    bool ready, allocation_attempted;
} home;

static HomeKey pending_key;
static const X4History *home_history;
static struct {
    const X4CatalogSnapshot *catalog;
    uint64_t revision;
    unsigned count, sources[X4_UI2_HOME_RECENT_MAX + 1];
} history_row;

void x4_ui2_home_catalog_changed(void)
{
    history_row.catalog = NULL;
}

void x4_ui2_home_history(const X4History *history)
{
    home_history = history;
    x4_ui2_home_catalog_changed();
}
static uint32_t resized[HERO_ART_W * HERO_ART_H];

static bool valid_catalog(const X4CatalogSnapshot *catalog)
{
    return catalog && catalog->state == X4_CATALOG_READY && catalog->count <= X4_CATALOG_MAX;
}

static bool valid_source(const X4CatalogSnapshot *catalog, unsigned source)
{
    return valid_catalog(catalog) && source < catalog->count &&
        x4_product_id_valid(catalog->titles[source].product_id);
}

static bool is_fortnite(const char *id)
{
    for (size_t i = 0; i < sizeof(fortnite_product); ++i) {
        unsigned char c = (unsigned char)id[i];
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c != (unsigned char)fortnite_product[i]) return false;
    }
    return true;
}

static unsigned fortnite_source(const X4CatalogSnapshot *catalog)
{
    if (valid_catalog(catalog))
        for (unsigned i = 0; i < catalog->count; ++i)
            if (is_fortnite(catalog->titles[i].product_id)) return i;
    return X4_CATALOG_MAX;
}

static void add_recent(X4Ui2HomeModel *model, unsigned source)
{
    if (model->mode != X4_UI2_HOME_WITH_RECENT) {
        model->mode = X4_UI2_HOME_WITH_RECENT;
        model->continuation = source;
        return;
    }
    if (source == model->continuation || model->recent_count == X4_UI2_HOME_RECENT_MAX) return;
    for (unsigned i = 0; i < model->recent_count; ++i)
        if (model->recent[i] == source) return;
    model->recent[model->recent_count++] = source;
}

static void update_history_row(const X4CatalogSnapshot *catalog)
{
    if (history_row.catalog == catalog && history_row.revision == home_history->revision) return;
    history_row.catalog = catalog;
    history_row.revision = home_history->revision;
    history_row.count = 0;
    uint64_t last[X4_UI2_HOME_RECENT_MAX + 1] = {0};
    const unsigned capacity = sizeof(last) / sizeof(*last);
    for (unsigned source = 0; source < catalog->count; ++source) {
        const X4HistoryEntry *entry = x4_history_find(home_history, catalog->titles[source].product_id);
        if (!entry) continue;
        unsigned i = 0;
        while (i < history_row.count && last[i] >= entry->last_session_s) ++i;
        if (i == capacity) continue;
        if (history_row.count < capacity) ++history_row.count;
        for (unsigned j = history_row.count - 1; j > i; --j) {
            last[j] = last[j - 1];
            history_row.sources[j] = history_row.sources[j - 1];
        }
        last[i] = entry->last_session_s;
        history_row.sources[i] = source;
    }
}

static void fill_recent(X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    const X4RecentGames *recent)
{
    model->mode = X4_UI2_HOME_EMPTY;
    if (valid_catalog(catalog) && home_history && home_history->count) {
        update_history_row(catalog);
        for (unsigned i = 0; i < history_row.count; ++i)
            add_recent(model, history_row.sources[i]);
    }
    if (model->mode == X4_UI2_HOME_EMPTY && recent && recent->state == X4_RECENT_ERROR)
        model->mode = X4_UI2_HOME_ERROR;
    else if (model->mode == X4_UI2_HOME_EMPTY && recent && recent->state == X4_RECENT_LOADING)
        model->mode = X4_UI2_HOME_LOADING;
    for (unsigned i = 0; i < x4_recent_count(recent) &&
            model->recent_count < X4_UI2_HOME_RECENT_MAX; ++i) {
        unsigned source;
        if (x4_recent_catalog_index(x4_recent_get(recent, i), catalog, &source) &&
                valid_source(catalog, source))
            add_recent(model, source);
    }
}

static void fill_popular(X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    const X4HomeRows *rows)
{
    model->popular[model->popular_count++] = fortnite_source(catalog);
    if (!valid_catalog(catalog) || !rows || rows->count > X4_HOME_ROW_COUNT) return;
    for (unsigned r = 0; r < rows->count; ++r) {
        const X4HomeRow *row = &rows->rows[r];
        if (row->kind != X4_HOME_POPULAR || row->count > X4_HOME_ROW_MAX) continue;
        for (unsigned i = 0; i < row->count && model->popular_count < X4_UI2_HOME_POPULAR_MAX; ++i) {
            unsigned source = row->indices[i];
            if (valid_source(catalog, source) && !is_fortnite(catalog->titles[source].product_id))
                model->popular[model->popular_count++] = source;
        }
        return;
    }
}

void x4_ui2_home_model(X4Ui2HomeModel *out, bool signed_in,
    const X4CatalogSnapshot *catalog, const X4RecentGames *recent, const X4HomeRows *rows)
{
    if (!out) return;
    *out = (X4Ui2HomeModel){.continuation = X4_CATALOG_MAX};
    if (!signed_in) {
        out->mode = X4_UI2_HOME_SIGNED_OUT;
        fill_popular(out, NULL, NULL);
        return;
    }
    if (valid_catalog(catalog)) out->catalog_count = catalog->count;
    fill_recent(out, catalog, recent);
    fill_popular(out, catalog, rows);
}

static unsigned row_count(const X4Ui2HomeModel *model, unsigned row)
{
    if (!model) return 0;
    switch (row) {
        case 0: return 2;
        case 1:
            if (model->mode == X4_UI2_HOME_SIGNED_OUT) return 0;
            return model->recent_count <= X4_UI2_HOME_RECENT_MAX ? model->recent_count + 1 : 1;
        default:
            return model->popular_count <= X4_UI2_HOME_POPULAR_MAX ? model->popular_count : 0;
    }
}

static int row_item(const X4Ui2HomeModel *model, unsigned row, unsigned col)
{
    if (row == 0) return (int)col;
    if (row == 1)
        return col == row_count(model, row) - 1u ? X4_UI2_HOME_RECENT_ALL :
            X4_UI2_HOME_RECENT_FIRST + (int)col;
    return X4_UI2_HOME_POPULAR_FIRST + (int)col;
}

static bool item_position(const X4Ui2HomeModel *model, int selected,
    unsigned *row, unsigned *col)
{
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < row_count(model, r); ++c)
            if (row_item(model, r, c) == selected) {
                *row = r;
                *col = c;
                return true;
            }
    return false;
}

int x4_ui2_home_normalize(const X4Ui2HomeModel *model, int selected)
{
    unsigned row, col;
    return item_position(model, selected, &row, &col) ? selected : X4_UI2_HOME_CONTINUE;
}

int x4_ui2_home_move(const X4Ui2HomeModel *model, int selected, enum X4Ui2HomeDirection direction)
{
    unsigned row, col;
    if (!item_position(model, selected, &row, &col)) return X4_UI2_HOME_CONTINUE;
    unsigned count = row_count(model, row);
    switch (direction) {
        case X4_UI2_HOME_LEFT:
            return col ? row_item(model, row, col - 1) : selected;
        case X4_UI2_HOME_RIGHT:
            return col + 1 < count ? row_item(model, row, col + 1) : selected;
        case X4_UI2_HOME_UP:
            if (!row) return selected;
            do { --row; } while (row && !row_count(model, row));
            if (!row) return col < 3 ? X4_UI2_HOME_CONTINUE : X4_UI2_HOME_EXPLORE;
            break;
        case X4_UI2_HOME_DOWN:
            if (row == 2) return selected;
            if (!row) {
                row = row_count(model, 1) ? 1 : 2;
                count = row_count(model, row);
                return count ? row_item(model, row, col ? count - 1 : 0) : selected;
            }
            row = 2;
            break;
    }
    count = row_count(model, row);
    return count ? row_item(model, row, col < count ? col : count - 1) : selected;
}

X4Ui2HomeRoute x4_ui2_home_activate(const X4Ui2HomeModel *model, int selected)
{
    X4Ui2HomeRoute route = {.source = X4_CATALOG_MAX};
    unsigned row, col;
    if (!item_position(model, selected, &row, &col)) return route;
    switch (selected) {
        case X4_UI2_HOME_CONTINUE:
            if (model->mode == X4_UI2_HOME_SIGNED_OUT) route.action = X4_UI2_HOME_LOGIN;
            else if (model->mode == X4_UI2_HOME_WITH_RECENT) {
                route.action = X4_UI2_HOME_GAME;
                route.source = model->continuation;
            } else route.action = X4_UI2_HOME_RECENTS;
            return route;
        case X4_UI2_HOME_EXPLORE:
            route.action = X4_UI2_HOME_CATALOG;
            return route;
        case X4_UI2_HOME_RECENT_ALL:
            route.action = X4_UI2_HOME_RECENTS;
            return route;
        case X4_UI2_HOME_POPULAR_FIRST:
            route.action = X4_UI2_HOME_FORTNITE;
            return route;
        default:
            route.source = row == 1 ? model->recent[col] : model->popular[col];
            if (route.source < X4_CATALOG_MAX) route.action = X4_UI2_HOME_GAME;
            return route;
    }
}

X4UiRect x4_ui2_home_bounds(void)
{
    return (X4UiRect){CONTENT_LEFT, CONTENT_TOP, CONTENT_WIDTH, CONTENT_HEIGHT};
}

bool x4_ui2_home_item_bounds(const X4Ui2HomeModel *model, int selected, X4UiRect *out)
{
    unsigned row, col;
    if (!out || !item_position(model, selected, &row, &col)) return false;
    if (!row)
        *out = selected == X4_UI2_HOME_CONTINUE ? (X4UiRect){CARD_X, CARD_Y, HERO_W, CARD_H} :
            (X4UiRect){EXPLORE_X, CARD_Y, EXPLORE_W, CARD_H};
    else
        *out = (X4UiRect){CARD_X + (int)col * TILE_STEP,
            row == 1 ? RECENT_TILE_Y : POPULAR_TILE_Y, TILE_W, TILE_H};
    return true;
}

bool x4_ui2_home_art_bounds(const X4Ui2HomeModel *model, int selected, X4UiRect *out)
{
    if (!out || !x4_ui2_home_item_bounds(model, selected, out)) return false;
    if (selected == X4_UI2_HOME_CONTINUE && model->mode == X4_UI2_HOME_WITH_RECENT)
        *out = (X4UiRect){out->x + out->w - HERO_ART_W - 15,
            out->y + (out->h - HERO_ART_H) / 2, HERO_ART_W, HERO_ART_H};
    else if (selected < X4_UI2_HOME_RECENT_FIRST || selected == X4_UI2_HOME_RECENT_ALL)
        return false;
    return true;
}

static void copy_label(char *out, size_t capacity, const char *source, size_t limit)
{
    const char *end = memchr(source, 0, limit);
    size_t n = end ? (size_t)(end - source) : limit;
    if (n >= capacity) n = capacity - 1;
    memcpy(out, source, n);
    out[n] = 0;
}

static void set_game(HomeItem *item, const X4CatalogSnapshot *catalog, unsigned source)
{
    if (!valid_source(catalog, source)) return;
    const X4CatalogTitle *title = &catalog->titles[source];
    copy_label(item->product, sizeof(item->product), title->product_id, sizeof(title->product_id));
    copy_label(item->name, sizeof(item->name), title->name, sizeof(title->name));
}

static void make_key(HomeKey *key, const X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog)
{
    memset(key, 0, sizeof(*key));
    key->language = x4_lang_current();
    key->mode = model->mode;
    key->count = model->catalog_count;
    key->recents = row_count(model, 1);
    key->popular = row_count(model, 2);
    if (model->mode != X4_UI2_HOME_SIGNED_OUT && catalog)
        copy_label(key->gamertag, sizeof(key->gamertag), catalog->gamertag, sizeof(catalog->gamertag));
    set_game(&key->items[X4_UI2_HOME_CONTINUE], catalog, model->continuation);
    if (model->mode == X4_UI2_HOME_WITH_RECENT) {
        const X4HistoryEntry *entry = x4_history_find(home_history, key->items[X4_UI2_HOME_CONTINUE].product);
        if (entry) {
            char last[X4_HISTORY_LABEL], played[X4_HISTORY_LABEL];
            bool english = key->language == X4_LANG_EN;
            x4_history_text(entry, x4_history_now(), english, last, played);
            snprintf(key->history, sizeof(key->history), "%s%s%s%s%s",
                last[0] ? x4_tr(X4_TR_U49_HOME_LAST_SESSION_PREFIX) : "", last,
                last[0] && played[0] ? " · " : "",
                played[0] ? x4_tr(X4_TR_U49_HOME_PLAY_TIME_PREFIX) : "", played);
        }
    }
    for (unsigned i = 0; i + 1 < key->recents; ++i)
        set_game(&key->items[X4_UI2_HOME_RECENT_FIRST + i], catalog, model->recent[i]);
    for (unsigned i = 0; i < key->popular; ++i)
        set_game(&key->items[X4_UI2_HOME_POPULAR_FIRST + i], catalog, model->popular[i]);
    if (key->popular) {
        HomeItem *first = &key->items[X4_UI2_HOME_POPULAR_FIRST];
        if (!first->product[0]) memcpy(first->product, fortnite_product, sizeof(fortnite_product));
        if (!first->name[0]) memcpy(first->name, "Fortnite", sizeof("Fortnite"));
    }
}

static void copy_unfocused(uint32_t *pixels, X4UiRect rect, bool save)
{
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        uint32_t *screen = pixels + (size_t)y * X4_UI_WIDTH + rect.x;
        uint32_t *saved = home.unfocused + (size_t)(y - CONTENT_TOP) * CONTENT_WIDTH + rect.x - CONTENT_LEFT;
        if (save) memcpy(saved, screen, (size_t)rect.w * sizeof(*saved));
        else memcpy(screen, saved, (size_t)rect.w * sizeof(*screen));
    }
}

static bool raster_valid(const X4CoverRaster *art)
{
    return art && art->rgba && art->width > 0 && art->height > 0 &&
        art->width <= X4_UI_WIDTH && art->height <= X4_UI_HEIGHT;
}

static const X4CoverRaster *pick_art(const X4CatalogAssetView *view)
{
    static const enum X4CoverSurface surfaces[] = {
        X4_COVER_ROW, X4_COVER_DETAIL, X4_COVER_GRID
    };
    for (unsigned i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); ++i)
        if (raster_valid(&view->surface[surfaces[i]])) return &view->surface[surfaces[i]];
    return NULL;
}

static HomeArtKey art_key(const X4CoverRaster *r, uint64_t generation)
{
    if (!r) return (HomeArtKey){0};
    uint64_t signature = UINT64_C(14695981039346656037);
    size_t size = (size_t)r->width * r->height;
    for (size_t i = 0; i < 12; ++i) {
        signature ^= r->rgba[(size - 1) * i / 11];
        signature *= UINT64_C(1099511628211);
    }
    return (HomeArtKey){(uintptr_t)r->rgba, r->width, r->height, generation, signature};
}

static bool equal_art(HomeArtKey a, HomeArtKey b)
{
    return a.address == b.address && a.width == b.width && a.height == b.height &&
        a.generation == b.generation && a.signature == b.signature;
}

static void artwork(uint32_t *pixels, X4UiRect rect, const X4CoverRaster *r, int radius)
{
    x4_ui2_card(pixels, rect.x, rect.y, rect.w, rect.h, radius);
    if (raster_valid(r)) {
        unsigned width = (unsigned)rect.w, height = (unsigned)rect.h;
        if (r->width * height > r->height * width) height = width * r->height / r->width;
        else width = height * r->width / r->height;
        if (width && height) {
            X4CoverRaster fitted = {width, height, resized};
            size_t source_bytes = (size_t)r->width * r->height * sizeof(*r->rgba);
            int x = rect.x + (rect.w - (int)width) / 2;
            int y = rect.y + (rect.h - (int)height) / 2;
            if (!x4_image_area((const unsigned char *)r->rgba, source_bytes, r->width, r->height,
                    resized, sizeof(resized) / sizeof(resized[0]), width, height, width) &&
                x4_apple_blit_image_exact(pixels, x, y, (int)width, (int)height, radius, rect.x + rect.w, &fitted))
                return;
        }
    }
    x4_apple_draw_icon(pixels, rect.x + (rect.w - 24) / 2, rect.y + (rect.h - 24) / 2,
        X4_ICON_DEVICE_GAMEPAD, X4_UI2_MUTED);
}

static void draw_header(uint32_t *pixels, const HomeKey *key)
{
    char greeting[76];
    const char *title = x4_tr(X4_TR_U49_HOME_WELCOME);
    if (key->gamertag[0]) {
        snprintf(greeting, sizeof(greeting), x4_tr(X4_TR_U49_HOME_WELCOME_FMT), key->gamertag);
        title = greeting;
    }
    x4_text_ellipsis_px(pixels, CARD_X, 132, 44, X4_FONT_BOLD, title,
        CONTENT_RIGHT - CARD_X, X4_UI2_TEXT);
    x4_apple_fill_round_rect(pixels, CARD_X, 185, 72, 3, 1, X4_UI2_ACCENT);
    x4_text_px(pixels, CARD_X, 196, 26, X4_FONT_REGULAR,
        x4_tr(X4_TR_U49_HOME_SUBTITLE), X4_UI2_MUTED);
    x4_text_px(pixels, CARD_X, RECENT_HEADING_Y, X4_UI2_HEADING, X4_FONT_BOLD,
        x4_tr(X4_TR_U49_HOME_CONTINUE), X4_UI2_TEXT);
    x4_text_px(pixels, CARD_X, POPULAR_HEADING_Y, X4_UI2_HEADING, X4_FONT_BOLD,
        x4_tr(X4_TR_U49_POPULAR_CLOUD), X4_UI2_TEXT);
    if (key->mode == X4_UI2_HOME_SIGNED_OUT)
        x4_text_px(pixels, CARD_X, RECENT_TILE_Y + 27, 26, X4_FONT_REGULAR,
            x4_tr(X4_TR_U49_HOME_SIGN_IN_RECENTS), X4_UI2_MUTED);
    else if (key->recents == 1)
        x4_text_px(pixels, CARD_X + TILE_STEP + 10, RECENT_TILE_Y + 48, 26, X4_FONT_REGULAR,
            x4_tr(X4_TR_U49_HOME_FUTURE_RECENTS), X4_UI2_MUTED);
    const X4Ui2Hint hints[] = {
        {X4_GLYPH_CROSS, x4_tr(X4_TR_U49_SELECT)},
        {X4_GLYPH_TRIANGLE, x4_tr(X4_TR_U49_ACCOUNT_TITLE)},
        {X4_GLYPH_OPTIONS, x4_tr(X4_TR_U49_SETTINGS)},
    };
    x4_ui2_footer(pixels, hints, sizeof(hints) / sizeof(hints[0]));
}

static int home_label_width(const char *label, int size, int weight, int padding,
    int minimum, int available)
{
    int width = x4_text_width_px(size, weight, label) + padding;
    if (width < minimum) width = minimum;
    return width < available ? width : available;
}

static void draw_continue(uint32_t *pixels, X4UiRect rect, const HomeKey *key, const X4CoverRaster *art)
{
    const char *title = x4_tr(X4_TR_U49_HOME_NEXT_GAME);
    const char *description = x4_tr(X4_TR_U49_HOME_EMPTY_RECENTS);
    const char *button = x4_tr(X4_TR_U49_MY_GAMES);
    x4_ui2_card(pixels, rect.x, rect.y, rect.w, rect.h, 22);
    const char *badge = key->mode == X4_UI2_HOME_SIGNED_OUT ? x4_tr(X4_TR_U49_ACCOUNT_TITLE) :
        x4_tr(X4_TR_U49_HOME_LAST_PLAYED);
    const int available = rect.w - 68 - (key->mode == X4_UI2_HOME_WITH_RECENT ? HERO_ART_W : 0);
    const int badge_width = home_label_width(badge, X4_UI2_CAPTION, X4_FONT_SEMIBOLD, 20,
        238, available);
    x4_ui2_chip(pixels, rect.x + 34, rect.y + 16, badge_width, 38, badge, true);
    if (key->mode == X4_UI2_HOME_WITH_RECENT) {
        const char *name = key->items[X4_UI2_HOME_CONTINUE].name;
        title = name[0] ? name : x4_tr(X4_TR_U49_HOME_LAST_GAME);
        description = NULL;
        button = x4_tr(X4_TR_U49_FORTNITE_OPEN);
        X4UiRect image = {rect.x + rect.w - HERO_ART_W - 15,
            rect.y + (rect.h - HERO_ART_H) / 2, HERO_ART_W, HERO_ART_H};
        artwork(pixels, image, art, 16);
    } else if (key->mode == X4_UI2_HOME_SIGNED_OUT) {
        title = x4_tr(X4_TR_U49_HOME_SIGN_IN_TITLE);
        description = x4_tr(X4_TR_U49_HOME_SIGN_IN_SUBTITLE);
        button = x4_tr(X4_TR_U49_ACCOUNT_SIGN_IN);
    } else if (key->mode == X4_UI2_HOME_LOADING) {
        description = x4_tr(X4_TR_U49_HOME_RECENTS_LOADING);
    } else if (key->mode == X4_UI2_HOME_ERROR) {
        description = x4_tr(X4_TR_U49_HOME_RECENTS_ERROR);
    }
    int width = key->mode == X4_UI2_HOME_WITH_RECENT ? 390 : 772;
    (void)x4_apple_text_wrap(pixels, rect.x + 34, rect.y + HERO_TITLE_Y, 40, X4_FONT_BOLD,
        title, width, HERO_TITLE_LINE, 2, X4_UI2_TEXT);
    if (description)
        x4_text_ellipsis_px(pixels, rect.x + 34, rect.y + 134, 26, X4_FONT_REGULAR,
            description, width, X4_UI2_MUTED);
    else if (key->history[0])
        x4_text_ellipsis_px(pixels, rect.x + 34, rect.y + HERO_HISTORY_Y, 18, X4_FONT_REGULAR,
            key->history, 523, X4_UI2_MUTED);
    const int button_width = home_label_width(button, X4_UI2_BODY, X4_FONT_BOLD, 24 + 34,
        218, available);
    x4_ui2_button(pixels, rect.x + 34, rect.y + HERO_BUTTON_Y, button_width, 48,
        button, true, false, X4_ICON_CHEVRON_RIGHT);
}

static void draw_explore(uint32_t *pixels, X4UiRect rect, const HomeKey *key)
{
    char count[82];
    const char *description = x4_tr(X4_TR_U49_HOME_FIND_GAME);
    if (key->count) {
        snprintf(count, sizeof(count), x4_tr_count(key->count, X4_TR_50_HOME_CLOUD_COUNT_ONE_FMT,
            X4_TR_U49_HOME_CLOUD_COUNT_FMT), key->count);
        description = count;
    }
    x4_ui2_card(pixels, rect.x, rect.y, rect.w, rect.h, 22);
    x4_apple_fill_round_rect(pixels, rect.x + 34, rect.y + 20, 58, 58, 18, X4_UI2_BACKGROUND);
    x4_apple_draw_icon(pixels, rect.x + 51, rect.y + 37, X4_ICON_DEVICE_GAMEPAD, X4_UI2_ACCENT);
    x4_text_ellipsis_px(pixels, rect.x + 34, rect.y + 86, X4_UI2_HEADING, X4_FONT_BOLD,
        x4_tr(X4_TR_U49_HOME_EXPLORE_CATALOG), rect.w - 65, X4_UI2_TEXT);
    (void)x4_apple_text_wrap(pixels, rect.x + 34, rect.y + 128, 26, X4_FONT_REGULAR,
        description, rect.w - 66, 28, 2, X4_UI2_MUTED);
    const char *button = x4_tr(X4_TR_U49_HOME_EXPLORE);
    const int button_width = home_label_width(button, X4_UI2_BODY, X4_FONT_BOLD, 24 + 34,
        180, rect.w - 68);
    x4_ui2_button(pixels, rect.x + 34, rect.y + HERO_BUTTON_Y, button_width, 48,
        button, false, false, X4_ICON_CHEVRON_RIGHT);
}

static void draw_tile(uint32_t *pixels, X4UiRect rect, const HomeKey *key, int index,
    const X4CoverRaster *art, bool title_visible)
{
    if (index == X4_UI2_HOME_RECENT_ALL) {
        x4_ui2_card(pixels, rect.x, rect.y, rect.w, rect.h, 14);
        x4_apple_draw_icon(pixels, rect.x + (rect.w - 24) / 2, rect.y + 26, X4_ICON_CHEVRON_RIGHT,
            X4_UI2_ACCENT);
        const char *label = x4_tr(X4_TR_U49_HOME_VIEW_ALL);
        int width = x4_text_width_px(28, X4_FONT_SEMIBOLD, label);
        x4_text_px(pixels, rect.x + (rect.w - width) / 2, rect.y + 63, 28, X4_FONT_SEMIBOLD,
            label, X4_UI2_TEXT);
        return;
    }
    artwork(pixels, rect, art, 14);
    if (index == X4_UI2_HOME_POPULAR_FIRST) {
        const char *badge = x4_tr(X4_TR_U49_FREE);
        const int width = home_label_width(badge, X4_UI2_CAPTION, X4_FONT_SEMIBOLD, 20,
            82, rect.w - 20);
        x4_ui2_chip(pixels, rect.x + 10, rect.y + 10, width, 30, badge, true);
    }
    if (title_visible) {
        const char *title = key->items[index].name;
        if (!title[0]) title = x4_tr(X4_TR_U49_ACCOUNT_GAME);
        x4_text_ellipsis_px(pixels, rect.x + 2, rect.y + rect.h + TILE_TITLE_GAP,
            X4_UI2_CAPTION, X4_FONT_SEMIBOLD,
            title, rect.w - 4, X4_UI2_TEXT);
    }
}

static void draw_item(uint32_t *pixels, X4UiRect rect, const HomeKey *key, int index,
    const X4CoverRaster *art, bool title_visible)
{
    if (index == X4_UI2_HOME_CONTINUE) draw_continue(pixels, rect, key, art);
    else if (index == X4_UI2_HOME_EXPLORE) draw_explore(pixels, rect, key);
    else draw_tile(pixels, rect, key, index, art, title_visible);
}

static void compose(uint32_t *pixels, const X4Ui2HomeModel *model, const HomeKey *key, X4CatalogAssets *assets)
{
    X4UiRect bounds = x4_ui2_home_bounds();
    x4_rect(pixels, bounds.x, bounds.y, bounds.w, bounds.h, X4_UI2_BACKGROUND);
    draw_header(pixels, key);
    for (int index = 0; index < X4_UI2_HOME_ITEM_COUNT; ++index) {
        X4UiRect rect;
        if (!x4_ui2_home_item_bounds(model, index, &rect)) continue;
        X4CatalogAssetView view;
        const char *product = key->items[index].product;
        bool borrowed = assets && product[0] && x4_catalog_assets_acquire(assets, product, &view);
        const X4CoverRaster *art = borrowed ? pick_art(&view) : NULL;
        draw_item(pixels, rect, key, index, art, true);
        home.art[index] = art_key(art, borrowed ? view.generation : 0);
        if (borrowed) x4_catalog_assets_release(assets, &view);
    }
}

static X4UiRect glow_rect(X4UiRect rect)
{
    return (X4UiRect){rect.x - X4_UI2_FOCUS_SPREAD, rect.y - X4_UI2_FOCUS_SPREAD,
        rect.w + X4_UI2_FOCUS_SPREAD * 2, rect.h + X4_UI2_FOCUS_SPREAD * 2};
}

static void note_damage(X4UiRect damage[2], size_t *count, X4UiRect rect)
{
    if (*count == SIZE_MAX) return;
    for (size_t i = 0; i < *count; ++i) {
        X4UiRect r = damage[i];
        if (r.x == rect.x && r.y == rect.y && r.w == rect.w && r.h == rect.h) return;
    }
    if (*count == 2) *count = SIZE_MAX;
    else damage[(*count)++] = rect;
}

static bool refresh_art(uint32_t *pixels, const X4Ui2HomeModel *model, X4CatalogAssets *assets,
    int focus, bool existing_focus, X4UiRect damage[2], size_t *count)
{
    if (!assets) return false;
    bool focus_changed = false;
    for (int index = 0; index < X4_UI2_HOME_ITEM_COUNT; ++index) {
        X4UiRect rect;
        const char *product = home.key.items[index].product;
        if (!product[0] || !x4_ui2_home_item_bounds(model, index, &rect)) continue;
        X4CatalogAssetView view;
        if (!x4_catalog_assets_acquire(assets, product, &view)) continue;
        const X4CoverRaster *art = pick_art(&view);
        HomeArtKey next = art_key(art, view.generation);
        if (art && !equal_art(home.art[index], next)) {
            X4UiRect changed = index == focus ? glow_rect(rect) : rect;
            if (index == focus && existing_focus) copy_unfocused(pixels, changed, false);
            x4_rect(pixels, rect.x, rect.y, rect.w, rect.h, X4_UI2_BACKGROUND);
            draw_item(pixels, rect, &home.key, index, art, false);
            copy_unfocused(pixels, rect, true);
            home.art[index] = next;
            note_damage(damage, count, changed);
            if (index == focus) focus_changed = true;
        }
        x4_catalog_assets_release(assets, &view);
    }
    return focus_changed;
}

size_t x4_ui2_home_draw(const X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    X4CatalogAssets *assets, uint64_t asset_revision, int selected, bool canvas_valid,
    uint32_t *pixels, X4UiRect damage[2])
{
    if (!model || !pixels || !damage) return 0;
    make_key(&pending_key, model, catalog);
    if (!home.allocation_attempted) {
        home.allocation_attempted = true;
        home.unfocused = malloc((size_t)CONTENT_WIDTH * CONTENT_HEIGHT * sizeof(*home.unfocused));
    }
    int focus = selected < 0 ? -1 : x4_ui2_home_normalize(model, selected);
    X4UiRect focus_bounds;
    bool has_focus = x4_ui2_home_item_bounds(model, focus, &focus_bounds);
    bool content_changed = !home.ready || home.assets != assets ||
        memcmp(&pending_key, &home.key, sizeof(home.key)) != 0;
    bool full = content_changed || !canvas_valid || home.canvas != pixels;
    bool focus_changed = full || focus != home.focus;
    size_t count = full ? SIZE_MAX : 0;
    if (content_changed) {
        memset(home.art, 0, sizeof(home.art));
        compose(pixels, model, &pending_key, assets);
        if (home.unfocused) copy_unfocused(pixels, x4_ui2_home_bounds(), true);
        memcpy(&home.key, &pending_key, sizeof(home.key));
        home.ready = home.unfocused != NULL;
        home.asset_revision = asset_revision;
        home.assets = assets;
    } else {
        if (full) copy_unfocused(pixels, x4_ui2_home_bounds(), false);
        else if (focus_changed) {
            X4UiRect previous;
            if (x4_ui2_home_item_bounds(model, home.focus, &previous)) {
                X4UiRect rect = glow_rect(previous);
                copy_unfocused(pixels, rect, false);
                note_damage(damage, &count, rect);
            }
        }
        if (asset_revision != home.asset_revision) {
            bool asset_focus = refresh_art(pixels, model, assets, focus, !focus_changed, damage, &count);
            focus_changed = focus_changed || asset_focus;
            home.asset_revision = asset_revision;
        }
    }
    if (has_focus && focus_changed) {
        x4_ui2_focus(pixels, focus_bounds.x, focus_bounds.y, focus_bounds.w, focus_bounds.h,
            focus < X4_UI2_HOME_RECENT_FIRST ? 22 : 14);
        note_damage(damage, &count, glow_rect(focus_bounds));
    }
    home.canvas = pixels;
    home.focus = focus;
    return count;
}

void x4_ui2_home_invalidate(void)
{
    home.canvas = NULL;
}

void x4_ui2_home_close(void)
{
    free(home.unfocused);
    memset(&home, 0, sizeof(home));
    memset(&pending_key, 0, sizeof(pending_key));
}
