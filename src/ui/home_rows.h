/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "recent_games.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Public SIGL identifiers from PsBox's catalog reference. */
#define X4_SIGL_URL_PREFIX "https://catalog.gamepass.com/sigls/v2?id="
#define X4_SIGL_GUID_FREE "d8f4afcd-882a-49e3-86b3-f61fa0172b75"
#define X4_SIGL_GUID_NEW "06323672-b8c8-43cc-b0de-32d5a9834749"
#define X4_SIGL_GUID_POPULAR "6a589fa0-d493-472b-8e20-3813699d7056"
#define X4_SIGL_GUID_LEAVING "31ff2361-2772-4622-849b-f4f1abb4ad1b"
#define X4_SIGL_LOCALE_FORMAT "&market=%s&language=%s"

typedef enum {
    X4_SIGL_FREE, X4_SIGL_NEW, X4_SIGL_POPULAR, X4_SIGL_LEAVING, X4_SIGL_COUNT
} X4SiglKind;

typedef struct {
    unsigned count;
    bool valid, pending;
    char ids[X4_CATALOG_MAX][32];
} X4SiglList;
typedef X4SiglList X4HomeSigls[X4_SIGL_COUNT];

typedef enum {
    X4_HOME_CONTINUE, X4_HOME_MINE, X4_HOME_FREE, X4_HOME_NEW, X4_HOME_POPULAR,
    X4_HOME_LEAVING, X4_HOME_ROW_COUNT
} X4HomeRowKind;
enum { X4_HOME_ROW_MAX = 30 };

typedef struct {
    X4HomeRowKind kind;
    unsigned count;
    uint16_t indices[X4_HOME_ROW_MAX];
} X4HomeRow;

typedef struct {
    unsigned count;
    X4HomeRow rows[X4_HOME_ROW_COUNT];
} X4HomeRows;

/* Console locales used by x4_lang have the form es-MX and market MX. */
static inline bool x4_sigl_locale_valid(const char *market, const char *locale)
{
    return market && locale && strlen(market) == 2 && strlen(locale) == 5 &&
        market[0] >= 'A' && market[0] <= 'Z' && market[1] >= 'A' && market[1] <= 'Z' &&
        locale[0] >= 'a' && locale[0] <= 'z' && locale[1] >= 'a' && locale[1] <= 'z' &&
        locale[2] == '-' && locale[3] >= 'A' && locale[3] <= 'Z' &&
        locale[4] >= 'A' && locale[4] <= 'Z';
}

bool x4_sigl_url(X4SiglKind kind, char *url, size_t capacity);
/* A valid empty list differs from failure. No partial list is published. */
bool x4_sigl_parse(X4SiglList *out, const char *json, size_t length);
/* Main-owned presentation model. Each index refers to the supplied READY
 * session catalog. Rebuild when that snapshot, MRU or the SIGLs change. */
/* First available provider MRU entry, in provider order. */
bool x4_home_continue_index(const X4CatalogSnapshot *catalog, const X4RecentGames *recent,
    unsigned *index);

void x4_home_rows_build(X4HomeRows *out, const X4CatalogSnapshot *catalog,
    const X4RecentGames *recent, const X4SiglList sigls[X4_SIGL_COUNT]);
