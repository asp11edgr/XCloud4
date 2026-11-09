/* SPDX-License-Identifier: GPL-3.0-only */
#define main capture_tarea39_entry
#include "capture_tarea39_tabs.c"
#undef main
#include "cloud_time_ui.h"
#include "loading_screen.h"
#include "picture_settings.h"
#include "ui_draw_metrics.h"
#if !X4_CAPTURE46_BEFORE
#include "game_page.h"
#endif
#include <math.h>

static const char *scene;
static uint32_t footer_before[WIDTH * HEIGHT];
static const uint32_t *footer_owner;
static unsigned measured_footers;
static float luminance[256];

static void capture_46(const char *name, const uint32_t *pixels)
{
    capture(name, pixels);
    scene = NULL;
}

static float color_luminance(uint32_t color)
{
    return 0.2126f * luminance[color & 255u] +
           0.7152f * luminance[(color >> 8) & 255u] +
           0.0722f * luminance[(color >> 16) & 255u];
}

static int compare_float(const void *a, const void *b)
{
    float left=*(const float *)a, right=*(const float *)b;
    return (left > right) - (left < right);
}

void x4_ui46_capture_contrast(const uint32_t *pixels, const char *role,
    int x, int y, int w, int h, uint32_t fg)
{
    if (!scene || !w || !h) return;
    assert(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= WIDTH && y + h <= HEIGHT);
    size_t count = (size_t)w * h;
    float *samples = malloc(count * sizeof(*samples));
    assert(samples);
    size_t n = 0;
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            samples[n++] = color_luminance(pixels[(size_t)j * WIDTH + i]);
    qsort(samples, count, sizeof(*samples), compare_float);
    double base = samples[(count * 95 + 99) / 100 - 1];
    double light = color_luminance(fg);
    double ratio = (fmax(base, light) + 0.05) / (fmin(base, light) + 0.05);
    fprintf(stderr, "CONTRAST46 %s %s %d %d %d %d %.6f\n", scene, role, x, y, w, h, ratio);
    free(samples);
}

void x4_ui46_capture_footer(const uint32_t *pixels, int x, int y, int w, int h, bool after)
{
    (void)x; (void)y; (void)w; (void)h;
    if (!scene) return;
    if (!after) {
        assert(!footer_owner);
        footer_owner = pixels;
        memcpy(footer_before, pixels, sizeof(footer_before));
        return;
    }
    assert(footer_owner == pixels);
    footer_owner = NULL;
    int left = WIDTH, right = -1, bottom = -1;
    for (int j=0; j<HEIGHT; ++j) {
        const uint32_t *row = pixels + (size_t)j * WIDTH;
        const uint32_t *before = footer_before + (size_t)j * WIDTH;
        if (!memcmp(row, before, WIDTH * sizeof(*row))) continue;
        for (int i=0; i<WIDTH; ++i)
            if (row[i] != before[i]) {
                if (i < left) left = i;
                if (i > right) right = i;
                bottom = j;
            }
    }
    if (right >= 0) {
        ++measured_footers;
        fprintf(stderr,"FOOTER46 %s %d %d %d\n",scene,left,right,bottom);
    }
}

enum { BENCH46_WARMUP = 20, BENCH46_SAMPLES = 120 };

static void bench46_report(enum X4UiScreen screen, const char *label,
    const uint64_t samples[BENCH46_SAMPLES])
{
    X4UiDrawMetricView view;
    assert(x4_ui_draw_metrics_snapshot(screen, true, &view));
    assert(view.draw.n == BENCH46_SAMPLES && !view.draw.p50_overflow &&
        !view.draw.p95_overflow);
    fprintf(stderr, "BENCH46 %s %llu %llu %llu %llu\n", label,
        (unsigned long long)view.draw.n, (unsigned long long)view.draw.p50_us,
        (unsigned long long)view.draw.p95_us, (unsigned long long)view.draw.max_us);
    fprintf(stderr, "SAMPLES46 %s", label);
    for (unsigned i = 0; i < BENCH46_SAMPLES; ++i)
        fprintf(stderr, " %llu", (unsigned long long)samples[i]);
    fputc('\n', stderr);
}

