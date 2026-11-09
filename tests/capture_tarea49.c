/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_CAPTURE47_MAIN capture49_unused_main
#define x4_ui46_capture_footer capture49_unused_footer
#include "capture_tarea47.c"
#undef x4_ui46_capture_footer
#undef X4_CAPTURE47_MAIN
#include "picture_settings.h"
#include "catalog_presentation.h"
#include "server_settings.h"
#include "stream_settings.h"
#include "cloud_time_ui.h"
#include "play_history.h"
#if X4_UI49_AFTER
#include "ui2_account.h"
#include "ui2_info.h"
#include "ui2_fortnite.h"
#include "ui2_modal.h"
#endif

static unsigned fit_count, frames;
static int footer_depth, footer_top, last_content;
static unsigned footer_checks;
static bool audit_text;
static unsigned game_pass_labels;

void __real_x4_ui2_shell_fit_legacy(uint32_t *, const uint32_t *);
void __wrap_x4_ui2_shell_fit_legacy(uint32_t *pixels, const uint32_t *legacy)
{
    ++fit_count;
    __real_x4_ui2_shell_fit_legacy(pixels, legacy);
}

void x4_ui46_capture_footer(const uint32_t *pixels, int x, int y, int w, int h, bool after)
{
    (void)pixels;
    if (!audit_text) return;
    assert(x >= X4_UI_SAFE_LEFT && y >= 0 && x+w <= X4_UI_SAFE_RIGHT && y+h <= X4_UI_SAFE_BOTTOM);
    if (!after) {
        ++footer_depth;
        if (y < footer_top) footer_top = y;
        ++footer_checks;
    } else {
        assert(footer_depth > 0);
        --footer_depth;
    }
}

static void inspect49(uint32_t *pixels, int x, int y, int size, int width, uint32_t color)
{
    if (!audit_text || width <= 0) return;
    const int bottom = y + size;
    if (x < X4_UI_SAFE_LEFT || x + width > X4_UI_SAFE_RIGHT || y < 0 || bottom > X4_UI_SAFE_BOTTOM) {
        fprintf(stderr, "T49 BOUNDS %s x=%d y=%d w=%d size=%d\n", active_scene,x,y,width,size);
        assert(x >= X4_UI_SAFE_LEFT && x + width <= X4_UI_SAFE_RIGHT && y >= 0 && bottom <= X4_UI_SAFE_BOTTOM);
    }
    if (x >= X4_UI2_SIDEBAR_W && !footer_depth && bottom > last_content)
        last_content = bottom;
    if (width < 12 || y >= 945 || x < X4_UI2_CONTENT_X) return;
    /* Sample the actual fill behind the text before glyph rendering. */
    double background[160];
    size_t n = 0;
    for (int row = y + size / 3; row < bottom; row += size > 20 ? 6 : 4)
        for (int col = x + 4; col < x + width && n < sizeof(background)/sizeof(*background); col += width / 28 + 6)
            background[n++] = luminance47(pixels[(size_t)row * WIDTH + col]);
    if (!n) return;
    qsort(background, n, sizeof(*background), compare_double47);
    double fg = luminance47(color);
    double bg = background[fg > 0.45 ? (n-1)*95/100 : (n-1)*5/100];
    contrast47(active_scene, "text", fg, bg, size >= 30 ? 3.0 : 4.5);
}

void __real_x4_text_px(uint32_t *, int,int,int,int,const char *,uint32_t);
void __wrap_x4_text_px(uint32_t *pixels, int x, int y, int size, int weight,
    const char *text, uint32_t color)
{
    if (text && !strcmp(text,"Game Pass")) ++game_pass_labels;
    if (audit_text && text && *text) inspect49(pixels,x,y,size,x4_text_width_px(size,weight,text),color);
    __real_x4_text_px(pixels,x,y,size,weight,text,color);
}

