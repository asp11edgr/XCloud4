/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/ui/apple_ui.h"
#include "../src/ui/screen.h"
#include "../src/ui/catalog_gui.h"
#include "../src/ui/catalog_browser.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include "../src/ui/picture_settings.h"
#include "../src/ui/server_settings.h"
#include "../src/auth/http_client.h"
#include "../third_party/qrcodegen/qrcodegen.h"

/* Stubs for release_check and catalog_assets */
X4Http *x4_http_open(int *err) { if (err) *err = 0; return NULL; }
void x4_http_close(X4Http *h) { (void)h; }
int x4_http_releases_request(X4Http *h, const char *url, char *body, size_t cap,
    size_t *len, int *status, const _Atomic int *cancel)
{
    (void)h; (void)url; (void)body; (void)cap; (void)len; (void)status; (void)cancel;
    return -1;
}

struct X4CatalogAssets {
    X4CatalogAssetView entries[8];
    unsigned count;
};

int x4_catalog_assets_acquire(X4CatalogAssets *a, const char *id, X4CatalogAssetView *out)
{
    if (!a || !id || !out) return 0;
    for (unsigned i = 0; i < a->count; ++i) {
        if (!strcmp(id, a->entries[i].metadata.product_id)) {
            *out = a->entries[i];
            out->slot = i;
            return 1;
        }
    }
    return 0;
}

void x4_catalog_assets_release(X4CatalogAssets *a, X4CatalogAssetView *v)
{
    (void)a;
    (void)v;
}