static void main_menu_46(const char *dir, uint32_t *pixels, bool benchmark)
{
    X4CoverRaster featured = decode(dir, forza, "852x480", X4_COVER_HOME_FEATURE);
    X4CoverRaster battle = decode(dir, fortnite, "796x448", X4_COVER_HOME_FORTNITE);
    x4_catalog_assets_compose_card(&featured);
    x4_catalog_assets_compose_card(&battle);
    X4HomeDrawContext context = {.signed_in=1, .featured_hero=&featured, .fortnite_cover=&battle};
    X4Screen screen = {.page=0, .selected=0, .signed_in=1};
    X4Controller controller = {0};
    const char *names[] = {"menu-continuar","menu-sin-recientes","menu-error-recientes","menu-sin-sesion"};
#if !X4_CAPTURE46_BEFORE
    X4RecentGames test = recent;
    unsigned index = X4_CATALOG_MAX;
    assert(x4_home_continue_index(&catalog,&test,&index));
    assert(index < catalog.count);
    context.continue_name = catalog.titles[index].name;
    unsigned art = asset_index(&assets, catalog.titles[index].product_id);
    assert(art < assets.count);
    context.continue_cover = &assets.entries[art].surface[X4_COVER_ROW];
    const enum X4RecentState states[] = {X4_RECENT_IDLE,X4_RECENT_LOADING,X4_RECENT_ERROR};
    for (unsigned i=0; i<sizeof(states)/sizeof(*states); ++i) {
        test.state = states[i];
        assert(!x4_home_continue_index(&catalog,&test,&index));
    }
    test = recent;
    assert(test.count > 1);
    test.entries[0] = test.entries[test.count-1];
    assert(x4_home_continue_index(&catalog,&test,&index));
    assert(!strcmp(catalog.titles[index].id, recent.entries[1].id));
#endif
    for (unsigned i=0; !benchmark && i<sizeof(names)/sizeof(*names); ++i) {
        screen.signed_in = context.signed_in = i != 3;
#if !X4_CAPTURE46_BEFORE
        if (i) { context.continue_name=NULL; context.continue_cover=NULL; }
#endif
        x4_screen_set_home_context(&context);
        x4_screen_home_invalidate();
        scene=names[i];
        x4_screen_draw(&screen,&controller,pixels);
        capture_46(names[i],pixels);
    }
    context.signed_in=screen.signed_in=1;
#if !X4_CAPTURE46_BEFORE
    context.continue_name=catalog.titles[0].name;
    context.continue_cover=&assets.entries[asset_index(&assets,forza)].surface[X4_COVER_ROW];
#endif
    x4_screen_set_home_context(&context);
    x4_screen_home_invalidate();
    x4_screen_draw(&screen,&controller,pixels);
    uint64_t samples[BENCH46_SAMPLES];
    const unsigned limit = benchmark ? BENCH46_WARMUP + BENCH46_SAMPLES : 60;
    for (unsigned i=0; i<limit; ++i) {
        screen.selected = (int)((i + (unsigned)benchmark) & 1u);
        uint64_t start=clock_us();
        x4_screen_draw(&screen,&controller,pixels);
        uint64_t end=clock_us();
        if (benchmark && i < BENCH46_WARMUP) continue;
        x4_ui_draw_metrics_record(X4_UI_MENU,true,true,end-start,0,0,0,end,0);
        if (benchmark) samples[i - BENCH46_WARMUP] = end - start;
    }
    if (benchmark) bench46_report(X4_UI_MENU, "menu", samples);
    x4_screen_set_home_context(NULL);
    free(featured.rgba);
    free(battle.rgba);
}

