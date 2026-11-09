/* SPDX-License-Identifier: GPL-3.0-only */
#include "home_rows.h"
#include "lang.h"
#include "product_metadata.h"
#include "../auth/json.h"
#include <stdio.h>
#include <stdlib.h>

enum { LOOKUP_SLOTS = X4_CATALOG_MAX * 2 };
_Static_assert((LOOKUP_SLOTS & (LOOKUP_SLOTS - 1)) == 0, "power-of-two catalog index");
_Static_assert(X4_CATALOG_MAX <= UINT16_MAX, "catalog index fits");
_Static_assert(sizeof(((X4SiglList *)0)->ids[0]) == X4_PRODUCT_ID_CAP, "shared ProductId limit");

bool x4_sigl_url(X4SiglKind kind, char *url, size_t capacity)
{
    static const char *const ids[X4_SIGL_COUNT] = {
        X4_SIGL_GUID_FREE, X4_SIGL_GUID_NEW, X4_SIGL_GUID_POPULAR, X4_SIGL_GUID_LEAVING
    };
    if (!url || !capacity) return false;
    url[0] = 0;
    const char *market = x4_lang_market(), *locale = x4_lang_locale();
    if ((unsigned)kind >= X4_SIGL_COUNT || !x4_sigl_locale_valid(market, locale)) return false;
    int length = snprintf(url, capacity, X4_SIGL_URL_PREFIX "%s" X4_SIGL_LOCALE_FORMAT,
        ids[kind], market, locale);
    if (length >= 0 && (size_t)length < capacity) return true;
    url[0] = 0;
    return false;
}

bool x4_sigl_parse(X4SiglList *out, const char *json, size_t length)
{
    if (!out) return false;
    out->count = 0;
    out->valid = out->pending = false;
    X4JsonSpan root, item, field;
    if (!json || x4_json_parse(json, length, &root) ||
        x4_json_type(root) != X4_JSON_T_ARRAY) return false;
    size_t cursor = 0;
    int next = x4_json_item(root, &cursor, &item);
    if (next < 0) return false;
    if (next == 1 && (x4_json_member(item, "title", &field) != 1 ||
        x4_json_type(field) != X4_JSON_T_STRING)) return false;
    unsigned count = 0;
    while ((next = x4_json_item(root, &cursor, &item)) == 1) {
        if (count == X4_CATALOG_MAX || x4_json_member(item, "id", &field) != 1 ||
            x4_json_token(field, out->ids[count], sizeof(out->ids[count])) ||
            !x4_product_id_valid(out->ids[count])) return false;
        ++count;
    }
    if (next < 0) return false;
    out->count = count;
    out->valid = true;
    return true;
}

static unsigned product_letter(unsigned ch)
{
    return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch;
}

static bool same_product(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        if (product_letter((unsigned char)*a) != product_letter((unsigned char)*b)) return false;
    }
    return *a == *b;
}

static unsigned product_slot(const char *id)
{
    uint32_t hash = UINT32_C(2166136261);
    for (; *id; ++id) hash = (hash ^ product_letter((unsigned char)*id)) * UINT32_C(16777619);
    return hash & (LOOKUP_SLOTS - 1);
}

static unsigned lookup_slot(const uint16_t *index, const X4CatalogSnapshot *catalog, const char *id)
{
    unsigned slot = product_slot(id);
    while (index[slot] && !same_product(id, catalog->titles[index[slot] - 1].product_id))
        slot = (slot + 1) & (LOOKUP_SLOTS - 1);
    return slot;
}

static uint16_t *make_lookup(const X4CatalogSnapshot *catalog)
{
    uint16_t *index = calloc(LOOKUP_SLOTS, sizeof(*index));
    if (!index) return NULL;
    for (unsigned i = 0; i < catalog->count; ++i) {
        const char *id = catalog->titles[i].product_id;
        if (!x4_product_id_valid(id)) continue;
        unsigned slot = lookup_slot(index, catalog, id);
        if (!index[slot]) index[slot] = (uint16_t)(i + 1);
    }
    return index;
}