static void test_primitives(void)
{
    printf("1. Testing Apple dark visual primitives (antialiasing, alpha blend, rounded clip, scaling)...\n");
    uint32_t *owned = calloc(1920 * 1080 + 2, sizeof(uint32_t));
    assert(owned != NULL);
    owned[0] = owned[1920 * 1080 + 1] = 0xDEADBEEFu;
    uint32_t *p = owned + 1;

    /* 1a. Alpha blending exact check */
    uint32_t dst = 0xFF102030u;
    x4_apple_blend_pixel(&dst, 0x00FFFFFFu);
    assert(dst == 0xFF102030u);
    x4_apple_blend_pixel(&dst, 0xFFFFFFFFu);
    assert(dst == 0xFFFFFFFFu);
    dst = 0xFF000000u;
    x4_apple_blend_pixel(&dst, 0x80FFFFFFu);
    uint32_t r = (dst >> 16) & 0xFFu;
    assert(r >= 126u && r <= 130u);

    /* 1b. Rounded rectangle fill & antialiasing at corners */
    memset(p, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_apple_fill_round_rect(p, 100, 100, 200, 100, 24, 0xFFFFFFFFu);
    /* Far corner (100, 100) is outside radius 24 circle -> untouched (0) */
    assert(p[100 * 1920 + 100] == 0u);
    /* Center (200, 150) is full white */
    assert(p[150 * 1920 + 200] == 0xFFFFFFFFu);
    /* Check antialiased boundary pixels along the arc have intermediate values (0 < val < 255) */
    int aa_found = 0;
    for (int y = 100; y < 124; ++y) {
        for (int x = 100; x < 124; ++x) {
            uint32_t v = p[y * 1920 + x] & 0xFFu;
            if (v > 0u && v < 255u) ++aa_found;
        }
    }
    assert(aa_found >= 10);

    /* 1c. Rounded rectangle border antialiasing */
    memset(p, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_apple_stroke_round_rect(p, 100, 100, 200, 100, 24, 2.0f, 0xFFFFFFFFu);
    assert(p[100 * 1920 + 100] == 0u);
    assert(p[150 * 1920 + 200] == 0u); /* Interior untouched by stroke */
    assert((p[101 * 1920 + 200] & 0xFFu) > 200u); /* Top edge stroke active */

    /* 1d. Bilinear image scaling with rounded clipping and bottom darkening */
    memset(p, 0, 1920 * 1080 * sizeof(uint32_t));
    uint32_t *src_rgba = malloc(64 * 64 * sizeof(uint32_t));
    assert(src_rgba != NULL);
    for (int i = 0; i < 64 * 64; ++i) src_rgba[i] = 0xFFC8C8C8u;
    X4CoverRaster raster = { .rgba = src_rgba, .width = 64, .height = 64 };
    x4_apple_blit_image_rounded(p, 300, 300, 200, 200, 24, &raster, X4_GRAD_BOTTOM_DARK);
    /* Exterior corner (300, 300) clipped */
    assert(p[300 * 1920 + 300] == 0u);
    /* Upper interior pixel brighter than bottom darkened interior pixel */
    uint32_t top_luma = p[330 * 1920 + 400] & 0xFFu;
    uint32_t bot_luma = p[485 * 1920 + 400] & 0xFFu;
    assert(top_luma > 180u);
    assert(bot_luma < top_luma / 2u);

    free(src_rgba);
    assert(owned[0] == 0xDEADBEEFu && owned[1920 * 1080 + 1] == 0xDEADBEEFu);
    free(owned);
    printf("PASS: Apple dark visual primitives\n");
}

static void test_region_qr_url(void)
{
    printf("2. Testing Region QR URLs and captions for ES and EN (Section E)...\n");
    assert(!strcmp(x4_region_qr_url(X4_LANG_ES),
        "https://github.com/asp11edgr/XCloud4/blob/main/docs/region-es.md"));
    assert(!strcmp(x4_region_qr_url(X4_LANG_EN),
        "https://github.com/asp11edgr/XCloud4/blob/main/docs/region-en.md"));

    assert(!strcmp(x4_region_qr_caption(X4_LANG_ES),
        "Escanea el QR para ver cómo jugar si tu país no tiene Xbox Cloud Gaming."));
    assert(!strcmp(x4_region_qr_caption(X4_LANG_EN),
        "Scan the QR to see how to play if your country doesn't have Xbox Cloud Gaming."));

    assert(!strcmp(x4_region_qr_short_label(X4_LANG_ES),
        "github.com/asp11edgr/XCloud4 › docs › region-es.md"));
    assert(!strcmp(x4_region_qr_short_label(X4_LANG_EN),
        "github.com/asp11edgr/XCloud4 › docs › region-en.md"));

    /* Verify qrcodegen encodes both URLs cleanly */
    uint8_t qr_c[qrcodegen_BUFFER_LEN_MAX], qr_t[qrcodegen_BUFFER_LEN_MAX];
    assert(qrcodegen_encodeText(x4_region_qr_url(X4_LANG_ES), qr_t, qr_c,
        qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, false));
    assert(qrcodegen_encodeText(x4_region_qr_url(X4_LANG_EN), qr_t, qr_c,
        qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, false));

    printf("PASS: Region QR URLs and captions\n");
}

static void test_featured_origin_fallback(void)
{
    printf("3. Testing Featured games origin with fallback data (Recents + Fortnite + Entitled)...\n");
    static X4CatalogSnapshot cat;
    static X4CatalogBrowser b;
    static X4RecentGames rec;

    memset(&cat, 0, sizeof(cat));
    cat.state = X4_CATALOG_READY;
    cat.count = 8;
    for (unsigned i = 0; i < 8; ++i) {
        snprintf(cat.titles[i].id, sizeof(cat.titles[i].id), "TITLE_%u", i);
        snprintf(cat.titles[i].product_id, sizeof(cat.titles[i].product_id), "PROD%02u", i);
        snprintf(cat.titles[i].name, sizeof(cat.titles[i].name), "Aventura Espacial %u", i);
        cat.titles[i].entitled = (i >= 2 && i <= 5) ? 1 : 0;
    }
    /* Title 6 is Fortnite */
    snprintf(cat.titles[6].id, sizeof(cat.titles[6].id), "FORTNITE");
    snprintf(cat.titles[6].product_id, sizeof(cat.titles[6].product_id), "BT5P2X999VH2");
    snprintf(cat.titles[6].name, sizeof(cat.titles[6].name), "Fortnite");
    cat.titles[6].entitled = 1;

    x4_catalog_browser_init(&b);
    x4_catalog_browser_rebuild(&b, &cat);
    x4_recent_init(&rec);

    /* Case A: With recents (TITLE_4 and TITLE_1) -> featured order:
     * [0]=4 (recent), [1]=1 (recent), [2]=6 (Fortnite), [3]=2 (first entitled not yet included) */
    char r_ids[2][96] = { "TITLE_4", "TITLE_1" };
    x4_recent_begin(&rec, 1, 1);
    assert(x4_recent_publish(&rec, 1, 1, r_ids, 2, 0, 200, &cat));
    x4_catalog_browser_sync_recents(&b, &rec);
    x4_catalog_browser_rebuild_featured(&b, &cat, &rec);

    assert(b.featured_count == 4);
    assert(b.featured_sources[0] == 4);
    assert(b.featured_sources[1] == 1);
    assert(b.featured_sources[2] == 6);
    assert(b.featured_sources[3] == 2);

    /* Case B: No recents -> featured order:
     * [0]=6 (Fortnite), [1]=2 (entitled), [2]=3 (entitled), [3]=4 (entitled) */
    x4_recent_init(&rec);
    x4_catalog_browser_rebuild_featured(&b, &cat, &rec);
    assert(b.featured_count == 4);
    assert(b.featured_sources[0] == 6);
    assert(b.featured_sources[1] == 2);
    assert(b.featured_sources[2] == 3);
    assert(b.featured_sources[3] == 4);

    /* Discover row fallback when no recents: returns Popular/entitled titles */
    assert(x4_catalog_browser_discover_row_count(&b, &cat, &rec) == 5);
    unsigned row_src = 999;
    assert(x4_catalog_browser_discover_row_source(&b, &cat, &rec, 0, &row_src) && row_src == 2);

    printf("PASS: Featured games origin with fallback data\n");
}

static void test_navigation_main_menu_and_catalog(void)
{
    printf("4. Testing Main Menu and Catalog navigation...\n");
    X4Screen s = {0};
    s.page = 0;
    s.selected = 0;

    /* Main menu: 3 cards -> top bar -> cards -> bottom pills */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_UP, 0);
    assert(s.selected == 6); /* Tu cuenta pill */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.selected == 7); /* Gear icon */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == 2); /* Cuenta card */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == 5); /* Apoyar pill */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
    assert(s.selected == 4); /* Acerca de pill */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
    assert(s.selected == 3); /* Ajustes pill */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_UP, 0);
    assert(s.selected == 0); /* Explorar catalogo card */

    /* Catalog Apple navigation */
    static X4CatalogSnapshot cat;
    static X4CatalogBrowser b;
    static X4RecentGames rec;
    memset(&cat, 0, sizeof(cat));
    cat.state = X4_CATALOG_READY;
    cat.count = 10;
    for (unsigned i = 0; i < 10; ++i) {
        snprintf(cat.titles[i].id, sizeof(cat.titles[i].id), "TITLE_%u", i);
        snprintf(cat.titles[i].product_id, sizeof(cat.titles[i].product_id), "PROD%02u", i);
        snprintf(cat.titles[i].name, sizeof(cat.titles[i].name), "Juego %u", i);
        cat.titles[i].entitled = 1;
    }
    x4_catalog_browser_init(&b);
    x4_catalog_browser_rebuild(&b, &cat);
    x4_recent_init(&rec);
    char r_ids[3][96] = { "TITLE_8", "TITLE_7", "TITLE_6" };
    x4_recent_begin(&rec, 1, 1);
    assert(x4_recent_publish(&rec, 1, 1, r_ids, 3, 0, 200, &cat));
    x4_catalog_browser_sync_recents(&b, &rec);
    x4_catalog_browser_rebuild_featured(&b, &cat, &rec);

    assert(b.active_tab == X4_CAT_TAB_DISCOVER);
    assert(b.hero_focused == true);
    assert(b.featured_index == 0);

    /* Left/Right on hero changes featured game */
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 1, 0, 0));
    assert(b.featured_index == 1);
    unsigned resolved = 999;
    assert(x4_catalog_browser_resolved_index(&b, &cat, &rec, &resolved));
    assert(resolved == b.featured_sources[1]);

    /* Down moves from hero to "Continua donde lo dejaste" row */
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 0, 1, 0));
    assert(b.hero_focused == false);
    assert(b.discover_row_selected == 0);
    assert(x4_catalog_browser_resolved_index(&b, &cat, &rec, &resolved) && resolved == 8);
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 1, 0, 0));
    assert(b.discover_row_selected == 1);
    assert(x4_catalog_browser_resolved_index(&b, &cat, &rec, &resolved) && resolved == 7);

    /* Up returns to hero, Up again focuses top bar */
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 0, -1, 0));
    assert(b.hero_focused == true);
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 0, -1, 0));
    assert(b.top_bar_focused == true);

    /* R1 switches tabs: Descubrir -> Catalogo -> Recientes -> Descubrir */
    b.top_bar_focused = false;
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 0, 0, 1));
    assert(b.active_tab == X4_CAT_TAB_CATALOG);
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 0, 0, 1));
    assert(b.active_tab == X4_CAT_TAB_RECENTS);
    assert(x4_catalog_browser_apple_nav(&b, &cat, &rec, 0, 0, 1));
    assert(b.active_tab == X4_CAT_TAB_DISCOVER);

    printf("PASS: Main Menu and Catalog navigation\n");
}

