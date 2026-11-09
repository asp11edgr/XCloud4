/* SPDX-License-Identifier: GPL-3.0-only */
#include "src/ui/ui2_shell.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HERO_ROWS = 413 };

static void background(uint32_t *pixels, unsigned first, unsigned end, unsigned variant)
{
    for (unsigned y = first; y < end; ++y)
        for (unsigned x = 0; x < X4_UI_WIDTH; ++x)
            pixels[(size_t)y * X4_UI_WIDTH + x] = X4_RGB(110 + x / 32,
                115 + y / 18, 139 + 50 * variant);
}

static void assert_damage(const uint32_t *before, const uint32_t *after,
    const X4UiRect *damage, size_t count)
{
    unsigned changed = 0;
    for (int y = 0; y < X4_UI_HEIGHT; ++y) {
        for (int x = 0; x < X4_UI_WIDTH; ++x) {
            const size_t pos = (size_t)y * X4_UI_WIDTH + x;
            if (before[pos] == after[pos]) continue;
            bool covered = false;
            for (size_t i = 0; i < count; ++i) {
                const X4UiRect r = damage[i];
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                    covered = true;
            }
            assert(covered);
            ++changed;
        }
    }
    assert(changed);
}

int main(void)
{
    const size_t bytes = (size_t)X4_UI_WIDTH * X4_UI_HEIGHT * sizeof(uint32_t);
    uint32_t *retained = malloc(bytes);
    uint32_t *reference = malloc(bytes);
    assert(retained && reference);
    X4Ui2Shell shell = {.section = X4_UI2_DISCOVER, .selected = X4_UI2_DISCOVER};
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    background(retained, 0, X4_UI_HEIGHT, 0);
    memcpy(reference, retained, bytes);
    x4_ui2_shell_draw_sidebar_backdrop(retained, &shell, X4_UI_HEIGHT);
    const int edge = X4_UI2_SIDEBAR_W - 1;
    const size_t sample = 20u * X4_UI_WIDTH;
    assert(retained[sample] != reference[sample]);
    assert(retained[sample + edge - 25] != reference[sample + edge - 25]);
    assert(retained[sample + edge] == reference[sample + edge]);
    assert(retained[sample + edge + 1] == reference[sample + edge + 1]);

    background(retained, 0, HERO_ROWS, 1);
    x4_ui2_shell_draw_sidebar_backdrop(retained, &shell, HERO_ROWS);
    background(reference, 0, HERO_ROWS, 1);
    x4_ui2_shell_draw_sidebar_backdrop(reference, &shell, X4_UI_HEIGHT);
    assert(memcmp(retained, reference, bytes) == 0);

    X4Ui2Shell next = shell;
    next.focused = true;
    next.selected = X4_UI2_CATALOG;
    X4UiRect damage[4];
    const size_t count = x4_ui2_shell_draw_sidebar_delta(retained, &shell, &next, damage);
    assert(count == 1);
    assert_damage(reference, retained, damage, count);
    x4_ui2_shell_draw_sidebar(reference, &next);
    assert(memcmp(retained, reference, bytes) == 0);

    free(reference);
    free(retained);
    puts("T48 sidebar PASS integrated edge, partial backdrop, exact focus damage");
    return 0;
}
