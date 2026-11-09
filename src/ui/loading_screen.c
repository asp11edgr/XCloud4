/* SPDX-License-Identifier: GPL-3.0-only */
#include "loading_screen.h"
#include "apple_ui.h"
#include "font.h"
#include "ui2_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#else
#include <time.h>
#endif

#define WIDTH 1920
#define HEIGHT 1080
#define PIXELS ((size_t)WIDTH*HEIGHT)
#define QW 240
#define QH 135
#define DX 510
#define DY 230
#define DW 900
#define DH 680
#define AX 800
#define AY 300
#define AW 320
#define AH 180
#define FRAME_US 33333ULL
#define FADE_US 160000ULL
#define HIST_BINS 256
#define HIST_STEP 25ULL

enum LoadingRegion { REGION_STAGE, REGION_STEPS, REGION_PROGRESS, REGION_NOTICE, REGION_COUNT };
static const struct { int y,height; } regions[REGION_COUNT] = {
    [REGION_STAGE]={624,74}, [REGION_STEPS]={710,110},
    [REGION_PROGRESS]={832,46}, [REGION_NOTICE]={882,28}
};

static struct {
    uint32_t *backdrop,*frame,*base_zone;
    uint32_t quarter[QW*QH],blur[QW*QH];
    uint32_t *destinations[4];
    unsigned destination_generation[4],generation;
    unsigned destination_region_generation[4][REGION_COUNT],region_generation[REGION_COUNT];
    unsigned destination_fade_rank[4];
    bool destination_fade_seeded[4];
    uint64_t begin_us,wait_us,first_us,animation_tick;
    uint64_t render_hist[HIST_BINS],render_total_us;
    uint64_t frame_hist[HIST_BINS],frame_total_us,pending_static_us;
    uint64_t fade_hist[HIST_BINS],fade_total_us;
    unsigned visual_percent,visual_seconds,visual_queue_seconds,visual_eta_seconds;
    bool visual_queue,visual_eta,visual_delayed;
    enum X4LoadingStage visual_stage;
    enum X4Lang language;
    char title[128];
    bool dirty,logged,static_ready,wait_started;
    X4LoadingView view;
} loading;
/* At most30x17 blocks share one Bayer rank. Lists are built once, before
 * playback. Incremental fade touches only the new ranks in each UI buffer. */
enum { FADE_RANKS=64, FADE_BLOCK=8, FADE_BLOCKS_PER_RANK=510 };
static uint32_t fade_offsets[FADE_RANKS][FADE_BLOCKS_PER_RANK];
static unsigned fade_counts[FADE_RANKS];
static bool fade_map_ready;
static void prepare_fade_map(void)
{
    if(fade_map_ready)return;
    static const unsigned char ranks[64]={0,48,12,60,3,51,15,63,32,16,44,28,35,19,47,31,8,56,4,52,11,59,7,55,40,24,36,20,43,27,39,23,2,50,14,62,1,49,13,61,34,18,46,30,33,17,45,29,10,58,6,54,9,57,5,53,42,26,38,22,41,25,37,21};
    for(unsigned by=0;by<HEIGHT/FADE_BLOCK;++by)for(unsigned bx=0;bx<WIDTH/FADE_BLOCK;++bx){
        unsigned rank=ranks[(by%8)*8+bx%8];
        fade_offsets[rank][fade_counts[rank]++]=(by*FADE_BLOCK)*WIDTH+bx*FADE_BLOCK;
    }
    fade_map_ready=true;
}

