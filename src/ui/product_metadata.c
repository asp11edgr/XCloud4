/* SPDX-License-Identifier: GPL-3.0-only */
#include "product_metadata.h"
#include "image_area.h"
#include "../auth/http_client.h"
#include "../auth/json.h"
#include "../auth/public_image_policy.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int x4_product_id_valid(const char *id)
{
    if (!id) return 0;
    size_t n = 0;
    for (; n < X4_PRODUCT_ID_CAP && id[n]; ++n) {
        unsigned char c = (unsigned char)id[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return 0;
    }
    return n > 0 && n < X4_PRODUCT_ID_CAP;
}
static const struct { unsigned width, height; } dimensions[] = {
    [X4_COVER_GRID] = {356, 200},
    [X4_COVER_DETAIL] = {524, 295},
    [X4_COVER_RECENT] = {96, 96},
    [X4_COVER_ROW] = {384, 216},
    [X4_COVER_HOME_FEATURE] = {852, 448},
    [X4_COVER_HOME_FORTNITE] = {420, 448},
    [X4_COVER_FORTNITE_POSTER] = {550, 770},
    [X4_COVER_BACKGROUND] = {1920, 1080}
};

int x4_product_cover_url(const char *uri, char out[X4_PUBLIC_COVER_URL_CAP])
{
    return x4_public_image_format(uri, dimensions[X4_COVER_GRID].width,
        dimensions[X4_COVER_GRID].height, out, X4_PUBLIC_COVER_URL_CAP);
}

int x4_product_surface_url(const X4ProductMetadata *metadata, enum X4CoverSurface surface,
    char out[X4_PUBLIC_COVER_URL_CAP])
{
    if (!out) return 0;
    out[0] = 0;
    if (!metadata || (unsigned)surface > X4_COVER_BACKGROUND) return 0;
    const char *url = metadata->cover_url;
    unsigned width = dimensions[surface].width, height = dimensions[surface].height;
    if (surface == X4_COVER_BACKGROUND || surface == X4_COVER_HOME_FORTNITE)
        url = metadata->hero_url;
    if (surface == X4_COVER_HOME_FEATURE && metadata->hero_url[0]) url = metadata->hero_url;
    if (surface == X4_COVER_RECENT || surface == X4_COVER_FORTNITE_POSTER)
        url = metadata->recent_url;
    if (!x4_http_public_image_url(url)) return 0;
    const char *query = strchr(url, '?');
    if (surface == X4_COVER_HOME_FEATURE) {
        width = 852;
        height = 480;
    } else if (surface == X4_COVER_HOME_FORTNITE) {
        width = 796;
        height = 448;
    } else if (surface == X4_COVER_RECENT || surface == X4_COVER_FORTNITE_POSTER) {
        bool poster = !strcmp(query, "?w=550&h=825&q=85&format=jpg");
        bool box = !strcmp(query, "?w=770&h=770&q=85&format=jpg") ||
            !strcmp(query, "?w=96&h=96&q=85&format=jpg");
        if (surface == X4_COVER_RECENT && !box) return 0;
        if (surface == X4_COVER_FORTNITE_POSTER) {
            if (!poster && !box) return 0;
            width = poster ? 550 : 770;
            height = poster ? 825 : 770;
        }
    }
    char uri[X4_PUBLIC_COVER_URL_CAP];
    size_t stem = (size_t)(query - url);
    memcpy(uri, url, stem);
    uri[stem] = 0;
    return x4_public_image_format(uri, width, height, out, X4_PUBLIC_COVER_URL_CAP);
}
static void display(X4JsonSpan object, const char *key, char *out, size_t capacity)
{
    X4JsonSpan v;
    out[0] = 0;
    if (x4_json_member(object, key, &v) == 1) x4_json_display_text(v, out, capacity);
}
static int same_id(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}
static int image_rank(const char *purpose)
{
    if (!strcmp(purpose, "TitledHeroArt")) return 4;
    if (!strcmp(purpose, "SuperHeroArt")) return 3;
    if (!strcmp(purpose, "BoxArt")) return 2;
    if (!strcmp(purpose, "Poster")) return 1;
    return 0;
}
static int hero_rank(const char *purpose)
{
    if (!strcmp(purpose, "SuperHeroArt")) return 2;
    if (!strcmp(purpose, "TitledHeroArt")) return 1;
    return 0;
}
static void game_property_details(X4JsonSpan properties, X4GameDetails *details)
{
    static const struct { const char *name; unsigned flags; } known[] = {
        {"XblSinglePlayer", X4_GAME_ATTR_SINGLE_PLAYER},
        {"SinglePlayer", X4_GAME_ATTR_SINGLE_PLAYER},
        {"XblOnlineMultiPlayer", X4_GAME_ATTR_ONLINE_MULTIPLAYER},
        {"XblLocalMultiPlayer", X4_GAME_ATTR_LOCAL_MULTIPLAYER},
        {"XblOnlineCoop", X4_GAME_ATTR_COOP},
        {"XblLocalCoop", X4_GAME_ATTR_COOP},
        {"CapabilityGamepad", X4_GAME_ATTR_GAMEPAD},
        {"XblGamepad", X4_GAME_ATTR_GAMEPAD},
        {"CapabilityKeyboardMouse", X4_GAME_ATTR_KEYBOARD_MOUSE},
        {"CapabilityKeyboardAndMouse", X4_GAME_ATTR_KEYBOARD_MOUSE},
        {"XblKeyboardMouse", X4_GAME_ATTR_KEYBOARD_MOUSE},
        {"ConsoleKeyboardMouse", X4_GAME_ATTR_KEYBOARD_MOUSE},
        {"CapabilityTouch", X4_GAME_ATTR_TOUCH},
        {"CapabilityTouchControls", X4_GAME_ATTR_TOUCH},
        {"XblTouchControls", X4_GAME_ATTR_TOUCH},
        {"OpenWorld", X4_GAME_ATTR_OPEN_WORLD},
        {"Indie", X4_GAME_ATTR_INDIE},
    };
    display(properties, "Subcategory", details->subcategory, sizeof(details->subcategory));
    X4JsonSpan attributes, attribute, value;
    size_t cursor = 0;
    if (x4_json_member(properties, "Attributes", &attributes) != 1) return;
    while (x4_json_item(attributes, &cursor, &attribute) == 1) {
        char name[64];
        if (x4_json_member(attribute, "Name", &value) != 1 ||
            x4_json_token(value, name, sizeof(name))) continue;
        for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
            if (same_id(name, known[i].name)) {
                details->attributes |= known[i].flags;
                break;
            }
        }
    }
}