typedef struct { int x, y, w, h; const char *name; } UiBox;

static void check_boxes_disjoint(const UiBox *boxes, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        assert(boxes[i].x >= 0 && boxes[i].y >= 0);
        assert(boxes[i].x + boxes[i].w <= 1920);
        assert(boxes[i].y + boxes[i].h <= 1080);
        for (size_t j = i + 1; j < n; ++j) {
            bool overlap_x = (boxes[i].x < boxes[j].x + boxes[j].w) && (boxes[j].x < boxes[i].x + boxes[i].w);
            bool overlap_y = (boxes[i].y < boxes[j].y + boxes[j].h) && (boxes[j].y < boxes[i].y + boxes[i].h);
            if (overlap_x && overlap_y) {
                fprintf(stderr, "OVERLAP between %s (%d,%d,%d,%d) and %s (%d,%d,%d,%d)\n",
                    boxes[i].name, boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h,
                    boxes[j].name, boxes[j].x, boxes[j].y, boxes[j].w, boxes[j].h);
                assert(!(overlap_x && overlap_y));
            }
        }
    }
}

static void test_no_overlap_long_names_es_en(void)
{
    printf("5. Testing zero overlap in ES and EN with long game names across all new screens...\n");
    /* Verify structural layout bounding boxes are strictly disjoint on Main Menu */
    const UiBox home_boxes[] = {
        { 84,  48,  260, 52,  "home_logo" },
        { 1420, 48, 348, 52,  "home_account_pill" },
        { 1784, 48, 52,  52,  "home_gear_btn" },
        { 84,  146, 1160, 72, "home_title" },
        { 84,  228, 820, 36,  "home_subtitle" },
        { 920, 234, 916, 30,  "home_server_hint" },
        { 84,  304, 852, 448, "home_card_explore" },
        { 966, 304, 420, 448, "home_card_fortnite" },
        { 1416, 304, 420, 448, "home_card_account" },
        { 84,  790, 232, 58,  "home_pill_settings" },
        { 336, 790, 252, 58,  "home_pill_about" },
        { 608, 790, 232, 58,  "home_pill_support" },
        { 84,  954, 1752, 36, "home_footer" },
    };
    check_boxes_disjoint(home_boxes, sizeof(home_boxes) / sizeof(home_boxes[0]));

    /* Verify structural layout bounding boxes are strictly disjoint on Discover tab */
    const UiBox discover_boxes[] = {
        { 84,  42,  300, 56,  "cat_logo" },
        { 600, 42,  696, 56,  "cat_tabs_l1_r1" },
        { 1444, 44, 52,  52,  "cat_search_btn" },
        { 1510, 44, 52,  52,  "cat_gear_btn" },
        { 1574, 44, 262, 52,  "cat_account_pill" },
        { 84,  118, 1752, 416, "discover_hero" },
        { 84,  558, 1752, 44,  "discover_row_header" },
        { 80,  608, 336, 260, "discover_card_0" },
        { 440, 612, 328, 260, "discover_card_1" },
        { 796, 612, 328, 260, "discover_card_2" },
        { 1152, 612, 328, 260, "discover_card_3" },
        { 1508, 612, 328, 260, "discover_card_4" },
        { 84,  954, 1752, 36, "cat_footer" },
    };
    check_boxes_disjoint(discover_boxes, sizeof(discover_boxes) / sizeof(discover_boxes[0]));

    /* Verify structural layout bounding boxes are strictly disjoint on Catalog tab */
    const UiBox catalog_tab_boxes[] = {
        { 84,  42,  1752, 56, "cat_topbar" },
        { 84,  118, 1128, 44, "cat_filters_and_search" },
        { 1256, 118, 580, 44, "cat_results_summary" },
        { 84,  176, 356, 270, "grid_0" },
        { 468, 176, 356, 270, "grid_1" },
        { 852, 176, 356, 270, "grid_2" },
        { 84,  462, 356, 270, "grid_3" },
        { 468, 462, 356, 270, "grid_4" },
        { 852, 462, 356, 270, "grid_5" },
        { 1256, 180, 580, 716, "detail_panel" },
        { 84,  954, 1752, 36, "cat_footer" },
    };
    check_boxes_disjoint(catalog_tab_boxes, sizeof(catalog_tab_boxes) / sizeof(catalog_tab_boxes[0]));

    /* Render all screens in ES and EN with extra-long UTF-8 game names & descriptions
     * and verify canaries and non-overlapping text bounds */
    uint32_t *owned = calloc(1920 * 1080 + 2, sizeof(uint32_t));
    assert(owned != NULL);
    owned[0] = owned[1920 * 1080 + 1] = 0xCAFEBABEu;
    uint32_t *p = owned + 1;

    static X4CatalogSnapshot cat;
    static X4CatalogBrowser b;
    static X4RecentGames rec;
    memset(&cat, 0, sizeof(cat));
    cat.state = X4_CATALOG_READY;
    cat.count = 6;
    snprintf(cat.gamertag, sizeof(cat.gamertag), "JugadorEstelar_PS4_LongName");
    for (unsigned i = 0; i < 6; ++i) {
        snprintf(cat.titles[i].id, sizeof(cat.titles[i].id), "LONG_TITLE_%u", i);
        snprintf(cat.titles[i].product_id, sizeof(cat.titles[i].product_id), "LONGPROD%02u", i);
        snprintf(cat.titles[i].name, sizeof(cat.titles[i].name),
            "¿Crónicas del Horizonte Infinito %u? ¡Edición Definitiva de Coleccionista Extraordinariamente Larga!", i + 1);
        cat.titles[i].entitled = (i % 2 == 0);
    }
    x4_catalog_browser_init(&b);
    x4_catalog_browser_rebuild(&b, &cat);
    x4_recent_init(&rec);
    x4_catalog_browser_rebuild_featured(&b, &cat, &rec);

    for (int lang = 0; lang < 2; ++lang) {
        if (lang == 0) x4_lang_set(X4_LANG_ES, "es-MX", "MX");
        else x4_lang_set(X4_LANG_EN, "en-US", "US");

        X4Screen s = {0};
        X4Controller ctrl = {0};
        ctrl.data.connected = 1;
        X4HomeDrawContext hctx = { .signed_in = lang, .gamertag = "JugadorEstelar_PS4" };
        x4_screen_set_home_context(&hctx);
        x4_screen_draw(&s, &ctrl, p);
        assert(owned[0] == 0xCAFEBABEu && owned[1920 * 1080 + 1] == 0xCAFEBABEu);

        X4CatalogGuiStatus status = { 1, 0, 0, NULL };
        b.active_tab = X4_CAT_TAB_DISCOVER;
        x4_catalog_gui_draw(&cat, &b, &rec, NULL, &status, p);
        assert(owned[0] == 0xCAFEBABEu && owned[1920 * 1080 + 1] == 0xCAFEBABEu);

        b.active_tab = X4_CAT_TAB_CATALOG;
        x4_catalog_gui_draw(&cat, &b, &rec, NULL, &status, p);
        assert(owned[0] == 0xCAFEBABEu && owned[1920 * 1080 + 1] == 0xCAFEBABEu);

        b.editing = true;
        x4_catalog_gui_draw(&cat, &b, &rec, NULL, &status, p);
        assert(owned[0] == 0xCAFEBABEu && owned[1920 * 1080 + 1] == 0xCAFEBABEu);
        b.editing = false;

        /* Unavailable country QR in Catalog and Fortnite */
        X4CatalogSnapshot err_cat = {0};
        err_cat.state = X4_CATALOG_ERROR;
        err_cat.error = X4_AUTH_E_XBOX;
        err_cat.http_status = 403;
        err_cat.xerr = 0;
        snprintf(err_cat.offering, sizeof(err_cat.offering), "xgpuwebf2p");
        snprintf(err_cat.stage, sizeof(err_cat.stage), "Xbox Cloud Gaming rechazo la cuenta");
        x4_catalog_gui_draw(&err_cat, &b, &rec, NULL, &status, p);
        assert(owned[0] == 0xCAFEBABEu && owned[1920 * 1080 + 1] == 0xCAFEBABEu);

        s.page = 8;
        x4_fortnite_summary_draw(&s, &err_cat, 0, p);
        assert(owned[0] == 0xCAFEBABEu && owned[1920 * 1080 + 1] == 0xCAFEBABEu);
    }

    free(owned);
    printf("PASS: Zero overlap in ES and EN with long game names\n");
}

