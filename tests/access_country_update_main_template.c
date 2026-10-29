/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui/apple_ui.h"
#include "auth/release_check.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ORBIS_PAD_BUTTON_CIRCLE 0x2000u
#define ORBIS_PAD_BUTTON_CROSS 0x4000u
#define ORBIS_PAD_BUTTON_OPTIONS 0x8u
#define ORBIS_PAD_BUTTON_TRIANGLE 0x1000u
/* @UPDATE_PURE_UI@ */
static X4UpdateNotice update_notice;
static struct {uint32_t pressed;} controller;
static struct {int page;bool exit_requested,open_settings_requested,explore_catalog_requested;} screen;
static struct {bool open;} picture_settings_ui;
static bool closing,fortnite_login_flow,pending_launch,live_retained,session_back,first_choice,busy;
static int pending_auth;
static void *auth;
static enum X4UpdateStatus release_status;
static bool valid_tag;
static unsigned status_reads,tag_reads;
#if !X4_PUBLIC
static bool update_test_requested,can_edit,stream_settings_consumed;
static unsigned actions,ui_pressed,normal_r3_calls;
static struct {bool open;} stream_settings_ui;
static bool x4_stream_settings_ui_handle(void *ui,unsigned a,bool can)
{(void)ui;(void)a;(void)can;++normal_r3_calls;return true;}
#endif
int x4_auth_busy(void *a){(void)a;return busy;}
bool x4_server_settings_awaiting_first_choice(void){return first_choice;}
enum X4UpdateStatus x4_release_check_status(char *out,size_t capacity)
{++status_reads;if(out&&capacity)snprintf(out,capacity,"%s","9.8.7");return release_status;}
bool x4_release_check_tag(char *out,size_t capacity)
{++tag_reads;if(out&&capacity)snprintf(out,capacity,"%s",valid_tag?(X4_PUBLIC?"v9.8.7":"lab-9.8.7"):"");return valid_tag;}
static void consume_step(void){/* @UPDATE_CONSUME@ */}
static void offer_step(void){/* @UPDATE_OFFER@ */}
static uint32_t notice_signature(void)
{
    uint32_t next_ui_sig=2166136261u;
#define UI_HASH(v) do { next_ui_sig ^= (uint32_t)(v); next_ui_sig *= 16777619u; } while (0)
    /* @UPDATE_SIGNATURE@ */
#undef UI_HASH
    return next_ui_sig;
}
#if !X4_PUBLIC
static void lab_step(void){/* @UPDATE_LAB@ */}
#endif
static void reset(void)
{
    memset(&update_notice,0,sizeof(update_notice));memset(&screen,0,sizeof(screen));
    controller.pressed=0;picture_settings_ui.open=false;
    closing=fortnite_login_flow=pending_launch=live_retained=session_back=first_choice=busy=false;
    pending_auth=-1;release_status=X4_UPDATE_AVAILABLE;valid_tag=true;status_reads=tag_reads=0;
#if !X4_PUBLIC
    update_test_requested=false;can_edit=true;stream_settings_consumed=false;actions=ui_pressed=normal_r3_calls=0;
    stream_settings_ui.open=false;
#endif
}
int main(void)
{
    reset();offer_step();assert(update_notice.open&&update_notice.shown&&status_reads==2&&tag_reads==1);
    assert(notice_signature()!=2166136261u);
    assert(!strcmp(update_notice.url,X4_PUBLIC?"https://github.com/asp11edgr/XCloud4/releases/tag/v9.8.7":"https://github.com/asp11edgr/XCloud4/releases/tag/lab-9.8.7"));
    unsigned before=status_reads;offer_step();assert(status_reads==before); /* No second offer. */
    const unsigned other_buttons[]={ORBIS_PAD_BUTTON_CROSS,ORBIS_PAD_BUTTON_OPTIONS,ORBIS_PAD_BUTTON_TRIANGLE,0xffffu&~ORBIS_PAD_BUTTON_CIRCLE};
    for(unsigned i=0;i<sizeof(other_buttons)/sizeof(*other_buttons);++i){controller.pressed=other_buttons[i];consume_step();assert(update_notice.open&&!controller.pressed);}
    controller.pressed=ORBIS_PAD_BUTTON_CIRCLE|ORBIS_PAD_BUTTON_CROSS|ORBIS_PAD_BUTTON_OPTIONS;
    consume_step();assert(!update_notice.open&&!controller.pressed);offer_step();assert(!update_notice.open&&status_reads==before);
    for(unsigned blocker=0;blocker<13;++blocker){
        reset();switch(blocker){case 0:closing=true;break;case 1:screen.exit_requested=true;break;
        case 2:screen.page=4;break;case 3:picture_settings_ui.open=true;break;case 4:first_choice=true;break;
        case 5:screen.open_settings_requested=true;break;case 6:screen.explore_catalog_requested=true;break;
        case 7:fortnite_login_flow=true;break;case 8:pending_auth=1;break;case 9:pending_launch=true;break;
        case 10:busy=true;break;case 11:live_retained=true;break;case 12:session_back=true;break;}
        offer_step();assert(!update_notice.open&&!update_notice.shown&&!status_reads&&!tag_reads);
    }
    reset();valid_tag=false;offer_step();assert(update_notice.open&&!strcmp(update_notice.url,"https://github.com/asp11edgr/XCloud4/releases"));
    for(unsigned state=0;state<=X4_UPDATE_FAILED;++state)if(state!=X4_UPDATE_AVAILABLE){reset();release_status=(enum X4UpdateStatus)state;offer_step();assert(!update_notice.open&&!update_notice.shown&&!tag_reads);}
#if !X4_PUBLIC
    reset();screen.page=5;stream_settings_ui.open=true;controller.pressed=ORBIS_PAD_BUTTON_TRIANGLE;ui_pressed=1;
    lab_step();assert(update_test_requested&&!stream_settings_ui.open&&stream_settings_consumed&&!normal_r3_calls&&screen.page==0&&!controller.pressed&&!ui_pressed);
    release_status=X4_UPDATE_FAILED;offer_step();assert(update_notice.open&&!update_notice.shown&&!update_test_requested&&!strcmp(update_notice.version,"9.9.9"));
    assert(!strcmp(update_notice.url,"https://github.com/asp11edgr/XCloud4/releases")&&!status_reads);
    /* A sample may close and the first real offer open in the same frame.
     * Both are open, but their pixels and QR differ: never reuse the sample. */
    uint32_t sample_signature=notice_signature();
    controller.pressed=ORBIS_PAD_BUTTON_CIRCLE;consume_step();
    release_status=X4_UPDATE_AVAILABLE;offer_step();
    assert(update_notice.open&&update_notice.shown&&!controller.pressed);
    assert(!strcmp(update_notice.version,"9.8.7"));
    assert(!strcmp(update_notice.url,"https://github.com/asp11edgr/XCloud4/releases/tag/lab-9.8.7"));
    assert(notice_signature()!=sample_signature);
    for(unsigned button=0;button<3;++button){reset();screen.page=5;stream_settings_ui.open=true;
        controller.pressed=button==0?ORBIS_PAD_BUTTON_TRIANGLE|ORBIS_PAD_BUTTON_CIRCLE:button==1?ORBIS_PAD_BUTTON_TRIANGLE|ORBIS_PAD_BUTTON_OPTIONS:ORBIS_PAD_BUTTON_CROSS;
        lab_step();assert(!update_test_requested&&normal_r3_calls==1&&screen.page==5);}
    reset();stream_settings_ui.open=true;offer_step();assert(!update_notice.open&&!status_reads);
#else
    reset();offer_step();assert(update_notice.open&&update_notice.shown&&!strcmp(update_notice.version,"9.8.7"));
#endif
    printf("PASS update main profile public=%d: exact input consumption, Home gating, once per boot, first-choice/login/OPTIONS priority, controlled tag URL and Lab-only non-network sample\n",X4_PUBLIC);
    return 0;
}
