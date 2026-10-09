/* SPDX-License-Identifier: GPL-3.0-only */
#include "xbox_live.h"
#include "device_auth.h"
#include "http_client.h"
#include "json.h"
#include "../streaming/rtc_transport.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

/* Original XCloud4 implementation of the public Xbox Live exchanges. The
 * protocol steps were studied in GreenVita (see docs/CATALOGO_XBOX.md);
 * no Rust implementation or private credential was taken from it. */
#define X4_XBOX_USER_URL "https://user.auth.xboxlive.com/user/authenticate"
#define X4_XBOX_XSTS_URL "https://xsts.auth.xboxlive.com/xsts/authorize"
#define X4_XBOX_STORE_PREFIX "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds="
#define X4_XBOX_STORE_SUFFIX "&market=MX&languages=es-MX&fieldsTemplate=Details"
#define X4_XBOX_GSSV_SUFFIX ".gssv-play-prod.xboxlive.com"
#define X4_XBOX_TOKEN_SIZE 16384
#define X4_XBOX_URL_SIZE 512
#define X4_XBOX_STORE_NAMED 32u
#define X4_XBOX_STORE_BATCH 8u
/* Per-entry bound on programs x subscriptions comparisons. */
#define X4_XBOX_PROGRAMS_MAX 16u
#define X4_XBOX_CANCELLED 1
/* XCloud4's own short description; no browser identity is claimed. */
#define X4_XBOX_DEVICE_INFO "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\"," \
    "\"clientAppVersion\":\"0.7.10\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\"," \
    "\"model\":\"PS4\"},\"os\":{\"name\":\"Orbis\",\"platform\":\"console\"}}}"

/* Cloud session preparation (milestone 0.5.0) and connection authorization
 * (0.6.2) followed by real RTC signaling (0.7.0). Timings in monotonic usec. */
#define X4_SESSION_USEC 1000000ull
#define X4_SESSION_PROVISION_USEC (180ull * X4_SESSION_USEC)
#define X4_SESSION_NEGOTIATE_USEC (90ull * X4_SESSION_USEC)
#define X4_SESSION_KEEPALIVE_USEC (30ull * X4_SESSION_USEC)
#define X4_SESSION_ICE_POLL_USEC X4_SESSION_USEC
#define X4_SESSION_SDP_POLL_USEC 500000ull
#define X4_SESSION_MEDIA_GRACE_USEC (30ull * X4_SESSION_USEC)
#define X4_SESSION_SDP_MAX 32768u
#define X4_SESSION_EXCHANGE_MAX (64u * 1024)
#define X4_SESSION_CANDIDATE_MAX 1024u
#define X4_SESSION_MID_MAX 32u
#define X4_SESSION_CANDIDATES_MAX 128u
#define X4_SESSION_POLL_USEC (2ull * X4_SESSION_USEC)
#define X4_SESSION_STEP_USEC 100000u
#define X4_SESSION_ID_MAX 128u
#define X4_SESSION_PREFIX "v5/sessions/cloud/"
/* Play request: settings fields as described by GreenVita's web dialect
 * (sdkType "web", nanoVersion), with this device's own osName and the
 * owner's locale; timezoneOffsetMinutes follows JS getTimezoneOffset for
 * UTC-6 (Mexico City). No other field is guessed. */
#define X4_SESSION_PLAY_HEAD "{\"clientSessionId\":\"\",\"titleId\":\""
#define X4_SESSION_PLAY_TAIL "\",\"systemUpdateGroup\":\"\",\"settings\":{" \
    "\"nanoVersion\":\"V3;WebrtcTransport.dll\",\"enableOptionalDataCollection\":false," \
    "\"enableTextToSpeech\":false,\"highContrast\":0,\"locale\":\"es-MX\",\"useIceConnection\":false," \
    "\"timezoneOffsetMinutes\":360,\"sdkType\":\"web\",\"osName\":\"Orbis\"}," \
    "\"serverId\":\"\",\"fallbackRegionNames\":[]}"

static const struct { const char *id, *url, *stage; } offerings[] = {
    {"xgpuweb", "https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user", "credenciales de Xbox Cloud Gaming"},
    {"xgpuwebf2p", "https://xgpuwebf2p.gssv-play-prod.xboxlive.com/v2/login/user", "credenciales de juegos gratuitos"},
};

struct X4XboxWork {
    char response[X4_HTTP_RESPONSE_MAX + 1];
    char request[X4_HTTP_JSON_BODY_MAX + 1];
    char user_token[X4_XBOX_TOKEN_SIZE], xsts_token[X4_XBOX_TOKEN_SIZE], gs_token[X4_XBOX_TOKEN_SIZE];
    char authorization[X4_XBOX_TOKEN_SIZE + 8];
    char url[X4_XBOX_URL_SIZE];
    /* https://<validated default region host>, set by default_region. */
    char origin[X4_XBOX_URL_SIZE];
    /* Validated ID of the remote session; private, never logged. */
    char session_id[X4_SESSION_ID_MAX + 1];
    /* Console-transfer token from the provider; private, wiped once the
     * /connect body was built. */
    char passport[X4_XBOX_TOKEN_SIZE];
    X4Http *http;
    const _Atomic int *cancel;
    X4XboxProgress progress;
    void *context;
    X4CatalogSnapshot result;
    /* Session runs only; progress above is NULL then. */
    X4SessionProgress session_progress;
    X4XboxPassport passport_provider;
    void *passport_context;
    X4SessionSnapshot session;
    uint64_t session_start;
    X4Rtc *rtc;
    X4SessionMediaCallback media_callback;
    void *media_user;
    _Atomic int *keyframe_requested;
    uint64_t next_keepalive;
    uint64_t sdp_sent_at;
    unsigned sdp_polls;
    unsigned local_candidates, remote_candidates;
    bool provisioned;
    /* Signaling may contain ICE credentials and local IP addresses. All
     * buffers are private, bounded and cleared before freeing this heap. */
    char sdp[X4_SESSION_SDP_MAX + 1];
    char exchange[X4_SESSION_EXCHANGE_MAX + 1];
    char candidate[X4_SESSION_CANDIDATE_MAX + 1], mid[X4_SESSION_MID_MAX + 1];
    char candidate_json[4096], candidate_object[4096];
    struct { char candidate[X4_SESSION_CANDIDATE_MAX + 1], mid[X4_SESSION_MID_MAX + 1]; }
        remote_seen[X4_SESSION_CANDIDATES_MAX];
};

X4XboxWork *x4_xbox_work_new(void)
{
    X4XboxWork *w = malloc(sizeof(*w));
    if (w) memset(w, 0, sizeof(*w));
    return w;
}

void x4_xbox_work_free(X4XboxWork *w)
{
    if (!w) return;
    x4_secure_clear(w, sizeof(*w));
    free(w);
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    size_t n = 0;
    if (src) while (n + 1 < capacity && src[n]) { dst[n] = src[n]; ++n; }
    dst[n] = 0;
}

static bool alphanumeric(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Store product IDs go into a query: alphanumeric only. */
static bool product_id(const char *id)
{
    if (!id[0]) return false;
    for (const char *p = id; *p; ++p) if (!alphanumeric((unsigned char)*p)) return false;
    return true;
}

static bool title_id(const char *id)
{
    if (!id[0]) return false;
    for (const char *p = id; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!alphanumeric(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

static bool same_id(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return false;
        if (!x) return true;
    }
}

/* Same token68 rule the HTTP layer applies to the Authorization value. */
static bool token68(const char *t)
{
    size_t i = 0;
    while (alphanumeric((unsigned char)t[i]) || t[i] == '-' || t[i] == '.' || t[i] == '_' ||
        t[i] == '~' || t[i] == '+' || t[i] == '/') ++i;
    if (i == 0) return false;
    while (t[i] == '=') ++i;
    return t[i] == 0;
}

/* The Microsoft token is embedded verbatim in a JSON string. */
static bool json_safe(const char *t)
{
    if (!t || !t[0]) return false;
    for (const unsigned char *p = (const unsigned char *)t; *p; ++p)
        if (*p < 0x21 || *p > 0x7e || *p == '"' || *p == '\\') return false;
    return true;
}

static void wipe_response(X4XboxWork *w, size_t length)
{
    x4_secure_clear(w->response, length < sizeof(w->response) ? length + 1 : sizeof(w->response));
}

static bool put(X4XboxWork *w, size_t *used, const char *text)
{
    size_t n = strlen(text);
    if (n >= sizeof(w->request) - *used) return false;
    memcpy(w->request + *used, text, n + 1);
    *used += n;
    return true;
}

/* Records the final outcome; returns -1 so callers can `return finish(...)`. */
static int finish(X4XboxWork *w, enum X4CatalogState state, int error, int status, const char *stage)
{
    w->result.state = state;
    w->result.error = error;
    w->result.http_status = status;
    copy_text(w->result.stage, sizeof(w->result.stage), stage);
    return -1;
}

/* Publishes the session view with a fresh elapsed time. */
static void session_publish(X4XboxWork *w)
{
    w->session.elapsed_seconds = (unsigned)((sceKernelGetProcessTime() - w->session_start) / X4_SESSION_USEC);
    if (w->session_progress) w->session_progress(w->context, &w->session);
}

static void step(X4XboxWork *w, const char *stage)
{
    copy_text(w->result.stage, sizeof(w->result.stage), stage);
    printf("XCloud4: Xbox etapa: %s\n", stage);
    if (w->progress) w->progress(w->context, stage, w->result.offering, w->result.region);
    if (w->session_progress) {
        copy_text(w->session.stage, sizeof(w->session.stage), stage);
        copy_text(w->session.region, sizeof(w->session.region), w->result.region);
        session_publish(w);
    }
}

/* One exchange; the request body is wiped either way. 0 leaves the body in
 * w->response with any HTTP status; otherwise X4_XBOX_CANCELLED or the native
 * error, with the response wiped. */
static int call(X4XboxWork *w, const char *url, const X4HttpHeader *headers, size_t count, bool post,
    size_t *length, int *status)
{
    int rc;
    *length = 0;
    *status = 0;
    if (atomic_load(w->cancel)) rc = X4_XBOX_CANCELLED;
    else {
        rc = x4_http_json_request(w->http, url, headers, count, post ? w->request : NULL,
            w->response, sizeof(w->response), length, status, w->cancel);
        if (rc == X4_HTTP_CANCELLED || atomic_load(w->cancel)) rc = X4_XBOX_CANCELLED;
    }
    x4_secure_clear(w->request, sizeof(w->request));
    if (rc) {
        wipe_response(w, *length);
        *length = 0;
        *status = 0;
    }
    return rc;
}

static int failed(X4XboxWork *w, int rc)
{
    if (rc == X4_XBOX_CANCELLED) return finish(w, X4_CATALOG_CANCELLED, 0, 0, "consulta cancelada");
    /* Stage is a literal owned by http; copy it while http is alive. */
    char stage[80];
    snprintf(stage, sizeof(stage), "HTTPS: %s", x4_http_stage(w->http));
    return finish(w, X4_CATALOG_ERROR, rc, 0, stage);
}

/* Reads a token68 string member; any failure leaves the output wiped. */
static bool read_token(X4JsonSpan object, const char *name, char *out, size_t capacity)
{
    X4JsonSpan v;
    if (x4_json_member(object, name, &v) != 1 || x4_json_token(v, out, capacity) < 0 || !token68(out)) {
        x4_secure_clear(out, capacity);
        return false;
    }
    return true;
}

/* Microsoft access token -> Xbox user token, RPS ticket "d=<token>". */
static int user_token(X4XboxWork *w, const char *microsoft_token)
{
    size_t used = 0, length = 0;
    int status = 0;
    step(w, "token de usuario Xbox");
    if (!json_safe(microsoft_token) ||
        !put(w, &used, "{\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\","
            "\"RpsTicket\":\"d=") ||
        !put(w, &used, microsoft_token) ||
        !put(w, &used, "\"},\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}")) {
        x4_secure_clear(w->request, sizeof(w->request));
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_FORM, 0, "solicitud de token Xbox");
    }
    const X4HttpHeader headers[] = {{"x-xbl-contract-version", "1"}};
    int rc = call(w, X4_XBOX_USER_URL, headers, 1, true, &length, &status);
    if (rc) return failed(w, rc);
    if (status != 200) {
        wipe_response(w, length);
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_XBOX, status, "Xbox rechazo la cuenta Microsoft");
    }
    X4JsonSpan root;
    bool ok = x4_json_parse(w->response, length, &root) == 0 &&
        read_token(root, "Token", w->user_token, sizeof(w->user_token));
    wipe_response(w, length);
    if (!ok) return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_RESPONSE, status, "token Xbox no valido");
    return 0;
}

