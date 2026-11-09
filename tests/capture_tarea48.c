/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_CAPTURE47_MAIN x4_capture47_unused_main
#define x4_ui46_capture_footer capture47_unused_footer
#include "capture_tarea47.c"
#undef x4_ui46_capture_footer
#undef X4_CAPTURE47_MAIN

#if X4_CAPTURE48_AFTER
#include "catalog_presentation.h"
#include "favorites.h"
#include "play_history.h"
#endif
#include <ctype.h>

static uint32_t *footer_before[3];
static unsigned footer_depth;
static unsigned audited_text;
static int lowest_content_ink[1920];
static int first_footer_ink[1920];
static unsigned scene_footers;

static void begin48(const char *name)
{
    CAPTURE_SCENE(name);
    for (size_t x = 0; x < WIDTH; ++x) {
        lowest_content_ink[x] = -1;
        first_footer_ink[x] = HEIGHT;
    }
    scene_footers = 0;
}

static void finish48(const char *name)
{
    assert(!footer_depth && active_scene && !strcmp(name, active_scene));
    int max_content_y = -1;
    for (size_t x = X4_UI2_SIDEBAR_W; x < X4_UI_SAFE_RIGHT; ++x) {
        if (lowest_content_ink[x] > max_content_y) max_content_y = lowest_content_ink[x];
        if (lowest_content_ink[x] >= first_footer_ink[x]) {
            fprintf(stderr, "B_FOOTER_OVERLAP %s x=%zu text_bottom=%d footer_top=%d\n",
                name, x, lowest_content_ink[x], first_footer_ink[x]);
            assert(lowest_content_ink[x] < first_footer_ink[x]);
        }
    }
    if (!strcmp(name, "inicio"))
        assert(scene_footers && max_content_y < X4_UI_FOOTER_Y - 24);
    fprintf(stderr, "B_FOOTER_PASS %s content_bottom=%d footer_sections=%u\n",
        name, max_content_y, scene_footers);
}

/* Capture-only: compare the actual glyph changes inside the prepared font
 * bounds so that descenders, antialiasing and clipped ellipses are measured. */
static bool text_bounds(X4UiRect *bounds, int x, int y, int size, int width)
{
    if (!active_scene || footer_depth || x < X4_UI2_SIDEBAR_W ||
        y < 840 || y >= HEIGHT || width <= 0) return false;
    int right = x + width + 4;
    if (right > WIDTH) right = WIDTH;
    int bottom = y + size + 10;
    if (bottom > HEIGHT) bottom = HEIGHT;
    *bounds = (X4UiRect){x, y, right - x, bottom - y};
    assert(bounds->w <= WIDTH && bounds->h <= 96);
    return bounds->w > 0;
}

static uint32_t text_before[1920 * 96];

static void ink_snapshot(const uint32_t *pixels, X4UiRect bounds)
{
    for (int row = 0; row < bounds.h; ++row)
        memcpy(text_before + row * bounds.w, pixels + (bounds.y + row) * WIDTH + bounds.x,
            (size_t)bounds.w * sizeof(*pixels));
}

static void ink_diff(const uint32_t *pixels, X4UiRect bounds)
{
    for (int row = 0; row < bounds.h; ++row) {
        const int y = bounds.y + row;
        for (int col = 0; col < bounds.w; ++col) {
            const int x = bounds.x + col;
            if (text_before[row * bounds.w + col] != pixels[y * WIDTH + x] &&
                lowest_content_ink[x] < y)
                lowest_content_ink[x] = y;
        }
    }
}

static void inspect_text(const char *text)
{
    if (!active_scene || !text) return;
    ++audited_text;
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
        const bool fps = tolower(c[0]) == 'f' && tolower(c[1]) == 'p' && tolower(c[2]) == 's';
        if (fps || c[0] == '%') {
            fprintf(stderr, "Texto excluido en %s: %s\n", active_scene, text);
            assert(!fps && c[0] != '%');
        }
    }
}

