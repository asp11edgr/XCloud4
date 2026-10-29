/* SPDX-License-Identifier: GPL-3.0-only */
/* The Xbox constructors AND the production HTTP implementation run here.
 * Only the native PS4 module/HTTP ABI is simulated. No request leaves the
 * process; fixture credentials are not captured or printed. */
#define _POSIX_C_SOURCE 200809L
#include <strings.h>
/* Some historical callers include this fixture without the runner's host
 * switch. Only host wait types are needed; no native wait API is exercised. */
#ifndef X4_STREAM_WAIT_HOST
#define X4_STREAM_WAIT_HOST 1
#endif
#include "../src/auth/xbox_live.c"
#include "../src/auth/ca_bundle.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "osName HTTP check failed at %d: %s\n", __LINE__, #x); abort(); } } while (0)

#if X4_STREAM_PROFILE == 0
static const char expected_os[] = "Orbis";
#define EXPECT_WIDTH 960
#define EXPECT_HEIGHT 540
#define EXPECT_FPS 30
#define EXPECT_BITRATE 5000
#define EXPECT_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000"
#elif X4_STREAM_PROFILE == 1
static const char expected_os[] = "android";
#define EXPECT_WIDTH 1280
#define EXPECT_HEIGHT 720
#define EXPECT_FPS 60
#define EXPECT_BITRATE 10000
#define EXPECT_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000"
#elif X4_STREAM_PROFILE == 2
static const char expected_os[] = "windows";
#define EXPECT_WIDTH 1920
#define EXPECT_HEIGHT 1080
#define EXPECT_FPS 60
#define EXPECT_BITRATE 12000
#define EXPECT_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240"
#else
#error Test requires a supported profile
#endif

static const char origin[] = "https://fixture-region.gssv-play-prod.xboxlive.com";
static const char session_prefix[] = "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/";
static char expected_device[640], expected_native_device[640], expected_play[1024];
#if X4_STREAM_PROFILE
static unsigned runtime_profile;
static const char *runtime_os;
#endif
enum TestRoute { TEST_USER, TEST_XSTS, TEST_LOGIN, TEST_TITLES, TEST_MRU,
    TEST_PLAY, TEST_STATE, TEST_CONNECT, TEST_SDP_GET, TEST_SDP_POST,
    TEST_ICE_GET, TEST_ICE_POST, TEST_KEEPALIVE, TEST_DELETE, TEST_ROUTE_COUNT };
static unsigned route_calls[TEST_ROUTE_COUNT], public_device_headers, templates, terminated;
enum ResourceKind { TEST_NONE, TEST_TEMPLATE, TEST_CONNECTION, TEST_REQUEST };
static struct TestResource {
    enum ResourceKind kind;
    int parent, method;
    enum TestRoute route;
    const char *borrowed_url, *response;
    uint64_t payload_size;
    size_t offset;
    unsigned tls, device_headers, client_headers, authorization_headers, contract_headers;
    int redirect, cookies;
    /* Public device description only. Never retain Authorization or tokens. */
    char device[640];
} resource[128];
static unsigned next_resource;
static uint64_t fake_clock = 1000000;
static _Atomic int cancel_flag;

uint64_t sceKernelGetProcessTime(void) { return fake_clock; }
int sceKernelUsleep(unsigned us) { fake_clock += us; return 0; }

