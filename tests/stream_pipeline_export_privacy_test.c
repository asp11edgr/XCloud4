/* SPDX-License-Identifier: GPL-3.0-only */
/* The actual exporter with a closed, idle diagnostic object. No native
 * decoder, presentation, audio, input, RTC or scheduler workers execute. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/media/live_trace.h"
#if X4_PUBLIC
#include "../src/streaming/remb_estimator.h"
#include "../src/ui/picture_settings.h"
#endif

static const char *directory;
static unsigned opens,renames,mkdirs;
#if X4_PUBLIC
static unsigned default_picture_calls, remb_calls, udp_calls;

void x4_rtc_diag_write_udp_json(FILE *f)
{
    ++udp_calls;
    /* Same unavailable output as the native reporter before a peer exists. */
    fputs("\"udp_buffers\":null,", f);
}

void x4_rtc_remb_finish_summary(const X4Trace *trace, uint64_t started_us, X4RembSummary *out)
{
    assert(!trace && started_us == 1000000 && out && remb_calls < 2);
    *out = (X4RembSummary){
        .sent = 30 + remb_calls, .decreases = 2, .increases = 3,
        .sent_bps = 13500007, .min_sent_bps = 10050000, .max_sent_bps = 20000000,
        .received_bps = 11800001, .max_loss_ppm = 30000,
        .max_delay_us = remb_calls ? 40001 : -25501, .have_max_delay = true
    };
    ++remb_calls;
}
/* The export passes an explicit request to vsync_policy. These declarations
 * complete its unused default-settings branch, not the UI/persistence path;
 * the fixture asserts that neither substitute is ever executed. */
X4PictureSettingsConfig x4_picture_settings_config(void)
{
    ++default_picture_calls;
    return (X4PictureSettingsConfig){X4_PIC_QUALITY_720P,1u,X4_VSYNC_AUTO,true};
}
X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption quality)
{
    ++default_picture_calls;assert(quality==X4_PIC_QUALITY_720P);
    return X4_REQUEST_720P60;
}
#endif
static void map_path(char out[512],const char *path)
{
    const char *base=strrchr(path,'/');assert(base);
    int n=snprintf(out,512,"%s/%s",directory,base+1);assert(n>0&&n<512);
}
FILE *export_fixture_fopen(const char *path,const char *mode)
{
#if X4_PUBLIC
    assert(!strcmp(path,"/data/xcloud4/last-stream.json.tmp")&&!strcmp(mode,"wb"));
#else
    assert(!strncmp(path,"/data/xcloud4-stream-",21)&&!strcmp(mode,"wbx"));
#endif
    ++opens;char mapped[512];map_path(mapped,path);return fopen(mapped,mode);
}
int export_fixture_mkdir(const char *path,mode_t mode)
{assert(!strcmp(path,"/data/xcloud4")&&mode==0777);++mkdirs;return 0;}
int export_fixture_rename(const char *from,const char *to)
{
    assert(!strcmp(from,"/data/xcloud4/last-stream.json.tmp"));
    assert(!strcmp(to,"/data/xcloud4/last-stream.json"));
    char a[512],b[512];map_path(a,from);map_path(b,to);++renames;return rename(a,b);
}
int export_fixture_remove(const char *path)
{assert(!strcmp(path,"/data/xcloud4/last-stream.json.tmp"));char p[512];map_path(p,path);return remove(p);}
int sceKernelFsync(int fd){return fsync(fd);}
void x4_trace_sdp_bandwidth_get(const X4Trace *trace,unsigned scope,unsigned values[4])
{(void)trace;(void)scope;memset(values,0,4*sizeof(*values));}
uint32_t x4_trace_sdp_profile_get(const X4Trace *trace,unsigned scope,unsigned pt)
{(void)trace;(void)scope;(void)pt;return 0;}
const char *x4_lang_locale(void){return "es-MX";}

#define fopen export_fixture_fopen
#define mkdir export_fixture_mkdir
#define rename export_fixture_rename
#define remove export_fixture_remove
#include "../src/video/stream_pipeline.c"
#undef fopen
#undef mkdir
#undef rename
#undef remove

