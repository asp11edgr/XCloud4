/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../src/ui/server_settings.h"
#include "../src/ui/picture_settings.h"
#include "../src/ui/lang.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static void test_persistence_and_corruption(const char *test_path)
{
    printf("Testing persistence and corruption handling...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);

    /* Clean state: no saved choice */
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), ""));

    /* Set and save a choice */
    CHECK(x4_server_settings_set_selected("BrazilSouth", true));
    CHECK(x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), "BrazilSouth"));

    /* Re-init from disk: must persist */
    x4_server_settings_init();
    CHECK(x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), "BrazilSouth"));

    /* Switch to Automatic ("") and save */
    CHECK(x4_server_settings_set_selected("", true));
    CHECK(x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), ""));

    /* Re-init */
    x4_server_settings_init();
    CHECK(x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), ""));

    /* Validation: host outside GSSV suffix must be rejected */
    CHECK(!x4_server_settings_validate_host("evil.com"));
    CHECK(!x4_server_settings_validate_host("xboxlive.com"));
    CHECK(!x4_server_settings_validate_host("test.gssv-play-prod.xboxlive.com.evil.com"));
    CHECK(x4_server_settings_validate_host("brazilsouth.gssv-play-prod.xboxlive.com"));
    CHECK(x4_server_settings_validate_host("uksouth.gssv-play-prod.xboxlive.com"));

    /* Validation: name characters */
    CHECK(!x4_server_settings_validate_name(""));
    CHECK(!x4_server_settings_validate_name("name/with/slashes"));
    CHECK(!x4_server_settings_validate_name("name;injection"));
    CHECK(x4_server_settings_validate_name("BrazilSouth"));
    CHECK(x4_server_settings_validate_name("Central US"));
    CHECK(x4_server_settings_validate_name("East-US_2"));

    /* Corruption: truncated file */
    FILE *f = fopen(test_path, "wb");
    CHECK(f != NULL);
    uint32_t partial[3] = {0x31565358u, 2u, 1u};
    fwrite(partial, 1, sizeof(partial), f);
    fclose(f);
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), ""));

    /* Version 1 format: must be rejected and fall back to Automatic */
    f = fopen(test_path, "wb");
    CHECK(f != NULL);
    uint32_t v1_rec[100];
    memset(v1_rec, 0, sizeof(v1_rec));
    v1_rec[0] = 0x31565358u; /* XSV1 */
    v1_rec[1] = 1u; /* version 1 */
    fwrite(v1_rec, 1, sizeof(v1_rec), f);
    fclose(f);
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());

    /* Version 2 format: must also be rejected */
    f = fopen(test_path, "wb");
    CHECK(f != NULL);
    uint32_t v2_rec[100];
    memset(v2_rec, 0, sizeof(v2_rec));
    v2_rec[0] = 0x31565358u; /* XSV1 */
    v2_rec[1] = 2u; /* version 2 */
    fwrite(v2_rec, 1, sizeof(v2_rec), f);
    fclose(f);
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());

    /* Corruption: invalid magic */
    f = fopen(test_path, "wb");
    CHECK(f != NULL);
    uint32_t bad_rec[100];
    memset(bad_rec, 0, sizeof(bad_rec));
    bad_rec[0] = 0xdeadbeefu;
    bad_rec[1] = 1u;
    fwrite(bad_rec, 1, sizeof(bad_rec), f);
    fclose(f);
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());

    /* Corruption: tampered checksum */
    CHECK(x4_server_settings_set_selected("BrazilSouth", true));
    f = fopen(test_path, "r+b");
    CHECK(f != NULL);
    fseek(f, -4, SEEK_END);
    uint32_t bad_csum = 0x12345678;
    fwrite(&bad_csum, 1, sizeof(bad_csum), f);
    fclose(f);
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());

    remove(test_path);
    printf("PASS: persistence and corruption handling\n");
}