static bool test_suffix(const char *text, const char *suffix)
{
    size_t n = strlen(text), s = strlen(suffix);
    return n >= s && !strcmp(text + n - s, suffix);
}
static enum TestRoute test_route(const char *url, int method)
{
    if (!strcmp(url, "https://user.auth.xboxlive.com/user/authenticate")) { CHECK(method == 1); return TEST_USER; }
    if (!strcmp(url, "https://xsts.auth.xboxlive.com/xsts/authorize")) { CHECK(method == 1); return TEST_XSTS; }
    if (!strcmp(url, "https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user")) { CHECK(method == 1); return TEST_LOGIN; }
    CHECK(!strncmp(url, origin, sizeof(origin) - 1));
    if (test_suffix(url, "/v2/titles")) { CHECK(method == 0); return TEST_TITLES; }
    if (test_suffix(url, "/v2/titles/mru?mr=25")) { CHECK(method == 0); return TEST_MRU; }
    CHECK(!strncmp(url, session_prefix, sizeof(session_prefix) - 1));
    const char *suffix = url + sizeof(session_prefix) - 1;
    if (!strcmp(suffix, "play")) { CHECK(method == 1); return TEST_PLAY; }
    CHECK(!strncmp(suffix, "HOST_SESSION", 12)); suffix += 12;
    if (!strcmp(suffix, "/state")) { CHECK(method == 0); return TEST_STATE; }
    if (!strcmp(suffix, "/connect")) { CHECK(method == 1); return TEST_CONNECT; }
    if (!strcmp(suffix, "/sdp")) { CHECK(method == 0 || method == 1); return method ? TEST_SDP_POST : TEST_SDP_GET; }
    if (!strcmp(suffix, "/ice")) { CHECK(method == 0 || method == 1); return method ? TEST_ICE_POST : TEST_ICE_GET; }
    if (!strcmp(suffix, "/keepalive")) { CHECK(method == 1); return TEST_KEEPALIVE; }
    CHECK(!suffix[0] && method == 5); return TEST_DELETE;
}
static int32_t test_new_resource(enum ResourceKind kind, int parent)
{
    CHECK(++next_resource < sizeof(resource) / sizeof(resource[0]));
    resource[next_resource] = (struct TestResource){ .kind = kind, .parent = parent, .redirect = -1, .cookies = -1 };
    return (int32_t)next_resource;
}
static int32_t native_zero(void) { return 0; }
static int32_t native_pool(const char *name, int32_t bytes, int32_t flags)
{ CHECK(!strcmp(name, "x4-http-net") && bytes == 256 * 1024 && !flags); return 51; }
static int32_t native_pool_destroy(int32_t id) { CHECK(id == 51); return 0; }
static int32_t native_ssl(size_t bytes) { CHECK(bytes == 512 * 1024); return 52; }
static int32_t native_ssl_term(int32_t id) { CHECK(id == 52); return 0; }
/* Baseline HTTP now loads the real bundled CA before requests. Simulate only
 * that native ABI; the certificates and descriptor layout stay production. */
static int32_t native_load_cert(int32_t http,int32_t count,void *list,void *cert,void *key)
{
    size_t actual_count=0;
    const SslPem * const *actual=x4_ca_bundle_get_ssl_pem_ptrs(&actual_count);
    CHECK(actual_count==X4_CA_BUNDLE_COUNT&&actual_count==11&&x4_ca_bundle_rsa_count()==7);
    CHECK(http==53&&count==(int32_t)actual_count&&list==(void *)actual&&!cert&&!key);
    for(size_t i=0;i<actual_count;++i){
        CHECK(actual[i]&&actual[i]->ptr&&actual[i]->size>100);
        const X4CaCertEntry *entry=x4_ca_bundle_entry(i);
        CHECK(entry&&actual[i]->ptr==entry->pem_data&&actual[i]->size==entry->pem_size);
        CHECK(!strncmp(actual[i]->ptr,"-----BEGIN CERTIFICATE-----",27));
    }
    return 0;
}

