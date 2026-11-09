/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#define X4_CATALOG_MAX 4096
enum X4CatalogState {
    X4_CATALOG_IDLE, X4_CATALOG_LOADING, X4_CATALOG_READY,
    X4_CATALOG_CANCELLED, X4_CATALOG_ERROR
};
typedef struct {
    char id[96], product_id[32], name[128];
    /* Keep all three boolean flags in the original entitlement word: changing
     * the size of this title would move the input state in the owning auth
     * structure, even though the input source itself is untouched. */
    unsigned entitled : 1; /* Only service-supplied entitlement/subscription matches. */
    unsigned is_free_in_store : 1;
    unsigned f2p_only : 1;
} X4CatalogTitle;
_Static_assert(sizeof(X4CatalogTitle) == 260, "Preserve the T31 title layout");
/* Public view has no access tokens, user hashes or request/response bodies. */
typedef struct {
    enum X4CatalogState state;
    int error, http_status;
    uint32_t xerr;
    unsigned count, total;
    int truncated;
    char stage[80], region[80], offering[24], gamertag[32];
    X4CatalogTitle titles[X4_CATALOG_MAX];
} X4CatalogSnapshot;

#define X4_RECENT_MAX 25u
/* Server MRU only, temporary memory. No local history, entitlement, token,
 * account identifier or claim that a game can be resumed. IDs must resolve
 * against the current catalog before a launch. */
typedef struct {
    enum X4CatalogState state;
    int error, http_status;
    unsigned count;
    uint64_t account_epoch, request_generation;
    char stage[80];
    char title_ids[X4_RECENT_MAX][96];
} X4RecentSnapshot;
