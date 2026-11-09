/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_CAPTURE47_MAIN inherited_capture47
#include "capture_tarea47.c"
#undef X4_CAPTURE47_MAIN
#include "ui_draw_metrics.h"

enum { WARMUP = 20, SAMPLES = 120, FRAMES = WARMUP + SAMPLES };

typedef struct {
    uint64_t first_us, warm_us[SAMPLES];
    unsigned partial, full;
    size_t max_regions;
    bool first_full;
} DrawSamples;

static void measure(DrawSamples *samples, enum X4UiScreen screen, unsigned frame,
    uint64_t start, uint64_t end, size_t damage)
{
    assert(damage > 0);
    if (frame < WARMUP) return;
    const uint64_t elapsed = end - start;
    samples->warm_us[frame - WARMUP] = elapsed;
    if (damage == SIZE_MAX) ++samples->full;
    else {
        ++samples->partial;
        if (damage > samples->max_regions) samples->max_regions = damage;
    }
    x4_ui_draw_metrics_record(screen, true, true, elapsed, 0, 0, 0, end, 0);
}

static int compare_times(const void *lhs, const void *rhs)
{
    const uint64_t a = *(const uint64_t *)lhs, b = *(const uint64_t *)rhs;
    return (a > b) - (a < b);
}

static void report(DrawSamples *samples, enum X4UiScreen screen, const char *name)
{
    X4UiDrawMetricView data;
    assert(x4_ui_draw_metrics_snapshot(screen, true, &data));
    assert(data.draw.n == SAMPLES && !data.draw.p50_overflow && !data.draw.p95_overflow);
    assert(samples->partial + samples->full == SAMPLES);
    qsort(samples->warm_us, SAMPLES, sizeof(samples->warm_us[0]), compare_times);
    assert(data.draw.max_us == samples->warm_us[SAMPLES - 1]);
    printf("BENCH48B %s %u %llu %u %llu %llu %llu %llu %llu %llu %u %u %zu\n", name, (unsigned)SAMPLES,
        (unsigned long long)samples->first_us, (unsigned)samples->first_full,
        (unsigned long long)samples->warm_us[(SAMPLES * 50 + 99) / 100 - 1],
        (unsigned long long)samples->warm_us[(SAMPLES * 95 + 99) / 100 - 1],
        (unsigned long long)samples->warm_us[(SAMPLES * 99 + 99) / 100 - 1],
        (unsigned long long)data.draw.max_us,
        (unsigned long long)data.draw.p50_us, (unsigned long long)data.draw.p95_us,
        samples->partial, samples->full, samples->max_regions);
}

static void browse(enum X4CatalogTab tab, bool search, uint32_t *pixels)
{
    set_tab(tab);
#if X4_BENCH_A2
    x4_catalog_browser_ui2_select(&browser, search ? X4_CAT_TAB_SEARCH : tab);
#if X4_BENCH_A3
    x4_catalog_browser_ui2_library_sync(&browser, &catalog, &recent, NULL);
    if (tab == X4_CAT_TAB_RECENTS) browser.library.focus = X4_UI2_LIBRARY_GRID;
#endif
#endif
    browser.hero_focused = false;
    browser.selected = browser.recent_selected = 0;
    x4_catalog_browser_query_clear(&browser);
    if (search) {
        assert(x4_catalog_browser_query_append(&browser, 'o'));
    }
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(!search || browser.filtered_count >= 2);
#if X4_BENCH_A2
    browser.ui2_content_focus = X4_UI2_CATALOG_GRID;
    browser.ui2_library_tabs = false;
#endif
    if (tab == X4_CAT_TAB_DISCOVER) {
        x4_catalog_browser_rebuild_featured(&browser, &catalog, &recent);
        (void)x4_catalog_browser_home_tick(&browser, &catalog, 100);
    } else {
        snprintf(browser.tab_background_id, sizeof(browser.tab_background_id), "%s", forza);
    }
    x4_catalog_gui_home_invalidate();
    DrawSamples samples = {0};
    const uint64_t first = clock_us();
    draw_tab(pixels);
    samples.first_us = clock_us() - first;
    samples.first_full = x4_catalog_gui_home_damage(NULL, NULL) == SIZE_MAX;
    x4_ui_draw_metrics_reset();
    const enum X4UiScreen screen = tab == X4_CAT_TAB_DISCOVER ? X4_UI_DISCOVER :
        tab == X4_CAT_TAB_RECENTS ? X4_UI_RECENTS : X4_UI_CATALOG;
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        /* The same two adjacent cards with the backdrop already ready. */
        assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent, frame & 1 ? -1 : 1, 0, 0));
        uint64_t start = clock_us();
        draw_tab(pixels);
        const uint64_t end = clock_us();
        measure(&samples, screen, frame, start, end, x4_catalog_gui_home_damage(NULL, NULL));
    }
    report(&samples, screen, search ? "buscar" : tab == X4_CAT_TAB_DISCOVER ? "descubrir" :
        tab == X4_CAT_TAB_RECENTS ? "mis-juegos" : "catalogo");
}

