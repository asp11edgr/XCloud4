/* SPDX-License-Identifier: GPL-3.0-only */
#include "http_client.h"
#include "../core/module.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

#define X4_NET_POOL_SIZE (256 * 1024)
#define X4_SSL_POOL_SIZE (512 * 1024)
#define X4_HTTP_POOL_SIZE (512 * 1024)
#define X4_HTTP_DEADLINE_USEC (30ull * 1000 * 1000)
#define X4_HTTP_TIMEOUT_USEC (5u * 1000 * 1000)
#define X4_HTTP_CHUNK 4096u
#define X4_HTTP_URL_MAX 256
#define X4_HTTP_FORM_MAX 8192
#define X4_HTTP_VERSION_1_1 2
#define X4_HTTP_METHOD_GET 0
#define X4_HTTP_METHOD_POST 1
#define X4_HTTP_METHOD_DELETE 5
#define X4_HTTP_HEADER_OVERWRITE 0u
/* SERVER_VERIFY | CN_CHECK | NOT_AFTER | NOT_BEFORE | KNOWN_CA | SNI */
#define X4_HTTPS_VERIFY_ALL 0xBDu

static const char allowed_prefix[] = "https://login.microsoftonline.com/consumers/";

/* Xbox Live / Store destinations for x4_http_json_request. */
#define X4_HTTP_JSON_URL_MAX 512
#define X4_HTTP_STORE_IDS_MAX 8u
#define X4_HTTP_STORE_ID_MAX 31u
#define X4_HTTP_REGION_LABELS_MAX 4u
static const char xbox_user_url[] = "https://user.auth.xboxlive.com/user/authenticate";
static const char xsts_url[] = "https://xsts.auth.xboxlive.com/xsts/authorize";
static const char *const login_urls[] = {
    "https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user",
    "https://xgpuwebf2p.gssv-play-prod.xboxlive.com/v2/login/user",
};
static const char https_scheme[] = "https://";
static const char gssv_suffix[] = ".gssv-play-prod.xboxlive.com";
static const char titles_path[] = "/v2/titles";
static const char store_prefix[] = "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=";
static const char store_suffix[] = "&market=MX&languages=es-MX&fieldsTemplate=Details";
static const char header_contract[] = "x-xbl-contract-version";
static const char header_client[] = "x-gssv-client";
static const char header_device[] = "X-MS-Device-Info";
static const char header_authorization[] = "Authorization";
static const char sessions_path[] = "/v5/sessions/cloud/";
#define X4_HTTP_SESSION_ID_MAX 128u
#define X4_HTTP_SESSION_HEADERS 3u

/* DEST_SESSION is produced only by classify_session, never by classify. */
enum Destination { DEST_NONE, DEST_XBOX_USER, DEST_XSTS, DEST_LOGIN, DEST_REGION, DEST_STORE, DEST_SESSION };
enum SessionRoute { ROUTE_NONE, ROUTE_PLAY, ROUTE_STATE, ROUTE_KEEPALIVE, ROUTE_RESOURCE };

/* Request inputs copied into one block. It must outlive the native request,
 * connection and template, so it is freed only after they were deleted. */
typedef struct {
    int method; /* native GET 0, POST 1, DELETE 5 */
    const char *url, *payload, *content_type;
    size_t payload_size, header_count, size;
    X4HttpHeader headers[X4_HTTP_HEADERS_MAX];
    char text[];
} Owned;
/* This application owns one sequential networking worker. Net stays global;
 * subsequent operations reuse the successful initialization we performed. */
static bool net_initialized;

