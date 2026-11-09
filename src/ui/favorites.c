/* SPDX-License-Identifier: GPL-3.0-only */
#include "favorites.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum { FAVORITES_HEADER = 14 };
static const unsigned char favorites_magic[] = {'X','4','F','A','V','1',0,0};

_Static_assert(sizeof(X4Favorite) == X4_PRODUCT_ID_CAP + X4_FAVORITE_NAME_CAP,
    "The favorite record consists only of its two character arrays");

static void canonical_id(char out[X4_PRODUCT_ID_CAP], const char *product_id)
{
    for (size_t i = 0; product_id[i]; ++i) {
        char c = product_id[i];
        out[i] = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    }
}

static unsigned favorite_index(const X4Favorites *favorites, const char *product_id)
{
    for (unsigned i = 0; i < favorites->count; ++i)
        if (!strcmp(favorites->items[i].product_id, product_id)) return i;
    return favorites->count;
}

bool x4_favorites_contains(const X4Favorites *favorites, const char *product_id)
{
    if (!favorites || favorites->count > X4_FAVORITES_MAX || !x4_product_id_valid(product_id))
        return false;
    char canonical[X4_PRODUCT_ID_CAP] = {0};
    canonical_id(canonical, product_id);
    return favorite_index(favorites, canonical) < favorites->count;
}

static uint32_t digest(const X4Favorites *favorites)
{
    const unsigned char *data = (const unsigned char *)favorites->items;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < favorites->count * sizeof(favorites->items[0]); ++i)
        hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}

static void make_header(unsigned char header[FAVORITES_HEADER], const X4Favorites *favorites)
{
    memcpy(header, favorites_magic, sizeof(favorites_magic));
    header[8] = (unsigned char)favorites->count;
    header[9] = (unsigned char)(favorites->count >> 8);
    const uint32_t sum = digest(favorites);
    for (unsigned i = 0; i < 4; ++i) header[10 + i] = (unsigned char)(sum >> (i * 8));
}

static bool record_valid(const X4Favorite *record)
{
    if (!x4_product_id_valid(record->product_id) ||
        !memchr(record->name, 0, sizeof(record->name))) return false;
    char canonical[X4_PRODUCT_ID_CAP] = {0};
    canonical_id(canonical, record->product_id);
    if (memcmp(canonical, record->product_id, sizeof(canonical))) return false;
    for (const unsigned char *c = (const unsigned char *)record->name; *c; ++c)
        if (*c < 0x20 || *c == 0x7f) return false;
    return true;
}

bool x4_favorites_load(X4Favorites *favorites, const char *path)
{
    if (!favorites) return false;
    memset(favorites, 0, sizeof(*favorites));
    if (!path) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;

    unsigned char header[FAVORITES_HEADER];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
        !memcmp(header, favorites_magic, sizeof(favorites_magic));
    unsigned count = ok ? (unsigned)header[8] | (unsigned)header[9] << 8 : 0;
    ok = ok && count <= X4_FAVORITES_MAX &&
        fread(favorites->items, sizeof(favorites->items[0]), count, file) == count &&
        fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) ok = false;
    if (ok) {
        favorites->count = count;
        unsigned char expected[FAVORITES_HEADER];
        make_header(expected, favorites);
        ok = !memcmp(expected, header, sizeof(expected));
    }
    for (unsigned i = 0; ok && i < count; ++i) {
        const X4Favorite *record = &favorites->items[i];
        ok = record_valid(record) && favorite_index(favorites, record->product_id) == i;
    }
    if (!ok) memset(favorites, 0, sizeof(*favorites));
    return ok;
}

static bool save(const X4Favorites *favorites, const char *path)
{
    if (!path) return true;
    char temporary[512];
    const int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return false;
#if defined(X4_OPENORBIS)
    if (!strcmp(path, X4_FAVORITES_PATH)) (void)mkdir("/data/xcloud4", 0777);
#endif
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    unsigned char header[FAVORITES_HEADER];
    make_header(header, favorites);
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(favorites->items, sizeof(favorites->items[0]), favorites->count, file) == favorites->count;
    if (fclose(file)) ok = false;
    if (ok && !rename(temporary, path)) return true;
    (void)remove(temporary);
    return false;
}

static void copy_name(char out[X4_FAVORITE_NAME_CAP], const char *name)
{
    if (!name) return;
    size_t length = 0;
    while (name[length] && length + 1 < X4_FAVORITE_NAME_CAP) ++length;
    if (name[length])
        while (length && ((unsigned char)name[length] & 0xc0) == 0x80) --length;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        out[i] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
}

enum X4FavoritesResult x4_favorites_toggle(X4Favorites *favorites,
    const char *product_id, const char *name, const char *path)
{
    if (!favorites || favorites->count > X4_FAVORITES_MAX || !x4_product_id_valid(product_id))
        return X4_FAVORITES_ERROR;
    X4Favorite item = {0};
    canonical_id(item.product_id, product_id);
    const unsigned index = favorite_index(favorites, item.product_id);
    if (index == favorites->count) {
        if (index == X4_FAVORITES_MAX) return X4_FAVORITES_FULL;
        copy_name(item.name, name);
        favorites->items[favorites->count++] = item;
        if (save(favorites, path)) return X4_FAVORITES_ADDED;
        --favorites->count;
        return X4_FAVORITES_ERROR;
    }

    item = favorites->items[index];
    X4Favorite *entry = &favorites->items[index];
    const size_t tail = (--favorites->count - index) * sizeof(*entry);
    if (tail) memmove(entry, entry + 1, tail);
    if (save(favorites, path)) return X4_FAVORITES_REMOVED;
    if (tail) memmove(entry + 1, entry, tail);
    *entry = item;
    ++favorites->count;
    return X4_FAVORITES_ERROR;
}
