/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/catalog_presentation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static X4CatalogSnapshot catalog;
static X4CatalogBrowser browser;
static X4CatalogPresentation presentation;
static X4Favorites favorites;
static X4GameDetails detail;
static X4History local_history;

static void setup(void)
{
    X4ProductMetadata metadata[20] = {0};
    catalog.state = X4_CATALOG_READY;
    catalog.count = 20;
    for (unsigned i = 0; i < catalog.count; ++i) {
        X4CatalogTitle *title = &catalog.titles[i];
        snprintf(title->id, sizeof(title->id), "TITLE%02u", i);
        snprintf(title->product_id, sizeof(title->product_id), "PRODUCT%02u", i);
        snprintf(title->name, sizeof(title->name), "Game %02u", i);
        title->entitled = !(i % 2);
        memcpy(metadata[i].product_id, title->product_id, sizeof(title->product_id));
        memcpy(metadata[i].name, title->name, sizeof(title->name));
        const char *genre = i < 4 ? "Sports" : i < 7 ? "Racing & flying" : i < 19 ? "Shooter" : "";
        snprintf(metadata[i].category, sizeof(metadata[i].category), "%s", genre);
    }
    x4_catalog_presentation_init(&presentation);
    x4_catalog_presentation_set_generation(&presentation, 1);
    assert(x4_catalog_presentation_ingest(&presentation, 1, 1, metadata, 20) == 20);
    x4_catalog_browser_init(&browser);
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_DISCOVER);
    assert(x4_catalog_presentation_apply_genres(&presentation, &browser, &catalog));
    assert(!x4_catalog_presentation_apply_genres(&presentation, &browser, &catalog));
    x4_catalog_browser_rebuild(&browser, &catalog);
}

static unsigned collection(X4Ui2CollectionKind kind)
{
    for (unsigned i = 0; i < browser.ui2_collection_count; ++i)
        if (browser.ui2_collections[i].kind == kind) return browser.ui2_collections[i].count;
    return 0;
}

static void collections_and_similar(void)
{
    assert(collection(X4_UI2_COLLECTION_SPORTS) == 4);
    assert(collection(X4_UI2_COLLECTION_RACING) == 0);
    assert(collection(X4_UI2_COLLECTION_SHOOTER) == 12);

    strcpy(detail.category, "Sports");
    strcpy(detail.subcategory, "Open world");
    detail.attributes = X4_GAME_ATTR_ONLINE_MULTIPLAYER;
    for (unsigned i = 0; i < 4; ++i) {
        const char *id = catalog.titles[i].product_id;
        assert(x4_catalog_presentation_ingest_detail(&presentation, id, &detail));
        assert(!x4_catalog_presentation_ingest_detail(&presentation, id, &detail));
    }
    assert(x4_catalog_presentation_apply_genres(&presentation, &browser, &catalog));
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(collection(X4_UI2_COLLECTION_MULTIPLAYER) == 4);
    assert(collection(X4_UI2_COLLECTION_OPEN_WORLD) == 4);
    assert(collection(X4_UI2_COLLECTION_SPORTS) == 4);
    assert(browser.ui2_genre_count == 5 && !strcmp(browser.ui2_genres[4].label, "Open world"));

    unsigned similar[X4_UI2_SIMILAR_MAX];
    assert(x4_catalog_browser_ui2_similar(&browser, &catalog, 7, similar) == X4_UI2_SIMILAR_MAX);
    for (unsigned i = 0; i < X4_UI2_SIMILAR_MAX; ++i) assert(similar[i] == 8 + i);
    assert(!x4_catalog_browser_ui2_similar(&browser, &catalog, 19, similar));

    X4HomeRows rows = {.count=1, .rows={{.kind=X4_HOME_CONTINUE, .count=2, .indices={0,1}}}};
    x4_catalog_browser_rebuild_featured(&browser, &catalog, NULL);
    x4_catalog_browser_home_apply(&browser, &catalog, &rows);
    assert(browser.featured_count == 4 && browser.featured_index == 0);
    assert(!x4_catalog_browser_ui2_left_edge(&browser));
    assert(!x4_catalog_browser_ui2_carousel_tick(&browser, &catalog, 1, false));
    assert(!x4_catalog_browser_ui2_carousel_tick(&browser, &catalog, 10000000, false));
    assert(x4_catalog_browser_ui2_carousel_tick(&browser, &catalog, 10000001, false));
    assert(browser.featured_index == 1);
    assert(!x4_catalog_browser_ui2_carousel_tick(&browser, &catalog, 19000000, true));
    assert(!x4_catalog_browser_ui2_carousel_tick(&browser, &catalog, 25000000, false));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, -1, 0, 0));
    assert(x4_catalog_browser_ui2_hero(&browser, &catalog) == &catalog.titles[0]);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(browser.hero_focused && browser.ui2_hero_favorite);
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_FAVORITE);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(!browser.hero_focused && !browser.ui2_collection_focus);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
    assert(browser.discover_row_selected == 1);
    rows.rows[0].indices[0] = 1;
    rows.rows[0].indices[1] = 0;
    x4_catalog_browser_home_apply(&browser, &catalog, &rows);
    assert(browser.discover_row_selected == 0);
    assert(x4_catalog_browser_selected_title(&browser, &catalog, NULL) == &catalog.titles[1]);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(browser.ui2_collection_focus && !browser.hero_focused);
    assert(browser.ui2_collections[0].source == 0);
    browser.ui2_collection_selected = browser.ui2_collection_count - 1;
    assert(!x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
    assert(browser.discover_row_selected == 0);
    browser.ui2_collection_selected = 0;
    unsigned source;
    assert(!x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.active_tab == X4_CAT_TAB_CATALOG && browser.ui2_collection_filter == X4_UI2_COLLECTION_MULTIPLAYER);
    assert(browser.filtered_count == 4 && browser.ui2_collection_filter_count == 4);
    assert(x4_catalog_browser_ui2_cycle_chip(&browser, &catalog, 1));
    assert(browser.ui2_collection_filter == X4_UI2_COLLECTION_NONE);
}