struct X4Http {
    int net_module, ssl_module, http_module;
    int32_t pool, ssl, http;
    bool unusable, quarantined;
    atomic_bool busy;
    /* Written by the worker, read by the UI; always a string literal. */
    _Atomic(const char *) stage;
    struct {
        int32_t (*net_init)(void);
        int32_t (*pool_create)(const char *, int32_t, int32_t);
        int32_t (*pool_destroy)(int32_t);
        int32_t (*ssl_init)(size_t);
        int32_t (*ssl_term)(int32_t);
        int32_t (*http_init)(int32_t, int32_t, size_t);
        int32_t (*http_term)(int32_t);
        int32_t (*create_template)(int32_t, const char *, int32_t, int32_t);
        int32_t (*create_connection)(int32_t, const char *, bool);
        int32_t (*create_request)(int32_t, int32_t, const char *, uint64_t);
        int32_t (*delete_template)(int32_t);
        int32_t (*delete_connection)(int32_t);
        int32_t (*delete_request)(int32_t);
        int32_t (*resolve_timeout)(int32_t, uint32_t);
        int32_t (*connect_timeout)(int32_t, uint32_t);
        int32_t (*send_timeout)(int32_t, uint32_t);
        int32_t (*recv_timeout)(int32_t, uint32_t);
        int32_t (*auto_redirect)(int32_t, int32_t);
        int32_t (*cookie_enabled)(int32_t, int32_t);
        int32_t (*add_header)(int32_t, const char *, const char *, uint32_t);
        int32_t (*https_enable)(int32_t, uint32_t);
        int32_t (*send)(int32_t, const void *, size_t);
        int32_t (*read)(int32_t, void *, size_t);
        int32_t (*status)(int32_t, int32_t *);
    } fn;
};

void x4_secure_clear(void *data, size_t size)
{
    volatile unsigned char *p = data;
    if (!p) return;
    while (size--) *p++ = 0;
    atomic_signal_fence(memory_order_seq_cst);
}

const char *x4_http_stage(const X4Http *h)
{
    return h ? h->stage : "sin contexto HTTPS";
}

/* Children before parents; stop at the first failure so nothing still
 * referenced by a live child is released. Never calls NetTerm. */
static void terminate_contexts(X4Http *h)
{
    int rc;
    if (h->http >= 0) {
        rc = h->fn.http_term(h->http);
        if (rc != 0) { printf("XCloud4: HttpTerm 0x%08x; Ssl/Net retenidos\n", (unsigned)rc); return; }
        h->http = -1;
    }
    if (h->ssl >= 0) {
        rc = h->fn.ssl_term(h->ssl);
        if (rc != 0) { printf("XCloud4: SslTerm 0x%08x; pool Net retenido\n", (unsigned)rc); return; }
        h->ssl = -1;
    }
    if (h->pool >= 0) {
        rc = h->fn.pool_destroy(h->pool);
        if (rc != 0) { printf("XCloud4: NetPoolDestroy 0x%08x\n", (unsigned)rc); return; }
        h->pool = -1;
    }
}

X4Http *x4_http_open(int *error)
{
    X4Http *h = malloc(sizeof(*h));
    if (!h) {
        printf("XCloud4: HTTPS sin memoria\n");
        if (error) *error = X4_HTTP_ALLOCATION;
        return NULL;
    }
    /* Compound literal: every function pointer starts as a real NULL. */
    *h = (X4Http){
        .net_module = -1, .ssl_module = -1, .http_module = -1,
        .pool = -1, .ssl = -1, .http = -1, .stage = "modulos de red",
    };
    atomic_init(&h->busy, false);
    int rc;
#define RESOLVE(module, name, field) do { \
        void *address_ = NULL; \
        rc = x4_module_symbol(module, name, &address_); \
        if (rc < 0) goto fail; \
        h->fn.field = (typeof(h->fn.field))address_; \
    } while (0)
    rc = h->net_module = x4_module_open("libSceNet");
    if (rc < 0) goto fail;
    RESOLVE(h->net_module, "sceNetInit", net_init);
    RESOLVE(h->net_module, "sceNetPoolCreate", pool_create);
    RESOLVE(h->net_module, "sceNetPoolDestroy", pool_destroy);
    rc = h->ssl_module = x4_module_open("libSceSsl");
    if (rc < 0) goto fail;
    RESOLVE(h->ssl_module, "sceSslInit", ssl_init);
    RESOLVE(h->ssl_module, "sceSslTerm", ssl_term);
    rc = h->http_module = x4_module_open("libSceHttp");
    if (rc < 0) goto fail;
    RESOLVE(h->http_module, "sceHttpInit", http_init);
    RESOLVE(h->http_module, "sceHttpTerm", http_term);
    RESOLVE(h->http_module, "sceHttpCreateTemplate", create_template);
    RESOLVE(h->http_module, "sceHttpCreateConnectionWithURL", create_connection);
    RESOLVE(h->http_module, "sceHttpCreateRequestWithURL", create_request);
    RESOLVE(h->http_module, "sceHttpDeleteTemplate", delete_template);
    RESOLVE(h->http_module, "sceHttpDeleteConnection", delete_connection);
    RESOLVE(h->http_module, "sceHttpDeleteRequest", delete_request);
    RESOLVE(h->http_module, "sceHttpSetResolveTimeOut", resolve_timeout);
    RESOLVE(h->http_module, "sceHttpSetConnectTimeOut", connect_timeout);
    RESOLVE(h->http_module, "sceHttpSetSendTimeOut", send_timeout);
    RESOLVE(h->http_module, "sceHttpSetRecvTimeOut", recv_timeout);
    RESOLVE(h->http_module, "sceHttpSetAutoRedirect", auto_redirect);
    RESOLVE(h->http_module, "sceHttpSetCookieEnabled", cookie_enabled);
    RESOLVE(h->http_module, "sceHttpAddRequestHeader", add_header);
    RESOLVE(h->http_module, "sceHttpsEnableOption", https_enable);
    RESOLVE(h->http_module, "sceHttpSendRequest", send);
    RESOLVE(h->http_module, "sceHttpReadData", read);
    RESOLVE(h->http_module, "sceHttpGetStatusCode", status);
