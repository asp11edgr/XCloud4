/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated host wrapper receives exact main.c blocks through its runner. */
#include "auth/device_auth.h"
#include "ui/access_country.h"
#include "ui/lang.h"
#include "ui/game_page.h"
#include "ui/favorites.h"
#include "ui/ui2_fortnite.h"
#include "ui/ui2_home.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ORBIS_PAD_BUTTON_LEFT 0x80u
#define ORBIS_PAD_BUTTON_RIGHT 0x20u
#define ORBIS_PAD_BUTTON_CROSS 0x4000u
#define ORBIS_PAD_BUTTON_CIRCLE 0x2000u
#define ORBIS_PAD_BUTTON_TRIANGLE 0x1000u
#define X4_FORTNITE_PRODUCT_ID "BT5P2X999VH2"
struct X4Auth { bool busy; X4AccessCountry country; } fake_auth;
static X4Auth *auth=&fake_auth;
static struct {bool open,first_time_mode,access_changed;} picture_settings_ui;
static struct {bool editing;} catalog_browser;
static bool game_input,closing;
static bool pic_consumed,pending_launch,live_retained,session_back;
static struct {int page,selected,catalog_from_home,exit_error;} screen;
static struct {bool open;} update_notice,cloud_launch;
static X4AuthSnapshot account;
static X4Ui2Fortnite fortnite;
static int previous_page,fortnite_return_page,fortnite_return_selection;
static bool reopen_catalog,game_input_owned;
typedef struct {
    bool open, free_in_store, has_details;
    X4GamePage page;
    char product[X4_PRODUCT_ID_CAP], name[64];
    X4GameDetails details;
    X4GamePageSimilar similar[X4_GAME_SIMILAR_MAX];
    unsigned similar_count;
} GamePageUi;
static GamePageUi game_page_ui;
static const X4Favorites favorites;
static struct {uint32_t pressed;} controller;
static X4CatalogSnapshot catalog,backend_catalog;
static X4AuthCatalogCursor catalog_cursor;
static X4AccessCountryUi access_card_ui;
static bool access_card_visible,access_query,access_query_started,access_query_cancelled;
static uint64_t access_notice_until,input_at;
static int access_query_page,pending_auth,rc;
static unsigned catalog_selected,ui_pressed,open_calls,handle_calls,cancel_calls,start_calls;
static unsigned seen_actions;
static bool seen_busy,assets_idle,finish_immediately;
static int assets_fault,start_result;
static X4CatalogAssets *catalog_assets;
static char ui_notice[256];
static X4AccessCountryUiResult next_ui_result;
static X4AccessCountryCardPhase card_phase;
static X4AccessCountry current_preference;
int x4_auth_busy(X4Auth *a){return a->busy;}
void x4_auth_cancel(X4Auth *a){++cancel_calls;a->busy=false;backend_catalog.state=X4_CATALOG_CANCELLED;}
X4AccessCountry x4_auth_access_country(const X4Auth *a){return a->country;}
int x4_auth_start(X4Auth *a,enum X4AuthAction action)
{
    assert(action==(screen.page==8?X4_AUTH_FORTNITE_QUERY:X4_AUTH_XBOX_CATALOG));
    ++start_calls;
    if(start_result)return start_result;
    a->country=current_preference;a->busy=!finish_immediately;
    backend_catalog.state=finish_immediately?X4_CATALOG_READY:X4_CATALOG_LOADING;
    if (finish_immediately && action==X4_AUTH_FORTNITE_QUERY) {
        backend_catalog.count=1;
        strcpy(backend_catalog.titles[0].product_id,X4_FORTNITE_PRODUCT_ID);
        backend_catalog.titles[0].entitled=1;
    }
    return 0;
}
int x4_auth_catalog_snapshot_cached(X4Auth *a,X4CatalogSnapshot *out,X4AuthCatalogCursor *cursor)
{(void)a;(void)cursor;*out=backend_catalog;return 1;}
void x4_catalog_assets_suspend(X4CatalogAssets *a){(void)a;}
enum X4AssetFault x4_catalog_assets_fault(X4CatalogAssets *a){(void)a;return assets_fault;}
int x4_catalog_assets_idle(X4CatalogAssets *a){(void)a;return assets_idle;}
void x4_access_country_ui_open(X4AccessCountryUi *ui){++open_calls;ui->selected=current_preference;}
X4AccessCountryUiResult x4_access_country_ui_handle(X4AccessCountryUi *ui,unsigned actions,bool busy,const char *origin)
{
    (void)ui;assert(!strcmp(origin,"tarjeta"));++handle_calls;seen_actions=actions;seen_busy=busy;
    X4AccessCountryUiResult result=next_ui_result;next_ui_result=X4_ACCESS_UI_NONE;return result;
}
void x4_access_country_card_set(const X4AccessCountryUi *ui,X4AccessCountryCardPhase phase)
{(void)ui;card_phase=phase;}
static void event_step(void){/* @ACCESS_SYNC@ */ /* @ACCESS_EVENT@ */}
static void dispatch_step(void){/* @FORTNITE_SCHEDULE@ */ /* @ACCESS_DISPATCH@ */}
static void completion_step(void){/* @ACCESS_COMPLETE@ */}
static void game_page_open(void *ui,int origin,unsigned index,const X4CatalogTitle *title,const X4RecentGame *recent)
{
    assert(ui==&game_page_ui&&origin==8&&index==0&&title==&catalog.titles[0]&&!recent);
    game_page_ui.open=true;
    strcpy(game_page_ui.product,title->product_id);
    x4_game_page_init(&game_page_ui.page);
}
static const X4CatalogTitle *game_page_selected(const GamePageUi *ui, const X4CatalogSnapshot *cat)
{return ui->open&&cat->state==X4_CATALOG_READY?&cat->titles[0]:NULL;}
bool x4_favorites_contains(const X4Favorites *items,const char *product)
{(void)items;(void)product;return false;}
/* @FORTNITE_GAME_MODEL@ */
static void fortnite_home_step(const X4Ui2HomeRoute route)
{
    switch (route.action) {
        /* @FORTNITE_HOME@ */
        default: break;
    }
    /* @FORTNITE_ENTER@ */
}
static void fortnite_back_step(void){/* @FORTNITE_BACK@ */}
static void fortnite_ready_step(void){/* @FORTNITE_READY@ */}
static bool lab_gate(void){/* @ACCESS_LAB_GATE@ */return can_edit;}
static bool picture_gate(void){/* @ACCESS_PICTURE_GATE@ */return can_edit_pic;}
static void reset(int page)
{
    memset(&catalog,0,sizeof(catalog));memset(&backend_catalog,0,sizeof(backend_catalog));
    catalog.state=X4_CATALOG_ERROR;catalog.error=X4_AUTH_E_XBOX;catalog.http_status=403;
    strcpy(catalog.offering,"xgpuwebf2p");strcpy(catalog.stage,"Xbox Cloud Gaming rechazo la cuenta");
    backend_catalog=catalog;screen.page=page;screen.catalog_from_home=0;
    fake_auth=(struct X4Auth){0};picture_settings_ui.open=false;
    picture_settings_ui.first_time_mode=picture_settings_ui.access_changed=catalog_browser.editing=game_input=closing=false;
    pic_consumed=pending_launch=live_retained=session_back=false;
    access_card_visible=access_query=access_query_started=access_query_cancelled=false;
    access_notice_until=0;input_at=1000000;pending_auth=-1;access_query_page=0;
    controller.pressed=0;ui_pressed=99;open_calls=handle_calls=cancel_calls=start_calls=0;
    assets_idle=true;finish_immediately=false;assets_fault=start_result=0;
    next_ui_result=X4_ACCESS_UI_NONE;current_preference=X4_ACCESS_US;card_phase=X4_ACCESS_CARD_SELECT;
    account.state=X4_AUTH_AUTHORIZED;
    fortnite=(X4Ui2Fortnite){.phase=page==8?X4_UI2_FORTNITE_REGION:X4_UI2_FORTNITE_IDLE};
    previous_page=page;fortnite_return_page=0;fortnite_return_selection=1;
    game_page_ui.open=game_input_owned=reopen_catalog=false;
}
static void apply(void)
{controller.pressed=ORBIS_PAD_BUTTON_CROSS;next_ui_result=X4_ACCESS_UI_APPLIED;event_step();}
static void test_home_to_fortnite(void)
{
    reset(0);
    const int focus=X4_UI2_HOME_POPULAR_FIRST;
    X4Ui2HomeModel home={.mode=X4_UI2_HOME_EMPTY,.popular_count=1,.popular={X4_CATALOG_MAX}};
    X4Ui2HomeRoute route=x4_ui2_home_activate(&home,focus);
    assert(route.action==X4_UI2_HOME_FORTNITE);
    home.popular[0]=0; /* Even when Fortnite was listed on the ordinary catalog. */
    assert(x4_ui2_home_activate(&home,focus).action==X4_UI2_HOME_FORTNITE);
    screen.selected=focus;
    fortnite_home_step(route);
    assert(screen.page==8&&x4_ui2_fortnite_waiting(&fortnite)&&!game_page_ui.open);
    completion_step(); /* A stale catalog snapshot must not advance a queued request. */
    assert(fortnite.phase==X4_UI2_FORTNITE_QUEUED);
    assets_idle=false;
    dispatch_step();
    assert(start_calls==0&&pending_auth==X4_AUTH_FORTNITE_QUERY);
    assets_idle=true;finish_immediately=true;
    dispatch_step();
    assert(start_calls==1&&fortnite.phase==X4_UI2_FORTNITE_QUERYING&&!game_page_ui.open);
    completion_step();fortnite_ready_step();dispatch_step();
    assert(start_calls==1&&fortnite.phase==X4_UI2_FORTNITE_READY&&game_page_ui.open);
    assert(game_page_ui.page.focus==X4_GAME_FOCUS_PLAY&&!catalog.titles[0].f2p_only);
    const X4GamePageModel model=game_page_model(&game_page_ui,&catalog,&favorites,false);
    assert(model.available&&model.entitled&&model.free_in_store);
    assert(x4_ui2_fortnite_is_product("bt5p2x999vh2"));
    assert(x4_game_page_update(&game_page_ui.page,&model,X4_GAME_INPUT_SELECT)==X4_GAME_ACTION_PLAY);

    reset(0);fortnite_home_step(route);
    start_result=X4_AUTH_E_BUSY;dispatch_step();completion_step();fortnite_ready_step();
    assert(fortnite.phase==X4_UI2_FORTNITE_ERROR&&fortnite.error==X4_AUTH_E_BUSY&&!game_page_ui.open);
    reset(0);fortnite_home_step(route);dispatch_step();
    backend_catalog.state=X4_CATALOG_ERROR;backend_catalog.error=X4_AUTH_E_FORTNITE_NOT_FOUND;
    fake_auth.busy=false;completion_step();fortnite_ready_step();
    assert(fortnite.phase==X4_UI2_FORTNITE_ERROR&&fortnite.error==X4_AUTH_E_FORTNITE_NOT_FOUND);
    assert(!game_page_ui.open);

    reset(0);screen.selected=focus;fortnite_home_step(route);dispatch_step();
    previous_page=8;ui_pressed=ORBIS_PAD_BUTTON_CIRCLE;screen.selected=0;
    fortnite_back_step();
    assert(screen.page==0&&screen.selected==focus&&reopen_catalog&&fortnite.phase==X4_UI2_FORTNITE_IDLE);
    backend_catalog.state=X4_CATALOG_READY;fake_auth.busy=false;
    completion_step();fortnite_ready_step();
    assert(!game_page_ui.open&&x4_ui2_home_normalize(&home,focus)==focus);

    reset(0);screen.selected=focus;fortnite_home_step(route);dispatch_step();
    backend_catalog.state=X4_CATALOG_ERROR;fake_auth.busy=false;
    completion_step();fortnite_ready_step();event_step();
    assert(fortnite.phase==X4_UI2_FORTNITE_REGION&&!game_page_ui.open&&access_card_visible);
    apply();finish_immediately=true;dispatch_step();completion_step();fortnite_ready_step();
    assert(start_calls==2&&fortnite.phase==X4_UI2_FORTNITE_READY&&game_page_ui.open&&!access_card_visible);
    puts("PASS exact main Fortnite: Inicio, consulta, ficha, Play, error, Círculo y reintento regional");
}