/* User token -> XSTS for the GSSV relying party. The user token is wiped
 * once sent, whatever the answer. */
static int xsts(X4XboxWork *w)
{
    size_t used = 0, length = 0;
    int status = 0;
    step(w, "autorizacion XSTS");
    bool built = put(w, &used, "{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"") &&
        put(w, &used, w->user_token) &&
        put(w, &used, "\"]},\"RelyingParty\":\"http://gssv.xboxlive.com/\",\"TokenType\":\"JWT\"}");
    x4_secure_clear(w->user_token, sizeof(w->user_token));
    if (!built) {
        x4_secure_clear(w->request, sizeof(w->request));
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_FORM, 0, "solicitud XSTS");
    }
    const X4HttpHeader headers[] = {{"x-xbl-contract-version", "1"}};
    int rc = call(w, X4_XBOX_XSTS_URL, headers, 1, true, &length, &status);
    if (rc) return failed(w, rc);
    X4JsonSpan root, v;
    if (status == 401) {
        /* Only the numeric XErr is read from a refusal. */
        uint32_t xerr = 0;
        if (x4_json_parse(w->response, length, &root) == 0 && x4_json_member(root, "XErr", &v) == 1 &&
            x4_json_uint32(v, &xerr) == 0)
            w->result.xerr = xerr;
        wipe_response(w, length);
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_XBOX, status, "Xbox rechazo la autorizacion XSTS");
    }
    if (status != 200) {
        wipe_response(w, length);
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_STATUS, status, "estado HTTP inesperado en XSTS");
    }
    bool ok = x4_json_parse(w->response, length, &root) == 0 &&
        read_token(root, "Token", w->xsts_token, sizeof(w->xsts_token));
    wipe_response(w, length);
    if (!ok) return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_RESPONSE, status, "token XSTS no valido");
    return 0;
}

static bool region_name(const char *name)
{
    if (!name[0]) return false;
    for (const char *p = name; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!alphanumeric(c) && c != ' ' && c != '-' && c != '_') return false;
    }
    return true;
}

/* baseUri must be https://<host> with an optional final '/'. The host is
 * lowercased and must end in the GSSV suffix; the HTTP layer checks labels
 * again before any connection. */
static bool region_url(X4XboxWork *w, const char *base, char *host, size_t capacity)
{
    static const char scheme[] = "https://";
    static const char suffix[] = X4_XBOX_GSSV_SUFFIX;
    size_t s = sizeof(scheme) - 1, tail = sizeof(suffix) - 1, n = 0;
    if (strncmp(base, scheme, s)) return false;
    const char *h = base + s;
    while (h[n] && h[n] != '/') ++n;
    if (n <= tail || n >= capacity || (h[n] == '/' && h[n + 1])) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = h[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        host[i] = c;
    }
    host[n] = 0;
    if (memcmp(host + n - tail, suffix, tail)) return false;
    int origin = snprintf(w->origin, sizeof(w->origin), "https://%s", host);
    int written = snprintf(w->url, sizeof(w->url), "https://%s/v2/titles", host);
    if (origin > 0 && (size_t)origin < sizeof(w->origin) && written > 0 && (size_t)written < sizeof(w->url))
        return true;
    x4_secure_clear(w->origin, sizeof(w->origin));
    return false;
}

/* First region flagged isDefault. Its name is shown when plain, else its host. */
static bool default_region(X4XboxWork *w, X4JsonSpan root)
{
    X4JsonSpan settings, regions, item, v;
    if (x4_json_member(root, "offeringSettings", &settings) != 1 ||
        x4_json_member(settings, "regions", &regions) != 1) return false;
    size_t cursor = 0;
    while (x4_json_item(regions, &cursor, &item) == 1) {
        int is_default = 0;
        if (x4_json_member(item, "isDefault", &v) != 1 || x4_json_bool(v, &is_default) < 0 || !is_default)
            continue;
        char base[256], host[256], name[sizeof(w->result.region)];
        if (x4_json_member(item, "baseUri", &v) != 1 || x4_json_token(v, base, sizeof(base)) < 0 ||
            !region_url(w, base, host, sizeof(host)))
            return false;
        if (x4_json_member(item, "name", &v) == 1 && x4_json_token(v, name, sizeof(name)) == 0 && region_name(name))
            copy_text(w->result.region, sizeof(w->result.region), name);
        else copy_text(w->result.region, sizeof(w->result.region), host);
        return true;
    }
    return false;
}

/* XSTS -> cloud credentials for one offering. 0 on success, 1 when the
 * service refused this offering (4xx), -1 on any other failure. */
static int login(X4XboxWork *w, size_t index)
{
    size_t used = 0, length = 0;
    int status = 0;
    w->result.state = X4_CATALOG_LOADING;
    w->result.error = 0;
    w->result.http_status = 0;
    w->result.region[0] = 0;
    copy_text(w->result.offering, sizeof(w->result.offering), offerings[index].id);
    step(w, offerings[index].stage);
    if (!put(w, &used, "{\"token\":\"") || !put(w, &used, w->xsts_token) ||
        !put(w, &used, "\",\"offeringId\":\"") || !put(w, &used, offerings[index].id) || !put(w, &used, "\"}")) {
        x4_secure_clear(w->request, sizeof(w->request));
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_FORM, 0, "solicitud de credenciales");
    }
    const X4HttpHeader headers[] = {{"x-gssv-client", "XboxComBrowser"}};
    int rc = call(w, offerings[index].url, headers, 1, true, &length, &status);
    if (rc) return failed(w, rc);
    if (status >= 400 && status < 500) {
        wipe_response(w, length);
        finish(w, X4_CATALOG_ERROR, X4_AUTH_E_XBOX, status, "Xbox Cloud Gaming rechazo la cuenta");
        return 1;
    }
    if (status != 200) {
        wipe_response(w, length);
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_STATUS, status, "estado HTTP inesperado en credenciales");
    }
    X4JsonSpan root;
    bool parsed = x4_json_parse(w->response, length, &root) == 0;
    bool token = parsed && read_token(root, "gsToken", w->gs_token, sizeof(w->gs_token));
    bool region = token && default_region(w, root);
    wipe_response(w, length);
    if (!token) return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_RESPONSE, status, "credenciales no validas");
    if (!region) {
        x4_secure_clear(w->gs_token, sizeof(w->gs_token));
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_REGION, status, "region predeterminada no valida");
    }
    printf("XCloud4: Xbox oferta %s lista\n", offerings[index].id);
    return 0;
}

/* entitled only from hasEntitlement or a program the service also lists in
 * userSubscriptions; never from presence in the list. */
static bool subscribed(X4JsonSpan details)
{
    X4JsonSpan programs, subscriptions, p, s;
    if (x4_json_member(details, "programs", &programs) != 1 ||
        x4_json_member(details, "userSubscriptions", &subscriptions) != 1) return false;
    char program[64], subscription[64];
    size_t pc = 0;
    for (unsigned i = 0; i < X4_XBOX_PROGRAMS_MAX && x4_json_item(programs, &pc, &p) == 1; ++i) {
        if (x4_json_token(p, program, sizeof(program)) < 0 || !program[0]) continue;
        size_t sc = 0;
        for (unsigned j = 0; j < X4_XBOX_PROGRAMS_MAX && x4_json_item(subscriptions, &sc, &s) == 1; ++j)
            if (x4_json_token(s, subscription, sizeof(subscription)) == 0 && !strcmp(program, subscription))
                return true;
    }
    return false;
}

/* Valid entry: an object with a plain titleId. details is optional, and any
 * malformed or duplicated detail member is treated as absent. */
static bool title_entry(X4JsonSpan item, X4CatalogTitle *t)
{
    X4JsonSpan v, details;
    memset(t, 0, sizeof(*t));
    if (x4_json_type(item) != X4_JSON_T_OBJECT) return false;
    if (x4_json_member(item, "titleId", &v) != 1 || x4_json_token(v, t->id, sizeof(t->id)) < 0 || !title_id(t->id))
        return false;
    if (x4_json_member(item, "details", &details) != 1 || x4_json_type(details) != X4_JSON_T_OBJECT) return true;
    if (x4_json_member(details, "productId", &v) == 1 &&
        (x4_json_token(v, t->product_id, sizeof(t->product_id)) < 0 || !product_id(t->product_id)))
        t->product_id[0] = 0;
    int entitled = 0;
    if (x4_json_member(details, "hasEntitlement", &v) == 1 && x4_json_bool(v, &entitled) == 0 && entitled)
        t->entitled = 1;
    else if (subscribed(details)) t->entitled = 1;
    return true;
}

/* gsToken -> default region /v2/titles. gsToken and its header are wiped
 * right after the request. */
