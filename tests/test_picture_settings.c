/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/picture_settings.h"
#include "../src/streaming/stream_profile.h"
#include "../src/input/gamepad.h"
#include "../src/ui/lang.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"

static void test_mapping_and_vsync(void)
{
    printf("Testing option -> profile mapping and default VSync...\n");
    assert(x4_picture_settings_to_profile(X4_PIC_QUALITY_1080P_ALTA) == X4_REQUEST_1080P_G);
    assert(x4_picture_settings_to_profile(X4_PIC_QUALITY_1080P) == X4_REQUEST_1080P_G8);
    assert(x4_picture_settings_to_profile(X4_PIC_QUALITY_720P) == X4_REQUEST_720P_HQ);

    /* Test stream settings integration */
    x4_picture_settings_set(X4_PIC_QUALITY_1080P_ALTA);
    X4StreamRequestConfig cfg = x4_stream_settings_get();
#if X4_PUBLIC
    assert(cfg.profile == X4_REQUEST_1080P_G);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);

    x4_picture_settings_set(X4_PIC_QUALITY_1080P);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);

    x4_picture_settings_set(X4_PIC_QUALITY_720P);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_720P_HQ);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);
#else
    (void)cfg;
#endif
    printf("PASS: mapping and VSync\n");
}

static void test_persistence(const char *test_path)
{
    printf("Testing persistence at %s...\n", test_path);
    x4_picture_settings_set_path(test_path);
    remove(test_path);

    /* Missing file -> returns default (Alta / 0) */
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Save option 1 (1080p), re-init -> 1 */
    assert(x4_picture_settings_set(X4_PIC_QUALITY_1080P) == true);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P);

    /* Save option 2 (720p), re-init -> 2 */
    assert(x4_picture_settings_set(X4_PIC_QUALITY_720P) == true);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_720P);

    /* Save option 0 (Alta), re-init -> 0 */
    assert(x4_picture_settings_set(X4_PIC_QUALITY_1080P_ALTA) == true);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Out of bounds set -> false, active setting preserved */
    assert(x4_picture_settings_set((X4PictureQualityOption)99) == false);
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Corruption: truncated file */
    FILE *f = fopen(test_path, "wb");
    assert(f != NULL);
    uint32_t partial[2] = {0x31533458u, 1u};
    fwrite(partial, 1, sizeof(partial), f);
    fclose(f);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Corruption: bad magic */
    f = fopen(test_path, "wb");
    assert(f != NULL);
    uint32_t bad_magic[4] = {0xdeadbeefu, 1u, 1u, 0u};
    fwrite(bad_magic, 1, sizeof(bad_magic), f);
    fclose(f);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Corruption: unknown version */
    f = fopen(test_path, "wb");
    assert(f != NULL);
    uint32_t bad_ver[4] = {0x31533458u, 99u, 1u, 0u};
    fwrite(bad_ver, 1, sizeof(bad_ver), f);
    fclose(f);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Corruption: invalid quality value */
    f = fopen(test_path, "wb");
    assert(f != NULL);
    uint32_t bad_val[4] = {0x31533458u, 1u, 5u, 0u};
    fwrite(bad_val, 1, sizeof(bad_val), f);
    fclose(f);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Corruption: tampered checksum */
    assert(x4_picture_settings_set(X4_PIC_QUALITY_1080P) == true);
    /* Read valid file, corrupt 1 byte of checksum */
    f = fopen(test_path, "r+b");
    assert(f != NULL);
    fseek(f, 24, SEEK_SET);
    uint8_t b = 0;
    fread(&b, 1, 1, f);
    b ^= 0xff;
    fseek(f, 24, SEEK_SET);
    fwrite(&b, 1, 1, f);
    fclose(f);
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    remove(test_path);
    printf("PASS: persistence and corruption handling\n");
}

/* Write old seven-word V2 records directly to exercise migration without
 * relying on what the new writer happens to produce. */
