/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/input/touch_menu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(s) do { if (!(s)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #s); exit(1); } } while (0)

static void test_touch_threshold(void)
{
    X4TouchMenu menu;
    x4_touch_menu_init(&menu, true);

    /* 1. Hold 2 fingers for 999 ms: must NOT open */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 1000000ULL));
    CHECK(!menu.is_open);
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 1500000ULL));
    CHECK(!menu.is_open);
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 1999999ULL));
    CHECK(!menu.is_open);

    /* Reach 1000 ms: opens! */
    CHECK(x4_touch_menu_feed_touch(&menu, true, 2, 2000000ULL));
    CHECK(menu.is_open);
    CHECK(x4_touch_menu_is_open(&menu));

    /* Extra time while fingers still on pad does not trigger re-open */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 2500000ULL));
    CHECK(menu.is_open);
}

static void test_one_finger_does_not_open(void)
{
    X4TouchMenu menu;
    x4_touch_menu_init(&menu, true);

    /* 1 finger held for multiple seconds: must NOT open */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 1, 1000000ULL));
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 1, 2500000ULL));
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 1, 5000000ULL));
    CHECK(!menu.is_open);

    /* Transition from 1 finger to 0 fingers */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 0, 5100000ULL));
    CHECK(!menu.is_open);
}

static void test_reopening_only_after_lifting(void)
{
    X4TouchMenu menu;
    x4_touch_menu_init(&menu, true);

    /* Open with 2 fingers for 1 s */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 1000000ULL));
    CHECK(x4_touch_menu_feed_touch(&menu, true, 2, 2000000ULL));
    CHECK(menu.is_open);

    /* User closes the menu via CIRCLE while 2 fingers are STILL touching */
    X4TouchMenuAction act = x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_CIRCLE, 2100000ULL);
    CHECK(act == X4_TOUCH_MENU_ACTION_CANCEL);
    CHECK(!menu.is_open);

    /* Fingers still touching (2 fingers) -> MUST NOT reopen without lifting */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 2200000ULL));
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 3500000ULL));
    CHECK(!menu.is_open);

    /* Lift fingers (0 fingers) */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 0, 3600000ULL));
    CHECK(!menu.is_open);

    /* Now touch again with 2 fingers for 1 s -> MUST reopen */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 4000000ULL));
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 4999999ULL));
    CHECK(!menu.is_open);
    CHECK(x4_touch_menu_feed_touch(&menu, true, 2, 5000000ULL));
    CHECK(menu.is_open);
}

static void test_game_input_gate(void)
{
    X4TouchMenu menu;
    x4_touch_menu_init(&menu, true);

    /* 2 fingers held while game_input is FALSE -> must NOT open */
    CHECK(!x4_touch_menu_feed_touch(&menu, false, 2, 1000000ULL));
    CHECK(!x4_touch_menu_feed_touch(&menu, false, 2, 3000000ULL));
    CHECK(!menu.is_open);
}

