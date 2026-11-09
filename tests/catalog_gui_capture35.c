/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/auth/json.h"
#include "../src/core/build_identity.h"
#include "../src/ui/catalog_gui.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 1920, HEIGHT = 1080, MAX_PRODUCTS = 32 };
static const size_t frame_bytes = WIDTH * HEIGHT * sizeof(uint32_t);
static X4CatalogSnapshot catalog;
static X4CatalogBrowser browser;
static X4RecentGames recent;
static X4HomeSigls sigls;
static const char *forza = "9NKX70BBCDRN";
static const char *fortnite = "BT5P2X999VH2", *plants_pc = "9NGP0DJVKSFL";
static const char *football_pc = "9PD0Z90216RJ";
static X4CatalogGuiStatus status = {1, 0, 0, NULL};

struct X4CatalogAssets {
    X4CatalogAssetView entry[MAX_PRODUCTS];
    unsigned count, active, pins;
    uint64_t revision;
    X4CatalogBackgroundView backgrounds[2];
};

static struct X4CatalogAssets assets;
bool x4_server_settings_awaiting_first_choice(void) { return false; }

static unsigned asset_index(const char *id)
{
    for (unsigned i = 0; i < assets.count; ++i) {
        if (!strcmp(id, assets.entry[i].metadata.product_id)) return i;
    }
    return MAX_PRODUCTS;
}

int x4_catalog_assets_acquire(X4CatalogAssets *a, const char *id, X4CatalogAssetView *view)
{
    if (!a) return 0;
    unsigned i = asset_index(id);
    if (i == MAX_PRODUCTS) return 0;
    *view = a->entry[i];
    return 1;
}

void x4_catalog_assets_release(X4CatalogAssets *a, X4CatalogAssetView *view)
{ (void)a; (void)view; }

int x4_catalog_assets_acquire_background(X4CatalogAssets *a, X4CatalogBackgroundView *view)
{
    if (!a || !a->backgrounds[a->active].raster.rgba) return 0;
    *view = a->backgrounds[a->active];
    assert(++a->pins <= 2);
    return 1;
}

void x4_catalog_assets_release_background(X4CatalogAssets *a, X4CatalogBackgroundView *view)
{
    assert(a && view->raster.rgba && a->pins);
    --a->pins;
}

void x4_catalog_assets_stats(X4CatalogAssets *a, X4CatalogAssetStats *out)
{
    memset(out, 0, sizeof(*out));
    if (a) out->revision = a->revision;
}

static unsigned char *read_file(const char *dir, const char *leaf, size_t *bytes)
{
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    assert(!fseek(f, 0, SEEK_END));
    long size = ftell(f);
    assert(size > 0 && size <= 2 * 1024 * 1024);
    rewind(f);
    unsigned char *data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, f) == (size_t)size);
    assert(!fclose(f));
    *bytes = (size_t)size;
    return data;
}

static void field(X4JsonSpan object, const char *key, char *out, size_t cap)
{
    X4JsonSpan value;
    assert(x4_json_member(object, key, &value) == 1);
    assert(!x4_json_display_text(value, out, cap));
}

static bool decode(const char *dir, const char *id, const char *dimensions,
    enum X4CoverSurface surface, X4CoverRaster *raster)
{
    char leaf[96];
    snprintf(leaf, sizeof(leaf), "images/%s-%s.jpg", id, dimensions);
    size_t length;
    unsigned char *data = read_file(dir, leaf, &length);
    if (!data) return false;
    assert(!x4_product_surface_decode(data, length, surface, raster));
    free(data);
    return true;
}

static void publish_background(const char *dir, const char *id)
{
    assert(x4_catalog_browser_home_product(&catalog, id));
    unsigned slot = assets.active ^ 1u;
    X4CatalogBackgroundView *bg = &assets.backgrounds[slot];
    free(bg->raster.rgba);
    assert(decode(dir, id, "1920x1080", X4_COVER_BACKGROUND, &bg->raster));
    x4_product_compose_background(&bg->raster);
    memcpy(bg->product_id, id, strlen(id) + 1);
    bg->generation = 1;
    bg->slot = slot;
    assets.active = slot;
    ++assets.revision;
}

static void load_sigl(X4JsonSpan lists, const char *key, X4SiglKind kind)
{
    X4JsonSpan list, value;
    assert(x4_json_member(lists, key, &list) == 1);
    size_t cursor = 0;
    X4SiglList *out = &sigls[kind];
    while (x4_json_item(list, &cursor, &value) == 1) {
        assert(out->count < X4_CATALOG_MAX);
        assert(!x4_json_token(value, out->ids[out->count++], X4_PRODUCT_ID_CAP));
    }
    out->valid = true;
}