static void tab_46(const char *name, enum X4CatalogTab tab, const char *id,
    const char *dir, uint32_t *pixels)
{
    set_tab(tab);
    if (tab != X4_CAT_TAB_DISCOVER) {
        snprintf(browser.tab_background_id,sizeof(browser.tab_background_id),"%s",id);
        browser.hero_focused=false;
    }
    x4_catalog_gui_home_invalidate();
    publish_background(dir,id);
    scene=name;
    draw_tab(pixels);
    capture_46(name,pixels);
}

#if !X4_CAPTURE46_BEFORE
static X4CoverRaster shot_raster(const char *dir, const char *filename, enum X4CoverSurface kind)
{
    size_t bytes;
    unsigned char *data=read_file(dir,filename,&bytes);
    X4CoverRaster raster={0};
    assert(data && !x4_product_surface_decode(data,bytes,kind,&raster));
    free(data);
    return raster;
}

static void page_46(const char *name, X4GamePage *page,
    const X4GamePageModel *model, uint32_t *pixels)
{
    scene=name;
    x4_game_page_draw(page,model,pixels);
    capture_46(name,pixels);
}

static void game_46(const char *dir, const char *fixtures, uint32_t *pixels, bool english, bool benchmark)
{
    unsigned fi=asset_index(&assets,forza), ft=asset_index(&assets,fortnite);
    assert(fi < assets.count && ft < assets.count);
    size_t metadata_bytes;
    unsigned char *metadata=read_file(fixtures,"forza.json",&metadata_bytes);
    static X4GameProductDetails parsed;
    X4ProductMetadata cover;
    assert(metadata && !x4_product_metadata_parse_details((const char *)metadata,metadata_bytes,
        forza,&cover,&parsed));
    free(metadata);
    assert(parsed.details.screenshot_count==6);
    X4GameDetails detail=parsed.details;
    X4CoverRaster shots[6], full=shot_raster(fixtures,"shot-full-in-game.jpg",X4_COVER_BACKGROUND);
    for (unsigned i=0; i<6; ++i) {
        char file[32];
        snprintf(file,sizeof(file),"shot-%u.jpg",i);
        shots[i]=shot_raster(fixtures,file,X4_COVER_DETAIL);
    }
    publish_background(dir,forza);
    X4GamePage page;
    x4_game_page_init(&page);
    X4GamePageModel model={.product_id=forza,.title="Forza Horizon 5",.available=true,
        .entitled=true,.details=&detail,.background=&assets.backgrounds[assets.active].raster};
    for (unsigned i=0; i<6; ++i) model.screenshots[i]=&shots[i];
    if (benchmark) {
        uint64_t samples[BENCH46_SAMPLES];
        x4_ui_draw_metrics_reset();
        for (unsigned i=0; i<BENCH46_WARMUP+BENCH46_SAMPLES; ++i) {
            uint64_t start=clock_us();
            x4_game_page_draw(&page,&model,pixels);
            uint64_t end=clock_us();
            if (i < BENCH46_WARMUP) continue;
            samples[i - BENCH46_WARMUP] = end - start;
            x4_ui_draw_metrics_record(X4_UI_OTHER,true,true,end-start,0,0,0,end,0);
        }
        bench46_report(X4_UI_OTHER, "ficha", samples);
        goto release;
    }
    page_46("ficha-forza",&page,&model,pixels);
    unsigned bright_top = 0;
    for (int x=84; x<684; ++x) {
        uint32_t source=model.background->rgba[x], shaded=pixels[x];
        for (unsigned shift=0; shift<24; shift+=8) {
            unsigned channel=(source>>shift)&255u;
            if (channel>=48u) ++bright_top;
            assert(((shaded>>shift)&255u)==(channel*37u+128u)/256u);
        }
    }
    assert(bright_top>0);
    fputs("PASS T46 game page: art shaded on y=0\n",stderr);
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_DOWN)==X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_RIGHT)==X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_SELECT)==X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page)==1);
    model.fullscreen=&full;
    page_46("ficha-captura-completa",&page,&model,pixels);
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_BACK)==X4_GAME_ACTION_NONE);
    assert(x4_game_page_fullscreen_index(&page)==-1);
    const char *paragraph=english ?
        "Discover the towns, roads and landscapes of Mexico at your own pace. Join friends for races, explore new routes, or return to your favorite roads in different seasons." :
        "Explora a tu ritmo los pueblos, las carreteras y los paisajes de México. Únete a tus amigos para competir, descubre rutas nuevas o vuelve a tus recorridos favoritos en distintas estaciones.";
    size_t used=0;
    for (unsigned i=0; i<12; ++i) {
        int n=snprintf(detail.description+used,sizeof(detail.description)-used,"%s\n\n",paragraph);
        assert(n > 0 && (size_t)n < sizeof(detail.description)-used);
        used += (size_t)n;
    }
    x4_game_page_init(&page);
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_SCROLL_DOWN)==X4_GAME_ACTION_NONE);
    page_46("ficha-descripcion-larga",&page,&model,pixels);
    detail.screenshot_count=0;
    x4_game_page_init(&page);
    page_46("ficha-sin-capturas",&page,&model,pixels);

    publish_background(dir,fortnite);
    model=(X4GamePageModel){.product_id=fortnite,.title="Fortnite",.available=true,
        .free_in_store=true,.category=assets.entries[ft].metadata.category,
        .studio=assets.entries[ft].metadata.publisher,.description=assets.entries[ft].metadata.description,
        .background=&assets.backgrounds[assets.active].raster};
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_RIGHT)==X4_GAME_ACTION_NONE);
    assert(x4_game_page_update(&page,&model,X4_GAME_INPUT_SELECT)==X4_GAME_ACTION_NONE);
    assert(page.qr_open);
    page_46("ficha-gratis-qr",&page,&model,pixels);
    x4_game_page_init(&page);
    model=(X4GamePageModel){.product_id="9NP1P1WFS0LB",.title="Halo Infinite",.available=true,
        .category="Shooter",.studio="Xbox Game Studios"};
    page_46("ficha-pago",&page,&model,pixels);
    for (unsigned i=0; i<30; ++i) {
        uint64_t start=clock_us();
        x4_game_page_draw(&page,&model,pixels);
        uint64_t end=clock_us();
        x4_ui_draw_metrics_record(X4_UI_OTHER,true,true,end-start,0,0,0,end,0);
    }