static void test_server_unavailable_fallback(const char *test_path)
{
    printf("Testing server unavailable fallback & warning...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);

    /* Initial state */
    x4_server_settings_init();
    CHECK(x4_server_settings_set_selected("JapanEast", true));
    CHECK(!x4_server_settings_server_not_available());

    /* Xbox returns regions WITHOUT JapanEast */
    X4ServerRegion list[2];
    memset(list, 0, sizeof(list));
    snprintf(list[0].name, sizeof(list[0].name), "UKSouth");
    snprintf(list[0].host, sizeof(list[0].host), "uksouth.gssv-play-prod.xboxlive.com");
    list[0].is_default = true;

    snprintf(list[1].name, sizeof(list[1].name), "WestEurope");
    snprintf(list[1].host, sizeof(list[1].host), "westeurope.gssv-play-prod.xboxlive.com");
    list[1].is_default = false;

    CHECK(x4_server_settings_update_regions(list, 2));

    /* Must detect unavailable, set flag and fall back to Automatic */
    CHECK(x4_server_settings_server_not_available());
    CHECK(!strcmp(x4_server_settings_get_selected(), ""));

    /* User acknowledges and selects UKSouth */
    CHECK(x4_server_settings_set_selected("UKSouth", true));
    x4_server_settings_clear_not_available();
    CHECK(!x4_server_settings_server_not_available());
    CHECK(!strcmp(x4_server_settings_get_selected(), "UKSouth"));

    remove(test_path);
    printf("PASS: server unavailable fallback\n");
}

static void test_tab_navigation_and_menu(const char *test_path)
{
    printf("Testing L1/R1 tab navigation and server selection...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);

    /* Populate regions */
    X4ServerRegion list[3];
    memset(list, 0, sizeof(list));
    snprintf(list[0].name, sizeof(list[0].name), "BrazilSouth");
    snprintf(list[0].host, sizeof(list[0].host), "brazilsouth.gssv-play-prod.xboxlive.com");
    list[0].is_default = false;

    snprintf(list[1].name, sizeof(list[1].name), "CentralUS");
    snprintf(list[1].host, sizeof(list[1].host), "centralus.gssv-play-prod.xboxlive.com");
    list[1].is_default = true;

    snprintf(list[2].name, sizeof(list[2].name), "WestEurope");
    snprintf(list[2].host, sizeof(list[2].host), "westeurope.gssv-play-prod.xboxlive.com");
    list[2].is_default = false;

    x4_server_settings_init();
    x4_server_settings_update_regions(list, 3);
    x4_server_settings_set_selected("", true); /* Automatic */

    X4PictureSettingsUi ui = {0};

    /* Open dialog */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true));
    CHECK(ui.open);
    CHECK(ui.tab == 0); /* starts on Picture */
    CHECK(ui.server_selected == 0); /* Automatic */

    /* Press L1 while on Tab 0: does not underflow */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_PREV_TAB, true));
    CHECK(ui.tab == 0);

    /* Press R1: switches to Tab 1 (Servidor) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_NEXT_TAB, true));
    CHECK(ui.tab == 1);

    /* The new Access section is reached from Server; its upper bound is 2. */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_NEXT_TAB, true));
    CHECK(ui.tab == 2);
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_NEXT_TAB, true));
    CHECK(ui.tab == 2);
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_PREV_TAB, true));
    CHECK(ui.tab == 1);

    /* In Tab 1: navigate options (0=Auto, 1=Brazil, 2=CentralUS, 3=WestEurope -> total 4) */
    CHECK(ui.server_selected == 0);

    /* DOWN -> 1 (BrazilSouth) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    CHECK(ui.server_selected == 1);

    /* DOWN -> 2 (CentralUS) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    CHECK(ui.server_selected == 2);

    /* DOWN -> 3 (WestEurope) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    CHECK(ui.server_selected == 3);

    /* DOWN wraps -> 0 (Auto) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    CHECK(ui.server_selected == 0);

    /* UP wraps -> 3 (WestEurope) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_UP, true));
    CHECK(ui.server_selected == 3);

    /* Cancel: closes without modifying saved selection */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CANCEL, true));
    CHECK(!ui.open);
    CHECK(!strcmp(x4_server_settings_get_selected(), ""));

    /* Reopen and confirm selection 1 (BrazilSouth) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true));
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_NEXT_TAB, true));
    CHECK(ui.tab == 1);
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true)); /* select 1 */
    CHECK(ui.server_selected == 1);
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CONFIRM, true));
    CHECK(!ui.open);
    CHECK(ui.server_changed);
    CHECK(!strcmp(x4_server_settings_get_selected(), "BrazilSouth"));

    remove(test_path);
    printf("PASS: L1/R1 tab navigation and menu\n");
}

