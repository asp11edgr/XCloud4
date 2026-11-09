/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/catalog_assets.h"
#include <string.h>

/* Historical UI fixtures exercise the catalog without a background worker.
 * The T35 integrated fixture supplies its own revisioned background cache. */
int x4_catalog_assets_acquire_background(X4CatalogAssets *a, X4CatalogBackgroundView *v)
{ (void)a; (void)v; return 0; }
void x4_catalog_assets_release_background(X4CatalogAssets *a, X4CatalogBackgroundView *v)
{ (void)a; (void)v; }
void x4_catalog_assets_stats(X4CatalogAssets *a, X4CatalogAssetStats *out)
{ (void)a; memset(out, 0, sizeof(*out)); }
