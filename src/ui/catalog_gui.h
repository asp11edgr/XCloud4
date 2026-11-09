/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "catalog_browser.h"
#include "catalog_assets.h"

typedef struct {
    int connected, busy, pending;
    const char *notice;
} X4CatalogGuiStatus;

/* UI status text only; no retry or network side effect. Used by placeholders
 * and host checks so a temporary failure cannot look permanently missing. */
const char *x4_catalog_gui_asset_status(enum X4CatalogAssetState state);

/* The same 1920x1080 CPU renderer is used by the console and host captures.
 * Borrowed Store surfaces are pinned only for a draw. No resize, file I/O,
 * network or font rasterization occurs here. A bounded 1920x1080 keyboard
 * backdrop is allocated only on opening and released when the keyboard closes. */
void x4_catalog_gui_draw(const X4CatalogSnapshot *catalog, const X4CatalogBrowser *browser,
    const X4RecentGames *recent, X4CatalogAssets *assets, const X4CatalogGuiStatus *status,
    uint32_t *pixels);

/* Main-owned persistent WB canvas variant. While the keyboard stays open,
 * only its modal rectangle is restored and its interactive controls redrawn. */
void x4_catalog_gui_draw_cached(const X4CatalogSnapshot *catalog, const X4CatalogBrowser *browser,
    const X4RecentGames *recent, X4CatalogAssets *assets, const X4CatalogGuiStatus *status,
    uint32_t *pixels);
#ifdef X4_CATALOG_GUI_TEST
/* Render the next frame fully without changing the held background. */
void x4_catalog_gui_home_test_force_full(void);
#endif
typedef struct {
    uint64_t background_preparations, cached_draws, full_canvas_copies;
    uint64_t allocation_failures;
    size_t cache_bytes;
    bool ready, last_draw_full;
} X4CatalogKeyboardCacheStats;
bool x4_catalog_gui_keyboard_cache_ready(const X4CatalogBrowser *browser);
void x4_catalog_gui_keyboard_cache_invalidate(void);
void x4_catalog_gui_keyboard_cache_snapshot(X4CatalogKeyboardCacheStats *out);

/* Retain content selection while the left navigation owns the visible focus. */
void x4_catalog_gui_sidebar_focused(bool focused);

/* Set the visible Discover clock before the draw signature is computed.
 * A changed marker requests the next bounded carousel transition frame. */
unsigned x4_catalog_gui_discover_clock(uint64_t now_us);
/* Zero when the last draw did not blend a carousel frame. Includes preparing
 * a new hero, when needed, as well as drawing the visible transition. */
unsigned x4_catalog_gui_discover_fade_step(uint64_t *elapsed_us);
/* Process-wide guard. The next frame copies the new hero without blending. */
void x4_catalog_gui_discover_disable_fade(void);

#ifdef X4_UI48_CAPTURE
void x4_ui48_capture_cover(X4UiRect bounds, const X4CoverRaster *source);
#endif