#undef RESOLVE

    h->stage = "NetInit";
    if (!net_initialized) {
        rc = h->fn.net_init();
        printf("XCloud4: NetInit -> 0x%08x\n", (unsigned)rc);
        if (rc < 0) goto fail;
        net_initialized = true;
    }
    h->stage = "pool de red";
    rc = h->fn.pool_create("x4-http-net", X4_NET_POOL_SIZE, 0);
    if (rc < 0) goto fail;
    h->pool = rc;
    h->stage = "SslInit";
    rc = h->fn.ssl_init(X4_SSL_POOL_SIZE);
    if (rc < 0) goto fail;
    h->ssl = rc;
    h->stage = "HttpInit";
    rc = h->fn.http_init(h->pool, h->ssl, X4_HTTP_POOL_SIZE);
    if (rc < 0) goto fail;
    h->http = rc;
    h->stage = "HTTPS listo";
    printf("XCloud4: HTTPS listo (Net/Ssl/Http)\n");
    if (error) *error = 0;
    return h;
fail:
    printf("XCloud4: HTTPS %s fallo 0x%08x\n", h->stage, (unsigned)rc);
    terminate_contexts(h);
    x4_secure_clear(h, sizeof(*h));
    free(h);
    if (error) *error = rc;
    return NULL;
}

void x4_http_close(X4Http *h)
{
    if (!h) return;
    if (h->quarantined) printf("XCloud4: HTTPS cierre omitido; recursos en cuarentena\n");
    else terminate_contexts(h);
    x4_secure_clear(h, sizeof(*h));
    free(h);
}

/* strnlen is POSIX and hidden under strict -std=c23. */
static size_t bounded_length(const char *text, size_t limit)
{
    size_t n = 0;
    while (n < limit && text[n]) ++n;
    return n;
}

static bool unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~';
}

/* Exact scheme/host prefix (no userinfo, port, http), then non-empty path
 * segments of unreserved characters only: no dot segments, escapes,
 * whitespace, query or fragment. */
static int check_url(const char *url)
{
    size_t prefix = sizeof(allowed_prefix) - 1;
    size_t size = bounded_length(url, X4_HTTP_URL_MAX + 1);
    if (size > X4_HTTP_URL_MAX || size <= prefix || memcmp(url, allowed_prefix, prefix)) return X4_HTTP_URL;
    const char *segment = url + prefix;
    for (const char *p = segment;; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || c == 0) {
            size_t n = (size_t)(p - segment);
            if (n == 0) return X4_HTTP_URL;
            if (segment[0] == '.' && (n == 1 || (n == 2 && segment[1] == '.'))) return X4_HTTP_URL;
            if (c == 0) return 0;
            segment = p + 1;
        } else if (!unreserved(c)) {
            return X4_HTTP_URL;
        }
    }
}

/* Encoded forms are visible ASCII only; this also rules out CR/LF. */
static int check_form(const char *form, size_t *size)
{
    size_t n = bounded_length(form, X4_HTTP_FORM_MAX + 1);
    if (n > X4_HTTP_FORM_MAX) return X4_HTTP_BOUND;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)form[i];
        if (c < 0x21 || c > 0x7e) return X4_HTTP_ARGUMENT;
    }
    *size = n;
    return 0;
}

