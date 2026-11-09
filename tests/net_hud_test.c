/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
/* Deterministic snapshots. No real media, network, clock, or /data access. */
#include "../src/ui/net_hud.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *directory;
static unsigned case_id;
static X4LiveMediaSnapshot media;
static X4SessionSnapshot session;
static uint64_t now;
static char path[1024];
static uint32_t *copy_begin,*copy_end;
static size_t copy_rows,copy_width;

/* The actual opaque-row routine has its own touch_menu test. This double
 * rejects every write outside the one synthetic framebuffer. */
void x4_touch_menu_copy_opaque_row(uint32_t *restrict dst,const uint32_t *restrict src,size_t n)
{
    assert(copy_begin&&dst>=copy_begin&&dst+n<=copy_end);
    assert(n<=X4_NET_HUD_MAX_WIDTH);
    memcpy(dst,src,n*sizeof(*dst));++copy_rows;if(n>copy_width)copy_width=n;
}
static void near(double got,double expected)
{assert(fabs(got-expected)<0.00001);}
static X4NetHudSnapshot view(void)
{X4NetHudSnapshot s;x4_net_hud_snapshot(&s);return s;}
static void fresh(unsigned mode)
{
    int n=snprintf(path,sizeof(path),"%s/case-%u.bin",directory,++case_id);
    assert(n>0&&(size_t)n<sizeof(path));x4_net_hud_test_reset(path);
    assert(x4_net_hud_mode()==X4_NET_HUD_WARNING);
    for(unsigned i=0;x4_net_hud_mode()!=mode&&i<4;++i)assert(!x4_net_hud_cycle_mode());
    assert(x4_net_hud_mode()==mode);
    memset(&media,0,sizeof(media));memset(&session,0,sizeof(session));
    media.height=1080;media.width=1920;media.video_present_calls=1;session.rtt_ms=20;
    now=1000000;(void)x4_net_hud_prepare(&media,&media,&session,0,now);
}
static bool tick(uint64_t delta,unsigned images,uint64_t video_bytes,uint64_t audio_bytes)
{
    now+=delta;media.video_present_calls+=images;
    media.video_packets+=images*16u;media.video_bytes+=video_bytes;media.audio_bytes+=audio_bytes;
    return x4_net_hud_prepare(&media,&media,&session,0,now);
}
static void statistics(void)
{
    fresh(X4_NET_HUD_BASIC);
    media.video_packets+=30;media.video_lost_packets+=10;
    session.rtt_ms=18;assert(tick(1000000,60,1500000,50000));
    X4NetHudSnapshot s=view();assert(s.have_stats&&s.best_rtt_ms==18&&s.rtt_ms==18);
    near(s.fps,60);near(s.mbps,12.4);near(s.loss_pct,1);
    assert(strstr(s.line,"1080p")&&strstr(s.line,"60 fps")&&strstr(s.line,"12.4 Mbps"));
    session.rtt_ms=12;(void)tick(1000000,60,1000000,0);assert(view().best_rtt_ms==12);
    session.rtt_ms=-1;(void)tick(1000000,60,1000000,0);
    assert(view().best_rtt_ms==12&&view().rtt_ms==-1&&strstr(view().line,"– ms"));
    near(view().mean_mbps,(12.4+8+8)/3);near(view().mean_loss_pct,1.0/3);
    /* A late calculation uses elapsed real time, rather than assuming 1 s. */
    (void)tick(2000000,120,2000000,0);near(view().fps,60);near(view().mbps,8);
}
static void warning_causes(void)
{
    for(unsigned cause=0;cause<5;++cause){
        fresh(X4_NET_HUD_WARNING);(void)tick(1000000,60,1000000,0);
        if(cause==0){++media.video_dropped_frames;(void)tick(1000000,60,1000000,0);}
        if(cause==1)(void)tick(1000000,0,1000000,0);
        if(cause==2)(void)tick(3000000,180,3000000,0);
        if(cause==3){++session.reconnects;(void)tick(1000000,60,1000000,0);}
        if(cause==4){session.rtt_ms=81;(void)tick(1000000,60,1000000,0);}
        assert(view().warning&&view().degraded_episodes==1&&view().degraded_seconds==1);
        assert(view().width<=560&&view().x+view().width==1888&&view().y==28);
        session.rtt_ms=20;
        /* On frozen video the full 3 s hold begins when a new picture returns. */
        (void)tick(16667,1,16667,0);uint64_t deadline=view().warning_until_us;
        while(now+16667<deadline)(void)tick(16667,1,16667,0);
        if(now<deadline-1)(void)tick(deadline-1-now,1,0,0);
        assert(view().warning);(void)tick(1,1,0,0);assert(!view().warning);
        assert(view().degraded_episodes==1);
    }
    fresh(X4_NET_HUD_WARNING);media.video_present_calls=0;
    x4_net_hud_test_reset(NULL);now=1000000;
    (void)x4_net_hud_prepare(&media,&media,&session,0,now);
    (void)tick(4000000,0,0,0);assert(!view().warning&&!view().first_picture&&!view().degraded_episodes);
    /* A continuing bad interval is one episode, not one klog per sample. */
    fresh(X4_NET_HUD_WARNING);(void)tick(1000000,60,1000000,0);
    for(unsigned i=0;i<4;++i)(void)tick(1000000,0,0,0);
    assert(view().degraded_episodes==1&&view().degraded_seconds==4);
    (void)tick(4000000,1,0,0);assert(view().warning);
    assert(view().warning_until_us==now+3000000&&view().degraded_episodes==1);
}
static void reconnection(void)
{
    fresh(X4_NET_HUD_BASIC);session.rtt_ms=12;(void)tick(1000000,60,1000000,0);
    assert(view().best_rtt_ms==12);
    session.reconnecting=1;++session.reconnects;session.rtt_ms=-1;
    (void)tick(1000000,0,90000000,5000000);
    assert(!view().warning&&view().reconnecting&&view().best_rtt_ms==-1&&!view().width);
    (void)tick(2000000,0,90000000,5000000);assert(!view().warning);
    session.reconnecting=0;session.rtt_ms=30;
    (void)tick(16667,1,90000000,5000000);
    assert(view().warning&&!view().reconnecting&&view().degraded_episodes==1&&view().best_rtt_ms==-1);
    (void)tick(1000000,60,1000000,0);
    near(view().mbps,8);near(view().fps,60);assert(view().best_rtt_ms==30);
    /* Defensive counter regression establishes fresh bases, never an unsigned spike. */
    media.video_present_calls=0;media.video_packets=0;media.video_bytes=0;media.audio_bytes=0;
    (void)tick(1000000,0,0,0);assert(!view().have_stats);
    (void)tick(1000000,60,1000000,0);near(view().mbps,8);near(view().fps,60);
}
static void graph_and_composition(void)
{
    fresh(X4_NET_HUD_FULL);
    for(unsigned i=0;i<80;++i)(void)tick(16667,1,16667,0);
    assert(view().frame_count==60);
    for(unsigned i=0;i<60;++i)near(view().frame_ms[i],16.667);
    unsigned count=view().frame_count;
    (void)tick(0,1,0,0);assert(view().frame_count==count);
    (void)tick(250001,1,250001,0);assert(view().frame_count==count);
    assert(view().max_frame_ms>250);
    (void)tick(40000,1,40000,0);near(view().frame_ms[59],40);
    fresh(X4_NET_HUD_BASIC);(void)tick(1000000,60,1000000,0);
    uint64_t compositions=view().compositions;
    for(unsigned i=0;i<999;++i)(void)tick(1000,0,0,0);
    assert(view().compositions==compositions);
    (void)tick(1000,60,1000000,0);assert(view().compositions==compositions+1);
    fresh(X4_NET_HUD_FULL);(void)tick(1000000,60,1000000,0);
    compositions=view().compositions;
    for(unsigned i=0;i<1000;++i)(void)tick(1000,i%17==0,1000,0);
    assert(view().compositions==compositions+4);
    /* Even warning/mode changes cannot make Full compose >4 times/s. */
    compositions=view().compositions;session.reconnects++;
    assert(!tick(1,1,0,0));assert(view().compositions==compositions);
    (void)tick(249999,15,250000,0);assert(view().compositions==compositions+1);
}
static void persistence(void)
{
    fresh(X4_NET_HUD_WARNING);assert(!x4_net_hud_cycle_mode());assert(x4_net_hud_mode()==X4_NET_HUD_BASIC);
    FILE *f=fopen(path,"rb");assert(f);unsigned char data[17];assert(fread(data,1,sizeof(data),f)==16);assert(!fclose(f));
    assert(!memcmp(data,"X4NH",4));x4_net_hud_test_reset(path);assert(x4_net_hud_mode()==X4_NET_HUD_BASIC);
    data[9]^=1;f=fopen(path,"wb");assert(f&&fwrite(data,1,16,f)==16&&!fclose(f));
    x4_net_hud_test_reset(path);assert(x4_net_hud_mode()==X4_NET_HUD_WARNING);
    /* A directory is unwritable as a mode file even when tests run as root. */
    x4_net_hud_test_reset(directory);assert(x4_net_hud_mode()==X4_NET_HUD_WARNING);
    assert(x4_net_hud_cycle_mode()<0&&x4_net_hud_mode()==X4_NET_HUD_BASIC);
    x4_net_hud_test_reset(NULL);assert(x4_net_hud_mode()==X4_NET_HUD_WARNING);
    assert(x4_net_hud_cycle_mode()<0&&x4_net_hud_mode()==X4_NET_HUD_BASIC);
}
static void draw_and_text(void)
{
    size_t size=(size_t)1920*1080;uint32_t *raw=malloc((size+32)*sizeof(*raw));assert(raw);
    copy_begin=raw+16;copy_end=copy_begin+size;
    for(unsigned language=0;language<2;++language){
        x4_lang_set(language?X4_LANG_EN:X4_LANG_ES,language?"en-US":"es-MX",language?"US":"MX");
        static const char *labels[2][4]={{"Apagados","Solo aviso","Básicos","Completos"},{"Off","Warning only","Basic","Full"}};
        for(unsigned mode=0;mode<4;++mode){
            fresh(mode);assert(!strcmp(x4_net_hud_mode_label(),labels[language][mode]));
            (void)tick(1000000,60,1500000,50000);++media.video_dropped_frames;
            (void)tick(1000000,60,1500000,50000);
            for(size_t i=0;i<size+32;++i)raw[i]=0x735aca19;
            copy_rows=copy_width=0;bool drawn=x4_net_hud_draw(copy_begin);X4NetHudSnapshot s=view();
            assert(drawn==(mode!=X4_NET_HUD_OFF));
            if(drawn){
                assert(copy_rows==(size_t)s.height_px&&copy_width==(size_t)s.width&&s.width<=560);
                assert(s.x>=32&&s.x+s.width<=1888&&s.y==28&&s.y+s.height_px<=1080);
                for(size_t i=0;i<size;++i){
                    int x=(int)(i%1920),y=(int)(i/1920);
                    if(x<s.x||x>=s.x+s.width||y<s.y||y>=s.y+s.height_px)assert(copy_begin[i]==0x735aca19);
                }
            }
            for(unsigned i=0;i<16;++i)assert(raw[i]==0x735aca19&&raw[size+16+i]==0x735aca19);
            if(mode>=X4_NET_HUD_BASIC){
                media.audio_error=-1;(void)tick(1000000,60,1000000,0);assert(view().y==64);
                media.audio_error=0;session.input_error=-1;(void)tick(1000000,60,1000000,0);assert(view().y==64);
                session.input_error=0;now+=1000000;media.video_present_calls+=60;
                (void)x4_net_hud_prepare(&media,&media,&session,1,now);assert(view().y==64);
            }
        }
        assert(x4_text_width_px(18,X4_FONT_SEMIBOLD,x4_tr(X4_TR_NET_HUD_UNSTABLE))+42<=560);
    }
    free(raw);copy_begin=copy_end=NULL;
}
static void final_summary(void)
{
    fresh(X4_NET_HUD_BASIC);(void)tick(1000000,60,1500000,50000);
    x4_net_hud_note_copy_us(20);x4_net_hud_note_copy_us(100);x4_net_hud_note_compose_us(30);
    assert(view().copy_samples==2&&view().copy_max_us==100&&view().copy_p95_upper_us>=100&&view().compose_max_us==30);
    FILE *f=tmpfile();assert(f);x4_net_hud_write_json(f);assert(!fflush(f)&&!fseek(f,0,SEEK_SET));
    char json[1024]={0};size_t n=fread(json,1,sizeof(json)-1,f);assert(n&&!ferror(f)&&!fclose(f));
    assert(strstr(json,"\"mode\":2")&&strstr(json,"\"degraded_episodes\":0")&&strstr(json,"\"degraded_seconds\":0"));
    assert(strstr(json,"\"best_rtt_ms\":20")&&strstr(json,"\"mean_mbps\":12.400000")&&strstr(json,"\"mean_loss_pct\":0.000000")&&strstr(json,"\"max_frame_ms\":"));
    /* A new session which fails before prepare exports neutral metrics. */
    f=tmpfile();assert(f);x4_net_hud_write_json(f);assert(!fflush(f)&&!fseek(f,0,SEEK_SET));
    memset(json,0,sizeof(json));n=fread(json,1,sizeof(json)-1,f);assert(n&&!ferror(f)&&!fclose(f));
    assert(strstr(json,"\"mode\":2")&&strstr(json,"\"best_rtt_ms\":-1")&&strstr(json,"\"mean_mbps\":0.000000"));
    assert(strstr(json,"\"degraded_episodes\":0")&&strstr(json,"\"degraded_seconds\":0")&&strstr(json,"\"max_frame_ms\":0.000"));
    /* Allocator reusing the same live pointer still starts a clean session. */
    session.rtt_ms=-1;now+=1000000;(void)x4_net_hud_prepare(&media,&media,&session,0,now);
    assert(!view().have_stats&&view().best_rtt_ms==-1&&view().mode==X4_NET_HUD_BASIC&&!view().copy_samples);
}
int main(int argc,char **argv)
{
    assert(argc==2);directory=argv[1];assert(!x4_font_init("assets/fonts/inter.x4font"));
    statistics();warning_causes();reconnection();graph_and_composition();persistence();draw_and_text();final_summary();
    x4_font_close();puts("PASS net_hud: snapshots, five causes, hold, reconnect, ring, cache, persistence, es/en, bounds, JSON");
    return 0;
}
