/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
/* Inspect real private row spans without a production testing API. */
#include "../src/ui/screen.c"
#include <assert.h>
#include <stdlib.h>

uint64_t sceKernelGetProcessTime(void) { return 10000000; }
const char *x4_server_settings_active_display_name(void) { return ""; }
const char *x4_picture_settings_active_quality_label(void) { return "1080p"; }
const char *x4_access_country_label(X4AccessCountry c) { (void)c; return ""; }

static void open_menu(X4TouchMenu *m,bool pub)
{
    x4_touch_menu_init(m,pub);assert(m->item_count==(pub?6:8));
    assert(!x4_touch_menu_feed_touch(m,true,2,1000000));
    assert(!x4_touch_menu_feed_touch(m,true,2,1999999));
    assert(x4_touch_menu_feed_touch(m,true,2,2000000));
    assert(m->is_open && !m->selected_index);
}
static void actions(void)
{
    static const X4TouchMenuAction pub_actions[]={
        X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG,X4_TOUCH_MENU_ACTION_TOGGLE_MUTE,
        X4_TOUCH_MENU_ACTION_XBOX_BUTTON,X4_TOUCH_MENU_ACTION_RELOAD_STREAM,
        X4_TOUCH_MENU_ACTION_NET_HUD,X4_TOUCH_MENU_ACTION_CANCEL};
    static const X4TouchMenuAction lab_actions[]={
        X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG,X4_TOUCH_MENU_ACTION_TOGGLE_MUTE,
        X4_TOUCH_MENU_ACTION_XBOX_BUTTON,X4_TOUCH_MENU_ACTION_RELOAD_STREAM,
        X4_TOUCH_MENU_ACTION_NET_HUD,X4_TOUCH_MENU_ACTION_MARK_TEST,
        X4_TOUCH_MENU_ACTION_EXIT_APP,X4_TOUCH_MENU_ACTION_CANCEL};
    _Static_assert(X4_TOUCH_MENU_ACTION_RELOAD_STREAM==7,"old IDs fixed");
    _Static_assert(X4_TOUCH_MENU_ACTION_NET_HUD==8,"new ID appended");
    for(int pub=0;pub<2;++pub) {
        const X4TouchMenuAction *expected=pub?pub_actions:lab_actions;
        X4TouchMenu m;open_menu(&m,pub!=0);int count=m.item_count;
        for(int i=0;i<count;++i) {
            open_menu(&m,pub!=0);m.selected_index=i;
            assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_CROSS,ORBIS_PAD_BUTTON_CROSS,2100000)==expected[i]);
            assert(m.is_open==(i==4));
            assert(!x4_touch_menu_filter_buttons(&m,ORBIS_PAD_BUTTON_CROSS));
        }
        open_menu(&m,pub!=0);
        assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_UP,0,2100000)==X4_TOUCH_MENU_ACTION_NONE);
        assert(m.selected_index==count-1);
        assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_DOWN,0,2200000)==X4_TOUCH_MENU_ACTION_NONE);
        assert(!m.selected_index);
        assert(x4_touch_menu_update(&m,0,ORBIS_PAD_BUTTON_CROSS,2200000+X4_TOUCH_MENU_TIMEOUT_US)==X4_TOUCH_MENU_ACTION_NONE);
        assert(!m.is_open && !x4_touch_menu_filter_buttons(&m,ORBIS_PAD_BUTTON_CROSS));
        assert(!x4_touch_menu_filter_buttons(&m,0));
        assert(x4_touch_menu_filter_buttons(&m,ORBIS_PAD_BUTTON_CROSS)==ORBIS_PAD_BUTTON_CROSS);
    }
}
static void touch_and_nexus(void)
{
    X4TouchMenu m;x4_touch_menu_init(&m,true);
    assert(!x4_touch_menu_feed_touch(&m,false,2,1000000));
    assert(!x4_touch_menu_feed_touch(&m,true,1,4000000));
    open_menu(&m,true);
    assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_CIRCLE,ORBIS_PAD_BUTTON_CIRCLE,2100000)==X4_TOUCH_MENU_ACTION_CANCEL);
    assert(!x4_touch_menu_feed_touch(&m,true,2,4000000));
    assert(!x4_touch_menu_feed_touch(&m,true,0,4100000));
    assert(!x4_touch_menu_feed_touch(&m,true,2,4200000));
    assert(x4_touch_menu_feed_touch(&m,true,2,5200000));
    x4_touch_menu_trigger_nexus(&m,6000000);
    assert(x4_touch_menu_is_nexus_active(&m,6199999));
    assert(!x4_touch_menu_is_nexus_active(&m,6200000));
    x4_touch_menu_close(&m);assert(!m.is_open && !m.suppress_mask);
}
static void cycle_keeps_open(const char *path)
{
    x4_net_hud_test_reset(path);assert(x4_net_hud_mode()==X4_NET_HUD_WARNING);
    for(int pub=0;pub<2;++pub) {
        X4TouchMenu m;open_menu(&m,pub!=0);m.selected_index=4;
        static const unsigned expected[]={X4_NET_HUD_BASIC,X4_NET_HUD_FULL,X4_NET_HUD_OFF,X4_NET_HUD_WARNING};
        for(unsigned i=0;i<4;++i) {
            assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_CROSS,ORBIS_PAD_BUTTON_CROSS,2100000+i)==X4_TOUCH_MENU_ACTION_NET_HUD);
            (void)x4_net_hud_cycle_mode();x4_touch_menu_cache_reset();
            assert(x4_net_hud_mode()==expected[i] && m.is_open && m.selected_index==4);
            assert(x4_touch_menu_prepare(&m,0));
        }
    }
}
static void render_bounds(void)
{
    const size_t pixels=(size_t)X4_WIDTH*X4_HEIGHT;const uint32_t guard=0xac512943;
    uint32_t *raw=malloc((pixels+32)*sizeof(*raw));assert(raw);uint32_t *frame=raw+16;
    for(int pub=0;pub<2;++pub) {
        X4TouchMenu m;open_menu(&m,pub!=0);m.selected_index=m.item_count-1;
        for(size_t i=0;i<pixels+32;++i)raw[i]=guard;
        x4_touch_menu_cache_reset();assert(x4_touch_menu_prepare(&m,0));
        assert(MENU_Y+menu_cache.height==(pub?938:1030));
        size_t expected_bytes=0;
        for(int row=0;row<menu_cache.height;++row) {
            size_t inset=menu_cache.inset[row],count=MENU_W-2*inset;
            size_t offset=(size_t)(MENU_Y+row)*X4_WIDTH+MENU_X+inset;
            assert(MENU_Y+row>=0 && MENU_Y+row<X4_HEIGHT);
            assert(MENU_X+inset+count<=X4_WIDTH && offset+count<=pixels);
            expected_bytes+=count*sizeof(*frame);
        }
        x4_touch_menu_draw(&m,0,frame);X4TouchMenuCacheStats stats;x4_touch_menu_cache_snapshot(&stats);
        assert(stats.frames==1 && stats.copied_bytes==expected_bytes);
        for(unsigned i=0;i<16;++i)assert(raw[i]==guard && raw[pixels+16+i]==guard);
        /* Lab last copied row is 1029. y=1030 and every lower row is intact. */
        assert(frame[(size_t)(MENU_Y+menu_cache.height-1)*X4_WIDTH+MENU_X+MENU_W/2]!=guard);
        for(int y=0;y<X4_HEIGHT;++y)if(y<MENU_Y || y>=MENU_Y+menu_cache.height)
            for(int x=0;x<X4_WIDTH;++x)assert(frame[(size_t)y*X4_WIDTH+x]==guard);
        uint64_t bytes=stats.copied_bytes;x4_touch_menu_draw(&m,0,frame);x4_touch_menu_cache_snapshot(&stats);
        assert(stats.regenerations==1 && stats.frames==2 && stats.copied_bytes==2*bytes);
    }
    free(raw);x4_touch_menu_cache_reset();
}
static void mic_toggle(void)
{
    for (int pub=0;pub<2;++pub) {
        X4TouchMenu m;open_menu(&m,pub!=0);
        assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_SQUARE,ORBIS_PAD_BUTTON_SQUARE,2100000)
            ==X4_TOUCH_MENU_ACTION_NONE && m.is_open && !m.suppress_mask);
        m.mic_enabled=true;
        assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_SQUARE,ORBIS_PAD_BUTTON_SQUARE,2200000)
            ==X4_TOUCH_MENU_ACTION_TOGGLE_MIC && m.is_open);
        assert(m.last_activity_us==2200000 && !x4_touch_menu_filter_buttons(&m,ORBIS_PAD_BUTTON_SQUARE));
        assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_CIRCLE,0,2300000)
            ==X4_TOUCH_MENU_ACTION_CANCEL && !m.is_open);
        assert(x4_touch_menu_update(&m,ORBIS_PAD_BUTTON_SQUARE,ORBIS_PAD_BUTTON_SQUARE,2400000)
            ==X4_TOUCH_MENU_ACTION_NONE);
    }
    puts("PASS T34 Square toggles microphone only in open opted-in menu, without changing existing rows");
}
int main(int argc,char **argv)
{
    assert(argc==2);x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!x4_font_init("assets/fonts/inter.x4font"));x4_apple_set_animation_phase(0);
    actions();touch_and_nexus();cycle_keeps_open(argv[1]);render_bounds();mic_toggle();x4_font_close();
    puts("PASS touch_menu_test: public 6/Lab 8, old actions, circular HUD mode keeps menu open, opaque rows bounded; Lab bottom y=1030");
    return 0;
}