static void test_first_time_flow(const char *test_path)
{
    printf("Testing first-time flow (X saves, CIRCLE skips saving)...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);

    /* New user without saved choice */
    x4_server_settings_init();
    CHECK(!x4_server_settings_has_saved_choice());

    /* Regions arrived */
    X4ServerRegion list[2];
    memset(list, 0, sizeof(list));
    snprintf(list[0].name, sizeof(list[0].name), "BrazilSouth");
    snprintf(list[0].host, sizeof(list[0].host), "brazilsouth.gssv-play-prod.xboxlive.com");
    list[0].is_default = false;

    snprintf(list[1].name, sizeof(list[1].name), "CentralUS");
    snprintf(list[1].host, sizeof(list[1].host), "centralus.gssv-play-prod.xboxlive.com");
    list[1].is_default = true;
    x4_server_settings_update_regions(list, 2);

    /* First-time modal opens */
    X4PictureSettingsUi ui = {0};
    x4_picture_settings_open_server_first_time(&ui);
    CHECK(ui.open);
    CHECK(ui.tab == 1);
    CHECK(ui.first_time_mode);
    CHECK(ui.server_selected == 0);

    /* In first-time mode, L1/R1 cannot switch away */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_PREV_TAB, true));
    CHECK(ui.tab == 1);

    /* Case A: User presses CIRCLE (cancels without saving) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CANCEL, true));
    CHECK(!ui.open);
    CHECK(!ui.first_time_mode);
    CHECK(x4_server_settings_first_choice_completed());
    /* Must NOT have saved choice */
    CHECK(!x4_server_settings_has_saved_choice());

    /* Next login: since has_saved_choice is still false, first-time modal opens again! */
    x4_picture_settings_open_server_first_time(&ui);
    CHECK(ui.open);
    CHECK(ui.first_time_mode);

    /* Case B: User selects Automatic and presses X (confirms and saves) */
    CHECK(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CONFIRM, true));
    CHECK(!ui.open);
    CHECK(!ui.first_time_mode);
    CHECK(x4_server_settings_has_saved_choice()); /* Now it IS saved */

    /* Re-init from disk: choice is saved, so first-time modal won't trigger */
    x4_server_settings_init();
    CHECK(x4_server_settings_has_saved_choice());

    remove(test_path);
    printf("PASS: first-time flow\n");
}