static void game(const char *fixtures, uint32_t *pixels)
{
    const X4ProductMetadata *meta = &assets.entries[asset_index(&assets, forza)].metadata;
    X4GamePageModel model = {
        .product_id = forza, .title = meta->name, .available = true, .entitled = true,
        .category = meta->category, .studio = meta->publisher, .description = meta->description,
        .background = &assets.backgrounds[assets.active].raster,
    };
    X4CoverRaster shots[2] = {screenshot47(fixtures, 0), screenshot47(fixtures, 1)};
    model.screenshots[0] = &shots[0];
    model.screenshots[1] = &shots[1];
    X4GameDetails detail = {.screenshot_count = 2};
    model.details = &detail;
    X4GamePage page;
    x4_game_page_init(&page);
    page.focus = X4_GAME_FOCUS_SCREENSHOTS;
    /* Both variants start on source 0. In A3 it is a table and therefore
     * no longer occupies the first visible position. */
    page.screenshot_index = 0;
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX];
    DrawSamples samples = {0};
    const uint64_t first = clock_us();
    const size_t first_damage = x4_game_page_draw_cached(&page, &model, 1, pixels, true, damage);
    samples.first_us = clock_us() - first;
    samples.first_full = first_damage == SIZE_MAX;
    x4_ui_draw_metrics_reset();
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        page.screenshot_index = (frame & 1u) ? 0 : 1;
        uint64_t start = clock_us();
        size_t changed = x4_game_page_draw_cached(&page, &model, 1, pixels, true, damage);
        const uint64_t end = clock_us();
        measure(&samples, X4_UI_GAME_PAGE, frame, start, end, changed);
        assert(changed > 0 && changed != SIZE_MAX);
    }
    report(&samples, X4_UI_GAME_PAGE, "ficha");
    x4_game_page_cache_dispose();
    free(shots[0].rgba);
    free(shots[1].rgba);
}

#ifndef X4_BENCH48_MAIN
#define X4_BENCH48_MAIN main
#endif
int X4_BENCH48_MAIN(int argc, char **argv)
{
    assert(argc == 4);
    bool en = !strcmp(argv[3], "en");
    x4_lang_set(en ? X4_LANG_EN : X4_LANG_ES, en ? "en-US" : "es-MX", en ? "US" : "MX");
    assert(!x4_font_init("assets/fonts/inter.x4font") && !x4_font_is_fallback());
    load_fixtures(argv[1]);
    uint32_t *pixels = guarded47();
    x4_catalog_gui_home_prewarm(pixels);
    publish_background(argv[1], forza);
    browse(X4_CAT_TAB_DISCOVER, false, pixels);
    browse(X4_CAT_TAB_CATALOG, false, pixels);
    browse(X4_CAT_TAB_RECENTS, false, pixels);
    browse(X4_CAT_TAB_CATALOG, true, pixels);
    game(argv[2], pixels);
    check_canvas(pixels);
    free(pixels - GUARDED_WORDS);
#if X4_BENCH_A3
    x4_catalog_browser_ui2_library_close(&browser);
#endif
    x4_font_close();
    return 0;
}