static void test_vsync_settings(void)
{
    const char *path="test_pic_vsync46c.bin";
    const char *legacy_path="test_pic_vsync46c.bin.v1";
    x4_picture_settings_set_path(path);
    remove(path);
    remove(legacy_path);
    const X4StreamVsyncPolicy default_policy=X4_PUBLIC?X4_VSYNC_ALWAYS:X4_VSYNC_AUTO;
    x4_picture_settings_init();
    assert(x4_picture_settings_config().vsync==default_policy);

    uint32_t legacy[4]={0x31533458u,1u,X4_PIC_QUALITY_720P,0u};
    legacy[3]=legacy[0]^(legacy[1]<<16)^(legacy[2]*0x9e3779b9u)^0xa5a55a5au;
    FILE *legacy_file=fopen(legacy_path,"wb");assert(legacy_file);
    assert(fwrite(legacy,1,sizeof(legacy),legacy_file)==sizeof(legacy)&&!fclose(legacy_file));
    x4_picture_settings_init();
    assert(x4_picture_settings_config().vsync==default_policy);
    assert(x4_picture_settings_get()==X4_PIC_QUALITY_720P);
    assert(!fopen(path,"rb")); /* Opening an old file does not rewrite it. */
    remove(legacy_path);

    for(unsigned policy=0;policy<4;++policy){
        unsigned effective=X4_PUBLIC&&policy!=X4_VSYNC_NEVER?X4_VSYNC_ALWAYS:policy;
        uint32_t record[7]={0x32533458u,2u,X4_PIC_QUALITY_1080P,0u,policy,0u,0u};
        uint32_t checksum=0x72656332u;
        for(unsigned i=0;i<6;++i)
            checksum=((checksum<<5)|(checksum>>27))^(record[i]+0x9e3779b9u);
        record[6]=checksum;
        FILE *file=fopen(path,"wb");assert(file);
        assert(fwrite(record,1,sizeof(record),file)==sizeof(record)&&!fclose(file));
        x4_picture_settings_init();
        X4PictureSettingsConfig loaded=x4_picture_settings_config();
        assert(loaded.quality==X4_PIC_QUALITY_1080P);
        assert(loaded.conversion_backend==X4_PUBLIC&&loaded.dither==(X4_PUBLIC!=0));
        assert((unsigned)loaded.vsync==effective);
        /* Public writes normalize even callers supplying the old CPU/No pair. */
        loaded.conversion_backend=0;loaded.dither=false;
        assert(x4_picture_settings_set_config(&loaded));
        uint32_t saved[7];
        file=fopen(path,"rb");assert(file);
        assert(fread(saved,1,sizeof(saved),file)==sizeof(saved)&&fgetc(file)==EOF&&!fclose(file));
        uint32_t expected[7];memcpy(expected,record,sizeof(expected));
        expected[3]=expected[5]=X4_PUBLIC;
        expected[4]=effective;checksum=0x72656332u;
        for(unsigned i=0;i<6;++i)checksum=((checksum<<5)|(checksum>>27))^(expected[i]+0x9e3779b9u);
        expected[6]=checksum;
        assert(!memcmp(saved,expected,sizeof(expected)));
#if X4_PUBLIC
        X4StreamRequestConfig request=x4_stream_settings_get();
        assert((unsigned)x4_stream_request_vsync_policy(&request)==effective);
        assert(!x4_stream_request_gpu_force_cpu(&request)&&x4_stream_request_dither_enabled(&request));
#endif
    }
    X4PictureSettingsConfig selected=x4_picture_settings_config();
    selected.vsync=X4_VSYNC_AUTO_T27;
    assert(!x4_picture_settings_set_config(&selected));
    assert(x4_picture_settings_config().vsync==(X4_PUBLIC?X4_VSYNC_ALWAYS:X4_VSYNC_SYNCED));
#if X4_PUBLIC
    selected.vsync=X4_VSYNC_AUTO;
    assert(x4_picture_settings_set_config(&selected));
    X4PictureSettingsUi ui={0};
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true)&&ui.selected==1);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    assert(x4_picture_settings_config().vsync==X4_VSYNC_ALWAYS);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    assert(x4_picture_settings_config().vsync==X4_VSYNC_NEVER);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true)&&ui.selected==1);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    assert(x4_picture_settings_config().vsync==X4_VSYNC_ALWAYS);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true)&&ui.selected==1);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_LEFT,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    assert(x4_picture_settings_config().vsync==X4_VSYNC_NEVER);
#endif
    for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes){
        for(unsigned policy=0;policy<4;++policy){
            unsigned effective=X4_PUBLIC&&policy==3u?0u:policy;
            unsigned set=x4_stream_interaction_with_vsync(modes,(X4StreamVsyncPolicy)policy);
            assert((set&~X4_INTERACTION_VSYNC_MASK)==(modes&~X4_INTERACTION_VSYNC_MASK));
            X4StreamRequestConfig request={.interaction_modes=(unsigned char)set};
            assert((unsigned)x4_stream_request_vsync_policy(&request)==effective);
            X4StreamRequestConfig captured=x4_stream_request_capture(&request);
            assert((unsigned)x4_stream_request_vsync_policy(&captured)==effective);
        }
    }
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!strcmp(x4_tr(X4_TR_VSYNC_SYNCED),"Sincronizada"));
#if X4_PUBLIC
    assert(!strcmp(x4_tr(X4_TR_VSYNC_ALWAYS),"Activada (recomendada)"));
    assert(!strcmp(x4_tr(X4_TR_VSYNC_NEVER),"Desactivada"));
    assert(!strcmp(x4_tr(X4_TR_VSYNC_HELP_NEVER),
        "Algo menos de retraso, pero en juegos a 60 fps puede verse una franja horizontal al mover la cámara."));
