/* SPDX-License-Identifier: GPL-3.0-only */
#define main capture47_inherited_fixture_main
#include "capture_tarea39_tabs.c"
#undef main

#include "game_page.h"
#include "loading_screen.h"
#include <math.h>

#if !X4_CAPTURE47_BEFORE
#include "ui2_boot.h"
#include "ui2_home.h"
#include "ui2_shell.h"
#include "canvas.h"
#endif

static const char *const sample_gamertag = "Jugador";
static const char *const home_scenes[] = {
    "inicio-recientes", "inicio-vacio", "inicio-cargando", "inicio-error", "inicio-sin-sesion"
};
static const char *const tab_scenes[] = {"descubrir", "catalogo", "mis-juegos"};
static const enum X4CatalogTab tab_types[] = {
    X4_CAT_TAB_DISCOVER, X4_CAT_TAB_CATALOG, X4_CAT_TAB_RECENTS
};

#ifdef X4_UI46_CAPTURE
static const char *active_scene;
#define CAPTURE_SCENE(name) (active_scene = (name))
#else
#define CAPTURE_SCENE(name) ((void)(name))
#endif

static uint32_t *guarded47(void)
{
    uint32_t *raw = malloc((pixel_count + 2 * GUARDED_WORDS) * sizeof(*raw));
    assert(raw);
    for (size_t i = 0; i < pixel_count + 2 * GUARDED_WORDS; ++i) raw[i] = sentinel;
    return raw + GUARDED_WORDS;
}

#if !X4_CAPTURE47_BEFORE || defined(X4_UI46_CAPTURE)
static double linear47(unsigned byte)
{
    double value = byte / 255.0;
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

static double luminance47(uint32_t rgba)
{
    return 0.2126 * linear47(rgba & 255u) + 0.7152 * linear47((rgba >> 8) & 255u) +
        0.0722 * linear47((rgba >> 16) & 255u);
}

static void contrast47(const char *scene, const char *role, double foreground, double background, double min)
{
    double contrast = (fmax(foreground, background) + 0.05) / (fmin(foreground, background) + 0.05);
    fprintf(stderr, "CAPTURE47 CONTRAST %s %s %.5f %.2f\n", scene, role, contrast, min);
#if !X4_CAPTURE47_BEFORE
    assert(contrast >= min);
#endif
}

#endif

#ifdef X4_UI46_CAPTURE
static int compare_double47(const void *a, const void *b)
{
    double first = *(const double *)a, second = *(const double *)b;
    return (first > second) - (first < second);
}

void x4_ui46_capture_contrast(const uint32_t *pixels, const char *role,
    int x, int y, int w, int h, uint32_t foreground)
{
    if (!active_scene || w <= 0 || h <= 0) return;
    assert(x >= X4_UI_SAFE_LEFT && x + w <= X4_UI_SAFE_RIGHT && y >= 0 && y + h <= X4_UI_SAFE_BOTTOM);
    size_t count = (size_t)w * h;
    double *samples = malloc(count * sizeof(*samples));
    assert(samples);
    size_t cursor = 0;
    for (int row = y; row < y + h; ++row)
        for (int col = x; col < x + w; ++col)
            samples[cursor++] = luminance47(pixels[(size_t)row * WIDTH + col]);
    qsort(samples, count, sizeof(*samples), compare_double47);
    contrast47(active_scene, role, luminance47(foreground),
        samples[(count * 95 + 99) / 100 - 1],
        strstr(role, "title") && !strstr(role, "subtitle") ? 3.0 : 4.5);
    free(samples);
}

void x4_ui46_capture_footer(const uint32_t *pixels, int x, int y, int w, int h, bool after)
{
    (void)pixels;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)after;
}
#endif

#if !X4_CAPTURE47_BEFORE
static X4Ui2Shell shell;
static X4Ui2Status status47;
static const char *const sidebar_scenes[X4_UI2_SECTION_COUNT] = {
    "barra-inicio", "barra-descubrir", "barra-catalogo", "barra-mis-juegos",
    "barra-buscar", "barra-ajustes", "barra-apoyar", "barra-acerca"
};

static void safe47(const char *scene, X4UiRect rect)
{
    assert(rect.x >= X4_UI_SAFE_LEFT && rect.y >= 0 && rect.w > 0 && rect.h > 0 &&
        rect.x + rect.w <= X4_UI_SAFE_RIGHT && rect.y + rect.h <= X4_UI_SAFE_BOTTOM);
    fprintf(stderr, "CAPTURE47 SAFE %s %d %d %d %d\n",
        scene, rect.x, rect.y, rect.w, rect.h);
}