static bool alphanumeric(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* One to four lowercase DNS labels in front of the fixed GSSV suffix. Only
 * [a-z0-9-] is accepted, which also rules out userinfo, ports and paths. */
static bool region_host(const char *host, size_t n)
{
    size_t suffix = sizeof(gssv_suffix) - 1;
    if (n <= suffix || n > 253 || memcmp(host + n - suffix, gssv_suffix, suffix)) return false;
    size_t labels = 0, length = 0, end = n - suffix;
    for (size_t i = 0; i <= end; ++i) {
        unsigned char c = i < end ? (unsigned char)host[i] : '.';
        if (c == '.') {
            if (length == 0 || length > 63 || host[i - 1] == '-' || host[i - length] == '-') return false;
            if (++labels > X4_HTTP_REGION_LABELS_MAX) return false;
            length = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            ++length;
        } else return false;
    }
    return true;
}

/* Fixed prefix and suffix around 1..8 comma separated alphanumeric IDs. */
static bool store_url(const char *url, size_t n)
{
    size_t prefix = sizeof(store_prefix) - 1, suffix = sizeof(store_suffix) - 1;
    if (n <= prefix + suffix || memcmp(url, store_prefix, prefix) || strcmp(url + n - suffix, store_suffix))
        return false;
    size_t ids = 0, length = 0, end = n - suffix;
    for (size_t i = prefix; i <= end; ++i) {
        unsigned char c = i < end ? (unsigned char)url[i] : ',';
        if (c == ',') {
            if (length == 0 || length > X4_HTTP_STORE_ID_MAX || ++ids > X4_HTTP_STORE_IDS_MAX) return false;
            length = 0;
        } else if (alphanumeric(c)) {
            ++length;
        } else return false;
    }
    return true;
}

static enum Destination classify(const char *url)
{
    size_t n = bounded_length(url, X4_HTTP_JSON_URL_MAX + 1);
    if (n > X4_HTTP_JSON_URL_MAX) return DEST_NONE;
    if (!strcmp(url, xbox_user_url)) return DEST_XBOX_USER;
    if (!strcmp(url, xsts_url)) return DEST_XSTS;
    for (size_t i = 0; i < sizeof(login_urls) / sizeof(login_urls[0]); ++i)
        if (!strcmp(url, login_urls[i])) return DEST_LOGIN;
    size_t scheme = sizeof(https_scheme) - 1, path = sizeof(titles_path) - 1;
    if (n > scheme + path && !memcmp(url, https_scheme, scheme) && !strcmp(url + n - path, titles_path) &&
        region_host(url + scheme, n - scheme - path))
        return DEST_REGION;
    if (store_url(url, n)) return DEST_STORE;
    return DEST_NONE;
}

/* ASCII case-insensitive comparison of n bytes against a lowercase word. */
static bool same_word(const char *text, size_t n, const char *word)
{
    size_t i = 0;
    for (; i < n && word[i]; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != word[i]) return false;
    }
    return i == n && !word[i];
}

/* 1..128 of [A-Za-z0-9_-]; GUIDs fit. Collection names are refused so an ID
 * can never address /play or /active. */
static bool session_id(const char *id, size_t n)
{
    if (n == 0 || n > X4_HTTP_SESSION_ID_MAX) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!alphanumeric(c) && c != '-' && c != '_') return false;
    }
    return !same_word(id, n, "play") && !same_word(id, n, "active");
}

/* https://<region host>/v5/sessions/cloud/{play | <id> | <id>/state |
 * <id>/keepalive}, nothing more. The host passes the same label checks as
 * the /v2/titles region. */
static enum SessionRoute classify_session(const char *url)
{
    size_t n = bounded_length(url, X4_HTTP_JSON_URL_MAX + 1);
    size_t scheme = sizeof(https_scheme) - 1, prefix = sizeof(sessions_path) - 1;
    if (n > X4_HTTP_JSON_URL_MAX || n <= scheme || memcmp(url, https_scheme, scheme)) return ROUTE_NONE;
    const char *host = url + scheme;
    size_t h = 0;
    while (host[h] && host[h] != '/') ++h;
    if (!region_host(host, h)) return ROUTE_NONE;
    const char *path = host + h;
    if (strncmp(path, sessions_path, prefix)) return ROUTE_NONE;
    const char *rest = path + prefix;
    if (!strcmp(rest, "play")) return ROUTE_PLAY;
    size_t id = 0;
    while (rest[id] && rest[id] != '/') ++id;
    if (!session_id(rest, id)) return ROUTE_NONE;
    const char *tail = rest + id;
    if (!tail[0]) return ROUTE_RESOURCE;
    if (!strcmp(tail, "/state")) return ROUTE_STATE;
    if (!strcmp(tail, "/keepalive")) return ROUTE_KEEPALIVE;
    return ROUTE_NONE;
}

