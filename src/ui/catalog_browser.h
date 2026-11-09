/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../auth/xbox_catalog.h"
#include "recent_games.h"
#include "home_rows.h"
#include "product_metadata.h"
#include "ui_surface.h"
#include "favorites.h"
#include "play_history.h"

enum {
    X4_CATALOG_QUERY_MAX = 48,
    X4_CATALOG_KEYBOARD_COLUMNS = 6,
    X4_CATALOG_KEYBOARD_KEYS = 36,
    X4_CATALOG_COLUMNS = 3,
    X4_CATALOG_ROWS = 2,
    X4_CATALOG_PAGE_SIZE = 6,
    X4_CATALOG_TAB_COLUMNS = 4,
    X4_CATALOG_TAB_ROWS = 2,
    X4_CATALOG_TAB_PAGE_SIZE = X4_CATALOG_TAB_COLUMNS * X4_CATALOG_TAB_ROWS,
    X4_CATALOG_FEATURED_MAX = 4,
    X4_CATALOG_DISCOVER_ROW_VISIBLE = 5,
    X4_CATALOG_UI2_DISCOVER_VISIBLE = 4,
    X4_CATALOG_UI2_COLUMNS = 5,
    X4_CATALOG_UI2_PAGE_SIZE = X4_CATALOG_UI2_COLUMNS * X4_CATALOG_TAB_ROWS,
    X4_CATALOG_POPULAR_FALLBACK_MAX = 10,
};
enum X4CatalogSection { X4_CATALOG_SECTION_CATALOG, X4_CATALOG_SECTION_RECENT };
enum X4CatalogTab {
    X4_CAT_TAB_DISCOVER = 0,
    X4_CAT_TAB_CATALOG  = 1,
    X4_CAT_TAB_RECENTS  = 2,
    X4_CAT_TAB_SEARCH   = 3,
    X4_CAT_TAB_COUNT    = 4,
};
enum X4CatalogFrameAction {
    X4_CATALOG_FRAME_LEFT = 1u << 0, X4_CATALOG_FRAME_RIGHT = 1u << 1,
    X4_CATALOG_FRAME_UP = 1u << 2, X4_CATALOG_FRAME_DOWN = 1u << 3,
    X4_CATALOG_FRAME_PAGE = 1u << 4, X4_CATALOG_FRAME_FILTER = 1u << 5,
    X4_CATALOG_FRAME_QUERY = 1u << 6, X4_CATALOG_FRAME_REFRESH = 1u << 7,
    X4_CATALOG_FRAME_CLEAR = 1u << 8, X4_CATALOG_FRAME_SNAPSHOT = 1u << 9,
    X4_CATALOG_FRAME_METADATA = 1u << 10,
};
enum X4CatalogFilter {
    X4_CATALOG_FILTER_ALL,
    X4_CATALOG_FILTER_CONFIRMED_ACCESS,
    X4_CATALOG_FILTER_UNCONFIRMED_ACCESS,
    X4_CATALOG_FILTER_FAVORITES,
    X4_CATALOG_FILTER_COUNT,
};

enum { X4_UI2_GENRE_MAX = 32, X4_UI2_SIMILAR_MAX = 10 };
typedef enum {
    X4_UI2_CATALOG_GRID, X4_UI2_CATALOG_SEARCH, X4_UI2_CATALOG_GENRES,
    X4_UI2_CATALOG_ACCESS, X4_UI2_CATALOG_SORT,
} X4Ui2CatalogFocus;
typedef enum {
    X4_UI2_SORT_FEATURED, X4_UI2_SORT_AZ, X4_UI2_SORT_NEW,
} X4Ui2CatalogSort;
typedef enum {
    X4_UI2_COLLECTION_NONE,
    X4_UI2_COLLECTION_MULTIPLAYER, X4_UI2_COLLECTION_OPEN_WORLD, X4_UI2_COLLECTION_SPORTS,
    X4_UI2_COLLECTION_RACING, X4_UI2_COLLECTION_SHOOTER, X4_UI2_COLLECTION_INDIE,
    X4_UI2_COLLECTION_COUNT
} X4Ui2CollectionKind;
typedef struct {
    X4Ui2CollectionKind kind;
    unsigned count;
    uint16_t source; /* Representative product in the current catalog. */
} X4Ui2Collection;
typedef struct {
    char label[64];
    unsigned count, slot;
} X4Ui2GenreChip;
typedef enum {
    X4_UI2_CATALOG_ACT_NONE, X4_UI2_CATALOG_ACT_CONSUMED,
    X4_UI2_CATALOG_ACT_KEYBOARD, X4_UI2_CATALOG_ACT_FAVORITE,
    X4_UI2_CATALOG_ACT_REMOVE_FAVORITE
} X4Ui2CatalogAction;