int main(void)
{
    test_home_to_fortnite();
    for(int page=5;page<=8;page+=3){
        reset(page);event_step();assert(open_calls==1&&handle_calls==1&&access_card_visible);
        controller.pressed=ORBIS_PAD_BUTTON_LEFT|ORBIS_PAD_BUTTON_RIGHT;event_step();
        assert(seen_actions==(X4_ACCESS_UI_LEFT|X4_ACCESS_UI_RIGHT)&&!controller.pressed&&!ui_pressed&&pending_auth<0);
        controller.pressed=ORBIS_PAD_BUTTON_TRIANGLE;event_step();assert(seen_actions==X4_ACCESS_UI_HELP&&pending_auth<0);
        controller.pressed=ORBIS_PAD_BUTTON_CROSS;event_step();assert(pending_auth<0); /* First warning confirmation. */
        apply();assert(access_query&&!access_query_started&&!access_query_cancelled);
        assert(page==8 ? fortnite.phase==X4_UI2_FORTNITE_QUEUED : pending_auth==X4_AUTH_XBOX_CATALOG);
        assets_idle=false;dispatch_step();assert(!start_calls&&pending_auth>=0);
        assets_idle=true;dispatch_step();assert(start_calls==1&&access_query_started&&auth->busy&&pending_auth<0);
        apply();assert(start_calls==1&&pending_auth<0&&seen_busy); /* Repeated X while busy cannot enqueue. */
        controller.pressed=ORBIS_PAD_BUTTON_CIRCLE;next_ui_result=X4_ACCESS_UI_CANCEL_QUERY;event_step();
        assert(cancel_calls==1&&access_query_cancelled&&!controller.pressed);
        if (page==8) assert(screen.page==0&&reopen_catalog&&!access_card_visible);
        completion_step();
        assert(!access_query&&!access_notice_until&&card_phase==X4_ACCESS_CARD_SELECT);
        reset(page);apply();finish_immediately=true;dispatch_step();
        assert(catalog.state==X4_CATALOG_ERROR&&backend_catalog.state==X4_CATALOG_READY);
        current_preference=X4_ACCESS_JP;completion_step(); /* Copy final snapshot before judging result. */
        assert(!access_query&&access_notice_until==4000000&&access_card_ui.selected==X4_ACCESS_US);
        assert(card_phase==X4_ACCESS_CARD_SUCCESS&&screen.page==page&&start_calls==1);
        reset(page);apply();dispatch_step();auth->busy=false;backend_catalog.state=X4_CATALOG_ERROR;
        completion_step();assert(!access_query&&!access_notice_until&&card_phase==X4_ACCESS_CARD_SELECT);
        reset(page);apply();controller.pressed=ORBIS_PAD_BUTTON_CIRCLE;next_ui_result=X4_ACCESS_UI_CANCEL_QUERY;
        event_step();dispatch_step();assert(!start_calls&&!access_query&&pending_auth<0);
        reset(page);picture_settings_ui.open=true;apply();assert(!handle_calls&&!open_calls&&pending_auth<0);
        reset(page);pic_consumed=true;apply();assert(!handle_calls&&!open_calls&&pending_auth<0);
        reset(page);controller.pressed=ORBIS_PAD_BUTTON_CIRCLE;next_ui_result=X4_ACCESS_UI_BACK;event_step();
        assert(screen.page==(page==8?0:4)&&pending_auth<0&&!start_calls&&!controller.pressed);
        reset(page);catalog.xerr=2148916235u;
        if (page==8) { x4_ui2_fortnite_begin(&fortnite);x4_ui2_fortnite_started(&fortnite,0);x4_ui2_fortnite_complete(&fortnite,&catalog,false); }
        apply();assert(!handle_calls&&!open_calls&&pending_auth<0);
        reset(page);catalog.http_status=0;catalog.error=-9999;
        if (page==8) { x4_ui2_fortnite_begin(&fortnite);x4_ui2_fortnite_started(&fortnite,0);x4_ui2_fortnite_complete(&fortnite,&catalog,false); }
        apply();assert(!handle_calls&&!open_calls&&pending_auth<0);
        reset(page);apply();start_result=X4_AUTH_E_BUSY;dispatch_step();assert(!access_query&&!access_notice_until&&start_calls==1);
        reset(page);access_query=true;access_query_started=true;auth->busy=false;
        assert(!lab_gate()&&!picture_gate()); /* Finished worker but pending outcome still owns the flow. */
        backend_catalog.state=X4_CATALOG_READY;completion_step();assert(lab_gate()&&picture_gate());
        reset(page);event_step();picture_settings_ui.access_changed=true;pic_consumed=true;current_preference=X4_ACCESS_JP;
        event_step();assert(!picture_settings_ui.access_changed&&access_card_ui.selected==X4_ACCESS_JP&&open_calls==2&&pending_auth<0&&!start_calls);
    }
    puts("PASS exact main blocks: catalog/Fortnite retry, first confirm, cancellation, duplicate edges, OPTIONS priority, fast completion snapshot, captured success country and excluded XSTS/network");
    return 0;
}
