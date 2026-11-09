/* SPDX-License-Identifier: GPL-3.0-only */
#include "apple_ui.h"
#include "catalog_gui.h"
#include "font.h"
#include "json.h"
#include "lang.h"
#include "release_check.h"
#include "screen.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WIDTH = 1920, HEIGHT = 1080, MAX_PRODUCTS = 32, GUARDED_WORDS = 16 };
static const size_t pixel_count = (size_t)WIDTH * HEIGHT;
static const uint32_t sentinel = UINT32_C(0xa713e592);
static const char forza[] = "9NKX70BBCDRN";
static const char fortnite[] = "BT5P2X999VH2";
static X4CatalogSnapshot catalog;
static X4CatalogBrowser browser;
static X4RecentGames recent;
static X4HomeSigls sigls;
static X4CatalogGuiStatus status = {.connected = 1};

struct X4CatalogAssets {
    X4CatalogAssetView entries[MAX_PRODUCTS];
    unsigned count, active, pins;
    uint64_t revision;
    bool background_available;
    X4CatalogBackgroundView backgrounds[2];
#if X4_CAPTURE39_PROTOTYPE
    enum X4CatalogAssetState background_state[MAX_PRODUCTS];
#endif
};
static struct X4CatalogAssets assets;

enum X4UpdateStatus x4_release_check_status(char *out, size_t capacity)
{
    if (capacity) out[0] = 0;
    return X4_UPDATE_LATEST;
}

static unsigned asset_index(const X4CatalogAssets *a, const char *id)
{
    for (unsigned i = 0; i < a->count; ++i)
        if (!strcmp(id, a->entries[i].metadata.product_id)) return i;
    return a->count;
}

int __wrap_x4_catalog_assets_acquire(X4CatalogAssets *a, const char *id, X4CatalogAssetView *out)
{
    if (!a || !id) return 0;
    unsigned i = asset_index(a, id);
    if (i == a->count) return 0;
    *out = a->entries[i];
    return 1;
}

void __wrap_x4_catalog_assets_release(X4CatalogAssets *a, X4CatalogAssetView *v)
{
    (void)a;
    (void)v;
}

int __wrap_x4_catalog_assets_acquire_background(X4CatalogAssets *a, X4CatalogBackgroundView *out)
{
    if (!a || !a->background_available || !a->backgrounds[a->active].raster.rgba) return 0;
    *out = a->backgrounds[a->active];
    assert(++a->pins <= 2);
    return 1;
}

void __wrap_x4_catalog_assets_release_background(X4CatalogAssets *a, X4CatalogBackgroundView *v)
{
    assert(a && v && v->raster.rgba && a->pins);
    --a->pins;
}

#if X4_CAPTURE39_PROTOTYPE
int __wrap_x4_catalog_assets_background_status(X4CatalogAssets *a, const char *id,
    enum X4CatalogAssetState *state)
{
    if (!a || !id || !state) return 0;
    unsigned i = asset_index(a, id);
    if (i == a->count) return 0;
    *state = a->background_state[i];
    return 1;
}

static void fixture_background_status(const char *id, enum X4CatalogAssetState state)
{
    unsigned i = asset_index(&assets, id);
    assert(i < assets.count);
    assets.background_state[i] = state;
    ++assets.revision;
}
#endif

void __wrap_x4_catalog_assets_stats(X4CatalogAssets *a, X4CatalogAssetStats *out)
{
    memset(out, 0, sizeof(*out));
    if (a) out->revision = a->revision;
}

static unsigned char *read_file(const char *dir, const char *leaf, size_t *length)
{
    char path[1024];
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
    *length = (size_t)size;
    return data;
}

static X4CoverRaster decode(const char *dir, const char *id, const char *size, enum X4CoverSurface surface)
{
    char leaf[96];
    int count = snprintf(leaf, sizeof(leaf), "images/%s-%s.jpg", id, size);
    assert(count > 0 && (size_t)count < sizeof(leaf));
    size_t length;
    unsigned char *bytes = read_file(dir, leaf, &length);
    if (!bytes) fprintf(stderr, "Falta la muestra pública %s\n", leaf);
    assert(bytes);
    X4CoverRaster raster = {0};
    assert(!x4_product_surface_decode(bytes, length, surface, &raster));
    assert(raster.rgba);
    free(bytes);
    return raster;
}