#endif
    x4_lang_set(X4_LANG_EN,"en-US","US");
    assert(!strcmp(x4_tr(X4_TR_VSYNC_SYNCED),"Synced"));
#if X4_PUBLIC
    assert(!strcmp(x4_tr(X4_TR_VSYNC_ALWAYS),"On (recommended)"));
    assert(!strcmp(x4_tr(X4_TR_VSYNC_NEVER),"Off"));
    assert(strstr(x4_tr(X4_TR_VSYNC_HELP_NEVER),"60 fps"));
#endif
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    selected.vsync=X4_VSYNC_AUTO;
    assert(x4_picture_settings_set_config(&selected));
    remove(path);
    puts("PASS T46c old V1 defaults and V2 Auto/Always -> On, Never -> Off; two options, stable record ABI, and ES/EN");
}

static void test_menu_navigation(const char *test_path)
{
    printf("Testing menu navigation and guards...\n");
    x4_picture_settings_set_path(test_path);
    x4_picture_settings_set(X4_PIC_QUALITY_1080P_ALTA);

    X4PictureSettingsUi ui = {0};

    /* Guard: cannot open if can_edit is false (e.g. in game or keyboard editing) */
    bool handled = x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, false);
    assert(!handled);
    assert(!ui.open);

    /* Open when can_edit is true */
    handled = x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true);
    assert(handled);
    assert(ui.open);
    assert(ui.selected == 0); /* current saved was Alta */

#if X4_PUBLIC
    /* Public wraps in both directions after two rows. */
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_UP, true));
    assert(ui.selected == 1);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_UP, true));
    assert(ui.selected == 0);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    assert(ui.selected == 1);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    assert(ui.selected == 0);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_RIGHT, true));
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CANCEL, true));
    assert(!ui.open && x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true));
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_RIGHT, true));
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CONFIRM, true));
    assert(!ui.open && x4_picture_settings_get() == X4_PIC_QUALITY_1080P);
#else
    /* Lab retains exactly the original three quality rows. */
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_UP, true));
    assert(ui.selected == 2);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_UP, true));
    assert(ui.selected == 1);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    assert(ui.selected == 2);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    assert(ui.selected == 0);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_UP, true));
    assert(ui.selected == 2);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CANCEL, true));
    assert(!ui.open && x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true));
    assert(ui.selected == 0);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_DOWN, true));
    assert(ui.selected == 1);
    assert(x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_CONFIRM, true));
    assert(!ui.open && x4_picture_settings_get() == X4_PIC_QUALITY_1080P);
#endif

    /* Guard: if open and can_edit suddenly becomes false, closes immediately */
    handled = x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true);
    assert(ui.open);
    handled = x4_picture_settings_ui_handle(&ui, 0, false);
    assert(handled); /* consumed */
    assert(!ui.open);

    remove(test_path);
    printf("PASS: navigation and guards\n");
}

/* OpenOrbis pad button bitmasks for testing */
#define TEST_PAD_SELECT   (1u << 0)
#define TEST_PAD_L3       (1u << 1)
#define TEST_PAD_R3       (1u << 2)
#define TEST_PAD_START    (1u << 3)
#define TEST_PAD_UP       (1u << 4)
#define TEST_PAD_RIGHT    (1u << 5)
#define TEST_PAD_DOWN     (1u << 6)
#define TEST_PAD_LEFT     (1u << 7)
#define TEST_PAD_L2       (1u << 8)
#define TEST_PAD_R2       (1u << 9)
#define TEST_PAD_L1       (1u << 10)
#define TEST_PAD_R1       (1u << 11)
#define TEST_PAD_TRIANGLE (1u << 12)
#define TEST_PAD_CIRCLE   (1u << 13)
#define TEST_PAD_CROSS    (1u << 14)
#define TEST_PAD_SQUARE   (1u << 15)
#define TEST_PAD_OPTIONS  (1u << 3)