static int32_t native_http(int32_t net, int32_t ssl, size_t bytes)
{ CHECK(net == 51 && ssl == 52 && bytes == 512 * 1024); return 53; }
static int32_t native_http_term(int32_t id)
{
    CHECK(id == 53);
    for (unsigned i = 1; i <= next_resource; ++i) CHECK(resource[i].kind == TEST_NONE);
    ++terminated; return 0;
}
static int32_t native_template(int32_t ctx, const char *agent, int32_t version, int32_t proxy)
{
    CHECK(ctx == 53 && !strcmp(agent, "XCloud4") && version == 2 && !proxy);
    ++templates; return test_new_resource(TEST_TEMPLATE, 0);
}
static int32_t native_timeout(int32_t id, uint32_t us)
{ CHECK(resource[id].kind == TEST_TEMPLATE && us == 5000000); return 0; }
static int32_t native_redirect(int32_t id, int32_t enabled)
{ CHECK(resource[id].kind == TEST_TEMPLATE && !enabled); resource[id].redirect = enabled; return 0; }
static int32_t native_cookie(int32_t id, int32_t enabled)
{ CHECK(resource[id].kind == TEST_TEMPLATE && !enabled); resource[id].cookies = enabled; return 0; }
static int32_t native_tls(int32_t id, uint32_t flags)
{ CHECK(resource[id].kind == TEST_TEMPLATE && flags == 0xBD); resource[id].tls = flags; return 0; }
static int32_t native_connection(int32_t tmpl, const char *url, bool keepalive)
{
    CHECK(resource[tmpl].kind == TEST_TEMPLATE && resource[tmpl].tls == 0xBD &&
        !resource[tmpl].redirect && !resource[tmpl].cookies && !keepalive);
    int32_t id = test_new_resource(TEST_CONNECTION, tmpl);
    resource[id].borrowed_url = url; return id;
}
static int32_t native_request(int32_t conn, int32_t method, const char *url, uint64_t bytes)
{
    CHECK(resource[conn].kind == TEST_CONNECTION && resource[conn].borrowed_url[0]);
    int32_t id = test_new_resource(TEST_REQUEST, conn);
    struct TestResource *r = &resource[id];
    r->method = method; r->borrowed_url = url; r->payload_size = bytes; r->route = test_route(url, method);
    return id;
}
static int32_t native_header(int32_t id, const char *name, const char *value, uint32_t mode)
{
    struct TestResource *r = &resource[id];
    CHECK(r->kind == TEST_REQUEST && !mode);
    if (!strcmp(name, "X-MS-Device-Info")) {
        CHECK(strlen(value) < sizeof(r->device));
        memcpy(r->device, value, strlen(value) + 1); ++r->device_headers;
    } else if (!strcmp(name, "x-gssv-client")) {
        CHECK(!strcmp(value, "XboxComBrowser")); ++r->client_headers;
    } else if (!strcmp(name, "Authorization")) {
        /* Check presence/type without copying, logging or exposing the value. */
        CHECK(!strncmp(value, "Bearer ", 7) && value[7]); ++r->authorization_headers;
    } else if (!strcmp(name, "x-xbl-contract-version")) {
        CHECK(!strcmp(value, "1")); ++r->contract_headers;
    } else if (!strcmp(name, "Accept") || !strcmp(name, "Content-Type")) CHECK(!strcmp(value, "application/json"));
    else if (!strcmp(name, "Accept-Encoding")) CHECK(!strcmp(value, "identity"));
    else CHECK(0);
    return 0;
}
static int32_t native_send(int32_t id, const void *body, size_t bytes)
{
    struct TestResource *r = &resource[id];
    CHECK(r->kind == TEST_REQUEST && bytes == r->payload_size);
    CHECK(r->method == 1 || (!body && !bytes));
    if (r->route >= TEST_TITLES) {
        const char *expected=r->route==TEST_PLAY?expected_device:expected_native_device;
        CHECK(r->device_headers == 1 && !strcmp(r->device, expected));
        CHECK(r->client_headers == 1 && r->authorization_headers == 1 && !r->contract_headers);
        ++public_device_headers;
    } else {
        /* These auth exchanges did not carry a device description in 94af. */
        CHECK(!r->device_headers && !r->authorization_headers);
        CHECK(r->client_headers == (r->route == TEST_LOGIN ? 1u : 0u));
        CHECK(r->contract_headers == (r->route == TEST_LOGIN ? 0u : 1u));
    }
    if (r->route == TEST_PLAY) {
        CHECK(body && bytes == strlen(expected_play) && !memcmp(body, expected_play, bytes));
        r->response = "{\"sessionPath\":\"v5/sessions/cloud/HOST_SESSION\"}";
    } else if (r->route == TEST_USER) r->response = "{\"Token\":\"fixture-user\"}";
    else if (r->route == TEST_XSTS) r->response = "{\"Token\":\"fixture-xsts\"}";
    else if (r->route == TEST_LOGIN) r->response = "{\"gsToken\":\"fixture-streaming\",\"offeringSettings\":{\"regions\":[{\"isDefault\":true,\"baseUri\":\"https://fixture-region.gssv-play-prod.xboxlive.com\",\"name\":\"Fixture Region\"}]}}";
    else if (r->route == TEST_TITLES || r->route == TEST_MRU) r->response = "{\"results\":[]}";
    else if (r->route == TEST_STATE) r->response = "{\"state\":\"ReadyToConnect\"}";
    else r->response = "{}";
    if (r->route == TEST_CONNECT) CHECK(body && bytes && strstr(body, "\"userToken\":") != NULL);
    if (r->route == TEST_SDP_POST) CHECK(body && bytes && strstr(body, "\"messageType\":\"offer\"") != NULL);
    if (r->route == TEST_ICE_POST) CHECK(body && bytes && strstr(body, "\"fixture\":true") != NULL);
    if (r->route == TEST_KEEPALIVE) CHECK(!bytes);
    ++route_calls[r->route]; return 0;
}
static int32_t native_status(int32_t id, int32_t *status)
{ CHECK(resource[id].kind == TEST_REQUEST); *status = 200; return 0; }
static int32_t native_read(int32_t id, void *dst, size_t capacity)
{
    struct TestResource *r = &resource[id];
    CHECK(r->kind == TEST_REQUEST && r->response && r->borrowed_url[0]);
    size_t remaining = strlen(r->response) - r->offset;
    size_t take = remaining < capacity ? remaining : capacity;
    memcpy(dst, r->response + r->offset, take); r->offset += take; return (int32_t)take;
}
static int32_t native_headers(int32_t id, char **headers, size_t *bytes)
{ CHECK(resource[id].kind == TEST_REQUEST); *headers = "HTTP/1.1 200 OK\r\n\r\n"; *bytes = strlen(*headers) + 1; return 0; }
static int32_t native_delete(int32_t id, enum ResourceKind kind)
{
    CHECK(resource[id].kind == kind);
    for (unsigned i = 1; i <= next_resource; ++i) CHECK(resource[i].kind == TEST_NONE || resource[i].parent != id);
    if (kind != TEST_TEMPLATE) CHECK(resource[id].borrowed_url && resource[id].borrowed_url[0]);
    resource[id] = (struct TestResource){0}; return 0;
}
static int32_t native_delete_template(int32_t id) { return native_delete(id, TEST_TEMPLATE); }
static int32_t native_delete_connection(int32_t id) { return native_delete(id, TEST_CONNECTION); }
static int32_t native_delete_request(int32_t id) { return native_delete(id, TEST_REQUEST); }