void __real_x4_text_ellipsis_px(uint32_t *,int,int,int,int,const char *,int,uint32_t);
void __wrap_x4_text_ellipsis_px(uint32_t *pixels, int x, int y, int size, int weight,
    const char *text, int cap, uint32_t color)
{
    if (text && !strcmp(text,"Game Pass")) ++game_pass_labels;
    if (audit_text && text && *text) {
        int width=x4_text_width_px(size,weight,text);
        inspect49(pixels,x,y,size,width < cap ? width : cap,color);
    }
    __real_x4_text_ellipsis_px(pixels,x,y,size,weight,text,cap,color);
}

static void begin49(const char *name, uint32_t *p, bool audit)
{
    CAPTURE_SCENE(name);
    audit_text = X4_UI49_AFTER && audit;
    footer_top = HEIGHT;
    last_content = 0;
    x4_rect(p,0,0,WIDTH,HEIGHT,X4_UI2_BACKGROUND);
}

static void frame49(const char *name,uint32_t *p)
{
    assert(!footer_depth);
    if (audit_text && last_content >= footer_top) {
        fprintf(stderr,"T49 FOOTER COLLISION %s %d %d\n",name,last_content,footer_top);
        assert(last_content < footer_top);
    }
    fprintf(stderr,"T49 FRAME_OK %s footer=%d text_bottom=%d\n",name,footer_top,last_content);
    audit_text=false;
    CAPTURE_SCENE(NULL);
    capture(name,p);
    ++frames;
}

static void shell49(uint32_t *p,X4Ui2Section section)
{
    x4_ui2_shell_set_section(&shell,section);
    x4_ui2_shell_draw(p,&shell,&status47);
}

#if !X4_UI49_AFTER
static void legacy49(uint32_t *p, uint32_t *scratch, X4Ui2Section section)
{
    x4_ui2_shell_fit_legacy(p,scratch);
    shell49(p,section);
}
#endif

static X4CatalogPresentation genres;
static void fixture_genres(void)
{
    static X4ProductMetadata batch[MAX_PRODUCTS];
    const char *const multiplayer[] = {
        forza, fortnite, "9PBG4MTM3JJC", "9NNFG8BQRCXL"
    };
    const char *const open_world[] = {
        forza, "9N0QWXC09MPT", "9N76QGDS5GJR", "C2N9CS4FS1QR"
    };
    x4_catalog_presentation_init(&genres);
    x4_catalog_presentation_set_generation(&genres, 1);
    for (unsigned i = 0; i < catalog.count; ++i) {
        unsigned index = asset_index(&assets, catalog.titles[i].product_id);
        assert(index < assets.count);
        batch[i] = assets.entries[index].metadata;
    }
    x4_catalog_browser_ui2_select(&browser, X4_CAT_TAB_DISCOVER);
    x4_catalog_browser_home_rebuild(&browser, &catalog, &recent, sigls);
    assert(x4_catalog_presentation_ingest(&genres, 1, 1, batch, catalog.count));
    X4GameDetails attribute_sample = {.attributes = X4_GAME_ATTR_ONLINE_MULTIPLAYER};
    for (unsigned i = 0; i < sizeof(multiplayer) / sizeof(*multiplayer); ++i)
        assert(x4_catalog_presentation_ingest_detail(&genres, multiplayer[i], &attribute_sample));
    for (unsigned i = 0; i < sizeof(open_world) / sizeof(*open_world); ++i) {
        attribute_sample.attributes = X4_GAME_ATTR_OPEN_WORLD |
            (i == 0 ? X4_GAME_ATTR_ONLINE_MULTIPLAYER : 0);
        assert(x4_catalog_presentation_ingest_detail(&genres, open_world[i], &attribute_sample));
    }
    assert(x4_catalog_presentation_apply_genres(&genres, &browser, &catalog));
    x4_catalog_browser_rebuild(&browser, &catalog);
    assert(browser.ui2_collection_count >= 2);
}

