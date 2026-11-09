/* SPDX-License-Identifier: GPL-3.0-only */
#define main bench47_fixture_entry
#include "capture_tarea39_tabs.c"
#undef main

#include "canvas.h"
#include "loading_screen.h"
#include "ui_draw_metrics.h"
#if !X4_CAPTURE47_BEFORE
#include "ui2_boot.h"
#include "ui2_home.h"
#include "ui2_shell.h"
#endif

enum { BENCH_WARMUP = 20, BENCH_SAMPLES = 120, BENCH_FRAMES = BENCH_WARMUP + BENCH_SAMPLES };
static uint64_t samples[BENCH_SAMPLES];

static uint64_t record(enum X4UiScreen screen, bool nav, unsigned frame, uint64_t start)
{
    uint64_t end = clock_us();
    uint64_t elapsed = end - start;
    if (frame >= BENCH_WARMUP) {
        samples[frame - BENCH_WARMUP] = elapsed;
        x4_ui_draw_metrics_record(screen, nav, true, elapsed, 0, 0, 0, end, 0);
    }
    return elapsed;
}

static void report(enum X4UiScreen screen, bool nav, const char *name)
{
    X4UiDrawMetricView view;
    assert(x4_ui_draw_metrics_snapshot(screen, nav, &view));
    assert(view.draw.n == BENCH_SAMPLES && !view.draw.p50_overflow && !view.draw.p95_overflow);
    printf("BENCH47 %s %llu %llu %llu %llu\n", name,
        (unsigned long long)view.draw.n, (unsigned long long)view.draw.p50_us,
        (unsigned long long)view.draw.p95_us, (unsigned long long)view.draw.max_us);
    printf("SAMPLES47 %s", name);
    for (unsigned i = 0; i < BENCH_SAMPLES; ++i)
        printf(" %llu", (unsigned long long)samples[i]);
    putchar('\n');
}

#if !X4_CAPTURE47_BEFORE
static void draw_shell(uint32_t *pixels, X4Ui2Section section, bool account)
{
    X4Ui2Shell nav = {0};
    X4Ui2Status header = {0};
    x4_ui2_shell_set_section(&nav, section);
    assert(x4_ui2_status_update(&header, X4_UI2_NETWORK_CABLE, 1080, 60, 18 * 60 + 42,
        account ? "Jugador" : NULL));
    x4_ui2_shell_draw(pixels, &nav, &header);
}
#endif

static void bench_home(const char *data, uint32_t *pixels)
{
#if X4_CAPTURE47_BEFORE
    X4CoverRaster featured = decode(data, forza, "852x480", X4_COVER_HOME_FEATURE);
    X4CoverRaster battle = decode(data, fortnite, "796x448", X4_COVER_HOME_FORTNITE);
    x4_catalog_assets_compose_card(&featured);
    x4_catalog_assets_compose_card(&battle);
    const X4CoverRaster *resume = &assets.entries[asset_index(&assets, forza)].surface[X4_COVER_ROW];
    X4HomeDrawContext ctx = {.signed_in = 1, .gamertag = "Jugador", .featured_hero = &featured,
        .fortnite_cover = &battle, .continue_name = catalog.titles[0].name, .continue_cover = resume};
    X4Screen menu = {.page = 0, .selected = 2, .signed_in = 1};
    X4Controller controller = {0};
    x4_screen_set_home_context(&ctx);
    x4_screen_home_invalidate();
    x4_screen_draw(&menu, &controller, pixels);
#else
    (void)data;
    X4Ui2HomeModel model;
    X4UiRect damage[2];
    x4_rect(pixels, 0, 0, WIDTH, HEIGHT, X4_UI2_BACKGROUND);
    x4_ui2_home_model(&model, true, &catalog, &recent, &browser.home_rows);
    assert(model.mode == X4_UI2_HOME_WITH_RECENT);
    assert(x4_ui2_home_draw(&model, &catalog, &assets, assets.revision, X4_UI2_HOME_CONTINUE,
        false, pixels, damage) == SIZE_MAX);
    draw_shell(pixels, X4_UI2_HOME, true);
#endif
    x4_ui_draw_metrics_reset();
    for (unsigned i = 0; i < BENCH_FRAMES; ++i) {
#if X4_CAPTURE47_BEFORE
        /* Same actions: Explore / Continue, in the legacy menu's card order. */
        menu.selected = (i & 1u) ? 2 : 0;
        uint64_t start = clock_us();
        x4_screen_draw(&menu, &controller, pixels);
#else
        int selected = (i & 1u) ? X4_UI2_HOME_CONTINUE : X4_UI2_HOME_EXPLORE;
        uint64_t start = clock_us();
        x4_ui2_home_model(&model, true, &catalog, &recent, &browser.home_rows);
        size_t changed = x4_ui2_home_draw(&model, &catalog, &assets, assets.revision,
            selected, true, pixels, damage);
#endif
        record(X4_UI_MENU, true, i, start);
#if X4_CAPTURE47_BEFORE
        assert(x4_screen_home_damage(NULL) > 0);
#else
        assert(changed > 0 && changed <= 2);
#endif
    }
    report(X4_UI_MENU, true, "inicio");
#if X4_CAPTURE47_BEFORE
    x4_screen_set_home_context(NULL);
    x4_screen_home_invalidate();
    free(featured.rgba);
    free(battle.rgba);
#endif
}