int x4_module_open(const char *name)
{ CHECK(!strcmp(name, "libSceNet") || !strcmp(name, "libSceSsl") || !strcmp(name, "libSceHttp")); return 1; }
int x4_module_symbol(int handle, const char *name, void **address)
{
    CHECK(handle == 1);
#define SYMBOL(symbol, function) if (!strcmp(name, symbol)) { *address = (void *)(function); return 0; }
    SYMBOL("sceNetInit", native_zero)
    SYMBOL("sceNetPoolCreate", native_pool)
    SYMBOL("sceNetPoolDestroy", native_pool_destroy)
    SYMBOL("sceSslInit", native_ssl)
    SYMBOL("sceSslTerm", native_ssl_term)
    SYMBOL("sceHttpInit", native_http)
    SYMBOL("sceHttpTerm", native_http_term)
    SYMBOL("sceHttpCreateTemplate", native_template)
    SYMBOL("sceHttpCreateConnectionWithURL", native_connection)
    SYMBOL("sceHttpCreateRequestWithURL", native_request)
    SYMBOL("sceHttpDeleteTemplate", native_delete_template)
    SYMBOL("sceHttpDeleteConnection", native_delete_connection)
    SYMBOL("sceHttpDeleteRequest", native_delete_request)
    SYMBOL("sceHttpSetResolveTimeOut", native_timeout)
    SYMBOL("sceHttpSetConnectTimeOut", native_timeout)
    SYMBOL("sceHttpSetSendTimeOut", native_timeout)
    SYMBOL("sceHttpSetRecvTimeOut", native_timeout)
    SYMBOL("sceHttpSetAutoRedirect", native_redirect)
    SYMBOL("sceHttpSetCookieEnabled", native_cookie)
    SYMBOL("sceHttpAddRequestHeader", native_header)
    SYMBOL("sceHttpsEnableOption", native_tls)
    SYMBOL("sceHttpsLoadCert", native_load_cert)
    SYMBOL("sceHttpSendRequest", native_send)
    SYMBOL("sceHttpReadData", native_read)
    SYMBOL("sceHttpGetStatusCode", native_status)
    SYMBOL("sceHttpGetAllResponseHeaders", native_headers)
#undef SYMBOL
    CHECK(0); return -1;
}