void __real_x4_text_px(uint32_t *, int, int, int, int, const char *, uint32_t);
void __wrap_x4_text_px(uint32_t *p, int x, int y, int size, int weight,
    const char *text, uint32_t color)
{
    inspect_text(text);
    X4UiRect bounds;
    const bool check = text && y >= 840 && text_bounds(&bounds, x, y, size,
        x4_text_width_px(size, weight, text));
    if (check) ink_snapshot(p, bounds);
    __real_x4_text_px(p, x, y, size, weight, text, color);
    if (check) ink_diff(p, bounds);
}

void __real_x4_text_ellipsis_px(uint32_t *, int, int, int, int, const char *, int, uint32_t);
void __wrap_x4_text_ellipsis_px(uint32_t *p, int x, int y, int size, int weight,
    const char *text, int width, uint32_t color)
{
    inspect_text(text);
    X4UiRect bounds;
    const bool check = text && text_bounds(&bounds, x, y, size, width);
    if (check) ink_snapshot(p, bounds);
    __real_x4_text_ellipsis_px(p, x, y, size, weight, text, width, color);
    if (check) ink_diff(p, bounds);
}

void x4_ui46_capture_footer(const uint32_t *pixels, int x, int y, int w, int h, bool after)
{
    if (!active_scene) return;
    assert(x >= X4_UI_SAFE_LEFT && y >= 0 && x + w <= X4_UI_SAFE_RIGHT && y + h <= X4_UI_SAFE_BOTTOM);
    if (!after) {
        assert(footer_depth < sizeof(footer_before) / sizeof(*footer_before));
        uint32_t *snapshot = malloc(pixel_count * sizeof(*pixels));
        assert(snapshot);
        memcpy(snapshot, pixels, pixel_count * sizeof(*pixels));
        footer_before[footer_depth++] = snapshot;
        return;
    }
    assert(footer_depth);
    uint32_t *snapshot = footer_before[--footer_depth];
    size_t changed = 0;
    ++scene_footers;
    for (size_t i = 0; i < pixel_count; ++i) {
        if (snapshot[i] == pixels[i]) continue;
        unsigned col = i % WIDTH, row = i / WIDTH;
        assert(col >= X4_UI_SAFE_LEFT && col < X4_UI_SAFE_RIGHT && row < X4_UI_SAFE_BOTTOM);
        if (row < (unsigned)first_footer_ink[col]) first_footer_ink[col] = (int)row;
        ++changed;
    }
    fprintf(stderr, "CAPTURE48 FOOTER %s %zu\n", active_scene, changed);
    free(snapshot);
}

#if X4_CAPTURE48_AFTER
static X4CatalogPresentation genres;
static X4Favorites favorites, no_favorites;
static X4History history;
static uint64_t fixture_now;

void x4_ui48_capture_cover(X4UiRect bounds, const X4CoverRaster *source)
{
    if (!active_scene || !source || !source->rgba) return;
    assert(bounds.w * 9 == bounds.h * 16 && source->width * 9 == source->height * 16);
    safe47(active_scene, bounds);
    fprintf(stderr, "CAPTURE48 COVER %s %dx%d -> %dx%d\n", active_scene,
        source->width, source->height, bounds.w, bounds.h);
}

static void fixture_genres(void)
{
    static X4ProductMetadata batch[MAX_PRODUCTS];
    const char *const multiplayer[] = {
        forza, fortnite, "9PBG4MTM3JJC", "9NNFG8BQRCXL"
    };
    const char *const open_world[] = {
        forza, "9N0QWXC09MPT", "9N76QGDS5GJR", "C2N9CS4FS1QR"
    };
    x4_catalog_presentation_init(&genres);
    x4_catalog_presentation_set_generation(&genres, 1);
    for (unsigned i = 0; i < catalog.count; ++i) {
        unsigned index = asset_index(&assets, catalog.titles[i].product_id);
        assert(index < assets.count);
        batch[i] = assets.entries[index].metadata;
    }
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_DISCOVER);
    x4_catalog_browser_home_rebuild(&browser, &catalog, &recent, sigls);
    assert(x4_catalog_presentation_ingest(&genres, 1, 1, batch, catalog.count));
    X4GameDetails attribute_sample = {.attributes = X4_GAME_ATTR_ONLINE_MULTIPLAYER};
    for (unsigned i = 0; i < sizeof(multiplayer) / sizeof(*multiplayer); ++i)
        assert(x4_catalog_presentation_ingest_detail(&genres, multiplayer[i], &attribute_sample));
    for (unsigned i = 0; i < sizeof(open_world) / sizeof(*open_world); ++i) {
        attribute_sample.attributes = X4_GAME_ATTR_OPEN_WORLD |
            (i == 0 ? X4_GAME_ATTR_ONLINE_MULTIPLAYER : 0);
        assert(x4_catalog_presentation_ingest_detail(&genres, open_world[i], &attribute_sample));
    }
    assert(x4_catalog_presentation_apply_genres(&genres, &browser, &catalog));
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(browser.ui2_collection_count >= 2);
}

