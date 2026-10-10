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
    int entitled; /* Only service-supplied entitlement/subscription matches. */
} X4CatalogTitle;
/* Public view has no access tokens, user hashes or request/response bodies. */
typedef struct {
    enum X4CatalogState state;
    int error, http_status;
    uint32_t xerr;
    unsigned count, total;
    int truncated;
    char stage[80], region[80], offering[24];
    X4CatalogTitle titles[X4_CATALOG_MAX];
} X4CatalogSnapshot;