static void bench_tab(const char *data, enum X4CatalogTab tab, uint32_t *pixels)
{
    const enum X4UiScreen metrics[] = {X4_UI_DISCOVER, X4_UI_CATALOG, X4_UI_RECENTS};
    const char *names[] = {"descubrir", "catalogo", "mis-juegos"};
    assert(tab <= X4_CAT_TAB_RECENTS);
    set_tab(tab);
    browser.selected = browser.recent_selected = 0;
    if (tab == X4_CAT_TAB_DISCOVER) {
        assert(x4_catalog_browser_home_tick(&browser, &catalog, 100));
        assert(!strcmp(browser.home_background_id, forza));
        browser.hero_focused = true;
    } else {
        browser.hero_focused = false;
        snprintf(browser.tab_background_id, sizeof(browser.tab_background_id), "%s", forza);
    }
    publish_background(data, forza);
    x4_catalog_gui_home_invalidate();
    draw_tab(pixels);
#if !X4_CAPTURE47_BEFORE
    const X4Ui2Section sections[] = {X4_UI2_DISCOVER, X4_UI2_CATALOG, X4_UI2_LIBRARY};
    draw_shell(pixels, sections[tab], true);
#endif
    x4_ui_draw_metrics_reset();
    for (unsigned i = 0; i < BENCH_FRAMES; ++i) {
        if (tab == X4_CAT_TAB_DISCOVER) browser.hero_focused = (i & 1u) != 0;
        else {
            /* Consecutive games in one row. The 300 ms background debounce
             * does not commit a different backdrop during alternating focus. */
            assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent,
                (i & 1u) ? -1 : 1, 0, 0));
            assert((tab == X4_CAT_TAB_CATALOG ? browser.selected : browser.recent_selected) == !(i & 1u));
        }
        uint64_t start = clock_us();
        draw_tab(pixels);
        record(metrics[tab], true, i, start);
        assert(x4_catalog_gui_home_damage(NULL, NULL) != 0);
    }
    report(metrics[tab], true, names[tab]);
}

static void bench_startup(uint32_t *pixels)
{
#if X4_CAPTURE47_BEFORE
    /* T46c has no splash. The comparable startup budget ends at the first
     * interactive signed-out Home frame, including its existing prewarm. */
    const enum X4UiScreen metric = X4_UI_OTHER;
    X4Screen menu = {.page = 0, .selected = 0};
    X4Controller controller = {0};
#else
    const enum X4UiScreen metric = X4_UI_BOOT;
    static const X4CatalogSnapshot empty;
    static const X4RecentGames no_recent;
#endif
    x4_ui_draw_metrics_reset();
    for (unsigned i = 0; i < BENCH_FRAMES; ++i) {
        x4_catalog_gui_home_invalidate();
#if X4_CAPTURE47_BEFORE
        x4_screen_home_invalidate();
        uint64_t start = clock_us();
        x4_screen_home_prepare(pixels);
        x4_catalog_gui_home_prewarm(pixels);
        x4_screen_draw(&menu, &controller, pixels);
#else
        x4_ui2_boot_reset();
        x4_ui2_home_invalidate();
        uint64_t start = clock_us();
        assert(x4_ui2_boot_draw(pixels, X4_UI2_BOOT_FONTS));
        assert(x4_ui2_boot_draw(pixels, X4_UI2_BOOT_CACHE));
        assert(x4_ui2_boot_draw(pixels, X4_UI2_BOOT_ACCOUNT));
        assert(x4_ui2_boot_draw(pixels, X4_UI2_BOOT_CATALOG));
        x4_catalog_gui_home_prewarm(pixels);
        assert(x4_ui2_boot_draw(pixels, X4_UI2_BOOT_READY));
        X4Ui2HomeModel model;
        X4UiRect damage[2];
        x4_rect(pixels, 0, 0, WIDTH, HEIGHT, X4_UI2_BACKGROUND);
        x4_ui2_home_model(&model, false, &empty, &no_recent, NULL);
        assert(x4_ui2_home_draw(&model, &empty, NULL, 0, X4_UI2_HOME_CONTINUE,
            false, pixels, damage) == SIZE_MAX);
        draw_shell(pixels, X4_UI2_HOME, false);
#endif
        uint64_t elapsed = record(metric, false, i, start);
        if (i == 0) printf("STARTUP47_COLD %llu\n", (unsigned long long)elapsed);
    }
    report(metric, false, "arranque");
}