static void choose48(enum X4CatalogTab tab)
{
    x4_catalog_browser_ui2_select(&browser, tab);
    x4_catalog_browser_rebuild(&browser, &catalog);
}
#else
static void choose48(enum X4CatalogTab tab)
{
    set_tab(tab);
    x4_catalog_browser_rebuild(&browser, &catalog);
}
#endif

static void tab48(const char *name, X4Ui2Section section, uint32_t *pixels)
{
    x4_catalog_gui_home_invalidate();
    x4_catalog_gui_sidebar_focused(false);
    x4_ui2_shell_set_section(&shell, section);
    begin48(name);
    draw_tab(pixels);
    x4_ui2_shell_draw(pixels, &shell, &status47);
    finish48(name);
    capture(name, pixels);
    CAPTURE_SCENE(NULL);
}

static void query48(const char *query)
{
    x4_catalog_browser_query_clear(&browser);
    for (const char *c = query; *c; ++c)
        assert(x4_catalog_browser_query_append(&browser, *c));
    x4_catalog_browser_rebuild(&browser, &catalog);
}

static void home48(uint32_t *pixels)
{
    X4Ui2HomeModel model;
    X4UiRect damage[2];
    x4_rect(pixels, 0, 0, WIDTH, HEIGHT, X4_UI2_BACKGROUND);
    x4_ui2_home_model(&model, true, &catalog, &recent, &browser.home_rows);
    begin48("inicio");
    (void)x4_ui2_home_draw(&model, &catalog, &assets, 1, X4_UI2_HOME_CONTINUE,
        false, pixels, damage);
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    x4_ui2_shell_draw(pixels, &shell, &status47);
    finish48("inicio");
    capture("inicio", pixels);
    CAPTURE_SCENE(NULL);
    x4_ui2_home_close();
}

