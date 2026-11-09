/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_CAPTURE46_ENTRY capture_tarea46_entry
#include "capture_tarea46.c"

static X4GameDetails details;
static X4CoverRaster shots[X4_GAME_SCREENSHOT_MAX], full_shot;

static X4GamePageModel fixture46b(const char *dir, const char *fixtures)
{
    static X4GameProductDetails parsed;
    X4ProductMetadata meta;
    size_t bytes;
    unsigned char *json = read_file(fixtures, "forza.json", &bytes);
    assert(json && !x4_product_metadata_parse_details((const char *)json, bytes,
        forza, &meta, &parsed));
    free(json);
    details = parsed.details;
    assert(details.screenshot_count == X4_GAME_SCREENSHOT_MAX);
    for (unsigned i=0; i<X4_GAME_SCREENSHOT_MAX; ++i) {
        char file[32];
        snprintf(file, sizeof(file), "shot-%u.jpg", i);
        shots[i] = shot_raster(fixtures, file, X4_COVER_DETAIL);
    }
    full_shot = shot_raster(fixtures, "shot-full-in-game.jpg", X4_COVER_BACKGROUND);
    publish_background(dir, forza);
    return (X4GamePageModel){
        .product_id=forza, .title="Forza Horizon 5", .available=true, .entitled=true,
        .details=&details, .background=&assets.backgrounds[assets.active].raster
    };
}

enum DamageKind { FULL, PARTIAL, IDLE };

static void frame46b(const char *name, uint32_t *p, uint32_t *previous,
    const X4GamePage *page, const X4GamePageModel *model, uint64_t generation,
    bool persistent, enum DamageKind want, bool emit)
{
#if X4_UI46B_BEFORE
    (void)previous; (void)generation; (void)persistent; (void)want;
    x4_game_page_draw(page, model, p);
#else
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX];
    size_t n = x4_game_page_draw_cached(page, model, generation, p, persistent, damage);
    assert(want == FULL ? n == SIZE_MAX : want == IDLE ? !n : n && n <= X4_GAME_PAGE_DAMAGE_MAX);
    if (n == SIZE_MAX) memcpy(previous, p, pixel_count * sizeof(*p));
    else for (size_t i=0; i<n; ++i) {
        const X4UiRect r = damage[i];
        assert(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= WIDTH && r.y + r.h <= HEIGHT);
        for (int y=r.y; y<r.y+r.h; ++y) {
            size_t start = (size_t)y * WIDTH + r.x;
            memcpy(previous + start, p + start, (size_t)r.w * sizeof(*p));
        }
    }
    if (memcmp(previous, p, pixel_count * sizeof(*p))) {
        for (size_t k=0; k<pixel_count; ++k) if (previous[k] != p[k]) {
            fprintf(stderr, "%s: fuera de los rectangulos x=%zu y=%zu actual=%08x esperado=%08x\n",
                name,k%WIDTH,k/WIDTH,p[k],previous[k]);
            break;
        }
        abort();
    }
    x4_game_page_draw(page, model, previous);
    if (memcmp(previous, p, pixel_count * sizeof(*p))) {
        for (size_t k=0; k<pixel_count; ++k) if (previous[k] != p[k]) {
            fprintf(stderr, "%s: pixel x=%zu y=%zu cache=%08x referencia=%08x\n",
                name,k%WIDTH,k/WIDTH,p[k],previous[k]);
            break;
        }
        abort();
    }
#endif
    check_canvas(p);
    if (emit) capture(name, p);
}

static void move46b(X4GamePage *page, const X4GamePageModel *model, unsigned input)
{
    assert(x4_game_page_update(page, model, input) == X4_GAME_ACTION_NONE);
}

static void pixels46b(uint32_t *p, X4GamePageModel model, bool emit)
{
    uint32_t *previous = malloc(pixel_count * sizeof(*p));
    assert(previous);
    X4GamePage page;
    x4_game_page_init(&page);
    uint64_t generation=1;
#define FRAME(label,expected) frame46b(label,p,previous,&page,&model,generation,true,expected,emit)
    FRAME("abrir", FULL);
    FRAME("quieta", IDLE);
    model.screenshots[0] = &shots[0];
    FRAME("llega-una", PARTIAL);
    snprintf(details.title, sizeof(details.title), "Forza Horizon 5: Titulo actualizado");
    FRAME("llega-titulo", PARTIAL);
    for (unsigned i=0; i<X4_GAME_SCREENSHOT_MAX; ++i) model.screenshots[i] = &shots[i];
    FRAME("llegan-restantes", PARTIAL);
    move46b(&page, &model, X4_GAME_INPUT_DOWN);
    FRAME("foco-1", PARTIAL);
    for (unsigned i=1; i<X4_GAME_SCREENSHOT_MAX; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "foco-%u", i+1);
        move46b(&page, &model, X4_GAME_INPUT_RIGHT);
        FRAME(name, PARTIAL);
        if (i==3) {
            model.screenshots[i-1] = NULL;
            FRAME("vecina-faltante", PARTIAL);
            model.screenshots[i-1] = &shots[i-1];
            FRAME("vecina-repuesta", PARTIAL);
        }
    }
    move46b(&page, &model, X4_GAME_INPUT_SELECT);
    FRAME("pantalla-miniatura", FULL);
    FRAME("pantalla-quieta", IDLE);
    model.fullscreen = &full_shot;
    FRAME("pantalla-imagen", FULL);
    FRAME("pantalla-imagen-quieta", IDLE);
    move46b(&page, &model, X4_GAME_INPUT_BACK);
    FRAME("pantalla-regresar", FULL);
    move46b(&page, &model, X4_GAME_INPUT_LEFT);
    FRAME("izquierda", PARTIAL);
    move46b(&page, &model, X4_GAME_INPUT_UP);
    FRAME("foco-jugar", PARTIAL);
    (void)snprintf(details.description, sizeof(details.description),
        "Linea 1\nLinea 2\nLinea 3\nLinea 4\nLinea 5\nLinea 6\nLinea 7\nLinea 8");
    FRAME("descripcion", PARTIAL);
    move46b(&page, &model, X4_GAME_INPUT_SCROLL_DOWN);
    FRAME("desplazar", PARTIAL);
    model.notice = "Aviso de prueba";
    FRAME("aviso", PARTIAL);
    model.notice = NULL;
    model.busy = true;
    FRAME("ocupada", PARTIAL);
    model.busy = false;
    model.entitled = false;
    model.free_in_store = true;
    FRAME("tienda", PARTIAL);
    move46b(&page, &model, X4_GAME_INPUT_RIGHT);
    FRAME("foco-tienda", PARTIAL);
    move46b(&page, &model, X4_GAME_INPUT_SELECT);
    FRAME("qr", FULL);
    FRAME("qr-quieto", IDLE);
    move46b(&page, &model, X4_GAME_INPUT_BACK);
    FRAME("qr-regresar", FULL);
    memset(p, 0x65, pixel_count * sizeof(*p));
    frame46b("overlay",p,previous,&page,&model,generation,false,FULL,emit);
    FRAME("overlay-regresar", FULL);
    FRAME("overlay-quieta", IDLE);
    ++generation;
    model.background->rgba[320] ^= 0x0000ffu;
    FRAME("cambia-fondo", FULL);