static int titles(X4XboxWork *w)
{
    size_t length = 0;
    int status = 0;
    step(w, "lista de titulos de la region");
    int written = snprintf(w->authorization, sizeof(w->authorization), "Bearer %s", w->gs_token);
    x4_secure_clear(w->gs_token, sizeof(w->gs_token));
    if (written < 0 || (size_t)written >= sizeof(w->authorization)) {
        x4_secure_clear(w->authorization, sizeof(w->authorization));
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_FORM, 0, "credencial de region");
    }
    const X4HttpHeader headers[] = {
        {"Authorization", w->authorization},
        {"x-gssv-client", "XboxComBrowser"},
        {"X-MS-Device-Info", X4_XBOX_DEVICE_INFO},
    };
    int rc = call(w, w->url, headers, sizeof(headers) / sizeof(headers[0]), false, &length, &status);
    x4_secure_clear(w->authorization, sizeof(w->authorization));
    if (rc) return failed(w, rc);
    if (status != 200) {
        wipe_response(w, length);
        return finish(w, X4_CATALOG_ERROR, status == 401 || status == 403 ? X4_AUTH_E_XBOX : X4_AUTH_E_STATUS,
            status, "Xbox rechazo la lista de titulos");
    }
    X4JsonSpan root, results, item;
    if (x4_json_parse(w->response, length, &root) || x4_json_member(root, "results", &results) != 1 ||
        x4_json_type(results) != X4_JSON_T_ARRAY) {
        wipe_response(w, length);
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_RESPONSE, status, "lista de titulos no valida");
    }
    X4CatalogSnapshot *r = &w->result;
    X4CatalogTitle t;
    unsigned total = 0, entitled = 0;
    size_t cursor = 0;
    int more;
    while ((more = x4_json_item(results, &cursor, &item)) == 1) {
        if (atomic_load(w->cancel)) {
            wipe_response(w, length);
            return finish(w, X4_CATALOG_CANCELLED, 0, 0, "consulta cancelada");
        }
        if (!title_entry(item, &t)) continue;
        /* Total counts validated received entries, including repetitions;
         * only the retained display list is deduplicated. */
        if (total < UINT32_MAX) ++total;
        /* Repeated titleId among kept entries: merge service facts only. */
        X4CatalogTitle *same = NULL;
        for (unsigned i = 0; i < r->count && !same; ++i)
            if (!strcmp(r->titles[i].id, t.id)) same = &r->titles[i];
        if (same) {
            if (t.entitled) same->entitled = 1;
            if (!same->product_id[0]) copy_text(same->product_id, sizeof(same->product_id), t.product_id);
            continue;
        }
        if (r->count < X4_CATALOG_MAX) r->titles[r->count++] = t;
        else r->truncated = 1;
    }
    wipe_response(w, length);
    if (more < 0) {
        r->count = 0;
        return finish(w, X4_CATALOG_ERROR, X4_AUTH_E_RESPONSE, status, "lista de titulos no valida");
    }
    for (unsigned i = 0; i < r->count; ++i) entitled += r->titles[i].entitled ? 1u : 0u;
    r->total = total;
    r->http_status = status;
    printf("XCloud4: catalogo Xbox validos=%u guardados=%u con acceso=%u\n", total, r->count, entitled);
    return 0;
}

/* Maps Products[].ProductId to kept titles among the first `limit`. */
static unsigned apply_names(X4XboxWork *w, unsigned limit, size_t length)
{
    X4JsonSpan root, products, product, v, localized, first;
    unsigned named = 0;
    if (x4_json_parse(w->response, length, &root) || x4_json_member(root, "Products", &products) != 1) return 0;
    size_t cursor = 0;
    while (x4_json_item(products, &cursor, &product) == 1) {
        char id[sizeof(w->result.titles[0].product_id)], name[sizeof(w->result.titles[0].name)];
        size_t lc = 0;
        if (x4_json_member(product, "ProductId", &v) != 1 || x4_json_token(v, id, sizeof(id)) < 0 ||
            !product_id(id))
            continue;
        if (x4_json_member(product, "LocalizedProperties", &localized) != 1 ||
            x4_json_item(localized, &lc, &first) != 1 ||
            x4_json_member(first, "ProductTitle", &v) != 1 || x4_json_text(v, name, sizeof(name)) < 0)
            continue;
        size_t start = 0, end = strlen(name);
        while (name[start] == ' ') ++start;
        while (end > start && name[end - 1] == ' ') --end;
        if (start == end) continue;
        name[end] = 0;
        for (unsigned i = 0; i < limit; ++i) {
            X4CatalogTitle *t = &w->result.titles[i];
            if (!t->name[0] && t->product_id[0] && same_id(t->product_id, id)) {
                copy_text(t->name, sizeof(t->name), name + start);
                ++named;
            }
        }
    }
    return named;
}

/* Public Store names for the first titles, at most 8 IDs per request.
 * Failures are not fatal; only cancellation stops the catalog. */
static int store_names(X4XboxWork *w)
{
    unsigned limit = w->result.count < X4_XBOX_STORE_NAMED ? w->result.count : X4_XBOX_STORE_NAMED;
    bool queued[X4_XBOX_STORE_NAMED] = {0};
    unsigned batch = 0, named = 0;
    for (;;) {
        size_t used = 0;
        unsigned ids = 0;
        copy_text(w->url, sizeof(w->url), X4_XBOX_STORE_PREFIX);
        used = strlen(w->url);
        for (unsigned i = 0; i < limit && ids < X4_XBOX_STORE_BATCH; ++i) {
            const char *id = w->result.titles[i].product_id;
            if (queued[i] || !id[0]) continue;
            queued[i] = true;
            /* An ID already queued for an earlier title is named with it. */
            bool repeated = false;
            for (unsigned j = 0; j < i && !repeated; ++j)
                repeated = w->result.titles[j].product_id[0] && same_id(w->result.titles[j].product_id, id);
            if (repeated) continue;
            size_t n = strlen(id);
            if (used + (ids ? 1 : 0) + n >= sizeof(w->url)) break;
            if (ids) w->url[used++] = ',';
            memcpy(w->url + used, id, n + 1);
            used += n;
            ++ids;
        }
        if (!ids) break;
        size_t tail = sizeof(X4_XBOX_STORE_SUFFIX) - 1;
        if (used + tail >= sizeof(w->url)) break;
        memcpy(w->url + used, X4_XBOX_STORE_SUFFIX, tail + 1);
        ++batch;
        step(w, "nombres de Microsoft Store");
        size_t length = 0;
        int status = 0;
        int rc = call(w, w->url, NULL, 0, false, &length, &status);
        if (rc == X4_XBOX_CANCELLED) return failed(w, rc);
        if (rc) {
            printf("XCloud4: Store lote %u error 0x%08x\n", batch, (unsigned)rc);
            continue;
        }
        unsigned got = status == 200 ? apply_names(w, limit, length) : 0;
        wipe_response(w, length);
        named += got;
        printf("XCloud4: Store lote %u estado %d nombres=%u\n", batch, status, got);
    }
    printf("XCloud4: Store lotes=%u nombres=%u\n", batch, named);
    return 0;
}

static void run(X4XboxWork *w, const char *microsoft_token)
{
    if (user_token(w, microsoft_token) || xsts(w)) return;
    /* f2p only when Xbox refused the main offering; never after a cancel
     * or a transport failure. */
    int rc = login(w, 0);
    if (rc == 1) rc = login(w, 1);
    x4_secure_clear(w->xsts_token, sizeof(w->xsts_token));
    if (rc) return;
    if (titles(w) || store_names(w)) return;
    if (atomic_load(w->cancel)) {
        finish(w, X4_CATALOG_CANCELLED, 0, 0, "consulta cancelada");
        return;
    }
    finish(w, X4_CATALOG_READY, 0, w->result.http_status, "catalogo listo");
}

const X4CatalogSnapshot *x4_xbox_catalog(X4XboxWork *w, const char *microsoft_token,
    const _Atomic int *cancel, X4XboxProgress progress, void *context)
{
    memset(&w->result, 0, sizeof(w->result));
    w->result.state = X4_CATALOG_LOADING;
    w->cancel = cancel;
    w->progress = progress;
    w->session_progress = NULL;
    w->context = context;
    int rc = 0;
    step(w, "iniciando HTTPS");
    w->http = x4_http_open(&rc);
    if (!w->http) finish(w, X4_CATALOG_ERROR, rc, 0, "inicio HTTPS (red/TLS)");
    else run(w, microsoft_token);
    x4_http_close(w->http);
    w->http = NULL;
    x4_secure_clear(w->user_token, sizeof(w->user_token));
    x4_secure_clear(w->xsts_token, sizeof(w->xsts_token));
    x4_secure_clear(w->gs_token, sizeof(w->gs_token));
    x4_secure_clear(w->authorization, sizeof(w->authorization));
    x4_secure_clear(w->request, sizeof(w->request));
    x4_secure_clear(w->url, sizeof(w->url));
    x4_secure_clear(w->origin, sizeof(w->origin));
    X4CatalogSnapshot *r = &w->result;
    if (r->state != X4_CATALOG_READY) {
        memset(r->titles, 0, sizeof(r->titles));
        r->count = r->total = 0;
        r->truncated = 0;
    }
    printf("XCloud4: catalogo Xbox fin estado=%d http=%d error=0x%08x xerr=%u titulos=%u/%u\n",
        (int)r->state, r->http_status, (unsigned)r->error, (unsigned)r->xerr, r->count, r->total);
    return r;
}

/* Cloud session, connection authorization and RTC signaling. The lifecycle
 * sends the console-transfer token to /connect once, exchanges the actual
 * RTC offer/answer and candidates, then maintains the session. Session
 * path, ID, bearer, Passport token and bodies never leave the workspace,
 * logs only carry literals and codes. */

/* Terminal outcome decided before the cleanup DELETE runs. */
typedef struct {
    enum X4SessionState state;
    int error, status;
    char stage[80];
} SessionEnd;

static uint64_t session_now(void)
{
    return sceKernelGetProcessTime();
}

static unsigned seconds_until(uint64_t deadline, uint64_t t)
{
    if (deadline == UINT64_MAX) return 0;
    return deadline > t ? (unsigned)((deadline - t + X4_SESSION_USEC - 1) / X4_SESSION_USEC) : 0;
}

static void session_state(X4XboxWork *w, enum X4SessionState state, const char *stage, unsigned left)
{
    w->session.state = state;
    w->session.seconds_left = left;
    copy_text(w->session.stage, sizeof(w->session.stage), stage);
    printf("XCloud4: sesion Xbox estado=%d etapa: %s\n", (int)state, stage);
    session_publish(w);
}

/* A zero status keeps the last HTTP status already recorded. */
static void session_end(X4XboxWork *w, enum X4SessionState state, int error, int status, const char *stage)
{
    w->session.error = error;
    if (status) w->session.http_status = status;
    session_state(w, state, stage, 0);
}

static void end_with(SessionEnd *end, enum X4SessionState state, int error, int status, const char *stage)
{
    end->state = state;
    end->error = error;
    end->status = status;
    copy_text(end->stage, sizeof(end->stage), stage);
}

/* Copies the HTTP stage literal while http is alive. */
static void transport_end(X4XboxWork *w, SessionEnd *end, int rc)
{
    char stage[80];
    snprintf(stage, sizeof(stage), "HTTPS: %s", x4_http_stage(w->http));
    end_with(end, X4_SESSION_ERROR, rc, 0, stage);
}

/* Sleeps in 100 ms steps: 0 once `until` arrives, 1 on cancel, 2 at
 * `deadline`. The countdown to `deadline` is republished every second. */
static int session_wait(X4XboxWork *w, uint64_t until, uint64_t deadline)
{
    for (;;) {
        if (atomic_load(w->cancel)) return 1;
        uint64_t t = session_now();
        if (t >= deadline) return 2;
        if (t >= until) return 0;
        unsigned left = seconds_until(deadline, t);
        unsigned elapsed = (unsigned)((t - w->session_start) / X4_SESSION_USEC);
        if (left != w->session.seconds_left || elapsed != w->session.elapsed_seconds) {
            w->session.seconds_left = left;
            session_publish(w);
        }
        sceKernelUsleep(X4_SESSION_STEP_USEC);
    }
}

/* 1..128 of [A-Za-z0-9_-] (GUIDs fit), never the collection names. */
static bool session_id_valid(const char *id)
{
    static const char *const reserved[] = {"play", "active"};
    size_t n = 0;
    while (n <= X4_SESSION_ID_MAX && id[n]) {
        unsigned char c = (unsigned char)id[n];
        if (!alphanumeric(c) && c != '-' && c != '_') return false;
        ++n;
    }
    if (n == 0 || n > X4_SESSION_ID_MAX) return false;
    for (size_t k = 0; k < sizeof(reserved) / sizeof(reserved[0]); ++k) {
        size_t i = 0;
        for (; i < n && reserved[k][i]; ++i) {
            char c = id[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != reserved[k][i]) break;
        }
        if (i == n && !reserved[k][i]) return false;
    }
    return true;
}