static void tabs48(const char *data, uint32_t *pixels)
{
    x4_catalog_gui_home_prewarm(pixels);
#if X4_CAPTURE48_AFTER
    fixture_genres();
#endif
    choose48(X4_CAT_TAB_DISCOVER);
    x4_catalog_browser_rebuild_featured(&browser, &catalog, &recent);
    assert(browser.featured_count >= 3 && browser.featured_count <= 5);
    x4_catalog_browser_ui2_library_sync(&browser, &catalog, &recent, &history);
    unsigned fortnite_source = 0;
    while (fortnite_source < catalog.count &&
        strcmp(catalog.titles[fortnite_source].product_id, fortnite)) ++fortnite_source;
    assert(fortnite_source < catalog.count);
    /* Keep a known second raster in the existing fixture shortlist. */
    for (unsigned i = 2; i < browser.featured_count; ++i)
        if (browser.featured_sources[i] == fortnite_source)
            browser.featured_sources[i] = browser.featured_sources[1];
    browser.featured_sources[1] = fortnite_source;
    browser.featured_index = 0;
    browser.hero_focused = false;
    (void)x4_catalog_browser_home_tick(&browser, &catalog, 100);
    publish_background(data, forza);
    tab48("descubrir", X4_UI2_DISCOVER, pixels);
    browser.hero_focused = true;
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 1, 0, 0));
    assert(!strcmp(x4_catalog_browser_ui2_hero(&browser, &catalog)->product_id, fortnite));
    (void)x4_catalog_browser_home_tick(&browser, &catalog, 200);
    (void)x4_catalog_browser_home_tick(&browser, &catalog, 400000);
    publish_background(data, fortnite);
    uint64_t total = 0;
    for (unsigned step = 0; step < 3; ++step) {
        unsigned marker = x4_catalog_gui_discover_clock(1000000u + step * 60000u);
        assert(step == 0 || marker == step + 1);
        uint64_t start = clock_us();
        draw_tab(pixels);
        uint64_t elapsed = clock_us() - start;
        total += elapsed;
        X4UiRect rects[8];
        size_t damage = x4_catalog_gui_home_damage(rects, NULL);
        assert(damage && damage != SIZE_MAX && damage <= 5);
        if (step > 0)
            assert(damage == 1 && rects[0].h <= 425 && rects[0].x == 0);
        uint64_t compose_us = 0;
        const unsigned rendered = x4_catalog_gui_discover_fade_step(&compose_us);
        assert(rendered == (step < 2 ? step + 1 : 0));
        fprintf(stderr, "B_FADE step=%u time_us=%llu compose_us=%llu damage=%zu\n", step,
            (unsigned long long)elapsed, (unsigned long long)compose_us, damage);
    }
    fprintf(stderr, "B_FADE_TOTAL total_us=%llu duration_ms=120\n", (unsigned long long)total);
    (void)x4_catalog_gui_discover_clock(0);
    tab48("descubrir-segundo", X4_UI2_DISCOVER, pixels);
    browser.hero_focused = false;
    browser.ui2_collection_focus = true;
    assert(browser.home_rows.count > 0);
    browser.home_first_row = browser.home_rows.count - 1;
    browser.home_row = browser.home_first_row;
    tab48("descubrir-colecciones", X4_UI2_DISCOVER, pixels);
    choose48(X4_CAT_TAB_CATALOG);
    tab48("catalogo-todos", X4_UI2_CATALOG, pixels);
#if X4_CAPTURE48_AFTER
    browser.ui2_content_focus = X4_UI2_CATALOG_GRID;
    for (unsigned i = 0; i < browser.ui2_genre_count; ++i) {
        assert(x4_catalog_browser_ui2_cycle_chip(&browser, &catalog, 1));
        if (!strcmp(browser.ui2_genres[browser.ui2_genre].label, "Shooter")) break;
    }
    assert(!strcmp(browser.ui2_genres[browser.ui2_genre].label, "Shooter"));
    assert(browser.filtered_count > 0 && browser.filtered_count < catalog.count);
    tab48("catalogo-genero", X4_UI2_CATALOG, pixels);
    browser.ui2_content_focus = X4_UI2_CATALOG_ACCESS;
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.ui2_selector_open);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 0, 1, 0));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.filter == X4_CATALOG_FILTER_CONFIRMED_ACCESS);
#else
    assert(x4_catalog_browser_cycle_filter(&browser, 1));
    x4_catalog_browser_rebuild(&browser, &catalog);
#endif
    tab48("catalogo-acceso", X4_UI2_CATALOG, pixels);
    browser.ui2_content_focus = X4_UI2_CATALOG_SORT;
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.ui2_selector_open);
    tab48("catalogo-orden", X4_UI2_CATALOG, pixels);
    assert(x4_catalog_browser_ui2_close_selector(&browser));
#if X4_CAPTURE48_AFTER
    assert(x4_favorites_toggle(&favorites, catalog.titles[0].product_id, catalog.titles[0].name, NULL) == X4_FAVORITES_ADDED);
    assert(x4_favorites_toggle(&favorites, catalog.titles[2].product_id, catalog.titles[2].name, NULL) == X4_FAVORITES_ADDED);
    x4_catalog_browser_ui2_favorites(&browser, &favorites);
    browser.ui2_genre = 0;
    browser.ui2_genre_slot = 0;
    browser.ui2_collection_filter = X4_UI2_COLLECTION_NONE;
    browser.filter = X4_CATALOG_FILTER_FAVORITES;
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(browser.filtered_count == 2);
    tab48("catalogo-favoritos", X4_UI2_CATALOG, pixels);
    browser.filter = X4_CATALOG_FILTER_ALL;
