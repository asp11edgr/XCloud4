/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
#include "touch_menu.h"
#include <string.h>

void x4_touch_menu_init(X4TouchMenu *menu, bool is_public)
{
    memset(menu, 0, sizeof(*menu));
    menu->is_public = is_public;
    menu->item_count = is_public ? 6 : 8;
}

bool x4_touch_menu_feed_touch(X4TouchMenu *menu, bool game_input, uint8_t fingers, uint64_t now_us)
{
    if (!game_input) {
        menu->touch_start_us = 0;
        menu->waiting_release = false;
        return false;
    }

    if (fingers < 2) {
        menu->touch_start_us = 0;
        menu->waiting_release = false;
        return false;
    }

    /* fingers >= 2 */
    if (menu->waiting_release || menu->is_open) {
        return false;
    }

    if (menu->touch_start_us == 0) {
        menu->touch_start_us = now_us;
        return false;
    }

    if (now_us - menu->touch_start_us >= X4_TOUCH_MENU_HOLD_US) {
        menu->is_open = true;
        menu->waiting_release = true;
        menu->selected_index = 0;
        menu->last_activity_us = now_us;
        menu->touch_start_us = 0;
        return true;
    }

    return false;
}

X4TouchMenuAction x4_touch_menu_update(X4TouchMenu *menu, uint32_t pressed, uint32_t held, uint64_t now_us)
{
    if (!menu->is_open) {
        return X4_TOUCH_MENU_ACTION_NONE;
    }

    /* 15-second inactivity timeout */
    if (now_us >= menu->last_activity_us &&
        now_us - menu->last_activity_us >= X4_TOUCH_MENU_TIMEOUT_US) {
        menu->is_open = false;
        menu->suppress_mask = held;
        return X4_TOUCH_MENU_ACTION_NONE;
    }

    if (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN)) {
        menu->last_activity_us = now_us;
        if (pressed & ORBIS_PAD_BUTTON_UP) {
            menu->selected_index = (menu->selected_index - 1 + menu->item_count) % menu->item_count;
        }
        if (pressed & ORBIS_PAD_BUTTON_DOWN) {
            menu->selected_index = (menu->selected_index + 1) % menu->item_count;
        }
    }

    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
        menu->is_open = false;
        menu->suppress_mask = held;
        return X4_TOUCH_MENU_ACTION_CANCEL;
    }

    if (menu->mic_enabled && (pressed & ORBIS_PAD_BUTTON_SQUARE)) {
        menu->last_activity_us = now_us;
        menu->suppress_mask = held;
        return X4_TOUCH_MENU_ACTION_TOGGLE_MIC;
    }

    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
        int idx = menu->selected_index;
        if (idx == 4) {
            menu->last_activity_us = now_us;
            menu->suppress_mask = held;
            return X4_TOUCH_MENU_ACTION_NET_HUD;
        }
        menu->is_open = false;
        menu->suppress_mask = held;
        if (menu->is_public) {
            switch (idx) {
            case 0: return X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG;
            case 1: return X4_TOUCH_MENU_ACTION_TOGGLE_MUTE;
            case 2: return X4_TOUCH_MENU_ACTION_XBOX_BUTTON;
            case 3: return X4_TOUCH_MENU_ACTION_RELOAD_STREAM;
            case 5: return X4_TOUCH_MENU_ACTION_CANCEL;
            default: return X4_TOUCH_MENU_ACTION_NONE;
            }
        } else {
            switch (idx) {
            case 0: return X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG;
            case 1: return X4_TOUCH_MENU_ACTION_TOGGLE_MUTE;
            case 2: return X4_TOUCH_MENU_ACTION_XBOX_BUTTON;
            case 3: return X4_TOUCH_MENU_ACTION_RELOAD_STREAM;
            case 5: return X4_TOUCH_MENU_ACTION_MARK_TEST;
            case 6: return X4_TOUCH_MENU_ACTION_EXIT_APP;
            case 7: return X4_TOUCH_MENU_ACTION_CANCEL;
            default: return X4_TOUCH_MENU_ACTION_NONE;
            }
        }
    }

    return X4_TOUCH_MENU_ACTION_NONE;
}

uint32_t x4_touch_menu_filter_buttons(X4TouchMenu *m, uint32_t buttons)
{
    m->suppress_mask &= buttons;
    return buttons & ~m->suppress_mask;
}

void x4_touch_menu_close(X4TouchMenu *menu)
{
    menu->is_open = false;
    menu->suppress_mask = 0;
}

bool x4_touch_menu_is_open(const X4TouchMenu *menu)
{
    return menu->is_open;
}

void x4_touch_menu_trigger_nexus(X4TouchMenu *menu, uint64_t now_us)
{
    menu->nexus_until_us = now_us + X4_TOUCH_NEXUS_PULSE_US;
}

bool x4_touch_menu_is_nexus_active(const X4TouchMenu *menu, uint64_t now_us)
{
    return menu->nexus_until_us != 0 && now_us < menu->nexus_until_us;
}