static void cover47(const char *scene, X4UiRect rect, const X4CoverRaster *source)
{
    assert(source && source->rgba && source->width * 9 == source->height * 16);
    assert(rect.w * 9 == rect.h * 16);
    safe47(scene, rect);
    fprintf(stderr, "CAPTURE47 COVER %s %d %d %d %d %u %u\n",
        scene, rect.x, rect.y, rect.w, rect.h, source->width, source->height);
}

static void nav47(void)
{
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    X4Ui2NavResult nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_LEFT, false);
    assert(!nav.consumed && !shell.focused);
    nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_LEFT, true);
    assert(nav.consumed && nav.changed && shell.focused && shell.selected == X4_UI2_HOME);
    for (unsigned i = 0; i < X4_UI2_SECTION_COUNT; ++i) {
        assert(shell.selected == (X4Ui2Section)i);
        nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_SELECT, true);
        assert(nav.consumed && nav.activate && nav.target == (X4Ui2Section)i);
        nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_DOWN, true);
        assert(nav.changed);
    }
    nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_UP, true);
    assert(nav.changed && shell.selected == X4_UI2_ABOUT);
    nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_CIRCLE, true);
    assert(!nav.consumed);
    nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_RIGHT, false);
    assert(nav.changed && !shell.focused);
    const X4Ui2Section loop[] = {X4_UI2_HOME, X4_UI2_DISCOVER, X4_UI2_CATALOG, X4_UI2_LIBRARY};
    for (unsigned i = 0; i < sizeof(loop) / sizeof(loop[0]); ++i) {
        unsigned n = (unsigned)(sizeof(loop) / sizeof(loop[0]));
        x4_ui2_shell_set_section(&shell, loop[i]);
        nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_NEXT, false);
        assert(nav.activate && nav.target == loop[(i + 1) % n]);
        nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_PREVIOUS, false);
        assert(nav.activate && nav.target == loop[(i + n - 1) % n]);
    }
    nav = x4_ui2_shell_handle(&shell, X4_UI2_NAV_OPTIONS, false);
    assert(nav.consumed && nav.activate && nav.target == X4_UI2_SETTINGS);
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    fputs("CAPTURE47 PASS sidebar cruceta, L1/R1, circle delegation, OPTIONS\n", stderr);
}

static void boot47(uint32_t *pixels, uint32_t *scratch)
{
    const char *const names[] = {
        "arranque-fuentes", "arranque-cache", "arranque-sesion", "arranque-catalogo", "arranque-listo"
    };
    x4_ui2_boot_reset();
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        X4UiRect damage;
        assert(x4_ui2_boot_draw(pixels, (X4Ui2BootStep)i));
        size_t count = x4_ui2_boot_damage(&damage);
        assert(count == (i ? 1 : SIZE_MAX));
        if (count == 1) safe47(names[i], damage);
        safe47(names[i], (X4UiRect){435, 270, 1050, 470});
        assert(pixels[704u * WIDTH + 674] == pixels[704u * WIDTH + 675]);
        assert(pixels[704u * WIDTH + 1244] == pixels[704u * WIDTH + 1245]);
        capture(names[i], pixels);
        if (i == X4_UI2_BOOT_CATALOG) {
            memcpy(scratch, pixels, pixel_count * sizeof(*pixels));
            x4_catalog_gui_home_prewarm(pixels);
            assert(!memcmp(scratch, pixels, pixel_count * sizeof(*pixels)));
        }
        assert(!x4_ui2_boot_draw(pixels, (X4Ui2BootStep)i));
        assert(x4_ui2_boot_damage(NULL) == 0);
    }
    x4_ui2_boot_reset();
    fputs("CAPTURE47 PASS real boot step redraws only on change\n", stderr);
}

static void home_render47(const X4Ui2HomeModel *model, int selection,
    bool canvas_valid, uint32_t *pixels)
{
    if (!canvas_valid) x4_rect(pixels, 0, 0, WIDTH, HEIGHT, X4_UI2_BACKGROUND);
    X4UiRect damage[2];
    (void)x4_ui2_home_draw(model, &catalog, &assets, assets.revision,
        selection, canvas_valid, pixels, damage);
    x4_ui2_shell_draw_sidebar(pixels, &shell);
    x4_ui2_shell_draw_status(pixels, &status47, !canvas_valid);
}