static void test_menu_navigation_and_timeout(void)
{
    X4TouchMenu menu;
    x4_touch_menu_init(&menu, true); /* Public: 5 items */

    /* Open */
    CHECK(!x4_touch_menu_feed_touch(&menu, true, 2, 1000000ULL));
    CHECK(x4_touch_menu_feed_touch(&menu, true, 2, 2000000ULL));
    CHECK(menu.is_open);
    CHECK(menu.selected_index == 0);

    /* Down arrow */
    CHECK(x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_DOWN, 2100000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(menu.selected_index == 1);
    CHECK(x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_DOWN, 2200000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(menu.selected_index == 2);
    CHECK(x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_DOWN, 2300000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(menu.selected_index == 3);
    CHECK(x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_DOWN, 2400000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(menu.selected_index == 4);
    /* Wrap around after the fifth public row. */
    CHECK(x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_DOWN, 2450000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(menu.selected_index == 0);

    /* Up arrow wrap around */
    CHECK(x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_UP, 2500000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(menu.selected_index == 4);

    /* Inactivity timeout (15 s) */
    CHECK(x4_touch_menu_update(&menu, 0, 0, 2500000ULL + 15000000ULL) == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(!menu.is_open);
}

static void test_actions_public_and_lab(void)
{
    /* Public: item 0 -> BACK, item 1 -> MUTE, item 2 -> XBOX, item 3 -> CANCEL */
    X4TouchMenu pub;
    x4_touch_menu_init(&pub, true);
    pub.is_open = true;
    pub.last_activity_us = 1000000ULL;
    pub.selected_index = 0;
    CHECK(x4_touch_menu_update(&pub, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG);

    pub.is_open = true;
    pub.last_activity_us = 1000000ULL;
    pub.selected_index = 1;
    CHECK(x4_touch_menu_update(&pub, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_TOGGLE_MUTE);

    pub.is_open = true;
    pub.last_activity_us = 1000000ULL;
    pub.selected_index = 2;
    CHECK(x4_touch_menu_update(&pub, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_XBOX_BUTTON);

    pub.is_open = true;
    pub.last_activity_us = 1000000ULL;
    pub.selected_index = 4;
    CHECK(x4_touch_menu_update(&pub, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_CANCEL);

    /* Lab: item 3 -> MARK_TEST, item 4 -> EXIT_APP, item 5 -> CANCEL */
    X4TouchMenu lab;
    x4_touch_menu_init(&lab, false);
    lab.is_open = true;
    lab.last_activity_us = 1000000ULL;
    lab.selected_index = 4;
    CHECK(x4_touch_menu_update(&lab, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_MARK_TEST);

    lab.is_open = true;
    lab.last_activity_us = 1000000ULL;
    lab.selected_index = 5;
    CHECK(x4_touch_menu_update(&lab, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_EXIT_APP);

    lab.is_open = true;
    lab.last_activity_us = 1000000ULL;
    lab.selected_index = 6;
    CHECK(x4_touch_menu_update(&lab, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL) == X4_TOUCH_MENU_ACTION_CANCEL);
}

static void test_reload_closes_and_suppresses(void)
{
    for(int public=0;public<2;++public) {
        X4TouchMenu menu;x4_touch_menu_init(&menu,public!=0);
        CHECK(menu.item_count==(public?5:7));
        menu.is_open=true;menu.last_activity_us=1000000;menu.selected_index=3;
        CHECK(x4_touch_menu_update(&menu,ORBIS_PAD_BUTTON_CROSS,ORBIS_PAD_BUTTON_CROSS,1100000)==X4_TOUCH_MENU_ACTION_RELOAD_STREAM);
        CHECK(!menu.is_open);
        CHECK(x4_touch_menu_filter_buttons(&menu,ORBIS_PAD_BUTTON_CROSS)==0);
    }
}

static void test_button_suppression(void)
{
    X4TouchMenu menu;

    /* 1. Close with CROSS while holding CROSS */
    x4_touch_menu_init(&menu, true);
    menu.is_open = true;
    menu.last_activity_us = 1000000ULL;
    menu.selected_index = 2; /* Xbox button */
    X4TouchMenuAction act = x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS, 1100000ULL);
    CHECK(act == X4_TOUCH_MENU_ACTION_XBOX_BUTTON);
    CHECK(!menu.is_open);

    /* Still holding CROSS: filter must suppress CROSS */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS) == 0);

    /* A new button (SQUARE) is pressed at the same time: SQUARE passes */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_SQUARE) == ORBIS_PAD_BUTTON_SQUARE);

    /* Release CROSS (no buttons held) */
    CHECK(x4_touch_menu_filter_buttons(&menu, 0) == 0);

    /* Press CROSS again: passes! */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS) == ORBIS_PAD_BUTTON_CROSS);

    /* 2. Same with CIRCLE */
    x4_touch_menu_init(&menu, true);
    menu.is_open = true;
    menu.last_activity_us = 1000000ULL;
    act = x4_touch_menu_update(&menu, ORBIS_PAD_BUTTON_CIRCLE, ORBIS_PAD_BUTTON_CIRCLE, 1100000ULL);
    CHECK(act == X4_TOUCH_MENU_ACTION_CANCEL);
    CHECK(!menu.is_open);

    /* Still holding CIRCLE: filter suppresses CIRCLE */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CIRCLE) == 0);

    /* SQUARE passes */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_SQUARE) == ORBIS_PAD_BUTTON_SQUARE);

    /* Release CIRCLE */
    CHECK(x4_touch_menu_filter_buttons(&menu, 0) == 0);

    /* Press CIRCLE again: passes */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CIRCLE) == ORBIS_PAD_BUTTON_CIRCLE);

    /* 3. Inactivity timeout (15 s) with button held */
    x4_touch_menu_init(&menu, true);
    menu.is_open = true;
    menu.last_activity_us = 1000000ULL;
    act = x4_touch_menu_update(&menu, 0, ORBIS_PAD_BUTTON_CROSS, 1000000ULL + X4_TOUCH_MENU_TIMEOUT_US);
    CHECK(act == X4_TOUCH_MENU_ACTION_NONE);
    CHECK(!menu.is_open);

    /* CROSS was held when timeout occurred: suppressed */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS) == 0);

    /* SQUARE passes */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_SQUARE) == ORBIS_PAD_BUTTON_SQUARE);

    /* Release CROSS */
    CHECK(x4_touch_menu_filter_buttons(&menu, 0) == 0);

    /* Press CROSS again: passes */
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS) == ORBIS_PAD_BUTTON_CROSS);

    /* 4. x4_touch_menu_close() clears suppress_mask */
    x4_touch_menu_init(&menu, true);
    menu.suppress_mask = ORBIS_PAD_BUTTON_CROSS;
    x4_touch_menu_close(&menu);
    CHECK(menu.suppress_mask == 0);
    CHECK(x4_touch_menu_filter_buttons(&menu, ORBIS_PAD_BUTTON_CROSS) == ORBIS_PAD_BUTTON_CROSS);
}

static void test_nexus_pulse(void)
{
    X4TouchMenu menu;
    x4_touch_menu_init(&menu, true);

    CHECK(!x4_touch_menu_is_nexus_active(&menu, 1000000ULL));
    x4_touch_menu_trigger_nexus(&menu, 1000000ULL);

    /* Active for 200 ms */
    CHECK(x4_touch_menu_is_nexus_active(&menu, 1000000ULL));
    CHECK(x4_touch_menu_is_nexus_active(&menu, 1100000ULL));
    CHECK(x4_touch_menu_is_nexus_active(&menu, 1199999ULL));
    CHECK(!x4_touch_menu_is_nexus_active(&menu, 1200000ULL));
    CHECK(!x4_touch_menu_is_nexus_active(&menu, 1500000ULL));
}

int main(void)
{
    test_reload_closes_and_suppresses();
    test_touch_threshold();
    test_one_finger_does_not_open();
    test_reopening_only_after_lifting();
    test_game_input_gate();
    test_menu_navigation_and_timeout();
    test_actions_public_and_lab();
    test_nexus_pulse();
    test_button_suppression();

    printf("PASS test_touch_menu\n");
    return 0;
}