static void test_button_suppression_and_dimming(void)
{
    printf("Testing button suppression behind open modal dialog...\n");
    X4PictureSettingsUi ui = {0};

    /* 1. Modal closed: buttons pass through normally */
    uint32_t controller_pressed = TEST_PAD_CROSS;
    uint32_t ui_pressed = controller_pressed;
    unsigned pic_actions = 0;
    if (controller_pressed & TEST_PAD_OPTIONS) pic_actions |= X4_PIC_SETTINGS_OPEN;
    if (controller_pressed & TEST_PAD_UP) pic_actions |= X4_PIC_SETTINGS_UP;
    if (controller_pressed & TEST_PAD_DOWN) pic_actions |= X4_PIC_SETTINGS_DOWN;
    if (controller_pressed & TEST_PAD_CROSS) pic_actions |= X4_PIC_SETTINGS_CONFIRM;
    if (controller_pressed & TEST_PAD_CIRCLE) pic_actions |= X4_PIC_SETTINGS_CANCEL;
    bool pic_consumed = x4_picture_settings_ui_handle(&ui, pic_actions, true);
    if (pic_consumed || ui.open) {
        ui_pressed = 0;
        controller_pressed = 0;
    }
    assert(!ui.open);
    assert(!pic_consumed);
    assert(controller_pressed == TEST_PAD_CROSS); /* Not suppressed when closed */
    assert(ui_pressed == TEST_PAD_CROSS);

    /* 2. Open modal with OPTIONS */
    controller_pressed = TEST_PAD_OPTIONS;
    ui_pressed = controller_pressed;
    pic_actions = X4_PIC_SETTINGS_OPEN;
    pic_consumed = x4_picture_settings_ui_handle(&ui, pic_actions, true);
    if (pic_consumed || ui.open) {
        ui_pressed = 0;
        controller_pressed = 0;
    }
    assert(pic_consumed);
    assert(ui.open);
    assert(controller_pressed == 0); /* Suppressed on opening frame! */
    assert(ui_pressed == 0);

    /* 3. With modal open: test CROSS (Confirm/Save) */
    /* Must be consumed by modal and suppressed for Account/Catalog behind */
    controller_pressed = TEST_PAD_CROSS;
    ui_pressed = controller_pressed;
    pic_actions = X4_PIC_SETTINGS_CONFIRM;
    pic_consumed = x4_picture_settings_ui_handle(&ui, pic_actions, true);
    if (pic_consumed || ui.open) {
        ui_pressed = 0;
        controller_pressed = 0;
    }
    assert(pic_consumed);
    assert(!ui.open); /* Confirmed and closed */
    assert(controller_pressed == 0); /* Behind pages NEVER see CROSS! */
    assert(ui_pressed == 0);

    /* 4. Reopen and test CIRCLE (Cancel) */
    x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true);
    assert(ui.open);
    controller_pressed = TEST_PAD_CIRCLE;
    ui_pressed = controller_pressed;
    pic_actions = X4_PIC_SETTINGS_CANCEL;
    pic_consumed = x4_picture_settings_ui_handle(&ui, pic_actions, true);
    if (pic_consumed || ui.open) {
        ui_pressed = 0;
        controller_pressed = 0;
    }
    assert(pic_consumed);
    assert(!ui.open);
    assert(controller_pressed == 0); /* Behind pages NEVER see CIRCLE! */
    assert(ui_pressed == 0);

    /* 5. Reopen and test UP / DOWN (Navigation inside modal) */
    x4_picture_settings_ui_handle(&ui, X4_PIC_SETTINGS_OPEN, true);
    assert(ui.open);
    controller_pressed = TEST_PAD_UP;
    ui_pressed = controller_pressed;
    pic_actions = X4_PIC_SETTINGS_UP;
    pic_consumed = x4_picture_settings_ui_handle(&ui, pic_actions, true);
    if (pic_consumed || ui.open) {
        ui_pressed = 0;
        controller_pressed = 0;
    }
    assert(pic_consumed);
    assert(ui.open);
    assert(controller_pressed == 0); /* Catalog selection does NOT move! */
    assert(ui_pressed == 0);

    controller_pressed = TEST_PAD_DOWN;
    ui_pressed = controller_pressed;
    pic_actions = X4_PIC_SETTINGS_DOWN;
    pic_consumed = x4_picture_settings_ui_handle(&ui, pic_actions, true);
    if (pic_consumed || ui.open) {
        ui_pressed = 0;
        controller_pressed = 0;
    }
    assert(pic_consumed);
    assert(ui.open);
    assert(controller_pressed == 0); /* Catalog selection does NOT move! */
    assert(ui_pressed == 0);

    /* 6. Verify background dimming */
    printf("Testing background dimming in x4_picture_settings_ui_draw...\n");
    uint32_t *fb = calloc(1920 * 1080, sizeof(uint32_t));
    assert(fb != NULL);
    /* Set distinct colors outside dialog box (top-left 10,10) and inside (500,500) */
    fb[10 + 10 * 1920] = 0xffa0b0c0;
    fb[500 + 500 * 1920] = 0xffffffff;

    /* When ui is open, draw must dim background by 50% */
    assert(ui.open);
    x4_picture_settings_ui_draw(&ui, fb);

    uint32_t expected_scrim = (((0xffa0b0c0 >> 1) & 0x007f7f7f) | 0xff000000);
    assert(fb[10 + 10 * 1920] == expected_scrim);

    /* Inside dialog box, pixel is covered by dialog background / cards */
    assert(fb[500 + 500 * 1920] != 0xffffffff);
    assert(fb[500 + 500 * 1920] != (((0xffffffff >> 1) & 0x007f7f7f) | 0xff000000));

    free(fb);
    printf("PASS: button suppression and background dimming\n");
}

