/* SPDX-License-Identifier: GPL-3.0-only */
/* The harness compares captured bytes in RAM and prints only their SHA. */
#include "tls_transport_fake.h"
#include "../src/ui/lang.h"

uint64_t sceKernelGetProcessTime(void) { return 1; }
int sceKernelUsleep(unsigned us) { (void)us; return 0; }
const char *x4_lang_store_suffix(void) { return "&market=MX&languages=es-MX&fieldsTemplate=Details"; }
const char *x4_tr(enum X4TextId id) { (void)id; return ""; }
const char *x4_lang_market(void) { return "MX"; }
const char *x4_lang_locale(void) { return "es-MX"; }

int __real_x4_tls_conn_write(X4TlsConn *, const void *, size_t, const X4TlsBudget *);
int __wrap_x4_tls_conn_write(X4TlsConn *conn, const void *bytes, size_t size, const X4TlsBudget *budget)
{
    /* A length delimiter distinguishes writes, including headers from body. */
    assert(printf("%zu\n", size) > 0);
    assert(fwrite(bytes, 1, size, stdout) == size);
    return __real_x4_tls_conn_write(conn, bytes, size, budget);
}

#define REGION "https://fixture-region.gssv-play-prod.xboxlive.com"
#define GAME REGION "/v5/sessions/cloud/FIXTURE"
#define STORE "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=FIXTURE&market=MX&languages=es-MX&fieldsTemplate=Details"
#define COVER "https://store-images.s-microsoft.com/image/apps.fixture?w=356&h=200&q=85&format=jpg"

int main(void)
{
    x4tf_reset();
    char body[128];
    size_t bytes;
    int status, error = 0;
    X4Http *http = x4_http_open(&error);
    assert(http && !error);
    const X4HttpHeader contract[] = {{"x-xbl-contract-version", "1"}};
    const X4HttpHeader login[] = {{"x-gssv-client", "XboxComBrowser"}, {"X-MS-Device-Info", "{}"}};
    const X4HttpHeader auth[] = {
        {"Authorization", "Bearer TEST-DATA-NOT-VALID"},
        {"x-gssv-client", "XboxComBrowser"},
        {"X-MS-Device-Info", "{}"}
    };
    const X4HttpHeader time[] = {
        {"Authorization", "XBL3.0 x=12345;TEST-DATA-NOT-VALID"}, {"MS-CV", "abcdefghijklmnop.0"}
    };
    assert(x4_http_request(http, "https://login.microsoftonline.com/consumers/oauth2/v2.0/token", "sample=1",
        body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_passport_request(http, "sample=1", body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_json_request(http, "https://user.auth.xboxlive.com/user/authenticate",
        contract, 1, "{}", body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_json_request(http, "https://xsts.auth.xboxlive.com/xsts/authorize",
        contract, 1, "{}", body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_json_request(http, "https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user",
        login, 2, "{}", body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_json_request(http, "https://xgpuwebf2p.gssv-play-prod.xboxlive.com/v2/login/user",
        login, 2, "{}", body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_json_request(http, REGION "/v2/titles",
        auth, 3, NULL, body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_json_request(http, REGION "/v2/titles/mru?mr=25",
        auth, 3, NULL, body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_cloud_time_request(http, X4_HTTP_CLOUD_TIME_PREFIX "MX" X4_HTTP_CLOUD_TIME_SUFFIX,
        time, 2, body, sizeof(body), &bytes, &status, NULL) == 0);
    const struct { enum X4HttpSessionMethod method; const char *url, *payload; } calls[] = {
        {X4_SESSION_HTTP_POST, REGION "/v5/sessions/cloud/play", "{}"},
        {X4_SESSION_HTTP_GET, GAME "/state", NULL},
        {X4_SESSION_HTTP_GET, REGION "/v1/waittime/FIXTURE", NULL},
        {X4_SESSION_HTTP_POST, GAME "/keepalive", ""},
        {X4_SESSION_HTTP_POST, GAME "/connect", "{}"},
        {X4_SESSION_HTTP_GET, GAME "/sdp", NULL},
        {X4_SESSION_HTTP_POST, GAME "/sdp", "{}"},
        {X4_SESSION_HTTP_GET, GAME "/ice", NULL},
        {X4_SESSION_HTTP_POST, GAME "/ice", "{}"},
        {X4_SESSION_HTTP_DELETE, GAME, NULL}
    };
    for (unsigned i = 0; i < sizeof(calls) / sizeof(*calls); ++i)
        assert(x4_http_session_request(http, calls[i].method, calls[i].url, auth, 3, calls[i].payload,
            body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_session_state_request(http, GAME "/state", auth, 3,
        body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_session_reconnect_request(http, X4_SESSION_HTTP_GET, GAME "/state", auth, 3,
        NULL, body, sizeof(body), &bytes, &status, NULL, 9999999) == 0);
    assert(x4_http_auth_reconnect_request(http, true, "sample=1",
        body, sizeof(body), &bytes, &status, NULL, 9999999) == 0);
    assert(x4_http_json_request(http, STORE, NULL, 0, NULL, body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_public_metadata_request(http, STORE, body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_public_metadata_request(http,
        "https://catalog.gamepass.com/sigls/v2?id=d8f4afcd-882a-49e3-86b3-f61fa0172b75&market=MX&language=es-MX",
        body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_public_image_request(http, COVER, (unsigned char *)body,
        sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4_http_releases_request(http,
#if X4_PUBLIC
        X4_HTTP_RELEASES_URL_PUBLIC,
#else
        X4_HTTP_RELEASES_URL_LAB,
#endif
        body, sizeof(body), &bytes, &status, NULL) == 0);
    assert(x4tf.requests == 27);
    x4_http_close(http);
    assert(!x4tf.live_connections && !x4tf.live_contexts);
    return 0;
}
