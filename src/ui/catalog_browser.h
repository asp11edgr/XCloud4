/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../auth/xbox_catalog.h"

enum {
    X4_CATALOG_QUERY_MAX = 48,
    X4_CATALOG_KEYBOARD_COLUMNS = 6,
    X4_CATALOG_KEYBOARD_KEYS = 36,
};
enum X4CatalogFilter {
    X4_CATALOG_FILTER_ALL,
    X4_CATALOG_FILTER_CONFIRMED_ACCESS,
    X4_CATALOG_FILTER_UNCONFIRMED_ACCESS,
    X4_CATALOG_FILTER_COUNT,
};

/* Main-only, static storage: the index map is about 8 KiB, not a second
 * catalog. Every index addresses the current validated source snapshot.
 * Group counts cover the entire retained list, independent of the query. */
typedef struct {
    uint16_t indices[X4_CATALOG_MAX];
    unsigned filtered_count, selected;
    unsigned group_counts[X4_CATALOG_FILTER_COUNT];
    enum X4CatalogFilter filter;
    char query[X4_CATALOG_QUERY_MAX + 1];
    bool editing;
    unsigned keyboard_cursor;
    char selected_id[96];
    bool criteria_dirty;
} X4CatalogBrowser;

void x4_catalog_browser_init(X4CatalogBrowser *browser);
/* Rebuild only after source/criteria change. Preserve the selected original
 * title ID when criteria did not change; loading keeps that anchor for a
 * refresh. Other non-READY states invalidate selection. No network calls. */
void x4_catalog_browser_rebuild(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog);
/* delta is a visible-item offset: item movement wraps, page movement clamps.
 * Pass the current catalog so the original selected ID remains cached. */
void x4_catalog_browser_move(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog,
    int delta, bool page_clamp);
/* Changed criteria clear selection; true requests a rebuild. */
bool x4_catalog_browser_cycle_filter(X4CatalogBrowser *browser, int delta);
bool x4_catalog_browser_query_append(X4CatalogBrowser *browser, char character);
bool x4_catalog_browser_query_delete(X4CatalogBrowser *browser);
bool x4_catalog_browser_query_clear(X4CatalogBrowser *browser);
void x4_catalog_browser_keyboard_move(X4CatalogBrowser *browser, int dx, int dy);
char x4_catalog_browser_keyboard_char(const X4CatalogBrowser *browser);
/* Fail closed on non-READY, invalid counts, stale ID/map or criteria. Never
 * pass a visible ordinal directly to x4_auth_start_session. */
bool x4_catalog_browser_selected_index(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, unsigned *source_index);

/* Results: TRIANGLE enters editing; L2/R2 cycle access groups; CROSS starts
 * the mapped title; SQUARE refreshes; CIRCLE returns to account.
 * Editing: dpad selects A-Z/0-9, CROSS appends, SQUARE deletes, TRIANGLE
 * clears, R1 inserts a space, R2 applies and CIRCLE returns to results.
 * Apply/return retain the query. OPTIONS remains global exit in both modes.
 * Main owns button dispatch and sets editing; the model consumes no pad. */