/* RFC 7235 token68: the only credential syntax ever sent. */
static bool token68(const char *t)
{
    size_t i = 0;
    while (alphanumeric((unsigned char)t[i]) || t[i] == '-' || t[i] == '.' || t[i] == '_' ||
        t[i] == '~' || t[i] == '+' || t[i] == '/') ++i;
    if (i == 0) return false;
    while (t[i] == '=') ++i;
    return t[i] == 0;
}

/* Bounded printable values without control bytes, then a per-destination
 * allowlist; a name may appear once. */
static int check_headers(enum Destination d, const X4HttpHeader *headers, size_t count)
{
    if (count > X4_HTTP_HEADERS_MAX || (count && !headers)) return X4_HTTP_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        const char *name = headers[i].name, *value = headers[i].value;
        if (!name || !value) return X4_HTTP_ARGUMENT;
        size_t n = bounded_length(value, X4_HTTP_HEADER_VALUE_MAX + 1);
        if (n == 0 || n > X4_HTTP_HEADER_VALUE_MAX || value[0] == ' ' || value[n - 1] == ' ') return X4_HTTP_BOUND;
        for (size_t k = 0; k < n; ++k) {
            unsigned char c = (unsigned char)value[k];
            if (c < 0x20 || c > 0x7e) return X4_HTTP_ARGUMENT;
        }
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(headers[j].name, name)) return X4_HTTP_ARGUMENT;
        bool allowed = false;
        if (!strcmp(name, header_contract)) allowed = d == DEST_XBOX_USER || d == DEST_XSTS;
        else if (!strcmp(name, header_client) || !strcmp(name, header_device))
            allowed = d == DEST_LOGIN || d == DEST_REGION || d == DEST_SESSION;
        else if (!strcmp(name, header_authorization))
            allowed = (d == DEST_REGION || d == DEST_SESSION) && !strncmp(value, "Bearer ", 7) &&
                token68(value + 7);
        if (!allowed) return X4_HTTP_ARGUMENT;
    }
    return 0;
}

/* Generated JSON is printable ASCII; this rules out CR/LF and NUL. */
static int check_json(const char *json, size_t *size)
{
    size_t n = bounded_length(json, X4_HTTP_JSON_BODY_MAX + 1);
    if (n == 0 || n > X4_HTTP_JSON_BODY_MAX) return X4_HTTP_BOUND;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)json[i];
        if (c < 0x20 || c > 0x7e) return X4_HTTP_ARGUMENT;
    }
    *size = n;
    return 0;
}

/* Inputs were validated, so every length here is already bounded. */
static Owned *own(int method, const char *url, const char *payload, size_t payload_size,
    const char *content_type, const X4HttpHeader *headers, size_t count)
{
    size_t url_size = strlen(url) + 1;
    size_t size = url_size + (payload ? payload_size + 1 : 0);
    for (size_t i = 0; i < count; ++i) size += strlen(headers[i].name) + strlen(headers[i].value) + 2;
    Owned *o = malloc(sizeof(*o) + size);
    if (!o) return NULL;
    memset(o, 0, sizeof(*o));
    o->method = method;
    o->size = size;
    o->content_type = content_type;
    o->payload_size = payload_size;
    o->header_count = count;
    char *p = o->text;
    memcpy(p, url, url_size);
    o->url = p;
    p += url_size;
    if (payload) {
        memcpy(p, payload, payload_size);
        p[payload_size] = 0;
        o->payload = p;
        p += payload_size + 1;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t n = strlen(headers[i].name) + 1;
        memcpy(p, headers[i].name, n);
        o->headers[i].name = p;
        p += n;
        n = strlen(headers[i].value) + 1;
        memcpy(p, headers[i].value, n);
        o->headers[i].value = p;
        p += n;
    }
    return o;
}

