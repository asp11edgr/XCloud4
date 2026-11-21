/* SPDX-License-Identifier: GPL-3.0-only */
#include "src/ui/ui2_shell.h"
#include "src/ui/font.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void background(uint32_t *pixels, unsigned rows, unsigned variant)
{
    for (unsigned y = 0; y < rows; ++y)
        for (unsigned x = 0; x < X4_UI_WIDTH; ++x)
            pixels[(size_t)y * X4_UI_WIDTH + x] = X4_RGB(110 + x / 32,
                115 + y / 18, 139 + 50 * variant);
}

int main(void)
{
    const size_t bytes = (size_t)X4_UI_WIDTH * X4_UI_HEIGHT * sizeof(uint32_t);
    uint32_t *retained = malloc(bytes), *reference = malloc(bytes);
    assert(retained && reference && !x4_font_init("assets/fonts/inter.x4font"));
    for (enum X4Lang lang = X4_LANG_ES; lang < X4_LANG_COUNT; ++lang) {
        x4_lang_set_ui(lang);
        X4Ui2Shell shell;
        x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
        X4Ui2Status status = {0};
        assert(x4_ui2_status_set(&status, X4_UI2_NETWORK_WIFI, "1080p", 700, "Player 1"));
        background(retained, X4_UI_HEIGHT, 0);
        memcpy(reference, retained, bytes);
        x4_ui2_shell_draw(retained, &shell, &status);
        assert(retained[0] != reference[0]);
        assert(retained[X4_UI2_HEADER_H * X4_UI_WIDTH] == reference[X4_UI2_HEADER_H * X4_UI_WIDTH]);

        assert(!x4_ui2_shell_handle(&shell, X4_UI2_NAV_UP, false).consumed);
        assert(!x4_ui2_shell_handle(&shell, X4_UI2_NAV_LEFT, true).consumed);
        assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_UP, true).changed);
        for (unsigned i = 0; i < X4_UI2_SECTION_COUNT; ++i) {
            assert(shell.selected == (X4Ui2Section)i);
            const X4Ui2NavResult selected = x4_ui2_shell_handle(&shell, X4_UI2_NAV_SELECT, false);
            assert(selected.activate && selected.consumed && selected.target == (X4Ui2Section)i);
            x4_ui2_shell_draw_header(retained, &shell, &status, false);
            assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_RIGHT, false).changed);
        }
        assert(shell.selected == X4_UI2_HOME);
        assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_LEFT, false).changed);
        assert(shell.selected == X4_UI2_ACCOUNT);
        assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_CIRCLE, false).consumed && !shell.focused);
        assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_UP, true).changed);
        assert(x4_ui2_shell_handle(&shell, X4_UI2_NAV_DOWN, true).consumed && !shell.focused);
        for (unsigned tab = X4_UI2_HOME; tab <= X4_UI2_SETTINGS; ++tab) {
            const unsigned count = X4_UI2_SETTINGS + 1u;
            x4_ui2_shell_set_section(&shell, (X4Ui2Section)tab);
            X4Ui2NavResult next = x4_ui2_shell_handle(&shell, X4_UI2_NAV_NEXT, false);
            X4Ui2NavResult prev = x4_ui2_shell_handle(&shell, X4_UI2_NAV_PREVIOUS, false);
            assert(next.consumed && next.activate && (unsigned)next.target == (tab + 1u) % count);
            assert(prev.consumed && prev.activate && (unsigned)prev.target == (tab + count - 1u) % count);
        }
        x4_ui2_shell_set_section(&shell, X4_UI2_HOME);
        assert(x4_ui2_status_set(&status, X4_UI2_NETWORK_CABLE, "720p", 701, "Player 2"));
        x4_ui2_shell_draw_header(retained, &shell, &status, false);
        x4_ui2_shell_draw(reference, &shell, &status);
        assert(!memcmp(retained, reference, bytes));

        background(retained, X4_UI2_HEADER_H, 1);
        background(reference, X4_UI2_HEADER_H, 1);
        x4_ui2_shell_draw_header(retained, &shell, &status, true);
        x4_ui2_shell_draw(reference, &shell, &status);
        assert(!memcmp(retained, reference, bytes));
    }
    x4_font_close();
    free(reference);
    free(retained);
    puts("PASS Xbox-style header: every route, six languages, status and retained compositing");
    return 0;
}