#endif
    choose48(X4_CAT_TAB_RECENTS);
    x4_catalog_browser_ui2_library_sync(&browser, &catalog, &recent, &history);
    assert(browser.library.bound && browser.library.members > 2);
    browser.library.focus = X4_UI2_LIBRARY_CONTINUE;
    tab48("mis-juegos", X4_UI2_LIBRARY, pixels);
    assert(x4_catalog_browser_ui2_library_manage(&browser));
    tab48("mis-juegos-administrar", X4_UI2_LIBRARY, pixels);
    assert(x4_catalog_browser_ui2_close_selector(&browser));
    browser.library.focus = X4_UI2_LIBRARY_SORT;
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    tab48("mis-juegos-orden", X4_UI2_LIBRARY, pixels);
    assert(x4_catalog_browser_ui2_close_selector(&browser));
    browser.library.focus = X4_UI2_LIBRARY_TABS;
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 1, 0, 0));
    browser.library.focus = X4_UI2_LIBRARY_GRID;
    tab48("mis-juegos-favoritos", X4_UI2_LIBRARY, pixels);
    browser.library.focus = X4_UI2_LIBRARY_TABS;
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 1, 0, 0));
    browser.library.focus = X4_UI2_LIBRARY_GRID;
    tab48("mis-juegos-recientes", X4_UI2_LIBRARY, pixels);
    X4RecentGames saved = recent;
    x4_recent_init(&recent);
    x4_catalog_browser_sync_recents(&browser, &recent);
#if X4_CAPTURE48_AFTER
    x4_catalog_browser_ui2_favorites(&browser, &no_favorites);
#endif
    enum X4CatalogState saved_state = catalog.state;
    catalog.state = X4_CATALOG_IDLE;
    x4_catalog_browser_ui2_library_sync(&browser, &catalog, &recent, NULL);
    browser.library.focus = X4_UI2_LIBRARY_TABS;
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, -1, 0, 0));
    tab48("mis-juegos-favoritos-vacio", X4_UI2_LIBRARY, pixels);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, -1, 0, 0));
    tab48("mis-juegos-vacio", X4_UI2_LIBRARY, pixels);
    catalog.state = saved_state;
    recent = saved;
    x4_catalog_browser_sync_recents(&browser, &recent);
#if X4_CAPTURE48_AFTER
    choose48(X4_CAT_TAB_SEARCH);
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_KEYBOARD);
#else
    choose48(X4_CAT_TAB_CATALOG);
    browser.filter = X4_CATALOG_FILTER_ALL;
#endif
    query48("Forza");
    assert(browser.filtered_count > 0);
#if X4_CAPTURE48_AFTER
    browser.editing = false;
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, 0, 1, 0));
    assert(browser.ui2_content_focus == X4_UI2_CATALOG_GRID);
#endif
    tab48("buscar-resultados", X4_UI2_SEARCH, pixels);
#if X4_CAPTURE48_AFTER
    browser.editing = true;
    tab48("buscar-teclado", X4_UI2_SEARCH, pixels);
    browser.editing = false;
    x4_catalog_gui_keyboard_cache_invalidate();
#endif
    query48("ZZZNORESULT");
    assert(!browser.filtered_count);
    tab48("buscar-vacio", X4_UI2_SEARCH, pixels);
    x4_catalog_gui_home_invalidate();
}

static void game48(const char *name, X4GamePage *page, const X4GamePageModel *model,
    uint32_t *pixels, uint32_t *scratch)
{
    begin48(name);
#if X4_CAPTURE48_AFTER
    (void)scratch;
    x4_game_page_draw(page, model, pixels);
#else
    x4_game_page_draw(page, model, scratch);
    x4_ui2_shell_fit_legacy(pixels, scratch);
#endif
    x4_ui2_shell_set_section(&shell, X4_UI2_CATALOG);
    x4_ui2_shell_draw(pixels, &shell, &status47);
    finish48(name);
    capture(name, pixels);
    CAPTURE_SCENE(NULL);
}