static uint64_t clock_us(void)
{
#ifdef X4_OPENORBIS
    return sceKernelGetProcessTime();
#else
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000ULL+(uint64_t)t.tv_nsec/1000ULL;
#endif
}
static void record_render(uint64_t elapsed,bool fade)
{
    unsigned b=(unsigned)(elapsed/HIST_STEP);
    if(b>=HIST_BINS)b=HIST_BINS-1;
    ++loading.render_hist[b];++loading.view.render_calls;
    loading.render_total_us+=elapsed;
    if(elapsed>loading.view.render_max_us)loading.view.render_max_us=elapsed;
    /* Charge all state-dependent preparation to the next displayed frame,
     * including the first-picture completion. Never hide its cost inside
     * update merely because update precedes draw in the main loop. */
    uint64_t total=elapsed+loading.pending_static_us;loading.pending_static_us=0;
    unsigned fb=(unsigned)(total/HIST_STEP);if(fb>=HIST_BINS)fb=HIST_BINS-1;
    ++loading.frame_hist[fb];loading.frame_total_us+=total;
    if(total>loading.view.frame_total_max_us)loading.view.frame_total_max_us=total;
    if(fade){
        ++loading.fade_hist[fb];++loading.view.fade_frames;loading.fade_total_us+=total;
        if(total>loading.view.fade_max_us)loading.view.fade_max_us=total;
    }
}
static uint64_t hist_percentile(const uint64_t hist[HIST_BINS],uint64_t count,uint64_t max,unsigned pct)
{
    if(!count)return 0;
    uint64_t need=(count*pct+99)/100,seen=0;
    for(unsigned i=0;i<HIST_BINS;++i){
        seen+=hist[i];
        if(seen>=need)return i==HIST_BINS-1?max:(i+1)*HIST_STEP;
    }
    return max;
}
static uint64_t percentile(unsigned pct)
{return hist_percentile(loading.render_hist,loading.view.render_calls,loading.view.render_max_us,pct);}
static uint64_t frame_percentile(unsigned pct)
{return hist_percentile(loading.frame_hist,loading.view.render_calls,loading.view.frame_total_max_us,pct);}
static uint64_t fade_percentile(unsigned pct)
{return hist_percentile(loading.fade_hist,loading.view.fade_frames,loading.view.fade_max_us,pct);}
static void log_metrics(void)
{
    if(loading.logged||!loading.view.background_preparations)return;
    printf("XCloud4 UI loading: render_n=%llu p50_us=%llu p95_us=%llu max_us=%llu frame_total_p50_us=%llu frame_total_p95_us=%llu frame_total_max_us=%llu background_us=%llu static_n=%llu static_max_us=%llu full_copies=%llu region_copies=%llu fade_frames=%llu fade_p50_us=%llu fade_p95_us=%llu fade_max_us=%llu first_native_us=%llu transition_ready_us=%llu transition_wait_us=%llu fade_seed_copies=%llu fade_cold_full_copies=%llu fade_blocks=%llu (UI-only measurements; frame_total includes state preparation; transition clock does not need another AU)\n",
        (unsigned long long)loading.view.render_calls,(unsigned long long)percentile(50),(unsigned long long)percentile(95),
        (unsigned long long)loading.view.render_max_us,(unsigned long long)frame_percentile(50),(unsigned long long)frame_percentile(95),(unsigned long long)loading.view.frame_total_max_us,(unsigned long long)loading.view.background_us,
        (unsigned long long)loading.view.static_preparations,(unsigned long long)loading.view.static_max_us,
        (unsigned long long)loading.view.full_buffer_initializations,(unsigned long long)loading.view.region_copies,(unsigned long long)loading.view.fade_frames,
        (unsigned long long)fade_percentile(50),(unsigned long long)fade_percentile(95),(unsigned long long)loading.view.fade_max_us,
        (unsigned long long)loading.view.first_picture_us,(unsigned long long)loading.view.transition_ready_us,(unsigned long long)loading.view.transition_wait_us,
        (unsigned long long)loading.view.fade_seed_copies,(unsigned long long)loading.view.fade_full_initializations,(unsigned long long)loading.view.fade_blocks_written);
    loading.logged=true;
}
static void copy_region(uint32_t *dst,const uint32_t *src,int x,int y,int w,int h)
{
    for(int row=y;row<y+h;++row)
        memcpy(dst+(size_t)row*WIDTH+x,src+(size_t)row*WIDTH+x,(size_t)w*4);
}
enum { BACKDROP_DIM = 61 };