static void home_geometry47(const char *scene, const X4Ui2HomeModel *model,
    const uint32_t *pixels)
{
    safe47(scene, x4_ui2_home_bounds());
    for (int item = 0; item < X4_UI2_HOME_ITEM_COUNT; ++item) {
        X4UiRect rect;
        if (!x4_ui2_home_art_bounds(model, item, &rect)) continue;
        X4Ui2HomeRoute route = x4_ui2_home_activate(model, item);
        const char *id = route.action == X4_UI2_HOME_GAME ? catalog.titles[route.source].product_id : fortnite;
        unsigned index = asset_index(&assets, id);
        assert(index < assets.count);
        const X4CoverRaster *source = &assets.entries[index].surface[X4_COVER_ROW];
        cover47(scene, rect, source);
    }
    uint32_t bg = pixels[(size_t)200 * WIDTH + 400];
    uint32_t card = pixels[(size_t)485 * WIDTH + 393];
    contrast47(scene, "titulo-inicio", luminance47(X4_UI2_TEXT), luminance47(bg), 3.0);
    contrast47(scene, "texto-tarjeta", luminance47(X4_UI2_MUTED), luminance47(card), 4.5);
    contrast47(scene, "boton-primario", luminance47(X4_UI2_INK), luminance47(X4_UI2_ACCENT), 4.5);
    contrast47(scene, "chip-gratis", luminance47(X4_UI2_ACCENT), luminance47(X4_UI2_ACTIVE), 4.5);
}

static void focus_cache47(const X4Ui2HomeModel *model, uint32_t *pixels, uint32_t *scratch)
{
    X4UiRect damage[2];
    home_render47(model, X4_UI2_HOME_CONTINUE, false, pixels);
    assert(x4_ui2_home_draw(model, &catalog, &assets, assets.revision,
        X4_UI2_HOME_CONTINUE, true, pixels, damage) == 0);
    int focus = x4_ui2_home_move(model, X4_UI2_HOME_CONTINUE, X4_UI2_HOME_DOWN);
    assert(focus == X4_UI2_HOME_RECENT_FIRST);
    assert(x4_ui2_home_draw(model, &catalog, &assets, assets.revision,
        focus, true, pixels, damage) == 2);
    assert(x4_ui2_home_draw(model, &catalog, &assets, assets.revision,
        focus, false, scratch, damage) == SIZE_MAX);
    X4UiRect bounds = x4_ui2_home_bounds();
    for (int y = bounds.y; y < bounds.y + bounds.h; ++y) {
        size_t at = (size_t)y * WIDTH + bounds.x;
        assert(!memcmp(pixels + at, scratch + at, (size_t)bounds.w * sizeof(*pixels)));
    }
    fputs("CAPTURE47 PASS Home retained canvas equals full composition; old/new focus only\n", stderr);
}

