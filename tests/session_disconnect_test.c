/* SPDX-License-Identifier: GPL-3.0-only */
/* Production decision/parser, snapshot boundary, and real HTTP state API.
 * Native HTTP operations and RTC snapshots are simulated; no network/I/O. */
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef X4_T41_LIFETIME_INTEGRATION
#define capture_printf t41_genshin_log
#define CAPTURE_LINKAGE
#else
#define CAPTURE_LINKAGE static
#endif
CAPTURE_LINKAGE int capture_printf(const char *, ...);
#define printf capture_printf
#include "../src/auth/xbox_live.c"
#undef printf

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "disconnect check failed line=%d\n", __LINE__); abort(); } } while (0)
static uint64_t fake_clock = 1000000;
uint64_t sceKernelGetProcessTime(void) { return fake_clock; }
void t29_state_http_clock_advance(uint64_t us) { fake_clock += us; }
void test_session_state_http(void);
void t29_state_http_init(void);
unsigned t29_state_http_calls(void);
X4Http *t29_state_http_context(void);
void t29_state_http_finish(void);
void t29_state_http_response(const char *, int);

static char captured[8192];
static size_t captured_size;
static X4RtcSnapshot snapshot;
static X4Rtc *snapshot_context;
#ifdef X4_T41_LIFETIME_INTEGRATION
static bool genshin_peer;
void t41_rtc_snapshot(X4Rtc *,X4RtcSnapshot *);
X4Rtc *t41_genshin_open(uint64_t at);
void t41_genshin_notice(X4Rtc *);
void t41_genshin_close(X4Rtc *,uint64_t at);
void t41_genshin_drain(X4Rtc *,X4RtcSnapshot *);
#endif
CAPTURE_LINKAGE int capture_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    size_t room = sizeof(captured)-captured_size;
    int result = vsnprintf(captured+captured_size, room, format, args);
    va_end(args);
    if (result > 0) captured_size += (size_t)result < room ? (size_t)result : room-1;
    return result;
}
void x4_rtc_snapshot(X4Rtc *rtc, X4RtcSnapshot *view)
{
    CHECK(rtc == snapshot_context && view);
#ifdef X4_T41_LIFETIME_INTEGRATION
    if(genshin_peer){t41_rtc_snapshot(rtc,view);return;}
#endif
    *view = snapshot;
}
int x4_rtc_get_selected_candidate_pair(X4Rtc *rtc, char *local, size_t lc, char *remote, size_t rc)
{ (void)rtc;(void)local;(void)lc;(void)remote;(void)rc;CHECK(false);return -1; }
void x4_rtc_diag_record_selected_pair(const char *local, const char *remote)
{ (void)local;(void)remote;CHECK(false); }

static unsigned forwarded;
static const uint8_t *expected_packet;
static void receive_original(void *user, int kind, const uint8_t *rtp, size_t size)
{
    CHECK(user == &forwarded && (kind == X4_RTC_VIDEO || kind == X4_RTC_AUDIO));
    CHECK(rtp == expected_packet && size == 12);
    ++forwarded;
}

static void decision_case(X4RtcState rtc, uint64_t gap, int expected, const char *json)
{
    SessionRemoteStatus remote;
    CHECK(session_remote_status(json, strlen(json), &remote));
    int code = session_disconnect_code(rtc, gap);
    CHECK(code == expected);
    if (!code) return;
    char message[80];
    session_disconnect_stage(code, &remote, message, sizeof(message));
    CHECK(strstr(message, code == X4_AUTH_E_XBOX_CLOSED ?
        "Xbox cerró la sesión" : "Se perdió la conexión con el servidor de Xbox") == message);
    CHECK((strstr(message, "SessionExpired") != NULL) == (remote.reason[0] != 0));
}

static X4XboxWork *fixture_work(void)
{
    X4XboxWork *w = calloc(1, sizeof(*w)); CHECK(w);
    atomic_init(&w->last_video_packet_us, 9500000);
    atomic_init(&w->last_audio_packet_us, 0);
    atomic_init(&w->first_video_packet_us, 0);
    w->session_start = 6000000;
    w->http = t29_state_http_context();
    copy_text(w->origin, sizeof(w->origin), "https://fixture-region.gssv-play-prod.xboxlive.com");
    copy_text(w->session_id, sizeof(w->session_id), "FAKE_SESSION");
    copy_text(w->authorization, sizeof(w->authorization), "Bearer FAKE_DISCONNECT_29");
    w->rtc = (X4Rtc *)w; snapshot_context = w->rtc;
    fake_clock = 10000000;
    captured_size = 0; captured[0] = 0;
    return w;
}