#undef FRAME
    free(previous);
}

#if !X4_UI46B_BEFORE
static void bench_game46b(uint32_t *p, X4GamePageModel model)
{
    static const unsigned inputs[] = {
        X4_GAME_INPUT_DOWN, X4_GAME_INPUT_RIGHT, X4_GAME_INPUT_RIGHT, X4_GAME_INPUT_RIGHT,
        X4_GAME_INPUT_RIGHT, X4_GAME_INPUT_RIGHT, X4_GAME_INPUT_LEFT, X4_GAME_INPUT_LEFT,
        X4_GAME_INPUT_LEFT, X4_GAME_INPUT_LEFT, X4_GAME_INPUT_LEFT, X4_GAME_INPUT_UP
    };
    for (unsigned i=0; i<X4_GAME_SCREENSHOT_MAX; ++i) model.screenshots[i] = &shots[i];
    X4GamePage page;
    x4_game_page_init(&page);
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX];
    assert(x4_game_page_draw_cached(&page,&model,1,p,true,damage)==SIZE_MAX);
    uint64_t samples[BENCH46_SAMPLES];
    for (unsigned i=0; i<BENCH46_WARMUP+BENCH46_SAMPLES; ++i) {
        move46b(&page,&model,inputs[i%(sizeof(inputs)/sizeof(inputs[0]))]);
        uint64_t start=clock_us();
        size_t n=x4_game_page_draw_cached(&page,&model,1,p,true,damage);
        uint64_t end=clock_us();
        assert(n && n<=X4_GAME_PAGE_DAMAGE_MAX);
        if (i<BENCH46_WARMUP) continue;
        samples[i-BENCH46_WARMUP]=end-start;
        x4_ui_draw_metrics_record(X4_UI_GAME_PAGE,true,true,end-start,0,0,0,end,0);
    }
    bench46_report(X4_UI_GAME_PAGE,"ficha",samples);
}
#endif

int main(int argc, char **argv)
{
    assert(argc==5);
    const char *dir=argv[1], *fixtures=argv[2];
    const bool english=!strcmp(argv[3],"en"), bench=!strcmp(argv[4],"--bench");
    x4_lang_set(english?X4_LANG_EN:X4_LANG_ES,english?"en-US":"es-MX",english?"US":"MX");
    assert(!x4_font_init("assets/fonts/noto_sans.x4font") && !x4_font_is_fallback());
    load_fixtures(dir);
    uint32_t *raw=malloc((pixel_count+2*GUARDED_WORDS)*sizeof(*raw));
    assert(raw);
    uint32_t *p=raw+GUARDED_WORDS;
    for (size_t i=0; i<pixel_count+2*GUARDED_WORDS; ++i) raw[i]=sentinel;
    x4_ui_draw_metrics_reset();
    if (bench) {
#if X4_UI46B_BEFORE
        main_menu_46(dir,p,true);
#else
        bench_game46b(p,fixture46b(dir,fixtures));
#endif
    } else {
        pixels46b(p,fixture46b(dir,fixtures),!strcmp(argv[4],"--pixels"));
        fputs("UI46B_PIXELS_OK\n",stderr);
    }
    check_canvas(p);
#if !X4_UI46B_BEFORE
    x4_game_page_cache_dispose();
#endif
    for (unsigned i=0; i<X4_GAME_SCREENSHOT_MAX; ++i) free(shots[i].rgba);
    free(full_shot.rgba);
    for(unsigned i=0;i<assets.count;++i)
        for(unsigned j=0;j<X4_COVER_SURFACES;++j) free(assets.entries[i].surface[j].rgba);
    for(unsigned i=0;i<2;++i) free(assets.backgrounds[i].raster.rgba);
    free(raw);
    x4_font_close();
    return 0;
}