static void extra_shell47(uint32_t *pixels, uint32_t *scratch)
{
    X4Ui2HomeModel model;
    x4_ui2_home_model(&model, true, &catalog, &recent, &browser.home_rows);
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_LEFT, true).changed);
    for (unsigned i = 0; i < X4_UI2_SECTION_COUNT; ++i) {
        assert(shell.selected == (X4Ui2Section)i);
        home_render47(&model, -1, i != 0, pixels);
        safe47(sidebar_scenes[i], x4_ui2_status_rect);
        capture(sidebar_scenes[i], pixels);
        assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_DOWN, true).changed);
    }
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    const X4Ui2Network nets[] = {X4_UI2_NETWORK_CABLE, X4_UI2_NETWORK_WIFI, X4_UI2_NETWORK_UNKNOWN};
    const char *const names[] = {"estado-cable", "estado-wifi", "estado-sin-red"};
    const int minute = 18 * 60 + 42;
    assert(x4_ui2_status_update(&status47, X4_UI2_NETWORK_UNKNOWN, 720, 30, -1, NULL));
    for (unsigned i = 0; i < sizeof(nets) / sizeof(nets[0]); ++i) {
        assert(x4_ui2_status_update(&status47, nets[i], 1080, 60, minute, sample_gamertag));
        assert(!x4_ui2_status_update(&status47, nets[i], 1080, 60, minute, sample_gamertag));
        assert(status47.minute == minute && !strcmp(status47.initial, "J"));
        home_render47(&model, X4_UI2_HOME_CONTINUE, i != 0, pixels);
        safe47(names[i], x4_ui2_status_rect);
        capture(names[i], pixels);
    }
    assert(x4_ui2_status_update(&status47, X4_UI2_NETWORK_UNKNOWN, 1080, 60, minute + 1, sample_gamertag));
    assert(!x4_ui2_status_update(&status47, X4_UI2_NETWORK_UNKNOWN, 1080, 60, minute + 1, sample_gamertag));
    assert(x4_ui2_status_update(&status47, X4_UI2_NETWORK_WIFI, 1080, 60, minute, sample_gamertag));
    const X4UiRect status_rect = x4_ui2_status_rect;
    const uint32_t artwork = X4_RGB(122, 170, 204);
    x4_rect(pixels, status_rect.x, status_rect.y, status_rect.w, status_rect.h, artwork);
    x4_ui2_shell_draw_status(pixels, &status47, true);
    assert(pixels[(size_t)status_rect.y * WIDTH + status_rect.x] == artwork);
    assert(x4_ui2_status_update(&status47, X4_UI2_NETWORK_WIFI, 1080, 60, minute + 1, sample_gamertag));
    x4_ui2_shell_draw_status(pixels, &status47, false);
    x4_rect(scratch, status_rect.x, status_rect.y, status_rect.w, status_rect.h, artwork);
    x4_ui2_shell_draw_status(scratch, &status47, true);
    for (int y = status_rect.y; y < status_rect.y + status_rect.h; ++y) {
        size_t at = (size_t)y * WIDTH + status_rect.x;
        assert(!memcmp(pixels + at, scratch + at, (size_t)status_rect.w * sizeof(*pixels)));
    }
    fputs("CAPTURE47 PASS network cable/wifi/fallback, minute-only update and art outside pills\n", stderr);
}
#endif

static void home47(const char *data, uint32_t *pixels, uint32_t *scratch)
{
    X4RecentGames sample = recent;
#if X4_CAPTURE47_BEFORE
    (void)scratch;
    X4CoverRaster featured = decode(data, forza, "852x480", X4_COVER_HOME_FEATURE);
    X4CoverRaster battle = decode(data, fortnite, "796x448", X4_COVER_HOME_FORTNITE);
    x4_catalog_assets_compose_card(&featured);
    x4_catalog_assets_compose_card(&battle);
#else
    (void)data;
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
#endif
    for (unsigned i = 0; i < sizeof(home_scenes) / sizeof(home_scenes[0]); ++i) {
        sample.state = i == 2 ? X4_RECENT_LOADING : i == 3 ? X4_RECENT_ERROR : X4_RECENT_READY;
        sample.count = i == 1 ? 0 : recent.count;
        bool signed_in = i != 4;
        CAPTURE_SCENE(home_scenes[i]);
#if X4_CAPTURE47_BEFORE
        X4HomeDrawContext context = {.signed_in = signed_in, .featured_hero = &featured,
            .fortnite_cover = &battle};
        if (signed_in) snprintf(context.gamertag, sizeof(context.gamertag), "%s", sample_gamertag);
        unsigned source;
        if (signed_in && x4_home_continue_index(&catalog, &sample, &source)) {
            unsigned a = asset_index(&assets, catalog.titles[source].product_id);
            assert(a < assets.count);
            context.continue_name = catalog.titles[source].name;
            context.continue_cover = &assets.entries[a].surface[X4_COVER_ROW];
        }
        X4Screen screen = {.page = 0, .selected = 2, .signed_in = signed_in};
        X4Controller controller = {0};
        x4_screen_set_home_context(&context);
        x4_screen_home_invalidate();
        x4_screen_draw(&screen, &controller, pixels);
#else
        X4Ui2HomeModel model;
        x4_ui2_home_model(&model, signed_in, &catalog, &sample, &browser.home_rows);
        const enum X4Ui2HomeMode modes[] = {
            X4_UI2_HOME_WITH_RECENT, X4_UI2_HOME_EMPTY, X4_UI2_HOME_LOADING,
            X4_UI2_HOME_ERROR, X4_UI2_HOME_SIGNED_OUT
        };
        assert(model.mode == modes[i]);
        X4Ui2HomeRoute route = x4_ui2_home_activate(&model, X4_UI2_HOME_CONTINUE);
        assert(route.action == (i == 0 ? X4_UI2_HOME_GAME : i == 4 ? X4_UI2_HOME_LOGIN : X4_UI2_HOME_RECENTS));
        route = x4_ui2_home_activate(&model, X4_UI2_HOME_POPULAR_FIRST);
        assert(route.action == (signed_in ? X4_UI2_HOME_GAME : X4_UI2_HOME_FORTNITE));
        if (signed_in) assert(!strcmp(catalog.titles[route.source].product_id, fortnite));
        assert(x4_ui2_home_activate(&model, X4_UI2_HOME_EXPLORE).action == X4_UI2_HOME_CATALOG);
        if (!i) {
            assert(model.recent_count == X4_UI2_HOME_RECENT_MAX);
            assert(x4_ui2_home_activate(&model, X4_UI2_HOME_RECENT_ALL).action == X4_UI2_HOME_RECENTS);
            focus_cache47(&model, pixels, scratch);
        }
        (void)x4_ui2_status_update(&status47, X4_UI2_NETWORK_WIFI, 1080, 60, 18 * 60 + 42,
            signed_in ? sample_gamertag : NULL);
        home_render47(&model, X4_UI2_HOME_CONTINUE, false, pixels);
        home_geometry47(home_scenes[i], &model, pixels);
#endif
        capture(home_scenes[i], pixels);
    }
#if X4_CAPTURE47_BEFORE
    x4_screen_set_home_context(NULL);
    x4_screen_home_invalidate();
    free(featured.rgba);
    free(battle.rgba);
#else
    fputs("CAPTURE47 PASS five Home states and routes; Fortnite first and usable without login\n", stderr);
#endif
    CAPTURE_SCENE(NULL);
}