static void test_translations_and_display_names(void)
{
    printf("Testing translations and display names...\n");
    (void)x4_lang_init();

    /* Spanish */
    x4_lang_set(X4_LANG_ES, "es-ES", "es");
    CHECK(!strcmp(x4_server_region_display_name("BrazilSouth"), "Sur de Brasil"));
    CHECK(!strcmp(x4_server_region_display_name("BRAZILSOUTH"), "Sur de Brasil"));
    CHECK(!strcmp(x4_server_region_display_name("MexicoCentral"), "Centro de México"));
    CHECK(!strcmp(x4_server_region_display_name("MEXICOCENTRAL"), "Centro de México"));
    CHECK(!strcmp(x4_server_region_display_name("UKSouth"), "Sur de Reino Unido"));
    CHECK(!strcmp(x4_server_region_display_name("WestEurope"), "Oeste de Europa"));
    CHECK(!strcmp(x4_server_region_display_name("SpainCentral"), "Centro de España"));
    CHECK(!strcmp(x4_server_region_display_name("CentralUS"), "Centro de EE. UU."));
    CHECK(!strcmp(x4_server_region_display_name("ChileCentral"), "Centro de Chile"));
    CHECK(!strcmp(x4_server_region_display_name("SwedenCentral"), "Centro de Suecia"));
    CHECK(!strcmp(x4_server_region_display_name("CentralIndia"), "Centro de India"));
    CHECK(!strcmp(x4_server_region_display_name("SouthIndia"), "Sur de India"));
    CHECK(!strcmp(x4_server_region_display_name("FranceCentral"), "Centro de Francia"));
    CHECK(!strcmp(x4_server_region_display_name("GermanyWestCentral"), "Centro-oeste de Alemania"));
    CHECK(!strcmp(x4_server_region_display_name("NorwayEast"), "Este de Noruega"));
    CHECK(!strcmp(x4_server_region_display_name("PolandCentral"), "Centro de Polonia"));
    CHECK(!strcmp(x4_server_region_display_name("ItalyNorth"), "Norte de Italia"));
    CHECK(!strcmp(x4_server_region_display_name("SouthAfricaNorth"), "Norte de Sudáfrica"));
    CHECK(!strcmp(x4_server_region_display_name("UAENorth"), "Norte de EAU"));
    CHECK(!strcmp(x4_server_region_display_name("EastAsia"), "Este de Asia"));
    CHECK(!strcmp(x4_server_region_display_name("SoutheastAsia"), "Sureste de Asia"));
    /* Unknown region preserves raw name */
    CHECK(!strcmp(x4_server_region_display_name("UnknownAzureRegion"), "UnknownAzureRegion"));

    /* English */
    x4_lang_set(X4_LANG_EN, "en-US", "en");
    CHECK(!strcmp(x4_server_region_display_name("BrazilSouth"), "Brazil South"));
    CHECK(!strcmp(x4_server_region_display_name("BRAZILSOUTH"), "Brazil South"));
    CHECK(!strcmp(x4_server_region_display_name("MexicoCentral"), "Mexico Central"));
    CHECK(!strcmp(x4_server_region_display_name("MEXICOCENTRAL"), "Mexico Central"));
    CHECK(!strcmp(x4_server_region_display_name("ChileCentral"), "Chile Central"));
    CHECK(!strcmp(x4_server_region_display_name("SwedenCentral"), "Sweden Central"));
    CHECK(!strcmp(x4_server_region_display_name("CentralIndia"), "Central India"));
    CHECK(!strcmp(x4_server_region_display_name("SouthIndia"), "South India"));
    CHECK(!strcmp(x4_server_region_display_name("FranceCentral"), "France Central"));
    CHECK(!strcmp(x4_server_region_display_name("GermanyWestCentral"), "Germany West Central"));
    CHECK(!strcmp(x4_server_region_display_name("NorwayEast"), "Norway East"));
    CHECK(!strcmp(x4_server_region_display_name("PolandCentral"), "Poland Central"));
    CHECK(!strcmp(x4_server_region_display_name("ItalyNorth"), "Italy North"));
    CHECK(!strcmp(x4_server_region_display_name("SouthAfricaNorth"), "South Africa North"));
    CHECK(!strcmp(x4_server_region_display_name("UAENorth"), "UAE North"));
    CHECK(!strcmp(x4_server_region_display_name("EastAsia"), "East Asia"));
    CHECK(!strcmp(x4_server_region_display_name("SoutheastAsia"), "Southeast Asia"));
    CHECK(!strcmp(x4_server_region_display_name("UnknownAzureRegion"), "UnknownAzureRegion"));

    printf("PASS: translations and display names\n");
}

static void test_32_regions_support(const char *test_path)
{
    printf("Testing support for 32 regions...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);
    x4_server_settings_init();

    X4ServerRegion list[32];
    memset(list, 0, sizeof(list));
    for (int i = 0; i < 32; ++i) {
        snprintf(list[i].name, sizeof(list[i].name), "Region%02d", i);
        snprintf(list[i].host, sizeof(list[i].host), "r%02d.gssv-play-prod.xboxlive.com", i);
        list[i].is_default = (i == 0);
    }

    CHECK(x4_server_settings_update_regions(list, 32));
    CHECK(x4_server_settings_get_region_count() == 32);

    /* Verify accessor */
    X4ServerRegion r;
    CHECK(x4_server_settings_get_region(31, &r));
    CHECK(!strcmp(r.name, "Region31"));

    /* Re-init from disk: all 32 preserved */
    x4_server_settings_init();
    CHECK(x4_server_settings_get_region_count() == 32);

    remove(test_path);
    printf("PASS: 32 regions support\n");
}

static void test_main_loop_busy_auth_and_choice(const char *test_path)
{
    printf("Testing main loop with busy auth and first choice...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);
    x4_server_settings_init();

    X4ServerRegion list[2];
    memset(list, 0, sizeof(list));
    snprintf(list[0].name, sizeof(list[0].name), "BrazilSouth");
    snprintf(list[0].host, sizeof(list[0].host), "brazilsouth.gssv-play-prod.xboxlive.com");
    list[0].is_default = true;

    snprintf(list[1].name, sizeof(list[1].name), "CentralUS");
    snprintf(list[1].host, sizeof(list[1].host), "centralus.gssv-play-prod.xboxlive.com");
    list[1].is_default = false;
    x4_server_settings_update_regions(list, 2);

    x4_server_settings_set_awaiting_first_choice(true);
    CHECK(x4_server_settings_awaiting_first_choice());

    X4PictureSettingsUi ui = {0};
    bool first_choice_modal_opened = false;

    /* Simulate 50 frames with auth busy (can_edit is false for normal dialogs) */
    for (int frame = 0; frame < 50; ++frame) {
        bool awaiting_first = x4_server_settings_awaiting_first_choice();
        if (awaiting_first) {
            if (!first_choice_modal_opened) {
                x4_picture_settings_open_server_first_time(&ui);
                first_choice_modal_opened = true;
            }
        } else {
            first_choice_modal_opened = false;
        }

        /* can_edit is passed as false (simulating busy auth) */
        x4_picture_settings_ui_handle(&ui, 0, false);
        /* Dialog MUST stay open in first_time_mode! */
        CHECK(ui.open);
        CHECK(ui.first_time_mode);
    }

    /* Simulate user pressing CIRCLE */
    x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CANCEL, false);
    CHECK(!ui.open);
    CHECK(!ui.first_time_mode);
    CHECK(x4_server_settings_first_choice_completed());
    CHECK(!x4_server_settings_has_saved_choice()); /* Not saved */

    /* Next session: opens again */
    x4_server_settings_set_awaiting_first_choice(true);
    first_choice_modal_opened = false;
    if (x4_server_settings_awaiting_first_choice() && !first_choice_modal_opened) {
        x4_picture_settings_open_server_first_time(&ui);
        first_choice_modal_opened = true;
    }
    CHECK(ui.open);
    /* Simulate user pressing CROSS */
    x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CONFIRM, false);
    CHECK(!ui.open);
    CHECK(x4_server_settings_has_saved_choice()); /* Now saved */

    remove(test_path);
    printf("PASS: main loop with busy auth and first choice\n");
}