enum {
    X4_UI2_LIBRARY_MAX = X4_CATALOG_MAX + X4_RECENT_MAX + X4_FAVORITES_MAX,
    X4_UI2_LIBRARY_SHELF = X4_CATALOG_UI2_COLUMNS,
};
typedef enum {
    X4_UI2_LIBRARY_ALL, X4_UI2_LIBRARY_FAVORITES, X4_UI2_LIBRARY_RECENT, X4_UI2_LIBRARY_TAB_COUNT,
} X4Ui2LibraryTab;
typedef enum {
    X4_UI2_LIBRARY_TABS, X4_UI2_LIBRARY_CONTINUE, X4_UI2_LIBRARY_SEE_ALL,
    X4_UI2_LIBRARY_SORT, X4_UI2_LIBRARY_GENRE, X4_UI2_LIBRARY_GRID,
} X4Ui2LibraryFocus;
typedef enum {
    X4_UI2_LIBRARY_RECENCY, X4_UI2_LIBRARY_ALPHABETIC, X4_UI2_LIBRARY_PLAYED,
    X4_UI2_LIBRARY_FAVORITE_FIRST, X4_UI2_LIBRARY_SORT_COUNT,
} X4Ui2LibrarySort;

typedef struct {
    const char *product_id, *title, *title_id;
    unsigned source; /* X4_CATALOG_MAX when no current title resolves. */
    bool favorite;
    const X4HistoryEntry *history;
} X4Ui2LibraryCard;
typedef struct X4Ui2LibraryStorage X4Ui2LibraryStorage;
typedef struct {
    const X4CatalogSnapshot *catalog;
    const X4RecentGames *recents;
    uint64_t recent_epoch, recent_generation, history_revision;
    unsigned catalog_count, recent_count, favorite_count;
    enum X4CatalogState catalog_state;
    enum X4RecentState recent_state;
    bool bound, dirty, popup, manage;
    X4Ui2LibraryTab tab;
    X4Ui2LibraryFocus focus;
    X4Ui2LibrarySort sort;
    unsigned genre, cursor, continue_selected, grid_selected, members, grid_count;
    X4Ui2LibraryStorage *storage;
} X4Ui2Library;

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
    enum X4CatalogSection section;
    unsigned recent_selected;
    char recent_selected_id[96];
    uint64_t recent_account_generation;
    bool recent_initialized;
    /* Apple dark UI (Phase 1) tabbed navigation state */
    enum X4CatalogTab active_tab;
    bool apple_mode;
    bool top_bar_focused;
    unsigned top_bar_index; /* 0..2 = tabs, 3 = Search, 4 = Settings, 5 = Account */
    bool hero_focused;      /* In Discover tab: true = hero, false = bottom row */
    unsigned featured_count;
    unsigned featured_index;
    uint16_t featured_sources[X4_CATALOG_FEATURED_MAX];
    unsigned discover_row_selected;
    unsigned detail_scroll_offset;
    X4HomeRows home_rows;
    unsigned home_columns[X4_HOME_ROW_COUNT];
    unsigned home_row, home_first_row;
    bool home_configured;
    uint64_t home_revision, home_focus_at;
    char home_candidate_id[X4_PRODUCT_ID_CAP];
    char home_background_id[X4_PRODUCT_ID_CAP];
    uint64_t tab_focus_at;
    char tab_candidate_id[X4_PRODUCT_ID_CAP], tab_background_id[X4_PRODUCT_ID_CAP];
    unsigned catalog_preferred_column, recent_preferred_column;
    bool open_fortnite_requested;
    bool open_settings_requested;
    bool open_account_requested;
    bool ui2_enabled, ui2_collection_focus, ui2_favorites;
    bool ui2_hero_favorite, ui2_selector_open, ui2_library_tabs;
    unsigned ui2_selector_cursor, ui2_favorite_preferred_column;
    X4Ui2CatalogFocus ui2_selector_preference;
    X4Ui2CatalogSort ui2_sort;
    X4HomeRow ui2_new;
    uint64_t ui2_revision, ui2_carousel_since;
    char ui2_featured_id[X4_PRODUCT_ID_CAP];
    char ui2_home_row_id[X4_PRODUCT_ID_CAP];
    X4Ui2CatalogFocus ui2_content_focus;
    X4Ui2CollectionKind ui2_collection_filter;
    X4Ui2Collection ui2_collections[X4_UI2_COLLECTION_COUNT - 1];
    unsigned ui2_collection_count, ui2_collection_selected, ui2_collection_filter_count;
    unsigned ui2_multiplayer_count;
    bool ui2_multiplayer_available;
    unsigned ui2_access_counts[X4_CATALOG_FILTER_COUNT];
    X4Ui2GenreChip ui2_genres[X4_UI2_GENRE_MAX];
    char ui2_genre_names[X4_UI2_GENRE_MAX][64];
    unsigned ui2_genre_count, ui2_genre, ui2_genre_slot, ui2_genre_sources;
    uint32_t ui2_genre_present, ui2_collection_genres[X4_UI2_COLLECTION_COUNT];
    uint32_t ui2_genre_masks[X4_CATALOG_MAX];
    uint8_t ui2_primary_genre[X4_CATALOG_MAX];
    uint16_t ui2_attributes[X4_CATALOG_MAX];
    char ui2_queries[2][X4_CATALOG_QUERY_MAX + 1];
    unsigned ui2_favorite_selected;
    const X4Favorites *favorites;
    const X4History *history;
    X4Ui2Library library;
    char ui2_favorite_id[X4_PRODUCT_ID_CAP];
} X4CatalogBrowser;