static void dropdowns_and_search(void)
{
    assert(browser.active_tab == X4_CAT_TAB_CATALOG);
    assert(browser.ui2_genre == 1 && browser.filtered_count == 4);
    assert(!strcmp(browser.ui2_genres[1].label, "Sports"));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, -1, 0));
    assert(browser.ui2_content_focus == X4_UI2_CATALOG_GENRES);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
    assert(browser.ui2_content_focus == X4_UI2_CATALOG_ACCESS);
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.ui2_selector_open);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(browser.filter == X4_CATALOG_FILTER_ALL && browser.filtered_count == 4);
    assert(x4_catalog_browser_ui2_close_selector(&browser));
    assert(!browser.ui2_selector_open && browser.filter == X4_CATALOG_FILTER_ALL);
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(!browser.ui2_selector_open && browser.filter == X4_CATALOG_FILTER_CONFIRMED_ACCESS &&
        browser.filtered_count == 2);
    unsigned source;
    assert(!x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source) && source == 0);
    assert(x4_catalog_browser_ui2_cycle_chip(&browser, &catalog, 1));
    assert(browser.ui2_content_focus == X4_UI2_CATALOG_GRID &&
        browser.filter == X4_CATALOG_FILTER_UNCONFIRMED_ACCESS && browser.filtered_count == 2);
    assert(x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source) && source == 1);
    assert(x4_catalog_browser_query_append(&browser, '0'));
    x4_catalog_browser_rebuild(&browser, &catalog);

    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_SEARCH);
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(!browser.query[0] && browser.filtered_count == catalog.count);
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_KEYBOARD);
    assert(x4_catalog_browser_query_append(&browser, '1'));
    x4_catalog_browser_rebuild(&browser, &catalog);
    browser.editing = false;
    assert(browser.filtered_count == 11);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source) && source == 1);

    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_CATALOG);
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(!strcmp(browser.query, "0") && browser.filtered_count == 2);
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_SEARCH);
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(!strcmp(browser.query, "1") && browser.filtered_count == 11);
}