static void field(X4JsonSpan object, const char *key, char *out, size_t capacity)
{
    X4JsonSpan v;
    assert(x4_json_member(object, key, &v) == 1);
    assert(!x4_json_display_text(v, out, capacity));
}

static void load_sigl(X4JsonSpan lists, const char *key, X4SiglKind kind)
{
    X4JsonSpan list, value;
    assert(x4_json_member(lists, key, &list) == 1);
    size_t cursor = 0;
    X4SiglList *out = &sigls[kind];
    int rc;
    while ((rc = x4_json_item(list, &cursor, &value)) == 1) {
        assert(out->count < X4_CATALOG_MAX);
        assert(!x4_json_token(value, out->ids[out->count++], X4_PRODUCT_ID_CAP));
    }
    assert(rc == 0);
    out->valid = true;
}

static void load_fixtures(const char *dir)
{
    size_t length, cursor = 0;
    unsigned char *body = read_file(dir, "dataset.json", &length);
    assert(body);
    X4JsonSpan root, products, rows, item;
    assert(!x4_json_parse((const char *)body, length, &root));
    assert(x4_json_member(root, "products", &products) == 1);
    assert(x4_json_member(root, "rows", &rows) == 1);

    int rc;
    while ((rc = x4_json_item(products, &cursor, &item)) == 1) {
        char id[X4_PRODUCT_ID_CAP];
        field(item, "id", id, sizeof(id));
        /* Omit known PC-only fixture IDs from the synthetic cloud catalogue. */
        if (!strcmp(id, "9NGP0DJVKSFL") || !strcmp(id, "9PD0Z90216RJ")) continue;
        assert(assets.count < MAX_PRODUCTS);
        X4CatalogAssetView *entry = &assets.entries[assets.count++];
        X4ProductMetadata *m = &entry->metadata;
        assert(x4_product_id_valid(id));
        memcpy(m->product_id, id, strlen(id) + 1);
        field(item, "name", m->name, sizeof(m->name));
        field(item, "publisher", m->publisher, sizeof(m->publisher));
        field(item, "category", m->category, sizeof(m->category));
        field(item, "description", m->description, sizeof(m->description));
        entry->generation = 1;
        entry->state = X4_ASSET_READY;
        entry->surface[X4_COVER_ROW] = decode(dir, id, "384x216", X4_COVER_ROW);
        assert(entry->surface[X4_COVER_ROW].width == 384 && entry->surface[X4_COVER_ROW].height == 216);
#if !X4_CAPTURE39_PROTOTYPE
        entry->surface[X4_COVER_GRID] = decode(dir, id, "384x216", X4_COVER_GRID);
#endif
    }
    assert(rc == 0 && assets.count >= 10);
    unsigned fi = asset_index(&assets, forza), ft = asset_index(&assets, fortnite);
    assert(fi < assets.count && ft < assets.count && fi != ft);
    unsigned unavailable = assets.count - 1;
    assert(unavailable != fi && unavailable != ft);

    /* Force a recognizable pair on the same column of consecutive rows. */
    unsigned order[MAX_PRODUCTS] = {fi, 0, 0, 0, ft};
    unsigned next = 1;
    for (unsigned i = 0; i < assets.count; ++i) {
        if (i == fi || i == ft || i == unavailable) continue;
        if (next == 4) ++next;
        assert(next < MAX_PRODUCTS);
        order[next++] = i;
    }
    catalog.count = catalog.total = next;
    catalog.state = X4_CATALOG_READY;
    for (unsigned i = 0; i < catalog.count; ++i) {
        X4CatalogTitle *title = &catalog.titles[i];
        const X4ProductMetadata *m = &assets.entries[order[i]].metadata;
        snprintf(title->id, sizeof(title->id), "FIXTURE_%u", i);
        snprintf(title->product_id, sizeof(title->product_id), "%s", m->product_id);
        snprintf(title->name, sizeof(title->name), "%.127s", m->name);
        title->entitled = i < 10;
        title->f2p_only = !strcmp(title->product_id, fortnite);
    }
#if !X4_CAPTURE39_PROTOTYPE
    for (unsigned i = 0; i <= 4; i += 4) {
        X4CatalogAssetView *entry = &assets.entries[order[i]];
        entry->surface[X4_COVER_DETAIL] = decode(dir, entry->metadata.product_id, "384x216", X4_COVER_DETAIL);
    }
#endif
    load_sigl(rows, "free", X4_SIGL_FREE);
    load_sigl(rows, "new", X4_SIGL_NEW);
    load_sigl(rows, "popular", X4_SIGL_POPULAR);
    load_sigl(rows, "leaving", X4_SIGL_LEAVING);
    free(body);

    char recent_ids[8][96] = {{0}};
    for (unsigned i = 0; i < 7; ++i) memcpy(recent_ids[i], catalog.titles[i].id, sizeof(recent_ids[i]));
    memcpy(recent_ids[7], "FIXTURE_UNAVAILABLE", sizeof("FIXTURE_UNAVAILABLE"));
    x4_recent_init(&recent);
    x4_recent_begin(&recent, 1, 1);
    assert(x4_recent_publish(&recent, 1, 1, recent_ids, 8, 0, 200, &catalog));
    assert(recent.count == 8);
    /* Synthetic MRU availability, with real public metadata and artwork. */
    const X4ProductMetadata *m = &assets.entries[unavailable].metadata;
    X4RecentGame *missing = &recent.entries[7];
    snprintf(missing->product_id, sizeof(missing->product_id), "%s", m->product_id);
    snprintf(missing->name, sizeof(missing->name), "%.127s", m->name);
    unsigned ignored;
    assert(!x4_recent_catalog_index(missing, &catalog, &ignored));

    x4_catalog_browser_init(&browser);
    browser.apple_mode = true;
    x4_catalog_browser_rebuild(&browser, &catalog);
    x4_catalog_browser_sync_recents(&browser, &recent);
    x4_catalog_browser_home_rebuild(&browser, &catalog, &recent, sigls);
}