static void game_market_details(X4JsonSpan product, X4GameDetails *details)
{
    X4JsonSpan markets, first, ratings, rating, value;
    size_t cursor = 0;
    if (x4_json_member(product, "MarketProperties", &markets) != 1 ||
        x4_json_item(markets, &cursor, &first) != 1) return;

    char release[48];
    if (x4_json_member(first, "OriginalReleaseDate", &value) == 1 &&
        !x4_json_token(value, release, sizeof(release)) && strlen(release) >= 10 &&
        release[4] == '-' && release[7] == '-' && release[0] >= '1' && release[0] <= '2') {
        bool digits = true;
        for (unsigned i = 0; i < 10; ++i)
            if (i != 4 && i != 7 && (release[i] < '0' || release[i] > '9')) digits = false;
        const int month = (release[5] - '0') * 10 + release[6] - '0';
        const int day = (release[8] - '0') * 10 + release[9] - '0';
        if (digits && month >= 1 && month <= 12 && day >= 1 && day <= 31) {
            memcpy(details->year, release, 4);
            memcpy(details->release_date, release, 10);
        }
    }

    if (x4_json_member(first, "ContentRatings", &ratings) != 1) return;
    cursor = 0;
    while (x4_json_item(ratings, &cursor, &rating) == 1) {
        char label[sizeof(details->age_rating)], system[16];
        if (x4_json_member(rating, "RatingId", &value) != 1 ||
            x4_json_token(value, label, sizeof(label)) || !label[0]) continue;
        if (x4_json_member(rating, "RatingSystem", &value) != 1 ||
            x4_json_token(value, system, sizeof(system))) continue;
        const bool esrb = !strcmp(system, "ESRB"), pegi = !strcmp(system, "PEGI");
        if (esrb || (pegi && !details->age_rating[0]))
            memcpy(details->age_rating, label, strlen(label) + 1);
        if (esrb) break;
    }
}

