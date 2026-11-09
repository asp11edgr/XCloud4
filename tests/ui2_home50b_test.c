/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_browser.h"
#include "canvas.h"
#include "ui2_home.h"
#include "ui2_theme.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static X4CatalogSnapshot catalog;
static X4HomeSigls sigls;

int x4_catalog_assets_acquire(X4CatalogAssets *assets, const char *id, X4CatalogAssetView *view)
{
    (void)assets;
    (void)id;
    (void)view;
    assert(!"the renderer should not look for network assets in this fixture");
    return 0;
}

void x4_catalog_assets_release(X4CatalogAssets *assets, X4CatalogAssetView *view)
{
    (void)assets;
    (void)view;
    assert(!"the fixture has no assets to release");
}

static void init_catalog(void)
{
    catalog.state = X4_CATALOG_READY;
    catalog.count = catalog.total = 3;
    catalog.titles[0] = (X4CatalogTitle){
        .id = "fixture-fortnite", .product_id = "BT5P2X999VH2", .name = "Fortnite",
        .f2p_only = 1, .is_free_in_store = 1,
    };
    catalog.titles[1] = (X4CatalogTitle){
        .id = "fixture-racer", .product_id = "TEST12345678", .name = "Fixture Racer", .entitled = 1,
    };
    catalog.titles[2] = (X4CatalogTitle){
        .id = "fixture-platform", .product_id = "TEST87654321", .name = "Fixture Platform", .entitled = 1,
    };
}

static void render(const X4Ui2HomeModel *model, bool warm, uint32_t *pixels)
{
    X4UiRect damage[2];
    if (!warm) x4_rect(pixels, 0, 0, X4_UI_WIDTH, X4_UI_HEIGHT, X4_UI2_BACKGROUND);
    assert(x4_ui2_home_draw(model, &catalog, NULL, 0, X4_UI2_HOME_CONTINUE,
        warm, pixels, damage) == SIZE_MAX);
}

static void check_late_home(enum X4Lang language, uint32_t *pixels, uint32_t *previous, size_t count)
{
    x4_lang_set_ui(language);
    X4CatalogBrowser browser;
    x4_catalog_browser_init(&browser);
    browser.ui2_enabled = true;
    catalog.gamertag[0] = '\0';
    memset(sigls, 0, sizeof(sigls));
    for (unsigned i = 0; i < X4_SIGL_COUNT; ++i) sigls[i].pending = true;

    X4Ui2HomeModel model;
    x4_catalog_browser_home_rebuild(&browser, &catalog, NULL, sigls);
    x4_ui2_home_model(&model, true, &catalog, NULL, &browser.home_rows);
    assert(model.popular_count == 1 && model.popular[0] == 0);
    const uint64_t pending_rows = browser.home_revision;
    render(&model, false, pixels);
    memcpy(previous, pixels, count * sizeof(*pixels));

    /* The worker publishes a Popular list while focus and inputs stay idle. */
    X4SiglList *list = &sigls[X4_SIGL_POPULAR];
    list->pending = false;
    list->valid = true;
    list->count = 3;
    for (unsigned i = 0; i < list->count; ++i)
        snprintf(list->ids[i], sizeof(list->ids[i]), "%s", catalog.titles[2 - i].product_id);
    x4_catalog_browser_home_rebuild(&browser, &catalog, NULL, sigls);
    assert(browser.home_revision == pending_rows + 1);
    x4_ui2_home_model(&model, true, &catalog, NULL, &browser.home_rows);
    assert(model.popular_count == 3 && model.popular[0] == 0 &&
        model.popular[1] == 2 && model.popular[2] == 1);
    render(&model, true, pixels);
    assert(memcmp(previous, pixels, count * sizeof(*pixels)));
    memcpy(previous, pixels, count * sizeof(*pixels));

    /* The name then arrives without changing the list or the focus. */
    memcpy(catalog.gamertag, "Jugador", sizeof("Jugador"));
    const uint64_t ready_rows = browser.home_revision;
    x4_catalog_browser_home_rebuild(&browser, &catalog, NULL, sigls);
    assert(browser.home_revision == ready_rows);
    x4_ui2_home_model(&model, true, &catalog, NULL, &browser.home_rows);
    render(&model, true, pixels);
    assert(memcmp(previous, pixels, count * sizeof(*pixels)));

    /* Check the final warm frame against a fresh Home with both publications. */
    memcpy(previous, pixels, count * sizeof(*pixels));
    x4_ui2_home_close();
    x4_catalog_browser_init(&browser);
    browser.ui2_enabled = true;
    x4_catalog_browser_home_rebuild(&browser, &catalog, NULL, sigls);
    x4_ui2_home_model(&model, true, &catalog, NULL, &browser.home_rows);
    render(&model, false, pixels);
    assert(!memcmp(previous, pixels, count * sizeof(*pixels)));
    assert(fwrite(previous, sizeof(*previous), count, stdout) == count);
    assert(fwrite(pixels, sizeof(*pixels), count, stdout) == count);
    x4_ui2_home_close();
}

int main(void)
{
    const size_t count = (size_t)X4_UI_WIDTH * X4_UI_HEIGHT;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    uint32_t *previous = malloc(count * sizeof(*previous));
    assert(pixels && previous);
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    init_catalog();
    for (enum X4Lang language = X4_LANG_ES; language < X4_LANG_COUNT; ++language)
        check_late_home(language, pixels, previous, count);
    x4_font_close();
    free(previous);
    free(pixels);
    return fflush(stdout) ? 1 : 0;
}