static void test_rtt_storage_and_persistence(const char *test_path)
{
    printf("Testing real game RTT storage, validation, and persistence (version 3)...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);
    x4_server_settings_init();

    /* Validation: region must be non-empty */
    CHECK(!x4_server_settings_record_game_rtt(NULL, 50));
    CHECK(!x4_server_settings_record_game_rtt("", 50));

    /* Validation: rtt must be 1..2000 ms */
    CHECK(!x4_server_settings_record_game_rtt("BrazilSouth", 0));
    CHECK(!x4_server_settings_record_game_rtt("BrazilSouth", -5));
    CHECK(!x4_server_settings_record_game_rtt("BrazilSouth", 2001));
    CHECK(!x4_server_settings_record_game_rtt("BrazilSouth", 50000));

    /* Valid records */
    CHECK(x4_server_settings_record_game_rtt("BrazilSouth", 45));
    CHECK(x4_server_settings_record_game_rtt("CentralUS", 85));
    CHECK(x4_server_settings_record_game_rtt("MexicoCentral", 18));

    /* Check in-memory getters */
    CHECK(x4_server_settings_get_region_rtt("BrazilSouth") == 45);
    CHECK(x4_server_settings_get_region_rtt("CentralUS") == 85);
    CHECK(x4_server_settings_get_region_rtt("MexicoCentral") == 18);
    CHECK(x4_server_settings_get_region_rtt("UnknownRegion") == 0);

    /* Re-init from disk: must persist in version 3 file format */
    x4_server_settings_init();
    CHECK(x4_server_settings_get_region_rtt("BrazilSouth") == 45);
    CHECK(x4_server_settings_get_region_rtt("CentralUS") == 85);
    CHECK(x4_server_settings_get_region_rtt("MexicoCentral") == 18);

    /* Update an existing measurement */
    CHECK(x4_server_settings_record_game_rtt("BrazilSouth", 40));
    CHECK(x4_server_settings_get_region_rtt("BrazilSouth") == 40);
    x4_server_settings_init();
    CHECK(x4_server_settings_get_region_rtt("BrazilSouth") == 40);

    remove(test_path);
    printf("PASS: real game RTT storage, validation, and persistence\n");
}

static void test_region_ordering_and_continents(const char *test_path)
{
    printf("Testing region ordering (measured ascending, unmeasured by continent + name)...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);
    x4_server_settings_init();

    /* Record RTT for some regions */
    CHECK(x4_server_settings_record_game_rtt("CentralUS", 85));
    CHECK(x4_server_settings_record_game_rtt("MexicoCentral", 18));
    CHECK(x4_server_settings_record_game_rtt("BrazilSouth", 140));

    /* Populate list with 10 regions */
    X4ServerRegion list[10];
    memset(list, 0, sizeof(list));

    /* Unmeasured: Europe */
    snprintf(list[0].name, sizeof(list[0].name), "WestEurope");
    snprintf(list[0].host, sizeof(list[0].host), "westeurope.gssv-play-prod.xboxlive.com");

    snprintf(list[1].name, sizeof(list[1].name), "UKSouth");
    snprintf(list[1].host, sizeof(list[1].host), "uksouth.gssv-play-prod.xboxlive.com");

    /* Measured: America */
    snprintf(list[2].name, sizeof(list[2].name), "CentralUS");
    snprintf(list[2].host, sizeof(list[2].host), "centralus.gssv-play-prod.xboxlive.com");

    snprintf(list[3].name, sizeof(list[3].name), "BrazilSouth");
    snprintf(list[3].host, sizeof(list[3].host), "brazilsouth.gssv-play-prod.xboxlive.com");

    snprintf(list[4].name, sizeof(list[4].name), "MexicoCentral");
    snprintf(list[4].host, sizeof(list[4].host), "mexicocentral.gssv-play-prod.xboxlive.com");
    list[4].is_default = true;

    /* Unmeasured: America */
    snprintf(list[5].name, sizeof(list[5].name), "EastUS");
    snprintf(list[5].host, sizeof(list[5].host), "eastus.gssv-play-prod.xboxlive.com");

    snprintf(list[6].name, sizeof(list[6].name), "ChileCentral");
    snprintf(list[6].host, sizeof(list[6].host), "chilecentral.gssv-play-prod.xboxlive.com");

    /* Unmeasured: Asia/Oceania */
    snprintf(list[7].name, sizeof(list[7].name), "JapanEast");
    snprintf(list[7].host, sizeof(list[7].host), "japaneast.gssv-play-prod.xboxlive.com");

    snprintf(list[8].name, sizeof(list[8].name), "AustraliaEast");
    snprintf(list[8].host, sizeof(list[8].host), "australiaeast.gssv-play-prod.xboxlive.com");

    /* Unmeasured: Other */
    snprintf(list[9].name, sizeof(list[9].name), "SouthAfricaNorth");
    snprintf(list[9].host, sizeof(list[9].host), "southafricanorth.gssv-play-prod.xboxlive.com");

    CHECK(x4_server_settings_update_regions(list, 10));

    X4ServerRegion out[10];
    CHECK(x4_server_settings_get_regions(out, 10) == 10);

    /* 1. Measured regions sorted ascending by ms:
     *    Index 0: MexicoCentral (18 ms)
     *    Index 1: CentralUS (85 ms)
     *    Index 2: BrazilSouth (140 ms) */
    CHECK(!strcmp(out[0].name, "MexicoCentral") && out[0].last_game_ms == 18);
    CHECK(!strcmp(out[1].name, "CentralUS") && out[1].last_game_ms == 85);
    CHECK(!strcmp(out[2].name, "BrazilSouth") && out[2].last_game_ms == 140);

    /* 2. Unmeasured regions grouped by continent, then alphabetical:
     *    América: ChileCentral < EastUS */
    CHECK(!strcmp(out[3].name, "ChileCentral") && out[3].last_game_ms == 0);
    CHECK(!strcmp(out[4].name, "EastUS") && out[4].last_game_ms == 0);

    /*    Europa: UKSouth < WestEurope */
    CHECK(!strcmp(out[5].name, "UKSouth") && out[5].last_game_ms == 0);
    CHECK(!strcmp(out[6].name, "WestEurope") && out[6].last_game_ms == 0);

    /*    Asia y Oceanía: AustraliaEast < JapanEast */
    CHECK(!strcmp(out[7].name, "AustraliaEast") && out[7].last_game_ms == 0);
    CHECK(!strcmp(out[8].name, "JapanEast") && out[8].last_game_ms == 0);

    /*    Otras: SouthAfricaNorth */
    CHECK(!strcmp(out[9].name, "SouthAfricaNorth") && out[9].last_game_ms == 0);

    remove(test_path);
    printf("PASS: region ordering (measured ascending, unmeasured by continent + name)\n");
}

static void test_json_serialization_v3(const char *test_path)
{
    printf("Testing session JSON telemetry serialization (v3 format)...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);
    x4_server_settings_init();

    X4ServerRegion list[2];
    memset(list, 0, sizeof(list));
    snprintf(list[0].name, sizeof(list[0].name), "BrazilSouth");
    snprintf(list[0].host, sizeof(list[0].host), "brazilsouth.gssv-play-prod.xboxlive.com");
    list[0].is_default = true;

    snprintf(list[1].name, sizeof(list[1].name), "UKSouth");
    snprintf(list[1].host, sizeof(list[1].host), "uksouth.gssv-play-prod.xboxlive.com");
    list[1].is_default = false;

    x4_server_settings_record_game_rtt("BrazilSouth", 42);
    x4_server_settings_update_regions(list, 2);
    x4_server_settings_set_selected("BrazilSouth", true);

    char json_buf[2048] = {0};
    FILE *mem = tmpfile();
    CHECK(mem != NULL);
    x4_server_settings_write_json(mem);
    rewind(mem);
    size_t r = fread(json_buf, 1, sizeof(json_buf) - 1, mem);
    fclose(mem);
    CHECK(r > 0);

    CHECK(strstr(json_buf, "\"server\":{"));
    CHECK(strstr(json_buf, "\"selected\":\"BrazilSouth\""));
    CHECK(strstr(json_buf, "\"is_automatic\":false"));
    CHECK(strstr(json_buf, "\"default\":\"BrazilSouth\""));
    CHECK(strstr(json_buf, "\"regions\":["));
    CHECK(strstr(json_buf, "\"api_host\":\"brazilsouth.gssv-play-prod.xboxlive.com\""));
    CHECK(strstr(json_buf, "\"last_game_ms\":42"));
    CHECK(strstr(json_buf, "\"last_game_ms\":0"));
    CHECK(!strstr(json_buf, "\"ping_ms\""));
    CHECK(!strstr(json_buf, "\"is_measuring\""));

    remove(test_path);
    printf("PASS: session JSON telemetry serialization (v3 format)\n");
}

static void test_api_host_and_net_test_host_separation(const char *test_path)
{
    printf("Testing api_host and net_test_host separation...\n");
    x4_server_settings_set_path(test_path);
    remove(test_path);
    x4_server_settings_init();

    X4ServerRegion list[1];
    memset(list, 0, sizeof(list));
    snprintf(list[0].name, sizeof(list[0].name), "NorthCentralUS");
    snprintf(list[0].api_host, sizeof(list[0].api_host), "northcentralus.core.gssv-play-prod.xboxlive.com");
    snprintf(list[0].host, sizeof(list[0].host), "northcentralus.core.gssv-play-prod.xboxlive.com");
    snprintf(list[0].net_test_host, sizeof(list[0].net_test_host), "northcentralus.networktest.gssv-play-prod.xboxlive.com");
    snprintf(list[0].base_uri, sizeof(list[0].base_uri), "https://northcentralus.core.gssv-play-prod.xboxlive.com");
    list[0].is_default = true;

    x4_server_settings_update_regions(list, 1);
    x4_server_settings_set_selected("NorthCentralUS", true);

    /* Re-init from disk: must preserve api_host in entry.host and not use net_test_host */
    x4_server_settings_init();
    CHECK(x4_server_settings_has_saved_choice());
    CHECK(!strcmp(x4_server_settings_get_selected(), "NorthCentralUS"));

    X4ServerRegion out;
    CHECK(x4_server_settings_get_region(0, &out));
    CHECK(!strcmp(out.name, "NorthCentralUS"));
    CHECK(!strcmp(out.api_host, "northcentralus.core.gssv-play-prod.xboxlive.com"));
    CHECK(!strcmp(out.host, "northcentralus.core.gssv-play-prod.xboxlive.com"));

    remove(test_path);
    printf("PASS: api_host and net_test_host separation\n");
}

int main(void)
{
    const char *test_path = "scratch_test_server_settings.bin";
    test_persistence_and_corruption(test_path);
    test_server_unavailable_fallback(test_path);
    test_tab_navigation_and_menu(test_path);
    test_first_time_flow(test_path);
    test_main_loop_busy_auth_and_choice(test_path);
    test_translations_and_display_names();
    test_32_regions_support(test_path);
    test_rtt_storage_and_persistence(test_path);
    test_region_ordering_and_continents(test_path);
    test_json_serialization_v3(test_path);
    test_api_host_and_net_test_host_separation(test_path);
    printf("\nALL SERVER SETTINGS TESTS PASSED!\n");
    return 0;
}