static int checkpoint(const _Atomic int *cancel, uint64_t deadline)
{
    if (cancel && atomic_load(cancel)) return X4_HTTP_CANCELLED;
    if (sceKernelGetProcessTime() >= deadline) return X4_HTTP_DEADLINE;
    return 0;
}

/* Delete request, connection, template. If one delete fails its parents stay
 * alive (quarantined) and the context refuses further requests. */
static bool release_request(X4Http *h, int32_t *tmpl, int32_t *conn, int32_t *req)
{
    int rc = 0;
    if (*req >= 0) {
        rc = h->fn.delete_request(*req);
        if (rc != 0) goto quarantine;
        *req = -1;
    }
    if (*conn >= 0) {
        rc = h->fn.delete_connection(*conn);
        if (rc != 0) goto quarantine;
        *conn = -1;
    }
    if (*tmpl >= 0) {
        rc = h->fn.delete_template(*tmpl);
        if (rc != 0) goto quarantine;
        *tmpl = -1;
    }
    return true;
quarantine:
    printf("XCloud4: HTTP liberar 0x%08x; cuarentena plantilla=%d conexion=%d solicitud=%d\n",
        (unsigned)rc, (int)*tmpl, (int)*conn, (int)*req);
    h->unusable = true;
    h->quarantined = true;
    return false;
}

/* Runs one validated exchange from owned inputs, which it releases. */
static int transfer(X4Http *h, Owned *o, char *body, size_t capacity, size_t *length, int *status,
    const _Atomic int *cancel)
{
    const char *url = o->url, *form = o->payload;
    size_t form_size = o->payload_size;
    int rc;
    uint64_t deadline = sceKernelGetProcessTime() + X4_HTTP_DEADLINE_USEC;
    int32_t tmpl = -1, conn = -1, req = -1, code = 0;
    size_t total = 0;
#define CHECKPOINT() do { rc = checkpoint(cancel, deadline); if (rc) goto done; } while (0)
#define CREATE(text, target, expr) do { \
        h->stage = text; CHECKPOINT(); \
        rc = (expr); if (rc < 0) goto done; \
        target = rc; \
    } while (0)
#define OPTION(text, expr) do { \
        h->stage = text; rc = (expr); \
        if (rc != 0) { if (rc > 0) rc = X4_HTTP_PROTOCOL; goto done; } \
    } while (0)
    CREATE("plantilla HTTPS", tmpl, h->fn.create_template(h->http, "XCloud4", X4_HTTP_VERSION_1_1, 0));
    OPTION("tiempo de resolucion", h->fn.resolve_timeout(tmpl, X4_HTTP_TIMEOUT_USEC));
    OPTION("tiempo de conexion", h->fn.connect_timeout(tmpl, X4_HTTP_TIMEOUT_USEC));
    OPTION("tiempo de envio", h->fn.send_timeout(tmpl, X4_HTTP_TIMEOUT_USEC));
    OPTION("tiempo de recepcion", h->fn.recv_timeout(tmpl, X4_HTTP_TIMEOUT_USEC));
    /* Redirects off: codes and tokens must never follow a Location header. */
    OPTION("redirecciones", h->fn.auto_redirect(tmpl, 0));
    OPTION("cookies", h->fn.cookie_enabled(tmpl, 0));
    /* Full certificate verification; failures surface, never bypassed. */
    OPTION("verificacion TLS", h->fn.https_enable(tmpl, X4_HTTPS_VERIFY_ALL));
    CREATE("conexion HTTPS", conn, h->fn.create_connection(tmpl, url, false));
    /* Method chosen explicitly by the caller and copied with the inputs. */
    CREATE("solicitud HTTPS", req, h->fn.create_request(conn, o->method, url, (uint64_t)form_size));
    OPTION("cabeceras", h->fn.add_header(req, "Accept", "application/json", X4_HTTP_HEADER_OVERWRITE));
    OPTION("cabeceras", h->fn.add_header(req, "Accept-Encoding", "identity", X4_HTTP_HEADER_OVERWRITE));
    if (o->content_type) OPTION("cabeceras", h->fn.add_header(req, "Content-Type",
        o->content_type, X4_HTTP_HEADER_OVERWRITE));
    for (size_t i = 0; i < o->header_count; ++i)
        OPTION("cabeceras", h->fn.add_header(req, o->headers[i].name, o->headers[i].value,
            X4_HTTP_HEADER_OVERWRITE));

    h->stage = "envio HTTPS/TLS";
    CHECKPOINT();
    rc = h->fn.send(req, form, form_size);
    if (rc != 0) { if (rc > 0) rc = X4_HTTP_PROTOCOL; goto done; }
    CHECKPOINT();
    OPTION("codigo de estado", h->fn.status(req, &code));
    if (code < 100 || code > 599) { rc = X4_HTTP_PROTOCOL; goto done; }

    h->stage = "lectura de respuesta";
    for (;;) {
        CHECKPOINT();
        size_t room = capacity - 1 - total;
        unsigned char probe = 0;
        /* A full buffer is accepted only if one more read proves EOF. */
        void *target = room ? (void *)(body + total) : (void *)&probe;
        uint32_t ask = room == 0 ? 1u : room < X4_HTTP_CHUNK ? (uint32_t)room : X4_HTTP_CHUNK;
        rc = h->fn.read(req, target, ask);
        x4_secure_clear(&probe, sizeof(probe));
        if (rc < 0) goto done;
        int got = rc;
        /* EOF counts only if cancel and deadline still hold afterwards. */
        CHECKPOINT();
        if (got == 0) break;
        if ((uint32_t)got > ask) { rc = X4_HTTP_PROTOCOL; goto done; }
        if (room == 0) { h->stage = "respuesta excede bufer"; rc = X4_HTTP_BOUND; goto done; }
        total += (size_t)got;
    }
    rc = 0;