#if !X4_CAPTURE47_BEFORE
static void prove_tab_cover47(const char *scene, const uint32_t *pixels, X4UiRect rect, const char *id)
{
    unsigned a = asset_index(&assets, id);
    assert(a < assets.count);
    const X4CoverRaster *source = &assets.entries[a].surface[X4_COVER_ROW];
    cover47(scene, rect, source);
    /* Interior witnesses distinguish full-frame scaling from a center crop. */
    for (int ry = 1; ry <= 4; ++ry) {
        for (int rx = 1; rx <= 4; ++rx) {
            int dx = (rect.w - 1) * rx / 5, dy = (rect.h - 1) * ry / 5;
            size_t src = (size_t)(dy * ((int)source->height - 1) / (rect.h - 1)) * source->width +
                (size_t)(dx * ((int)source->width - 1) / (rect.w - 1));
            size_t dst = (size_t)(rect.y + dy) * WIDTH + rect.x + dx;
            assert((pixels[dst] & UINT32_C(0xffffff)) == (source->rgba[src] & UINT32_C(0xffffff)));
        }
    }
}

static void tab_geometry47(unsigned tab, const uint32_t *pixels)
{
    for (unsigned row = 0; row < 2; ++row) {
        unsigned first, total, columns;
        if (!tab) {
            unsigned r = browser.home_first_row + row;
            if (r >= browser.home_rows.count) break;
            total = browser.home_rows.rows[r].count;
            first = x4_catalog_browser_home_row_start(&browser, r);
            columns = X4_CATALOG_DISCOVER_ROW_VISIBLE;
        } else {
            unsigned selected = tab == 1 ? browser.selected : browser.recent_selected;
            first = (selected / X4_CATALOG_TAB_PAGE_SIZE) * X4_CATALOG_TAB_PAGE_SIZE +
                row * X4_CATALOG_TAB_COLUMNS;
            total = tab == 1 ? browser.filtered_count : x4_recent_count(&recent);
            columns = X4_CATALOG_TAB_COLUMNS;
        }
        for (unsigned i = 0; i < columns && first + i < total; ++i) {
            unsigned index = first + i;
            const char *id;
            if (!tab) id = catalog.titles[browser.home_rows.rows[browser.home_first_row + row].indices[index]].product_id;
            else if (tab == 1) id = catalog.titles[browser.indices[index]].product_id;
            else id = x4_recent_get(&recent, index)->product_id;
            X4UiRect rect = {388 + (int)i * (tab ? 364 : 288), 493 + (int)row * 248,
                tab ? 336 : 256, tab ? 189 : 144};
            prove_tab_cover47(tab_scenes[tab], pixels, rect, id);
        }
    }
}
#endif

