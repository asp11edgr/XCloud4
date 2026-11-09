/* SPDX-License-Identifier: GPL-3.0-only */
#include "http_client.h"
#include "public_image_policy.h"
#include <string.h>

static const char host[] = "//store-images.s-microsoft.com/image/apps.";
static const char scheme[] = "https:";
static const struct {
    unsigned width, height;
    const char *query;
} sizes[] = {
#define IMAGE_SIZE(w, h) {w, h, "?w=" #w "&h=" #h "&q=85&format=jpg"}
    IMAGE_SIZE(384, 216),
    IMAGE_SIZE(356, 200),
    IMAGE_SIZE(524, 295),
    IMAGE_SIZE(96, 96),
    IMAGE_SIZE(1920, 1080),
    IMAGE_SIZE(852, 480),
    IMAGE_SIZE(796, 448),
    IMAGE_SIZE(550, 825),
    IMAGE_SIZE(770, 770)
#undef IMAGE_SIZE
};

static size_t bounded_length(const char *p, size_t capacity)
{
    size_t n = 0;
    while (n < capacity && p[n]) ++n;
    return n;
}

static int filename(const char *p, size_t n)
{
    if (!n) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

int x4_http_public_image_url(const char *url)
{
    const size_t prefix = sizeof(scheme) + sizeof(host) - 2;
    if (!url || strncmp(url, scheme, sizeof(scheme) - 1) ||
        strncmp(url + sizeof(scheme) - 1, host, sizeof(host) - 1)) return 0;
    size_t length = bounded_length(url, 512);
    if (length == 512) return 0;
    const char *query = strchr(url + prefix, '?');
    if (!query || !filename(url + prefix, (size_t)(query - url - prefix))) return 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        if (!strcmp(query, sizes[i].query)) return 1;
    return 0;
}

int x4_public_image_format(const char *uri, unsigned width, unsigned height,
    char *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!uri) return 0;
    if (!strncmp(uri, scheme, sizeof(scheme) - 1)) uri += sizeof(scheme) - 1;
    if (strncmp(uri, host, sizeof(host) - 1)) return 0;
    const size_t host_len = sizeof(host) - 1;
    size_t length = bounded_length(uri, 512);
    if (length == 512 || !filename(uri + host_len, length - host_len)) return 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (sizes[i].width != width || sizes[i].height != height) continue;
        size_t scheme_len = sizeof(scheme) - 1;
        size_t query_len = strlen(sizes[i].query) + 1;
        if (scheme_len + length + query_len > capacity ||
            scheme_len + length + query_len > 512) return 0;
        memcpy(out, scheme, scheme_len);
        memcpy(out + scheme_len, uri, length);
        memcpy(out + scheme_len + length, sizes[i].query, query_len);
        return 1;
    }
    return 0;
}