static void sorting(void)
{
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_CATALOG);
    assert(x4_catalog_browser_query_clear(&browser));
    browser.ui2_genre_slot = 0;
    browser.filter = X4_CATALOG_FILTER_ALL;
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(x4_catalog_browser_ui2_selector_count(&browser, X4_UI2_CATALOG_SORT) == 2);

    strcpy(catalog.titles[0].name, "Zulu");
    strcpy(catalog.titles[1].name, "Águila");
    const X4HomeRows rows = {.count=3, .rows={
        {.kind=X4_HOME_MINE, .count=1, .indices={2}},
        {.kind=X4_HOME_NEW, .count=2, .indices={5,3}},
        {.kind=X4_HOME_POPULAR, .count=1, .indices={7}}}};
    x4_catalog_browser_home_apply(&browser, &catalog, &rows);
    assert(browser.home_rows.count == 2 && browser.home_rows.rows[0].kind == X4_HOME_POPULAR &&
        browser.home_rows.rows[1].kind == X4_HOME_MINE);
    assert(x4_catalog_browser_ui2_selector_count(&browser, X4_UI2_CATALOG_SORT) == 3);
    browser.ui2_content_focus = X4_UI2_CATALOG_SORT;
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.ui2_sort == X4_UI2_SORT_AZ);
    assert(browser.indices[0] == 1 && browser.indices[catalog.count - 1] == 0);
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.ui2_sort == X4_UI2_SORT_NEW && browser.indices[0] == 5 && browser.indices[1] == 3 &&
        browser.indices[2] == 0);
    x4_catalog_browser_home_apply(&browser, &catalog, &(X4HomeRows){0});
    assert(browser.ui2_sort == X4_UI2_SORT_FEATURED && browser.indices[0] == 0);
}

static void library_focus(void)
{
    favorites.count = 2;
    strcpy(favorites.items[0].product_id, catalog.titles[0].product_id);
    strcpy(favorites.items[0].name, catalog.titles[0].name);
    strcpy(favorites.items[1].product_id, "UNAVAILABLE");
    strcpy(favorites.items[1].name, "Saved game");
    x4_catalog_browser_ui2_favorites(&browser, &favorites);
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_RECENTS);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, -1, 0));
    assert(browser.ui2_library_tabs && !browser.ui2_favorites);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
    assert(browser.ui2_favorites && !x4_catalog_browser_ui2_favorite(&browser));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    unsigned source;
    assert(x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source) && source == 0);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
    assert(!x4_catalog_browser_resolved_index(&browser, &catalog, NULL, &source));
    const X4Favorite *selected = x4_catalog_browser_ui2_favorite(&browser);
    assert(selected && !strcmp(selected->name, "Saved game"));
    favorites.items[0] = favorites.items[1];
    favorites.count = 1;
    x4_catalog_browser_ui2_favorites(&browser, &favorites);
    assert(browser.ui2_favorite_selected == 0 && x4_catalog_browser_ui2_left_edge(&browser));
    assert(x4_catalog_browser_ui2_favorite(&browser) == &favorites.items[0]);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, -1, 0));
    assert(!x4_catalog_browser_ui2_favorite(&browser));
}

static void five_column_catalog(void)
{
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_CATALOG);
    if (browser.query[0]) assert(x4_catalog_browser_query_clear(&browser));
    /* Reset the ordinal and its remembered ID through the criteria path. */
    assert(x4_catalog_browser_query_append(&browser, 'G'));
    assert(x4_catalog_browser_query_clear(&browser));
    browser.ui2_genre_slot = 0;
    browser.filter = X4_CATALOG_FILTER_ALL;
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(browser.filtered_count == 20 && X4_CATALOG_UI2_PAGE_SIZE == 10);
    browser.ui2_content_focus = X4_UI2_CATALOG_GRID;
    unsigned source;
    assert(x4_catalog_browser_selected_index(&browser, &catalog, &source));
    assert(browser.selected == 0 && source == browser.indices[0]);
    for (unsigned col = 1; col < X4_CATALOG_UI2_COLUMNS; ++col) {
        assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
        assert(browser.selected == col);
    }
    assert(browser.selected == 4 && !x4_catalog_browser_ui2_left_edge(&browser));
    assert(!x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 1, 0, 0));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, 1, 0));
    assert(browser.selected == 9);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, NULL, 0, -1, 0));
    assert(browser.selected == 4);

    char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP];
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX];
    const size_t requested = x4_catalog_presentation_demand_surfaces(
        &presentation, &browser, &catalog, NULL, ids, masks);
    assert(requested == 20);
    assert(!strcmp(ids[0], catalog.titles[browser.indices[4]].product_id));
    assert(!strcmp(ids[9], catalog.titles[browser.indices[9]].product_id));
    assert(!strcmp(ids[10], catalog.titles[browser.indices[10]].product_id));
    for (size_t i = 0; i < requested; ++i) assert(masks[i] == (1u << X4_COVER_ROW));
}