void x4_live_interaction_metrics_write(FILE *f)
{fputs("{\"available\":false,\"fixture_only\":true}",f);}
void x4_server_settings_write_json(FILE *f)
{fputs("\"server\":{\"selected\":\"auto\",\"fixture_only\":true},",f);}
uint64_t x4_receive_test_clock(void){return 1234567;}
void x4_receive_test_sleep(unsigned usec){(void)usec;assert(0);}
uint64_t x4_receive_test_threadid(void){return 1;}
void x4_mic_rr_observe(const void *packet,size_t bytes)
{(void)packet;(void)bytes;assert(false); /* The fixture sends only RTP. */}
void x4_trace_monitor_transport_packet(X4Trace *trace,unsigned stage,bool valid,uint32_t ssrc,
    uint16_t seq,unsigned pt,size_t bytes,uint64_t time,uint64_t prior,uint32_t flags)
{
    (void)trace;(void)stage;(void)valid;(void)ssrc;(void)seq;(void)pt;
    (void)bytes;(void)time;(void)prior;(void)flags;
    assert(false); /* No trace sink is attached in this fixture. */
}

static X4StreamPipeline *closed_report(uint64_t session,unsigned profile)
{
    X4StreamPipeline *p=calloc(1,sizeof(*p));assert(p);
    p->request=x4_stream_request_config((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709);
    p->session=session;p->started=1000000;p->ended=2000000;p->closed=true;
    p->au_queue=x4_stream_au_queue_create();assert(p->au_queue);
    p->diagnostics=x4_stream_diagnostics_create(p->started);assert(p->diagnostics);
    return p;
}
static void dispose_report(X4StreamPipeline *p)
{
    x4_stream_au_queue_stop(p->au_queue);assert(x4_stream_au_queue_destroy(p->au_queue));
    x4_stream_diagnostics_free(p->diagnostics);free(p);
}
static void snapshot_first(void)
{
    char path[512];map_path(path,"/data/xcloud4/last-stream.json");
    FILE *f=fopen(path,"rb");assert(f);char first[512];map_path(first,"/first-public.json");
    FILE *g=fopen(first,"wb");assert(g);unsigned char data[1024];size_t n;
    while((n=fread(data,1,sizeof(data),f))!=0)assert(fwrite(data,1,n,g)==n);
    assert(!ferror(f));assert(!fclose(f)&&!fclose(g));
}
int main(int argc,char **argv)
{
    assert(argc==2);directory=argv[1];
    x4_rtc_receive_diag_reset();
    x4_rtc_diag_record_remote_candidate("candidate:1 1 UDP 1 192.0.2.4 40001 typ host","video");
    x4_rtc_diag_record_remote_candidate("candidate:2 1 UDP 1 198.51.100.4 40002 typ srflx","video");
    x4_rtc_diag_record_remote_candidate("candidate:3 1 TCP 1 2001:db8::4 40003 typ relay","video");
    x4_rtc_diag_record_selected_pair("192.0.2.5:50001","2001:db8::4:40003");
    const uint8_t video_rtp[] = {0x80,102,0,1,0,0,0,1,0x12,0x34,0x56,0x78,0x65};
    x4_rtc_receive_diag_set_expected_video(UINT32_C(0x12345678),102,"video");
    (void)x4_rtc_receive_packet(X4_RD_SRTP_VALID,video_rtp,sizeof(video_rtp),0,X4_RD_AUTHENTICATED);
    X4StreamPipeline *first=closed_report(101,0);assert(!x4_stream_pipeline_export(first,NULL));dispose_report(first);
#if X4_PUBLIC
    snapshot_first();
#endif
    X4StreamPipeline *second=closed_report(102,1);assert(!x4_stream_pipeline_export(second,NULL));dispose_report(second);
    assert(opens==2);
#if X4_PUBLIC
    assert(renames==2&&mkdirs==2&&!default_picture_calls&&remb_calls==2&&udp_calls==2);
#else
    assert(!renames&&!mkdirs);
#endif
    puts("PASS two actual exporter calls: public atomic overwrite / Lab unique session paths; no native workers");
    return 0;
}