release:
    free(full.rgba);
    for(unsigned i=0;i<6;++i) free(shots[i].rgba);
}
#endif

static void options_46(uint32_t *pixels)
{
    X4PictureSettingsUi ui={0};
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    x4_apple_bg_draw(pixels);
    scene="calidad-imagen";
    x4_picture_settings_ui_draw(&ui,pixels);
    capture_46("calidad-imagen",pixels);
    ui.tab=2;
    x4_apple_bg_draw(pixels);
    scene="ajustes-iconos";
    x4_picture_settings_ui_draw(&ui,pixels);
    capture_46("ajustes-iconos",pixels);
}

#if !X4_CAPTURE46_BEFORE
#include "capture_tarea46_damage.h"
#endif

#ifndef X4_CAPTURE46_ENTRY
#define X4_CAPTURE46_ENTRY main
#endif
int X4_CAPTURE46_ENTRY(int argc,char **argv)
{
    bool benchmark=argc==5 && !strcmp(argv[4],"--bench");
    assert(argc==4 || benchmark);
    const char *dir=argv[1], *fixtures=argv[2];
    bool english=!strcmp(argv[3],"en");
    x4_lang_set(english?X4_LANG_EN:X4_LANG_ES,english?"en-US":"es-MX",english?"US":"MX");
    assert(!x4_font_init("assets/fonts/noto_sans.x4font"));
    assert(!x4_font_is_fallback());
    for (unsigned i=0; !benchmark && i<256; ++i) {
        double value=i/255.0;
        luminance[i]=(float)(value<=0.04045?value/12.92:pow((value+0.055)/1.055,2.4));
    }
    load_fixtures(dir);
    uint32_t *raw=malloc((pixel_count+2*GUARDED_WORDS)*sizeof(*raw));
    assert(raw);
    uint32_t *pixels=raw+GUARDED_WORDS;
    for(size_t i=0;i<pixel_count+2*GUARDED_WORDS;++i)raw[i]=sentinel;
    x4_ui_draw_metrics_reset();
    main_menu_46(dir,pixels,benchmark);
    x4_catalog_gui_home_prewarm(pixels);
    set_tab(X4_CAT_TAB_DISCOVER);
    assert(x4_catalog_browser_home_tick(&browser,&catalog,100));
    assert(!strcmp(browser.home_background_id,forza));
    if (benchmark) {
        x4_ui_draw_metrics_reset();
        x4_catalog_gui_home_invalidate();
        publish_background(dir,forza);
        browser.hero_focused=true;
        draw_tab(pixels);
    } else tab_46("descubrir-forza",X4_CAT_TAB_DISCOVER,forza,dir,pixels);
    uint64_t samples[BENCH46_SAMPLES];
    const unsigned limit = benchmark ? BENCH46_WARMUP + BENCH46_SAMPLES : 60;
    for (unsigned i=0; i<limit; ++i) {
        browser.hero_focused=(i & 1u)!=0;
        uint64_t start=clock_us();
        draw_tab(pixels);
        uint64_t end=clock_us();
        if (benchmark && i < BENCH46_WARMUP) continue;
        x4_ui_draw_metrics_record(X4_UI_DISCOVER,true,true,end-start,0,0,0,end,0);
        if (benchmark) samples[i - BENCH46_WARMUP] = end - start;
    }
    if (benchmark) {
        bench46_report(X4_UI_DISCOVER, "descubrir", samples);
#if !X4_CAPTURE46_BEFORE
        game_46(dir,fixtures,pixels,english,true);
#endif
        check_canvas(pixels);
    } else {
#if !X4_CAPTURE46_BEFORE
        assert_discover_focus_damage46(pixels);
#endif
        tab_46("catalogo",X4_CAT_TAB_CATALOG,forza,dir,pixels);
        tab_46("recientes",X4_CAT_TAB_RECENTS,fortnite,dir,pixels);
        scene="aviso-tiempo-nube";
        x4_cloud_time_confirm_draw(pixels);
        capture_46("aviso-tiempo-nube",pixels);
        options_46(pixels);
        publish_background(dir,forza);
        scene="carga";
        assert(x4_loading_screen_begin("Forza Horizon 5",&assets.backgrounds[assets.active].raster,1000000));
        assert(x4_loading_screen_draw(pixels,1900000));
        x4_loading_screen_end();
        capture_46("carga",pixels);
#if !X4_CAPTURE46_BEFORE
        game_46(dir,fixtures,pixels,english,false);
#endif
        for (unsigned i=0; i<3; ++i) {
            enum X4UiScreen screen=(enum X4UiScreen[]){X4_UI_MENU,X4_UI_DISCOVER,X4_UI_OTHER}[i];
            X4UiDrawMetricView view;
            if(x4_ui_draw_metrics_snapshot(screen,true,&view) && view.draw.n)
                fprintf(stderr,"TIMING46 %s %llu %llu %llu %llu\n",
                    (const char *[]){"menu","descubrir","ficha"}[i],
                    (unsigned long long)view.draw.n,(unsigned long long)view.draw.p50_us,
                    (unsigned long long)view.draw.p95_us,(unsigned long long)view.draw.max_us);
        }
        fprintf(stderr,"FOOTER46_COUNT %u\n",measured_footers);
    }
#if X4_CAPTURE46_BEFORE
    (void)fixtures;
#endif
    x4_catalog_gui_home_invalidate();
    x4_screen_home_invalidate();
    for(unsigned i=0;i<assets.count;++i)
        for(unsigned j=0;j<X4_COVER_SURFACES;++j)free(assets.entries[i].surface[j].rgba);
    for(unsigned i=0;i<2;++i)free(assets.backgrounds[i].raster.rgba);
    free(raw);
    x4_font_close();
    return 0;
}