static void test_multiplayer_preference(void)
{
    const char *path="test_pic_multiplayer.bin";
    const char *prefs="test_pic_multiplayer.bin.pads";
    x4_picture_settings_set_path(path);
    remove(prefs);
    x4_picture_settings_init();
    assert(x4_picture_settings_multiplayer());

    X4GamepadMatchOption option={0};
    assert(x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),true));
    X4PictureSettingsUi ui={0};
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    for(unsigned i=0;i<3;++i)
        assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_NEXT_TAB,true));
    assert(ui.tab==3&&ui.draft_multiplayer);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    assert(!ui.draft_multiplayer&&x4_picture_settings_multiplayer());
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    assert(!ui.open&&!x4_picture_settings_multiplayer());
    assert(x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),true));
    x4_picture_settings_init();
    assert(!x4_picture_settings_multiplayer());
    assert(!x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),false));
    assert(!x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),true));

    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    for(unsigned i=0;i<3;++i)
        assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_NEXT_TAB,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_LEFT,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true));
    assert(!x4_picture_settings_multiplayer());
    assert(x4_picture_settings_set_multiplayer(true));
    x4_picture_settings_init();
    assert(x4_picture_settings_multiplayer());
    assert(!x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),true));
    assert(x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),false));
    assert(x4_gamepad_match_option(&option,x4_picture_settings_multiplayer(),true));

    assert(x4_gamepad_menu_index(0x0fu,true)==0);
    assert(x4_gamepad_menu_index(0x0eu,true)==1);
    assert(x4_gamepad_menu_index(0x0cu,true)==2);
    assert(x4_gamepad_menu_index(0x08u,true)==3);
    assert(x4_gamepad_menu_index(0x0eu,false)==0);
    assert(x4_gamepad_menu_index(0,true)==0);
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!strcmp(x4_tr(X4_TR_PAD_SETTINGS_ON),"Activado"));
    x4_lang_set(X4_LANG_EN,"en-US","US");
    assert(!strcmp(x4_tr(X4_TR_PAD_SETTINGS_OFF),"Off"));
    remove(prefs);
    puts("PASS T33 preference default, save, reload, cancel, next-match capture, ES/EN and menu fallback");
}

static void open_mic_settings(X4PictureSettingsUi *ui)
{
    assert(x4_picture_settings_ui_handle(ui,X4_PIC_SETTINGS_OPEN,true));
    for(unsigned i=0;i<4;++i)
        assert(x4_picture_settings_ui_handle(ui,X4_PIC_SETTINGS_NEXT_TAB,true));
    assert(ui->tab==4 && !ui->mic_selected);
}

