/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent C adaptation of the pinned client's statistics/warning model.
 * Main owns metrics, the cache, and the final stream JSON export. */
#include "net_hud.h"
#include "canvas.h"
#include "font.h"
#include "lang.h"
#include "ui_perf.h"
#include <errno.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

enum { CACHE_ROWS=124, LINE_H=36, GRAPH_H=88, SAMPLE_US=1000000,
       FULL_COMPOSE_US=250000, WARNING_US=3000000 };
#define HUD_PATH "/data/xcloud4/net-hud-v1.bin"
#define RGB(r,g,b) (UINT32_C(0xff000000)|((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(uint32_t)(r))
static const uint32_t background=RGB(11,16,29),foreground=RGB(238,244,252),amber=RGB(255,190,62);
/* Existing UI primitives have a fixed 1920-pixel stride. Only the bounded
 * 560x124 spans below are drawn/copied; cache rows stay in ordinary CPU BSS. */
static _Alignas(16) uint32_t cache[1920*CACHE_ROWS];
extern void x4_touch_menu_copy_opaque_row(uint32_t *restrict,const uint32_t *restrict,size_t);
static struct {
    bool mode_loaded,active,finalized,first_picture,reconnecting,have_stats;
    bool cache_valid,cache_warning,cache_line,await_picture,reconnect_notice;
    unsigned mode,reconnects,cache_mode,frame_count,frame_next;
    const void *key;
    uint64_t sample_at,last_picture_at,presentations,base_presentations;
    uint64_t base_video,base_lost,base_dropped,base_video_bytes,base_audio_bytes;
    uint64_t warning_until,episodes,bad_seconds,last_bad_sample,samples,compositions,last_compose,last_full_compose,last_log;
    uint64_t last_now,compose_max;
    double fps,mbps,loss,sum_mbps,sum_loss,max_frame;
    int rtt,best_rtt,height,cache_x,cache_y,cache_w,cache_h;
    enum X4Lang cache_language;
    float frames[X4_NET_HUD_FRAMES];
    X4UiPerf copies;
    char line[256];
} hud={.mode=X4_NET_HUD_WARNING,.rtt=-1,.best_rtt=-1};
#ifdef X4_NET_HUD_TEST
static char test_path[1024];
#endif
static const char *settings_path(void)
{
#ifdef X4_NET_HUD_TEST
    /* A host harness must opt into its own file; never write /data in tests. */
    return test_path[0]?test_path:NULL;
#else
    return HUD_PATH;
#endif
}
static uint32_t read32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void write32(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t record_checksum(const unsigned char *p)
{uint32_t h=2166136261u;for(unsigned i=0;i<12;++i)h=(h^p[i])*16777619u;return h;}
static void load_mode(void)
{
    if(hud.mode_loaded)return;
    hud.mode_loaded=true;hud.mode=X4_NET_HUD_WARNING;
    const char *path=settings_path();if(!path)return;
    FILE *f=fopen(path,"rb");if(!f)return;
    unsigned char record[16];size_t n=fread(record,1,sizeof(record),f);int extra=fgetc(f);
    bool ok=!ferror(f);if(fclose(f))ok=false;
    if(ok&&n==16&&extra==EOF&&!memcmp(record,"X4NH",4)&&read32(record+4)==1&&
       read32(record+8)<=X4_NET_HUD_FULL&&read32(record+12)==record_checksum(record))hud.mode=read32(record+8);
}
unsigned x4_net_hud_mode(void) {load_mode();return hud.mode;}
const char *x4_net_hud_mode_label(void)
{
    static const enum X4TextId ids[]={X4_TR_NET_HUD_OFF,X4_TR_NET_HUD_WARNING,X4_TR_NET_HUD_BASIC,X4_TR_NET_HUD_FULL};
    return x4_tr(ids[x4_net_hud_mode()]);
}
int x4_net_hud_cycle_mode(void)
{
    load_mode();hud.mode=(hud.mode+1u)%4u;hud.cache_valid=false;
    unsigned char record[16]={ 'X','4','N','H' };write32(record+4,1);write32(record+8,hud.mode);
    write32(record+12,record_checksum(record));
    const char *path=settings_path();
    if(!path)return -EINVAL;
    if(!strcmp(path,HUD_PATH)){
#if defined(_WIN32)
        (void)_mkdir("/data/xcloud4");
#else
        (void)mkdir("/data/xcloud4",0777);
#endif
    }
    errno=0;FILE *f=fopen(path,"wb");bool ok=f!=NULL;
    if(f){ok=fwrite(record,1,sizeof(record),f)==sizeof(record)&&fflush(f)==0;if(fclose(f))ok=false;}
    if(!ok){int error=errno?errno:EIO;printf("XCloud4: indicadores de red no guardados codigo=%d; modo valido durante esta ejecucion\n",error);return -error;}
    return 0;
}
static void bases(const X4LiveMediaSnapshot *m,uint64_t now)
{
    hud.sample_at=now;hud.base_presentations=m->video_present_calls;
    hud.base_video=m->video_packets;hud.base_lost=m->video_lost_packets;hud.base_dropped=m->video_dropped_frames;
    hud.base_video_bytes=m->video_bytes;hud.base_audio_bytes=m->audio_bytes;
}
static void begin_session(const void *key,const X4LiveMediaSnapshot *m,const X4SessionSnapshot *s,uint64_t now)
{
    unsigned mode=hud.mode;bool loaded=hud.mode_loaded;
    memset(&hud,0,sizeof(hud));hud.mode=mode;hud.mode_loaded=loaded;
    hud.key=key;hud.active=true;hud.rtt=hud.best_rtt=-1;hud.height=(int)m->height;
    hud.reconnects=s->reconnects;hud.reconnecting=s->reconnecting!=0;hud.last_now=hud.last_log=now;
    hud.presentations=m->video_present_calls;hud.first_picture=hud.presentations!=0;
    if(hud.first_picture)hud.last_picture_at=now;
    bases(m,now);
}
static bool warning_at(uint64_t now)
{return hud.first_picture&&!hud.reconnecting&&now<hud.warning_until;}
static void degrade(uint64_t now,unsigned causes,uint64_t dropped,bool frame_changed)
{
    if(!hud.first_picture||hud.reconnecting||!causes)return;
    if(!warning_at(now)&&!hud.await_picture){
        ++hud.episodes;
        printf("XCloud4: conexion inestable causa=0x%02x imagenes_perdidas=%llu rtt_ms=%d mejor_rtt_ms=%d\n",
            causes,(unsigned long long)dropped,hud.rtt,hud.best_rtt);
    }
    /* Same meaning as badSeconds in the reference: bad observations, not the
     * three-second visual hold. A reconnect event shares its one-second bin. */
    if(!hud.bad_seconds||now<hud.last_bad_sample||now-hud.last_bad_sample>=SAMPLE_US){
        ++hud.bad_seconds;hud.last_bad_sample=now;
    }
    hud.warning_until=now+WARNING_US;
    if((causes&(2u|4u|8u))&&!frame_changed)hud.await_picture=true;
}
static void picture(const X4LiveMediaSnapshot *m,uint64_t now)
{
    if(m->video_present_calls<=hud.presentations)return;
    hud.first_picture=true;
    if(hud.last_picture_at&&now>hud.last_picture_at&&!hud.reconnecting){
        double ms=(double)(now-hud.last_picture_at)/1000.0;
        if(ms>hud.max_frame)hud.max_frame=ms;
        if(ms<=250.0){hud.frames[hud.frame_next]=(float)ms;hud.frame_next=(hud.frame_next+1u)%X4_NET_HUD_FRAMES;if(hud.frame_count<X4_NET_HUD_FRAMES)++hud.frame_count;}
    }
    hud.last_picture_at=now;hud.presentations=m->video_present_calls;
    if(hud.await_picture&&!hud.reconnecting){hud.warning_until=now+WARNING_US;hud.await_picture=false;}
}
static void update(const X4LiveMediaSnapshot *m,const X4SessionSnapshot *s,uint64_t now)
{
    bool entering=s->reconnecting&&!hud.reconnecting,leaving=!s->reconnecting&&hud.reconnecting;
    bool changed=s->reconnects!=hud.reconnects;
    bool frame_changed=m->video_present_calls>hud.presentations;
    if(entering||leaving||changed){
        bases(m,now);hud.best_rtt=-1;hud.rtt=-1;hud.have_stats=false;hud.last_picture_at=0;
        hud.warning_until=0;hud.await_picture=false;hud.reconnect_notice=true;
    }
    hud.reconnecting=s->reconnecting!=0;hud.reconnects=s->reconnects;
    hud.height=(int)m->height;hud.rtt=s->rtt_ms>0?s->rtt_ms:-1;
    if(m->video_present_calls<hud.presentations){
        hud.presentations=m->video_present_calls;hud.last_picture_at=0;
        bases(m,now);hud.have_stats=false;
    }
    if(!hud.reconnecting&&hud.reconnect_notice){
        degrade(now,8u,0,frame_changed);hud.reconnect_notice=false;
    }
    picture(m,now);
    if(hud.reconnecting){bases(m,now);hud.best_rtt=-1;hud.warning_until=0;hud.last_picture_at=0;return;}
    if(now<hud.sample_at){bases(m,now);hud.last_picture_at=0;return;}
    uint64_t elapsed=now-hud.sample_at;if(elapsed<SAMPLE_US)return;
    if(m->video_present_calls<hud.base_presentations||m->video_packets<hud.base_video||m->video_lost_packets<hud.base_lost||
       m->video_dropped_frames<hud.base_dropped||m->video_bytes<hud.base_video_bytes||m->audio_bytes<hud.base_audio_bytes){bases(m,now);hud.have_stats=false;return;}
    uint64_t shown=m->video_present_calls-hud.base_presentations,got=m->video_packets-hud.base_video;
    uint64_t lost=m->video_lost_packets-hud.base_lost,dropped=m->video_dropped_frames-hud.base_dropped;
    double seconds=(double)elapsed/1000000.0;
    hud.fps=(double)shown/seconds;
    hud.mbps=((double)(m->video_bytes-hud.base_video_bytes)+(double)(m->audio_bytes-hud.base_audio_bytes))*8.0/(double)elapsed;
    hud.loss=got||lost?100.0*(double)lost/((double)got+(double)lost):0;
    if(hud.rtt>0&&(hud.best_rtt<0||hud.rtt<hud.best_rtt))hud.best_rtt=hud.rtt;
    hud.have_stats=hud.first_picture;unsigned causes=0;
    if(dropped)causes|=1u;if(!shown)causes|=2u;if(elapsed>2500000u)causes|=4u;
    if(hud.rtt>0&&hud.best_rtt>0&&hud.rtt>hud.best_rtt+60)causes|=16u;
    degrade(now,causes,dropped,frame_changed);
    if(hud.first_picture){++hud.samples;hud.sum_mbps+=hud.mbps;hud.sum_loss+=hud.loss;}
    bases(m,now);
}
static void dot(int x,int y)
{
    for(int row=-3;row<=3;++row)for(int col=-3;col<=3;++col)
        if(row*row+col*col<=9)cache[(size_t)(y+row)*1920+x+col]=amber;
}
static void compose(bool warning,bool line,int y,uint64_t now)
{
    hud.cache_x=line?32:0;hud.cache_y=line?y:28;hud.cache_h=LINE_H;
    const char *warn=x4_tr(X4_TR_NET_HUD_UNSTABLE);
    if(!line){
        int w=x4_text_width_px(18,X4_FONT_SEMIBOLD,warn)+42;
        if(w>X4_NET_HUD_MAX_WIDTH)w=X4_NET_HUD_MAX_WIDTH;
        hud.cache_w=w;hud.cache_x=1888-w;
        x4_rect(cache,0,0,w,LINE_H,background);dot(15,18);
        x4_text_ellipsis_px(cache,28,7,18,X4_FONT_SEMIBOLD,warn,w-38,amber);
        hud.line[0]=0;
    }else{
        char ping[24];if(hud.rtt>0)snprintf(ping,sizeof(ping),"%d ms",hud.rtt);else snprintf(ping,sizeof(ping),"– ms");
        snprintf(hud.line,sizeof(hud.line),"%dp · %.0f fps · %.1f Mbps · %s · %.1f %%",hud.height,hud.fps,hud.mbps,ping,hud.loss);
        char suffix[96];snprintf(suffix,sizeof(suffix)," · %s",warn);
        int size=18;
        while(size>12&&x4_text_width_px(size,X4_FONT_SEMIBOLD,hud.line)+(warning?x4_text_width_px(size,X4_FONT_SEMIBOLD,suffix):0)>536)--size;
        hud.cache_w=X4_NET_HUD_MAX_WIDTH;
        hud.cache_h=hud.mode==X4_NET_HUD_FULL?CACHE_ROWS:LINE_H;
        x4_rect(cache,0,0,hud.cache_w,hud.cache_h,background);
        int suffix_width=warning?x4_text_width_px(size,X4_FONT_SEMIBOLD,suffix):0;
        if(suffix_width>260)suffix_width=260;
        int available=536-suffix_width;
        int base_width=x4_text_width_px(size,X4_FONT_SEMIBOLD,hud.line);
        if(base_width>available)base_width=available;
        x4_text_ellipsis_px(cache,12,8,size,X4_FONT_SEMIBOLD,hud.line,available,foreground);
        if(warning)x4_text_ellipsis_px(cache,12+base_width,8,size,X4_FONT_SEMIBOLD,suffix,536-base_width,amber);
        if(hud.mode==X4_NET_HUD_FULL){
            const int left=12,top=44,bottom=112,height=68;
            int guide=bottom-(int)(16.7/40.0*height);x4_rect(cache,left,guide,536,1,RGB(73,125,99));
            for(unsigned i=0;i<hud.frame_count;++i){
                unsigned at=(hud.frame_next+X4_NET_HUD_FRAMES-hud.frame_count+i)%X4_NET_HUD_FRAMES;
                float ms=hud.frames[at],bounded=ms>40?40:ms;
                int bar=(int)(bounded*(float)height/40.0f);if(bar<1)bar=1;
                uint32_t color=ms<=17.5f?RGB(80,207,125):ms<=33.3f?amber:RGB(244,88,89);
                int x=left+(int)i*536/X4_NET_HUD_FRAMES;
                x4_rect(cache,x,bottom-bar,6,bar,color);
            }
            x4_rect(cache,left,bottom,536,1,RGB(65,76,90));
            (void)top;
        }
    }
    hud.cache_valid=true;hud.cache_warning=warning;hud.cache_line=line;hud.cache_mode=hud.mode;
    hud.cache_language=x4_lang_current();hud.last_compose=now;
    if(hud.mode==X4_NET_HUD_FULL)hud.last_full_compose=now;
    ++hud.compositions;
}
bool x4_net_hud_prepare(const void *key,const X4LiveMediaSnapshot *m,const X4SessionSnapshot *s,int muted,uint64_t now)
{
    load_mode();if(!m||!s)return false;
    if(!hud.active||hud.finalized||key!=hud.key)begin_session(key,m,s,now);
    else update(m,s,now);
    hud.last_now=now;
    bool composed=false;
    bool warning=warning_at(now),line=hud.have_stats&&hud.height>0&&hud.mode>=X4_NET_HUD_BASIC;
    if(hud.mode==X4_NET_HUD_OFF||hud.reconnecting||(!line&&!warning))hud.cache_valid=false;
    else{
        int y=m->audio_error||s->input_error||muted?64:28;
        /* Relocation needs no new pixels. Basic composition exceptions are
         * limited to mode/warning; language/data wait for its next 1 s slot. */
        if(hud.cache_valid&&hud.cache_line&&line)hud.cache_y=y;
        bool changed=!hud.cache_valid||hud.cache_mode!=hud.mode||hud.cache_warning!=warning;
        if(hud.mode==X4_NET_HUD_FULL)
            changed=changed||hud.cache_line!=line||hud.cache_language!=x4_lang_current();
        bool due=now>=hud.last_compose&&now-hud.last_compose>=SAMPLE_US;
        if(hud.mode==X4_NET_HUD_FULL){
            bool full_due=!hud.compositions||(now>=hud.last_full_compose&&now-hud.last_full_compose>=FULL_COMPOSE_US);
            if(!full_due){if(changed)hud.cache_valid=false;}
            else {compose(warning,line,y,now);composed=true;}
        }else if(changed||due){compose(warning,line,y,now);composed=true;}
    }
#if !X4_PUBLIC
    if(now>=hud.last_log&&now-hud.last_log>=5000000u){
        printf("XCloud4 net HUD: fps=%.3f mbps=%.3f rtt_ms=%d best_rtt_ms=%d loss_pct=%.3f mode=%u copy_p95_upper_us=%llu copy_max_us=%llu compose_max_us=%llu\n",
            hud.fps,hud.mbps,hud.rtt,hud.best_rtt,hud.loss,hud.mode,
            (unsigned long long)x4_ui_perf_percentile(&hud.copies,95),(unsigned long long)hud.copies.max,(unsigned long long)hud.compose_max);
        hud.last_log=now;
    }
#endif
    return composed;
}
bool x4_net_hud_draw(uint32_t *pixels)
{
    if(!pixels||!hud.cache_valid||hud.mode==X4_NET_HUD_OFF||hud.reconnecting)return false;
    for(int row=0;row<hud.cache_h;++row)
        x4_touch_menu_copy_opaque_row(pixels+(size_t)(hud.cache_y+row)*1920+hud.cache_x,
            cache+(size_t)row*1920,(size_t)hud.cache_w);
    return true;
}
void x4_net_hud_note_copy_us(uint64_t us) {x4_ui_perf_record(&hud.copies,us);}
void x4_net_hud_note_compose_us(uint64_t us) {if(us>hud.compose_max)hud.compose_max=us;}
void x4_net_hud_snapshot(X4NetHudSnapshot *out)
{
    if(!out)return;
    *out=(X4NetHudSnapshot){.mode=x4_net_hud_mode(),.height=(unsigned)hud.height,.frame_count=hud.frame_count,
        .rtt_ms=hud.rtt,.best_rtt_ms=hud.best_rtt,.fps=hud.fps,.mbps=hud.mbps,.loss_pct=hud.loss,
        .mean_mbps=hud.samples?hud.sum_mbps/(double)hud.samples:0,.mean_loss_pct=hud.samples?hud.sum_loss/(double)hud.samples:0,
        .max_frame_ms=hud.max_frame,.have_stats=hud.have_stats,.warning=warning_at(hud.last_now),
        .reconnecting=hud.reconnecting,.first_picture=hud.first_picture,.degraded_episodes=hud.episodes,.degraded_seconds=hud.bad_seconds,
        .warning_until_us=hud.warning_until,.compositions=hud.compositions,.copy_samples=hud.copies.n,
        .copy_p95_upper_us=x4_ui_perf_percentile(&hud.copies,95),.copy_max_us=hud.copies.max,.compose_max_us=hud.compose_max,
        .x=hud.cache_x,.y=hud.cache_y,.width=hud.cache_valid?hud.cache_w:0,.height_px=hud.cache_valid?hud.cache_h:0};
    for(unsigned i=0;i<hud.frame_count;++i)out->frame_ms[i]=hud.frames[(hud.frame_next+X4_NET_HUD_FRAMES-hud.frame_count+i)%X4_NET_HUD_FRAMES];
    memcpy(out->line,hud.line,sizeof(out->line));
}
void x4_net_hud_write_json(FILE *f)
{
    if(!f)return;
    /* An HTTP failure can close the next session before its first prepare.
     * Never attribute the preceding finalized stream's metrics to that case. */
    X4NetHudSnapshot s={.mode=x4_net_hud_mode(),.rtt_ms=-1,.best_rtt_ms=-1};
    if(hud.active&&!hud.finalized)x4_net_hud_snapshot(&s);
    fprintf(f,"{\"mode\":%u,\"degraded_episodes\":%llu,\"degraded_seconds\":%llu,\"best_rtt_ms\":%d,\"mean_mbps\":%.6f,\"mean_loss_pct\":%.6f,\"max_frame_ms\":%.3f}",
        s.mode,(unsigned long long)s.degraded_episodes,(unsigned long long)s.degraded_seconds,s.best_rtt_ms,
        s.mean_mbps,s.mean_loss_pct,s.max_frame_ms);
    hud.finalized=true;
}
#ifdef X4_NET_HUD_TEST
void x4_net_hud_test_reset(const char *path)
{
    memset(&hud,0,sizeof(hud));hud.mode=X4_NET_HUD_WARNING;hud.rtt=hud.best_rtt=-1;
    snprintf(test_path,sizeof(test_path),"%s",path?path:"");
}
#endif
