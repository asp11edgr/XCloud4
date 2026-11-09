/* SPDX-License-Identifier: GPL-3.0-only */
/* Real session control with simulated peers, monotonic time and /state.
 * No sockets, files or credentials. Only fixed dummy values reach buffers. */
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
static int quiet_printf(const char *format, ...);
#define printf quiet_printf
#include "../src/auth/xbox_live.c"
#undef printf
#include <assert.h>

struct X4Rtc { unsigned id; uint64_t opened_at; X4RtcMediaCallback media; void *user; bool closed; };
static struct X4Rtc peers[32];
static _Atomic int cancelled, reload;
static uint64_t clock_us, cancel_at;
static unsigned opens, closes, binds, queries, normal_calls, heartbeats, attempts, recovered, last_cause, deletes;
static bool deliver_video, fail_heartbeat, terminal_notice, initial_zero, terminal_on_close, connect_accepted;
static bool normal_stream, heartbeat_after_first, stop_after_recovery, fail_delete;
static unsigned stream_views;
static bool normal_transport_close, closing_snapshot;
static int heartbeat_error, heartbeat_status;
static const char *heartbeat_body, *http_stage;
static SessionEnd *stream_end;
static unsigned state_mode;
static uint64_t query_cost;
static X4XboxWork *owner;
static int quiet_printf(const char *format, ...) { (void)format; return 0; }
uint64_t sceKernelGetProcessTime(void) { return clock_us; }
int sceKernelUsleep(unsigned us)
{ clock_us += us; if(cancel_at && clock_us>=cancel_at)atomic_store(&cancelled,1);return 0; }
void x4_secure_clear(void *p,size_t n) { volatile unsigned char *b=p;while(n--)*b++=0; }
const char *x4_http_stage(const X4Http *h) { (void)h;return http_stage; }
static int response(char *out,size_t capacity,size_t *length,int *status,const char *value)
{ assert(strlen(value)<capacity);strcpy(out,value);*length=strlen(value);*status=200;return 0; }
int x4_http_session_persistence(X4Http *http,bool enabled)
{ (void)http; (void)enabled; return 0; }
int x4_http_session_state_request(X4Http *http,const char *url,const X4HttpHeader *headers,size_t hc,
    char *out,size_t cap,size_t *len,int *status,const _Atomic int *cancel)
{
    (void)http;(void)headers;(void)cancel;assert(hc==3 && strstr(url,"/state"));
    ++queries;clock_us+=query_cost;
    const char *value=state_mode==1&&!connect_accepted?"{\"state\":\"ReadyToConnect\"}":
        state_mode==2?"{\"state\":\"Provisioning\"}":
        state_mode==3?"{\"state\":\"WaitingForResources\"}":
        state_mode==4?"{\"state\":\"Failed\"}":
        state_mode==5?"{\"state\":\"Closed\",\"reason\":\"KickForOutOfStreamingTime\"}":
        "{\"state\":\"Provisioned\"}";
    return response(out,cap,len,status,value);
}
int x4_http_session_request(X4Http *http,enum X4HttpSessionMethod method,const char *url,
    const X4HttpHeader *headers,size_t hc,const char *json,char *out,size_t cap,size_t *len,int *status,const _Atomic int *cancel)
{
    (void)http;(void)headers;(void)json;(void)cancel;assert(hc==3);
    assert(!strstr(url,"/play"));
    if(method==X4_SESSION_HTTP_DELETE){
        ++deletes;*len=0;*status=0;out[0]=0;
        return fail_delete?-7100:response(out,cap,len,status,"{}");
    }
    ++normal_calls;
    if(strstr(url,"/connect"))connect_accepted=true;
    if(strstr(url,"/keepalive")){
        ++heartbeats;
        if(fail_heartbeat&&(!heartbeat_after_first||heartbeats>1)){
            if(heartbeat_status){int rc=response(out,cap,len,status,heartbeat_body?heartbeat_body:"{}");*status=heartbeat_status;return rc;}
            *len=0;*status=0;out[0]=0;return heartbeat_error;
        }
    }
    if(strstr(url,"/state"))return response(out,cap,len,status,"{\"state\":\"Provisioned\"}");
    if(strstr(url,"/sdp")&&method==X4_SESSION_HTTP_GET)
        return response(out,cap,len,status,"{\"exchangeResponse\":\"{\\\"sdp\\\":\\\"v=0\\\\r\\\\n\\\"}\"}");
    if(strstr(url,"/ice")&&method==X4_SESSION_HTTP_GET)
        return response(out,cap,len,status,"{\"exchangeResponse\":\"{\\\"candidates\\\":[]}\"}");
    return response(out,cap,len,status,"{}");
}
int x4_http_session_reconnect_request(X4Http *http,enum X4HttpSessionMethod method,const char *url,
    const X4HttpHeader *headers,size_t hc,const char *json,char *out,size_t cap,size_t *len,
    int *status,const _Atomic int *cancel,uint64_t deadline)
{
    if(deadline<=clock_us){*len=0;*status=0;out[0]=0;return X4_HTTP_DEADLINE;}
    if(strstr(url,"/state"))return x4_http_session_state_request(http,url,headers,hc,out,cap,len,status,cancel);
    return x4_http_session_request(http,method,url,headers,hc,json,out,cap,len,status,cancel);
}
X4Rtc *x4_rtc_open(int *error) {assert(normal_stream);*error=0;peers[0].opened_at=clock_us;return &peers[0];}
X4Rtc *x4_rtc_open_reconnect_request(int *error,const X4StreamRequestConfig *request)
{ (void)request;*error=0;assert(opens<31&&peers[opens].closed);++opens;peers[opens]=(struct X4Rtc){.id=opens,.opened_at=clock_us};return &peers[opens]; }
int x4_rtc_close(X4Rtc *rtc) { if(!rtc)return 0;assert(!rtc->closed);rtc->closed=true;++closes;return 0; }
int x4_rtc_close_snapshot(X4Rtc *rtc,X4RtcSnapshot *final)
{
    if(!rtc)return 0;
    if(final){
        closing_snapshot=true;
        x4_rtc_snapshot(rtc,final);
        closing_snapshot=false;
        if(terminal_on_close){final->server_closed=true;strcpy(final->server_reason,"KickForClosedGame");}
    }
    return x4_rtc_close(rtc);
}
void x4_rtc_set_media_callback(X4Rtc *r,X4RtcMediaCallback cb,void *u) {r->media=cb;r->user=u;}
void x4_rtc_set_diagnostic_trace(X4Rtc *r,X4Trace *t) {(void)r;(void)t;}
void x4_rtc_set_rumble_mailbox(X4Rtc *r,X4Rumble *t) {(void)r;(void)t;}
void x4_rtc_set_nack_grace_inbox(X4Rtc *r,X4NackGraceInbox *t) {(void)r;(void)t;}
int x4_rtc_set_gamepad_source(X4Rtc *r,X4GamepadSource source,void *u)
{(void)source;(void)u;if(!r->id){assert(normal_stream);return 0;}assert(clock_us-r->opened_at>=1000000);++binds;return 0;}
int x4_rtc_local_description(X4Rtc *r,char *s,size_t c) {(void)r;assert(c>6);strcpy(s,"v=0\r\n");return 1;}
int x4_rtc_set_remote_description(X4Rtc *r,const char *s) {(void)r;assert(!strncmp(s,"v=0",3));return 0;}
void x4_rtc_get_sdp_video(const X4Rtc *r,uint32_t *s,uint32_t *p,char *m,size_t c)
{(void)r;*s=0;*p=116;if(c)m[0]=0;}
int x4_rtc_next_local_candidate(X4Rtc *r,char *c,size_t cc,char *m,size_t mc)
{(void)r;(void)c;(void)cc;(void)m;(void)mc;return 0;}
int x4_rtc_add_remote_candidate(X4Rtc *r,const char *c,const char *m) {(void)r;(void)c;(void)m;return 0;}
int x4_rtc_get_selected_candidate_pair(X4Rtc *r,char *l,size_t lc,char *m,size_t mc)
{(void)r;if(lc)l[0]=0;if(mc)m[0]=0;return 0;}
void x4_rtc_snapshot(X4Rtc *r,X4RtcSnapshot *v)
{
    assert(r&&!r->closed);*v=(X4RtcSnapshot){.state=X4_RTC_CONNECTED};
    if(!r->id){
        v->video_packets=initial_zero?0:9;v->audio_packets=initial_zero?0:7;
        if(normal_stream&&normal_transport_close&&(closing_snapshot?stream_views:++stream_views)>1)
            v->state=X4_RTC_CLOSED;
        if(!closing_snapshot&&normal_stream&&r->media){uint8_t packet[12]={0};r->media(r->user,X4_RTC_VIDEO,packet,sizeof(packet));r->media(r->user,X4_RTC_AUDIO,packet,sizeof(packet));}
        return;
    }
    assert(owner->session.state==X4_SESSION_STREAMING);
    if(!owner->session.reconnecting){
        v->video_packets=2;v->audio_packets=1;
        if(stop_after_recovery&&!closing_snapshot){
            assert(stream_end&&!stream_end->error&&!stream_end->stage[0]);
            atomic_store(&cancelled,1);
        }
        return;
    }
    if(terminal_notice){v->server_closed=true;strcpy(v->server_reason,"KickForClosedGame");return;}
    if(deliver_video && clock_us-r->opened_at>=1000000){
        v->video_packets=2;v->audio_packets=1;
        if(!closing_snapshot&&r->media){uint8_t packet[12]={0};r->media(r->user,X4_RTC_VIDEO,packet,sizeof(packet));}
    }
}
int x4_rtc_request_keyframe(X4Rtc *r) {(void)r;return 0;}
int x4_rtc_get_rtt_ms(X4Rtc *r) {(void)r;return -1;}
void x4_rtc_diag_record_rtt_sample(int rtt) {(void)rtt;}
void x4_rtc_diag_get_rtt_summary(X4RttSummary *out) {*out=(X4RttSummary){0};}
bool x4_server_settings_record_game_rtt(const char *region,int rtt) {(void)region;(void)rtt;return true;}
void x4_rtc_diag_record_remote_candidate(const char *c,const char *m) {(void)c;(void)m;}
void x4_rtc_diag_record_selected_pair(const char *l,const char *r) {(void)l;(void)r;}
void x4_rtc_diag_reconnect_attempt(unsigned cause) {assert(cause>=1&&cause<=X4_RECONNECT_KEEPALIVE);last_cause=cause;++attempts;}
void x4_rtc_diag_reconnect_result(unsigned cause,bool yes,uint64_t ms)
{assert(cause>=1&&cause<=X4_RECONNECT_KEEPALIVE);if(yes){assert(ms>=1000);++recovered;}}
static int passport(void *ctx,X4Http *h,char *out,size_t cap,int *status,const char **stage,const _Atomic int *cancel)
{(void)ctx;(void)h;(void)stage;(void)cancel;assert(cap>12);strcpy(out,"FAKE_PASSPORT");*status=200;return 0;}
static X4XboxWork *fixture(void)
{
    owner=calloc(1,sizeof(*owner));assert(owner);clock_us=1000000;cancel_at=0;
    atomic_store(&cancelled,0);atomic_store(&reload,0);atomic_init(&owner->last_video_packet_us,0);atomic_init(&owner->last_audio_packet_us,0);
    opens=closes=binds=queries=normal_calls=heartbeats=attempts=recovered=last_cause=deletes=0;
    deliver_video=true;fail_heartbeat=terminal_notice=initial_zero=terminal_on_close=connect_accepted=false;state_mode=0;query_cost=0;
    normal_stream=heartbeat_after_first=stop_after_recovery=fail_delete=false;
    stream_views=0;normal_transport_close=closing_snapshot=false;
    heartbeat_error=-7000;heartbeat_status=0;heartbeat_body=NULL;http_stage="fixture";stream_end=NULL;
    memset(peers,0,sizeof(peers));owner->rtc=&peers[0];owner->cancel=&cancelled;owner->reload_requested=&reload;
    owner->session.state=X4_SESSION_STREAMING;owner->had_connected=true;owner->passport_provider=passport;
    strcpy(owner->origin,"https://fixture.gssv-play-prod.xboxlive.com");strcpy(owner->session_id,"FAKE_SESSION");
    return owner;
}
static void release_fixture(void) {session_peer_close(owner,NULL);x4_secure_clear(owner,sizeof(*owner));free(owner);owner=NULL;}
int main(void)
{
    X4XboxWork *w=fixture();X4RtcSnapshot v={.state=X4_RTC_FAILED};
    assert(session_reconnect_trigger(w,&v,clock_us,clock_us,0)==X4_RECONNECT_FAILED);
    w->had_connected=false;assert(!session_reconnect_trigger(w,&v,clock_us,clock_us,0));w->had_connected=true;
    v.state=X4_RTC_DISCONNECTED;assert(!session_reconnect_trigger(w,&v,clock_us+2000000,clock_us,clock_us));
    assert(session_reconnect_trigger(w,&v,clock_us+3000000,clock_us,clock_us)==X4_RECONNECT_DISCONNECTED);
    v.state=X4_RTC_CONNECTED;assert(session_reconnect_trigger(w,&v,clock_us+3000000,clock_us,0)==X4_RECONNECT_NO_VIDEO);
    atomic_store(&w->last_audio_packet_us,clock_us+3000000);assert(!session_reconnect_trigger(w,&v,clock_us+3000000,clock_us,0));
    atomic_store(&w->last_audio_packet_us,clock_us+15000000);assert(session_reconnect_trigger(w,&v,clock_us+15000000,clock_us,0)==X4_RECONNECT_NO_VIDEO);
    atomic_store(&reload,1);assert(session_reconnect_trigger(w,&v,clock_us,clock_us,0)==X4_RECONNECT_MANUAL);
    v.server_closed=true;atomic_store(&reload,1);assert(!session_reconnect_trigger(w,&v,clock_us,clock_us,0));release_fixture();
    for(unsigned mode=0;mode<2;++mode){w=fixture();state_mode=mode;SessionEnd end={0};
        assert(session_reconnect(w,&end,X4_RECONNECT_FAILED,-12));
        assert(w->session.state==X4_SESSION_STREAMING&&!w->session.reconnecting&&w->session.reconnects==1);
        assert(w->peer_video_base==9&&w->peer_audio_base==7&&opens==1&&binds==1&&closes==1&&recovered==1);
        assert(!atomic_load(&w->reload_requested[0]));release_fixture();}
    for(unsigned mode=2;mode<4;++mode){w=fixture();state_mode=mode;SessionEnd end={0};
        assert(!session_reconnect(w,&end,X4_RECONNECT_MANUAL,0));assert(end.error==X4_AUTH_E_CONNECTION_LOST&&attempts==20&&queries==20&&!opens);release_fixture();}
    w=fixture();deliver_video=false;SessionEnd end={0};assert(!session_reconnect(w,&end,X4_RECONNECT_FAILED,0));
    assert(end.error==X4_AUTH_E_CONNECTION_LOST&&clock_us<=121100000&&attempts<=12);release_fixture();
    w=fixture();state_mode=4;end=(SessionEnd){0};assert(!session_reconnect(w,&end,X4_RECONNECT_FAILED,0));
    assert(end.error==X4_AUTH_E_XBOX_CLOSED&&queries==1&&!opens);release_fixture();
    w=fixture();state_mode=5;end=(SessionEnd){0};assert(!session_reconnect(w,&end,X4_RECONNECT_FAILED,0));
    assert(end.error==X4_AUTH_E_XBOX_CLOSED&&!strcmp(end.stage,X4_STAGE_SERVER_STREAMING_TIME));
    assert(queries==1&&!opens);release_fixture();
    w=fixture();state_mode=2;cancel_at=clock_us+1500000;end=(SessionEnd){0};
    assert(!session_reconnect(w,&end,X4_RECONNECT_MANUAL,0));assert(end.state==X4_SESSION_CANCELLED&&!w->rtc);release_fixture();
    w=fixture();fail_heartbeat=true;end=(SessionEnd){0};assert(session_reconnect(w,&end,X4_RECONNECT_FAILED,0));assert(heartbeats>=1);release_fixture();
    w=fixture();terminal_notice=true;end=(SessionEnd){0};assert(!session_reconnect(w,&end,X4_RECONNECT_FAILED,0));
    assert(end.error==X4_AUTH_E_XBOX_CLOSED&&queries==1);release_fixture();
    w=fixture();terminal_on_close=true;end=(SessionEnd){0};assert(!session_reconnect(w,&end,X4_RECONNECT_FAILED,0));
    assert(end.error==X4_AUTH_E_XBOX_CLOSED&&!queries&&!opens);release_fixture();
    w=fixture();initial_zero=true;clock_us+=60000000;end=(SessionEnd){0};
    assert(session_reconnect(w,&end,X4_RECONNECT_NO_VIDEO,0));assert(!end.error&&w->session.reconnects==1);release_fixture();
    /* Transport-only heartbeat failures are private decisions until the
     * common reconnect route owns them; a live HTTP reply is never retried. */
    static const char *const network_stages[]={"resolucion DNS","conexion HTTPS","envio HTTPS/TLS","lectura de respuesta"};
    static const int network_errors[]={(int32_t)UINT32_C(0x80410136),
        (int32_t)UINT32_C(0x8041013c),(int32_t)UINT32_C(0x804101e2),-1100};
    for(unsigned i=0;i<sizeof(network_stages)/sizeof(network_stages[0]);++i){
        w=fixture();http_stage=network_stages[i];fail_heartbeat=true;heartbeat_error=network_errors[i];end=(SessionEnd){0};
        assert(!signal_keepalive(w,UINT64_MAX,&end));v=(X4RtcSnapshot){.state=X4_RTC_CONNECTED};
        assert(end.error==X4_AUTH_E_CONNECTION_LOST&&end.state==X4_SESSION_ERROR&&!end.status);
        assert(w->session.state==X4_SESSION_STREAMING&&!w->session.keepalive_http_status);
        unsigned cause=X4_RECONNECT_KEEPALIVE;
        assert(!strcmp(session_reconnect_label(cause),"keepalive"));
        memset(&end,0,sizeof(end));assert(session_reconnect(w,&end,cause,0));
        assert(!end.error&&recovered==1&&last_cause==X4_RECONNECT_KEEPALIVE&&!deletes);release_fixture();
    }
    const int local_errors[]={X4_HTTP_CANCELLED,X4_HTTP_BOUND,X4_HTTP_URL,X4_HTTP_ALLOCATION,
        X4_HTTP_ARGUMENT,X4_HTTP_UNUSABLE,X4_HTTP_PROTOCOL,X4_HTTP_BUSY,
        X4_HTTP_TLS_ALERT,X4_HTTP_TLS_CERTIFICATE,X4_HTTP_TLS_CERTIFICATE_TIME,-1099};
    for(unsigned i=0;i<sizeof(local_errors)/sizeof(local_errors[0]);++i){
        w=fixture();fail_heartbeat=true;heartbeat_error=local_errors[i];end=(SessionEnd){0};
        assert(!signal_keepalive(w,UINT64_MAX,&end));v=(X4RtcSnapshot){.state=X4_RTC_FAILED};
        assert(end.error!=X4_AUTH_E_CONNECTION_LOST&&!attempts);release_fixture();
    }
    w=fixture();fail_heartbeat=true;heartbeat_error=X4_HTTP_DEADLINE;end=(SessionEnd){0};
    assert(!signal_keepalive(w,UINT64_MAX,&end));v=(X4RtcSnapshot){.state=X4_RTC_CONNECTED};
    assert(end.error==X4_AUTH_E_CONNECTION_LOST);release_fixture();

    /* Exercise session_stream itself, including pending-cause consumption
     * and clearing the old transport error before a successful reconnect. */
    w=fixture();normal_stream=true;w->had_connected=false;w->provisioned=true;
    normal_transport_close=terminal_on_close=true;end=(SessionEnd){0};
    session_stream(w,&end);
    assert(end.error==X4_AUTH_E_XBOX_CLOSED&&!strcmp(end.stage,X4_STAGE_SERVER_GAME_CLOSED));
    assert(!queries&&!attempts&&!w->rtc&&closes==1);
    session_cleanup(w,&end);
    assert(deletes==1&&!strcmp(w->session.stage,X4_STAGE_SERVER_GAME_CLOSED));release_fixture();

    w=fixture();normal_stream=true;w->had_connected=false;w->provisioned=true;
    fail_heartbeat=heartbeat_after_first=stop_after_recovery=true;end=(SessionEnd){0};stream_end=&end;
    session_stream(w,&end);
    assert(end.state==X4_SESSION_CANCELLED&&!end.error&&!w->rtc);
    assert(recovered==1&&attempts==1&&last_cause==X4_RECONNECT_KEEPALIVE&&!deletes);release_fixture();

    w=fixture();normal_stream=true;w->had_connected=false;w->provisioned=true;
    state_mode=2;fail_heartbeat=heartbeat_after_first=true;end=(SessionEnd){0};
    session_stream(w,&end);
    assert(end.error==X4_AUTH_E_CONNECTION_LOST&&attempts==20&&queries==20&&last_cause==X4_RECONNECT_KEEPALIVE);
    assert(!strcmp(end.stage,"Se perdió la conexión con el servidor de Xbox")&&!w->rtc&&!deletes);
    fail_delete=true;session_cleanup(w,&end);
    assert(deletes==1&&w->session.cleanup_failed&&w->session.cleanup_error==-7100);
    assert(w->session.error==X4_AUTH_E_CONNECTION_LOST&&!strcmp(w->session.stage,end.stage));release_fixture();

    static const struct { int status; const char *body; int error; } terminal_heartbeats[]={
        {404,"{}",X4_AUTH_E_STATUS},{410,"{\"code\":\"SessionNotActive\"}",X4_AUTH_E_STATUS},
        {200,"{\"code\":\"SessionNotActive\"}",X4_AUTH_E_SESSION},
        {200,"{\"code\":\"SessionNotFound\"}",X4_AUTH_E_SESSION}};
    for(unsigned i=0;i<sizeof(terminal_heartbeats)/sizeof(terminal_heartbeats[0]);++i){
        w=fixture();normal_stream=true;w->had_connected=false;w->provisioned=true;
        fail_heartbeat=heartbeat_after_first=true;heartbeat_status=terminal_heartbeats[i].status;
        heartbeat_body=terminal_heartbeats[i].body;end=(SessionEnd){0};session_stream(w,&end);
        assert(end.error==terminal_heartbeats[i].error&&end.status==terminal_heartbeats[i].status);
        assert(!attempts&&!recovered&&!queries&&!opens&&!w->rtc);release_fixture();
    }
    w=fixture();normal_stream=true;w->had_connected=false;w->provisioned=true;fail_heartbeat=true;end=(SessionEnd){0};
    session_stream(w,&end);
    assert(!w->had_connected&&!attempts&&!queries&&!recovered&&end.error==-7000);
    assert(!strcmp(end.stage,"HTTPS: fixture")&&!w->rtc);release_fixture();
    puts("PASS real reconnect: original four triggers and bounded recovery; keepalive network trigger and normal session loop; 404/410/SessionNotActive/SessionNotFound terminal; preconnect unchanged; CONNECTION_LOST survives failed DELETE");
    return 0;
}
