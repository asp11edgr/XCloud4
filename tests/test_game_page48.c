/* SPDX-License-Identifier: GPL-3.0-only */
#include "src/ui/favorites.h"
#include "src/ui/font.h"
#include "src/ui/game_page.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static X4Favorites saved, restored;

static void favorites_cases(const char *path)
{
    assert(!x4_favorites_load(&saved, path) && !saved.count);
    assert(x4_favorites_toggle(&saved, "abc123", "Juego de prueba", path) == X4_FAVORITES_ADDED);
    assert(x4_favorites_contains(&saved, "ABC123"));
    assert(x4_favorites_load(&restored, path) && restored.count == 1);
    assert(!strcmp(restored.items[0].name, "Juego de prueba"));
    assert(x4_favorites_toggle(&restored, "ABC123", NULL, path) == X4_FAVORITES_REMOVED);
    assert(x4_favorites_load(&saved, path) && !saved.count);
    for (unsigned i = 0; i < X4_FAVORITES_MAX; ++i) {
        char id[16];
        assert(snprintf(id, sizeof(id), "PRODUCT%03u", i) > 0);
        assert(x4_favorites_toggle(&saved, id, "Jugador", NULL) == X4_FAVORITES_ADDED);
    }
    assert(saved.count == X4_FAVORITES_MAX);
    assert(x4_favorites_toggle(&saved, "OVERLIMIT", "Jugador", path) == X4_FAVORITES_FULL);
    assert(x4_favorites_toggle(&saved, "PRODUCT199", NULL, path) == X4_FAVORITES_REMOVED);
    assert(x4_favorites_load(&restored, path) && restored.count == X4_FAVORITES_MAX - 1);
    assert(x4_favorites_toggle(&saved, "NUEVO", "Jugador", "missing-directory/test.bin") == X4_FAVORITES_ERROR);
    assert(!x4_favorites_contains(&saved, "NUEVO") && saved.count == restored.count);
    assert(x4_favorites_toggle(&saved, "PRODUCT001", NULL, "missing-directory/test.bin") == X4_FAVORITES_ERROR);
    assert(x4_favorites_contains(&saved, "PRODUCT001") && saved.count == restored.count);

    FILE *file = fopen(path, "r+b");
    assert(file && !fseek(file, 14 + X4_PRODUCT_ID_CAP, SEEK_SET));
    assert(fputc('Z', file) == 'Z' && !fclose(file));
    assert(!x4_favorites_load(&restored, path) && !restored.count);
    assert(x4_favorites_toggle(&restored, "NUEVO", "Jugador", path) == X4_FAVORITES_ADDED);
    assert(x4_favorites_load(&saved, path) && saved.count == 1);
    assert(!strcmp(saved.items[0].name, "Jugador"));
    puts("PASS favoritos: guardar/cargar/quitar, límite 200, corrupción y reversión ante fallo");
}

static void metadata_cases(void)
{
    static const char json[] =
        "{\"Products\":[{\"ProductId\":\"TESTABC\",\"Properties\":{\"Category\":\"Racing & flying\","
        "\"Subcategory\":\"Open World\",\"Attributes\":[{\"Name\":\"XblOnlineCoop\"},{\"Name\":\"XblOnlineMultiPlayer\"},"
        "{\"Name\":\"CapabilityKeyboardMouse\"},{\"Name\":\"XblSinglePlayer\"},{\"Name\":\"Unknown\"}]},"
        "\"MarketProperties\":[{\"OriginalReleaseDate\":\"2021-11-09T06:00:00.0000000Z\","
        "\"ContentRatings\":[{\"RatingSystem\":\"PEGI\",\"RatingId\":\"PEGI:3\"},"
        "{\"RatingSystem\":\"ESRB\",\"RatingId\":\"ESRB:E\"}]}],"
        "\"LocalizedProperties\":[{\"ProductTitle\":\"Juego\",\"PublisherName\":\"Editor\","
        "\"DeveloperName\":\"Estudio\",\"ProductDescription\":\"Texto de prueba.\"}]}]}";
    static X4GameProductDetails game;
    X4ProductMetadata basic, without_details;
    assert(!x4_product_metadata_parse_details(json, sizeof(json) - 1, "testabc", &basic, &game));
    assert(!strcmp(game.details.category, "Racing & flying"));
    assert(!strcmp(game.details.subcategory, "Open World"));
    assert(!strcmp(game.details.publisher, "Editor") && !strcmp(game.details.developer, "Estudio"));
    assert(!strcmp(game.details.year, "2021") && !strcmp(game.details.release_date, "2021-11-09"));
    assert(!strcmp(game.details.age_rating, "ESRB:E"));
    assert(game.details.attributes == (X4_GAME_ATTR_ONLINE_MULTIPLAYER | X4_GAME_ATTR_COOP |
        X4_GAME_ATTR_KEYBOARD_MOUSE | X4_GAME_ATTR_SINGLE_PLAYER));
    assert(!x4_product_metadata_parse(json, sizeof(json) - 1, "testabc", &without_details));
    assert(!memcmp(&basic, &without_details, sizeof(basic)));
    puts("PASS metadatos existentes: género, clasificación, fecha y atributos sin cambio del registro básico");
}