static void bench_connection(uint32_t *pixels)
{
    const X4CoverRaster *art = &assets.entries[asset_index(&assets, forza)].surface[X4_COVER_ROW];
    const uint64_t began = 1000000u, tick = 34000u;
    assert(x4_loading_screen_begin("Forza Horizon 5", art, began));
    X4SessionSnapshot session = {0};
    x4_ui_draw_metrics_reset();
    for (unsigned i = 0; i < BENCH_FRAMES; ++i) {
        uint64_t now = began + tick * i;
        if (i >= 24) {
            if (i < 45) session = (X4SessionSnapshot){.state = X4_SESSION_WAITING,
                .waiting_for_resources = 1, .queue_elapsed_seconds = 92,
                .queue_eta_available = 1, .queue_eta_seconds = 48};
            else if (i < 65) session = (X4SessionSnapshot){.state = X4_SESSION_AUTHORIZING};
            else if (i < 85) session = (X4SessionSnapshot){.state = X4_SESSION_CONNECTING};
            else session = (X4SessionSnapshot){.state = X4_SESSION_STREAMING, .rtc_connected = 1};
            x4_loading_screen_update(&session, now);
        }
        uint64_t start = clock_us();
        assert(x4_loading_screen_draw(pixels, now));
        record(X4_UI_LOADING, true, i, start);
    }
    report(X4_UI_LOADING, true, "conexion");
    X4LoadingView view;
    x4_loading_screen_snapshot(&view);
    assert(view.full_buffer_initializations == 1 && view.region_copies == BENCH_FRAMES - 1 &&
        view.render_calls == BENCH_FRAMES && view.background_preparations == 1);
    fputs("LOADING47 ", stdout);
    x4_loading_screen_metrics_write(stdout);
    putchar('\n');
    x4_loading_screen_end();
}

int main(int argc, char **argv)
{
    assert(argc == 3 && (!strcmp(argv[2], "es") || !strcmp(argv[2], "en")));
    bool english = !strcmp(argv[2], "en");
    x4_lang_set(english ? X4_LANG_EN : X4_LANG_ES, english ? "en-US" : "es-MX", english ? "US" : "MX");
    assert(!x4_font_init("assets/fonts/inter.x4font") && !x4_font_is_fallback());
    load_fixtures(argv[1]);
    snprintf(catalog.gamertag, sizeof(catalog.gamertag), "%s", "Jugador");
    uint32_t *raw = malloc((pixel_count + 2 * GUARDED_WORDS) * sizeof(*raw));
    assert(raw);
    for (size_t i = 0; i < pixel_count + 2 * GUARDED_WORDS; ++i) raw[i] = sentinel;
    uint32_t *pixels = raw + GUARDED_WORDS;
    bench_startup(pixels);
    bench_home(argv[1], pixels);
    bench_tab(argv[1], X4_CAT_TAB_DISCOVER, pixels);
    bench_tab(argv[1], X4_CAT_TAB_CATALOG, pixels);
    bench_tab(argv[1], X4_CAT_TAB_RECENTS, pixels);
    bench_connection(pixels);
    x4_catalog_gui_home_invalidate();
#if !X4_CAPTURE47_BEFORE
    x4_ui2_home_close();
#endif
    assert(!assets.pins);
    for (unsigned i = 0; i < assets.count; ++i)
        for (unsigned j = 0; j < X4_COVER_SURFACES; ++j) free(assets.entries[i].surface[j].rgba);
    for (unsigned i = 0; i < 2; ++i) free(assets.backgrounds[i].raster.rgba);
    check_canvas(pixels);
    free(raw);
    x4_font_close();
    return 0;
}