static void library_a3(void)
{
    static X4RecentGames recents;
    const char ids[][96] = {"TITLE00", "TITLE01"};
    x4_recent_begin(&recents, 8, 1);
    assert(x4_recent_publish(&recents, 8, 1, ids, 2, 0, 200, &catalog));
    assert(x4_history_record(&local_history, catalog.titles[2].product_id, 200000, 7200, NULL));
    assert(x4_history_record(&local_history, catalog.titles[0].product_id, 100000, 3600, NULL));
    memset(&favorites, 0, sizeof(favorites));
    assert(x4_favorites_toggle(&favorites, catalog.titles[0].product_id, catalog.titles[0].name, NULL) ==
        X4_FAVORITES_ADDED);
    assert(x4_favorites_toggle(&favorites, "UNAVAILABLE", "Saved game", NULL) == X4_FAVORITES_ADDED);
    x4_catalog_browser_ui2_favorites(&browser, &favorites);
    x4_catalog_browser_ui2_library_sync(&browser, &catalog, &recents, &local_history);
    assert(browser.library.members == 12 && browser.library.favorite_count == 2 &&
        browser.library.recent_count == 2 && browser.library.grid_count == 12);
    X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_selection(&browser);
    assert(card.source == 0 && card.favorite && card.history && card.history->played_s == 3600);
    assert(x4_catalog_browser_ui2_library_manage(&browser));
    assert(x4_catalog_browser_ui2_library_selection(&browser).source == 0);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 0, 1, 0));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_REMOVE_FAVORITE);
    assert(!browser.library.manage && x4_catalog_browser_ui2_library_selection(&browser).source == 0);
    browser.library.focus = X4_UI2_LIBRARY_SORT;
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 0, 1, 0));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 0, 1, 0));
    assert(x4_catalog_browser_ui2_activate(&browser, &catalog) == X4_UI2_CATALOG_ACT_CONSUMED);
    assert(browser.library.sort == X4_UI2_LIBRARY_PLAYED);
    assert(x4_catalog_browser_ui2_library_card(&browser, false, 0).source == 2);
    browser.library.focus = X4_UI2_LIBRARY_GRID;
    browser.library.grid_selected = 0;
    for (unsigned col = 1; col < X4_CATALOG_UI2_COLUMNS; ++col) {
        assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 1, 0, 0));
        assert(browser.library.grid_selected == col);
    }
    assert(!x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 1, 0, 0));
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 0, 1, 0));
    assert(browser.library.grid_selected == 9);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 0, 1, 0));
    assert(browser.library.grid_selected == 11); /* incomplete last row */
    browser.library.focus = X4_UI2_LIBRARY_TABS;
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 1, 0, 0));
    assert(browser.library.tab == X4_UI2_LIBRARY_FAVORITES && browser.library.grid_count == 2);
    assert(x4_catalog_browser_apple_nav(&browser, &catalog, &recents, 1, 0, 0));
    assert(browser.library.tab == X4_UI2_LIBRARY_RECENT && browser.library.grid_count == 2);
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_CATALOG);
    browser.filter = X4_CATALOG_FILTER_FAVORITES;
    browser.ui2_genre_slot = 0;
    browser.ui2_collection_filter = X4_UI2_COLLECTION_NONE;
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(browser.filtered_count == 1 && browser.indices[0] == 0);
    unsigned genre = 0;
    assert(x4_catalog_browser_ui2_genre_option(&browser, 1, &genre) == X4_UI2_COLLECTION_MULTIPLAYER);
    assert(browser.ui2_multiplayer_count == 1);
    x4_catalog_browser_ui2_library_close(&browser);
}

int main(void)
{
    setup();
    collections_and_similar();
    dropdowns_and_search();
    sorting();
    five_column_catalog();
    library_focus();
    library_a3();
    puts("PASS T48: colecciones, carrusel, desplegables, orden, búsqueda, similares y cuadrículas de cinco columnas");
    return 0;
}
