/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define X4_TOUCH_MENU_HOLD_US 1000000ULL      /* 1000 ms hold threshold */
#define X4_TOUCH_MENU_TIMEOUT_US 15000000ULL  /* 15 s inactivity timeout */
#define X4_TOUCH_NEXUS_PULSE_US 200000ULL     /* 200 ms Xbox nexus pulse */

#ifndef ORBIS_PAD_BUTTON_UP
#define ORBIS_PAD_BUTTON_UP 0x00000010u
#endif
#ifndef ORBIS_PAD_BUTTON_DOWN
#define ORBIS_PAD_BUTTON_DOWN 0x00000040u
#endif
#ifndef ORBIS_PAD_BUTTON_CROSS
#define ORBIS_PAD_BUTTON_CROSS 0x00004000u
#endif
#ifndef ORBIS_PAD_BUTTON_CIRCLE
#define ORBIS_PAD_BUTTON_CIRCLE 0x00002000u
#endif
#ifndef ORBIS_PAD_BUTTON_SQUARE
#define ORBIS_PAD_BUTTON_SQUARE 0x00008000u
#endif

typedef enum {
    X4_TOUCH_MENU_ACTION_NONE = 0,
    X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG,
    X4_TOUCH_MENU_ACTION_TOGGLE_MUTE,
    X4_TOUCH_MENU_ACTION_XBOX_BUTTON,
    X4_TOUCH_MENU_ACTION_CANCEL,
    X4_TOUCH_MENU_ACTION_MARK_TEST,
    X4_TOUCH_MENU_ACTION_EXIT_APP,
    X4_TOUCH_MENU_ACTION_RELOAD_STREAM,
    X4_TOUCH_MENU_ACTION_NET_HUD,
    X4_TOUCH_MENU_ACTION_TOGGLE_MIC,
} X4TouchMenuAction;

typedef struct {
    bool is_open;
    bool waiting_release;
    bool is_public;
    int selected_index;
    int item_count;
    uint32_t suppress_mask;
    uint64_t touch_start_us;
    uint64_t last_activity_us;
    uint64_t nexus_until_us;
    bool mic_enabled; /* Current launch snapshot, not the saved preference. */
} X4TouchMenu;

void x4_touch_menu_init(X4TouchMenu *menu, bool is_public);
bool x4_touch_menu_feed_touch(X4TouchMenu *menu, bool game_input, uint8_t fingers, uint64_t now_us);
X4TouchMenuAction x4_touch_menu_update(X4TouchMenu *menu, uint32_t pressed, uint32_t held, uint64_t now_us);
uint32_t x4_touch_menu_filter_buttons(X4TouchMenu *m, uint32_t buttons);
void x4_touch_menu_close(X4TouchMenu *menu);
bool x4_touch_menu_is_open(const X4TouchMenu *menu);
void x4_touch_menu_trigger_nexus(X4TouchMenu *menu, uint64_t now_us);
bool x4_touch_menu_is_nexus_active(const X4TouchMenu *menu, uint64_t now_us);
