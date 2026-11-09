/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui_draw_metrics.h"
#include <inttypes.h>
#include <limits.h>
#include <string.h>
enum { UI_BUCKETS=1024, UI_QUANTUM_US=100 };
static const char *const screen_names[X4_UI_SCREEN_COUNT]={
    "menu","catalogo","account","fortnite","about","support","loading","queue","errors",
    "options_picture","options_server","first_time","keyboard","lab_settings","demo","other",
    "descubrir","recientes","ficha","arranque","descubrir_fundido","modal"
};
#if !X4_PUBLIC
typedef struct {
    uint32_t bins[UI_BUCKETS];
    uint64_t n,total,max,overflow,saturated;
} Timing;
typedef struct {
    Timing draw,copy,present;
    uint64_t presentations,copy_only,bytes;
    bool dirty;
} Row;
static Row rows[X4_UI_SCREEN_COUNT][2];
static uint64_t last_emit_us,last_dirty_skips;
static void add(Timing *h,uint64_t us)
{
    uint64_t bucket=us/UI_QUANTUM_US;
    if(bucket>=UI_BUCKETS){bucket=UI_BUCKETS-1;++h->overflow;}
    if(h->bins[bucket]!=UINT32_MAX)++h->bins[bucket];else ++h->saturated;
    ++h->n;h->total+=us;if(us>h->max)h->max=us;
}
static uint64_t percentile(const Timing *h,const Timing *other,unsigned pct,bool *overflow)
{
    *overflow=false;
    uint64_t total=h->n+(other?other->n:0),target=(total*pct+99)/100,sum=0;
    if(!target)return 0;
    for(unsigned i=0;i<UI_BUCKETS;++i){
        sum+=(uint64_t)h->bins[i]+(other?other->bins[i]:0);
        if(sum<target)continue;
        if(i==UI_BUCKETS-1){*overflow=true;return (uint64_t)(UI_BUCKETS-1)*UI_QUANTUM_US;}
        return (uint64_t)(i+1)*UI_QUANTUM_US;
    }
    *overflow=true;return (uint64_t)(UI_BUCKETS-1)*UI_QUANTUM_US;
}
static X4UiTimingView view(const Timing *h,const Timing *other)
{
    X4UiTimingView v={.n=h->n,.max_us=h->max,.total_us=h->total,.overflow=h->overflow+h->saturated};
    if(other){
        v.n+=other->n;v.total_us+=other->total;v.overflow+=other->overflow+other->saturated;
        if(other->max>v.max_us)v.max_us=other->max;
    }
    v.p50_us=percentile(h,other,50,&v.p50_overflow);
    v.p95_us=percentile(h,other,95,&v.p95_overflow);
    return v;
}
#endif
static void write_metrics(FILE *f,enum X4UiScreen screen,const char *phase,
    const X4UiDrawMetricView *v,uint64_t skips)
{
    if(!f||!v||(unsigned)screen>=X4_UI_SCREEN_COUNT)return;
    fprintf(f,"XCloud4 UI draw: screen=%s phase=%s frames=%" PRIu64 " present_frames=%" PRIu64
        " draw_p50_us=%" PRIu64 " draw_p95_us=%" PRIu64 " draw_max_us=%" PRIu64
        " copy_p50_us=%" PRIu64 " copy_p95_us=%" PRIu64 " copy_max_us=%" PRIu64
        " present_p50_us=%" PRIu64 " present_p95_us=%" PRIu64 " present_max_us=%" PRIu64
        " copied_bytes=%" PRIu64 " copy_only=%" PRIu64 " dirty_skips=%" PRIu64
        " bucket_us=100 draw_overflow=%" PRIu64 " copy_overflow=%" PRIu64 " present_overflow=%" PRIu64
        " draw_p50_overflow=%u draw_p95_overflow=%u copy_p95_overflow=%u present_p95_overflow=%u\n",
        screen_names[screen],phase,v->draw.n,v->presentations,
        v->draw.p50_us,v->draw.p95_us,v->draw.max_us,v->copy.p50_us,v->copy.p95_us,v->copy.max_us,
        v->present.p50_us,v->present.p95_us,v->present.max_us,v->copied_bytes,v->copy_only,skips,
        v->draw.overflow,v->copy.overflow,v->present.overflow,(unsigned)v->draw.p50_overflow,
        (unsigned)v->draw.p95_overflow,(unsigned)v->copy.p95_overflow,(unsigned)v->present.p95_overflow);
}
void x4_ui_draw_metrics_write(FILE *f,enum X4UiScreen screen,bool navigation,
    const X4UiDrawMetricView *v,uint64_t skips)
{
    write_metrics(f,screen,navigation?"navigation":"full",v,skips);
}
void x4_ui_draw_metrics_reset(void)
{
#if !X4_PUBLIC
    memset(rows,0,sizeof(rows));last_emit_us=last_dirty_skips=0;
#endif
}
void x4_ui_draw_metrics_record(enum X4UiScreen screen,bool navigation,bool redrawn,
    uint64_t draw_us,uint64_t copy_us,uint64_t present_us,uint64_t bytes,uint64_t now,uint64_t skips)
{
#if !X4_PUBLIC
    if((unsigned)screen>=X4_UI_SCREEN_COUNT)return;
    last_dirty_skips=skips;
    Row *row=&rows[screen][navigation?1:0];
    ++row->presentations;row->bytes+=bytes;row->dirty=true;
    if(redrawn)add(&row->draw,draw_us);else ++row->copy_only;
    add(&row->copy,copy_us);add(&row->present,present_us);x4_ui_draw_metrics_poll(now,skips);
#else
    (void)screen;(void)navigation;(void)redrawn;(void)draw_us;(void)copy_us;(void)present_us;(void)bytes;(void)now;(void)skips;
#endif
}
void x4_ui_draw_metrics_flush(uint64_t now)
{
#if !X4_PUBLIC
    for(unsigned s=0;s<X4_UI_SCREEN_COUNT;++s){
        Row *full=&rows[s][0],*nav=&rows[s][1];
        if(!full->dirty&&!nav->dirty)continue;
        X4UiDrawMetricView v={full->presentations+nav->presentations,
            full->copy_only+nav->copy_only,full->bytes+nav->bytes,
            view(&full->draw,&nav->draw),view(&full->copy,&nav->copy),view(&full->present,&nav->present)};
        write_metrics(stdout,(enum X4UiScreen)s,"all",&v,last_dirty_skips);
        full->dirty=nav->dirty=false;
    }
    last_emit_us=now;
#else
    (void)now;
#endif
}
void x4_ui_draw_metrics_poll(uint64_t now,uint64_t skips)
{
#if !X4_PUBLIC
    last_dirty_skips=skips;if(!last_emit_us)last_emit_us=now;
    if(now>=last_emit_us&&now-last_emit_us>=10000000u)x4_ui_draw_metrics_flush(now);
#else
    (void)now;(void)skips;
#endif
}
bool x4_ui_draw_metrics_snapshot(enum X4UiScreen screen,bool navigation,X4UiDrawMetricView *out)
{
#if !X4_PUBLIC
    if(!out||(unsigned)screen>=X4_UI_SCREEN_COUNT)return false;
    const Row *r=&rows[screen][navigation?1:0];
    *out=(X4UiDrawMetricView){r->presentations,r->copy_only,r->bytes,view(&r->draw,NULL),view(&r->copy,NULL),view(&r->present,NULL)};return true;
#else
    (void)screen;(void)navigation;if(out)memset(out,0,sizeof(*out));return false;
#endif
}