/* sessionPath: an optional single leading '/', then exactly
 * v5/sessions/cloud/<id>. Only the validated ID is kept. */
static bool session_path(X4XboxWork *w, size_t length)
{
    char path[256] = {0};
    X4JsonSpan root, v;
    bool ok = false;
    if (x4_json_parse(w->response, length, &root) == 0 && x4_json_member(root, "sessionPath", &v) == 1 &&
        x4_json_token(v, path, sizeof(path)) == 0) {
        const char *p = path[0] == '/' ? path + 1 : path;
        size_t prefix = sizeof(X4_SESSION_PREFIX) - 1;
        if (!strncmp(p, X4_SESSION_PREFIX, prefix) && session_id_valid(p + prefix)) {
            copy_text(w->session_id, sizeof(w->session_id), p + prefix);
            ok = true;
        }
    }
    x4_secure_clear(path, sizeof(path));
    return ok;
}

/* <origin>/v5/sessions/cloud/<id><suffix>, or .../play when no ID is given. */
static bool session_url(X4XboxWork *w, const char *id, const char *suffix)
{
    int written = snprintf(w->url, sizeof(w->url), "%s/" X4_SESSION_PREFIX "%s%s", w->origin, id, suffix);
    if (written > 0 && (size_t)written < sizeof(w->url)) return true;
    x4_secure_clear(w->url, sizeof(w->url));
    return false;
}

/* One session exchange with the private regional bearer. cancel may be NULL
 * for exchanges that must not be abandoned midway. The request body is wiped
 * either way; on error the response is wiped too. */
static int session_call(X4XboxWork *w, enum X4HttpSessionMethod method, const char *json,
    const _Atomic int *cancel, size_t *length, int *status)
{
    const X4HttpHeader headers[] = {
        {"Authorization", w->authorization},
        {"x-gssv-client", "XboxComBrowser"},
        {"X-MS-Device-Info", X4_XBOX_DEVICE_INFO},
    };
    *length = 0;
    *status = 0;
    int rc = x4_http_session_request(w->http, method, w->url, headers, sizeof(headers) / sizeof(headers[0]),
        json, w->response, sizeof(w->response), length, status, cancel);
    x4_secure_clear(w->request, sizeof(w->request));
    if (rc) {
        wipe_response(w, *length);
        *length = 0;
        *status = 0;
    }
    return rc;
}

/* Server state -> public meaning and a constant stage. Unknown values keep
 * waiting under a generic literal; the raw string is never shown. */
static enum X4SessionState remote_state(const char *state, const char **stage)
{
    if (!strcmp(state, "ReadyToConnect")) { *stage = "Xbox lista para negociar la conexion"; return X4_SESSION_READY; }
    if (!strcmp(state, "Provisioned")) { *stage = "sesion aprovisionada por Xbox"; return X4_SESSION_READY; }
    if (!strcmp(state, "Failed") || !strcmp(state, "Error")) {
        *stage = "Xbox reporto un fallo de la sesion";
        return X4_SESSION_ERROR;
    }
    if (!strcmp(state, "Provisioning")) *stage = "Xbox prepara la sesion";
    else if (!strcmp(state, "WaitingForResources")) *stage = "esperando recursos de Xbox";
    else *stage = "esperando respuesta de Xbox";
    return X4_SESSION_WAITING;
}

/* Failed authentication step recorded in w->result. No remote session
 * exists yet, so there is nothing to clean up. */
static void session_auth_failed(X4XboxWork *w)
{
    if (w->result.state == X4_CATALOG_CANCELLED || atomic_load(w->cancel))
        session_end(w, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
    else session_end(w, X4_SESSION_ERROR, w->result.error, w->result.http_status, w->result.stage);
}

/* POST /play. 1 with a validated session ID; 0 when no remote session can
 * exist; -1 when one may exist but cannot be addressed. The terminal view is
 * published for 0 and -1. */
static int session_create(X4XboxWork *w, const X4CatalogTitle *title)
{
    size_t used = 0, length = 0;
    int status = 0;
    session_state(w, X4_SESSION_STARTING, "solicitando sesion en la nube", 0);
    if (!put(w, &used, X4_SESSION_PLAY_HEAD) || !put(w, &used, title->id) || !put(w, &used, X4_SESSION_PLAY_TAIL) ||
        !session_url(w, "play", "")) {
        x4_secure_clear(w->request, sizeof(w->request));
        session_end(w, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "solicitud de sesion");
        return 0;
    }
    /* Last point where a cancel provably leaves nothing on the server. */
    if (atomic_load(w->cancel)) {
        x4_secure_clear(w->request, sizeof(w->request));
        session_end(w, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
        return 0;
    }
    /* Not cancellable: an abandoned read could drop the body naming a session
     * the server already created. Bounded by the HTTP layer's 30 s deadline
     * and native timeouts; a cancel is honoured right after, by deleting.
     * Never retried: a repeat could create a second session. */
    int rc = session_call(w, X4_SESSION_HTTP_POST, w->request, NULL, &length, &status);
    if (rc) {
        /* These are refused before anything is sent. Other failures may come
         * after the server created a session we cannot address. */
        bool unsent = rc == X4_HTTP_URL || rc == X4_HTTP_ARGUMENT || rc == X4_HTTP_ALLOCATION ||
            rc == X4_HTTP_UNUSABLE || rc == X4_HTTP_BUSY;
        SessionEnd end;
        transport_end(w, &end, rc);
        if (!unsent) {
            w->session.cleanup_failed = 1;
            w->session.cleanup_error = X4_AUTH_E_UNCONFIRMED;
        }
        session_end(w, end.state, end.error, 0, end.stage);
        return unsent ? 0 : -1;
    }
    if (status < 200 || status > 299) {
        wipe_response(w, length);
        session_end(w, X4_SESSION_ERROR, status == 401 || status == 403 ? X4_AUTH_E_XBOX : X4_AUTH_E_STATUS,
            status, "Xbox no inicio la sesion");
        return 0;
    }
    bool ok = session_path(w, length);
    wipe_response(w, length);
    if (!ok) {
        w->session.cleanup_failed = 1;
        w->session.cleanup_error = X4_AUTH_E_UNCONFIRMED;
        session_end(w, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta de sesion no valida");
        return -1;
    }
    w->session.http_status = status;
    printf("XCloud4: sesion Xbox creada, estado HTTP %d\n", status);
    return 1;
}

/* GET .../state every 2 s until READY, a failure, cancel or the deadline.
 * true when READY was reached; otherwise *end holds the outcome. */
static bool session_provision(X4XboxWork *w, uint64_t deadline, SessionEnd *end)
{
    uint64_t next = session_now();
    session_state(w, X4_SESSION_WAITING, "esperando respuesta de Xbox", seconds_until(deadline, next));
    for (;;) {
        int waited = session_wait(w, next, deadline);
        if (waited == 1) { end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada"); return false; }
        if (waited == 2) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, 0, "Xbox no preparo la sesion a tiempo");
            return false;
        }
        if (!session_url(w, w->session_id, "/state")) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "consulta de estado");
            return false;
        }
        size_t length = 0;
        int status = 0;
        int rc = session_call(w, X4_SESSION_HTTP_GET, NULL, w->cancel, &length, &status);
        uint64_t done = session_now();
        if (rc == X4_HTTP_CANCELLED || atomic_load(w->cancel)) {
            wipe_response(w, length);
            end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
            return false;
        }
        if (rc) { transport_end(w, end, rc); return false; }
        /* A request may finish after the provisioning deadline. Do not
         * publish a late READY response as success. Cleanup still follows. */
        if (done >= deadline) {
            wipe_response(w, length);
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, status,
                "Xbox no preparo la sesion a tiempo");
            return false;
        }
        if (status != 200) {
            wipe_response(w, length);
            end_with(end, X4_SESSION_ERROR, status == 401 || status == 403 ? X4_AUTH_E_XBOX : X4_AUTH_E_STATUS,
                status, "estado HTTP inesperado de la sesion");
            return false;
        }
        /* A string state is required; one that does not fit or is not plain
         * ASCII reads as empty, i.e. unknown, and keeps the bounded wait. */
        char state[40] = {0};
        X4JsonSpan root, v;
        bool valid = x4_json_parse(w->response, length, &root) == 0 && x4_json_member(root, "state", &v) == 1 &&
            x4_json_type(v) == X4_JSON_T_STRING;
        if (valid && x4_json_token(v, state, sizeof(state)) < 0) state[0] = 0;
        wipe_response(w, length);
        if (!valid) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, status, "estado de sesion no valido");
            return false;
        }
        const char *stage = NULL;
        enum X4SessionState next_state = remote_state(state, &stage);
        bool provisioned = !strcmp(state, "Provisioned");
        x4_secure_clear(state, sizeof(state));
        w->session.http_status = status;
        if (next_state == X4_SESSION_ERROR) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_SESSION, status, stage);
            return false;
        }
        if (next_state == X4_SESSION_READY) {
            /* Server-side readiness only; nothing is connected or rendered. */
            w->session.ready_seen = 1;
            w->provisioned = provisioned;
            session_state(w, X4_SESSION_READY, stage, 0);
            return true;
        }
        if (strcmp(w->session.stage, stage)) session_state(w, X4_SESSION_WAITING, stage, seconds_until(deadline, done));
        next = done + X4_SESSION_POLL_USEC;
    }
}

/* {"userToken":"<token>"} into w->request. '"' and '\' are escaped and any
 * byte outside printable ASCII is refused, so the untrusted token can never
 * end the string or add a control character. The worst case (16 KiB, all
 * escaped) stays far below the request buffer; every step is bounded. */
static bool connect_body(X4XboxWork *w)
{
    static const char head[] = "{\"userToken\":\"", tail[] = "\"}";
    size_t used = sizeof(head) - 1, i = 0;
    memcpy(w->request, head, used);
    for (; i < sizeof(w->passport) && w->passport[i]; ++i) {
        unsigned char c = (unsigned char)w->passport[i];
        if (c < 0x20 || c > 0x7e) return false;
        bool escape = c == '"' || c == '\\';
        /* Room for this character, the tail and its NUL. */
        if (used + (escape ? 2 : 1) + sizeof(tail) > sizeof(w->request)) return false;
        if (escape) w->request[used++] = '\\';
        w->request[used++] = (char)c;
    }
    if (i == 0 || i == sizeof(w->passport)) return false;
    memcpy(w->request + used, tail, sizeof(tail));
    return true;
}

/* 2xx /connect body: empty (JSON whitespace only) is accepted; otherwise it
 * must be one valid JSON object, refused when it carries a
 * non-null errorDetails. 1 accepted, 0 refused by Xbox, -1 malformed or
 * ambiguous. Nothing from the body is kept or logged. */
static int connect_result(X4XboxWork *w, size_t length)
{
    size_t i = 0;
    while (i < length && (w->response[i] == ' ' || w->response[i] == '\t' || w->response[i] == '\r' ||
        w->response[i] == '\n')) ++i;
    if (i == length) return 1;
    X4JsonSpan root, v;
    if (x4_json_parse(w->response, length, &root)) return -1;
    if (x4_json_type(root) != X4_JSON_T_OBJECT) return -1;
    int found = x4_json_member(root, "errorDetails", &v);
    if (found < 0) return -1;
    return found == 1 && x4_json_type(v) != X4_JSON_T_NULL ? 0 : 1;
}