static void base49(const char *data, uint32_t *p, bool discover)
{
    if (discover) {
        set_tab(X4_CAT_TAB_DISCOVER);
        x4_catalog_browser_ui2_select(&browser,X4_CAT_TAB_DISCOVER);
        x4_catalog_browser_home_rebuild(&browser,&catalog,&recent,sigls);
        x4_catalog_browser_rebuild_featured(&browser,&catalog,&recent);
        assert(browser.featured_count>=3 && browser.featured_count<=5);
        unsigned featured=0;
        for (; featured<browser.featured_count; ++featured)
            if (!strcmp(catalog.titles[browser.featured_sources[featured]].product_id,forza)) break;
        assert(featured<browser.featured_count);
        browser.featured_index=featured;
        assert(!strcmp(x4_catalog_browser_ui2_hero(&browser,&catalog)->product_id,forza));
        (void)x4_catalog_browser_home_tick(&browser,&catalog,100);
        fprintf(stderr,"T49 FEATURED_READY carousel=%u chips_end=384 next_heading=413 gap=29\n",
            browser.featured_count);
        publish_background(data,forza);
        x4_catalog_gui_home_invalidate();
        draw_tab(p);
        shell49(p,X4_UI2_DISCOVER);
    } else {
        X4Ui2HomeModel model;
        X4UiRect damage[2];
        x4_ui2_home_model(&model,true,&catalog,&recent,&browser.home_rows);
        (void)x4_ui2_home_draw(&model,&catalog,&assets,1,X4_UI2_HOME_CONTINUE,false,p,damage);
        shell49(p,X4_UI2_HOME);
    }
}

static void seed_regions49(void)
{
    static const char *const names[]={"eastus","westus","westeurope","northeurope",
        "brazilsouth","japaneast","australiaeast","southindia","southcentralus","canadacentral"};
    X4ServerRegion regions[sizeof(names)/sizeof(*names)]={0};
    for(size_t i=0;i<sizeof(names)/sizeof(*names);++i) {
        snprintf(regions[i].name,sizeof(regions[i].name),"%s",names[i]);
        snprintf(regions[i].api_host,sizeof(regions[i].api_host),"%s.core.gssv-play-prod.xboxlive.com",names[i]);
        snprintf(regions[i].net_test_host,sizeof(regions[i].net_test_host),"%s.core.gssv-play-prod.xboxlive.com",names[i]);
        regions[i].is_default=i==0;
    }
    assert(x4_server_settings_update_regions(regions,sizeof(names)/sizeof(*names)));
}

static void settings49(uint32_t *p,uint32_t *scratch,const char *data)
{
    static const char *const keys[]={"ajustes-transmision","ajustes-servidor","ajustes-acceso",
        "ajustes-mandos","ajustes-microfono","ajustes-cuenta","ajustes-acerca",
        "ajustes-desplegable","ajustes-guardar-activo","ajustes-sin-guardar","servidor-desplazado","servidor-inicial"};
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        X4PictureSettingsUi ui={0};
        begin49(keys[i],p,true);
#if X4_UI49_AFTER
        (void)scratch;
        const unsigned tab=i<7?i:i>=10?1:0;
        if(i==11) {
            base49(data,p,false);
            x4_picture_settings_open_server_first_time(&ui);
            x4_ui2_modal_scrim(p);
            x4_picture_settings_ui_draw_modal(&ui,p);
        } else {
            x4_picture_settings_ui_open_section(&ui,(X4PictureSettingsSection)tab);
            if(i==7) x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true);
            if(i==8||i==9) {
                x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true);
                if(i==9) x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true);
                assert(x4_picture_settings_ui_dirty(&ui));
            }
            if(i==10)
                for(unsigned r=0;r<8;++r) x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true);
            x4_picture_settings_ui_draw_page(&ui,p);
            shell49(p,X4_UI2_SETTINGS);
            if(x4_picture_settings_ui_has_modal(&ui)) {
                x4_ui2_modal_scrim(p);
                x4_picture_settings_ui_draw_modal(&ui,p);
            }
        }