static void fixtures(const char *dir)
{
    size_t length, pos = 0;
    unsigned char *data = read_file(dir, "dataset.json", &length);
    X4JsonSpan root, products, rows, item;
    assert(data && !x4_json_parse((const char *)data, length, &root));
    assert(x4_json_member(root, "products", &products) == 1);
    assert(x4_json_member(root, "rows", &rows) == 1);
    catalog.state = X4_CATALOG_READY;
    while (x4_json_item(products, &pos, &item) == 1) {
        char id[X4_PRODUCT_ID_CAP];
        field(item, "id", id, sizeof(id));
        /* Fixture for /v2/titles. Both PC IDs remain in their captured SIGLs. */
        if (!strcmp(id, plants_pc) || !strcmp(id, football_pc)) continue;
        unsigned i = catalog.count++;
        assert(i < MAX_PRODUCTS);
        X4CatalogTitle *title = &catalog.titles[i];
        snprintf(title->id, sizeof(title->id), "FIXTURE_%u", i);
        memcpy(title->product_id, id, strlen(id) + 1);
        X4CatalogAssetView *entry = &assets.entry[i];
        X4ProductMetadata *m = &entry->metadata;
        memcpy(m->product_id, id, strlen(id) + 1);
        field(item, "name", m->name, sizeof(m->name));
        field(item, "publisher", m->publisher, sizeof(m->publisher));
        field(item, "category", m->category, sizeof(m->category));
        field(item, "description", m->description, sizeof(m->description));
        memcpy(title->name, m->name, sizeof(title->name));
        title->entitled = i < 10;
        title->f2p_only = !strcmp(id, fortnite);
        entry->generation = 1;
        entry->state = X4_ASSET_READY;
        assert(decode(dir, id, "384x216", X4_COVER_ROW, &entry->surface[X4_COVER_ROW]));
        if (i < 6) {
            /* The T35 catalog fixture used Fortnite's 384×216 input before
             * the 356×200 network sample was added. Keep that input for 05. */
            if (!strcmp(id, fortnite) ||
                !decode(dir, id, "356x200", X4_COVER_GRID, &entry->surface[X4_COVER_GRID]))
                assert(decode(dir, id, "384x216", X4_COVER_GRID, &entry->surface[X4_COVER_GRID]));
        }
        (void)decode(dir, id, "524x295", X4_COVER_DETAIL, &entry->surface[X4_COVER_DETAIL]);
        ++assets.count;
    }
    catalog.total = catalog.count;
    load_sigl(rows, "free", X4_SIGL_FREE);
    load_sigl(rows, "new", X4_SIGL_NEW);
    load_sigl(rows, "popular", X4_SIGL_POPULAR);
    load_sigl(rows, "leaving", X4_SIGL_LEAVING);
    free(data);
    x4_recent_init(&recent);
    char recent_ids[6][96];
    for (unsigned i = 0; i < 6; ++i) memcpy(recent_ids[i], catalog.titles[i].id, sizeof(recent_ids[i]));
    x4_recent_begin(&recent, 1, 1);
    assert(x4_recent_publish(&recent, 1, 1, recent_ids, 6, 0, 200, &catalog));
    x4_catalog_browser_init(&browser);
    browser.apple_mode = true;
    x4_catalog_browser_rebuild(&browser, &catalog);
    x4_catalog_browser_sync_recents(&browser, &recent);
    x4_catalog_browser_home_rebuild(&browser, &catalog, &recent, sigls);
}

static void capture(const char *out, const char *name, const uint32_t *p)
{
    char path[768];
    snprintf(path, sizeof(path), "%s/%s.ppm", out, name);
    FILE *f = fopen(path, "wb");
    assert(f && fprintf(f, "P6\n1920 1080\n255\n") > 0);
    for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
        unsigned char rgb[3] = { p[i] & 255u, (p[i] >> 8) & 255u, (p[i] >> 16) & 255u };
        assert(fwrite(rgb, 1, sizeof(rgb), f) == sizeof(rgb));
    }
    assert(!fclose(f));
    printf("capture PASS %s\n", name);
}

static void assert_recent_covers(const uint32_t *pixels)
{
    enum { CARD_W = 356, CARD_H = 200, RADIUS = 18 };
    size_t checked = 0;
    unsigned start = browser.recent_selected / 6 * 6;
    unsigned count = x4_recent_count(&recent);
    assert(count == 6 && start == 0);
    for (unsigned i = 0; i < count; ++i) {
        const X4RecentGame *game = x4_recent_get(&recent, start + i);
        assert(game);
        unsigned index = asset_index(game->product_id);
        assert(index < assets.count);
        const X4CoverRaster *cover = &assets.entry[index].surface[X4_COVER_GRID];
        assert(cover->rgba && cover->width == CARD_W && cover->height == CARD_H);
        int cx = 84 + (int)(i % 3) * 384;
        int cy = 176 + (int)(i / 3) * 286;
        /* The focused card has a 3 px stroke plus anti-aliasing. */
        int border = (!browser.top_bar_focused && browser.recent_selected == start + i) ? 4 : 2;
        for (int y = border; y < CARD_H - border; ++y) {
            /* Distances in half-pixels from the corner arc centres. */
            int dy = y < RADIUS ? 2 * (RADIUS - y) - 1 :
                y >= CARD_H - RADIUS ? 2 * (y - (CARD_H - RADIUS)) + 1 : 0;
            for (int x = border; x < CARD_W - border; ++x) {
                int dx = x < RADIUS ? 2 * (RADIUS - x) - 1 :
                    x >= CARD_W - RADIUS ? 2 * (x - (CARD_W - RADIUS)) + 1 : 0;
                if (dx && dy && dx * dx + dy * dy >= 4 * (RADIUS - border) * (RADIUS - border))
                    continue;
                assert(pixels[(size_t)(cy + y) * WIDTH + cx + x] == cover->rgba[(size_t)y * CARD_W + x]);
                ++checked;
            }
        }
    }
    printf("PASS Recientes: %u tarjetas 356x200, %zu pixeles interiores identicos\n", count, checked);
}