void x4_catalog_browser_init(X4CatalogBrowser *browser);
/* UI 2.0 uses the same browser and product anchors as the legacy paths.
 * Catalog and Search keep independent queries; an enter may require rebuild.
 * The owner of favorites keeps its buffer alive while bound. */
void x4_catalog_browser_ui2_select(X4CatalogBrowser *browser, enum X4CatalogTab tab);
X4Ui2CatalogAction x4_catalog_browser_ui2_activate(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog);
bool x4_catalog_browser_ui2_cycle_chip(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, int direction);
bool x4_catalog_browser_ui2_left_edge(const X4CatalogBrowser *browser);
bool x4_catalog_browser_ui2_close_selector(X4CatalogBrowser *browser);
unsigned x4_catalog_browser_ui2_selector_count(const X4CatalogBrowser *browser,
    X4Ui2CatalogFocus selector);
unsigned x4_catalog_browser_ui2_selector_value(const X4CatalogBrowser *browser,
    X4Ui2CatalogFocus selector);
X4Ui2CollectionKind x4_catalog_browser_ui2_genre_option(const X4CatalogBrowser *browser,
    unsigned value, unsigned *genre_index);
const X4CatalogTitle *x4_catalog_browser_ui2_hero(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog);
bool x4_catalog_browser_ui2_carousel_tick(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, uint64_t now_us, bool interacting);
void x4_catalog_browser_ui2_favorites(X4CatalogBrowser *browser, const X4Favorites *favorites);
const X4Favorite *x4_catalog_browser_ui2_favorite(const X4CatalogBrowser *browser);
void x4_catalog_browser_ui2_library_sync(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *recent, const X4History *history);
void x4_catalog_browser_ui2_library_close(X4CatalogBrowser *browser);
X4Ui2LibraryCard x4_catalog_browser_ui2_library_card(const X4CatalogBrowser *browser,
    bool continuing, unsigned ordinal);
X4Ui2LibraryCard x4_catalog_browser_ui2_library_selection(const X4CatalogBrowser *browser);
bool x4_catalog_browser_ui2_library_manage(X4CatalogBrowser *browser);
unsigned x4_catalog_browser_ui2_similar(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, unsigned source, unsigned out[X4_UI2_SIMILAR_MAX]);