#undef OPTION
#undef CREATE
#undef CHECKPOINT
done:
    if (rc == X4_HTTP_CANCELLED) h->stage = "cancelado";
    else if (rc == X4_HTTP_DEADLINE) h->stage = "plazo agotado";
    if (rc != 0) {
        printf("XCloud4: HTTPS %s -> 0x%08x\n", h->stage, (unsigned)rc);
        x4_secure_clear(body, capacity);
    } else {
        body[total] = 0;
        *length = total;
        *status = code;
        h->stage = "respuesta recibida";
        printf("XCloud4: HTTPS estado %d, %zu bytes\n", (int)code, total);
    }
    /* In quarantine the owned inputs stay allocated until process exit. */
    if (release_request(h, &tmpl, &conn, &req)) {
        size_t owned = sizeof(*o) + o->size;
        x4_secure_clear(o, owned);
        free(o);
    } else if (rc == 0) h->stage = "recursos HTTP en cuarentena";
    return rc;
}

static int perform(X4Http *h, const char *url, const char *form, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    h->stage = "argumentos HTTPS";
    if (!url || !body || capacity == 0 || !length || !status) return X4_HTTP_ARGUMENT;
    body[0] = 0;
    *length = 0;
    *status = 0;
    if (h->unusable) { h->stage = "HTTPS inutilizable"; return X4_HTTP_UNUSABLE; }
    h->stage = "URL rechazada";
    int rc = check_url(url);
    if (rc) return rc;
    size_t form_size = 0;
    if (form) {
        h->stage = "formulario rechazado";
        rc = check_form(form, &form_size);
        if (rc) return rc;
    }
    Owned *o = own(form ? X4_HTTP_METHOD_POST : X4_HTTP_METHOD_GET, url, form, form_size,
        form ? "application/x-www-form-urlencoded" : NULL, NULL, 0);
    if (!o) { h->stage = "memoria de solicitud"; return X4_HTTP_ALLOCATION; }
    return transfer(h, o, body, capacity, length, status, cancel);
}

static int perform_json(X4Http *h, const char *url, const X4HttpHeader *headers, size_t count,
    const char *json, char *body, size_t capacity, size_t *length, int *status, const _Atomic int *cancel)
{
    h->stage = "argumentos HTTPS";
    if (!url || !body || capacity == 0 || !length || !status) return X4_HTTP_ARGUMENT;
    body[0] = 0;
    *length = 0;
    *status = 0;
    if (capacity > X4_HTTP_RESPONSE_MAX + 1) return X4_HTTP_ARGUMENT;
    if (h->unusable) { h->stage = "HTTPS inutilizable"; return X4_HTTP_UNUSABLE; }
    h->stage = "URL rechazada";
    enum Destination d = classify(url);
    if (d == DEST_NONE) return X4_HTTP_URL;
    bool post = d == DEST_XBOX_USER || d == DEST_XSTS || d == DEST_LOGIN;
    h->stage = "metodo rechazado";
    if (post != (json != NULL)) return X4_HTTP_ARGUMENT;
    size_t json_size = 0;
    int rc;
    if (json) {
        h->stage = "cuerpo JSON rechazado";
        rc = check_json(json, &json_size);
        if (rc) return rc;
    }
    h->stage = "cabecera rechazada";
    rc = check_headers(d, headers, count);
    if (rc) return rc;
    Owned *o = own(post ? X4_HTTP_METHOD_POST : X4_HTTP_METHOD_GET, url, json, json_size,
        json ? "application/json" : NULL, headers, count);
    if (!o) { h->stage = "memoria de solicitud"; return X4_HTTP_ALLOCATION; }
    return transfer(h, o, body, capacity, length, status, cancel);
}