static void capture_catalog_baseline(const char *out, uint32_t *pixels)
{
    char summary[256];
    unsigned count = browser.filtered_count;
    unsigned start = count ? x4_catalog_browser_page_start(&browser) : 0;
    int length = snprintf(summary, sizeof(summary), x4_tr(X4_TR_CAT_RESULTS_FMT),
        count, count ? start / 6 + 1 : 0, (count + 5) / 6, "0.7.71-lab");
    assert(length > 0 && (size_t)length < sizeof(summary));

    /* Keep the prior version text as an input to the visual regression.
     * Both renders use the current tree and the same raster path. */
    x4_catalog_gui_draw(&catalog, &browser, &recent, &assets, &status, pixels);
    uint32_t *current = malloc(frame_bytes);
    assert(current);
    memcpy(current, pixels, frame_bytes);
    status.notice = summary;
    x4_catalog_gui_draw(&catalog, &browser, &recent, &assets, &status, pixels);
    status.notice = NULL;

    /* Only the visible summary rectangle may differ with the live identity. */
    unsigned changed = 0;
    for (int y = 0; y < HEIGHT; ++y) {
        const uint32_t *row = pixels + (size_t)y * WIDTH;
        const uint32_t *old = current + (size_t)y * WIDTH;
        if (y >= 126 && y < 160) {
            assert(!memcmp(row, old, 1256 * sizeof(*row)));
            assert(!memcmp(row + 1836, old + 1836, (WIDTH - 1836) * sizeof(*row)));
            for (int x = 1256; x < 1836; ++x) changed += row[x] != old[x];
        } else {
            assert(!memcmp(row, old, WIDTH * sizeof(*row)));
        }
    }
    free(current);
    assert(changed);
    printf("05-catalogo fixture: %s (identidad real %s; %u pixeles cambian solo en el rotulo)\n",
        summary, X4_PRODUCT_VERSION, changed);
    capture(out, "05-catalogo", pixels);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    assert(!x4_font_init("assets/fonts/noto_sans.x4font"));
    fixtures(argv[1]);
    uint32_t *pixels = malloc(frame_bytes);
    assert(pixels);

    x4_catalog_gui_home_prewarm(pixels);
    assert(x4_catalog_browser_home_tick(&browser, &catalog, 100));
    assert(!strcmp(browser.home_background_id, forza));
    publish_background(argv[1], forza);
    x4_catalog_gui_draw_cached(&catalog, &browser, &recent, &assets, &status, pixels);
    capture(argv[2], "01-jugar-forza", pixels);

    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 0, 0, 1));
    assert(browser.active_tab == X4_CAT_TAB_CATALOG);
    capture_catalog_baseline(argv[2], pixels);
    assert(!assets.pins);

    X4CoverRaster fortnite_grid = {0};
    if (decode(argv[1], fortnite, "356x200", X4_COVER_GRID, &fortnite_grid)) {
        unsigned index = asset_index(fortnite);
        assert(index < assets.count);
        free(assets.entry[index].surface[X4_COVER_GRID].rgba);
        assets.entry[index].surface[X4_COVER_GRID] = fortnite_grid;
        puts("06-recientes fixture: entrada Fortnite 356x200 del servidor");
    }
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 0, 0, 1));
    assert(browser.active_tab == X4_CAT_TAB_RECENTS);
    x4_catalog_gui_draw(&catalog, &browser, &recent, &assets, &status, pixels);
    assert_recent_covers(pixels);
    capture(argv[2], "06-recientes", pixels);

    x4_catalog_gui_home_invalidate();
    for (unsigned i = 0; i < assets.count; ++i) {
        for (unsigned j = 0; j < X4_COVER_SURFACES; ++j) free(assets.entry[i].surface[j].rgba);
    }
    for (unsigned i = 0; i < 2; ++i) free(assets.backgrounds[i].raster.rgba);
    free(pixels);
    x4_font_close();
    return 0;
}