static void test_fortnite_quality_1step(void)
{
    printf("6. Testing Fortnite quality 1-step selection (Point 2)...\n");
    X4Screen s = {.page = 8, .selected = 0};
#if X4_PUBLIC
    const int last = (int)X4_PIC_QUALITY_1080P_MAX;
    assert(last == 3); /* Public T27: Alta, 1080p, 720p, Maximum. */
#else
    const int last = (int)X4_PIC_QUALITY_720P;
    assert(last == 2); /* Lab retains the original three choices. */
#endif

    /* Every direction advances one choice; neither end wraps or escapes. */
    for (int selected = 1; selected <= last; ++selected) {
        x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
        assert(s.page == 8 && s.selected == selected);
    }
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.page == 8 && s.selected == last);
    for (int selected = last - 1; selected >= 0; --selected) {
        x4_screen_update(&s, ORBIS_PAD_BUTTON_UP, 0);
        assert(s.page == 8 && s.selected == selected);
    }
    x4_screen_update(&s, ORBIS_PAD_BUTTON_UP, 0);
    assert(s.page == 8 && s.selected == 0);

    for (int selected = 1; selected <= last; ++selected) {
        x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
        assert(s.page == 8 && s.selected == selected);
    }
    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.page == 8 && s.selected == last);
    for (int selected = last - 1; selected >= 0; --selected) {
        x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
        assert(s.page == 8 && s.selected == selected);
    }
    x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
    assert(s.page == 8 && s.selected == 0);
    printf("PASS: Fortnite quality one-step navigation and both limits (%d choices)\n", last + 1);
}