#else
        (void)data;
        if(i==11) x4_picture_settings_open_server_first_time(&ui);
        else {
            const unsigned tab = i < 5 ? i : i >= 10 ? 1 : 0;
            x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true);
            for (unsigned step = 0; step < tab; ++step)
                x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_NEXT_TAB,true);
            assert(ui.tab == tab);
            if(i==8||i==9) x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true);
            if(i==10) for(unsigned r=0;r<8;++r) x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true);
        }
        x4_rect(scratch,0,0,WIDTH,HEIGHT,X4_UI2_BACKGROUND);
        x4_picture_settings_ui_draw(&ui,scratch);
        legacy49(p,scratch,X4_UI2_SETTINGS);
#endif
        frame49(keys[i],p);
    }
}

static void account49(uint32_t *p,uint32_t *scratch)
{
    static const char *const keys[]={"cuenta-historial","cuenta-vacia","cuenta-recordar",
        "cuenta-aviso-recordar","cuenta-cerrar-sesion","cuenta-preparada","cuenta-codigo",
        "cuenta-caducada","cuenta-error","cuenta-conectando","cuenta-cancelada","cuenta-rechazada","cuenta-xsts"};
#if X4_UI49_AFTER
    static X4History local;
    X4Ui2AccountSummary recent_games={0};
    for(unsigned i=0;i<5 && i<catalog.count;++i)
        assert(x4_history_record(&local,catalog.titles[i].product_id,200000+i,28800/(i+1),NULL));
    x4_ui2_account_summary(&local,&recent_games);
#endif
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        X4AuthSnapshot auth={.state=X4_AUTH_AUTHORIZED};
        if(i>=5) {
            static const enum X4AuthState states[]={X4_AUTH_IDLE,X4_AUTH_WAITING,X4_AUTH_EXPIRED,
                X4_AUTH_ERROR,X4_AUTH_CONNECTING,X4_AUTH_CANCELLED,X4_AUTH_DENIED,X4_AUTH_ERROR};
            auth.state=states[i-5];
        }
        if(i==6) {
            snprintf(auth.user_code,sizeof(auth.user_code),"ABCD-EFGH");
            snprintf(auth.verification_uri,sizeof(auth.verification_uri),"https://www.microsoft.com/link");
            auth.seconds_left=487;
        }
        if(i==8||i==12) {
            auth.error=-20;
            auth.http_status=403;
        }
        const enum X4CatalogState previous_catalog_state = catalog.state;
        if (i == 12) {
            catalog.state = X4_CATALOG_ERROR;
            catalog.xerr = 2148916233u;
            catalog.http_status = 401;
            snprintf(auth.stage, sizeof(auth.stage), "Xbox rechazo la autorizacion XSTS");
        }
        begin49(keys[i],p,true);
#if X4_UI49_AFTER
        (void)scratch;
        const X4Ui2Account account={0};
        const X4Ui2AccountModel model={.auth=&auth,.remember={.enabled=i==2||i==4,.available=true},
            .focus_owned=true,.gamertag="Jugador",.initial="J",
            .server=x4_tr(X4_TR_SERVER_OPT_AUTO),.country=x4_access_country_label(X4_ACCESS_AUTO),
            .cloud_time={.remaining_s=7200,.max_s=36000,.source=X4_CLOUD_TIME_ACCOUNT},
            .history=i!=1?&recent_games:NULL,.catalog=&catalog,.assets=&assets};
        X4UiRect damage[2];
        (void)x4_ui2_account_draw(&account,&model,p,false,damage);
        shell49(p,X4_UI2_HOME);
        if(i==3||i==4) {
            x4_ui2_modal_scrim(p);
            if(i==3) x4_ui2_account_remember_modal(p);
            else x4_ui2_account_logout_modal(true,false,p);
        }
#else
        x4_auth_draw(&auth,false,false,scratch);
        x4_auth_remember_draw(i==2||i==4,true,i==3,scratch);
        legacy49(p,scratch,X4_UI2_HOME);
#endif
        frame49(keys[i],p);
        catalog.state = previous_catalog_state;
        catalog.xerr = 0;
        catalog.http_status = 0;
    }
}