/* READY -> AUTHORIZING -> AUTHORIZED. The private provider renews the
 * Microsoft access and obtains the console-transfer token on this worker and
 * this HTTPS context; that token only lives in w->passport and w->request
 * and is wiped once the body is built and sent. /connect is posted at most
 * once and never after a cancel was seen. true once Xbox accepted it;
 * otherwise *end holds the outcome and the DELETE still follows. */
static bool session_authorize(X4XboxWork *w, SessionEnd *end)
{
    session_state(w, X4_SESSION_AUTHORIZING, "autorizando conexion con Microsoft", 0);
    if (atomic_load(w->cancel)) { end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada"); return false; }
    int passport_status = 0;
    const char *stage = NULL;
    int rc = w->passport_provider(w->passport_context, w->http, w->passport, sizeof(w->passport),
        &passport_status, &stage, w->cancel);
    w->session.passport_http_status = passport_status;
    if (rc) {
        x4_secure_clear(w->passport, sizeof(w->passport));
        if (rc == X4_XBOX_PASSPORT_CANCELLED) end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
        else end_with(end, X4_SESSION_ERROR, rc, passport_status,
            stage ? stage : "Microsoft no pudo autorizar la conexion");
        return false;
    }
    bool built = connect_body(w) && session_url(w, w->session_id, "/connect");
    x4_secure_clear(w->passport, sizeof(w->passport));
    if (!built) {
        x4_secure_clear(w->request, sizeof(w->request));
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "solicitud de autorizacion de conexion");
        return false;
    }
    /* Last point where a cancel provably keeps /connect unsent. */
    if (atomic_load(w->cancel)) {
        x4_secure_clear(w->request, sizeof(w->request));
        end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
        return false;
    }
    session_state(w, X4_SESSION_AUTHORIZING, "enviando autorizacion a Xbox", 0);
    size_t length = 0;
    int status = 0;
    /* Cancellable: whatever /connect did, the cleanup DELETE follows. */
    rc = session_call(w, X4_SESSION_HTTP_POST, w->request, w->cancel, &length, &status);
    if (rc == X4_HTTP_CANCELLED) { end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada"); return false; }
    if (rc) { transport_end(w, end, rc); return false; }
    w->session.connect_http_status = status;
    printf("XCloud4: sesion Xbox /connect estado HTTP %d\n", status);
    if (status < 200 || status > 299) {
        wipe_response(w, length);
        end_with(end, X4_SESSION_ERROR, status == 401 || status == 403 ? X4_AUTH_E_XBOX : X4_AUTH_E_STATUS,
            status, "Xbox rechazo la autorizacion de conexion");
        return false;
    }
    int result = connect_result(w, length);
    wipe_response(w, length);
    if (result < 0) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta de autorizacion no valida");
        return false;
    }
    if (result == 0) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_SESSION, status, "Xbox rechazo la autorizacion de conexion");
        return false;
    }
    /* Accepted (202 included): authorization only, no media connection. */
    w->session.connection_authorized = 1;
    w->session.http_status = status;
    if (atomic_load(w->cancel)) { end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada"); return false; }
    session_state(w, X4_SESSION_AUTHORIZED, "Xbox acepto la autorizacion de conexion",
        0);
    return true;
}

/* Private JSON writer for signaling. The outer HTTP layer permits ASCII
 * JSON only, so CR/LF/TAB in SDP must be escaped, never flattened. */
typedef struct { char *data; size_t capacity, used; } SignalWriter;

static bool signal_put(SignalWriter *b, const char *text)
{
    size_t n = strlen(text);
    if (n >= b->capacity - b->used) return false;
    memcpy(b->data + b->used, text, n + 1);
    b->used += n;
    return true;
}

static bool signal_string(SignalWriter *b, const char *text, size_t limit)
{
    if (!signal_put(b, "\"")) return false;
    size_t i = 0;
    for (; i < limit && text[i]; ++i) {
        unsigned char c = (unsigned char)text[i];
        const char *escape = NULL;
        if (c == '"') escape = "\\\"";
        else if (c == '\\') escape = "\\\\";
        else if (c == '\r') escape = "\\r";
        else if (c == '\n') escape = "\\n";
        else if (c == '\t') escape = "\\t";
        else if (c < 0x20 || c > 0x7e) return false;
        if (escape) { if (!signal_put(b, escape)) return false; }
        else {
            char one[2] = {(char)c, 0};
            if (!signal_put(b, one)) return false;
        }
    }
    if (i == limit) return false;
    return signal_put(b, "\"");
}

static int signal_hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Only validated string spans are accepted. SDP is ASCII with CR/LF/TAB;
 * escaped NUL, other controls, surrogates and non-ASCII are refused. */
static bool signal_decode(X4JsonSpan v, char *out, size_t capacity, bool lines)
{
    if (!capacity) return false;
    out[0] = 0;
    if (x4_json_type(v) != X4_JSON_T_STRING || v.length < 2) return false;
    size_t n = 0;
    for (size_t i = 1; i + 1 < v.length; ++i) {
        unsigned c = (unsigned char)v.data[i];
        if (c == '\\') {
            if (++i + 1 >= v.length) goto bad;
            c = (unsigned char)v.data[i];
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'r': c = '\r'; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'u': {
                if (i + 4 + 1 >= v.length) goto bad;
                c = 0;
                for (unsigned k = 0; k < 4; ++k) {
                    int h = signal_hex((unsigned char)v.data[++i]);
                    if (h < 0) goto bad;
                    c = c * 16 + (unsigned)h;
                }
                break;
            }
            default: goto bad;
            }
        }
        if ((c < 0x20 || c > 0x7e) && !(lines && (c == '\r' || c == '\n' || c == '\t'))) goto bad;
        if (n + 1 >= capacity) goto bad;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return true;
bad:
    x4_secure_clear(out, capacity);
    return false;
}

static bool signal_offer(X4XboxWork *w)
{
    SignalWriter b = {w->request, sizeof(w->request), 0};
    static const char tail[] =
        ",\"requestId\":\"1\",\"configuration\":{\"chatConfiguration\":{"
        "\"bytesPerSample\":2,\"expectedClipDurationMs\":20,\"format\":{"
        "\"codec\":\"opus\",\"container\":\"webm\"},\"numChannels\":1,\"sampleFrequencyHz\":24000},"
        "\"chat\":{\"minVersion\":1,\"maxVersion\":1},"
        "\"control\":{\"minVersion\":1,\"maxVersion\":3},"
        "\"input\":{\"minVersion\":1,\"maxVersion\":9},"
        "\"message\":{\"minVersion\":1,\"maxVersion\":1},"
        "\"reliableinput\":{\"minVersion\":9,\"maxVersion\":9},"
        "\"unreliableinput\":{\"minVersion\":9,\"maxVersion\":9}}}";
    return signal_put(&b, "{\"messageType\":\"offer\",\"sdp\":") &&
        signal_string(&b, w->sdp, sizeof(w->sdp)) && signal_put(&b, tail);
}

/* 0: pending, 1: complete nested exchange, -1: malformed, -2: refused.
 * The returned span points into the private decoded exchange buffer. */
static int signal_exchange(X4XboxWork *w, size_t length, int status, X4JsonSpan *exchange)
{
    if (status == 204 && !length) return 0;
    X4JsonSpan root, v;
    if (x4_json_parse(w->response, length, &root) || x4_json_type(root) != X4_JSON_T_OBJECT) return -1;
    int member = x4_json_member(root, "errorDetails", &v);
    if (member < 0) return -1;
    if (member && x4_json_type(v) != X4_JSON_T_NULL) return -2;
    member = x4_json_member(root, "status", &v);
    if (member < 0) return -1;
    if (member) {
        uint32_t n;
        if (x4_json_uint32(v, &n)) return -1;
        if (n == 204) return 0;
    }
    if (x4_json_member(root, "exchangeResponse", &v) != 1 ||
        !signal_decode(v, w->exchange, sizeof(w->exchange), true)) return -1;
    if (x4_json_parse(w->exchange, strlen(w->exchange), exchange)) return -1;
    return 1;
}

static bool signal_cancelled(X4XboxWork *w, SessionEnd *end)
{
    if (!atomic_load(w->cancel)) return false;
    end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
    return true;
}

/* /connect is asynchronous (202 on the verified console). GreenVita waits
 * for Provisioned after authorizing, before exchanging SDP. ReadyToConnect
 * alone cannot prove the game worker is ready for that next exchange. */
static bool signal_provisioned(X4XboxWork *w, uint64_t deadline, SessionEnd *end)
{
    if (w->provisioned) return !signal_cancelled(w, end);
    session_state(w, X4_SESSION_NEGOTIATING, "esperando conexion aprovisionada por Xbox", seconds_until(deadline, session_now()));
    for (;;) {
        if (signal_cancelled(w, end)) return false;
        if (!session_url(w, w->session_id, "/state")) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "consulta de conexion no valida"); return false;
        }
        size_t length = 0;
        int status = 0;
        int rc = session_call(w, X4_SESSION_HTTP_GET, NULL, w->cancel, &length, &status);
        if (signal_cancelled(w, end) || rc == X4_HTTP_CANCELLED) {
            wipe_response(w, length);
            end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada"); return false;
        }
        if (rc) { transport_end(w, end, rc); return false; }
        if (session_now() >= deadline) {
            wipe_response(w, length);
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, status, "Xbox no aprovisiono la conexion a tiempo"); return false;
        }
        if (status != 200) {
            wipe_response(w, length);
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_STATUS, status, "Xbox no confirmo el estado de conexion"); return false;
        }
        X4JsonSpan root, value;
        char state[40] = {0};
        bool valid = !x4_json_parse(w->response, length, &root) &&
            x4_json_member(root, "state", &value) == 1 && !x4_json_token(value, state, sizeof(state));
        wipe_response(w, length);
        if (!valid) { end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, status, "estado de conexion no valido"); return false; }
        bool ready = !strcmp(state, "Provisioned");
        bool failed = !strcmp(state, "Failed") || !strcmp(state, "Error");
        x4_secure_clear(state, sizeof(state));
        w->session.http_status = status;
        if (failed) { end_with(end, X4_SESSION_ERROR, X4_AUTH_E_SESSION, status, "Xbox rechazo la conexion aprovisionada"); return false; }
        if (ready) {
            w->provisioned = true;
            printf("XCloud4: conexion aprovisionada HTTP %d\n", status);
            return true;
        }
        int waited = session_wait(w, session_now() + X4_SESSION_SDP_POLL_USEC, deadline);
        if (waited) {
            end_with(end, waited == 1 ? X4_SESSION_CANCELLED : X4_SESSION_ERROR,
                waited == 1 ? 0 : X4_AUTH_E_DEADLINE, 0, waited == 1 ? "sesion cancelada" : "Xbox no aprovisiono la conexion a tiempo"); return false;
        }
    }
}

/* Only fixed classifications are logged, never a remote string. Code 0 is
 * absent, 1 unknown, and 2..14 follow the literal allowlist below. */
static int signal_error_literal(X4JsonSpan value)
{
    static const char *const allowed[] = {
        "SessionNotActive", "SessionNotFound", "SessionExpired", "InvalidSdp",
        "InvalidOffer", "InvalidRequest", "BadRequest", "Unauthorized",
        "NotAuthorized", "Forbidden", "Timeout", "InternalServerError", "ServiceUnavailable"
    };
    char code[64] = {0};
    int result = 1;
    if (!x4_json_token(value, code, sizeof(code))) {
        for (unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)
            if (!strcmp(code, allowed[i])) { result=(int)i+2; break; }
    }
    x4_secure_clear(code, sizeof(code));
    return result;
}