static void action(X4GamePage *page, const X4GamePageModel *model, unsigned buttons, enum X4GamePageAction wanted)
{
    assert(x4_game_page_update(page, model, buttons) == wanted);
}

static void navigation_cases(void)
{
    X4GamePageSimilar similar[] = {{3, "Tercero", NULL}, {9, "Noveno", NULL}};
    static X4GameDetails details;
    details.screenshot_count = 2;
    X4GamePageModel model = {.product_id = "TESTABC", .title = "Jugador", .available = true,
        .details = &details, .similar = similar, .similar_count = 2};
    X4GamePage page;
    x4_game_page_init(&page);
    assert(page.focus == X4_GAME_FOCUS_PLAY && page.tab == X4_GAME_TAB_SCREENSHOTS);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_PLAY);
    action(&page, &model, X4_GAME_INPUT_RIGHT | X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_FAVORITE);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_FAVORITE);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_STORE);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(page.qr_open);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_REFRESH);
    assert(!page.qr_open);

    action(&page, &model, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_TABS);
    action(&page, &model, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 1);
    action(&page, &model, X4_GAME_INPUT_BACK, X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == -1);
    action(&page, &model, X4_GAME_INPUT_NEXT_TAB, X4_GAME_ACTION_NONE);
    assert(page.tab == X4_GAME_TAB_DETAILS && page.focus == X4_GAME_FOCUS_TABS);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    assert(page.tab == X4_GAME_TAB_SIMILAR);
    action(&page, &model, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_SIMILAR);
    assert(similar[page.similar_index].source_index == 9);
    action(&page, &model, X4_GAME_INPUT_BACK | X4_GAME_INPUT_SELECT, X4_GAME_ACTION_BACK);
    puts("PASS ficha: pestañas, Capturas, QR, Favorito, Similares y Jugar exclusivo");
}

static uint32_t retained[1920 * 1080], direct[1920 * 1080], art_pixels[32 * 18];

static size_t draw_compare(X4GamePage *page, X4GamePageModel *model, bool expect_partial)
{
    static unsigned frame;
    ++frame;
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX];
    size_t count = x4_game_page_draw_cached(page, model, 1, retained, true, damage);
    if (expect_partial) assert(count && count <= X4_GAME_PAGE_DAMAGE_MAX);
    x4_game_page_draw(page, model, direct);
    if (memcmp(retained, direct, sizeof(retained))) {
        size_t at = 0;
        while (retained[at] == direct[at]) ++at;
        fprintf(stderr, "Ficha frame=%u foco=%d tab=%d daño=%zu píxel=(%zu,%zu) retenido=%08x directo=%08x\n",
            frame, (int)page->focus, (int)page->tab, count, at % 1920, at / 1920,
            (unsigned)retained[at], (unsigned)direct[at]);
        if (count <= X4_GAME_PAGE_DAMAGE_MAX) {
            for (size_t i = 0; i < count; ++i)
                fprintf(stderr, "  daño[%zu]=%d,%d %dx%d\n",
                    i, damage[i].x, damage[i].y, damage[i].w, damage[i].h);
        }
        assert(!"Ficha retenida diferente al dibujo completo");
    }
    return count;
}

