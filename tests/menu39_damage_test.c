/* SPDX-License-Identifier: GPL-3.0-only */
#include "apple_ui.h"
#include "font.h"
#include "lang.h"
#include "release_check.h"
#include "screen.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WIDTH = 1920, HEIGHT = 1080, MOVES = 32 };
static bool update_available;

static uint64_t now_ns(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC_RAW, &t));
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

enum X4UpdateStatus x4_release_check_status(char *version, size_t size)
{
    if (size) snprintf(version, size, "%s", update_available ? "9.99.0" : "");
    return update_available ? X4_UPDATE_AVAILABLE : X4_UPDATE_LATEST;
}

static X4CoverRaster art(unsigned width, unsigned height)
{
    X4CoverRaster raster = {width, height, malloc((size_t)width * height * sizeof(uint32_t))};
    assert(raster.rgba);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            raster.rgba[(size_t)y * width + x] = X4_RGB((x + y) % 256u, y % 256u, x % 256u);
    return raster;
}

#if X4_MENU39_DAMAGE
static X4UiRect expected_rect(int selected, const X4HomeDrawContext *ctx)
{
    static const X4UiRect elements[] = {
        {84,304,852,448}, {966,304,420,448}, {1416,304,420,448},
        {84,790,232,58}, {336,790,252,58}, {608,790,232,58},
        {0,48,0,52}, {1784,48,52,52}
    };
    X4UiRect r = elements[selected];
    if (selected == 6) {
        const char *label = ctx->signed_in && ctx->gamertag[0] ?
            ctx->gamertag : x4_tr(X4_TR_APPLE_YOUR_ACCOUNT);
        r.w = x4_text_width_px(22, X4_FONT_SEMIBOLD, label) + 74;
        if (r.w < 190) r.w = 190;
        if (r.w > 340) r.w = 340;
        r.x = 1784 - 16 - r.w;
    }
    return (X4UiRect){r.x - 14, r.y - 14, r.w + 28, r.h + 28};
}

static bool same_rect(X4UiRect a, X4UiRect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static bool contained(X4UiRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static size_t assert_damage(int prior, int selected, bool full, const X4HomeDrawContext *ctx,
    const uint32_t *previous, const uint32_t *pixels)
{
    X4UiRect rects[2];
    size_t count = x4_screen_home_damage(rects);
    if (full) {
        assert(count == SIZE_MAX);
        return (size_t)WIDTH * HEIGHT * sizeof(*pixels);
    }
    assert(count == (selected == prior ? 0u : 2u));
    if (count) {
        assert(same_rect(rects[0], expected_rect(prior, ctx)));
        assert(same_rect(rects[1], expected_rect(selected, ctx)));
    }
    size_t bytes = 0;
    for (size_t r = 0; r < count; ++r)
        bytes += (size_t)rects[r].w * rects[r].h * sizeof(*pixels);
    assert(bytes < (size_t)WIDTH * HEIGHT * sizeof(*pixels));
    for (int y = 0; y < HEIGHT; ++y) {
        size_t start = (size_t)y * WIDTH;
        if (!memcmp(previous + start, pixels + start, (size_t)WIDTH * sizeof(*pixels))) continue;
        for (int x = 0; x < WIDTH; ++x) {
            if (previous[start + x] == pixels[start + x]) continue;
            assert(count && (contained(rects[0], x, y) || contained(rects[1], x, y)));
        }
    }
    return bytes;
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    bool prepare = !strcmp(argv[1], "prepared");
    assert(prepare || !strcmp(argv[1], "cold"));
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    if (x4_font_init("assets/fonts/inter.x4font"))
        assert(!x4_font_init("assets/fonts/noto_sans.x4font"));
    assert(!x4_font_is_fallback());
    size_t count = (size_t)WIDTH * HEIGHT;
    uint32_t *pixels = calloc(count, sizeof(*pixels));
    assert(pixels);
    X4CoverRaster featured = art(852, 448);
    X4CoverRaster fortnite = art(420, 448);
    X4HomeDrawContext context = {0};
    X4Screen screen = {.page = 0};
    X4Controller controller = {0};
#if X4_MENU39_DAMAGE
    uint32_t *previous = malloc(count * sizeof(*previous));
    assert(previous);
    int prior = 0;
    uint64_t asset_revision = 1;
    uint64_t start = now_ns();
    if (prepare) x4_screen_home_prepare(pixels);
    fprintf(stderr, "MENU39 prepare mode=%s ns=%" PRIu64 "\n", argv[1], prepare ? now_ns() - start : 0);
#else
    assert(!prepare);
#endif

    static const int focus[MOVES] = {
        0,1,2,7,6,5,4,3, 0,1,1,7,6,0,3,4,
        2,4,5,3, 7,6,0,1, 0,2,5,6, 4,3,7,0
    };
    for (unsigned i = 0; i < MOVES; ++i) {
#if X4_MENU39_DAMAGE
        bool full = i == 0 || i == 8 || i == 16 || i == 20 || i == 24 || i == 28;
        if (i) memcpy(previous, pixels, count * sizeof(*pixels));
#endif
        switch (i) {
        case 8:
            context.featured_hero = &featured;
            context.fortnite_cover = &fortnite;
            break;
        case 16:
            context.signed_in = 1;
            memcpy(context.gamertag, "LocalPlayer_Long_0123456789012",
                sizeof("LocalPlayer_Long_0123456789012"));
            x4_lang_set(X4_LANG_EN, "en-US", "US");
            break;
        case 20:
            context.fortnite_cover = NULL;
            update_available = true;
            break;
        case 24:
            /* Simulate a new raster published at a reused address. */
            featured.rgba[448u * 852u / 2u + 300u] = X4_RGB(244, 8, 6);
#if X4_MENU39_DAMAGE
            ++asset_revision;
#endif
            break;
        case 28:
#if X4_MENU39_DAMAGE
            x4_screen_home_invalidate();
#endif
            memset(pixels, 0x18, count * sizeof(*pixels));
            break;
        default:
            break;
        }
        screen.selected = focus[i];
        x4_screen_set_home_context(&context);
#if X4_MENU39_DAMAGE
        x4_screen_home_asset_revision(asset_revision);
#endif
        uint64_t began = now_ns();
        x4_screen_draw(&screen, &controller, pixels);
        uint64_t elapsed = now_ns() - began;
#if X4_MENU39_DAMAGE
        size_t bytes = assert_damage(prior, screen.selected, full, &context, previous, pixels);
        prior = screen.selected;
#else
        size_t bytes = count * sizeof(*pixels);
#endif
        fprintf(stderr, "MENU39 draw mode=%s step=%u ns=%" PRIu64 " damage_bytes=%zu\n",
            argv[1], i, elapsed, bytes);
        assert(fwrite(pixels, sizeof(*pixels), count, stdout) == count);
    }
    assert(!fflush(stdout));
    x4_screen_set_home_context(NULL);
#if X4_MENU39_DAMAGE
    free(previous);
#endif
    free(featured.rgba);
    free(fortnite.rgba);
    free(pixels);
    x4_font_close();
    return 0;
}