static void remaining49(const char *data,uint32_t *p,uint32_t *scratch)
{
    const char *keys[]={"acerca","apoyar","fortnite","actualizacion-inicio","actualizacion-descubrir",
        "tiempo-inicio","tiempo-descubrir","pais-acceso","error-salida","inicio","descubrir",
#if !X4_PUBLIC
        "lab-desplazado",
#endif
    };
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        begin49(keys[i],p,i<3||i==7||i==8||i>=11);
        X4Screen screen={.page=i==0?2:i==1?7:8};
        const X4Ui2Section section=i==0?X4_UI2_ABOUT:i==1?X4_UI2_SUPPORT:X4_UI2_HOME;
        const X4Controller controller={0};
        (void)controller;
        (void)screen;
        if(i<3) {
            unsigned asset=asset_index(&assets,fortnite);
#if X4_UI49_AFTER
            if(i==0) x4_ui2_about_draw(p,NULL,X4_UPDATE_AVAILABLE,"1.4.0");
            else if(i==1) x4_ui2_support_draw(p);
            else {
                const X4Ui2Fortnite view={.phase=X4_UI2_FORTNITE_QUERYING};
                x4_ui2_fortnite_draw(p,&view,&catalog,&assets.entries[asset]);
            }
            shell49(p,section);
#else
            if(i==2) {
                X4HomeDrawContext context={.fortnite_cover=&assets.entries[asset].surface[X4_COVER_ROW]};
                x4_screen_set_home_context(&context);
                x4_fortnite_summary_draw(&screen,&catalog,false,scratch);
            } else x4_screen_draw(&screen,&controller,scratch);
            legacy49(p,scratch,section);
#endif
        } else if(i==9||i==10) base49(data,p,i==10);
        else if(i==3||i==4||i==5||i==6) {
            base49(data,p,i==4||i==6);
#if X4_UI49_AFTER
            (void)scratch;
            audit_text=true;
            x4_ui2_modal_scrim(p);
            if(i<5) {
                X4UpdateNotice notice={0};
                assert(x4_apple_update_notice_offer(&notice,"9.9.9",NULL,true,false));
                x4_ui2_update_notice_draw(&notice,p);
            } else x4_ui2_cloud_confirm_draw(p);
#else
            memcpy(scratch,p,pixel_count*sizeof(*p));
            if(i<5) {
                X4UpdateNotice notice={0};
                assert(x4_apple_update_notice_offer(&notice,"9.9.9",NULL,true,false));
                x4_apple_update_notice_draw(&notice,scratch);
            } else x4_cloud_time_confirm_draw(scratch);
            legacy49(p,scratch,i==4||i==6?X4_UI2_DISCOVER:X4_UI2_HOME);
#endif
        } else if(i==7||i==8) {
            const X4AccessCountryUi country={.selected=X4_ACCESS_AUTO};
#if X4_UI49_AFTER
            shell49(p,X4_UI2_CATALOG);
            x4_ui2_modal_scrim(p);
            if(i==7) x4_ui2_modal_draw_access(p,&country,X4_ACCESS_CARD_SELECT);
            else x4_ui2_exit_error_draw(-10,p);
#else
            x4_rect(scratch,0,0,WIDTH,HEIGHT,X4_UI2_BACKGROUND);
            if(i==7) {
                x4_access_country_card_set(&country,X4_ACCESS_CARD_SELECT);
                x4_apple_draw_access_card(scratch,252,288,1408,586);
                x4_access_country_card_set(NULL,X4_ACCESS_CARD_SELECT);
            } else x4_exit_error_draw(-10,scratch);
            legacy49(p,scratch,X4_UI2_CATALOG);
#endif
        }
#if !X4_PUBLIC
        else {
            const X4StreamSettingsUi lab={.open=true,.row=20};
#if X4_UI49_AFTER
            x4_stream_settings_ui_draw_page(&lab,p);
            shell49(p,X4_UI2_HOME);
#else
            x4_rect(scratch,0,0,WIDTH,HEIGHT,X4_UI2_BACKGROUND);
            x4_stream_settings_ui_draw(&lab,scratch);
            legacy49(p,scratch,X4_UI2_HOME);
#endif
        }
#endif
        frame49(keys[i],p);
    }
