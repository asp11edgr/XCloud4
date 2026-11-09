/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define X4_PRODUCT_ID_CAP 32
#define X4_PUBLIC_COVER_URL_CAP 512
typedef struct {
    char product_id[X4_PRODUCT_ID_CAP];
    char name[128], publisher[128], developer[128], category[64], description[512];
    char cover_url[X4_PUBLIC_COVER_URL_CAP];
    char hero_url[X4_PUBLIC_COVER_URL_CAP];
    /* Vertical art. The canonical size distinguishes Poster from BoxArt.
     * Old disk records may still contain a 96x96 BoxArt URL here. */
    char recent_url[X4_PUBLIC_COVER_URL_CAP];
} X4ProductMetadata;

#define X4_GAME_SCREENSHOT_MAX 6u
/* Xbox Store listings allow 10,000 description characters. Preserve up to
 * four UTF-8 bytes per character plus the terminator in the full game page. */
#define X4_GAME_DESCRIPTION_CHARS 10000u
#define X4_GAME_DESCRIPTION_BYTES (4u * X4_GAME_DESCRIPTION_CHARS + 1u)
enum X4GameAttribute {
    X4_GAME_ATTR_SINGLE_PLAYER = 1u << 0,
    X4_GAME_ATTR_ONLINE_MULTIPLAYER = 1u << 1,
    X4_GAME_ATTR_COOP = 1u << 2,
    X4_GAME_ATTR_GAMEPAD = 1u << 3,
    X4_GAME_ATTR_KEYBOARD_MOUSE = 1u << 4,
    X4_GAME_ATTR_TOUCH = 1u << 5,
    X4_GAME_ATTR_LOCAL_MULTIPLAYER = 1u << 6,
    X4_GAME_ATTR_OPEN_WORLD = 1u << 7,
    X4_GAME_ATTR_INDIE = 1u << 8,
};

typedef struct {
    char title[128], publisher[128], developer[128], category[64];
    char year[5], age_rating[32], description[X4_GAME_DESCRIPTION_BYTES];
    unsigned screenshot_count;
    char subcategory[64], release_date[11];
    unsigned attributes;
} X4GameDetails;

/* Kept separate from X4ProductMetadata, including its on-disk ABI. */
typedef struct {
    X4GameDetails details;
    char screenshot_url[X4_GAME_SCREENSHOT_MAX][X4_PUBLIC_COVER_URL_CAP];
} X4GameProductDetails;

int x4_product_metadata_parse_details(const char *json, size_t length, const char *product_id,
    X4ProductMetadata *metadata, X4GameProductDetails *details);
int x4_product_screenshot_url(const X4GameProductDetails *details, unsigned index, int fullscreen,
    char out[X4_PUBLIC_COVER_URL_CAP]);

int x4_product_id_valid(const char *id);
/* Parses only the matching validated ProductId from public Store metadata.
 * A name/image is not entitlement. Missing optional fields stay empty. */
int x4_product_metadata_parse(const char *json, size_t length, const char *product_id,
    X4ProductMetadata *out);
int x4_product_cover_url(const char *uri, char out[X4_PUBLIC_COVER_URL_CAP]);

enum X4CoverSurface {
    X4_COVER_GRID, X4_COVER_DETAIL, X4_COVER_RECENT, X4_COVER_ROW,
    X4_COVER_HOME_FEATURE, X4_COVER_HOME_FORTNITE, X4_COVER_FORTNITE_POSTER,
    X4_COVER_SURFACES,
    X4_COVER_BACKGROUND = X4_COVER_SURFACES
};
typedef struct { unsigned width, height; uint32_t *rgba; } X4CoverRaster;
typedef struct { X4CoverRaster surface[X4_COVER_SURFACES]; } X4CoverRasters;

int x4_product_surface_url(const X4ProductMetadata *metadata, enum X4CoverSurface surface,
    char out[X4_PUBLIC_COVER_URL_CAP]);
/* Single decoder owner. JPEG/PNG only, at most 2 MiB encoded, 2048 pixels per
 * axis and 16 MiB decoder allocation. Returns the final surface dimensions. */
int x4_product_surface_decode(const unsigned char *encoded, size_t length,
    enum X4CoverSurface surface, X4CoverRaster *out);
/* Apply the Descubrir gradient once, in place, to a 1920x1080 background. */
void x4_product_compose_background(X4CoverRaster *raster);
/* Existing local application icon variants. Public covers use the per-surface API. */
int x4_cover_decode(const unsigned char *encoded, size_t length, X4CoverRasters *out);
void x4_cover_rasters_free(X4CoverRasters *out);
