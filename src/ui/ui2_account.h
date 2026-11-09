/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../auth/device_auth.h"
#include "../auth/cloud_time.h"
#include "catalog_assets.h"
#include "play_history.h"
#include "ui_surface.h"

enum { X4_UI2_ACCOUNT_TOP_GAMES = 5, X4_UI2_ACCOUNT_HISTORY_FIRST = 4 };

typedef struct {
    unsigned row, left_row;
} X4Ui2Account;

typedef struct {
    unsigned games, count;
    uint64_t total_seconds;
    X4HistoryEntry most_played[X4_UI2_ACCOUNT_TOP_GAMES];
} X4Ui2AccountSummary;

typedef struct {
    const X4AuthSnapshot *auth;
    X4AuthRememberSnapshot remember;
    bool busy, closing, focus_owned;
    const char *gamertag, *initial, *server, *country;
    X4CloudTime cloud_time;
    const X4Ui2AccountSummary *history;
    const X4CatalogSnapshot *catalog;
    X4CatalogAssets *assets;
    uint64_t assets_revision;
} X4Ui2AccountModel;

typedef enum {
    X4_UI2_ACCOUNT_NONE, X4_UI2_ACCOUNT_SIGN_IN, X4_UI2_ACCOUNT_REMEMBER,
    X4_UI2_ACCOUNT_SERVER, X4_UI2_ACCOUNT_COUNTRY, X4_UI2_ACCOUNT_OPEN_GAME,
    X4_UI2_ACCOUNT_SIGN_OUT
} X4Ui2AccountAction;

/* Read-only view of the local history. Ordered by time, then last session. */
void x4_ui2_account_summary(const X4History *history, X4Ui2AccountSummary *out);
bool x4_ui2_account_move(X4Ui2Account *account, const X4Ui2AccountModel *model,
    int dx, int dy);
X4Ui2AccountAction x4_ui2_account_select(const X4Ui2Account *account,
    const X4Ui2AccountModel *model, const char **product_id);
/* The retained canvas is owned by the caller. SIZE_MAX means a full redraw. */
size_t x4_ui2_account_draw(const X4Ui2Account *account, const X4Ui2AccountModel *model,
    uint32_t *pixels, bool retained, X4UiRect damage[2]);
void x4_ui2_account_remember_modal(uint32_t *pixels);
void x4_ui2_account_logout_modal(bool remembered, bool confirm_focused, uint32_t *pixels);
