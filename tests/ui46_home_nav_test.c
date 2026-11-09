/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/screen.h"
#include <assert.h>
#include <stdio.h>

static void press(X4Screen *screen, uint32_t button)
{
    x4_screen_update(screen, button, 0);
}

int main(void)
{
    X4Screen screen = {.signed_in = true};
    press(&screen, ORBIS_PAD_BUTTON_LEFT);
    assert(screen.selected == 2);
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 0 && screen.open_recent_requested);
    screen.open_recent_requested = 0;
    screen.signed_in = false;
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 4 && !screen.open_recent_requested);
    press(&screen, ORBIS_PAD_BUTTON_CIRCLE);
    assert(screen.page == 0 && screen.selected == 2);

    press(&screen, ORBIS_PAD_BUTTON_UP);
    assert(screen.selected == 7);
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 0 && screen.open_settings_requested);
    screen.open_settings_requested = 0;
    press(&screen, ORBIS_PAD_BUTTON_LEFT);
    assert(screen.selected == 6);
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 4 && !screen.open_settings_requested);
    press(&screen, ORBIS_PAD_BUTTON_CIRCLE);

    screen.selected = 0;
    press(&screen, ORBIS_PAD_BUTTON_DOWN);
    assert(screen.selected == 4);
    press(&screen, ORBIS_PAD_BUTTON_RIGHT);
    assert(screen.selected == 5);
    press(&screen, ORBIS_PAD_BUTTON_RIGHT);
    assert(screen.selected == 4);
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 2 && !screen.open_settings_requested);
    press(&screen, ORBIS_PAD_BUTTON_CIRCLE);
    press(&screen, ORBIS_PAD_BUTTON_LEFT);
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 7 && !screen.open_settings_requested);
    press(&screen, ORBIS_PAD_BUTTON_CIRCLE);

    screen.selected = 1;
    press(&screen, ORBIS_PAD_BUTTON_CROSS);
    assert(screen.page == 8 && !screen.open_recent_requested);
    press(&screen, ORBIS_PAD_BUTTON_CIRCLE);
    assert(screen.page == 0 && screen.selected == 1);

    puts("PASS T46 home navigation: Continue/Account, top gear, two bottom pills, Fortnite route and retained selection");
    return 0;
}