static uint32_t dim_rgb(unsigned r,unsigned g,unsigned b)
{
    /* Bake one opacity over the entire blurred image before caching it. */
    return X4_RGB(r*BACKDROP_DIM/255,g*BACKDROP_DIM/255,b*BACKDROP_DIM/255);
}
static bool valid_game(const X4CoverRaster *r)
{
    if(!r||!r->rgba||!r->width||!r->height||r->width>4096||r->height>4096)return false;
    uint64_t a=(uint64_t)r->width*9,b=(uint64_t)r->height*16;
    uint64_t delta=a>b?a-b:b-a;
    return delta*100<=b; /* at most one percent aspect-ratio deviation */
}
static void prepare_backdrop(const X4CoverRaster *game)
{
    loading.view.has_game_background=valid_game(game);
    if(loading.view.has_game_background){
        for(unsigned y=0;y<QH;++y)for(unsigned x=0;x<QW;++x){
            unsigned sx=x*game->width/QW,sy=y*game->height/QH;
            loading.quarter[(size_t)y*QW+x]=game->rgba[(size_t)sy*game->width+sx];
        }
        /* One bounded separable box blur on the 1/8-scale image. */
        for(int y=0;y<QH;++y)for(int x=0;x<QW;++x){
            unsigned rr=0,gg=0,bb=0;
            for(int k=-6;k<=6;++k){int xx=x+k;if(xx<0)xx=0;if(xx>=QW)xx=QW-1;
                uint32_t c=loading.quarter[(size_t)y*QW+xx];rr+=c&255;gg+=(c>>8)&255;bb+=(c>>16)&255;}
            loading.blur[(size_t)y*QW+x]=X4_RGB(rr/13,gg/13,bb/13);
        }
        for(int y=0;y<QH;++y)for(int x=0;x<QW;++x){
            unsigned rr=0,gg=0,bb=0;
            for(int k=-6;k<=6;++k){int yy=y+k;if(yy<0)yy=0;if(yy>=QH)yy=QH-1;
                uint32_t c=loading.blur[(size_t)yy*QW+x];rr+=c&255;gg+=(c>>8)&255;bb+=(c>>16)&255;}
            loading.quarter[(size_t)y*QW+x]=dim_rgb(rr/13,gg/13,bb/13);
        }
        for(unsigned y=0;y<HEIGHT;++y)for(unsigned x=0;x<WIDTH;++x)
            loading.backdrop[(size_t)y*WIDTH+x]=loading.quarter[(size_t)(y/8)*QW+x/8];
    }else x4_apple_bg_draw(loading.backdrop);
    /* All backing text is cached; no public game logo or Xbox artwork. */
    (void)x4_ui2_brand(loading.backdrop,X4_UI_SAFE_LEFT,55,32);
    ++loading.view.background_preparations;
}
static void centered_text_at(uint32_t *p,int center,int y,int size,int weight,
    const char *text,int max_width,uint32_t color)
{
    char fit[256];
    x4_text_fit_utf8(fit,sizeof(fit),size,weight,text,max_width);
    int width=x4_text_width_px(size,weight,fit);
    x4_text_px(p,center-width/2,y,size,weight,fit,color);
}

static void centered_text(uint32_t *p,int y,int size,int weight,const char *text,uint32_t color)
{
    centered_text_at(p,WIDTH/2,y,size,weight,text,820,color);
}

enum X4LoadingStep x4_loading_screen_step(enum X4LoadingStage stage)
{
    switch(stage){
    case X4_LOADING_REQUEST:return X4_LOADING_STEP_SESSION;
    case X4_LOADING_RESOURCES:
    case X4_LOADING_AUTHORIZE:return X4_LOADING_STEP_SERVER;
    case X4_LOADING_CONNECT:
    case X4_LOADING_CONNECTED:return X4_LOADING_STEP_OPTIMIZE;
    case X4_LOADING_FIRST_PICTURE:return X4_LOADING_STEP_READY;
    case X4_LOADING_COMPLETE:return X4_LOADING_STEPS;
    default:return X4_LOADING_STEP_INVALID;
    }
}

static const char *stage_text(char out[160])
{
    switch(loading.view.stage){
    case X4_LOADING_REQUEST:return x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_REQUEST);
    case X4_LOADING_RESOURCES:return x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_WAIT_RESOURCES);
    case X4_LOADING_AUTHORIZE:return x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_AUTHORIZE);
    case X4_LOADING_CONNECT:return x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_CONNECT);
    case X4_LOADING_CONNECTED:return x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_CONNECTED);
    case X4_LOADING_COMPLETE:return x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_READY);
    default:
        snprintf(out,160,x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_SESSION_PREPARING_FMT),loading.view.waiting_seconds);
        return out;
    }
}
/* The ring stencil is independent of the backdrop. Rasterize its annular
 * sectors once, then animate with sparse stores inside the existing zone. */
enum { RING_SEGMENTS=32, RING_OUTER=72, RING_INNER=64,
    RING_SECTOR_CAP=160, RING_CX=WIDTH/2, RING_CY=390 };
_Static_assert(RING_CX-RING_OUTER>=AX&&RING_CX+RING_OUTER<AX+AW&&
    RING_CY-RING_OUTER>=AY&&RING_CY+RING_OUTER<AY+AH,
    "Animated ring stays within the published damage region");
static uint32_t ring_offsets[RING_SEGMENTS][RING_SECTOR_CAP];
static uint16_t ring_counts[RING_SEGMENTS];
static bool ring_ready;

