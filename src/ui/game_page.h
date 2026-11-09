/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "product_metadata.h"
#include "ui_surface.h"

enum { X4_GAME_SIMILAR_MAX = 10 };
typedef struct {
    unsigned source_index;
    const char *title;
    const X4CoverRaster *image;
} X4GamePageSimilar;

/* Borrowed data is only used during a call. The page neither pins assets nor
 * remembers a catalog index that a refreshed snapshot could invalidate. */
typedef struct {
    const char *product_id, *title;
    bool available, entitled, free_in_store, busy;
    const X4GameDetails *details;
    const char *category, *studio, *description;
    const char *last_session, *played_time; /* Localized values, when recorded on this console. */
    const char *notice;
    const X4CoverRaster *background;
    const X4CoverRaster *screenshots[X4_GAME_SCREENSHOT_MAX];
    const X4CoverRaster *fullscreen;
    bool favorite;
    const X4GamePageSimilar *similar;
    unsigned similar_count;
} X4GamePageModel;

typedef enum {
    X4_GAME_FOCUS_PLAY, X4_GAME_FOCUS_STORE, X4_GAME_FOCUS_SCREENSHOTS,
    X4_GAME_FOCUS_FAVORITE, X4_GAME_FOCUS_TABS, X4_GAME_FOCUS_DETAILS,
    X4_GAME_FOCUS_SIMILAR,
} X4GamePageFocus;

typedef enum {
    X4_GAME_TAB_SCREENSHOTS, X4_GAME_TAB_DETAILS, X4_GAME_TAB_SIMILAR, X4_GAME_TAB_COUNT,
} X4GamePageTab;

typedef struct {
    X4GamePageFocus focus;
    X4GamePageTab tab;
    unsigned screenshot_index; /* Index in the source manifest. */
    unsigned description_scroll, similar_index;
    bool qr_open, fullscreen_open;
} X4GamePage;

enum X4GamePageInput {
    X4_GAME_INPUT_UP = 1u << 0,
    X4_GAME_INPUT_DOWN = 1u << 1,
    X4_GAME_INPUT_LEFT = 1u << 2,
    X4_GAME_INPUT_RIGHT = 1u << 3,
    X4_GAME_INPUT_SELECT = 1u << 4,
    X4_GAME_INPUT_BACK = 1u << 5,
    X4_GAME_INPUT_SCROLL_UP = 1u << 6,
    X4_GAME_INPUT_SCROLL_DOWN = 1u << 7,
    X4_GAME_INPUT_PREVIOUS_TAB = 1u << 8,
    X4_GAME_INPUT_NEXT_TAB = 1u << 9,
};

enum X4GamePageAction {
    X4_GAME_ACTION_NONE, X4_GAME_ACTION_PLAY, X4_GAME_ACTION_BACK,
    X4_GAME_ACTION_REFRESH, X4_GAME_ACTION_FAVORITE, X4_GAME_ACTION_SIMILAR,
};

enum { X4_GAME_STORE_URL_CAP = 72 };
void x4_game_page_init(X4GamePage *page);
enum X4GamePageAction x4_game_page_update(X4GamePage *page,
    const X4GamePageModel *model, unsigned input);
int x4_game_page_fullscreen_index(const X4GamePage *page);
bool x4_game_page_store_url(const char *product_id, char *out, size_t capacity);
void x4_game_page_draw(const X4GamePage *page, const X4GamePageModel *model,
    uint32_t *pixels);

enum { X4_GAME_PAGE_DAMAGE_MAX = 11 };
/* Main/UI thread only. SIZE_MAX marks a full frame. A persistent canvas may
 * be reused until another renderer writes to it or the game page closes. */
size_t x4_game_page_draw_cached(const X4GamePage *page, const X4GamePageModel *model,
    uint64_t background_generation, uint32_t *pixels, bool persistent,
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX]);
void x4_game_page_cache_invalidate(void);
void x4_game_page_cache_dispose(void);