static void publish_background(const char *dir, const char *id)
{
    unsigned slot = assets.active ^ 1u;
    X4CatalogBackgroundView *view = &assets.backgrounds[slot];
    free(view->raster.rgba);
    view->raster = decode(dir, id, "1920x1080", X4_COVER_BACKGROUND);
    assert(view->raster.width == WIDTH && view->raster.height == HEIGHT);
    x4_product_compose_background(&view->raster);
    snprintf(view->product_id, sizeof(view->product_id), "%s", id);
    view->generation = 1;
    view->slot = slot;
    assets.active = slot;
    assets.background_available = true;
    ++assets.revision;
#if X4_CAPTURE39_PROTOTYPE
    fixture_background_status(id, X4_ASSET_READY);
#endif
}

static void check_canvas(const uint32_t *pixels)
{
    for (unsigned i = 0; i < GUARDED_WORDS; ++i) {
        assert(pixels[(ptrdiff_t)i - GUARDED_WORDS] == sentinel);
        assert(pixels[pixel_count + i] == sentinel);
    }
}

static void capture(const char *name, const uint32_t *pixels)
{
    check_canvas(pixels);
    assert(printf("FRAME %s\n", name) > 0);
    for (unsigned y = 0; y < HEIGHT; ++y) {
        unsigned char row[WIDTH * 3];
        for (unsigned x = 0; x < WIDTH; ++x) {
            uint32_t pixel = pixels[(size_t)y * WIDTH + x];
            row[3 * x] = (unsigned char)pixel;
            row[3 * x + 1] = (unsigned char)(pixel >> 8);
            row[3 * x + 2] = (unsigned char)(pixel >> 16);
        }
        assert(fwrite(row, 1, sizeof(row), stdout) == sizeof(row));
    }
    fprintf(stderr, "CAPTURE_RENDER_PASS %s\n", name);
}

