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
    "\"clientAppVersion\":\"0.4.0\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\"," \
    "\"model\":\"PS4\"},\"os\":{\"name\":\"Orbis\",\"platform\":\"console\"}}}"

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
    X4Http *http;
    const _Atomic int *cancel;
    X4XboxProgress progress;
    void *context;
    X4CatalogSnapshot result;
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

static void step(X4XboxWork *w, const char *stage)
{
    copy_text(w->result.stage, sizeof(w->result.stage), stage);
    printf("XCloud4: Xbox etapa: %s\n", stage);
    if (w->progress) w->progress(w->context, stage, w->result.offering, w->result.region);
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
    int written = snprintf(w->url, sizeof(w->url), "https://%s/v2/titles", host);
    return written > 0 && (size_t)written < sizeof(w->url);
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
