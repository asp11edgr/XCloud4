/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_home.h"
#include "ui2_theme.h"
#include "canvas.h"

#include <stdlib.h>
#include <string.h>

enum {
    CONTENT_WIDTH = X4_UI_WIDTH, CONTENT_HEIGHT = X4_UI_SAFE_BOTTOM,
    HERO_HEIGHT = 690, HERO_FADE = 172,
    CARD_X = X4_UI_SAFE_LEFT, TILE_W = 336, TILE_H = 189, TILE_STEP = 354,
    SHELF_HEADING_Y = 639, SHELF_TILE_Y = 700, TILE_TITLE_GAP = 10,
    HERO_TITLE_Y = 308, HERO_BODY_Y = 395,
    HERO_BUTTON_Y = 438, HERO_BUTTON_W = 322, HERO_BUTTON_H = 66,
    TILE_RADIUS = X4_UI2_RADIUS_CARD,
};
_Static_assert(TILE_W * 9 == TILE_H * 16, "Home game art uses 16:9");
_Static_assert(CARD_X + (X4_UI2_HOME_TILE_MAX - 1) * TILE_STEP + TILE_W <= X4_UI_SAFE_RIGHT,
    "The popular shelf stays inside the TV safe area");
_Static_assert(SHELF_TILE_Y + TILE_H + TILE_TITLE_GAP + X4_UI2_BODY < X4_UI_FOOTER_Y,
    "Home card titles clear the controller hints");
_Static_assert(HERO_BUTTON_Y + HERO_BUTTON_H + X4_UI2_FOCUS_SPREAD < HERO_HEIGHT,
    "Home button damage remains inside the banner");
static const char fortnite_product[] = "BT5P2X999VH2";

typedef struct {
    char product[X4_PRODUCT_ID_CAP];
    char name[128];
} HomeItem;

typedef struct {
    enum X4Lang language;
    enum X4Ui2HomeMode mode;
    unsigned shelf;
    bool recent_shelf;
    HomeItem items[X4_UI2_HOME_ITEM_COUNT];
} HomeKey;

typedef struct {
    uintptr_t address;
    unsigned width, height;
    uint64_t generation, signature;
} HomeArtKey;

static struct {
    HomeKey key;
    HomeArtKey art[X4_UI2_HOME_ITEM_COUNT], hero_art;
    uint32_t *unfocused;
    const uint32_t *canvas;
    X4CatalogAssets *assets;
    uint64_t asset_revision;
    int focus, featured;
    bool ready, allocation_attempted;
} home;