static int signal_error_code(X4JsonSpan object)
{
    X4JsonSpan value;
    int member = x4_json_member(object, "code", &value);
    return member < 1 ? member : signal_error_literal(value);
}

static int signal_error_class(X4JsonSpan value)
{
    X4JsonType type = x4_json_type(value);
    if (type == X4_JSON_T_OBJECT) return signal_error_code(value);
    if (type == X4_JSON_T_STRING) return signal_error_literal(value);
    return type == X4_JSON_T_NULL ? 0 : 1;
}

/* Error codes may arrive as signed HRESULTs. Keep their 32-bit pattern and
 * sign separately; this reader does not affect the business JSON parser. */
static bool signal_error_number(X4JsonSpan value, uint32_t *bits, bool *negative)
{
    *bits = 0;
    *negative = false;
    if (!x4_json_uint32(value, bits)) return true;
    if (x4_json_type(value) != X4_JSON_T_NUMBER || value.length < 2 || value.length > 11 ||
        value.data[0] != '-') return false;
    uint32_t magnitude = 0;
    for (size_t i = 1; i < value.length; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if (c < '0' || c > '9') return false;
        uint32_t digit = (uint32_t)(c - '0');
        if (magnitude > (2147483648u - digit) / 10u) return false;
        magnitude = magnitude * 10u + digit;
    }
    *bits = 0u - magnitude;
    *negative = true;
    return true;
}

/* Node 0 is the root, 1 errorDetails, 2 error, and 3/4 their details
 * objects. Only JSON types, fixed classes and validated 32-bit numbers
 * escape the response buffer; message/details text is never printed. */
static void signal_error_shape(unsigned node, X4JsonSpan value)
{
    X4JsonType type = x4_json_type(value);
    X4JsonSpan code = {0}, status = {0}, details = {0}, message = {0};
    int code_type = 0, status_type = 0, details_type = 0, message_type = 0;
    uint32_t code_number = 0, status_number = 0;
    bool code_number_ok = false, code_negative = false, status_number_ok = false;
    if (type == X4_JSON_T_OBJECT) {
        int found = x4_json_member(value, "code", &code);
        code_type = found == 1 ? (int)x4_json_type(code) : found;
        if (found == 1) code_number_ok = signal_error_number(code, &code_number, &code_negative);
        found = x4_json_member(value, "status", &status);
        status_type = found == 1 ? (int)x4_json_type(status) : found;
        if (found == 1) status_number_ok = !x4_json_uint32(status, &status_number);
        found = x4_json_member(value, "details", &details);
        details_type = found == 1 ? (int)x4_json_type(details) : found;
        found = x4_json_member(value, "message", &message);
        message_type = found == 1 ? (int)x4_json_type(message) : found;
    } else if (type == X4_JSON_T_NUMBER) {
        code_type = (int)type;
        code_number_ok = signal_error_number(value, &code_number, &code_negative);
    }
    printf("XCloud4: signal error shape node=%u type=%d class=%d code_type=%d code_num_ok=%d code_num=%u code_negative=%d status_type=%d status_num_ok=%d status_num=%u details_type=%d message_type=%d\n",
        node, (int)type, signal_error_class(value), code_type, code_number_ok, code_number, code_negative,
        status_type, status_number_ok, status_number, details_type, message_type);
}

/* Primary Xbox clients model errorDetails.code as a public service code.
 * Admit only a short ASCII identifier here, never a message or other field. */
static void signal_public_error_code(X4JsonSpan details)
{
    char name[65] = {0};
    X4JsonSpan code;
    bool valid = false;
    if (x4_json_type(details) == X4_JSON_T_OBJECT &&
        x4_json_member(details, "code", &code) == 1 &&
        x4_json_type(code) == X4_JSON_T_STRING &&
        !x4_json_token(code, name, sizeof(name))) {
        size_t length = strlen(name);
        unsigned char first = (unsigned char)name[0];
        valid = length >= 1 && length <= 64 &&
            ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z'));
        for (size_t i = 1; valid && i < length; ++i) {
            unsigned char c = (unsigned char)name[i];
            valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_';
        }
    }
    if (valid) printf("XCloud4: signal public error name=%s\n", name);
    x4_secure_clear(name, sizeof(name));
}

static void signal_error_diagnostic(X4XboxWork *w, size_t length, int status, const char *suffix)
{
    X4JsonSpan root={0}, details={0}, error={0};
    bool valid = !x4_json_parse(w->response, length, &root) && x4_json_type(root)==X4_JSON_T_OBJECT;
    int top_code=-1, details_type=0, details_code=-1, error_type=0, error_code=-1;
    if (valid) {
        top_code=signal_error_code(root);
        if (x4_json_member(root,"errorDetails",&details)==1) {
            details_type=x4_json_type(details);
            details_code=signal_error_class(details);
        }
        if (x4_json_member(root,"error",&error)==1) {
            error_type=x4_json_type(error);
            error_code=signal_error_class(error);
        }
    }
    int route=!strcmp(suffix,"/sdp")?1:!strcmp(suffix,"/ice")?2:3;
    uint64_t now=session_now();
    unsigned long long elapsed=w->sdp_sent_at && now>=w->sdp_sent_at ? (now-w->sdp_sent_at)/1000ull:0;
    printf("XCloud4: signal HTTP failure route=%d http=%d bytes=%zu object=%d code=%d details_type=%d details_code=%d error_type=%d error_code=%d sdp_polls=%u elapsed_ms=%llu\n",
        route,status,length,valid,top_code,details_type,details_code,error_type,error_code,w->sdp_polls,elapsed);
    if (valid) {
        signal_error_shape(0, root);
        if (details_type) signal_error_shape(1, details);
        if (error_type) signal_error_shape(2, error);
        if (details_type == X4_JSON_T_OBJECT) signal_public_error_code(details);
        X4JsonSpan nested;
        if (details_type == X4_JSON_T_OBJECT && x4_json_member(details, "details", &nested) == 1 &&
            x4_json_type(nested) == X4_JSON_T_OBJECT) signal_error_shape(3, nested);
        if (error_type == X4_JSON_T_OBJECT && x4_json_member(error, "details", &nested) == 1 &&
            x4_json_type(nested) == X4_JSON_T_OBJECT) signal_error_shape(4, nested);
    }
}

/* Applies the same deadline after blocking native HTTP as before it. */
static bool signal_call(X4XboxWork *w, const char *suffix, enum X4HttpSessionMethod method,
    const char *json, uint64_t deadline, SessionEnd *end, size_t *length, int *status)
{
    if (signal_cancelled(w, end)) return false;
    if (session_now() >= deadline) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, 0, "WebRTC no negocio a tiempo");
        return false;
    }
    if (!session_url(w, w->session_id, suffix)) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "solicitud WebRTC no valida");
        return false;
    }
    int rc = session_call(w, method, json, w->cancel, length, status);
    if (!strcmp(suffix, "/sdp")) w->session.sdp_http_status = *status;
    else if (!strcmp(suffix, "/ice")) w->session.ice_http_status = *status;
    else w->session.keepalive_http_status = *status;
    if (signal_cancelled(w, end) || rc == X4_HTTP_CANCELLED) {
        end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
        return false;
    }
    if (rc) { transport_end(w, end, rc); return false; }
    if (session_now() >= deadline) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, *status, "WebRTC no negocio a tiempo");
        return false;
    }
    w->session.http_status = *status;
    if (*status < 200 || *status > 299) {
        signal_error_diagnostic(w,*length,*status,suffix);
        const char *stage = !strcmp(suffix, "/sdp") ? "Xbox rechazo la negociacion SDP" :
            !strcmp(suffix, "/ice") ? "Xbox rechazo el intercambio ICE" : "Xbox no mantuvo la sesion";
        end_with(end, X4_SESSION_ERROR, *status == 401 || *status == 403 ? X4_AUTH_E_XBOX : X4_AUTH_E_STATUS,
            *status, stage);
        return false;
    }
    return true;
}

static bool signal_keepalive(X4XboxWork *w, uint64_t deadline, SessionEnd *end)
{
    if (session_now() < w->next_keepalive) return true;
    uint64_t now=session_now();
    unsigned long long elapsed=w->sdp_sent_at && now>=w->sdp_sent_at ? (now-w->sdp_sent_at)/1000ull:0;
    printf("XCloud4: signal keepalive sdp_polls=%u elapsed_ms=%llu interval_ms=%llu\n",
        w->sdp_polls,elapsed,X4_SESSION_KEEPALIVE_USEC/1000ull);
    size_t length = 0;
    int status = 0;
    if (!signal_call(w, "/keepalive", X4_SESSION_HTTP_POST, "", deadline, end, &length, &status)) return false;
    int result = connect_result(w, length);
    bool gone = false;
    if (result > 0 && length) {
        X4JsonSpan root, code;
        char text[64];
        if (!x4_json_parse(w->response, length, &root)) {
            int found = x4_json_member(root, "code", &code);
            if (found < 0) result = -1;
            else if (found && x4_json_type(code) != X4_JSON_T_NULL) {
                if (x4_json_token(code, text, sizeof(text))) result = -1;
                else gone = !strcmp(text, "SessionNotActive") || !strcmp(text, "SessionNotFound");
            }
        }
    }
    if (result < 1 || gone) signal_error_diagnostic(w, length, status, "/keepalive");
    wipe_response(w, length);
    if (result < 1 || gone) {
        end_with(end, X4_SESSION_ERROR, result < 0 ? X4_AUTH_E_RESPONSE : X4_AUTH_E_SESSION, status,
            gone ? "Xbox termino la sesion" : "Xbox no confirmo el mantenimiento de sesion");
        return false;
    }
    w->next_keepalive = session_now() + X4_SESSION_KEEPALIVE_USEC;
    return true;
}

static bool signal_ack(X4XboxWork *w, const char *suffix, uint64_t deadline, SessionEnd *end)
{
    size_t length = 0;
    int status = 0;
    if (!signal_call(w, suffix, X4_SESSION_HTTP_POST, w->request, deadline, end, &length, &status)) return false;
    int result = connect_result(w, length);
    if (result < 1) signal_error_diagnostic(w, length, status, suffix);
    wipe_response(w, length);
    if (result < 1) {
        end_with(end, X4_SESSION_ERROR, result < 0 ? X4_AUTH_E_RESPONSE : X4_AUTH_E_SESSION, status,
            !strcmp(suffix, "/sdp") ? "Xbox no acepto la oferta SDP" : "Xbox no acepto los candidatos ICE");
        return false;
    }
    printf("XCloud4: intercambio %s HTTP %d\n", !strcmp(suffix, "/sdp") ? "SDP" : "ICE", status);
    return true;
}