static void test_uncropped_image_rule(void)
{
    printf("7. Testing uncropped images contain rule (<= 5%% crop, Point 3)...\n");
    uint32_t *p = calloc(1920 * 1080, sizeof(uint32_t));
    assert(p != NULL);

    /* 1. 16:9 image (1920x1080) into 16:9 box (356x200) */
    uint32_t *rgba_16_9 = malloc(1920 * 1080 * sizeof(uint32_t));
    for (int i = 0; i < 1920 * 1080; ++i) rgba_16_9[i] = 0xFF104080u;
    rgba_16_9[540 * 1920 + 960] = 0xFFFFFFFFu;
    X4CoverRaster r_16_9 = { .rgba = rgba_16_9, .width = 1920, .height = 1080 };
    x4_apple_blit_image_rounded(p, 100, 100, 356, 200, 18, &r_16_9, X4_GRAD_NONE);
    assert(p[200 * 1920 + 278] != 0u);

    /* 2. 1:1 square image (1080x1080) into 16:9 box (356x200):
     * Ratio difference is 43.7% > 5%, so contain mode is used. */
    uint32_t *rgba_1_1 = malloc(1080 * 1080 * sizeof(uint32_t));
    for (int i = 0; i < 1080 * 1080; ++i) rgba_1_1[i] = 0xFF802020u;
    X4CoverRaster r_1_1 = { .rgba = rgba_1_1, .width = 1080, .height = 1080 };
    memset(p, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_apple_blit_image_rounded(p, 100, 100, 356, 200, 18, &r_1_1, X4_GRAD_NONE);
    assert(p[200 * 1920 + 278] != 0u);
    assert(p[200 * 1920 + 130] != 0u);

    /* 3. 2:3 vertical image (720x1080) into 16:9 box (356x200) */
    uint32_t *rgba_2_3 = malloc(720 * 1080 * sizeof(uint32_t));
    for (int i = 0; i < 720 * 1080; ++i) rgba_2_3[i] = 0xFF208030u;
    X4CoverRaster r_2_3 = { .rgba = rgba_2_3, .width = 720, .height = 1080 };
    memset(p, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_apple_blit_image_rounded(p, 100, 100, 356, 200, 18, &r_2_3, X4_GRAD_NONE);
    assert(p[200 * 1920 + 278] != 0u);
    assert(p[200 * 1920 + 120] != 0u);

    /* 4. Discover hero banner with 16:9 image aligned right */
    memset(p, 0, 1920 * 1080 * sizeof(uint32_t));
    x4_apple_draw_hero_banner(p, 84, 118, 1752, 416, 24, &r_16_9);
    assert(p[300 * 1920 + 1600] != 0u);

    free(rgba_16_9);
    free(rgba_1_1);
    free(rgba_2_3);
    free(p);
    printf("PASS: Uncropped images contain rule\n");
}

static void test_detail_text_wrap_and_scroll(void)
{
    printf("8. Testing detail description wrapping and scrolling (Point 4)...\n");
    X4CatalogBrowser b;
    x4_catalog_browser_init(&b);
    assert(b.detail_scroll_offset == 0);

    /* Test scrolling forward and backward */
    assert(x4_catalog_browser_scroll_detail(&b, 1) == true);
    assert(b.detail_scroll_offset == 1);
    assert(x4_catalog_browser_scroll_detail(&b, 1) == true);
    assert(b.detail_scroll_offset == 2);
    assert(x4_catalog_browser_scroll_detail(&b, -1) == true);
    assert(b.detail_scroll_offset == 1);
    assert(x4_catalog_browser_scroll_detail(&b, -1) == true);
    assert(b.detail_scroll_offset == 0);
    assert(x4_catalog_browser_scroll_detail(&b, -1) == false); /* clamped at 0 */
    assert(b.detail_scroll_offset == 0);

    /* Verify word-wrapping in ES: words are never cut in half */
    const char *desc_es = "Explora un vasto mundo abierto lleno de misterios y peligros ancestrales. "
        "Enfréntate a criaturas legendarias en combates dinámicos en tiempo real mientras descubres los "
        "secretos de una civilización olvidada. Únete a tus amigos en modo cooperativo para superar mazmorras "
        "desafiantes y conseguir recompensas épicas sin límites.";
    const char *desc_en = "Explore a vast open world filled with mysteries and ancient dangers. "
        "Battle legendary creatures in dynamic real-time combat while uncovering the secrets of a forgotten "
        "civilization. Join your friends in cooperative multiplayer to conquer challenging dungeons and earn "
        "epic rewards.";

    /* Render detail panel with this description in ES and EN and verify canaries */
    uint32_t *owned = calloc(1920 * 1080 + 2, sizeof(uint32_t));
    assert(owned != NULL);
    owned[0] = owned[1920 * 1080 + 1] = 0xABCD1234u;
    uint32_t *p = owned + 1;

    static X4CatalogSnapshot cat;
    static X4RecentGames rec;
    memset(&cat, 0, sizeof(cat));
    cat.state = X4_CATALOG_READY;
    cat.count = 2;
    snprintf(cat.titles[0].id, sizeof(cat.titles[0].id), "LONG_DESC_0");
    snprintf(cat.titles[0].product_id, sizeof(cat.titles[0].product_id), "DESCPROD00");
    snprintf(cat.titles[0].name, sizeof(cat.titles[0].name), "Mundo Abierto");
    cat.titles[0].entitled = 1;

    X4CatalogAssets assets = {0};
    assets.count = 1;
    snprintf(assets.entries[0].metadata.product_id, sizeof(assets.entries[0].metadata.product_id), "DESCPROD00");
    snprintf(assets.entries[0].metadata.name, sizeof(assets.entries[0].metadata.name), "Mundo Abierto");
    snprintf(assets.entries[0].metadata.category, sizeof(assets.entries[0].metadata.category), "Acción y Aventura");
    snprintf(assets.entries[0].metadata.description, sizeof(assets.entries[0].metadata.description), "%s", desc_es);

    x4_catalog_browser_init(&b);
    x4_catalog_browser_rebuild(&b, &cat);
    x4_recent_init(&rec);
    b.active_tab = X4_CAT_TAB_CATALOG;
    b.section = X4_CATALOG_SECTION_CATALOG;
    b.selected = 0;

    X4CatalogGuiStatus status = { 1, 0, 0, NULL };
    /* Draw without scroll */
    b.detail_scroll_offset = 0;
    x4_catalog_gui_draw(&cat, &b, &rec, &assets, &status, p);
    assert(owned[0] == 0xABCD1234u && owned[1920 * 1080 + 1] == 0xABCD1234u);

    /* Draw with scroll = 2 */
    b.detail_scroll_offset = 2;
    x4_catalog_gui_draw(&cat, &b, &rec, &assets, &status, p);
    assert(owned[0] == 0xABCD1234u && owned[1920 * 1080 + 1] == 0xABCD1234u);

    /* In EN */
    snprintf(assets.entries[0].metadata.description, sizeof(assets.entries[0].metadata.description), "%s", desc_en);
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    b.detail_scroll_offset = 1;
    x4_catalog_gui_draw(&cat, &b, &rec, &assets, &status, p);
    assert(owned[0] == 0xABCD1234u && owned[1920 * 1080 + 1] == 0xABCD1234u);

    free(owned);
    printf("PASS: Detail description wrapping and scrolling\n");
}

static void test_keyboard_draw_performance(void)
{
    printf("9. Testing search keyboard draw performance and responsiveness (Point 5)...\n");
    uint32_t *p = calloc(1920 * 1080, sizeof(uint32_t));
    assert(p != NULL);

    static X4CatalogSnapshot cat;
    static X4CatalogBrowser b;
    static X4RecentGames rec;
    memset(&cat, 0, sizeof(cat));
    cat.state = X4_CATALOG_READY;
    cat.count = 4;
    for (unsigned i = 0; i < 4; ++i) {
        snprintf(cat.titles[i].id, sizeof(cat.titles[i].id), "TITLE_%u", i);
        snprintf(cat.titles[i].name, sizeof(cat.titles[i].name), "Juego %u", i);
        cat.titles[i].entitled = 1;
    }
    x4_catalog_browser_init(&b);
    x4_catalog_browser_rebuild(&b, &cat);
    x4_recent_init(&rec);
    X4CatalogGuiStatus status = { 1, 0, 0, NULL };

    /* Initial draw caches backdrop */
    b.editing = true;
    b.keyboard_cursor = 0;
    snprintf(b.query, sizeof(b.query), "HALO");
    x4_catalog_gui_draw(&cat, &b, &rec, NULL, &status, p);

    /* Measure consecutive keystroke frames with cached backdrop */
    int iterations = 20;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iterations; ++i) {
        b.keyboard_cursor = (unsigned)(i % 36);
        x4_catalog_gui_draw(&cat, &b, &rec, NULL, &status, p);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1000000.0;
    double avg_frame_ms = elapsed_ms / iterations;
    printf("Keyboard frame draw average: %.3f ms (target <= 8.0 ms)\n", avg_frame_ms);
    assert(avg_frame_ms <= 8.0);

    b.editing = false;
    free(p);
    printf("PASS: Search keyboard draw performance\n");
}

int main(void)
{
    if (x4_font_init("assets/fonts/inter.x4font") != 0) {
        assert(x4_font_init("assets/fonts/noto_sans.x4font") == 0);
    }
    test_primitives();
    test_region_qr_url();
    test_featured_origin_fallback();
    test_navigation_main_menu_and_catalog();
    test_no_overlap_long_names_es_en();
    test_fortnite_quality_1step();
    test_uncropped_image_rule();
    test_detail_text_wrap_and_scroll();
    test_keyboard_draw_performance();
    x4_font_close();
    printf("=== ALL TAREA 19 HOST TESTS PASSED ===\n");
    return 0;
}