static void games48(const char *data, const char *fixtures, uint32_t *pixels, uint32_t *scratch)
{
    unsigned art = asset_index(&assets, forza);
    assert(art < assets.count);
    const X4ProductMetadata *meta = &assets.entries[art].metadata;
    publish_background(data, forza);
    static X4GameDetails detail;
    snprintf(detail.title, sizeof(detail.title), "%s", meta->name);
    snprintf(detail.publisher, sizeof(detail.publisher), "%s", meta->publisher);
    snprintf(detail.developer, sizeof(detail.developer), "%s", meta->developer);
    snprintf(detail.category, sizeof(detail.category), "%s", "Racing & flying");
    snprintf(detail.year, sizeof(detail.year), "%s", "2021");
    snprintf(detail.age_rating, sizeof(detail.age_rating), "%s", "ESRB E");
    snprintf(detail.description, sizeof(detail.description), "%s", meta->description);
    detail.screenshot_count = X4_GAME_SCREENSHOT_MAX;
#if X4_CAPTURE48_AFTER
    snprintf(detail.release_date, sizeof(detail.release_date), "%s", "2021-11-09");
    detail.attributes = X4_GAME_ATTR_SINGLE_PLAYER | X4_GAME_ATTR_ONLINE_MULTIPLAYER |
        X4_GAME_ATTR_COOP | X4_GAME_ATTR_GAMEPAD;
    const char *const related_id = "9PBG4MTM3JJC";
    X4GamePageSimilar similar[X4_GAME_SIMILAR_MAX];
    unsigned indexes[X4_UI2_SIMILAR_MAX];
    unsigned source = 0;
    while (source < catalog.count && strcmp(catalog.titles[source].product_id, related_id)) ++source;
    assert(source < catalog.count);
    unsigned similar_count = x4_catalog_browser_ui2_similar(&browser, &catalog, source, indexes);
    assert(similar_count >= 4);
    for (unsigned i = 0; i < similar_count; ++i) {
        unsigned cover = asset_index(&assets, catalog.titles[indexes[i]].product_id);
        assert(cover < assets.count);
        similar[i] = (X4GamePageSimilar){indexes[i], catalog.titles[indexes[i]].name,
            &assets.entries[cover].surface[X4_COVER_ROW]};
    }
#endif
    X4GamePageModel model = {
        .product_id = forza, .title = meta->name, .available = true, .entitled = true,
        .details = &detail, .category = detail.category, .studio = detail.publisher,
        .description = detail.description, .background = &assets.backgrounds[assets.active].raster,
    };
    char last[X4_HISTORY_LABEL], played[X4_HISTORY_LABEL];
    x4_history_text(x4_history_find(&history, forza), fixture_now,
        x4_lang_current() == X4_LANG_EN, last, played);
    model.last_session = last;
    model.played_time = played;
    X4CoverRaster shots[X4_GAME_SCREENSHOT_MAX];
    for (unsigned i = 0; i < X4_GAME_SCREENSHOT_MAX; ++i) {
        shots[i] = screenshot47(fixtures, i);
        model.screenshots[i] = &shots[i];
    }
    X4GamePage page;
    x4_game_page_init(&page);
    assert(page.focus == X4_GAME_FOCUS_PLAY);
    /* The first original Forza raster is an actual store comparison.
     * Exercise the same ordinal-to-source mapping that main uses. */
    x4_game_page_draw(&page, &model, pixels);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_DOWN) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_DOWN) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) > 0);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_BACK) == X4_GAME_ACTION_NONE);
    for (unsigned i = 1; i < X4_GAME_SCREENSHOT_MAX; ++i)
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_RIGHT) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 0);
    x4_game_page_init(&page);
    game48("ficha-capturas", &page, &model, pixels, scratch);
    X4GamePageModel without_history = model;
    without_history.last_session = without_history.played_time = NULL;
    game48("ficha-sin-historial", &page, &without_history, pixels, scratch);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_PLAY);
