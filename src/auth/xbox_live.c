/* SPDX-License-Identifier: GPL-3.0-only */
#include "xbox_live.h"
#include "device_auth.h"
#include "http_client.h"
#include "json.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

/* Original XCloud4 implementation of the public Xbox Live exchanges. The
 * protocol steps were studied in GreenVita (see docs/CATALOGO_XBOX.md);
 * no code, client ID or credential was taken from it. */
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
    "\"clientAppVersion\":\"0.5.0\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\"," \
    "\"model\":\"PS4\"},\"os\":{\"name\":\"Orbis\",\"platform\":\"console\"}}}"

/* Cloud session preparation (milestone 0.5.0). Timings in monotonic usec. */
#define X4_SESSION_USEC 1000000ull
#define X4_SESSION_PROVISION_USEC (180ull * X4_SESSION_USEC)
#define X4_SESSION_READY_USEC (45ull * X4_SESSION_USEC)
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
    X4Http *http;
    const _Atomic int *cancel;
    X4XboxProgress progress;
    void *context;
    X4CatalogSnapshot result;
    /* Session runs only; progress above is NULL then. */
    X4SessionProgress session_progress;
    X4SessionSnapshot session;
    uint64_t session_start;
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

/* Cloud session preparation. Only the lifecycle up to "ready to negotiate"
 * exists here: no /connect passport, SDP, ICE, WebRTC or media. No keepalive
 * is sent: nothing shows a session needs one before /connect, and READY is
 * held at most 45 s before the session is deleted. Session path, ID, bearer
 * and bodies never leave the workspace, logs only carry literals and codes. */

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
        x4_secure_clear(state, sizeof(state));
        w->session.http_status = status;
        if (next_state == X4_SESSION_ERROR) {
            end_with(end, X4_SESSION_ERROR, X4_AUTH_E_SESSION, status, stage);
            return false;
        }
        if (next_state == X4_SESSION_READY) {
            /* Server-side readiness only; nothing is connected or rendered. */
            w->session.ready_seen = 1;
            session_state(w, X4_SESSION_READY, stage, (unsigned)(X4_SESSION_READY_USEC / X4_SESSION_USEC));
            return true;
        }
        if (strcmp(w->session.stage, stage)) session_state(w, X4_SESSION_WAITING, stage, seconds_until(deadline, done));
        next = done + X4_SESSION_POLL_USEC;
    }
}

/* READY is held at most 45 s so no ready session is left idle on the
 * server; without a connect step there is nothing else to do with it. */
static void session_hold(X4XboxWork *w, SessionEnd *end)
{
    if (session_wait(w, UINT64_MAX, session_now() + X4_SESSION_READY_USEC) == 1)
        end_with(end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
    else end_with(end, X4_SESSION_CLOSED, 0, 0, "sesion cerrada: conexion de video pendiente");
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
    SessionEnd end;
    if (atomic_load(w->cancel)) end_with(&end, X4_SESSION_CANCELLED, 0, 0, "sesion cancelada");
    else if (session_provision(w, deadline, &end)) session_hold(w, &end);
    session_cleanup(w, &end);
}

const X4SessionSnapshot *x4_xbox_session(X4XboxWork *w, const char *microsoft_token,
    const X4CatalogTitle *title, const char *offering, const _Atomic int *cancel,
    X4SessionProgress progress, void *context)
{
    memset(&w->result, 0, sizeof(w->result));
    memset(&w->session, 0, sizeof(w->session));
    w->result.state = X4_CATALOG_LOADING;
    w->session.state = X4_SESSION_STARTING;
    w->cancel = cancel;
    w->progress = NULL;
    w->session_progress = progress;
    w->context = context;
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
    memset(w->result.titles, 0, sizeof(w->result.titles));
    printf("XCloud4: sesion Xbox fin estado=%d http=%d error=0x%08x lista=%d limpieza=%d/%d/0x%08x\n",
        (int)w->session.state, w->session.http_status, (unsigned)w->session.error, w->session.ready_seen,
        w->session.cleanup_failed, w->session.cleanup_http_status, (unsigned)w->session.cleanup_error);
    return &w->session;
}