static void prepare_ring(void)
{
    if(ring_ready)return;
    for(int y=-RING_OUTER;y<=RING_OUTER;++y)for(int x=-RING_OUTER;x<=RING_OUTER;++x){
        int r2=x*x+y*y;
        if(r2<RING_INNER*RING_INNER||r2>RING_OUTER*RING_OUTER)continue;
        float angle=atan2f((float)x,(float)-y);
        unsigned sector=(unsigned)((angle+6.28318530718f)*RING_SEGMENTS/6.28318530718f)%RING_SEGMENTS;
        unsigned n=ring_counts[sector];
        if(n<RING_SECTOR_CAP){
            ring_offsets[sector][n]=(uint32_t)((RING_CY+y)*WIDTH+RING_CX+x);
            ring_counts[sector]=(uint16_t)(n+1);
        }
    }
    ring_ready=true;
}

static void paint_ring(uint32_t *p,unsigned phase,unsigned count,uint32_t color)
{
    for(unsigned i=0;i<count;++i){
        unsigned sector=(phase+i)%RING_SEGMENTS;
        for(unsigned j=0;j<ring_counts[sector];++j)p[ring_offsets[sector][j]]=color;
    }
}

static void animation(unsigned phase)
{
    paint_ring(loading.frame,phase%RING_SEGMENTS,
        loading.view.first_picture?RING_SEGMENTS:11,X4_UI2_ACCENT);
}

static void prepare_fixed(void)
{
    const int cloud_height=48;
    centered_text(loading.backdrop,242,64,X4_FONT_BOLD,loading.title,X4_UI2_TEXT);
    x4_apple_outer_glow(loading.backdrop,RING_CX-74,RING_CY-74,148,148,74,10,
        X4_RGBA(54,246,135,36));
    x4_apple_fill_round_rect(loading.backdrop,RING_CX-60,RING_CY-60,120,120,60,
        X4_RGBA(6,18,26,222));
    paint_ring(loading.backdrop,0,RING_SEGMENTS,X4_UI2_BORDER);
    x4_ui2_cloud(loading.backdrop,RING_CX-x4_ui2_cloud_width(cloud_height)/2,
        RING_CY-cloud_height/2,cloud_height,X4_UI2_TEXT);
    centered_text(loading.backdrop,510,44,X4_FONT_BOLD,
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_CONNECT_CLOUD),X4_UI2_TEXT);
    centered_text(loading.backdrop,565,26,X4_FONT_REGULAR,
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_PREPARE),X4_UI2_TEXT);
    x4_apple_fill_round_rect(loading.backdrop,620,840,680,6,3,X4_UI2_BORDER);
    const char *cancel=x4_tr_for_lang(loading.language, X4_TR_AUTH_REMEMBER_WARNING_CANCEL);
    int hint_width=38+x4_text_width_px(22,X4_FONT_REGULAR,cancel);
    x4_apple_draw_footer_hint(loading.backdrop,(WIDTH-hint_width)/2,X4_UI_FOOTER_Y,X4_PAD_CIRCLE,cancel);
}

static void restore_dynamic(enum LoadingRegion region)
{
    int y=regions[region].y, end=y+regions[region].height;
    for(int row=y;row<end;++row)
        memcpy(loading.backdrop+(size_t)row*WIDTH+DX,
            loading.base_zone+(size_t)(row-DY)*DW,(size_t)DW*4);
}

static void draw_stage(void)
{
    char label[192];
    if(!loading.view.waiting_for_resources){
        centered_text(loading.backdrop,648,26,X4_FONT_REGULAR,
            stage_text(label),X4_UI2_MUTED);
        return;
    }
    centered_text(loading.backdrop,627,28,X4_FONT_SEMIBOLD,
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_QUEUE),X4_UI2_TEXT);
    if(loading.view.queue_eta_available)
        snprintf(label,sizeof(label),x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_QUEUE_ETA_FMT),
            loading.view.queue_elapsed_seconds/60,loading.view.queue_elapsed_seconds%60,
            loading.view.queue_eta_seconds/60,loading.view.queue_eta_seconds%60);
    else snprintf(label,sizeof(label),x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_QUEUE_TIME_FMT),
        loading.view.queue_elapsed_seconds/60,loading.view.queue_elapsed_seconds%60);
    centered_text(loading.backdrop,665,22,X4_FONT_REGULAR,label,X4_UI2_MUTED);
}

static void draw_check(uint32_t *p,int x,int y)
{
    for(int i=0;i<7;++i)x4_apple_fill_round_rect(p,x-9+i,y-1+i,4,4,2,X4_UI2_INK);
    for(int i=0;i<13;++i)x4_apple_fill_round_rect(p,x-3+i,y+5-i,4,4,2,X4_UI2_INK);
}