#if X4_CAPTURE48_AFTER
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_NEXT_TAB) == X4_GAME_ACTION_NONE);
    assert(page.tab == X4_GAME_TAB_DETAILS);
    game48("ficha-detalles", &page, &model, pixels, scratch);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_NEXT_TAB) == X4_GAME_ACTION_NONE);
    assert(page.tab == X4_GAME_TAB_SIMILAR);
    page.focus = X4_GAME_FOCUS_SIMILAR;
    const X4ProductMetadata *related_meta = &assets.entries[asset_index(&assets, related_id)].metadata;
    publish_background(data, related_id);
    X4GamePageModel related = {
        .product_id = related_id, .title = related_meta->name, .available = true, .entitled = true,
        .category = related_meta->category, .studio = related_meta->publisher,
        .description = related_meta->description, .background = &assets.backgrounds[assets.active].raster,
        .similar = similar, .similar_count = similar_count
    };
    game48("ficha-similares", &page, &related, pixels, scratch);
    assert(x4_game_page_update(&page, &related, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_SIMILAR);
    x4_game_page_init(&page);
    page.focus = X4_GAME_FOCUS_FAVORITE;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_FAVORITE);
    model.favorite = true;
    game48("ficha-favorito", &page, &model, pixels, scratch);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_BACK) == X4_GAME_ACTION_BACK);
#endif
    x4_game_page_init(&page);
    model.entitled = false;
    model.free_in_store = true;
    model.product_id = fortnite;
    model.title = "Fortnite";
    model.details = NULL;
    model.category = "Shooter";
    model.studio = "Epic Games";
    model.description = NULL;
    model.background = NULL;
    model.last_session = model.played_time = NULL;
    page.focus = X4_GAME_FOCUS_STORE;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    assert(page.qr_open);
    game48("ficha-qr", &page, &model, pixels, scratch);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_BACK) == X4_GAME_ACTION_NONE && !page.qr_open);
    for (unsigned i = 0; i < X4_GAME_SCREENSHOT_MAX; ++i) free(shots[i].rgba);
    x4_game_page_cache_dispose();
}

int main(int argc, char **argv)
{
    assert(argc == 4 && (!strcmp(argv[3], "es") || !strcmp(argv[3], "en")));
    const bool en = !strcmp(argv[3], "en");
    x4_lang_set(en ? X4_LANG_EN : X4_LANG_ES, en ? "en-US" : "es-MX", en ? "US" : "MX");
    assert(!x4_font_init("assets/fonts/inter.x4font") && !x4_font_is_fallback());
    load_fixtures(argv[1]);
    snprintf(catalog.gamertag, sizeof(catalog.gamertag), "%s", sample_gamertag);
    uint32_t *pixels = guarded47(), *scratch = guarded47();
    fixture_now = x4_history_now();
    assert(fixture_now > 3 * 86400);
    assert(x4_history_record(&history, forza, fixture_now - 86400, 14400, NULL));
    assert(x4_history_record(&history, catalog.titles[2].product_id, fixture_now - 3 * 86400, 840, NULL));
    x4_ui2_home_history(&history);
    assert(x4_ui2_status_set(&status47, en ? X4_UI2_NETWORK_WIFI : X4_UI2_NETWORK_CABLE,
        en ? "1080p High" : "1080p Alta", 1122, "Jugador"));
    home48(pixels);
    tabs48(argv[1], pixels);
    games48(argv[1], argv[2], pixels, scratch);
    x4_catalog_gui_keyboard_cache_invalidate();
    x4_catalog_gui_home_invalidate();
    x4_catalog_browser_ui2_library_close(&browser);
    assert(!assets.pins && !footer_depth);
    for (unsigned i = 0; i < assets.count; ++i)
        for (unsigned j = 0; j < X4_COVER_SURFACES; ++j) free(assets.entries[i].surface[j].rgba);
    for (unsigned i = 0; i < 2; ++i) free(assets.backgrounds[i].raster.rgba);
    check_canvas(pixels);
    check_canvas(scratch);
    free(pixels - GUARDED_WORDS);
    free(scratch - GUARDED_WORDS);
    x4_font_close();
    assert(audited_text > 250);
    fprintf(stderr, "A3_TEXT_PASS audited=%u\n", audited_text);
    fputs("HOST48_RENDER_PASS\n", stderr);
    return 0;
}