static void draw_tab(uint32_t *pixels)
{
    x4_catalog_gui_draw_cached(&catalog, &browser, &recent, &assets, &status, pixels);
}

static void tab_capture(const char *name, uint32_t *pixels)
{
    draw_tab(pixels);
    capture(name, pixels);
}

static void capture_menu(const char *data, uint32_t *pixels)
{
    X4CoverRaster featured = decode(data, forza, "852x480", X4_COVER_HOME_FEATURE);
    X4CoverRaster battle = decode(data, fortnite, "796x448", X4_COVER_HOME_FORTNITE);
    assert(featured.width == 852 && featured.height == 448);
    assert(battle.width == 420 && battle.height == 448);
    x4_catalog_assets_compose_card(&featured);
    x4_catalog_assets_compose_card(&battle);
    X4HomeDrawContext context = {.signed_in = 1, .featured_hero = &featured, .fortnite_cover = &battle};
    X4Screen screen = {.page = 0, .selected = 0, .signed_in = 1};
    X4Controller controller = {0};
    x4_screen_set_home_context(&context);
    x4_screen_draw(&screen, &controller, pixels);
    capture("12-menu", pixels);
    x4_screen_set_home_context(NULL);
    free(featured.rgba);
    free(battle.rgba);
}

static void set_tab(enum X4CatalogTab tab)
{
    browser.active_tab = tab;
    browser.top_bar_focused = false;
    browser.section = tab == X4_CAT_TAB_RECENTS ? X4_CATALOG_SECTION_RECENT : X4_CATALOG_SECTION_CATALOG;
}