static void tabs47(const char *data, uint32_t *pixels, uint32_t *scratch)
{
#if X4_CAPTURE47_BEFORE
    (void)scratch;
#endif
    x4_catalog_gui_home_prewarm(pixels);
    for (unsigned i = 0; i < sizeof(tab_types) / sizeof(tab_types[0]); ++i) {
        set_tab(tab_types[i]);
        if (!i) {
            assert(x4_catalog_browser_home_tick(&browser, &catalog, 100));
            browser.hero_focused = true;
        } else {
            browser.hero_focused = false;
            snprintf(browser.tab_background_id, sizeof(browser.tab_background_id), "%s", forza);
        }
        publish_background(data, forza);
        x4_catalog_gui_home_invalidate();
        CAPTURE_SCENE(tab_scenes[i]);
#if !X4_CAPTURE47_BEFORE
        x4_catalog_gui_sidebar_focused(false);
        const X4Ui2Section sections[] = {X4_UI2_DISCOVER, X4_UI2_CATALOG, X4_UI2_LIBRARY};
        x4_ui2_shell_set_section(&shell, sections[i]);
#endif
        draw_tab(pixels);
#if !X4_CAPTURE47_BEFORE
        x4_ui2_shell_draw(pixels, &shell, &status47);
        tab_geometry47(i, pixels);
#endif
        capture(tab_scenes[i], pixels);
#if !X4_CAPTURE47_BEFORE
        if (i) {
            const int movement[][2] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
            x4_catalog_gui_home_test_force_full();
            draw_tab(pixels);
            for (unsigned n = 0; n < sizeof(movement) / sizeof(movement[0]); ++n) {
                assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recent,
                    movement[n][0], movement[n][1], 0));
                draw_tab(pixels);
                assert(x4_catalog_gui_home_damage(NULL, NULL) != SIZE_MAX);
                memcpy(scratch, pixels, pixel_count * sizeof(*pixels));
                x4_catalog_gui_home_test_force_full();
                draw_tab(pixels);
                assert(!memcmp(scratch, pixels, pixel_count * sizeof(*pixels)));
            }
        }
#endif
    }
    CAPTURE_SCENE(NULL);
    x4_catalog_gui_home_invalidate();
}

static void render_loading47(const char *name, uint32_t *pixels, uint64_t now,
    enum X4LoadingStage stage)
{
    X4LoadingView view;
    x4_loading_screen_snapshot(&view);
    assert(view.stage == stage && view.active && view.has_game_background);
    assert(x4_loading_screen_draw(pixels, now));
#if !X4_CAPTURE47_BEFORE
    safe47(name, (X4UiRect){X4_UI_SAFE_LEFT, 55, 220, 40});
    safe47(name, (X4UiRect){510, 230, 900, 680});
    safe47(name, (X4UiRect){870, 948, 180, 40});
#endif
    capture(name, pixels);
}

