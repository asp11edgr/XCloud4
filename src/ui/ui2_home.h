/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "play_history.h"
#include "catalog_assets.h"
#include "ui_surface.h"

enum {
    X4_UI2_HOME_CONTINUE = 0,
    X4_UI2_HOME_EXPLORE,
    X4_UI2_HOME_RECENT_FIRST,
    X4_UI2_HOME_RECENT_MAX = 5,
    X4_UI2_HOME_RECENT_ALL = X4_UI2_HOME_RECENT_FIRST + X4_UI2_HOME_RECENT_MAX,
    X4_UI2_HOME_POPULAR_FIRST,
    X4_UI2_HOME_POPULAR_MAX = 6,
    X4_UI2_HOME_ITEM_COUNT = X4_UI2_HOME_POPULAR_FIRST + X4_UI2_HOME_POPULAR_MAX
};
enum X4Ui2HomeMode {
    X4_UI2_HOME_SIGNED_OUT, X4_UI2_HOME_WITH_RECENT, X4_UI2_HOME_EMPTY,
    X4_UI2_HOME_LOADING, X4_UI2_HOME_ERROR
};
enum X4Ui2HomeDirection {
    X4_UI2_HOME_LEFT, X4_UI2_HOME_RIGHT, X4_UI2_HOME_UP, X4_UI2_HOME_DOWN
};
enum X4Ui2HomeAction {
    X4_UI2_HOME_NONE, X4_UI2_HOME_LOGIN, X4_UI2_HOME_CATALOG,
    X4_UI2_HOME_RECENTS, X4_UI2_HOME_GAME, X4_UI2_HOME_FORTNITE
};
typedef struct {
    enum X4Ui2HomeMode mode;
    unsigned catalog_count;
    unsigned continuation; /* X4_CATALOG_MAX when unavailable. */
    unsigned recent_count, popular_count;
    unsigned recent[X4_UI2_HOME_RECENT_MAX], popular[X4_UI2_HOME_POPULAR_MAX];
} X4Ui2HomeModel;
typedef struct {
    enum X4Ui2HomeAction action;
    unsigned source; /* Index in the same catalog snapshot for GAME. */
} X4Ui2HomeRoute;

/* UI owner only. Read already published provider data and the same Popular
 * row Discover uses. A signed-out model never includes prior account data. */
void x4_ui2_home_model(X4Ui2HomeModel *out, bool signed_in,
    const X4CatalogSnapshot *catalog, const X4RecentGames *recent,
    const X4HomeRows *rows);
int x4_ui2_home_normalize(const X4Ui2HomeModel *model, int selected);
int x4_ui2_home_move(const X4Ui2HomeModel *model, int selected,
    enum X4Ui2HomeDirection direction);
X4Ui2HomeRoute x4_ui2_home_activate(const X4Ui2HomeModel *model, int selected);

/* Home owns only this content rectangle. The shell/status have their own
 * independent damage and can safely be combined by the UI owner. */
X4UiRect x4_ui2_home_bounds(void);
bool x4_ui2_home_item_bounds(const X4Ui2HomeModel *model, int selected, X4UiRect *out);
bool x4_ui2_home_art_bounds(const X4Ui2HomeModel *model, int selected, X4UiRect *out);

/* Persistent canvas and precomposed unfocused content. selected=-1 hides the
 * content focus while the sidebar owns it. SIZE_MAX means the whole Home
 * content bounds changed. Otherwise up to two focus damage rectangles. The
 * renderer borrows previously cached art only; never requests a download. */
size_t x4_ui2_home_draw(const X4Ui2HomeModel *model, const X4CatalogSnapshot *catalog,
    X4CatalogAssets *assets, uint64_t asset_revision, int selected, bool canvas_valid,
    uint32_t *pixels, X4UiRect damage[2]);
void x4_ui2_home_invalidate(void);
void x4_ui2_home_history(const X4History *history);
/* Invalidate the index mapping whenever the main owner publishes a new catalog. */
void x4_ui2_home_catalog_changed(void);
void x4_ui2_home_close(void);