static int perform_session(X4Http *h, enum X4HttpSessionMethod method, const char *url,
    const X4HttpHeader *headers, size_t count, const char *json, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    h->stage = "argumentos HTTPS";
    if (!url || !body || capacity == 0 || !length || !status) return X4_HTTP_ARGUMENT;
    body[0] = 0;
    *length = 0;
    *status = 0;
    if (capacity > X4_HTTP_RESPONSE_MAX + 1) return X4_HTTP_ARGUMENT;
    if (h->unusable) { h->stage = "HTTPS inutilizable"; return X4_HTTP_UNUSABLE; }
    h->stage = "URL rechazada";
    enum SessionRoute route = classify_session(url);
    if (route == ROUTE_NONE) return X4_HTTP_URL;
    /* One method per route; the body rule is part of the allowlist. */
    h->stage = "metodo rechazado";
    bool allowed = false;
    int native = X4_HTTP_METHOD_GET;
    switch (route) {
    case ROUTE_PLAY:
        allowed = method == X4_SESSION_HTTP_POST && json && json[0];
        native = X4_HTTP_METHOD_POST;
        break;
    case ROUTE_KEEPALIVE:
        allowed = method == X4_SESSION_HTTP_POST && json && !json[0];
        native = X4_HTTP_METHOD_POST;
        break;
    case ROUTE_STATE:
        allowed = method == X4_SESSION_HTTP_GET && !json;
        native = X4_HTTP_METHOD_GET;
        break;
    case ROUTE_RESOURCE:
        allowed = method == X4_SESSION_HTTP_DELETE && !json;
        native = X4_HTTP_METHOD_DELETE;
        break;
    case ROUTE_NONE:
        break;
    }
    if (!allowed) return X4_HTTP_ARGUMENT;
    size_t json_size = 0;
    int rc;
    if (route == ROUTE_PLAY) {
        h->stage = "cuerpo JSON rechazado";
        rc = check_json(json, &json_size);
        if (rc) return rc;
    }
    h->stage = "cabecera rechazada";
    if (count != X4_HTTP_SESSION_HEADERS) return X4_HTTP_ARGUMENT;
    /* Three distinct names out of a three-name allowlist: all are present. */
    rc = check_headers(DEST_SESSION, headers, count);
    if (rc) return rc;
    /* The empty keepalive POST still declares application/json. */
    Owned *o = own(native, url, json, json_size, json ? "application/json" : NULL, headers, count);
    if (!o) { h->stage = "memoria de solicitud"; return X4_HTTP_ALLOCATION; }
    return transfer(h, o, body, capacity, length, status, cancel);
}

int x4_http_request(X4Http *h, const char *url, const char *form, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    if (!h) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    /* Single owner: a concurrent call is refused without touching state. */
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform(h, url, form, body, capacity, length, status, cancel);
    atomic_store(&h->busy, false);
    return rc;
}

int x4_http_json_request(X4Http *h, const char *url, const X4HttpHeader *headers, size_t header_count,
    const char *json, char *body, size_t capacity, size_t *length, int *status, const _Atomic int *cancel)
{
    if (!h) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform_json(h, url, headers, header_count, json, body, capacity, length, status, cancel);
    atomic_store(&h->busy, false);
    return rc;
}

int x4_http_session_request(X4Http *h, enum X4HttpSessionMethod method, const char *url,
    const X4HttpHeader *headers, size_t header_count, const char *json, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    if (!h) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform_session(h, method, url, headers, header_count, json, body, capacity, length, status,
        cancel);
    atomic_store(&h->busy, false);
    return rc;
}