static void draw_steps(enum X4LoadingStep current)
{
    const char *labels[X4_LOADING_STEPS]={
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_STEP_SESSION),
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_STEP_SERVER),
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_STEP_OPTIMIZE),
        x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_STEP_READY)
    };
    enum { FIRST_X=622, STEP_X=225, Y=746, DIAMETER=38 };
    uint32_t *p=loading.backdrop;
    x4_apple_fill_round_rect(p,FIRST_X,Y-1,3*STEP_X,3,1,X4_UI2_BORDER);
    if(current>X4_LOADING_STEP_SESSION){
        int reached=current<X4_LOADING_STEPS?(int)current:X4_LOADING_STEPS-1;
        x4_apple_fill_round_rect(p,FIRST_X,Y-1,reached*STEP_X,3,1,X4_UI2_ACCENT);
    }
    for(unsigned i=0;i<X4_LOADING_STEPS;++i){
        int x=FIRST_X+(int)i*STEP_X;
        bool done=i<(unsigned)current, active=i==(unsigned)current;
        int left=x-DIAMETER/2,top=Y-DIAMETER/2;
        x4_apple_fill_round_rect(p,left,top,DIAMETER,DIAMETER,DIAMETER/2,
            done?X4_UI2_ACCENT:X4_UI2_BACKGROUND);
        if(done)draw_check(p,x,Y);
        else{
            x4_apple_stroke_round_rect(p,left,top,DIAMETER,DIAMETER,DIAMETER/2,
                active?3.0f:2.0f,active?X4_UI2_ACCENT:X4_UI2_BORDER);
            char index[]={(char)('1'+i),'\0'};
            centered_text_at(p,x,Y-12,20,X4_FONT_SEMIBOLD,index,DIAMETER,
                active?X4_UI2_ACCENT:X4_UI2_MUTED);
        }
        centered_text_at(p,x,780,active?20:18,active?X4_FONT_SEMIBOLD:X4_FONT_REGULAR,
            labels[i],246,active||done?X4_UI2_TEXT:X4_UI2_MUTED);
    }
}

static void draw_progress(void)
{
    int filled=(int)(680u*loading.view.percent/100u);
    if(filled>0)x4_apple_fill_round_rect(loading.backdrop,620,840,filled,6,
        filled<6?filled/2:3,X4_UI2_ACCENT);
    char percent[16];
    snprintf(percent,sizeof(percent),"%u %%",loading.view.percent);
    centered_text(loading.backdrop,852,20,X4_FONT_SEMIBOLD,percent,X4_UI2_TEXT);
}

static void draw_notice(void)
{
    if(loading.view.waiting_for_resources)
        centered_text(loading.backdrop,885,18,X4_FONT_REGULAR,
            x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_SESSION_QUEUE_CANCEL),
            X4_UI2_MUTED);
    else if(loading.view.delayed_notice)
        centered_text(loading.backdrop,885,18,X4_FONT_REGULAR,
            x4_tr_for_lang(loading.language, X4_TR_50_SCREEN_LOAD_CANCEL_ANYTIME),X4_UI2_MUTED);
}

static void prepare_static(void)
{
    if(!loading.view.active||loading.view.stage==X4_LOADING_ERROR)return;
    enum X4LoadingStep step=x4_loading_screen_step(loading.view.stage);
    bool first=!loading.static_ready;
    const bool update[REGION_COUNT]={
        [REGION_STAGE]=first||loading.visual_stage!=loading.view.stage||
            loading.visual_seconds!=loading.view.waiting_seconds||
            loading.visual_queue!=loading.view.waiting_for_resources||
            loading.visual_queue_seconds!=loading.view.queue_elapsed_seconds||
            loading.visual_eta!=loading.view.queue_eta_available||
            loading.visual_eta_seconds!=loading.view.queue_eta_seconds,
        [REGION_STEPS]=first||step!=x4_loading_screen_step(loading.visual_stage),
        [REGION_PROGRESS]=first||loading.visual_percent!=loading.view.percent,
        [REGION_NOTICE]=first||loading.visual_queue!=loading.view.waiting_for_resources||
            loading.visual_delayed!=loading.view.delayed_notice
    };
    if(!update[REGION_STAGE]&&!update[REGION_STEPS]&&!update[REGION_PROGRESS]&&!update[REGION_NOTICE])return;
    uint64_t before=clock_us();
    for(unsigned i=0;i<REGION_COUNT;++i)if(update[i]){
        restore_dynamic((enum LoadingRegion)i);
        switch(i){
        case REGION_STAGE:draw_stage();break;
        case REGION_STEPS:draw_steps(step);break;
        case REGION_PROGRESS:draw_progress();break;
        case REGION_NOTICE:draw_notice();break;
        }
        copy_region(loading.frame,loading.backdrop,DX,regions[i].y,DW,regions[i].height);
        ++loading.region_generation[i];
    }
    loading.visual_percent=loading.view.percent;loading.visual_seconds=loading.view.waiting_seconds;
    loading.visual_stage=loading.view.stage;loading.visual_queue=loading.view.waiting_for_resources;
    loading.visual_queue_seconds=loading.view.queue_elapsed_seconds;
    loading.visual_eta=loading.view.queue_eta_available;loading.visual_eta_seconds=loading.view.queue_eta_seconds;
    loading.visual_delayed=loading.view.delayed_notice;loading.static_ready=true;
    uint64_t elapsed=clock_us()-before;++loading.view.static_preparations;
    loading.pending_static_us+=elapsed;
    if(elapsed>loading.view.static_max_us)loading.view.static_max_us=elapsed;
}

