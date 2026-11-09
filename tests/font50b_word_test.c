/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui/font.h"

static uint32_t pixels[1920 * 1080];

int main(int argc, char **argv)
{
    assert(argc == 2 && x4_font_init(argv[1]) == 0);
    char fitted[128];
    const char *words = "Niño 20763 action";
    const int whole_width = x4_text_width_px(28, 1, words);
    assert(x4_text_fit_utf8(fitted, sizeof(fitted), 28, 1, words, whole_width) == strlen(words));
    assert(!strcmp(fitted, words));
    x4_text_fit_utf8(fitted, sizeof(fitted), 28, 1, words, x4_text_width_px(28, 1, "Niño 207…"));
    assert(!strcmp(fitted, "Niño…"));
    x4_text_fit_utf8(fitted, sizeof(fitted), 28, 1, "étoile lumière", x4_text_width_px(28, 1, "étoile lum…"));
    assert(!strcmp(fitted, "étoile…"));
    x4_text_fit_utf8(fitted, 11, 28, 1, words, 1000);
    assert(!strcmp(fitted, "Niño…"));
    assert(x4_text_fit_utf8(fitted, sizeof(fitted), 28, 1, words,
        x4_text_width_px(28, 1, "Ni")) == 0 && !*fitted);

    const char *title = "München Licht";
    const int small_width = x4_text_width_px(15, 1, "München…");
    assert(!x4_text_fit_utf8(fitted, sizeof(fitted), 20, 1, title, small_width));
    assert(x4_text_fit_utf8(fitted, sizeof(fitted), 15, 1, title, small_width));
    assert(!strcmp(fitted, "München…"));
    x4_text_ellipsis_px(pixels, 0, INT_MAX, 20, 1, title, small_width, 0xffffffffu);
    x4_text_ellipsis_px(pixels, 0, 0, 20, 1, title, small_width, 0xffffffffu);
    unsigned visible = 0;
    for (unsigned i = 0; i < 1920u * 30u; ++i) visible += pixels[i] != 0;
    assert(visible);
    x4_font_close();
    puts("FONT50B_WORD_PASS full, UTF-8, number, capacity, narrow, title fallback");
    return 0;
}