int x4_product_metadata_parse_details(const char *json, size_t length, const char *id,
    X4ProductMetadata *out, X4GameProductDetails *game)
{
    if (game) memset(game, 0, sizeof(*game));
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!x4_product_id_valid(id)) return -1;
    X4JsonSpan root, products, product, v, localized, first, properties;
    if (x4_json_parse(json, length, &root) || x4_json_member(root, "Products", &products) != 1) return -1;
    size_t cursor = 0;
    int found = 0, rc;
    while ((rc = x4_json_item(products, &cursor, &product)) == 1) {
        char product_id[X4_PRODUCT_ID_CAP];
        if (x4_json_member(product, "ProductId", &v) != 1 ||
            x4_json_token(v, product_id, sizeof(product_id)) || !x4_product_id_valid(product_id) ||
            !same_id(product_id, id)) continue;
        if (found) {
            memset(out, 0, sizeof(*out));
            if (game) memset(game, 0, sizeof(*game));
            return -1;
        }
        found = 1;
        memcpy(out->product_id, id, strlen(id) + 1);
        if (x4_json_member(product, "Properties", &properties) == 1) {
            display(properties, "Category", out->category, sizeof(out->category));
            if (game) game_property_details(properties, &game->details);
        }
        if (game) {
            memcpy(game->details.category, out->category, sizeof(game->details.category));
            game_market_details(product, &game->details);
        }
        size_t lc = 0;
        if (x4_json_member(product, "LocalizedProperties", &localized) != 1 ||
            x4_json_item(localized, &lc, &first) != 1) continue;
        display(first, "ProductTitle", out->name, sizeof(out->name));
        display(first, "PublisherName", out->publisher, sizeof(out->publisher));
        display(first, "DeveloperName", out->developer, sizeof(out->developer));
        display(first, "ShortDescription", out->description, sizeof(out->description));
        if (!out->description[0]) display(first, "ProductDescription", out->description, sizeof(out->description));
        if (game) {
            memcpy(game->details.title, out->name, sizeof(game->details.title));
            memcpy(game->details.publisher, out->publisher, sizeof(game->details.publisher));
            memcpy(game->details.developer, out->developer, sizeof(game->details.developer));
            display(first, "ProductDescription", game->details.description, sizeof(game->details.description));
            if (!game->details.description[0])
                memcpy(game->details.description, out->description, strlen(out->description) + 1);
        }
        X4JsonSpan images, image;
        if (x4_json_member(first, "Images", &images) != 1) continue;
        size_t ic = 0;
        int best = 0, best_hero = 0, best_vertical = 0;
        while (x4_json_item(images, &ic, &image) == 1) {
            char purpose[64], uri[X4_PUBLIC_COVER_URL_CAP], url[X4_PUBLIC_COVER_URL_CAP];
            uint32_t width, height;
            if (x4_json_member(image, "ImagePurpose", &v) != 1 || x4_json_token(v, purpose, sizeof(purpose)) ||
                x4_json_member(image, "Uri", &v) != 1 || x4_json_token(v, uri, sizeof(uri)) ||
                x4_json_member(image, "Width", &v) != 1 || x4_json_uint32(v, &width) ||
                x4_json_member(image, "Height", &v) != 1 || x4_json_uint32(v, &height) ||
                !width || !height || width > 16384 || height > 16384 || !x4_product_cover_url(uri, url)) continue;
            if (game && !strcmp(purpose, "Screenshot") &&
                game->details.screenshot_count < X4_GAME_SCREENSHOT_MAX) {
                unsigned count = game->details.screenshot_count;
                /* Use the existing allowlisted Details thumbnail size. */
                if (x4_public_image_format(uri, dimensions[X4_COVER_DETAIL].width,
                        dimensions[X4_COVER_DETAIL].height, url, sizeof(url))) {
                    unsigned i = 0;
                    while (i < count && strcmp(game->screenshot_url[i], url)) ++i;
                    if (i == count) {
                        memcpy(game->screenshot_url[count], url, strlen(url) + 1);
                        ++game->details.screenshot_count;
                    }
                }
                continue;
            }
            int rank = image_rank(purpose);
            if (rank > best) { best = rank; memcpy(out->cover_url, url, strlen(url) + 1); }
            int h_rank = hero_rank(purpose);
            if (h_rank > best_hero &&
                x4_public_image_format(uri, dimensions[X4_COVER_BACKGROUND].width,
                    dimensions[X4_COVER_BACKGROUND].height, url, sizeof(url))) {
                best_hero = h_rank;
                memcpy(out->hero_url, url, strlen(url) + 1);
            }
            bool poster = !strcmp(purpose, "Poster");
            int vertical_rank = poster ? 2 : !strcmp(purpose, "BoxArt");
            if (vertical_rank > best_vertical &&
                x4_public_image_format(uri, poster ? 550 : 770, poster ? 825 : 770, url, sizeof(url))) {
                best_vertical = vertical_rank;
                memcpy(out->recent_url, url, strlen(url) + 1);
            }
        }
    }
    if (rc < 0) {
        memset(out, 0, sizeof(*out));
        if (game) memset(game, 0, sizeof(*game));
        return -1;
    }
    return found ? 0 : -1;
}