static void render_frame(uint64_t now)
{
    unsigned phase=(unsigned)((now>=loading.begin_us?now-loading.begin_us:0)/FRAME_US);
    copy_region(loading.frame,loading.backdrop,AX,AY,AW,AH);
    animation(phase);
    ++loading.generation;
    loading.animation_tick=now/FRAME_US;loading.dirty=false;
}
bool x4_loading_screen_begin(const char *title,const X4CoverRaster *game,uint64_t now)
{
    x4_loading_screen_end();memset(&loading,0,sizeof(loading));
    loading.backdrop=malloc(PIXELS*4);loading.frame=malloc(PIXELS*4);
    loading.base_zone=malloc((size_t)DW*DH*4);
    if(!loading.backdrop||!loading.frame||!loading.base_zone){free(loading.backdrop);free(loading.frame);free(loading.base_zone);loading.backdrop=loading.frame=loading.base_zone=NULL;return false;}
    loading.language=x4_lang_current();loading.begin_us=now;
    loading.view.active=true;loading.view.stage=X4_LOADING_REQUEST;loading.view.percent=5;
    snprintf(loading.title,sizeof(loading.title),"%s",title&&title[0]?title:x4_tr_for_lang(loading.language, X4_TR_U49_HOME_NEXT_GAME));
    uint64_t before=clock_us();prepare_fade_map();prepare_ring();prepare_backdrop(game);prepare_fixed();memcpy(loading.frame,loading.backdrop,PIXELS*4);
    for(int y=0;y<DH;++y)memcpy(loading.base_zone+(size_t)y*DW,loading.backdrop+(size_t)(DY+y)*WIDTH+DX,(size_t)DW*4);
    loading.view.background_us=clock_us()-before;prepare_static();loading.dirty=true;
    return true;
}
void x4_loading_screen_end(void)
{
    log_metrics();
    free(loading.backdrop);free(loading.frame);free(loading.base_zone);loading.backdrop=loading.frame=loading.base_zone=NULL;
    loading.view.active=false;
}
bool x4_loading_screen_active(void){return loading.view.active;}
void x4_loading_screen_update(const X4SessionSnapshot *s,uint64_t now)
{
    if(!loading.view.active||!s||loading.view.first_picture)return;
    if(s->state==X4_SESSION_ERROR||s->state==X4_SESSION_STOPPING||s->state==X4_SESSION_CLOSED||s->state==X4_SESSION_CANCELLED||s->error||s->http_status>=400||s->cleanup_failed){loading.view.stage=X4_LOADING_ERROR;return;}
    loading.view.waiting_for_resources=s->waiting_for_resources!=0;
    loading.view.queue_elapsed_seconds=s->queue_elapsed_seconds;
    loading.view.queue_eta_available=s->queue_eta_available!=0;
    loading.view.queue_eta_seconds=s->queue_eta_seconds;
    if(loading.view.waiting_for_resources)loading.view.delayed_notice=false;
    enum X4LoadingStage stage=loading.view.stage;unsigned percent=loading.view.percent;
    switch(s->state){
    case X4_SESSION_WAITING:stage=X4_LOADING_RESOURCES;percent=20;break;
    case X4_SESSION_READY:stage=X4_LOADING_RESOURCES;percent=35;break;
    case X4_SESSION_AUTHORIZING:stage=X4_LOADING_AUTHORIZE;percent=42;break;
    case X4_SESSION_AUTHORIZED:stage=X4_LOADING_AUTHORIZE;percent=49;break;
    case X4_SESSION_NEGOTIATING:stage=X4_LOADING_CONNECT;percent=55;break;
    case X4_SESSION_CONNECTING:stage=X4_LOADING_CONNECT;percent=64;break;
    case X4_SESSION_STREAMING:stage=X4_LOADING_CONNECTED;percent=70;break;
    default:break;
    }
    if(s->rtc_connected){stage=X4_LOADING_CONNECTED;percent=70;}
    if(stage==X4_LOADING_CONNECTED||stage==X4_LOADING_FIRST_PICTURE){
        if(!loading.wait_started){loading.wait_us=now;loading.wait_started=true;}
        uint64_t age=now>=loading.wait_us?now-loading.wait_us:0;
        if(age>=250000ULL)stage=X4_LOADING_FIRST_PICTURE;
        if(age>30000000ULL)age=30000000ULL;
        percent=70+(unsigned)(age*25/30000000ULL);
        loading.view.waiting_seconds=(unsigned)((now>=loading.wait_us?now-loading.wait_us:0)/1000000ULL);
        loading.view.delayed_notice=loading.view.waiting_seconds>=5;
    }
    if(stage>loading.view.stage)loading.view.stage=stage;
    if(percent>loading.view.percent)loading.view.percent=percent;
    if(loading.view.percent>95)loading.view.percent=95;
    prepare_static();
}
void x4_loading_screen_first_picture(uint64_t now)
{
    if(!loading.view.active||loading.view.first_picture||loading.view.stage==X4_LOADING_ERROR)return;
    loading.view.first_picture=true;loading.first_us=now;
    loading.view.first_picture_us=now;
    loading.view.percent=100;loading.view.stage=X4_LOADING_COMPLETE;
    loading.view.delayed_notice=false;prepare_static();loading.dirty=true;
}
bool x4_loading_screen_transition_ready(uint64_t now)
{
    if(!loading.view.active||loading.view.stage==X4_LOADING_ERROR)return true;
    if(!loading.view.first_picture||now<loading.first_us||now-loading.first_us<FADE_US)return false;
    if(!loading.view.transition_ready_us){
        loading.view.transition_ready_us=now;
        loading.view.transition_wait_us=now-loading.first_us;
    }
    return true;
}
static void blacken_new_ranks(uint32_t *dst,unsigned from,unsigned to)
{
#if defined(__SSE2__)
    const __m128i black=_mm_set1_epi32((int)UINT32_C(0xff000000));
#endif
    for(unsigned rank=from;rank<to;++rank)for(unsigned block=0;block<fade_counts[rank];++block){
        size_t offset=fade_offsets[rank][block];
        for(unsigned row=0;row<FADE_BLOCK;++row){
            uint32_t *line=dst+offset+(size_t)row*WIDTH;
#if defined(__SSE2__)
            _mm_storeu_si128((__m128i *)(void *)line,black);
            _mm_storeu_si128((__m128i *)(void *)(line+4),black);
#else
            for(unsigned pixel=0;pixel<FADE_BLOCK;++pixel)line[pixel]=UINT32_C(0xff000000);
#endif
        }
        ++loading.view.fade_blocks_written;
    }
}
bool x4_loading_screen_draw(uint32_t *pixels,uint64_t now)
{
    if(!pixels||!loading.view.active||loading.view.stage==X4_LOADING_ERROR)return false;
    uint64_t before=clock_us();
    bool changed=loading.dirty||now/FRAME_US!=loading.animation_tick;
    if(loading.view.first_picture){if(loading.dirty)render_frame(loading.first_us);}
    else if(changed)render_frame(now);
    unsigned slot=0;while(slot<4&&loading.destinations[slot]&&loading.destinations[slot]!=pixels)++slot;
    if(slot==4)slot=3;
    bool fresh=loading.destinations[slot]!=pixels;
    if(loading.view.first_picture){
        uint64_t age=now>=loading.first_us?now-loading.first_us:0;
        unsigned rank=age>=FADE_US?FADE_RANKS:(unsigned)(age*FADE_RANKS/FADE_US);
        if(!fresh&&loading.destination_fade_seeded[slot]&&loading.destination_fade_rank[slot]==rank)return false;
        if(fresh){
            loading.destinations[slot]=pixels;++loading.view.full_buffer_initializations;
            memcpy(pixels,loading.frame,PIXELS*4);++loading.view.fade_full_initializations;
            loading.destination_fade_rank[slot]=0;loading.destination_fade_seeded[slot]=true;
        }else if(!loading.destination_fade_seeded[slot]){
            /* Outside the center this buffer still contains its once-copied
             * backdrop. Seed only the100% center, exactly once per buffer. */
            copy_region(pixels,loading.frame,DX,DY,DW,DH);++loading.view.fade_seed_copies;
            loading.destination_fade_rank[slot]=0;loading.destination_fade_seeded[slot]=true;
        }
        blacken_new_ranks(pixels,loading.destination_fade_rank[slot],rank);
        loading.destination_fade_rank[slot]=rank;
    }else{
        bool animate=fresh||loading.destination_generation[slot]!=loading.generation;
        bool text=false;
        for(unsigned i=0;i<REGION_COUNT;++i)
            text|=loading.destination_region_generation[slot][i]!=loading.region_generation[i];
        if(!animate&&!text)return false;
        if(fresh){loading.destinations[slot]=pixels;++loading.view.full_buffer_initializations;memcpy(pixels,loading.frame,PIXELS*4);}
        else{
            for(unsigned i=0;i<REGION_COUNT;++i)
                if(loading.destination_region_generation[slot][i]!=loading.region_generation[i])
                    copy_region(pixels,loading.frame,DX,regions[i].y,DW,regions[i].height);
            if(animate)copy_region(pixels,loading.frame,AX,AY,AW,AH);
            ++loading.view.region_copies;
        }
    }
    loading.destination_generation[slot]=loading.generation;
    memcpy(loading.destination_region_generation[slot],loading.region_generation,sizeof(loading.region_generation));
    record_render(clock_us()-before,loading.view.first_picture);return true;
}
void x4_loading_screen_snapshot(X4LoadingView *out){if(out)*out=loading.view;}
void x4_loading_screen_metrics_write(FILE *f)
{
    if(!f)return;
    fprintf(f,"{\"render_n\":%llu,\"render_p50_us\":%llu,\"render_p95_us\":%llu,\"render_max_us\":%llu,\"render_mean_us\":%.3f,\"frame_total_p50_us\":%llu,\"frame_total_p95_us\":%llu,\"frame_total_max_us\":%llu,\"frame_total_mean_us\":%.3f,\"background_preparations\":%llu,\"background_us\":%llu,\"background_from_game\":%s,\"full_buffer_initializations\":%llu,\"region_copies\":%llu,\"fade_frames\":%llu,\"static_preparations\":%llu,\"static_max_us\":%llu,\"fade_us\":160000,\"animation_hz\":30,\"animation_zone\":{\"x\":800,\"y\":300,\"width\":320,\"height\":180},\"state_dirty_rectangles\":4,\"progress_is_stage_estimate\":true,\"transition_mode\":\"incremental_8x8_dissolve_to_dark_before_stream_handoff\",\"first_native_picture_us\":%llu,\"transition_ready_observed_us\":%llu,\"transition_wait_us\":%llu,\"fade_p50_us\":%llu,\"fade_p95_us\":%llu,\"fade_max_us\":%llu,\"fade_mean_us\":%.3f,\"fade_seed_copies\":%llu,\"fade_cold_full_copies\":%llu,\"fade_blocks_written\":%llu,\"transition_scope\":\"UI clock independent of new AU; first native picture is main observation, not proof of physical display\"}",
        (unsigned long long)loading.view.render_calls,(unsigned long long)percentile(50),(unsigned long long)percentile(95),(unsigned long long)loading.view.render_max_us,
        loading.view.render_calls?(double)loading.render_total_us/(double)loading.view.render_calls:0,
        (unsigned long long)frame_percentile(50),(unsigned long long)frame_percentile(95),(unsigned long long)loading.view.frame_total_max_us,
        loading.view.render_calls?(double)loading.frame_total_us/(double)loading.view.render_calls:0,
        (unsigned long long)loading.view.background_preparations,(unsigned long long)loading.view.background_us,loading.view.has_game_background?"true":"false",
        (unsigned long long)loading.view.full_buffer_initializations,(unsigned long long)loading.view.region_copies,(unsigned long long)loading.view.fade_frames,
        (unsigned long long)loading.view.static_preparations,(unsigned long long)loading.view.static_max_us,
        (unsigned long long)loading.view.first_picture_us,(unsigned long long)loading.view.transition_ready_us,(unsigned long long)loading.view.transition_wait_us,
        (unsigned long long)fade_percentile(50),(unsigned long long)fade_percentile(95),(unsigned long long)loading.view.fade_max_us,
        loading.view.fade_frames?(double)loading.fade_total_us/(double)loading.view.fade_frames:0,
        (unsigned long long)loading.view.fade_seed_copies,(unsigned long long)loading.view.fade_full_initializations,(unsigned long long)loading.view.fade_blocks_written);
}