static void initial_failure_case(X4RtcState state, int native_error)
{
    t29_state_http_init(); X4XboxWork *w = fixture_work();
    snapshot = (X4RtcSnapshot){.state=state,.error=native_error};
    SessionEnd end = {0}; X4RtcSnapshot view;
    CHECK(!signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(end.error == (native_error ? native_error : X4_AUTH_E_SESSION));
    CHECK(!strcmp(end.stage,"fallo la conexion WebRTC"));
    CHECK(!w->had_connected && !w->disconnect_query_done && !t29_state_http_calls());
    t29_state_http_finish(); x4_secure_clear(w,sizeof(*w));free(w);
}

static void connected_loss_case(X4RtcState state, int native_error, const char *json, bool reason)
{
    t29_state_http_init(); X4XboxWork *w = fixture_work();
    t29_state_http_response(json,200);
    snapshot = (X4RtcSnapshot){.state=X4_RTC_CONNECTED,.video_packets=1,.audio_packets=1};
    SessionEnd end = {0}; X4RtcSnapshot view;
    CHECK(signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(w->had_connected && !w->disconnect_query_done && !t29_state_http_calls());
    captured_size=0;captured[0]=0;
    snapshot.state=state;snapshot.error=native_error;
    if (state == X4_RTC_FAILED) {
        CHECK(signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
        CHECK(session_reconnect_trigger(w,&view,fake_clock,6000000,0)==X4_RECONNECT_FAILED);
        CHECK(!end.error && !w->disconnect_query_done && !t29_state_http_calls());
        t29_state_http_finish(); x4_secure_clear(w,sizeof(*w));free(w);
        return;
    }
    CHECK(!signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(end.transport_closed && !w->disconnect_query_done && !t29_state_http_calls());
    CHECK(!session_classify_peer_close(w,&end,&view));
    int expected=state==X4_RTC_CLOSED?X4_AUTH_E_XBOX_CLOSED:X4_AUTH_E_CONNECTION_LOST;
    CHECK(end.error==expected && w->disconnect_query_done && t29_state_http_calls()==1);
    CHECK((strstr(end.stage,"SessionExpired")!=NULL)==reason);
    CHECK(strstr(captured,"ultimo_video_ms=500") && strstr(captured,"sesion_s=4"));
    CHECK(strstr(captured,state==X4_RTC_CLOSED?"estado=CLOSED":"estado=FAILED"));
    CHECK(strstr(captured,native_error?"rtc_error=0x80431068":"rtc_error=0x00000000"));
    CHECK(!strstr(captured,"192.0.2.") && !strstr(captured,"2001:db8") &&
        !strstr(captured,"candidate:") && !strstr(captured,"Bearer") &&
        !strstr(captured,"FAKE_SESSION") && !strstr(captured,"fixture-private-placeholder"));
    CHECK(!w->response[0]);
    /* Defensive re-entry must not issue a second state request. */
    CHECK(!signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(!session_classify_peer_close(w,&end,&view));
    CHECK(end.error==expected && t29_state_http_calls()==1);
    t29_state_http_finish(); x4_secure_clear(w,sizeof(*w));free(w);
}

static void play_error_case(int status, const char *body, const char *safe_code,
    const char *safe_message, bool entitlement)
{
    X4XboxWork *w = fixture_work();
    copy_text(w->response, sizeof(w->response), body);
    session_play_failure(w, strlen(body), status);
    CHECK(w->session.state == X4_SESSION_ERROR && w->session.http_status == status);
    CHECK(w->session.error == (status == 403 ? X4_AUTH_E_XBOX : X4_AUTH_E_STATUS));
    CHECK(w->response[0] == 0);
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    const char *es = x4_tr_stage(w->session.stage);
    if (entitlement)
        CHECK(!strcmp(es, "Este juego debe añadirse a tu cuenta. Ábrelo una vez en xbox.com/play y pulsa «Obtener»; después vuelve a intentarlo."));
    else {
        CHECK(strstr(es, X4_STAGE_PLAY_FAILED_PREFIX) == es);
        if (safe_code) CHECK(strstr(es, safe_code));
        else {
            char expected[16];
            snprintf(expected, sizeof(expected), "HTTP %d", status);
            CHECK(strstr(es, expected));
        }
    }
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    const char *en = x4_tr_stage(w->session.stage);
    CHECK(strstr(en, entitlement ? "This game must be added" : "Xbox could not start this game (code ") == en);
#if !X4_PUBLIC
    char expected_log[320];
    snprintf(expected_log, sizeof(expected_log), "error /play http=%d code=%s message=%s", status,
        safe_code ? safe_code : "sin_codigo", safe_message);
    CHECK(strstr(captured, expected_log));
#else
    (void)safe_message;
    CHECK(!strstr(captured, "error /play"));
#endif
    CHECK(!strstr(captured, "FAKE_PRIVATE") && !strstr(w->session.stage, "FAKE_PRIVATE"));
    free(w);
}

static void early_close_case(uint64_t video_duration, const char *body, int http,
    bool explicit_close, bool early)
{
    t29_state_http_init();
    X4XboxWork *w = fixture_work();
    fake_clock = 30000000;
    t29_state_http_response(body, http);
    w->had_connected = true;
    atomic_store(&w->first_video_packet_us, video_duration ? fake_clock - video_duration : 0);
    snapshot = (X4RtcSnapshot){.state=X4_RTC_CLOSED,.server_closed=explicit_close};
    if (explicit_close)
        copy_text(snapshot.server_reason, sizeof(snapshot.server_reason), "FAKE_PRIVATE_GAMERTAG");
    SessionEnd end = {0};
    X4RtcSnapshot view;
    CHECK(!signal_rtc_view(w, &end, &view, UINT64_MAX, NULL));
    CHECK(!t29_state_http_calls());
    CHECK(session_classify_peer_close(w,&end,&view) == explicit_close);
    CHECK(end.error == X4_AUTH_E_XBOX_CLOSED);
    CHECK(t29_state_http_calls() == (explicit_close ? 0 : 1));
    if (early) {
        CHECK(!strcmp(end.stage, X4_STAGE_EARLY_XBOX_CLOSE));
        x4_lang_set(X4_LANG_ES, "es-MX", "MX");
        CHECK(!strcmp(x4_tr_stage(end.stage),
            "Xbox cerró el juego al iniciarlo. Si también falla en xbox.com/play, el problema es del juego o de tu cuenta."));
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        CHECK(strstr(x4_tr_stage(end.stage), "Xbox closed the game at launch."));
    } else CHECK(strstr(end.stage, "Xbox cerró la sesión") == end.stage);
    CHECK(!strstr(captured, "FAKE_PRIVATE") && !strstr(end.stage, "FAKE_PRIVATE"));
    t29_state_http_finish();
    free(w);
}

static void server_notice_case(const char *reason, const char *key, const char *es, const char *en,
    bool late)
{
    t29_state_http_init();
    X4XboxWork *w = fixture_work();
    w->had_connected = true;
    snapshot = (X4RtcSnapshot){.state=X4_RTC_CLOSED,.error=-42,.server_closed=!late};
    if (!late) copy_text(snapshot.server_reason,sizeof(snapshot.server_reason),reason);
    SessionEnd end = {0};
    X4RtcSnapshot view;
    CHECK(!signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(end.transport_closed == late && !t29_state_http_calls());
    if (late) {
        view.server_closed = true;
        copy_text(view.server_reason,sizeof(view.server_reason),reason);
    }
    CHECK(session_classify_peer_close(w,&end,&view));
    CHECK(!end.transport_closed && end.error == X4_AUTH_E_XBOX_CLOSED);
    CHECK(!t29_state_http_calls() && !strstr(captured,"sin_motivo") && strstr(captured,reason));
    CHECK(!strcmp(end.stage,key));
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    CHECK(!strcmp(x4_tr_stage(end.stage),es));
    x4_lang_set(X4_LANG_EN,"en-US","US");
    CHECK(!strcmp(x4_tr_stage(end.stage),en));
    t29_state_http_finish();
    free(w);
}

#ifdef X4_T41_LIFETIME_INTEGRATION
static void genshin_notice_then_dtls_close(void)
{
    t29_state_http_init();
    X4XboxWork *w=fixture_work();
    w->rtc=snapshot_context=t41_genshin_open(fake_clock);
    genshin_peer=true;
    atomic_store(&w->first_video_packet_us,fake_clock-100000);
    SessionEnd end={0};
    X4RtcSnapshot view;
    CHECK(signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(w->had_connected&&!view.server_closed);

    t41_genshin_notice(w->rtc);
    fake_clock+=3600000;
    t41_genshin_close(w->rtc,fake_clock);
    CHECK(!signal_rtc_view(w,&end,&view,UINT64_MAX,NULL));
    CHECK(view.state==X4_RTC_CLOSED&&view.server_closed);
    CHECK(!end.transport_closed&&end.error==X4_AUTH_E_XBOX_CLOSED);
    X4RtcSnapshot final;
    t41_genshin_drain(w->rtc,&final);
    w->rtc=NULL;
    genshin_peer=false;
    CHECK(session_classify_peer_close(w,&end,&final));
    CHECK(!t29_state_http_calls()&&!strstr(captured,"sin_motivo"));

    static const char terminal[]="cierre Xbox motivo=KickForClosedGame";
    const char *reported=strstr(captured,terminal);
    CHECK(reported&&!strstr(reported+1,terminal));
    CHECK(!strstr(captured,"FAKE_PRIVATE_TRANSACTION"));
#if !X4_PUBLIC
    CHECK(strstr(captured,"resultado=aceptado paso=aviso motivo=terminal"));
    CHECK(strstr(captured,"RTC lifetime paso=ack rc=0"));
    CHECK(strstr(captured,"parte=DTLS origen=observado"));
#else
    CHECK(!strstr(captured,"RTC lifetime"));
#endif
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    CHECK(!strcmp(x4_tr_stage(end.stage),
        "El juego se cerró en el servidor de Xbox. Si también falla en xbox.com/play, es un problema del juego."));
    puts("PASS T41 Genshin: opaque string ACK, DTLS +3600 ms, exact Xbox terminal log and Spanish copy");
    t29_state_http_finish();
    free(w);
}
#endif

int main(void)
{
    const char *fixtures[] = {"{\"state\":\"Closed\"}",
        "{\"state\":\"Closed\",\"errorDetails\":{\"code\":\"SessionExpired\"}}"};
    for (unsigned i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); ++i) {
        decision_case(X4_RTC_CLOSED, 0, X4_AUTH_E_XBOX_CLOSED, fixtures[i]);
        decision_case(X4_RTC_FAILED, 0, X4_AUTH_E_CONNECTION_LOST, fixtures[i]);
        decision_case(X4_RTC_DISCONNECTED, 2000000, 0, fixtures[i]);
        decision_case(X4_RTC_DISCONNECTED, 3000000, X4_AUTH_E_CONNECTION_LOST, fixtures[i]);
        decision_case(X4_RTC_DISCONNECTED, 5000000, X4_AUTH_E_CONNECTION_LOST, fixtures[i]);
        decision_case(X4_RTC_DISCONNECTED, 11000000, X4_AUTH_E_CONNECTION_LOST, fixtures[i]);
    }
    CHECK(!session_disconnect_code(X4_RTC_CONNECTED, UINT64_MAX));
    SessionRemoteStatus remote;
    const char *numeric = "{\"state\":\"Failed\",\"error\":{\"code\":-2147483648}}";
    CHECK(session_remote_status(numeric, strlen(numeric), &remote));
    CHECK(!strcmp(remote.reason, "0x80000000"));
    const char *reason = "{\"state\":\"Closed\",\"reason\":\"SessionTimeLimitExceeded\"}";
    CHECK(session_remote_status(reason, strlen(reason), &remote));
    CHECK(!strcmp(remote.reason, "SessionTimeLimitExceeded"));
    const char *kick = "{\"state\":\"Closed\",\"reason\":\"KickForOutOfStreamingTime\"}";
    CHECK(session_remote_status(kick,strlen(kick),&remote));
    char stage[80];
    session_disconnect_stage(X4_AUTH_E_XBOX_CLOSED,&remote,stage,sizeof(stage));
    CHECK(!strcmp(stage,X4_STAGE_SERVER_STREAMING_TIME));
    for (unsigned late = 0; late < 2; ++late) {
        server_notice_case("KickForClosedGame",X4_STAGE_SERVER_GAME_CLOSED,
            "El juego se cerró en el servidor de Xbox. Si también falla en xbox.com/play, es un problema del juego.",
            "The game closed on the Xbox server. If it also fails at xbox.com/play, the problem is with the game.",late);
        server_notice_case("KickForOutOfStreamingTime",X4_STAGE_SERVER_STREAMING_TIME,
            "Se acabó tu tiempo de juego en la nube de este mes (Xbox limita las horas según tu Game Pass). Puedes comprar más horas en xbox.com.",
            "Your cloud gaming time for this month has run out (Xbox limits hours based on your Game Pass plan). You can buy more hours at xbox.com.",late);
    }
#ifdef X4_T41_LIFETIME_INTEGRATION
    genshin_notice_then_dtls_close();
#endif
    const char *untrusted = "{\"state\":\"Closed\",\"reason\":\"192.0.2.4:1234\",\"error\":{\"message\":\"fixture-private-placeholder\"}}";
    CHECK(session_remote_status(untrusted, strlen(untrusted), &remote));
    CHECK(!remote.reason[0]);
    const char *invalid[] = {"{}", "{\"state\":5}", "{\"state\":\"Closed\",\"state\":\"Failed\"}", "[]", "not-json"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        CHECK(!session_remote_status(invalid[i], strlen(invalid[i]), &remote));
        CHECK(!remote.reason[0] && !remote.state[0]);
    }
    X4XboxWork *w = calloc(1, sizeof(*w));
    CHECK(w != NULL);
    atomic_init(&w->last_video_packet_us, 0);
    atomic_init(&w->last_audio_packet_us, 0);
    atomic_init(&w->first_video_packet_us, 0);
    w->media_callback = receive_original; w->media_user = &forwarded;
    uint8_t packet[12] = {0}; expected_packet = packet;
    fake_clock = 1000000;
    session_media(w, X4_RTC_VIDEO, packet, sizeof(packet));
    CHECK(atomic_load(&w->last_video_packet_us) == 1000000 && forwarded == 1);
    CHECK(atomic_load(&w->first_video_packet_us) == 1000000);
    fake_clock = 2000000;
    session_media(w, X4_RTC_AUDIO, packet, sizeof(packet));
    CHECK(atomic_load(&w->last_video_packet_us) == 1000000 && forwarded == 2);
    CHECK(atomic_load(&w->last_audio_packet_us) == 2000000);
    session_media(w, X4_RTC_VIDEO, packet, sizeof(packet));
    CHECK(atomic_load(&w->last_video_packet_us) == 2000000 && forwarded == 3);
    CHECK(atomic_load(&w->first_video_packet_us) == 1000000);
    free(w);
    test_session_state_http();
    initial_failure_case(X4_RTC_FAILED,0);
    initial_failure_case(X4_RTC_FAILED,(int)0x80431068u);
    initial_failure_case(X4_RTC_CLOSED,0);
    initial_failure_case(X4_RTC_CLOSED,(int)0x80431068u);
    for (unsigned i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {
        connected_loss_case(X4_RTC_CLOSED,0,fixtures[i],i!=0);
        connected_loss_case(X4_RTC_FAILED,(int)0x80431068u,fixtures[i],i!=0);
    }
    connected_loss_case(X4_RTC_FAILED,(int)0x80431068u,untrusted,false);
    play_error_case(400, "{\"error\":{\"code\":\"TitleNotEntitled\",\"message\":\"The user is not entitled to play this title.\"}}",
        "TitleNotEntitled", "The user is not entitled to play this title.", true);
    play_error_case(400, "{\"message\":\"The user does not have a license for this title.\"}",
        NULL, "The user does not have a license for this title.", true);
    play_error_case(400, "{\"code\":\"FAKE_PRIVATE_CODE\",\"message\":\"FAKE_PRIVATE_GAMERTAG / FAKE_PRIVATE_TOKEN\",\"id\":\"FAKE_PRIVATE_SESSION\"}",
        NULL, "oculto", false);
    play_error_case(403, "{\"ErrorCode\":-2147467259,\"Message\":\"Forbidden\"}",
        "0x80004005", "Forbidden", false);
    play_error_case(403, "{\"code\":\"0x80150001\",\"message\":\"FAKE_PRIVATE_TOKEN\"}",
        "0x80150001", "oculto", false);
    play_error_case(403, "", NULL, "sin_mensaje", false);
    play_error_case(400, "{\"message\":\"FAKE_PRIVATE_TOKEN", NULL, "sin_mensaje", false);
    early_close_case(2000000, fixtures[0], 200, false, true);
    early_close_case(14999999, fixtures[0], 200, false, true);
    early_close_case(15000000, fixtures[0], 200, false, false);
    early_close_case(0, fixtures[0], 200, false, false);
    early_close_case(2000000, fixtures[1], 200, false, false);
    early_close_case(2000000, untrusted, 200, false, false);
    early_close_case(2000000, fixtures[0], 404, false, false);
    early_close_case(2000000, fixtures[0], 200, true, false);
    puts("PASS launch diagnostics: /play bilingual entitlement/fallback, safe log, early versus late and explicit closure");
    puts("PASS RTC diagnostic real HTTP API: initial errors unchanged/no query; CLOSED terminal one query; FAILED requests reconnect; native error retained; no private data");
    printf("PASS session disconnect: %u checks; CLOSED/FAILED/DISCONNECTED5s/11s; bounded reasons\n", checks);
    return 0;
}