static void loading47(uint32_t *pixels, uint32_t *scratch)
{
#if X4_CAPTURE47_BEFORE
    (void)scratch;
#else
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    assert(x4_loading_screen_step(X4_LOADING_REQUEST) == X4_LOADING_STEP_SESSION);
    assert(x4_loading_screen_step(X4_LOADING_RESOURCES) == X4_LOADING_STEP_SERVER);
    assert(x4_loading_screen_step(X4_LOADING_AUTHORIZE) == X4_LOADING_STEP_SERVER);
    assert(x4_loading_screen_step(X4_LOADING_CONNECT) == X4_LOADING_STEP_OPTIMIZE);
    assert(x4_loading_screen_step(X4_LOADING_CONNECTED) == X4_LOADING_STEP_OPTIMIZE);
    assert(x4_loading_screen_step(X4_LOADING_FIRST_PICTURE) == X4_LOADING_STEP_READY);
    assert(x4_loading_screen_step(X4_LOADING_COMPLETE) == X4_LOADING_STEPS);
    assert(x4_loading_screen_step(X4_LOADING_ERROR) == X4_LOADING_STEP_INVALID);
#endif
    unsigned a = asset_index(&assets, forza);
    assert(a < assets.count);
    /* The inherited fixture always supplies a complete 16:9 ROW raster. */
    const X4CoverRaster *art = &assets.entries[a].surface[X4_COVER_ROW];
    const uint64_t began = UINT64_C(1000000);
#if !X4_CAPTURE47_BEFORE
    uint32_t white[16 * 9];
    for (unsigned i = 0; i < 16 * 9; ++i) white[i] = X4_RGB(255, 255, 255);
    const X4CoverRaster flat = {.width = 16, .height = 9, .rgba = white};
    assert(x4_loading_screen_begin("Fondo plano", &flat, began));
    assert(x4_loading_screen_draw(pixels, began));
    const uint32_t dim = X4_RGB(61, 61, 61);
    assert(pixels[543u * WIDTH + 400] == dim && pixels[544u * WIDTH + 400] == dim);
    x4_loading_screen_end();
#endif
    assert(x4_loading_screen_begin("Forza Horizon 5", art, began));
    render_loading47("conexion-1", pixels, began, X4_LOADING_REQUEST);
    X4SessionSnapshot session = {.state = X4_SESSION_AUTHORIZING};
    x4_loading_screen_update(&session, began + 250000);
    render_loading47("conexion-2", pixels, began + 250000, X4_LOADING_AUTHORIZE);
    session.state = X4_SESSION_CONNECTING;
    x4_loading_screen_update(&session, began + 500000);
    render_loading47("conexion-3", pixels, began + 500000, X4_LOADING_CONNECT);
    session.state = X4_SESSION_STREAMING;
    session.rtc_connected = 1;
    x4_loading_screen_update(&session, began + 750000);
    x4_loading_screen_update(&session, began + 1250000);
    render_loading47("conexion-4", pixels, began + 1250000, X4_LOADING_FIRST_PICTURE);
    X4LoadingView view;
    x4_loading_screen_snapshot(&view);
    assert(view.full_buffer_initializations == 1 && view.percent < 100 && view.render_calls == 4);
    x4_loading_screen_first_picture(began + 1500000);
    assert(!x4_loading_screen_transition_ready(began + 1659999));
    assert(x4_loading_screen_transition_ready(began + 1660000));
    x4_loading_screen_end();

    assert(x4_loading_screen_begin("Forza Horizon 5", art, began));
    session = (X4SessionSnapshot){.state = X4_SESSION_WAITING, .waiting_for_resources = 1,
        .queue_elapsed_seconds = 92, .queue_eta_available = 1, .queue_eta_seconds = 48};
    x4_loading_screen_update(&session, began + 250000);
    x4_loading_screen_snapshot(&view);
    assert(view.waiting_for_resources && view.queue_elapsed_seconds == 92 &&
        view.queue_eta_available && view.queue_eta_seconds == 48);
    render_loading47("conexion-fila", pixels, began + 250000, X4_LOADING_RESOURCES);
    session = (X4SessionSnapshot){.state = X4_SESSION_ERROR, .error = -37, .http_status = 503,
        .title_name = "Forza Horizon 5", .stage = "No se pudo preparar la sesión"};
    x4_loading_screen_update(&session, began + 500000);
    x4_loading_screen_snapshot(&view);
    assert(view.stage == X4_LOADING_ERROR && !x4_loading_screen_draw(pixels, began + 500000));
#if X4_CAPTURE47_BEFORE
    x4_session_draw(&session, 0, 0, pixels);
#else
    x4_session_draw(&session, 0, 0, scratch);
    x4_ui2_shell_fit_legacy(pixels, scratch);
    x4_ui2_shell_draw(pixels, &shell, &status47);
#endif
    capture("conexion-error", pixels);
    fputs("CAPTURE47 LOADING ", stderr);
    x4_loading_screen_metrics_write(stderr);
    fputc('\n', stderr);
    x4_loading_screen_end();
    fputs("CAPTURE47 PASS four loading steps, queue ETA, error, uniform backdrop and 160 ms handoff\n", stderr);
}

static X4CoverRaster screenshot47(const char *fixtures, unsigned index)
{
    char file[24];
    snprintf(file, sizeof(file), "shot-%u.jpg", index);
    size_t length;
    unsigned char *bytes = read_file(fixtures, file, &length);
    X4CoverRaster raster = {0};
    assert(bytes && !x4_product_surface_decode(bytes, length, X4_COVER_DETAIL, &raster));
    free(bytes);
    return raster;
}

