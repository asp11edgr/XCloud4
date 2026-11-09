/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "catalog_assets.h"

enum X4Ui2FortnitePhase {
    X4_UI2_FORTNITE_IDLE,
    X4_UI2_FORTNITE_QUEUED,
    X4_UI2_FORTNITE_QUERYING,
    X4_UI2_FORTNITE_READY,
    X4_UI2_FORTNITE_REGION,
    X4_UI2_FORTNITE_ERROR,
};

typedef struct {
    enum X4Ui2FortnitePhase phase;
    int error;
} X4Ui2Fortnite;

bool x4_ui2_fortnite_is_product(const char *product);
void x4_ui2_fortnite_begin(X4Ui2Fortnite *view);
void x4_ui2_fortnite_started(X4Ui2Fortnite *view, int result);
void x4_ui2_fortnite_complete(X4Ui2Fortnite *view, const X4CatalogSnapshot *catalog, bool busy);
void x4_ui2_fortnite_back(X4Ui2Fortnite *view);
bool x4_ui2_fortnite_waiting(const X4Ui2Fortnite *view);
void x4_ui2_fortnite_draw(uint32_t *pixels, const X4Ui2Fortnite *view,
    const X4CatalogSnapshot *catalog, const X4CatalogAssetView *artwork);
