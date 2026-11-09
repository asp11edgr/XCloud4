/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/home_rows.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <stdio.h>

#define PC_ID "9NGP0DJVKSFL"
#define PARSE(out, json) x4_sigl_parse((out), (json), sizeof(json) - 1)

static X4CatalogSnapshot catalog;
static X4HomeSigls sigls;

static const X4HomeRow *row(const X4HomeRows *home, X4HomeRowKind kind)
{
    for (unsigned i = 0; i < home->count; ++i) {
        if (home->rows[i].kind == kind) return &home->rows[i];
    }
    return NULL;
}

static void expect(const X4HomeRows *home, X4HomeRowKind kind,
    const uint16_t *indices, unsigned count)
{
    const X4HomeRow *found = row(home, kind);
    assert(found && found->count == count);
    for (unsigned i = 0; i < count; ++i) assert(found->indices[i] == indices[i]);
}

static void assert_session_only(const X4HomeRows *home)
{
    for (unsigned i = 0; i < home->count; ++i) {
        const X4HomeRow *entry = &home->rows[i];
        for (unsigned j = 0; j < entry->count; ++j) {
            assert(entry->indices[j] < catalog.count);
            assert(strcmp(catalog.titles[entry->indices[j]].product_id, PC_ID));
        }
    }
}

static void test_parser(void)
{
    X4SiglList *list = &sigls[X4_SIGL_NEW];
    list->pending = true;
    assert(PARSE(list, "[{\"title\":\"Novedades é\"},{\"id\":\"9ID1\"},{\"id\":\"aId2\"}]"));
    assert(list->valid && !list->pending && list->count == 2);
    assert(!strcmp(list->ids[0], "9ID1") && !strcmp(list->ids[1], "aId2"));
    assert(!PARSE(list, "[{\"title\":\"Novedades\"},{\"id\":\"OK\"},"));
    assert(!list->valid && !list->pending && list->count == 0);
    assert(PARSE(list, "[]") && list->valid && !list->count);
    assert(PARSE(list, "[{\"title\":\"Vacía\"}]") && list->valid && !list->count);
    assert(!PARSE(list, "{}"));
    assert(!PARSE(list, "[{\"title\":42},{\"id\":\"OK\"}]"));
    assert(!PARSE(list, "[{\"title\":\"x\"},{\"id\":\"OK\"},{\"id\":\"bad/id\"}]"));
    assert(!PARSE(list, "[{\"title\":\"x\"},{\"id\":\"OK\",\"id\":\"again\"}]"));
    assert(!PARSE(list, "[{\"title\":\"x\"},{\"id\":\"ABCDEFGHIJKLMNOPQRSTUVWXYZ123456\"}]"));
    assert(!list->valid && list->count == 0);
}

