/* SPDX-License-Identifier: GPL-3.0-only */
#include "font.h"
#include "lang.h"
#include "release_check.h"
#include "screen.h"
#if X4_CAPTURE_NEW_SURFACES
#include "catalog_assets.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

enum { WIDTH = 1920, HEIGHT = 1080 };

/* Draw a settled frame with no release notification or network activity. */
enum X4UpdateStatus x4_release_check_status(char *version_out, size_t capacity)
{
    if (capacity) version_out[0] = '\0';
    return X4_UPDATE_LATEST;
}

static X4CoverRaster load_surface(const char *data, const char *name,
    enum X4CoverSurface surface, unsigned width, unsigned height)
{
    char path[1024];
    int count = snprintf(path, sizeof(path), "%s/images/%s.jpg", data, name);
    assert(count > 0 && (size_t)count < sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) fprintf(stderr, "Falta el fixture %s\n", path);
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && size <= 2 * 1024 * 1024);
    rewind(file);
    unsigned char *bytes = malloc((size_t)size);
    assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
    assert(!fclose(file));
    X4CoverRaster raster = {0};
    assert(!x4_product_surface_decode(bytes, (size_t)size, surface, &raster));
    assert(raster.rgba && raster.width == width && raster.height == height);
    free(bytes);
    return raster;
}

static void write_ppm(const char *out, const char *screen, const char *mode, const uint32_t *pixels)
{
    char path[1024];
    int count = snprintf(path, sizeof(path), "%s/%s-%s.ppm", out, screen, mode);
    assert(count > 0 && (size_t)count < sizeof(path));
    FILE *file = fopen(path, "wbx");
    assert(file);
    assert(fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT) > 0);
    for (int y = 0; y < HEIGHT; ++y) {
        unsigned char row[WIDTH * 3];
        for (int x = 0; x < WIDTH; ++x) {
            uint32_t rgba = pixels[y * WIDTH + x];
            row[x * 3] = (unsigned char)rgba;
            row[x * 3 + 1] = (unsigned char)(rgba >> 8);
            row[x * 3 + 2] = (unsigned char)(rgba >> 16);
        }
        assert(fwrite(row, 1, sizeof(row), file) == sizeof(row));
    }
    assert(!fclose(file));
}

#if X4_CAPTURE_NEW_SURFACES
/* Rectangles deliberately exclude text, buttons, badges, focus and rounded borders. */
static void check_pixels(const char *name, const uint32_t *frame, const X4CoverRaster *raster,
    unsigned left, unsigned top, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    assert(x0 < x1 && x1 <= raster->width && y0 < y1 && y1 <= raster->height);
    assert(left + x1 <= WIDTH && top + y1 <= HEIGHT);
    for (unsigned y = y0; y < y1; ++y) {
        for (unsigned x = x0; x < x1; ++x) {
            uint32_t drawn = frame[(top + y) * WIDTH + left + x];
            uint32_t source = raster->rgba[y * raster->width + x];
            if (drawn != source) {
                fprintf(stderr, "%s: píxel diferente (%u,%u), pantalla=%08x raster=%08x\n",
                    name, x, y, drawn, source);
                abort();
            }
        }
    }
    printf("PASS_PIXELES_1_1 %s %ux%u raster=%ux%u\n",
        name, x1 - x0, y1 - y0, raster->width, raster->height);
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *data = argv[1], *out = argv[2];
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    if (x4_font_init("assets/fonts/inter.x4font") != 0)
        assert(x4_font_init("assets/fonts/noto_sans.x4font") == 0);
    uint32_t *frame = calloc(WIDTH * HEIGHT, sizeof(*frame));
    assert(frame);
    X4HomeDrawContext context = {.signed_in = 1};
#if X4_CAPTURE_NEW_SURFACES
    const char *mode = "despues";
    X4CoverRaster featured = load_surface(data, "9NKX70BBCDRN-852x480", X4_COVER_HOME_FEATURE, 852, 448);
    X4CoverRaster fortnite = load_surface(data, "BT5P2X999VH2-796x448", X4_COVER_HOME_FORTNITE, 420, 448);
    X4CoverRaster poster = load_surface(data, "BT5P2X999VH2-550x825", X4_COVER_FORTNITE_POSTER, 550, 770);
    x4_catalog_assets_compose_card(&featured);
    x4_catalog_assets_compose_card(&fortnite);
    context.fortnite_poster = &poster;
#else
    const char *mode = "antes";
    X4CoverRaster featured = load_surface(data, "9NKX70BBCDRN-524x295", X4_COVER_DETAIL, 524, 295);
    X4CoverRaster fortnite = load_surface(data, "BT5P2X999VH2-356x200", X4_COVER_GRID, 356, 200);
#endif
    context.featured_hero = &featured;
    context.fortnite_cover = &fortnite;
    x4_screen_set_home_context(&context);

    X4Screen screen = {.page = 0, .selected = 0, .signed_in = 1};
    X4Controller controller = {0};
    x4_screen_draw(&screen, &controller, frame);
#if X4_CAPTURE_NEW_SURFACES
    check_pixels("menu-forza", frame, &featured, 84, 304, 32, 32, 820, 208);
    check_pixels("menu-fortnite", frame, &fortnite, 966, 304, 32, 80, 388, 260);
#endif
    write_ppm(out, "menu", mode, frame);

    screen.page = 8;
    x4_fortnite_summary_draw(&screen, NULL, 0, frame);
#if X4_CAPTURE_NEW_SURFACES
    check_pixels("pantalla-fortnite", frame, &poster, 84, 170, 32, 32, 518, 738);
    free(poster.rgba);
#endif
    write_ppm(out, "fortnite", mode, frame);
    x4_screen_set_home_context(NULL);
    x4_font_close();
    free(featured.rgba);
    free(fortnite.rgba);
    free(frame);
    return 0;
}