static void ficha47(const char *data, const char *fixtures, uint32_t *pixels, uint32_t *scratch)
{
    unsigned a = asset_index(&assets, forza);
    assert(a < assets.count);
    const X4ProductMetadata *meta = &assets.entries[a].metadata;
    publish_background(data, forza);
    X4GamePage page;
    x4_game_page_init(&page);
    X4GameDetails details = {.screenshot_count = X4_GAME_SCREENSHOT_MAX};
    X4CoverRaster shots[X4_GAME_SCREENSHOT_MAX];
    X4GamePageModel model = {
        .product_id = forza, .title = meta->name, .available = true, .entitled = true,
        .details = &details, .category = meta->category, .studio = meta->publisher,
        .description = meta->description, .background = &assets.backgrounds[assets.active].raster
    };
    for (unsigned i = 0; i < X4_GAME_SCREENSHOT_MAX; ++i) {
        shots[i] = screenshot47(fixtures, i);
        model.screenshots[i] = &shots[i];
    }
#if X4_CAPTURE47_BEFORE
    (void)scratch;
    x4_game_page_draw(&page, &model, pixels);
#else
    X4Ui2HomeModel home;
    x4_ui2_home_model(&home, true, &catalog, &recent, &browser.home_rows);
    int selection = X4_UI2_HOME_CONTINUE;
    X4Ui2HomeRoute route = x4_ui2_home_activate(&home, selection);
    assert(route.action == X4_UI2_HOME_GAME && !strcmp(catalog.titles[route.source].product_id, forza));
    x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
    x4_game_page_draw(&page, &model, scratch);
    x4_ui2_shell_fit_legacy(pixels, scratch);
    x4_ui2_shell_draw(pixels, &shell, &status47);
    safe47("ficha", (X4UiRect){380, 128, 1456, 819});
#endif
    capture("ficha", pixels);
    model.busy = true;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    model.busy = false;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_PLAY);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_BACK) == X4_GAME_ACTION_BACK);
#if !X4_CAPTURE47_BEFORE
    assert(x4_ui2_home_normalize(&home, selection) == selection);
    assert(x4_ui2_home_activate(&home, selection).source == route.source);
    fputs("CAPTURE47 PASS Home opens ficha; only ficha Play launches; Back keeps origin slot\n", stderr);
#endif
    for (unsigned i = 0; i < X4_GAME_SCREENSHOT_MAX; ++i) free(shots[i].rgba);
    x4_game_page_cache_dispose();
}

#ifndef X4_CAPTURE47_MAIN
#define X4_CAPTURE47_MAIN main
#endif
int X4_CAPTURE47_MAIN(int argc, char **argv)
{
    assert(argc == 4 && (!strcmp(argv[3], "es") || !strcmp(argv[3], "en")));
    bool en = !strcmp(argv[3], "en");
    x4_lang_set(en ? X4_LANG_EN : X4_LANG_ES, en ? "en-US" : "es-MX", en ? "US" : "MX");
    assert(!x4_font_init("assets/fonts/inter.x4font") && !x4_font_is_fallback());
    load_fixtures(argv[1]);
    snprintf(catalog.gamertag, sizeof(catalog.gamertag), "%s", sample_gamertag);
    uint32_t *pixels = guarded47();
    uint32_t *scratch = guarded47();
#if !X4_CAPTURE47_BEFORE
    nav47();
    assert(x4_ui2_status_update(&status47, X4_UI2_NETWORK_UNKNOWN, 0, 0, -1, NULL));
    boot47(pixels, scratch);
#endif
    home47(argv[1], pixels, scratch);
#if !X4_CAPTURE47_BEFORE
    extra_shell47(pixels, scratch);
#endif
    tabs47(argv[1], pixels, scratch);
    loading47(pixels, scratch);
    ficha47(argv[1], argv[2], pixels, scratch);
#if !X4_CAPTURE47_BEFORE
    x4_ui2_home_close();
#endif
    x4_catalog_gui_home_invalidate();
    assert(!assets.pins);
    for (unsigned i = 0; i < assets.count; ++i)
        for (unsigned j = 0; j < X4_COVER_SURFACES; ++j) free(assets.entries[i].surface[j].rgba);
    for (unsigned i = 0; i < 2; ++i) free(assets.backgrounds[i].raster.rgba);
    check_canvas(pixels);
    check_canvas(scratch);
    free(pixels - GUARDED_WORDS);
    free(scratch - GUARDED_WORDS);
    x4_font_close();
    assert(!fflush(stdout));
    return 0;
}