static void test_url(void)
{
    char url[128];
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    assert(x4_sigl_url(X4_SIGL_FREE, url, sizeof(url)));
    assert(!strcmp(url, X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=MX&language=es-MX"));
    x4_lang_set(X4_LANG_EN, "en-GB", "GB");
    assert(x4_sigl_url(X4_SIGL_LEAVING, url, sizeof(url)));
    assert(!strcmp(url, X4_SIGL_URL_PREFIX X4_SIGL_GUID_LEAVING "&market=GB&language=en-GB"));
    assert(!x4_sigl_url(X4_SIGL_COUNT, url, sizeof(url)) && !url[0]);
    assert(!x4_sigl_url(X4_SIGL_NEW, url, 16) && !url[0]);
    x4_lang_set(X4_LANG_EN, "en-US&x", "US");
    assert(!x4_sigl_url(X4_SIGL_POPULAR, url, sizeof(url)) && !url[0]);
}

static void test_order_and_session_membership(void)
{
    catalog.state = X4_CATALOG_READY;
    catalog.count = 6;
    catalog.titles[0] = (X4CatalogTitle){.id = "mine-first", .product_id = "9MINEFIRST", .entitled = 1};
    catalog.titles[1] = (X4CatalogTitle){.id = "f2p", .product_id = "9FREEONLY", .entitled = 1, .f2p_only = 1};
    catalog.titles[2] = (X4CatalogTitle){.id = "free-store", .product_id = "9FREESTORE", .is_free_in_store = 1};
    catalog.titles[3] = (X4CatalogTitle){.id = "mine-second", .product_id = "9MINESECOND", .entitled = 1};
    catalog.titles[4] = (X4CatalogTitle){.id = "both", .product_id = "9MINEFREE", .entitled = 1, .is_free_in_store = 1};
    catalog.titles[5] = (X4CatalogTitle){.id = "alternate", .product_id = "9minefirst", .entitled = 1};

    X4RecentGames recent;
    x4_recent_init(&recent);
    x4_recent_begin(&recent, 1, 1);
    static const char recent_ids[][96] = {"MINE-SECOND", "stale-pc", "f2p", "mine-first"};
    assert(x4_recent_publish(&recent, 1, 1, recent_ids, 4, 0, 200, &catalog));
    strcpy(recent.entries[1].product_id, PC_ID);
    strcpy(recent.entries[1].name, "Plants vs. Zombies: Replanted - PC");

    assert(PARSE(&sigls[X4_SIGL_FREE],
        "[{\"title\":\"Servidor\"},{\"id\":\"" PC_ID "\"},"
        "{\"id\":\"9FREEONLY\"},{\"id\":\"9MINESECOND\"},{\"id\":\"9freeonly\"}]"));
    assert(PARSE(&sigls[X4_SIGL_NEW],
        "[{\"title\":\"Servidor\"},{\"id\":\"" PC_ID "\"},"
        "{\"id\":\"9minesecond\"},{\"id\":\"9MINEFIRST\"}]"));
    assert(PARSE(&sigls[X4_SIGL_POPULAR],
        "[{\"title\":\"Servidor\"},{\"id\":\"" PC_ID "\"},"
        "{\"id\":\"9MINEFREE\"},{\"id\":\"9MINESECOND\"}]"));
    assert(PARSE(&sigls[X4_SIGL_LEAVING],
        "[{\"title\":\"Servidor\"},{\"id\":\"" PC_ID "\"},{\"id\":\"9FREESTORE\"}]"));

    X4HomeRows home;
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    assert(home.count == X4_HOME_ROW_COUNT);
    for (unsigned i = 0; i < home.count; ++i) assert(home.rows[i].kind == (X4HomeRowKind)i);
    expect(&home, X4_HOME_CONTINUE, (uint16_t[]){3, 1, 0}, 3);
    expect(&home, X4_HOME_MINE, (uint16_t[]){4}, 1);
    expect(&home, X4_HOME_FREE, (uint16_t[]){1, 3}, 2);
    expect(&home, X4_HOME_NEW, (uint16_t[]){3, 0}, 2);
    expect(&home, X4_HOME_POPULAR, (uint16_t[]){4, 3}, 2);
    expect(&home, X4_HOME_LEAVING, (uint16_t[]){2}, 1);
    assert_session_only(&home);

    assert(!PARSE(&sigls[X4_SIGL_FREE], "[{\"title\":\"Error\"},"));
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    expect(&home, X4_HOME_FREE, (uint16_t[]){1, 2, 4}, 3);
    assert_session_only(&home);

    assert(PARSE(&sigls[X4_SIGL_FREE],
        "[{\"title\":\"Plants vs. Zombies: Replanted - PC\"},{\"id\":\"" PC_ID "\"}]"));
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    assert(!row(&home, X4_HOME_FREE));
    assert_session_only(&home);
    assert(PARSE(&sigls[X4_SIGL_FREE], "[]"));
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    assert(!row(&home, X4_HOME_FREE));
    sigls[X4_SIGL_FREE].pending = true;
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    assert(!row(&home, X4_HOME_FREE));

    catalog.state = X4_CATALOG_LOADING;
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    assert(!home.count);
}

static void test_continue_and_filtered_empty(void)
{
    memset(&catalog, 0, sizeof(catalog));
    memset(sigls, 0, sizeof(sigls));
    catalog.state = X4_CATALOG_READY;
    catalog.count = 2;
    catalog.titles[0] = (X4CatalogTitle){.id = "first", .product_id = "9FORZA", .entitled = 1};
    catalog.titles[1] = (X4CatalogTitle){.id = "alias", .product_id = "9forza", .entitled = 1};
    X4RecentGames recent;
    x4_recent_init(&recent);
    unsigned selected = 17;
    assert(!x4_home_continue_index(&catalog, &recent, &selected) && selected == 17);
    x4_recent_begin(&recent, 1, 1);
    assert(!x4_home_continue_index(&catalog, &recent, &selected));
    assert(!x4_recent_publish(&recent, 1, 1, NULL, 0, -1, 503, &catalog));
    assert(!x4_home_continue_index(&catalog, &recent, &selected));
    x4_recent_begin(&recent, 1, 2);
    assert(x4_recent_publish(&recent, 1, 2, NULL, 0, 0, 200, &catalog));
    assert(!x4_home_continue_index(&catalog, &recent, &selected));
    x4_recent_begin(&recent, 1, 3);
    static const char ids[][96] = {"removed", "alias", "first"};
    assert(x4_recent_publish(&recent, 1, 3, ids, 3, 0, 200, &catalog));
    assert(x4_home_continue_index(&catalog, &recent, &selected) && selected == 1);
    X4HomeRows home;
    x4_home_rows_build(&home, &catalog, &recent, sigls);
    expect(&home, X4_HOME_CONTINUE, (uint16_t[]){1}, 1);
    assert(!row(&home, X4_HOME_MINE));
}

static void test_limit_and_empty_rows(void)
{
    memset(&catalog, 0, sizeof(catalog));
    memset(sigls, 0, sizeof(sigls));
    catalog.state = X4_CATALOG_READY;
    X4HomeRows home;
    x4_home_rows_build(&home, &catalog, NULL, sigls);
    assert(!home.count);
    catalog.count = X4_HOME_ROW_MAX + 5;
    for (unsigned i = 0; i < catalog.count; ++i) {
        X4CatalogTitle *entry = &catalog.titles[i];
        snprintf(entry->id, sizeof(entry->id), "title%u", i);
        snprintf(entry->product_id, sizeof(entry->product_id), "PRODUCT%u", i);
        entry->entitled = 1;
        strcpy(sigls[X4_SIGL_NEW].ids[catalog.count - 1 - i], entry->product_id);
    }
    sigls[X4_SIGL_NEW].valid = true;
    sigls[X4_SIGL_NEW].count = catalog.count;
    x4_home_rows_build(&home, &catalog, NULL, sigls);
    assert(home.count == 2);
    assert(home.rows[0].kind == X4_HOME_MINE && home.rows[0].count == X4_HOME_ROW_MAX);
    assert(home.rows[1].kind == X4_HOME_NEW && home.rows[1].count == X4_HOME_ROW_MAX);
    for (unsigned i = 0; i < X4_HOME_ROW_MAX; ++i) {
        assert(home.rows[0].indices[i] == i);
        assert(home.rows[1].indices[i] == catalog.count - 1 - i);
    }
    catalog.count = X4_CATALOG_MAX + 1;
    x4_home_rows_build(&home, &catalog, NULL, sigls);
    assert(!home.count);
}

int main(void)
{
    test_parser();
    test_url();
    test_order_and_session_membership();
    test_continue_and_filtered_empty();
    test_limit_and_empty_rows();
    puts("home_rows PASS: ordered session membership, PC exclusion, F2P fallback, Continue fallback, My games exclusion and empty rows");
    return 0;
}
