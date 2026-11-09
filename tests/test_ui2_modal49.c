/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/ui2_modal.h"
#include "../src/ui/font.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool record;
static char drawn[256];
static int first_y, second_y, cross_x, cross_y, circle_x;
static unsigned crosses, circles;

int __real_x4_ui2_pad_glyph(uint32_t *, int, int, enum X4PadGlyph);
int __wrap_x4_ui2_pad_glyph(uint32_t *p, int x, int y, enum X4PadGlyph glyph)
{
    if (p && glyph == X4_PAD_CROSS) {
        ++crosses;
        cross_x = x;
        cross_y = y;
    }
    if (p && glyph == X4_PAD_CIRCLE) {
        ++circles;
        circle_x = x;
    }
    return __real_x4_ui2_pad_glyph(p, x, y, glyph);
}

void __real_x4_text_px(uint32_t *, int, int, int, int, const char *, uint32_t);
void __wrap_x4_text_px(uint32_t *p, int x, int y, int size, int weight,
    const char *text, uint32_t color)
{
    if (!strcmp(text, "First paragraph.")) first_y = y;
    if (!strcmp(text, "Second paragraph.")) second_y = y;
    if (record) {
        assert(strlen(text) < sizeof(drawn));
        strcpy(drawn, text);
    }
    __real_x4_text_px(p, x, y, size, weight, text, color);
}

static void fill(uint32_t *p)
{
    for (size_t i = 0; i < (size_t)X4_UI_WIDTH * X4_UI_HEIGHT; ++i)
        p[i] = 0xff000000u | ((uint32_t)i & 0x00ffffffu);
}

int main(void)
{
    const size_t bytes = (size_t)X4_UI_WIDTH * X4_UI_HEIGHT * sizeof(uint32_t);
    uint32_t *retained = malloc(bytes);
    uint32_t *fresh = malloc(bytes);
    assert(retained && fresh);
    assert(x4_font_init("assets/fonts/inter.x4font") == 0);
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");

    fill(retained);
    fill(fresh);
    x4_ui2_modal_scrim(retained);
    X4UpdateNotice update = {0};
    assert(x4_apple_update_notice_offer(&update, "9.9.9", "v9.9.9", true, true));
    x4_ui2_update_notice_draw(&update, retained);
    const X4UiRect large = x4_ui2_modal_damage();
    assert(large.h > 500);

    X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, "Sign out", false, false, false},
        {X4_PAD_CIRCLE, "Cancel", false, true, true},
    };
    crosses = circles = 0;
    x4_ui2_modal_message(retained, "Sign out", "First paragraph.", "Second paragraph.",
        X4_UI2_MUTED, actions, 2);
    assert(crosses == 1 && circles == 1 && cross_x + 40 >= circle_x && cross_x < circle_x);
    assert(second_y - (first_y + X4_UI2_LABEL) == 12);
    assert(cross_y - 16 - (second_y + X4_UI2_LABEL) >= 24);
    const X4UiRect damage = x4_ui2_modal_damage();
    assert(damage.x == large.x && damage.y == large.y && damage.h == large.h);

    crosses = circles = 0;
    actions[0].focused = true;
    actions[1].focused = false;
    x4_ui2_modal_message(retained, "Sign out", "First paragraph.", "Second paragraph.",
        X4_UI2_MUTED, actions, 2);
    assert(crosses == 1 && circles == 1 && circle_x - cross_x >= 300);
    const X4UiRect small = x4_ui2_modal_damage();
    assert(small.h == 300 && small.y > large.y);
    for (int y = 0; y < X4_UI_HEIGHT; ++y)
        for (int x = 0; x < X4_UI_WIDTH; ++x) {
            if (x >= small.x && x < small.x + small.w && y >= small.y && y < small.y + small.h) continue;
            size_t i = (size_t)y * X4_UI_WIDTH + x;
            assert(retained[i] == (0xff000000u | ((fresh[i] & 0x00fcfcfcu) >> 2)));
        }
    x4_ui2_modal_scrim(fresh);
    x4_ui2_modal_message(fresh, "Sign out", "First paragraph.", "Second paragraph.",
        X4_UI2_MUTED, actions, 2);
    assert(!memcmp(fresh, retained, bytes));

    fill(retained);
    fill(fresh);
    x4_ui2_modal_scrim(retained);
    X4AccessCountryUi access = {.selected = X4_ACCESS_BR, .help = true};
    x4_ui2_modal_draw_access(retained, &access, X4_ACCESS_CARD_SELECT);
    access.help = false;
    x4_ui2_modal_draw_access(retained, &access, X4_ACCESS_CARD_SELECT);
    x4_ui2_modal_scrim(fresh);
    x4_ui2_modal_draw_access(fresh, &access, X4_ACCESS_CARD_SELECT);
    assert(!memcmp(fresh, retained, bytes));

    x4_apple_access_card_use_overlay(true);
    x4_apple_draw_access_card(retained, 300, 200, 1000, 500);
    assert(!memcmp(fresh, retained, bytes));
    x4_apple_access_card_use_overlay(false);

    const int size = X4_UI2_BODY;
    const int width = x4_text_width_px(size, X4_FONT_REGULAR, "alpha beta");
    record = true;
    assert(x4_ui2_text_bounded(retained, 100, 100, size, X4_FONT_REGULAR,
        "alpha beta gamma", width, 36, 1, X4_UI2_TEXT) == 1);
    record = false;
    assert(!strcmp(drawn, "alpha…"));
    assert(x4_text_width_px(size, X4_FONT_REGULAR, drawn) <= width);

    free(fresh);
    free(retained);
    x4_font_close();
    puts("PASS modal49: retained backdrop, resize damage, 12px paragraphs, 24px action gap, cross follows focus, circle stays fixed, ellipsis");
    return 0;
}
