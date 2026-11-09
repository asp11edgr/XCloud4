/* SPDX-License-Identifier: GPL-3.0-only */
#include "apple_ui.h"
#include "font.h"
#include "lang.h"
#include "release_check.h"
#include "screen.h"
#include "server_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 1920, HEIGHT = 1080, MOVES = 32, HALO = 14 };
static bool full_reference, update_available;
static unsigned patch_attempts, compositions[3];

enum X4UpdateStatus x4_release_check_status(char *version, size_t size)
{
    if (size) snprintf(version, size, "%s", update_available ? "9.99.0" : "");
    return update_available ? X4_UPDATE_AVAILABLE : X4_UPDATE_LATEST;
}

bool x4_server_settings_has_saved_choice(void)
{
    return true;
}

void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size)
{
    /* Fail only the three composed menu patches in the full reference.
     * Ordinary art, font and background allocations remain available. */
    if (size == (852 + 2 * HALO) * (448 + 2 * HALO) * sizeof(uint32_t) ||
        size == (420 + 2 * HALO) * (448 + 2 * HALO) * sizeof(uint32_t)) {
        ++patch_attempts;
        if (full_reference) return NULL;
    }
    return __real_malloc(size);
}

void __real_x4_apple_vertical_gradient_round_rect(uint32_t *pixels, int x, int y,
    int w, int h, int radius, uint32_t top, uint32_t bottom);

void __wrap_x4_apple_vertical_gradient_round_rect(uint32_t *pixels, int x, int y,
    int w, int h, int radius, uint32_t top, uint32_t bottom)
{
    if (x == 84 && y == 396 && w == 852) ++compositions[0];
    if (x == 966 && y == 436 && w == 420) ++compositions[1];
    if (x == 1434 && y == 514 && w == 384) ++compositions[2];
    __real_x4_apple_vertical_gradient_round_rect(pixels, x, y, w, h, radius, top, bottom);
}

static X4CoverRaster fixture(unsigned w, unsigned h)
{
    X4CoverRaster art = {w, h, malloc((size_t)w * h * sizeof(uint32_t))};
    assert(art.rgba);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            art.rgba[(size_t)y * w + x] = X4_RGB((x + y) % 256u, y % 256u, x % 256u);
    return art;
}

static X4UiRect focus_rect(int focus, const X4HomeDrawContext *context)
{
    static const X4UiRect elements[] = {
        {84,304,852,448}, {966,304,420,448}, {1416,304,420,448},
        {0}, {84,790,252,58}, {356,790,232,58}, {0,48,0,52}, {1784,48,52,52}
    };
    X4UiRect r = elements[focus];
    if (focus == 6) {
        const char *label = context->signed_in ? context->gamertag : x4_tr(X4_TR_APPLE_YOUR_ACCOUNT);
        r.w = x4_text_width_px(22, X4_FONT_SEMIBOLD, label) + 74;
        if (r.w < 190) r.w = 190;
        if (r.w > 340) r.w = 340;
        r.x = 1768 - r.w;
    }
    return (X4UiRect){r.x - HALO, r.y - HALO, r.w + 2 * HALO, r.h + 2 * HALO};
}