/* Rebuild only after source/criteria change. Preserve the selected original
 * title ID when criteria did not change; loading keeps that anchor for a
 * refresh. Other non-READY states invalidate selection. No network calls. */
void x4_catalog_browser_rebuild(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog);
/* delta is a visible-item offset: item movement wraps, page movement clamps.
 * Pass the current catalog so the original selected ID remains cached. */
void x4_catalog_browser_move(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog,
    int delta, bool page_clamp);
/* Three columns/two rows. Horizontal movement stops at its row edge; row
 * movement retains the column and clamps an incomplete second row. */
bool x4_catalog_browser_grid_move(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog,
    int dx, int dy);
bool x4_catalog_browser_page_move(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog,
    const X4RecentGames *history, int direction);
unsigned x4_catalog_browser_page_start(const X4CatalogBrowser *browser);
unsigned x4_catalog_browser_page_count(const X4CatalogBrowser *browser);
/* Sync after account/history changes. Selection follows the remembered ID;
 * the first loaded account starts on recent games when history exists. */
void x4_catalog_browser_sync_recents(X4CatalogBrowser *browser, const X4RecentGames *history);
/* Six compact recent cards share the width of three catalog columns. UP
 * selects the first recent card above that column (column * 2); DOWN maps
 * the recent local position / 2 to the current catalog page's first row.
 * Both clamp incomplete pages. Recent LEFT/RIGHT stops at the visible
 * six-card page edges. Empty history never has a launch selection. */
bool x4_catalog_browser_nav(X4CatalogBrowser *browser, const X4CatalogSnapshot *catalog,
    const X4RecentGames *history, int dx, int dy);
bool x4_catalog_browser_resolved_index(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history, unsigned *source_index);
const X4CatalogTitle *x4_catalog_browser_selected_title(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history);
/* The caller maps simultaneous input and snapshot/metadata changes to these
 * flags before launching. LEFT and RIGHT are deliberately included. */
bool x4_catalog_browser_can_launch_frame(uint32_t action_flags);
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

/* Apple dark UI (Phase 1) helpers for Discover / Catalog / Recents / Fortnite tabs */
void x4_catalog_browser_rebuild_featured(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history);
unsigned x4_catalog_browser_discover_row_count(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history);
bool x4_catalog_browser_discover_row_source(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history,
    unsigned row_idx, unsigned *source_index);
bool x4_catalog_browser_apple_nav(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history,
    int dx, int dy, int tab_delta);
bool x4_catalog_browser_activate_top_bar(X4CatalogBrowser *browser);
bool x4_catalog_browser_scroll_detail(X4CatalogBrowser *browser, int delta);

void x4_catalog_browser_home_apply(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4HomeRows *rows);
static inline void x4_catalog_browser_home_rebuild(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history,
    const X4SiglList sigls[X4_SIGL_COUNT])
{
    X4HomeRows rows;
    x4_home_rows_build(&rows, catalog, history, sigls);
    x4_catalog_browser_home_apply(browser, catalog, &rows);
}
bool x4_catalog_browser_home_tick(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, uint64_t now_us);
/* Secondary tabs clear the worker request during the 300 ms debounce.
 * The UI keeps its own pin while the current title's art is pending. */
const char *x4_catalog_browser_tab_background(X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog, const X4RecentGames *history, uint64_t now_us);
unsigned x4_catalog_browser_home_row_start(const X4CatalogBrowser *browser, unsigned row);
const X4CatalogTitle *x4_catalog_browser_home_product(const X4CatalogSnapshot *catalog,
    const char *product_id);

typedef enum {
    X4_HOME_DAMAGE_OTHER, X4_HOME_DAMAGE_HORIZONTAL,
    X4_HOME_DAMAGE_VERTICAL, X4_HOME_DAMAGE_BACKGROUND, X4_HOME_DAMAGE_TAB
} X4HomeDamageKind;
void x4_catalog_gui_home_invalidate(void);
void x4_catalog_gui_home_prewarm(uint32_t *canvas);
/* SIZE_MAX means full canvas. Zero means the canvas was left untouched. */
size_t x4_catalog_gui_home_damage(X4UiRect rects[5], X4HomeDamageKind *kind);
