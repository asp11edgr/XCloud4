/* SPDX-License-Identifier: GPL-3.0-only */
/* Production Xbox constructors + HTTP policy/wire, with a deterministic
 * TLS link double. No native HTTP calls, network, files or credential logs. */
#define _POSIX_C_SOURCE 200809L
#include <strings.h>
#ifndef X4_STREAM_WAIT_HOST
#define X4_STREAM_WAIT_HOST 1
#endif
#include "../src/auth/xbox_live.c"
#include "../src/auth/ca_bundle.h"
#include "tls_transport_fake.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"osName HTTP check failed at %d\n",__LINE__); abort(); } } while(0)
#if X4_STREAM_PROFILE == 0
static const char expected_os[]="Orbis";
#define EXPECT_WIDTH 960
#define EXPECT_HEIGHT 540
#define EXPECT_FPS 30
#define EXPECT_BITRATE 5000
#define EXPECT_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000"
#elif X4_STREAM_PROFILE == 1
static const char expected_os[]="android";
#define EXPECT_WIDTH 1280
#define EXPECT_HEIGHT 720
#define EXPECT_FPS 60
#define EXPECT_BITRATE 10000
#define EXPECT_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000"
#elif X4_STREAM_PROFILE == 2
static const char expected_os[]="windows";
#define EXPECT_WIDTH 1920
#define EXPECT_HEIGHT 1080
#define EXPECT_FPS 60
#define EXPECT_BITRATE 12000
#define EXPECT_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240"
#else
#error Supported profile required
#endif
static const char origin[]="https://fixture-region.gssv-play-prod.xboxlive.com";
static const char session_prefix[]="https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/";
static char expected_device[640],expected_native_device[640],expected_play[1024];
#if X4_STREAM_PROFILE
static unsigned runtime_profile;
static const char *runtime_os;
#endif
static unsigned route_calls[14],public_device_headers,templates,terminated,next_resource;
enum TestRoute { TEST_USER,TEST_XSTS,TEST_LOGIN,TEST_TITLES,TEST_MRU,TEST_PLAY,TEST_STATE,
 TEST_CONNECT,TEST_SDP_GET,TEST_SDP_POST,TEST_ICE_GET,TEST_ICE_POST,TEST_KEEPALIVE,TEST_DELETE,TEST_ROUTE_COUNT };
