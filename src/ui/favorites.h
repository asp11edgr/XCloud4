/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "product_metadata.h"
#include <stdbool.h>

#define X4_FAVORITES_PATH "/data/xcloud4/favorites-v1.bin"
enum { X4_FAVORITES_MAX = 200, X4_FAVORITE_NAME_CAP = 128 };

typedef struct {
    char product_id[X4_PRODUCT_ID_CAP];
    char name[X4_FAVORITE_NAME_CAP];
} X4Favorite;

typedef struct {
    X4Favorite items[X4_FAVORITES_MAX];
    unsigned count;
} X4Favorites;

enum X4FavoritesResult {
    X4_FAVORITES_ERROR, X4_FAVORITES_ADDED, X4_FAVORITES_REMOVED, X4_FAVORITES_FULL,
};

/* UI owner only. Missing, corrupt and unknown versions leave an empty list. */
bool x4_favorites_load(X4Favorites *favorites, const char *path);
bool x4_favorites_contains(const X4Favorites *favorites, const char *product_id);
/* A failed replacement leaves the previous list and file intact. With a NULL
 * path, apply the same validated operation to memory only. */
enum X4FavoritesResult x4_favorites_toggle(X4Favorites *favorites,
    const char *product_id, const char *name, const char *path);