int x4_product_metadata_parse(const char *json, size_t length, const char *id, X4ProductMetadata *out)
{
    return x4_product_metadata_parse_details(json, length, id, out, NULL);
}

int x4_product_screenshot_url(const X4GameProductDetails *game, unsigned index, int fullscreen,
    char out[X4_PUBLIC_COVER_URL_CAP])
{
    if (!out) return 0;
    out[0] = 0;
    if (!game || index >= X4_GAME_SCREENSHOT_MAX || index >= game->details.screenshot_count)
        return 0;
    const char *url = game->screenshot_url[index];
    if (!x4_http_public_image_url(url)) return 0;
    if (!fullscreen) {
        memcpy(out, url, strlen(url) + 1);
        return 1;
    }
    char uri[X4_PUBLIC_COVER_URL_CAP];
    size_t stem = (size_t)(strchr(url, '?') - url);
    memcpy(uri, url, stem);
    uri[stem] = 0;
    return x4_public_image_format(uri, dimensions[X4_COVER_BACKGROUND].width,
        dimensions[X4_COVER_BACKGROUND].height, out, X4_PUBLIC_COVER_URL_CAP);
}

/* The decoder is restricted to one cache worker. Budget includes temporary
 * buffers and allocation headers, not merely the final decoded pixels. */
static size_t decode_allocated;
#define X4_IMAGE_DECODE_BUDGET (16u * 1024u * 1024u)
typedef union { size_t size; max_align_t align; } Allocation;
static void *image_alloc(size_t n)
{
    if (n > X4_IMAGE_DECODE_BUDGET - sizeof(Allocation) ||
        decode_allocated > X4_IMAGE_DECODE_BUDGET - n - sizeof(Allocation)) return NULL;
    Allocation *a = malloc(sizeof(*a) + n);
    if (!a) return NULL;
    a->size = n + sizeof(*a); decode_allocated += a->size;
    return a + 1;
}
static void image_free(void *p)
{
    if (!p) return;
    Allocation *a = (Allocation *)p - 1;
    decode_allocated -= a->size; free(a);
}
static void *image_realloc(void *p, size_t n)
{
    if (!p) return image_alloc(n);
    Allocation *old = (Allocation *)p - 1;
    size_t size = old->size - sizeof(*old);
    void *fresh = image_alloc(n);
    if (!fresh) return NULL;
    memcpy(fresh, p, n < size ? n : size); image_free(p); return fresh;
}
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(n) image_alloc(n)
#define STBI_FREE(p) image_free(p)
#define STBI_REALLOC(p,n) image_realloc(p,n)
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