enum ResourceKind { TEST_NONE,TEST_REQUEST };
static struct TestResource { enum ResourceKind kind;enum TestRoute route; } resource[128];
static uint64_t fake_clock=1000000;
static _Atomic int cancel_flag;
uint64_t sceKernelGetProcessTime(void){return fake_clock;}
int sceKernelUsleep(unsigned us){fake_clock+=us;return 0;}
static bool test_suffix(const char *text,const char *suffix)
{size_t n=strlen(text),s=strlen(suffix);return n>=s&&!strcmp(text+n-s,suffix);}
static enum TestRoute test_route(const char *url,int method)
{
 if(!strcmp(url,"https://user.auth.xboxlive.com/user/authenticate")){CHECK(method==1);return TEST_USER;}
 if(!strcmp(url,"https://xsts.auth.xboxlive.com/xsts/authorize")){CHECK(method==1);return TEST_XSTS;}
 if(!strcmp(url,"https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user")||
    !strcmp(url,"https://xgpuwebf2p.gssv-play-prod.xboxlive.com/v2/login/user")){CHECK(method==1);return TEST_LOGIN;}
 CHECK(!strncmp(url,origin,sizeof(origin)-1));
 if(test_suffix(url,"/v2/titles")){CHECK(method==0);return TEST_TITLES;}
 if(test_suffix(url,"/v2/titles/mru?mr=25")){CHECK(method==0);return TEST_MRU;}
 CHECK(!strncmp(url,session_prefix,sizeof(session_prefix)-1));
 const char *suffix=url+sizeof(session_prefix)-1;
 if(!strcmp(suffix,"play")){CHECK(method==1);return TEST_PLAY;}
 CHECK(!strncmp(suffix,"HOST_SESSION",12));suffix+=12;
 if(!strcmp(suffix,"/state")){CHECK(method==0);return TEST_STATE;}
 if(!strcmp(suffix,"/connect")){CHECK(method==1);return TEST_CONNECT;}
 if(!strcmp(suffix,"/sdp")){CHECK(method==0||method==1);return method?TEST_SDP_POST:TEST_SDP_GET;}
 if(!strcmp(suffix,"/ice")){CHECK(method==0||method==1);return method?TEST_ICE_POST:TEST_ICE_GET;}
 if(!strcmp(suffix,"/keepalive")){CHECK(method==1);return TEST_KEEPALIVE;}
 CHECK(!suffix[0]&&method==5);return TEST_DELETE;
}
static void transport_request(const X4TfRequest *r,X4TfResponse *response)
{
 CHECK(++next_resource<128);++templates;
 enum TestRoute route=test_route(r->url,r->method);
 resource[next_resource]=(struct TestResource){TEST_REQUEST,route};
 CHECK(!r->keepalive); /* Auth/session never shares a public connection. */
 CHECK(x4tf_header_count(r,"Host")==1&&!x4tf_header(r,"Cookie"));
 CHECK(x4tf_header(r,"Accept")&&!strcmp(x4tf_header(r,"Accept"),"application/json"));
 if(r->method==1)CHECK(x4tf_header(r,"Content-Type")&&!strcmp(x4tf_header(r,"Content-Type"),"application/json"));
 const char *device=x4tf_header(r,"X-MS-Device-Info"),*client=x4tf_header(r,"x-gssv-client");
 const char *authorization=x4tf_header(r,"Authorization"),*contract=x4tf_header(r,"x-xbl-contract-version");
 if(route>=TEST_TITLES){
  const char *expected=route==TEST_PLAY?expected_device:expected_native_device;
  CHECK(x4tf_header_count(r,"X-MS-Device-Info")==1&&device&&!strcmp(device,expected));
  CHECK(x4tf_header_count(r,"x-gssv-client")==1&&client&&!strcmp(client,"XboxComBrowser"));
  CHECK(x4tf_header_count(r,"Authorization")==1&&authorization&&!strncmp(authorization,"Bearer ",7)&&authorization[7]);
  CHECK(!contract);++public_device_headers;
 }else{
  CHECK(!device&&!authorization);
  CHECK((client!=NULL)==(route==TEST_LOGIN));
  if(client)CHECK(!strcmp(client,"XboxComBrowser"));
  CHECK((contract!=NULL)==(route!=TEST_LOGIN));if(contract)CHECK(!strcmp(contract,"1"));
 }
 for(size_t i=0;i<r->header_count;++i){
  const char *name=r->headers[i].name;
  CHECK(!strcmp(name,"Host")||!strcmp(name,"Accept")||!strcmp(name,"Accept-Encoding")||
   !strcmp(name,"Content-Type")||!strcmp(name,"Content-Length")||!strcmp(name,"Connection")||!strcmp(name,"User-Agent")||
   !strcmp(name,"X-MS-Device-Info")||!strcmp(name,"x-gssv-client")||!strcmp(name,"Authorization")||!strcmp(name,"x-xbl-contract-version"));
 }
 CHECK(x4tf_header(r,"User-Agent")&&!strcmp(x4tf_header(r,"User-Agent"),"XCloud4"));
 CHECK(r->method==1||!r->body_size);
 if(route==TEST_PLAY){CHECK(r->body_size==strlen(expected_play)&&!memcmp(r->body,expected_play,r->body_size));response->body="{\"sessionPath\":\"v5/sessions/cloud/HOST_SESSION\"}";}
 else if(route==TEST_USER)response->body="{\"Token\":\"fixture-user\"}";
 else if(route==TEST_XSTS)response->body="{\"Token\":\"fixture-xsts\"}";
 else if(route==TEST_LOGIN)response->body="{\"gsToken\":\"fixture-streaming\",\"offeringSettings\":{\"regions\":[{\"isDefault\":true,\"baseUri\":\"https://fixture-region.gssv-play-prod.xboxlive.com\",\"name\":\"Fixture Region\"}]}}";
 else if(route==TEST_TITLES||route==TEST_MRU)response->body="{\"results\":[]}";
 else if(route==TEST_STATE)response->body="{\"state\":\"ReadyToConnect\"}";
 else response->body="{}";
 if(route==TEST_CONNECT)CHECK(r->body_size&&strstr(r->body,"\"userToken\":"));
 if(route==TEST_SDP_POST)CHECK(r->body_size&&strstr(r->body,"\"messageType\":\"offer\""));
 if(route==TEST_ICE_POST)CHECK(r->body_size&&strstr(r->body,"\"fixture\":true"));
 if(route==TEST_KEEPALIVE)CHECK(!r->body_size);
 ++route_calls[route];
}
static void transport_connection_closed(void)
{for(unsigned i=1;i<=next_resource;++i)resource[i].kind=TEST_NONE;}
static void transport_http_closed(void)
{for(unsigned i=1;i<=next_resource;++i)CHECK(resource[i].kind==TEST_NONE);++terminated;}

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
    x4tf_reset();x4tf.on_request=transport_request;x4tf.on_connection_close=transport_connection_closed;x4tf.on_context_close=transport_http_closed;
    CHECK(x4_ca_bundle_count()==11&&x4_ca_bundle_rsa_count()==7);
    for(unsigned root=0;root<11;++root){const X4CaCertEntry *entry=x4_ca_bundle_entry(root);CHECK(entry&&entry->der_data&&entry->der_size>100&&entry->der_data[0]==0x30);}
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