static bool signal_sdp(X4XboxWork *w, uint64_t deadline, SessionEnd *end)
{
    session_state(w, X4_SESSION_NEGOTIATING, "preparando oferta WebRTC", seconds_until(deadline, session_now()));
    for (;;) {
        if (signal_cancelled(w, end)) return false;
        int rc = x4_rtc_local_description(w->rtc, w->sdp, sizeof(w->sdp));
        if (rc < 0) { end_with(end, X4_SESSION_ERROR, rc, 0, "no se pudo preparar la oferta WebRTC"); return false; }
        if (rc > 0) break;
        if (!signal_keepalive(w, deadline, end)) return false;
        int waited = session_wait(w, session_now() + X4_SESSION_STEP_USEC, deadline);
        if (waited) {
            end_with(end, waited == 1 ? X4_SESSION_CANCELLED : X4_SESSION_ERROR,
                waited == 1 ? 0 : X4_AUTH_E_DEADLINE, 0, waited == 1 ? "sesion cancelada" : "oferta WebRTC no disponible");
            return false;
        }
    }
    if (!signal_keepalive(w, deadline, end) || !signal_offer(w)) {
        if (!end->stage[0]) end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "oferta WebRTC fuera de limite");
        return false;
    }
    x4_secure_clear(w->sdp, sizeof(w->sdp));
    if (!signal_ack(w, "/sdp", deadline, end)) return false;
    w->sdp_sent_at=session_now();
    w->sdp_polls=0;
    session_state(w, X4_SESSION_NEGOTIATING, "esperando respuesta SDP de Xbox", seconds_until(deadline, session_now()));
    for (;;) {
        if (!signal_keepalive(w, deadline, end)) return false;
        size_t length = 0;
        int status = 0;
        ++w->sdp_polls;
        if (!signal_call(w, "/sdp", X4_SESSION_HTTP_GET, NULL, deadline, end, &length, &status)) return false;
        X4JsonSpan exchange, sdp;
        int result = signal_exchange(w, length, status, &exchange);
        if (result < 0) signal_error_diagnostic(w, length, status, "/sdp");
        wipe_response(w, length);
        if (result == 1) {
            bool valid = x4_json_type(exchange) == X4_JSON_T_OBJECT &&
                x4_json_member(exchange, "sdp", &sdp) == 1 && signal_decode(sdp, w->sdp, sizeof(w->sdp), true) &&
                w->sdp[0];
            x4_secure_clear(w->exchange, sizeof(w->exchange));
            if (!valid) { end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta SDP no valida"); return false; }
            int rc = x4_rtc_set_remote_description(w->rtc, w->sdp);
            x4_secure_clear(w->sdp, sizeof(w->sdp));
            if (rc < 0) { end_with(end, X4_SESSION_ERROR, rc, status, "WebRTC rechazo la respuesta SDP"); return false; }
            printf("XCloud4: respuesta SDP aplicada HTTP %d\n", status);
            return true;
        }
        x4_secure_clear(w->exchange, sizeof(w->exchange));
        if (result < 0) {
            end_with(end, X4_SESSION_ERROR, result == -2 ? X4_AUTH_E_SESSION : X4_AUTH_E_RESPONSE, status,
                result == -2 ? "Xbox rechazo el intercambio SDP" : "respuesta SDP no valida");
            return false;
        }
        int waited = session_wait(w, session_now() + X4_SESSION_SDP_POLL_USEC, deadline);
        if (waited) {
            end_with(end, waited == 1 ? X4_SESSION_CANCELLED : X4_SESSION_ERROR,
                waited == 1 ? 0 : X4_AUTH_E_DEADLINE, 0, waited == 1 ? "sesion cancelada" : "Xbox no respondio al SDP a tiempo");
            return false;
        }
    }
}

/* The Xbox API expects an array of serialized JSON strings, not objects.
 * Up to 16 freshly gathered candidates per POST keeps one request bounded. */
static bool signal_local_ice(X4XboxWork *w, uint64_t deadline, SessionEnd *end)
{
    SignalWriter body = {w->request, sizeof(w->request), 0};
    if (!signal_put(&body, "{\"candidates\":[")) return false;
    unsigned count = 0;
    while (count < 16) {
        int rc = x4_rtc_next_local_candidate(w->rtc, w->candidate, sizeof(w->candidate), w->mid, sizeof(w->mid));
        if (rc < 0) { end_with(end, X4_SESSION_ERROR, rc, 0, "no se pudo reunir candidatos ICE"); return false; }
        if (!rc) break;
        if (++w->local_candidates > X4_SESSION_CANDIDATES_MAX) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, 0, "demasiados candidatos ICE locales"); return false;
        }
        if (!w->mid[0]) copy_text(w->mid, sizeof(w->mid), "0");
        uint32_t index = 0;
        for (size_t k = 0; w->mid[k]; ++k) {
            if (w->mid[k] < '0' || w->mid[k] > '9' || index > 6) { index = 0; break; }
            index = index * 10 + (unsigned)(w->mid[k] - '0');
        }
        if (index > 64) index = 0;
        SignalWriter candidate = {w->candidate_json, sizeof(w->candidate_json), 0};
        char tail[64];
        snprintf(tail, sizeof(tail), ",\"sdpMLineIndex\":%u}", index);
        bool built = signal_put(&candidate, "{\"candidate\":") &&
            signal_string(&candidate, w->candidate, sizeof(w->candidate)) && signal_put(&candidate, ",\"sdpMid\":") &&
            signal_string(&candidate, w->mid, sizeof(w->mid)) && signal_put(&candidate, tail) &&
            (!count || signal_put(&body, ",")) && signal_string(&body, w->candidate_json, sizeof(w->candidate_json));
        x4_secure_clear(w->candidate, sizeof(w->candidate));
        x4_secure_clear(w->mid, sizeof(w->mid));
        x4_secure_clear(w->candidate_json, sizeof(w->candidate_json));
        if (!built) { end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "candidato ICE fuera de limite"); return false; }
        ++count;
    }
    if (!count) { x4_secure_clear(w->request, sizeof(w->request)); return true; }
    if (!signal_put(&body, "]}")) { end_with(end, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "solicitud ICE fuera de limite"); return false; }
    return signal_ack(w, "/ice", deadline, end);
}

/* Trim/a= normalization matches the reference protocol. The native peer
 * currently binds IPv4, so IPv6-literal candidates cannot be used. */
static bool signal_candidate_normalize(char *candidate)
{
    char *p = candidate;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) --n;
    p[n] = 0;
    if (!strncmp(p, "a=", 2)) p += 2;
    if (strncmp(p, "candidate:", 10)) return false;
    unsigned field = 0;
    const char *word = p;
    while (*word && field < 4) {
        while (*word && *word != ' ') ++word;
        while (*word == ' ') ++word;
        ++field;
    }
    if (field != 4 || !*word) return false;
    for (const char *address = word; *address && *address != ' '; ++address) if (*address == ':') return false;
    for (const unsigned char *c = (const unsigned char *)p; *c; ++c) if (*c < 0x20 || *c > 0x7e) return false;
    memmove(candidate, p, strlen(p) + 1);
    return true;
}

static bool signal_remote_item(X4XboxWork *w, X4JsonSpan item, SessionEnd *end)
{
    X4JsonSpan object = item, value;
    if (x4_json_type(item) == X4_JSON_T_STRING) {
        if (!signal_decode(item, w->candidate_object, sizeof(w->candidate_object), true) ||
            x4_json_parse(w->candidate_object, strlen(w->candidate_object), &object)) goto malformed;
    }
    if (x4_json_type(object) != X4_JSON_T_OBJECT || x4_json_member(object, "candidate", &value) != 1 ||
        !signal_decode(value, w->candidate, sizeof(w->candidate), false)) goto malformed;
    if (!signal_candidate_normalize(w->candidate)) return true;
    int member = x4_json_member(object, "sdpMid", &value);
    if (member < 0) goto malformed;
    if (!member || x4_json_type(value) == X4_JSON_T_NULL) copy_text(w->mid, sizeof(w->mid), "0");
    else if (!signal_decode(value, w->mid, sizeof(w->mid), false)) goto malformed;
    if (!w->mid[0]) copy_text(w->mid, sizeof(w->mid), "0");
    member = x4_json_member(object, "sdpMLineIndex", &value);
    if (member < 0) goto malformed;
    if (member && x4_json_type(value) != X4_JSON_T_NULL) {
        uint32_t index;
        if (x4_json_uint32(value, &index) || index > 64) goto malformed;
    }
    for (unsigned i = 0; i < w->remote_candidates; ++i)
        if (!strcmp(w->remote_seen[i].candidate, w->candidate) && !strcmp(w->remote_seen[i].mid, w->mid)) return true;
    if (w->remote_candidates == X4_SESSION_CANDIDATES_MAX) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, 0, "demasiados candidatos ICE remotos"); return false;
    }
    int rc = x4_rtc_add_remote_candidate(w->rtc, w->candidate, w->mid);
    if (rc < 0) { end_with(end, X4_SESSION_ERROR, rc, 0, "WebRTC rechazo un candidato ICE"); return false; }
    unsigned i = w->remote_candidates++;
    copy_text(w->remote_seen[i].candidate, sizeof(w->remote_seen[i].candidate), w->candidate);
    copy_text(w->remote_seen[i].mid, sizeof(w->remote_seen[i].mid), w->mid);
    return true;
malformed:
    end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, 0, "candidato ICE remoto no valido");
    return false;
}

static bool signal_remote_ice(X4XboxWork *w, uint64_t deadline, SessionEnd *end)
{
    size_t length = 0;
    int status = 0;
    if (!signal_call(w, "/ice", X4_SESSION_HTTP_GET, NULL, deadline, end, &length, &status)) return false;
    X4JsonSpan exchange, candidates, item;
    int result = signal_exchange(w, length, status, &exchange);
    if (result < 0) signal_error_diagnostic(w, length, status, "/ice");
    wipe_response(w, length);
    if (!result) return true;
    if (result < 0) {
        end_with(end, X4_SESSION_ERROR, result == -2 ? X4_AUTH_E_SESSION : X4_AUTH_E_RESPONSE, status,
            result == -2 ? "Xbox rechazo el intercambio ICE" : "respuesta ICE no valida");
        return false;
    }
    candidates = exchange;
    if (x4_json_type(exchange) == X4_JSON_T_OBJECT && x4_json_member(exchange, "candidates", &candidates) != 1)
        goto malformed;
    if (x4_json_type(candidates) != X4_JSON_T_ARRAY) goto malformed;
    size_t cursor = 0;
    unsigned count = 0;
    for (;;) {
        int found = x4_json_item(candidates, &cursor, &item);
        if (found < 0) goto malformed;
        if (!found) break;
        if (++count > X4_SESSION_CANDIDATES_MAX) goto malformed;
        if (!signal_remote_item(w, item, end)) return false;
    }
    x4_secure_clear(w->exchange, sizeof(w->exchange));
    x4_secure_clear(w->candidate_object, sizeof(w->candidate_object));
    x4_secure_clear(w->candidate, sizeof(w->candidate));
    x4_secure_clear(w->mid, sizeof(w->mid));
    return true;
malformed:
    end_with(end, X4_SESSION_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta ICE no valida");
    return false;
}

/* Publishes state from the actual peer only. Packet counters describe
 * authenticated RTP receipt; native decoding/rendering is a separate stage. */
static bool signal_rtc_view(X4XboxWork *w, SessionEnd *end, X4RtcSnapshot *view, uint64_t deadline)
{
    x4_rtc_snapshot(w->rtc, view);
    if (session_now() >= deadline) {
        end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, 0, "WebRTC no conecto a tiempo");
        return false;
    }
    w->session.rtc_connected = view->state == X4_RTC_CONNECTED;
    w->session.video_packets = view->video_packets;
    w->session.audio_packets = view->audio_packets;
    if (view->error || view->state == X4_RTC_FAILED || view->state == X4_RTC_CLOSED) {
        end_with(end, X4_SESSION_ERROR, view->error ? view->error : X4_AUTH_E_SESSION, 0, "fallo la conexion WebRTC");
        return false;
    }
    if (view->state == X4_RTC_CONNECTED) {
        enum X4SessionState state = view->video_packets || view->audio_packets ? X4_SESSION_STREAMING : X4_SESSION_CONNECTING;
        const char *stage = view->video_packets && view->audio_packets ? "recibiendo imagen y sonido de Xbox" :
            view->video_packets ? "recibiendo video de Xbox; esperando audio" :
            view->audio_packets ? "recibiendo audio de Xbox; esperando video" : "WebRTC conectado, esperando imagen y sonido";
        if (w->session.state != state || strcmp(w->session.stage, stage)) session_state(w, state, stage, 0);
    }
    return true;
}