static HomeKey pending_key;
static const X4History *home_history;
static struct {
    const X4CatalogSnapshot *catalog;
    uint64_t revision;
    unsigned source;
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

static unsigned most_recent_local(const X4CatalogSnapshot *catalog)
{
    if (!valid_catalog(catalog) || !home_history || !home_history->count) return X4_CATALOG_MAX;
    if (history_row.catalog == catalog && history_row.revision == home_history->revision)
        return history_row.source;
    history_row.catalog = catalog;
    history_row.revision = home_history->revision;
    history_row.source = X4_CATALOG_MAX;
    uint64_t latest = 0;
    for (unsigned source = 0; source < catalog->count; ++source) {
        if (!valid_source(catalog, source)) continue;
        const X4HistoryEntry *entry = x4_history_find(home_history, catalog->titles[source].product_id);
        if (entry && (history_row.source == X4_CATALOG_MAX || entry->last_session_s > latest)) {
            latest = entry->last_session_s;
            history_row.source = source;
        }
    }
    return history_row.source;
}

static void fill_recent(X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    const X4RecentGames *recent)
{
    unsigned source = most_recent_local(catalog);
    if (source == X4_CATALOG_MAX) (void)x4_home_continue_index(catalog, recent, &source);
    if (valid_source(catalog, source)) {
        model->mode = X4_UI2_HOME_WITH_RECENT;
        model->continuation = source;
    } else if (recent && recent->state == X4_RECENT_LOADING) {
        model->mode = X4_UI2_HOME_LOADING;
    } else if (recent && recent->state == X4_RECENT_ERROR) {
        model->mode = X4_UI2_HOME_ERROR;
    } else {
        model->mode = X4_UI2_HOME_EMPTY;
    }
}

static void fill_popular(X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    const X4HomeRows *rows)
{
    model->shelf[model->shelf_count++] = fortnite_source(catalog);
    if (!valid_catalog(catalog) || !rows || rows->count > X4_HOME_ROW_COUNT) return;
    for (unsigned r = 0; r < rows->count; ++r) {
        const X4HomeRow *row = &rows->rows[r];
        if (row->kind != X4_HOME_POPULAR || row->count > X4_HOME_ROW_MAX) continue;
        for (unsigned i = 0; i < row->count && model->shelf_count < X4_UI2_HOME_TILE_MAX; ++i) {
            unsigned source = row->indices[i];
            if (valid_source(catalog, source) && !is_fortnite(catalog->titles[source].product_id))
                model->shelf[model->shelf_count++] = source;
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
    fill_recent(out, catalog, recent);
    if (out->mode != X4_UI2_HOME_WITH_RECENT) {
        fill_popular(out, catalog, rows);
        return;
    }
    out->recent_shelf = true;
    out->shelf[out->shelf_count++] = out->continuation;
    for (unsigned i = 0; i < x4_recent_count(recent) && out->shelf_count < X4_UI2_HOME_TILE_MAX; ++i) {
        unsigned source;
        if (!x4_recent_catalog_index(x4_recent_get(recent, i), catalog, &source) ||
            !valid_source(catalog, source)) continue;
        unsigned existing = 0;
        while (existing < out->shelf_count &&
            strcmp(catalog->titles[out->shelf[existing]].product_id, catalog->titles[source].product_id))
            ++existing;
        if (existing == out->shelf_count) out->shelf[out->shelf_count++] = source;
    }
}

static unsigned shelf_count(const X4Ui2HomeModel *model)
{
    return model && model->shelf_count <= X4_UI2_HOME_TILE_MAX ? model->shelf_count : 0;
}

static bool is_tile(const X4Ui2HomeModel *model, int selected)
{
    return selected >= X4_UI2_HOME_TILE_FIRST &&
        (unsigned)(selected - X4_UI2_HOME_TILE_FIRST) < shelf_count(model);
}

int x4_ui2_home_normalize(const X4Ui2HomeModel *model, int selected)
{
    return is_tile(model, selected) ? selected : X4_UI2_HOME_CONTINUE;
}

int x4_ui2_home_move(const X4Ui2HomeModel *model, int selected, enum X4Ui2HomeDirection direction)
{
    selected = x4_ui2_home_normalize(model, selected);
    if (selected == X4_UI2_HOME_CONTINUE) {
        if (direction == X4_UI2_HOME_DOWN && shelf_count(model)) return X4_UI2_HOME_TILE_FIRST;
        return selected;
    }
    if (direction == X4_UI2_HOME_UP) return X4_UI2_HOME_CONTINUE;
    if (direction == X4_UI2_HOME_LEFT && is_tile(model, selected - 1)) return selected - 1;
    if (direction == X4_UI2_HOME_RIGHT && is_tile(model, selected + 1)) return selected + 1;
    return selected;
}

X4Ui2HomeRoute x4_ui2_home_activate(const X4Ui2HomeModel *model, int selected)
{
    X4Ui2HomeRoute route = {.source = X4_CATALOG_MAX};
    if (!model) return route;
    if (selected == X4_UI2_HOME_CONTINUE) {
        switch (model->mode) {
        case X4_UI2_HOME_SIGNED_OUT: route.action = X4_UI2_HOME_LOGIN; break;
        case X4_UI2_HOME_WITH_RECENT:
            route.action = X4_UI2_HOME_GAME;
            route.source = model->continuation;
            break;
        case X4_UI2_HOME_EMPTY: route.action = X4_UI2_HOME_CATALOG; break;
        default: route.action = X4_UI2_HOME_RECENTS; break;
        }
    } else if (is_tile(model, selected)) {
        if (!model->recent_shelf && selected == X4_UI2_HOME_TILE_FIRST) {
            route.action = X4_UI2_HOME_FORTNITE;
        } else {
            route.source = model->shelf[selected - X4_UI2_HOME_TILE_FIRST];
            if (route.source < X4_CATALOG_MAX) route.action = X4_UI2_HOME_GAME;
        }
    }
    return route;
}

X4UiRect x4_ui2_home_bounds(void)
{
    return (X4UiRect){0, 0, CONTENT_WIDTH, CONTENT_HEIGHT};
}

bool x4_ui2_home_item_bounds(const X4Ui2HomeModel *model, int selected, X4UiRect *out)
{
    if (!model || !out) return false;
    if (selected == X4_UI2_HOME_CONTINUE) {
        *out = (X4UiRect){CARD_X, HERO_BUTTON_Y, HERO_BUTTON_W, HERO_BUTTON_H};
    } else if (is_tile(model, selected)) {
        int column = selected - X4_UI2_HOME_TILE_FIRST;
        *out = (X4UiRect){CARD_X + column * TILE_STEP, SHELF_TILE_Y, TILE_W, TILE_H};
    } else {
        return false;
    }
    return true;
}

bool x4_ui2_home_art_bounds(const X4Ui2HomeModel *model, int selected, X4UiRect *out)
{
    if (!x4_ui2_home_item_bounds(model, selected, out)) return false;
    if (selected == X4_UI2_HOME_CONTINUE)
        *out = (X4UiRect){0, 0, CONTENT_WIDTH, HERO_HEIGHT};
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

static const char *hero_product(const X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog)
{
    return model->mode == X4_UI2_HOME_WITH_RECENT && valid_source(catalog, model->continuation) ?
        catalog->titles[model->continuation].product_id : fortnite_product;
}

static void make_key(HomeKey *key, const X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog)
{
    memset(key, 0, sizeof(*key));
    key->language = x4_lang_current();
    key->mode = model->mode;
    key->shelf = shelf_count(model);
    key->recent_shelf = model->recent_shelf;
    HomeItem *hero = &key->items[X4_UI2_HOME_CONTINUE];
    if (model->mode == X4_UI2_HOME_WITH_RECENT)
        set_game(hero, catalog, model->continuation);
    if (!hero->product[0]) memcpy(hero->product, fortnite_product, sizeof(fortnite_product));
    for (unsigned i = 0; i < key->shelf; ++i)
        set_game(&key->items[X4_UI2_HOME_TILE_FIRST + i], catalog, model->shelf[i]);
    if (key->shelf && !key->recent_shelf) {
        HomeItem *first = &key->items[X4_UI2_HOME_TILE_FIRST];
        if (!first->product[0]) memcpy(first->product, fortnite_product, sizeof(fortnite_product));
        if (!first->name[0]) memcpy(first->name, "Fortnite", sizeof("Fortnite"));
    }
}

size_t x4_ui2_home_cover_demand(const X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    const char *ids[X4_UI2_HOME_COVER_MAX], uint32_t masks[X4_UI2_HOME_COVER_MAX])
{
    if (!model || !ids || !masks) return 0;
    ids[0] = hero_product(model, catalog);
    masks[0] = 1u << X4_COVER_HOME_FEATURE;
    size_t count = 1;
    for (unsigned i = 0; i < shelf_count(model); ++i) {
        const char *id = valid_source(catalog, model->shelf[i]) ?
            catalog->titles[model->shelf[i]].product_id : NULL;
        if (!id && !i && !model->recent_shelf) id = fortnite_product;
        if (!id) continue;
        size_t at = 0;
        while (at < count && strcmp(ids[at], id)) ++at;
        if (at == count) {
            ids[count] = id;
            masks[count++] = 0;
        }
        masks[at] |= 1u << X4_COVER_ROW;
    }
    return count;
}

static void copy_unfocused(uint32_t *pixels, X4UiRect rect, bool save)
{
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        uint32_t *screen = pixels + (size_t)y * X4_UI_WIDTH + rect.x;
        uint32_t *saved = home.unfocused + (size_t)y * CONTENT_WIDTH + rect.x;
        if (save) memcpy(saved, screen, (size_t)rect.w * sizeof(*saved));
        else memcpy(screen, saved, (size_t)rect.w * sizeof(*screen));
    }
}

static bool raster_valid(const X4CoverRaster *art)
{
    return art && art->rgba && art->width > 0 && art->height > 0 &&
        art->width <= X4_UI_WIDTH && art->height <= X4_UI_HEIGHT;
}

static const X4CoverRaster *pick_art(const X4CatalogAssetView *view, int index)
{
    if (index == X4_UI2_HOME_CONTINUE && raster_valid(&view->surface[X4_COVER_HOME_FEATURE]))
        return &view->surface[X4_COVER_HOME_FEATURE];
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

static const char *hero_action(const HomeKey *key, int featured)
{
    if (featured != X4_UI2_HOME_CONTINUE) return x4_tr(X4_TR_U49_VIEW_GAME_SHORT);
    switch (key->mode) {
    case X4_UI2_HOME_WITH_RECENT: return x4_tr(X4_TR_50_SCREEN_HOME_CONTINUE);
    case X4_UI2_HOME_SIGNED_OUT: return x4_tr(X4_TR_U49_ACCOUNT_SIGN_IN);
    case X4_UI2_HOME_EMPTY: return x4_tr(X4_TR_U49_HOME_EXPLORE_CATALOG);
    default: return x4_tr(X4_TR_U49_MY_GAMES);
    }
}

static void draw_hero_button(uint32_t *pixels, X4UiRect rect, const HomeKey *key,
    int featured, bool focused)
{
    const int radius = X4_UI2_RADIUS_CONTROL;
    const char *label = hero_action(key, featured);
    const int size = 28;
    int width = x4_text_width_px(size, X4_FONT_BOLD, label);
    const int maximum = rect.w - 70;
    if (width > maximum) width = maximum;
    const int inset = (rect.w - width - 38) / 2;
    x4_apple_fill_round_rect(pixels, rect.x, rect.y, rect.w, rect.h, radius, X4_UI2_TEXT);
    if (focused) x4_ui2_focus(pixels, rect.x, rect.y, rect.w, rect.h, radius);
    x4_ui2_play(pixels, rect.x + inset, rect.y + (rect.h - 24) / 2, X4_UI2_INK);
    x4_text_ellipsis_px(pixels, rect.x + inset + 38, rect.y + (rect.h - size) / 2,
        size, X4_FONT_BOLD, label, maximum, X4_UI2_INK);
}

static void shade_banner(uint32_t *pixels, X4UiRect rect)
{
    enum { LEFT_SOLID = 190, LEFT_FADE = 970 };
    for (int y = 0; y < rect.h; ++y) {
        uint32_t *row = pixels + (size_t)y * X4_UI_WIDTH;
        int bottom = y - (rect.h - HERO_FADE);
        const unsigned fade = bottom > 0 ? 255u * (unsigned)bottom / (HERO_FADE - 1) : 0;
        for (int x = 0; x < LEFT_SOLID + LEFT_FADE; ++x) {
            const unsigned opacity = x < LEFT_SOLID ? 248 :
                248u * (unsigned)(LEFT_SOLID + LEFT_FADE - x) / LEFT_FADE;
            const unsigned alpha = fade + (255 - fade) * opacity / 255;
            x4_apple_blend_pixel(row + x, X4_UI2_TINT(alpha));
        }
        if (fade)
            for (int x = LEFT_SOLID + LEFT_FADE; x < rect.w; ++x)
                x4_apple_blend_pixel(row + x, X4_UI2_TINT(fade));
    }
}

static void draw_hero(uint32_t *pixels, const X4Ui2HomeModel *model, const HomeKey *key,
    int featured, const X4CoverRaster *art)
{
    X4UiRect rect, button;
    x4_ui2_home_art_bounds(model, X4_UI2_HOME_CONTINUE, &rect);
    x4_ui2_home_item_bounds(model, X4_UI2_HOME_CONTINUE, &button);
    x4_rect(pixels, rect.x, rect.y, rect.w, rect.h, X4_UI2_BACKGROUND);
    if (raster_valid(art)) {
        x4_apple_blit_banner(pixels, rect.x, rect.y, rect.w, rect.h, art);
        shade_banner(pixels, rect);
    }

    const char *title = x4_tr(X4_TR_U49_HOME_NEXT_GAME);
    const char *description = NULL;
    if (featured != X4_UI2_HOME_CONTINUE) {
        title = key->items[featured].name;
        if (!title[0]) title = x4_tr(X4_TR_U49_ACCOUNT_GAME);
    } else switch (key->mode) {
    case X4_UI2_HOME_WITH_RECENT:
        title = key->items[X4_UI2_HOME_CONTINUE].name;
        if (!title[0]) title = x4_tr(X4_TR_U49_HOME_LAST_GAME);
        break;
    case X4_UI2_HOME_SIGNED_OUT:
        title = x4_tr(X4_TR_U49_HOME_SIGN_IN_TITLE);
        description = x4_tr(X4_TR_U49_HOME_SIGN_IN_SUBTITLE);
        break;
    case X4_UI2_HOME_LOADING: description = x4_tr(X4_TR_U49_HOME_RECENTS_LOADING); break;
    case X4_UI2_HOME_ERROR: description = x4_tr(X4_TR_U49_HOME_RECENTS_ERROR); break;
    default: break;
    }
    x4_text_ellipsis_px(pixels, CARD_X, HERO_TITLE_Y, description ? 62 : 88, X4_FONT_BOLD,
        title, 1030, X4_UI2_TEXT);
    if (description)
        x4_text_ellipsis_px(pixels, CARD_X, HERO_BODY_Y, 24, X4_FONT_REGULAR,
            description, 880, X4_UI2_MUTED);
    draw_hero_button(pixels, button, key, featured, false);
    x4_text_px(pixels, CARD_X, SHELF_HEADING_Y, 36, X4_FONT_BOLD,
        x4_tr(key->recent_shelf ? X4_TR_CAT_RECENT_TITLE : X4_TR_U49_POPULAR_CLOUD),
        X4_UI2_TEXT);
}

static int featured_index(const HomeKey *key, int focus)
{
    if (focus < X4_UI2_HOME_TILE_FIRST || key->mode == X4_UI2_HOME_SIGNED_OUT ||
        !strcmp(key->items[focus].product, key->items[X4_UI2_HOME_CONTINUE].product))
        return X4_UI2_HOME_CONTINUE;
    return focus;
}

static bool refresh_hero(uint32_t *pixels, const X4Ui2HomeModel *model, const HomeKey *key,
    X4CatalogAssets *assets, int featured, bool force)
{
    X4CatalogAssetView view;
    int art_index = featured;
    const char *product = key->items[art_index].product;
    bool borrowed = assets && product[0] && x4_catalog_assets_acquire(assets, product, &view);
    const X4CoverRaster *art = borrowed ? pick_art(&view, art_index) : NULL;
    const HomeArtKey next = art_key(art, borrowed ? view.generation : 0);
    const bool redraw = force || (art && !equal_art(home.hero_art, next));
    if (redraw) {
        draw_hero(pixels, model, key, featured, art);
        home.hero_art = next;
        home.featured = featured;
    }
    if (borrowed) x4_catalog_assets_release(assets, &view);
    return redraw;
}

static void draw_tile(uint32_t *pixels, X4UiRect rect, const HomeItem *item,
    const X4CoverRaster *art, bool title_visible)
{
    x4_ui2_card(pixels, rect.x, rect.y, rect.w, rect.h, TILE_RADIUS);
    if (raster_valid(art)) {
        x4_apple_blit_card_rounded(pixels, rect.x, rect.y, rect.w, rect.h,
            TILE_RADIUS, art, X4_GRAD_NONE);
    } else {
        x4_apple_draw_icon(pixels, rect.x + (rect.w - 24) / 2, rect.y + (rect.h - 24) / 2,
            X4_ICON_DEVICE_GAMEPAD, X4_UI2_MUTED);
    }
    if (title_visible)
        x4_text_ellipsis_px(pixels, rect.x + 2, rect.y + rect.h + TILE_TITLE_GAP,
            X4_UI2_BODY, X4_FONT_SEMIBOLD, item->name[0] ? item->name :
            x4_tr(X4_TR_U49_ACCOUNT_GAME), rect.w - 4, X4_UI2_TEXT);
}

static void compose(uint32_t *pixels, const X4Ui2HomeModel *model, const HomeKey *key,
    X4CatalogAssets *assets, int featured)
{
    X4UiRect bounds = x4_ui2_home_bounds();
    x4_rect(pixels, bounds.x, bounds.y, bounds.w, bounds.h, X4_UI2_BACKGROUND);
    refresh_hero(pixels, model, key, assets, featured, true);
    for (int index = X4_UI2_HOME_TILE_FIRST; index < X4_UI2_HOME_ITEM_COUNT; ++index) {
        X4UiRect rect;
        if (!x4_ui2_home_item_bounds(model, index, &rect)) continue;
        X4CatalogAssetView view;
        const char *product = key->items[index].product;
        bool borrowed = assets && product[0] && x4_catalog_assets_acquire(assets, product, &view);
        const X4CoverRaster *art = borrowed ? pick_art(&view, index) : NULL;
        draw_tile(pixels, rect, &key->items[index], art, true);
        home.art[index] = art_key(art, borrowed ? view.generation : 0);
        if (borrowed) x4_catalog_assets_release(assets, &view);
    }
    const X4Ui2Hint hints[] = {
        {X4_GLYPH_CROSS, x4_tr(X4_TR_U49_SELECT)},
        {X4_GLYPH_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)},
    };
    x4_ui2_footer_left(pixels, hints, sizeof(hints) / sizeof(hints[0]));
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
    for (int index = X4_UI2_HOME_TILE_FIRST; index < X4_UI2_HOME_ITEM_COUNT; ++index) {
        X4UiRect rect;
        const char *product = home.key.items[index].product;
        if (!product[0] || !x4_ui2_home_item_bounds(model, index, &rect)) continue;
        X4CatalogAssetView view;
        if (!x4_catalog_assets_acquire(assets, product, &view)) continue;
        const X4CoverRaster *art = pick_art(&view, index);
        HomeArtKey next = art_key(art, view.generation);
        if (art && !equal_art(home.art[index], next)) {
            const X4UiRect changed = index == focus ? glow_rect(rect) : rect;
            if (index == focus && existing_focus) copy_unfocused(pixels, changed, false);
            x4_rect(pixels, rect.x, rect.y, rect.w, rect.h, X4_UI2_BACKGROUND);
            draw_tile(pixels, rect, &home.key.items[index], art, false);
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
    int featured = featured_index(&pending_key, focus);
    X4UiRect focus_bounds;
    bool has_focus = x4_ui2_home_item_bounds(model, focus, &focus_bounds);
    bool content_changed = !home.ready || home.assets != assets ||
        memcmp(&pending_key, &home.key, sizeof(home.key)) != 0;
    bool full = content_changed || !canvas_valid || home.canvas != pixels;
    bool focus_changed = full || focus != home.focus;
    size_t count = full ? SIZE_MAX : 0;
    if (content_changed) {
        memset(home.art, 0, sizeof(home.art));
        compose(pixels, model, &pending_key, assets, featured);
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
        if (featured != home.featured || asset_revision != home.asset_revision) {
            const bool changed = refresh_hero(pixels, model, &home.key, assets, featured,
                featured != home.featured);
            if (changed) {
                X4UiRect rect;
                x4_ui2_home_art_bounds(model, X4_UI2_HOME_CONTINUE, &rect);
                copy_unfocused(pixels, rect, true);
                note_damage(damage, &count, rect);
                if (focus == X4_UI2_HOME_CONTINUE) focus_changed = true;
            }
        }
        if (asset_revision != home.asset_revision) {
            bool asset_focus = refresh_art(pixels, model, assets, focus, !focus_changed, damage, &count);
            focus_changed = focus_changed || asset_focus;
            home.asset_revision = asset_revision;
        }
    }
    if (has_focus && focus_changed) {
        if (focus == X4_UI2_HOME_CONTINUE) draw_hero_button(pixels, focus_bounds, &home.key, featured, true);
        else x4_ui2_focus(pixels, focus_bounds.x, focus_bounds.y, focus_bounds.w, focus_bounds.h, TILE_RADIUS);
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