static bool contains(X4UiRect rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static void check_damage(int old_focus, int new_focus, const X4HomeDrawContext *context,
    const uint32_t *before, const uint32_t *after)
{
    X4UiRect actual[2] = {0};
    size_t count = x4_screen_home_damage(actual);
    assert(count == (old_focus == new_focus ? 0u : 2u));
    if (count) {
        X4UiRect expected[] = {focus_rect(old_focus, context), focus_rect(new_focus, context)};
        for (unsigned i = 0; i < 2; ++i)
            assert(actual[i].x == expected[i].x && actual[i].y == expected[i].y &&
                actual[i].w == expected[i].w && actual[i].h == expected[i].h);
    }
    for (int y = 0; y < HEIGHT; ++y) {
        size_t row = (size_t)y * WIDTH;
        if (!memcmp(before + row, after + row, WIDTH * sizeof(*before))) continue;
        for (int x = 0; x < WIDTH; ++x) {
            if (before[row + x] == after[row + x]) continue;
            assert(count && (contains(actual[0], x, y) || contains(actual[1], x, y)));
        }
    }
}

static bool continue_title_changed(const uint32_t *before, const uint32_t *after)
{
    for (int y = 604; y < 666; ++y) {
        size_t at = (size_t)y * WIDTH + 1452;
        if (memcmp(before + at, after + at, 348 * sizeof(*before))) return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    full_reference = !strcmp(argv[1], "full");
    assert(full_reference || !strcmp(argv[1], "incremental"));
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    if (x4_font_init("assets/fonts/inter.x4font"))
        assert(!x4_font_init("assets/fonts/noto_sans.x4font"));
    assert(!x4_font_is_fallback());

    size_t count = (size_t)WIDTH * HEIGHT;
    size_t bytes = count * sizeof(uint32_t);
    uint32_t *pixels = malloc(bytes), *previous = malloc(bytes);
    assert(pixels && previous);
    X4CoverRaster featured = fixture(852, 448), battle = fixture(420, 448);
    X4CoverRaster row = fixture(384, 216), square = fixture(96, 96);
    X4HomeDrawContext context = {0};
    X4Screen screen = {0};
    X4Controller controller = {0};
    uint64_t revision = 1;
    if (!full_reference) x4_screen_home_prepare(pixels);
    memset(pixels, 0x18, bytes); /* The cache owns its pixels, even after the work canvas is reused. */

    static const int focus[MOVES] = {
        0,1,2,2,4,5,7,6, 0,1,7,6,2,5,2,2,
        2,2,2,1,6,7,0,2, 0,2,2,6,7,4,5,5
    };
    const uint32_t revised = X4_RGB(0xf7, 0x22, 0x99);
    for (unsigned i = 0; i < MOVES; ++i) {
        memcpy(previous, pixels, bytes);
        unsigned drawn[3];
        memcpy(drawn, compositions, sizeof(drawn));
        bool data_changed = true;
        switch (i) {
        case 0:
            break;
        case 8:
            context.featured_hero = &featured;
            context.fortnite_cover = &battle;
            break;
        case 12:
            context.signed_in = 1;
            snprintf(context.gamertag, sizeof(context.gamertag), "LocalPlayer_Test");
            break;
        case 14:
            context.continue_name = "Nombre de juego largo con subtítulo";
            context.continue_cover = &row;
            break;
        case 16:
            /* Simulate three recycled asset allocations without changing their addresses. */
            featured.rgba[70 * featured.width + 210] = revised;
            battle.rgba[80 * battle.width + 160] = revised;
            row.rgba[82 * row.width + 126] = revised;
            ++revision;
            break;
        case 17:
            context.continue_name = "Título distinto con la misma portada";
            break;
        case 18:
            context.continue_cover = &square;
            break;
        case 20:
            x4_lang_set(X4_LANG_EN, "en-US", "US");
            break;
        case 22:
            update_available = true;
            break;
        case 24:
            x4_screen_home_invalidate();
            memset(pixels, 0x33, bytes);
            break;
        case 26:
            context.continue_name = NULL;
            context.continue_cover = NULL;
            break;
        case 27:
            context.signed_in = 0;
            break;
        default:
            data_changed = false;
            break;
        }
        screen.selected = focus[i];
        x4_screen_set_home_context(&context);
        x4_screen_home_asset_revision(revision);
        if (full_reference) {
            x4_screen_home_invalidate();
            memset(pixels, 0xa5, bytes);
        }
        x4_screen_draw(&screen, &controller, pixels);

        if (full_reference || data_changed)
            assert(x4_screen_home_damage(NULL) == SIZE_MAX);
        else
            check_damage(focus[i - 1], focus[i], &context, previous, pixels);
        if (!full_reference && (!data_changed || i == 22 || i == 24))
            assert(!memcmp(drawn, compositions, sizeof(drawn)));
        if (i == 16) {
            const size_t art_pixels[] = {
                (304 + 70) * WIDTH + 84 + 210,
                (304 + 80) * WIDTH + 966 + 160,
                (304 + 75 + 82) * WIDTH + 1416 + 18 + 126
            };
            for (unsigned card = 0; card < 3; ++card) {
                assert(previous[art_pixels[card]] != revised && pixels[art_pixels[card]] == revised);
                assert(compositions[card] == drawn[card] + 1);
            }
        }
        if (i == 17) {
            assert(continue_title_changed(previous, pixels));
            if (!full_reference)
                assert(compositions[0] == drawn[0] && compositions[1] == drawn[1] &&
                    compositions[2] == drawn[2] + 1);
        }
        if (i == 18)
            assert(pixels[(304 + 75 + 86) * WIDTH + 1416 + 18 + 192] != X4_RGB(26, 38, 52));
        assert(fwrite(pixels, sizeof(*pixels), count, stdout) == count);
    }
    assert(!fflush(stdout));
    assert(patch_attempts == 3);
    x4_screen_set_home_context(NULL);
    free(square.rgba);
    free(row.rgba);
    free(battle.rgba);
    free(featured.rgba);
    free(previous);
    free(pixels);
    x4_font_close();
    return 0;
}