static int raster(const unsigned char *source, unsigned sw, unsigned sh, unsigned w, unsigned h, X4CoverRaster *out)
{
    const size_t count = (size_t)w * h;
    uint32_t *pixels = malloc(count * sizeof(*pixels));
    if (!pixels) return -1;
    unsigned dw = w, dh = (unsigned)((uint64_t)sh * w / sw);
    if (dh > h) { dh = h; dw = (unsigned)((uint64_t)sw * h / sh); }
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    unsigned ox = (w - dw) / 2, oy = (h - dh) / 2;
    if (dw != w || dh != h)
        for (size_t i = 0; i < count; ++i) pixels[i] = 0xff30261du;
    size_t offset = (size_t)oy * w + ox;
    if (x4_image_area(source, (size_t)sw * sh * 4, sw, sh,
            pixels + offset, count - offset, dw, dh, w)) {
        free(pixels);
        return -1;
    }
    *out = (X4CoverRaster){w, h, pixels};
    return 0;
}
void x4_cover_rasters_free(X4CoverRasters *out)
{
    if (!out) return;
    for (unsigned i = 0; i < X4_COVER_SURFACES; ++i) free(out->surface[i].rgba);
    memset(out, 0, sizeof(*out));
}
static unsigned char *decode(const unsigned char *encoded, size_t length, int *w, int *h)
{
    if (!encoded || !length || length > X4_HTTP_IMAGE_MAX || length > INT_MAX || decode_allocated)
        return NULL;
    int channels;
    if (!stbi_info_from_memory(encoded, (int)length, w, h, &channels) ||
        *w <= 0 || *h <= 0 || *w > 2048 || *h > 2048) return NULL;
    return stbi_load_from_memory(encoded, (int)length, w, h, &channels, 4);
}

int x4_product_surface_decode(const unsigned char *encoded, size_t length,
    enum X4CoverSurface surface, X4CoverRaster *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if ((unsigned)surface > X4_COVER_BACKGROUND) return -1;
    int w, h;
    unsigned char *pixels = decode(encoded, length, &w, &h);
    if (!pixels) return -1;
    unsigned width = dimensions[surface].width, height = dimensions[surface].height;
    int rc = -1;
    if (surface == X4_COVER_HOME_FEATURE || surface == X4_COVER_HOME_FORTNITE ||
        surface == X4_COVER_FORTNITE_POSTER) {
        size_t count = (size_t)width * height;
        uint32_t *crop = malloc(count * sizeof(*crop));
        if (crop) {
            rc = x4_image_area_cover(pixels, (size_t)w * h * 4, (unsigned)w, (unsigned)h,
                crop, count, width, height, width);
            if (rc) free(crop);
            else *out = (X4CoverRaster){width, height, crop};
        }
    } else {
        rc = raster(pixels, (unsigned)w, (unsigned)h, width, height, out);
    }
    stbi_image_free(pixels);
    return rc;
}

void x4_product_compose_background(X4CoverRaster *raster)
{
    if (!raster || !raster->rgba || raster->width != 1920 || raster->height != 1080) return;
    for (unsigned y = 0; y < raster->height; ++y) {
        unsigned bottom = y > 390u ? (y - 390u) * 248u / (1080u - 390u) : 0;
        for (unsigned x = 0; x < raster->width; ++x) {
            unsigned left = x < 1320u ? (1320u - x) * 224u / 1320u : 0;
            unsigned keep = (255u - left) * (255u - bottom) / 255u;
            unsigned alpha = 255u - keep;
            uint32_t *pixel = &raster->rgba[(size_t)y * raster->width + x];
            uint32_t color = *pixel;
            unsigned r = ((color & 255u) * keep + 6u * alpha + 127u) / 255u;
            unsigned g = (((color >> 8u) & 255u) * keep + 10u * alpha + 127u) / 255u;
            unsigned b = (((color >> 16u) & 255u) * keep + 18u * alpha + 127u) / 255u;
            *pixel = UINT32_C(0xff000000) | (b << 16u) | (g << 8u) | r;
        }
    }
}

int x4_cover_decode(const unsigned char *encoded, size_t length, X4CoverRasters *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    int w, h;
    unsigned char *pixels = decode(encoded, length, &w, &h);
    if (!pixels) return -1;
    int rc = raster(pixels, (unsigned)w, (unsigned)h, 320, 224, &out->surface[X4_COVER_GRID]) ||
        raster(pixels, (unsigned)w, (unsigned)h, 512, 288, &out->surface[X4_COVER_DETAIL]) ||
        raster(pixels, (unsigned)w, (unsigned)h, 96, 96, &out->surface[X4_COVER_RECENT]);
    stbi_image_free(pixels);
    if (rc) { x4_cover_rasters_free(out); return -1; }
    return 0;
}