static void test_mic_settings(void)
{
    const char *prefs="test_pic_mic.bin.mic";
    x4_picture_settings_set_path("test_pic_mic.bin");
    remove(prefs);
    x4_picture_settings_init();
    assert(!x4_picture_settings_mic_enabled() && x4_picture_settings_mic_ms()==10);

    X4PictureSettingsUi ui={0};
    open_mic_settings(&ui);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    assert(ui.draft_mic_enabled && !x4_picture_settings_mic_enabled());
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true));
    assert(!x4_picture_settings_mic_enabled());
    open_mic_settings(&ui);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
#if X4_PUBLIC
    assert(!ui.mic_selected && ui.draft_mic_ms==10);
#else
    assert(ui.mic_selected==1);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    assert(ui.draft_mic_ms==20 && x4_picture_settings_mic_ms()==10);
#endif
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    x4_picture_settings_init();
    assert(x4_picture_settings_mic_enabled() && x4_picture_settings_mic_ms()==(X4_PUBLIC?10:20));

    /* A shared file written by Lab must never enable public 20 ms. */
    FILE *f=fopen(prefs,"wb");assert(f);
    assert(fputc('3',f)=='3' && !fclose(f));
    x4_picture_settings_init();
    assert(x4_picture_settings_mic_enabled() && x4_picture_settings_mic_ms()==(X4_PUBLIC?10:20));
    open_mic_settings(&ui);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_LEFT,true));
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    f=fopen(prefs,"rb");assert(f);
    assert(fgetc(f)=='2' && fgetc(f)==EOF && !fclose(f));
    f=fopen(prefs,"wb");assert(f);
    assert(fputs("3x",f)>=0 && !fclose(f));
    x4_picture_settings_init();
    assert(!x4_picture_settings_mic_enabled() && x4_picture_settings_mic_ms()==10);

    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!strcmp(x4_tr(X4_TR_MIC_SETTINGS_TAB),"Micrófono"));
    x4_lang_set(X4_LANG_EN,"en-US","US");
    assert(!strcmp(x4_tr(X4_TR_MIC_UNMUTE),"Unmute microphone"));
    remove(prefs);
    puts("PASS T34 mic preference default off, cancel, persisted enable and Lab duration, public 10 ms");
}

static void capture_menu(const char *path)
{
    enum { width=1920,height=1080 };
    x4_picture_settings_set_path(path);
    x4_picture_settings_init();
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!x4_font_init("assets/fonts/noto_sans.x4font"));
    uint32_t *pixels=calloc(width*height,sizeof(*pixels));
    assert(pixels);
    x4_apple_bg_draw(pixels);
    X4PictureSettingsUi ui={0};
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    x4_picture_settings_ui_draw(&ui,pixels);
    FILE *file=fopen(path,"wb");assert(file);
    assert(fprintf(file,"P6\n%d %d\n255\n",width,height)>0);
    unsigned char rgb[width*3];
    for(unsigned y=0;y<height;++y){
        for(unsigned x=0;x<width;++x){
            uint32_t argb=pixels[y*width+x];
            rgb[x*3]=(unsigned char)(argb>>16);
            rgb[x*3+1]=(unsigned char)(argb>>8);
            rgb[x*3+2]=(unsigned char)argb;
        }
        assert(fwrite(rgb,1,sizeof(rgb),file)==sizeof(rgb));
    }
    assert(!fclose(file));
    free(pixels);
}

int main(int argc,char **argv)
{
    if(argc==3&&!strcmp(argv[1],"--capture")){capture_menu(argv[2]);return 0;}
    if(argc==2&&!strcmp(argv[1],"--vsync-only")){test_vsync_settings();return 0;}
    assert(argc==1);
    x4_picture_settings_set_path("test_pic_settings.bin");
    remove("test_pic_settings.bin");
    x4_picture_settings_init();
    test_mapping_and_vsync();
    test_persistence("test_pic_settings.bin");
    test_vsync_settings();
    test_menu_navigation("test_pic_settings.bin");
    if(!getenv("X4_PICTURE43C_ONLY")){
        test_button_suppression_and_dimming();
        test_multiplayer_preference();
        test_mic_settings();
    }
    puts("ALL test_picture_settings PASSED");
    return 0;
}
