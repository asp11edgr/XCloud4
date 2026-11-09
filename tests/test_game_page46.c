/* SPDX-License-Identifier: GPL-3.0-only */
#include "src/ui/apple_ui.h"
#include "src/ui/game_page.h"
#include "third_party/qrcodegen/qrcodegen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1920, H = 1080 };

static const char *const ids[] = {"9NNX1VVR3KNQ", "BT5P2X999VH2", "9NP1P1WFS0LB"};
static const char *const urls[] = {
    "https://www.xbox.com/games/store/p/9NNX1VVR3KNQ",
    "https://www.xbox.com/games/store/p/BT5P2X999VH2",
    "https://www.xbox.com/games/store/p/9NP1P1WFS0LB",
};

static void test_play_and_navigation(void)
{
    X4GamePage page;
    X4GameDetails details = {.screenshot_count = 6};
    X4GamePageModel model = {
        .title = "Juego de prueba", .product_id = ids[0],
        .available = true, .entitled = true, .details = &details,
    };
    x4_game_page_init(&page);
    assert(page.focus == X4_GAME_FOCUS_PLAY);
    assert(x4_game_page_fullscreen_index(&page) == -1);
    assert(x4_game_page_update(&page, &model, 0) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_PLAY);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_RIGHT | X4_GAME_INPUT_SELECT) ==
        X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_PLAY);
    model.busy = true;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    model.busy = false;
    model.available = false;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    model.available = true;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_DOWN | X4_GAME_INPUT_SELECT) ==
        X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_SCREENSHOTS && !page.fullscreen_open);
    for (int i = 0; i < 8; ++i)
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_RIGHT) == X4_GAME_ACTION_NONE);
    assert(page.screenshot_index == 5);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page) == 5);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_BACK) == X4_GAME_ACTION_NONE);
    assert(!page.fullscreen_open && page.screenshot_index == 5 &&
        page.focus == X4_GAME_FOCUS_SCREENSHOTS);
    details.screenshot_count = 0;
    assert(x4_game_page_update(&page, &model, 0) == X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_PLAY && !page.screenshot_index);
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_BACK | X4_GAME_INPUT_SELECT) ==
        X4_GAME_ACTION_BACK);
    puts("PASS T46 game page: Play alone, safe simultaneous input, six shots, fullscreen and Back");
}

static void test_description_scroll(void)
{
    X4GamePage page;
    X4GameDetails details = {
        .description = "Primera línea\nSegunda línea\nTercera línea\nCuarta línea\n"
            "Quinta línea\nSexta línea\nSéptima línea\nOctava línea",
    };
    X4GamePageModel model = {.details = &details};
    x4_game_page_init(&page);
    for (int i = 0; i < 8; ++i)
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SCROLL_DOWN) == X4_GAME_ACTION_NONE);
    assert(page.description_scroll == 3);
    for (int i = 0; i < 8; ++i)
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SCROLL_UP) == X4_GAME_ACTION_NONE);
    assert(!page.description_scroll);

    /* No spaces, a truncated final multibyte sequence, and paragraphs. */
    memset(details.description, 'w', sizeof(details.description) - 1);
    details.description[sizeof(details.description) - 2] = (char)0xe2;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SCROLL_DOWN) == X4_GAME_ACTION_NONE);
    assert(page.description_scroll == 1);
    page.description_scroll = UINT32_MAX;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SCROLL_DOWN) == X4_GAME_ACTION_NONE);
    assert(page.description_scroll > 20);

    /* The maximum codepoint count can also consist almost entirely of
     * short paragraphs. A same-length replacement must rebuild layout. */
    const size_t paragraphs = sizeof(details.description) > 10000 ? 10000 : 4000;
    memset(details.description, '\n', paragraphs - 1);
    details.description[paragraphs - 1] = 'Z';
    details.description[paragraphs] = 0;
    page.description_scroll = UINT32_MAX;
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SCROLL_DOWN) == X4_GAME_ACTION_NONE);
    assert(page.description_scroll == paragraphs - 5);
    details.description[0] = 'a';
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SCROLL_DOWN) == X4_GAME_ACTION_NONE);
    assert(page.description_scroll == paragraphs - 6);
    puts("PASS T46 game page: complete description scroll, extreme size and exact layout invalidation");
}

static void check_qr_pixels(const uint32_t *p, const char *expected)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
    assert(qrcodegen_encodeText(expected, temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
        10, qrcodegen_Mask_AUTO, true));
    const int size = qrcodegen_getSize(qr), edge = 310;
    const int scale = edge / (size + 8);
    const int left = 1225 - edge / 2 + (edge - (size + 8) * scale) / 2;
    const int top = 484 - edge / 2 + (edge - (size + 8) * scale) / 2;
    for (int y = -4; y < size + 4; ++y) {
        for (int x = -4; x < size + 4; ++x) {
            const uint32_t module = qrcodegen_getModule(qr, x, y) ?
                X4_RGB(9, 15, 25) : X4_RGB(255, 255, 255);
            assert(p[(size_t)(top + (y + 4) * scale + scale / 2) * W +
                left + (x + 4) * scale + scale / 2] == module);
        }
    }
}

static void test_store(void)
{
    uint32_t *p = malloc((size_t)W * H * sizeof(*p));
    assert(p);
    X4GamePage page;
    X4GamePageModel model = {.title = "Juego de prueba", .available = true};
    char url[X4_GAME_STORE_URL_CAP];
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        model.product_id = ids[i];
        model.free_in_store = i == 1;
        assert(x4_game_page_store_url(ids[i], url, sizeof(url)) && !strcmp(url, urls[i]));
        x4_game_page_init(&page);
        x4_lang_set(i == 1 ? X4_LANG_EN : X4_LANG_ES, i == 1 ? "en-US" : "es-MX",
            i == 1 ? "US" : "MX");
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_RIGHT) == X4_GAME_ACTION_NONE);
        assert(page.focus == X4_GAME_FOCUS_STORE);
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
        assert(page.qr_open);
        x4_game_page_draw(&page, &model, p);
        check_qr_pixels(p, urls[i]);
        model.busy = true;
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_NONE);
        model.busy = false;
        assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_SELECT) == X4_GAME_ACTION_REFRESH);
        assert(!page.qr_open && page.focus == X4_GAME_FOCUS_STORE);
    }
    model.entitled = true;
    assert(x4_game_page_update(&page, &model, 0) == X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_PLAY);
    model.entitled = false;
    model.product_id = "9NNX1VVR3KNQ/redirect";
    assert(x4_game_page_update(&page, &model, X4_GAME_INPUT_RIGHT) == X4_GAME_ACTION_NONE);
    assert(page.focus == X4_GAME_FOCUS_PLAY);
    assert(!x4_game_page_store_url(model.product_id, url, sizeof(url)) && !url[0]);
    assert(!x4_game_page_store_url(ids[0], url, 2) && !url[0]);
    assert(!x4_game_page_store_url(NULL, url, sizeof(url)) && !url[0]);
    free(p);
    puts("PASS T46 game page: entitlement actions and exact QR pixels for three public product URLs");
}

int main(void)
{
    test_play_and_navigation();
    test_description_scroll();
    test_store();
    return 0;
}