static unsigned find_product(const uint16_t *index, const X4CatalogSnapshot *catalog, const char *id)
{
    if (!x4_product_id_valid(id)) return catalog->count;
    if (index) {
        unsigned match = index[lookup_slot(index, catalog, id)];
        return match ? match - 1 : catalog->count;
    }
    for (unsigned i = 0; i < catalog->count; ++i) {
        if (same_product(id, catalog->titles[i].product_id)) return i;
    }
    return catalog->count;
}

static bool has_product(const X4HomeRow *row, const X4CatalogSnapshot *catalog, const char *id)
{
    for (unsigned i = 0; i < row->count; ++i)
        if (same_product(id, catalog->titles[row->indices[i]].product_id)) return true;
    return false;
}

static void add_title(X4HomeRow *row, const X4CatalogSnapshot *catalog, unsigned index)
{
    if (index >= catalog->count || row->count == X4_HOME_ROW_MAX) return;
    const X4CatalogTitle *title = &catalog->titles[index];
    if (!x4_product_id_valid(title->product_id) ||
        !x4_recent_title_id_valid(title->id, sizeof(title->id)) ||
        has_product(row, catalog, title->product_id)) return;
    row->indices[row->count++] = (uint16_t)index;
}

bool x4_home_continue_index(const X4CatalogSnapshot *catalog, const X4RecentGames *recent,
    unsigned *index)
{
    if (!index) return false;
    for (unsigned i = 0; i < x4_recent_count(recent); ++i) {
        unsigned source;
        if (x4_recent_catalog_index(x4_recent_get(recent, i), catalog, &source) &&
            x4_product_id_valid(catalog->titles[source].product_id)) {
            *index = source;
            return true;
        }
    }
    return false;
}

static void add_row(X4HomeRows *out, const X4HomeRow *row)
{
    if (row->count) out->rows[out->count++] = *row;
}

void x4_home_rows_build(X4HomeRows *out, const X4CatalogSnapshot *catalog,
    const X4RecentGames *recent, const X4SiglList sigls[X4_SIGL_COUNT])
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!catalog || catalog->state != X4_CATALOG_READY || !catalog->count ||
        catalog->count > X4_CATALOG_MAX) return;

    X4HomeRow continuing = {.kind = X4_HOME_CONTINUE};
    for (unsigned i = 0; i < x4_recent_count(recent); ++i) {
        unsigned index;
        if (x4_recent_catalog_index(x4_recent_get(recent, i), catalog, &index))
            add_title(&continuing, catalog, index);
    }
    add_row(out, &continuing);

    X4HomeRow mine = {.kind = X4_HOME_MINE};
    for (unsigned i = 0; i < catalog->count && mine.count < X4_HOME_ROW_MAX; ++i) {
        const X4CatalogTitle *title = &catalog->titles[i];
        if (title->entitled && !title->f2p_only &&
            !has_product(&continuing, catalog, title->product_id)) add_title(&mine, catalog, i);
    }
    add_row(out, &mine);

    static const X4HomeRowKind kinds[X4_SIGL_COUNT] = {
        X4_HOME_FREE, X4_HOME_NEW, X4_HOME_POPULAR, X4_HOME_LEAVING
    };
    uint16_t *index = NULL;
    bool indexed = false;
    for (unsigned kind = 0; kind < X4_SIGL_COUNT; ++kind) {
        X4HomeRow row = {.kind = kinds[kind]};
        const X4SiglList *list = sigls ? &sigls[kind] : NULL;
        if (list && list->pending) continue;
        if (list && list->valid && list->count <= X4_CATALOG_MAX) {
            if (list->count && !indexed) {
                index = make_lookup(catalog);
                indexed = true;
            }
            for (unsigned i = 0; i < list->count && row.count < X4_HOME_ROW_MAX; ++i)
                add_title(&row, catalog, find_product(index, catalog, list->ids[i]));
        } else if (kind == X4_SIGL_FREE) {
            for (unsigned i = 0; i < catalog->count && row.count < X4_HOME_ROW_MAX; ++i) {
                const X4CatalogTitle *title = &catalog->titles[i];
                if (title->is_free_in_store || title->f2p_only) add_title(&row, catalog, i);
            }
        }
        add_row(out, &row);
    }
    free(index);
}