#if X4_UI49_AFTER
    unsigned asset=asset_index(&assets,fortnite);
    const X4CatalogAssetView *art=&assets.entries[asset];
    begin49("fortnite-lista-gratis",p,true);
    game_pass_labels=0;
    publish_background(data,fortnite);
    X4GamePage page;
    x4_game_page_init(&page);
    const X4GamePageModel model={.product_id=fortnite,.title=art->metadata.name,
        .description=art->metadata.description,.studio=art->metadata.publisher,
        .category=art->metadata.category,.available=true,.entitled=true,.free_in_store=true,
        .background=&assets.backgrounds[assets.active].raster};
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX];
    assert(x4_game_page_draw_cached(&page,&model,49,p,false,damage)==SIZE_MAX);
    shell49(p,X4_UI2_HOME);
    assert(game_pass_labels==0);
    frame49("fortnite-lista-gratis",p);
    begin49("fortnite-error",p,true);
    const X4Ui2Fortnite view={.phase=X4_UI2_FORTNITE_ERROR,.error=X4_AUTH_E_FORTNITE_NOT_FOUND};
    x4_ui2_fortnite_draw(p,&view,&catalog,art);
    shell49(p,X4_UI2_HOME);
    frame49("fortnite-error",p);
#endif
}

int main(int argc,char **argv)
{
    assert(argc==4);
    const bool en=!strcmp(argv[3],"en"), pseudo=!strcmp(argv[3],"largo");
    assert(en||pseudo||!strcmp(argv[3],"es"));
    x4_lang_set(en?X4_LANG_EN:X4_LANG_ES,en?"en-US":"es-MX",en?"US":"MX");
#if X4_UI49_AFTER
    if(pseudo) {
        size_t n=strlen(x4_tr(X4_TR_U49_ACCOUNT_LOGOUT_REMEMBER));
        x4_lang_pseudo_for_test(true);
        assert(strlen(x4_tr(X4_TR_U49_ACCOUNT_LOGOUT_REMEMBER))==n+(35*n+99)/100);
    }
    x4_apple_access_card_use_overlay(true);
#endif
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    load_fixtures(argv[1]);
    snprintf(catalog.gamertag,sizeof(catalog.gamertag),"Jugador");
    (void)x4_ui2_status_set(&status47,X4_UI2_NETWORK_CABLE,en?"1080p High":"1080p Alta",18*60+42,"Jugador");
    uint32_t *p=guarded47(),*scratch=guarded47();
#if X4_UI49_AFTER
    static X4History last_played;
    assert(x4_history_record(&last_played,forza,x4_history_now(),1560,NULL));
    x4_ui2_home_history(&last_played);
#endif
    seed_regions49();
    fixture_genres();
    settings49(p,scratch,argv[1]);
    account49(p,scratch);
    remaining49(argv[1],p,scratch);
    check_canvas(p);
    check_canvas(scratch);
    assert(X4_UI49_AFTER ? fit_count==0 : fit_count>0);
    fprintf(stderr,"T49 FINAL frames=%u fit_legacy=%u footer_checks=%u\n",frames,fit_count,footer_checks);
    assert(!fflush(stdout));
    return 0;
}