#if X4_CAPTURE39_PROTOTYPE
static void query(const char *text)
{
    x4_catalog_browser_query_clear(&browser);
    while (*text) assert(x4_catalog_browser_query_append(&browser, *text++));
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(browser.selected == 0);
}
static uint64_t clock_us(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void damage_frame(uint32_t *pixels, unsigned step, const char *category, bool full)
{
    if (full) x4_catalog_gui_home_test_force_full();
    uint64_t start = clock_us();
    draw_tab(pixels);
    uint64_t elapsed = clock_us() - start;
    X4UiRect rects[5];
    size_t count = x4_catalog_gui_home_damage(rects, NULL);
    if (!full && (!strcmp(category, "horizontal") || !strcmp(category, "vertical"))) {
        assert(count > 0 && count <= (!strcmp(category, "horizontal") ? 2u : 3u));
        for (size_t i = 0; i < count; ++i) {
            assert(rects[i].y == 112 || rects[i].y == 486 || rects[i].y == 768);
        }
    }
    if (!full && !strcmp(category, "background")) assert(count == SIZE_MAX);
    size_t bytes = count == SIZE_MAX ? pixel_count * sizeof(*pixels) : 0;
    if (count != SIZE_MAX)
        for (size_t i = 0; i < count; ++i)
            bytes += (size_t)rects[i].w * (size_t)rects[i].h * sizeof(*pixels);
    fprintf(stderr, "DAMAGE39 %s %u %s %s %llu %zu\n",
        full ? "full" : "incremental", step,
        browser.active_tab == X4_CAT_TAB_CATALOG ? "catalogo" : "recientes", category,
        (unsigned long long)elapsed, bytes);
    check_canvas(pixels);
    assert(printf("FRAME damage-%02u\n", step)>0);
    assert(fwrite(pixels,sizeof(*pixels),pixel_count,stdout)==pixel_count);
}

static void damage_test(const char *data, uint32_t *pixels, bool full)
{
    unsigned step = 0;
    static const int steps[4][2] = {{1,0},{0,1},{-1,0},{0,-1}};
    set_tab(X4_CAT_TAB_CATALOG);
    damage_frame(pixels, step++, "first", full);
    for (unsigned tab = 0; tab < 2; ++tab) {
        if (tab) {
            assert(x4_catalog_browser_apple_nav(&browser,&catalog,&recent,0,0,1));
            damage_frame(pixels, step++, "tab", full);
        }
        for (unsigned j = 0; j < 16; ++j) {
            int dx = steps[j % 4][0], dy = steps[j % 4][1];
            assert(x4_catalog_browser_apple_nav(&browser,&catalog,&recent,dx,dy,0));
            damage_frame(pixels, step++, dx ? "horizontal" : "vertical", full);
        }
        if (!tab) {
            assert(x4_catalog_browser_cycle_filter(&browser,1));
            x4_catalog_browser_rebuild(&browser,&catalog);
            damage_frame(pixels, step++, "filter", full);
            assert(x4_catalog_browser_cycle_filter(&browser,-1));
            x4_catalog_browser_rebuild(&browser,&catalog);
            query("Forza");
            damage_frame(pixels, step++, "query", full);
            publish_background(data, forza);
            damage_frame(pixels, step++, "background", full);
            query("");
            damage_frame(pixels, step++, "query", full);
            assets.background_available = false;
            fixture_background_status(forza, X4_ASSET_MISSING);
            damage_frame(pixels, step++, "neutral", full);
        } else {
            publish_background(data, forza);
            damage_frame(pixels, step++, "background", full);
        }
    }
    assert(step >= 38);
}

#endif

int main(int argc, char **argv)
{
#if X4_CAPTURE39_PROTOTYPE
    assert(argc == 3 || argc == 4);
#else
    assert(argc == 3);
#endif
    const char *data = argv[1], *menu_data = argv[2];
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    assert(!x4_font_init("assets/fonts/noto_sans.x4font"));
    assert(!x4_font_is_fallback());
    load_fixtures(data);
    uint32_t *raw = malloc((pixel_count + 2 * GUARDED_WORDS) * sizeof(*raw));
    assert(raw);
    uint32_t *pixels = raw + GUARDED_WORDS;
    for (size_t i = 0; i < pixel_count + 2 * GUARDED_WORDS; ++i) raw[i] = sentinel;

#if X4_CAPTURE39_PROTOTYPE
    for (unsigned i = 0; i < assets.count; ++i) assets.background_state[i] = X4_ASSET_MISSING;
    if (argc == 4) {
        assert(!strcmp(argv[3], "damage-full") || !strcmp(argv[3], "damage-incremental"));
        damage_test(data, pixels, !strcmp(argv[3], "damage-full"));
        goto release;
    }
#endif
    capture_menu(menu_data, pixels);
    x4_catalog_gui_home_prewarm(pixels);
    set_tab(X4_CAT_TAB_DISCOVER);
    assert(x4_catalog_browser_home_tick(&browser, &catalog, 100));
    assert(!strcmp(browser.home_background_id, forza));
    publish_background(data, forza);
    draw_tab(pixels);
    capture("11-descubrir", pixels);
    x4_catalog_gui_home_invalidate();
    assert(!assets.pins);

    set_tab(X4_CAT_TAB_CATALOG);
    assert(browser.selected == 0 && browser.filter == X4_CATALOG_FILTER_ALL);
    tab_capture("01-catalogo-todos", pixels);

#if X4_CAPTURE39_PROTOTYPE
    x4_catalog_browser_move(&browser, &catalog, 4, true);
    assert(browser.selected == 4 && !strcmp(catalog.titles[browser.indices[4]].product_id, fortnite));
    uint32_t previous_background = pixels[pixel_count - 1];
    fixture_background_status(fortnite, X4_ASSET_PENDING);
    tab_capture("10a-catalogo-fondo-en-camino", pixels);
    assert(pixels[pixel_count - 1] == previous_background);
    fixture_background_status(fortnite, X4_ASSET_TEMPORARY_FAILURE);
    tab_capture("10b-catalogo-fondo-fallido", pixels);
    uint32_t neutral = pixels[pixel_count - 1];
    assert(neutral != previous_background);
    uint32_t *neutral_canvas = malloc(pixel_count * sizeof(*neutral_canvas));
    assert(neutral_canvas);
    x4_apple_bg_draw(neutral_canvas);
    assert(neutral == neutral_canvas[pixel_count - 1]);
    free(neutral_canvas);
    publish_background(data, fortnite);
    tab_capture("02-catalogo-segunda-fila", pixels);
    assert(pixels[pixel_count - 1] == assets.backgrounds[assets.active].raster.rgba[pixel_count - 1]);

    assert(x4_catalog_browser_cycle_filter(&browser, 1));
    query("Forza");
    assert(browser.filter == X4_CATALOG_FILTER_CONFIRMED_ACCESS && browser.filtered_count);
    publish_background(data, forza);
    tab_capture("03-catalogo-acceso-busqueda", pixels);

    assert(x4_catalog_browser_cycle_filter(&browser, -1));
    query("Fortnite");
    assert(browser.filtered_count == 1);
    publish_background(data, fortnite);
    browser.detail_scroll_offset = 1;
    tab_capture("04-catalogo-descripcion", pixels);
    browser.detail_scroll_offset = 0;

    query("ZZZ 997");
    assert(!browser.filtered_count);
    tab_capture("05-catalogo-sin-resultados", pixels);
    assert(pixels[pixel_count - 1] == neutral);
    query("");

    set_tab(X4_CAT_TAB_RECENTS);
    browser.recent_selected = 7;
    memcpy(browser.recent_selected_id, recent.entries[7].id, sizeof(browser.recent_selected_id));
    assert(!x4_catalog_browser_selected_title(&browser, &catalog, &recent));
    tab_capture("06-recientes-no-disponible", pixels);
    assert(pixels[pixel_count - 1] == neutral);

    x4_recent_begin(&recent, 1, 2);
    x4_catalog_browser_sync_recents(&browser, &recent);
    tab_capture("07a-recientes-cargando", pixels);
    assert(!x4_recent_publish(&recent, 1, 2, NULL, 0, -5000, 503, &catalog));
    assert(recent.state == X4_RECENT_ERROR);
    x4_catalog_browser_sync_recents(&browser, &recent);
    tab_capture("07b-recientes-error", pixels);
    assert(pixels[pixel_count - 1] == neutral);
    x4_recent_begin(&recent, 1, 3);
    assert(x4_recent_publish(&recent, 1, 3, NULL, 0, 0, 200, &catalog));
    x4_catalog_browser_sync_recents(&browser, &recent);
    tab_capture("09-recientes-vacio", pixels);

    set_tab(X4_CAT_TAB_CATALOG);
    publish_background(data, forza);
    for (unsigned i = 1; i < 8; ++i) {
        unsigned idx = asset_index(&assets, catalog.titles[i].product_id);
        assert(idx < assets.count);
        X4CatalogAssetView *entry = &assets.entries[idx];
        free(entry->surface[X4_COVER_ROW].rgba);
        entry->surface[X4_COVER_ROW] = (X4CoverRaster){0};
        entry->state = X4_ASSET_PENDING;
    }
    ++assets.revision;
    tab_capture("08-catalogo-portadas-pendientes", pixels);
#else
    set_tab(X4_CAT_TAB_RECENTS);
    browser.recent_selected = 7;
    memcpy(browser.recent_selected_id, recent.entries[7].id, sizeof(browser.recent_selected_id));
    tab_capture("06-recientes-no-disponible", pixels);
#endif

#if X4_CAPTURE39_PROTOTYPE
release:
#endif
    x4_catalog_gui_home_invalidate();
    assert(!assets.pins);
    for (unsigned i = 0; i < assets.count; ++i)
        for (unsigned j = 0; j < X4_COVER_SURFACES; ++j) free(assets.entries[i].surface[j].rgba);
    for (unsigned i = 0; i < 2; ++i) free(assets.backgrounds[i].raster.rgba);
    free(raw);
    assert(!fflush(stdout));
    x4_font_close();
    return 0;
}