static int test_passport(void *context, X4Http *http, char *out, size_t capacity,
    int *status, const char **stage, const _Atomic int *cancel)
{
    CHECK(!context && http && cancel == &cancel_flag && capacity > 32);
    memcpy(out, "fixture-not-a-real-passport", sizeof("fixture-not-a-real-passport"));
    *status = 200; *stage = "host fixture"; return 0;
}
static void expected_contract(void)
{
#if X4_STREAM_PROFILE
    static const char *names[]={"android","windows","windows","windows","windows","windows"};
    runtime_os=names[runtime_profile];
    const char *os=runtime_os;
    const char *native_os="android";
#else
    const char *os=expected_os,*native_os=expected_os;
    CHECK(!strcmp(X4_STREAM_OS, expected_os));
    CHECK(X4_STREAM_WIDTH == EXPECT_WIDTH && X4_STREAM_HEIGHT == EXPECT_HEIGHT && X4_STREAM_FPS == EXPECT_FPS &&
        X4_STREAM_BITRATE_KBPS == EXPECT_BITRATE && !strcmp(X4_STREAM_H264_FMTP, EXPECT_FMTP));
#endif
    int n = snprintf(expected_native_device, sizeof(expected_native_device),
        "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\","
        "\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\","
        "\"model\":\"PS4\"},\"os\":{\"name\":\"%s\",\"platform\":\"console\"}}}",native_os);
    CHECK(n > 0 && (size_t)n < sizeof(expected_native_device));
    copy_text(expected_device,sizeof(expected_device),expected_native_device);
#if X4_STREAM_PROFILE
    if(runtime_profile==2) n=snprintf(expected_device,sizeof(expected_device),
        "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\",\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\",\"model\":\"PS4\"},\"os\":{\"name\":\"windows\",\"platform\":\"console\"}}}");
    else if(runtime_profile) n=snprintf(expected_device,sizeof(expected_device),
        "{\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\","
        "\"clientAppVersion\":\"26.1.97\",\"clientSdkVersion\":\"10.3.7\",\"httpEnvironment\":\"prod\","
        "\"sdkInstallId\":\"\"}},\"dev\":{\"os\":{\"name\":\"%s\",\"ver\":\"22631.2715\",\"platform\":\"desktop\"},"
        "\"hw\":{\"make\":\"Microsoft\",\"model\":\"unknown\",\"sdktype\":\"web\"},"
        "\"browser\":{\"browserName\":\"chrome\",\"browserVersion\":\"140.0.3485.54\"},\"displayInfo\":{"
        "\"dimensions\":{\"widthInPixels\":%u,\"heightInPixels\":%u},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}}}}",
        os,4096u,2160u);
#endif
    CHECK(n > 0 && (size_t)n < sizeof(expected_device));
    n = snprintf(expected_play, sizeof(expected_play),
        "{\"clientSessionId\":\"\",\"titleId\":\"HOST_GAME\",\"systemUpdateGroup\":\"\",\"settings\":{"
        "\"nanoVersion\":\"V3;WebrtcTransport.dll\",\"enableOptionalDataCollection\":false,"
        "\"enableTextToSpeech\":false,\"highContrast\":0,\"locale\":\"es-MX\",\"useIceConnection\":false,"
        "\"timezoneOffsetMinutes\":-360,\"sdkType\":\"web\",\"osName\":\"%s\"},"
        "\"serverId\":\"\",\"fallbackRegionNames\":[]}", os);
    CHECK(n > 0 && (size_t)n < sizeof(expected_play));
}
static void run_real_constructors(void)
{
    X4XboxWork *w = x4_xbox_work_new(); CHECK(w);
#if X4_STREAM_PROFILE
    /* A menu update after construction cannot change this worker. The same
     * snapshot setter used by device_auth also rejects an in-flight worker. */
    X4StreamRequestConfig c=x4_stream_request_config((X4StreamRequestProfile)runtime_profile,X4_COLOR_AUTO_709);
    CHECK(x4_xbox_work_set_request_config(w,&c));
    CHECK(x4_stream_settings_set((X4StreamRequestProfile)((runtime_profile+1)%X4_REQUEST_PROFILE_COUNT),X4_COLOR_FORCE_601));
#endif
    const X4CatalogSnapshot *catalog = x4_xbox_catalog(w, "fixture-not-a-real-access-token", &cancel_flag, NULL, NULL);
    CHECK(catalog->state == X4_CATALOG_READY && !catalog->count && w->recent.state == X4_CATALOG_READY);
    CHECK(!w->http && !w->gs_token[0] && !w->authorization[0]);
    /* Session secrets are fixture-only. The production session constructors,
     * URL allowlist, owned headers/body, and native send are not substituted. */
    int error = -1; w->http = x4_http_open(&error); CHECK(w->http && !error);
#if X4_STREAM_PROFILE
    CHECK(!x4_xbox_work_set_request_config(w,&c));
#endif
    copy_text(w->origin, sizeof(w->origin), origin);
    copy_text(w->authorization, sizeof(w->authorization), "Bearer fixture-not-a-real-gs-token");
    w->session_start = session_now(); w->passport_provider = test_passport;
#if X4_STREAM_PROFILE
    /* The web header is forbidden on every wrong route, host or verb even
     * before the production destination allowlist gets a chance to run. */
    const char *bad_urls[]={"https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/HOST_SESSION/state",
        "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/play?extra=1",
        "https://untrusted.example/v5/sessions/cloud/play",
        "https://fixture-region.gssv-play-prod.xboxlive.com/v2/titles"};
    unsigned before=next_resource;
    for(unsigned i=0;i<sizeof(bad_urls)/sizeof(bad_urls[0]);++i) {
        copy_text(w->url,sizeof(w->url),bad_urls[i]);copy_text(w->request,sizeof(w->request),"{}");
        size_t denied_length=123;int denied_status=123;
        CHECK(session_call_impl(w,X4_SESSION_HTTP_POST,w->request,NULL,&denied_length,&denied_status,true)==X4_HTTP_URL);
        CHECK(next_resource==before&&!w->request[0]&&!denied_length&&!denied_status);
    }
    CHECK(session_url(w,"play",""));
    size_t denied_length=123;int denied_status=123;
    CHECK(session_call_impl(w,X4_SESSION_HTTP_GET,NULL,NULL,&denied_length,&denied_status,true)==X4_HTTP_URL);
    CHECK(next_resource==before&&!denied_length&&!denied_status);
#endif
    X4CatalogTitle title = {0}; copy_text(title.id, sizeof(title.id), "HOST_GAME");
    CHECK(session_create(w, &title) == 1 && !strcmp(w->session_id, "HOST_SESSION"));
    SessionEnd end = {0}; uint64_t deadline = session_now() + 90000000;
    CHECK(session_provision(w, deadline, &end));
    CHECK(session_authorize(w, &end));
    size_t length = 0; int status = 0;
    copy_text(w->sdp, sizeof(w->sdp), "v=0\r\n"); CHECK(signal_offer(w));
    CHECK(signal_call(w, "/sdp", X4_SESSION_HTTP_POST, w->request, deadline, &end, &length, &status));
    CHECK(signal_call(w, "/sdp", X4_SESSION_HTTP_GET, NULL, deadline, &end, &length, &status));
    copy_text(w->request, sizeof(w->request), "{\"fixture\":true}");
    CHECK(signal_call(w, "/ice", X4_SESSION_HTTP_POST, w->request, deadline, &end, &length, &status));
    CHECK(signal_call(w, "/ice", X4_SESSION_HTTP_GET, NULL, deadline, &end, &length, &status));
    CHECK(signal_keepalive(w, deadline, &end));
    end_with(&end, X4_SESSION_CLOSED, 0, 0, "host fixture complete");
    session_cleanup(w, &end); CHECK(!w->session.cleanup_failed && !w->session_id[0]);
    x4_http_close(w->http); w->http = NULL; x4_xbox_work_free(w);
}
int main(void)
{
    const char *settings_path=getenv("X4_SESSION_FIXTURE_SETTINGS_PATH");
    x4_server_settings_set_path(settings_path?settings_path:"/nonexistent-tarea20c-host/server-settings.bin");
    CHECK(!strcmp(X4_STREAM_OS,expected_os));
#if X4_STREAM_PROFILE
    CHECK(x4_stream_settings_get().profile==X4_REQUEST_1080P_G);
    for(runtime_profile=0;runtime_profile<X4_REQUEST_PROFILE_COUNT;++runtime_profile) {
        memset(route_calls,0,sizeof(route_calls));memset(resource,0,sizeof(resource));
        public_device_headers=templates=terminated=next_resource=0;
        CHECK(x4_stream_settings_set((X4StreamRequestProfile)runtime_profile,X4_COLOR_AUTO_709));
#endif
    expected_contract(); run_real_constructors();
    for (unsigned i = 0; i < TEST_ROUTE_COUNT; ++i) CHECK(route_calls[i] == 1);
    CHECK(public_device_headers == TEST_ROUTE_COUNT - TEST_TITLES && templates == TEST_ROUTE_COUNT && terminated == 2);
    printf("production play-only device + HTTP profile=%d PASS: %u checks; %d routes, %u device headers; play os=%s; remaining routes native baseline; no live network\n",
        X4_STREAM_PROFILE, checks, TEST_ROUTE_COUNT, public_device_headers,
#if X4_STREAM_PROFILE
        runtime_os);
    }
#else
        expected_os);
#endif
    return 0;
}