static void damage_cases(void)
{
    (void)x4_font_init("assets/fonts");
    X4CoverRaster art = {32, 18, art_pixels};
    for (size_t i = 0; i < sizeof(art_pixels) / sizeof(*art_pixels); ++i)
        art_pixels[i] = UINT32_C(0xffeeeeee) - (uint32_t)(i % 32);
    static X4GameDetails details;
    details.screenshot_count = 2;
    details.attributes = X4_GAME_ATTR_ONLINE_MULTIPLAYER | X4_GAME_ATTR_COOP;
    strcpy(details.description, "Linea de prueba para un detalle más largo.\nUna segunda línea.\n"
        "Tercera.\nCuarta.\nQuinta.\nSexta.\nSeptima.\nOctava.");
    X4GamePageSimilar similar[] = {{1, "Otro juego", &art}, {2, "Segundo juego", &art}};
    X4GamePageModel model = {.product_id = "TESTABC", .title = "Jugador", .available = true,
        .details = &details, .screenshots = {&art, &art}, .background = &art,
        .similar = similar, .similar_count = 2, .notice = "Aviso de prueba"};
    X4GamePage page;
    x4_game_page_init(&page);
    draw_compare(&page, &model, false);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    assert(draw_compare(&page, &model, true) == 2);
    model.favorite = true;
    draw_compare(&page, &model, true);
    model.last_session = "Ayer";
    draw_compare(&page, &model, true);
    model.played_time = "4 h";
    draw_compare(&page, &model, true);
    model.last_session = NULL;
    draw_compare(&page, &model, true);
    model.played_time = NULL;
    draw_compare(&page, &model, true);
    strcpy(details.publisher, "Editor de prueba");
    draw_compare(&page, &model, true);
    strcpy(details.title, "Un título de juego más largo para verificar que el editor y las insignias"
        " se desplazan cuando hay una segunda línea");
    draw_compare(&page, &model, true);
    model.last_session = "Ayer";
    model.played_time = "4 h";
    draw_compare(&page, &model, true);
    strcpy(details.title, "Juego");
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, false);
    action(&page, &model, X4_GAME_INPUT_BACK, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, false);
    action(&page, &model, X4_GAME_INPUT_NEXT_TAB, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_SCROLL_DOWN, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_NEXT_TAB, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    action(&page, &model, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, true);
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX];
    assert(x4_game_page_draw_cached(&page, &model, 1, retained, true, damage) == 0);
    x4_game_page_cache_dispose();
    x4_font_close();
    puts("PASS ficha: píxeles idénticos a composición completa con foco, pestañas, historial, favoritos y pantalla completa");
}

static void screenshot_order_cases(void)
{
    /* Reduced color pattern of the Forza comparison card. The letters mark
     * light neutral, dark neutral and saturated cells of its sample grid. */
    static const char chart_grid[8][13] = {
        "ssnnnnnnwwww", "nnnssnnssnnw", "wwnssssswsnw", "wwwssssssssw",
        "wsswwwwwwwww", "wsnwwwwwwwww", "nsnnnnnnnnnn", "nnnnnnnnnnss",
    };
    enum { W = 64, H = 36 };
    static uint32_t chart_pixels[W * H], game_pixels[W * H], sky_pixels[W * H];
    X4CoverRaster chart = {W, H, chart_pixels}, gameplay = {W, H, game_pixels}, sky = {W, H, sky_pixels};
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < W; ++x) {
            const unsigned i = y * W + x;
            const char sample = chart_grid[y * 8 / H][x * 12 / W];
            chart_pixels[i] = sample == 'w' ? UINT32_C(0xffc5c6c8) :
                sample == 'n' ? UINT32_C(0xff343434) : UINT32_C(0xff732230);
            game_pixels[i] = i % 2 ? UINT32_C(0xff251bb0) : UINT32_C(0xffc33240);
            sky_pixels[i] = y < H / 2 ? UINT32_C(0xfff0f0f0) : UINT32_C(0xff343434);
        }
    }
    static X4GameDetails details;
    details.screenshot_count = 3;
    X4GamePageModel model = {.product_id = "ORDER", .title = "Prueba", .available = true,
        .details = &details, .screenshots = {&chart, &gameplay, &sky}};
    X4GamePage page;
    x4_game_page_init(&page);
    draw_compare(&page, &model, false);
    /* Input sees metadata only. The immutable classification from drawing
     * determines the visible order and opening a shot retains its source ID. */
    X4GamePageModel input = model;
    memset(input.screenshots, 0, sizeof(input.screenshots));
    action(&page, &input, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    action(&page, &input, X4_GAME_INPUT_DOWN, X4_GAME_ACTION_NONE);
    action(&page, &input, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 1);
    draw_compare(&page, &model, false);
    action(&page, &input, X4_GAME_INPUT_BACK, X4_GAME_ACTION_NONE);
    action(&page, &input, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    action(&page, &input, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 2);
    action(&page, &input, X4_GAME_INPUT_BACK, X4_GAME_ACTION_NONE);
    action(&page, &input, X4_GAME_INPUT_RIGHT, X4_GAME_ACTION_NONE);
    action(&page, &input, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 0);
    draw_compare(&page, &model, false);
    action(&page, &input, X4_GAME_INPUT_BACK, X4_GAME_ACTION_NONE);
    draw_compare(&page, &model, false);
    /* When the selected thumbnail arrives or changes category, keep the
     * source index and restore both affected regions. */
    model.screenshots[0] = &gameplay;
    draw_compare(&page, &model, true);
    action(&page, &input, X4_GAME_INPUT_SELECT, X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 0);
    x4_game_page_cache_dispose();
    puts("PASS ficha: capturas de juego primero, orden estable e índice original para pantalla completa");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    favorites_cases(argv[1]);
    metadata_cases();
    navigation_cases();
    damage_cases();
    screenshot_order_cases();
    return 0;
}