static void session_stream(X4XboxWork *w, SessionEnd *end)
{
    int rc = 0;
    uint64_t deadline = session_now() + X4_SESSION_NEGOTIATE_USEC;
    if (!signal_provisioned(w, deadline, end)) return;
    deadline = session_now() + X4_SESSION_NEGOTIATE_USEC;
    w->next_keepalive = session_now();
    w->rtc = x4_rtc_open(&rc);
    if (!w->rtc) { end_with(end, X4_SESSION_ERROR, rc ? rc : X4_AUTH_E_SESSION, 0, "no se pudo iniciar WebRTC nativo"); return; }
    x4_rtc_set_media_callback(w->rtc, w->media_callback, w->media_user);
    if (!signal_sdp(w, deadline, end)) goto done;
    session_state(w, X4_SESSION_CONNECTING, "negociando ruta ICE con Xbox", seconds_until(deadline, session_now()));
    uint64_t next_ice = session_now(), connected_at = 0, last_keyframe = 0, disconnected_at = 0;
    for (;;) {
        if (signal_cancelled(w, end)) break;
        X4RtcSnapshot view;
        if (!signal_rtc_view(w, end, &view, deadline)) break;
        uint64_t t = session_now();
        if (view.state == X4_RTC_CONNECTED && !connected_at) {
            connected_at = t;
            deadline = UINT64_MAX;
            printf("XCloud4: conexion WebRTC establecida\n");
        }
        if (!connected_at && t >= deadline) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, 0, "WebRTC no conecto a tiempo"); break;
        }
        if (view.state == X4_RTC_DISCONNECTED) {
            if (!disconnected_at) disconnected_at = t;
            if (t - disconnected_at > 10 * X4_SESSION_USEC) {
                end_with(end, X4_SESSION_ERROR, X4_AUTH_E_SESSION, 0, "se perdio la conexion WebRTC"); break;
            }
        } else disconnected_at = 0;
        if (connected_at && !view.video_packets && !view.audio_packets && t - connected_at >= X4_SESSION_MEDIA_GRACE_USEC) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_DEADLINE, 0, "WebRTC conectado sin recibir imagen o sonido"); break;
        }
        if (connected_at && t - last_keyframe >= 500000) {
            bool requested = w->keyframe_requested && atomic_exchange(w->keyframe_requested, 0);
            if (!view.video_packets || requested) {
                x4_rtc_request_keyframe(w->rtc);
                last_keyframe = t;
            }
        }
        if (!signal_keepalive(w, deadline, end) || !signal_local_ice(w, deadline, end)) break;
        if (!connected_at && session_now() >= next_ice) {
            if (!signal_remote_ice(w, deadline, end)) break;
            next_ice = session_now() + X4_SESSION_ICE_POLL_USEC;
        }
        int waited = session_wait(w, session_now() + X4_SESSION_STEP_USEC, deadline);
        if (waited) {
            end_with(end, waited == 1 ? X4_SESSION_CANCELLED : X4_SESSION_ERROR,
                waited == 1 ? 0 : X4_AUTH_E_DEADLINE, 0, waited == 1 ? "sesion cancelada" : "WebRTC no conecto a tiempo"); break;
        }
    }
done:
    /* This joins transport callbacks before caller may release its media
     * receiver. It precedes remote DELETE and auth.finished publication. */
    x4_rtc_close(w->rtc);
    w->rtc = NULL;
    w->session.rtc_connected = 0;
    x4_secure_clear(w->sdp, sizeof(w->sdp));
    x4_secure_clear(w->exchange, sizeof(w->exchange));
    x4_secure_clear(w->candidate, sizeof(w->candidate));
    x4_secure_clear(w->mid, sizeof(w->mid));
    x4_secure_clear(w->candidate_json, sizeof(w->candidate_json));
    x4_secure_clear(w->candidate_object, sizeof(w->candidate_object));
    x4_secure_clear(w->remote_seen, sizeof(w->remote_seen));
}


/* One DELETE, deliberately not cancellable (cancel NULL): bounded by the
 * HTTP layer's 30 s deadline and native timeouts. 2xx, 404 and 410 confirm
 * the session is gone. Any other result sets cleanup_failed and turns the
 * outcome into ERROR, keeping the original error; a cancel never hides it. */
static void session_cleanup(X4XboxWork *w, const SessionEnd *end)
{
    size_t length = 0;
    int status = 0, rc;
    session_state(w, X4_SESSION_STOPPING, "cerrando sesion en Xbox", 0);
    if (session_url(w, w->session_id, "")) rc = session_call(w, X4_SESSION_HTTP_DELETE, NULL, NULL, &length, &status);
    else rc = X4_AUTH_E_FORM;
    wipe_response(w, length);
    x4_secure_clear(w->session_id, sizeof(w->session_id));
    x4_secure_clear(w->url, sizeof(w->url));
    w->session.cleanup_http_status = status;
    if (rc) {
        w->session.cleanup_failed = 1;
        w->session.cleanup_error = rc;
    } else if (!((status >= 200 && status <= 299) || status == 404 || status == 410)) {
        w->session.cleanup_failed = 1;
        w->session.cleanup_error = X4_AUTH_E_STATUS;
    }
    printf("XCloud4: sesion Xbox cierre http=%d error=0x%08x\n", status, (unsigned)w->session.cleanup_error);
    if (w->session.cleanup_failed)
        session_end(w, X4_SESSION_ERROR, end->error, end->status, "no se confirmo el cierre de la sesion");
    else session_end(w, end->state, end->error, end->status, end->stage);
}

static void session_run(X4XboxWork *w, const char *microsoft_token, const X4CatalogTitle *title, size_t index)
{
    if (user_token(w, microsoft_token) || xsts(w)) { session_auth_failed(w); return; }
    /* Exactly the offering the catalog came from; a refusal is final. */
    int rc = login(w, index);
    x4_secure_clear(w->xsts_token, sizeof(w->xsts_token));
    if (rc) { session_auth_failed(w); return; }
    copy_text(w->session.region, sizeof(w->session.region), w->result.region);
    int written = snprintf(w->authorization, sizeof(w->authorization), "Bearer %s", w->gs_token);
    x4_secure_clear(w->gs_token, sizeof(w->gs_token));
    if (written < 0 || (size_t)written >= sizeof(w->authorization)) {
        x4_secure_clear(w->authorization, sizeof(w->authorization));
        session_end(w, X4_SESSION_ERROR, X4_AUTH_E_FORM, 0, "credencial de region");
        return;
    }
    uint64_t deadline = session_now() + X4_SESSION_PROVISION_USEC;
    if (session_create(w, title) != 1) return;
    /* From here a validated path exists: every outcome ends in the DELETE. */
    SessionEnd end = {0};
    if (atomic_load(w->cancel)) end_with(&end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
    else if (session_provision(w, deadline, &end) && session_authorize(w, &end)) session_stream(w, &end);
    session_cleanup(w, &end);
}

const X4SessionSnapshot *x4_xbox_session(X4XboxWork *w, const char *microsoft_token,
    const X4CatalogTitle *title, const char *offering, const _Atomic int *cancel,
    X4SessionProgress progress, void *context, X4XboxPassport passport, void *passport_context,
    X4SessionMediaCallback media_callback, void *media_user,
    _Atomic int *keyframe_requested)
{
    memset(&w->result, 0, sizeof(w->result));
    memset(&w->session, 0, sizeof(w->session));
    w->result.state = X4_CATALOG_LOADING;
    w->session.state = X4_SESSION_STARTING;
    w->cancel = cancel;
    w->progress = NULL;
    w->session_progress = progress;
    w->context = context;
    w->passport_provider = passport;
    w->passport_context = passport_context;
    w->media_callback = media_callback;
    w->media_user = media_user;
    w->keyframe_requested = keyframe_requested;
    w->local_candidates = w->remote_candidates = 0;
    w->provisioned = false;
    w->session_start = session_now();
    size_t index = sizeof(offerings) / sizeof(offerings[0]);
    for (size_t i = 0; offering && i < sizeof(offerings) / sizeof(offerings[0]); ++i)
        if (!strcmp(offering, offerings[i].id)) index = i;
    if (index < sizeof(offerings) / sizeof(offerings[0]))
        copy_text(w->session.offering, sizeof(w->session.offering), offerings[index].id);
    size_t id = 0;
    if (title) {
        while (id < sizeof(title->id) && title->id[id]) ++id;
    }
    if (!title || id == sizeof(title->id) || !title_id(title->id) ||
        index == sizeof(offerings) / sizeof(offerings[0])) {
        session_end(w, X4_SESSION_ERROR, X4_AUTH_E_ARGUMENT, 0, "titulo u oferta no valida");
        return &w->session;
    }
    /* Refused before any request, so no remote session can exist. */
    if (!passport) {
        session_end(w, X4_SESSION_ERROR, X4_AUTH_E_ARGUMENT, 0, "autorizacion de conexion no disponible");
        return &w->session;
    }
    copy_text(w->session.title_name, sizeof(w->session.title_name), title->name[0] ? title->name : title->id);
    int rc = 0;
    step(w, "iniciando HTTPS");
    w->http = x4_http_open(&rc);
    if (!w->http) session_end(w, X4_SESSION_ERROR, rc, 0, "inicio HTTPS (red/TLS)");
    else session_run(w, microsoft_token, title, index);
    x4_http_close(w->http);
    w->http = NULL;
    x4_secure_clear(w->user_token, sizeof(w->user_token));
    x4_secure_clear(w->xsts_token, sizeof(w->xsts_token));
    x4_secure_clear(w->gs_token, sizeof(w->gs_token));
    x4_secure_clear(w->authorization, sizeof(w->authorization));
    x4_secure_clear(w->request, sizeof(w->request));
    x4_secure_clear(w->url, sizeof(w->url));
    x4_secure_clear(w->origin, sizeof(w->origin));
    x4_secure_clear(w->session_id, sizeof(w->session_id));
    x4_secure_clear(w->passport, sizeof(w->passport));
    w->passport_provider = NULL;
    w->passport_context = NULL;
    w->media_callback = NULL;
    w->media_user = NULL;
    w->keyframe_requested = NULL;
    memset(w->result.titles, 0, sizeof(w->result.titles));
    printf("XCloud4: sesion Xbox fin estado=%d http=%d error=0x%08x lista=%d autorizada=%d passport=%d "
        "connect=%d sdp=%d ice=%d rtc=%d video=%llu audio=%llu limpieza=%d/%d/0x%08x\n",
        (int)w->session.state, w->session.http_status, (unsigned)w->session.error, w->session.ready_seen,
        w->session.connection_authorized, w->session.passport_http_status, w->session.connect_http_status,
        w->session.sdp_http_status, w->session.ice_http_status, w->session.rtc_connected,
        (unsigned long long)w->session.video_packets, (unsigned long long)w->session.audio_packets,
        w->session.cleanup_failed, w->session.cleanup_http_status, (unsigned)w->session.cleanup_error);
    return &w->session;
}
