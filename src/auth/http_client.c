/* SPDX-License-Identifier: GPL-3.0-only */
#include "http_client.h"
#include "ca_bundle.h"
#include "../core/module.h"
#include "../ui/lang.h"
#include "../ui/access_country.h"
#include "../core/build_identity.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <orbis/libkernel.h>

#define X4_NET_POOL_SIZE (256 * 1024)
#define X4_SSL_POOL_SIZE (512 * 1024)
#define X4_HTTP_POOL_SIZE (512 * 1024)
#define X4_HTTP_DEADLINE_USEC (30ull * 1000 * 1000)
#define X4_HTTP_TIMEOUT_USEC (5u * 1000 * 1000)
#define X4_HTTP_CHUNK 4096u
#define X4_HTTP_URL_MAX 256
#define X4_HTTP_VERSION_1_1 2
#define X4_HTTP_METHOD_GET 0
#define X4_HTTP_METHOD_POST 1
#define X4_HTTP_METHOD_DELETE 5
#define X4_HTTP_HEADER_OVERWRITE 0u
/* SERVER_VERIFY | CN_CHECK | NOT_AFTER | NOT_BEFORE | KNOWN_CA | SNI */
#define X4_HTTPS_VERIFY_ALL 0xBDu

static const char allowed_prefix[] = "https://login.microsoftonline.com/consumers/";
/* The only destination of x4_http_passport_request. */
static const char passport_url[] = "https://login.live.com/oauth20_token.srf";

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
static const char recent_titles_path[] = "/v2/titles/mru?mr=25";
static const char store_prefix[] = "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=";
#define store_suffix x4_lang_store_suffix()
static const char header_contract[] = "x-xbl-contract-version";
static const char header_client[] = "x-gssv-client";
static const char header_device[] = "X-MS-Device-Info";
static const char header_authorization[] = "Authorization";
static const char header_forwarded_for[] = "X-Forwarded-For";
static const char sessions_path[] = "/v5/sessions/cloud/";
#define X4_HTTP_SESSION_ID_MAX 128u
#define X4_HTTP_SESSION_HEADERS 3u

/* DEST_SESSION is produced only by classify_session, never by classify. */
enum Destination { DEST_NONE, DEST_XBOX_USER, DEST_XSTS, DEST_LOGIN, DEST_REGION, DEST_STORE, DEST_SESSION };
enum SessionRoute { ROUTE_NONE, ROUTE_WAITTIME, ROUTE_PLAY, ROUTE_STATE, ROUTE_KEEPALIVE, ROUTE_CONNECT, ROUTE_SDP, ROUTE_ICE, ROUTE_RESOURCE };

/* Request inputs copied into one block. It must outlive the native request,
 * connection and template, so it is freed only after they were deleted. */
typedef struct {
    int method; /* native GET 0, POST 1, DELETE 5 */
    const char *url, *payload, *content_type;
    const char *accept;
    const char *user_agent;
    uint64_t deadline_us;
    size_t payload_size, header_count, size;
    X4HttpHeader headers[X4_HTTP_HEADERS_MAX];
    char text[];
} Owned;
/* A native connection may retain the URL passed to CreateConnectionWithURL.
 * Its public-only anchor therefore outlives every request and is freed only
 * after the connection and template have both been deleted. */
typedef struct {
    int32_t tmpl, conn;
    Owned *anchor;
} PublicConnection;
/* Real typed ABI in pinned OpenOrbis libkernel.h. */
int sceKernelUsleep(unsigned int microseconds);

enum {
    X4_NET_INIT_UNSET = 0,
    X4_NET_INIT_BUSY  = 1,
    X4_NET_INIT_READY = 2,
};
static _Atomic int net_init_state = X4_NET_INIT_UNSET;

struct X4Http {
    int net_module, ssl_module, http_module;
    int32_t pool, ssl, http;
    bool unusable, quarantined;
    atomic_bool busy;
    PublicConnection public_connection[2]; /* Store, cover host; never auth */
    uint64_t public_retry_after_ms;
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
        int32_t (*all_response_headers)(int32_t, char **, size_t *);
        int32_t (*https_load_cert)(int32_t, int32_t, void *, void *, void *);
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
        .public_connection = {{.tmpl = -1, .conn = -1}, {.tmpl = -1, .conn = -1}},
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
    /* Real typed ABI in pinned OpenOrbis Http.h. Optional: absence cannot
     * make existing authentication fail; only Retry-After becomes unknown. */
    void *headers_address = NULL;
    if (x4_module_symbol(h->http_module, "sceHttpGetAllResponseHeaders", &headers_address) >= 0)
        h->fn.all_response_headers = (typeof(h->fn.all_response_headers))headers_address;
    void *load_cert_address = NULL;
    if (x4_module_symbol(h->http_module, "sceHttpsLoadCert", &load_cert_address) >= 0)
        h->fn.https_load_cert = (typeof(h->fn.https_load_cert))load_cert_address;

    h->stage = "NetInit";
    for (;;) {
        int expected = X4_NET_INIT_UNSET;
        if (atomic_compare_exchange_strong(&net_init_state, &expected, X4_NET_INIT_BUSY)) {
            rc = h->fn.net_init();
            printf("XCloud4: NetInit -> 0x%08x\n", (unsigned)rc);
            if (rc < 0 && (unsigned)rc != 0x80410103u) {
                atomic_store(&net_init_state, X4_NET_INIT_UNSET);
                goto fail;
            }
            atomic_store(&net_init_state, X4_NET_INIT_READY);
            break;
        } else if (expected == X4_NET_INIT_READY) {
            break;
        } else {
            sceKernelUsleep(1000);
        }
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
    if (h->fn.https_load_cert) {
        static _Atomic int ca_log_once = 0;
        size_t ca_count = 0;
        const SslPem * const *pem_ptrs = x4_ca_bundle_get_ssl_pem_ptrs(&ca_count);
        int cert_rc = h->fn.https_load_cert(h->http, (int32_t)ca_count, (void *)pem_ptrs, NULL, NULL);
        const char *ca_mode = "todas";
        if (cert_rc < 0) {
            pem_ptrs = x4_ca_bundle_get_ssl_pem_ptrs_rsa(&ca_count);
            cert_rc = h->fn.https_load_cert(h->http, (int32_t)ca_count, (void *)pem_ptrs, NULL, NULL);
            if (cert_rc >= 0) {
                ca_mode = "solo RSA";
            } else {
                ca_mode = "ninguna";
                ca_count = 0;
            }
        }
        int expected_log = 0;
        if (atomic_compare_exchange_strong(&ca_log_once, &expected_log, 1)) {
            printf("XCloud4: CA propias: %u cargadas (%s) rc=0x%08x\n",
                   (unsigned)ca_count, ca_mode, (unsigned)cert_rc);
        }
    }
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
    if (!h->quarantined) (void)x4_http_public_reset(h);
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
    size_t prefix = sizeof(store_prefix) - 1, suffix = strlen(store_suffix);
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
    path = sizeof(recent_titles_path) - 1;
    if (n > scheme + path && !memcmp(url, https_scheme, scheme) &&
        !strcmp(url + n - path, recent_titles_path) && region_host(url + scheme, n - scheme - path))
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
 * <id>/keepalive | <id>/connect | <id>/sdp | <id>/ice}, plus the optional
 * GET /v1/waittime/<titleId>. The host passes the same label checks as
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
    /* Optional queue estimate: the exact researched GET route only. The
     * public title ID is one safe segment, not a session ID or arbitrary URL. */
    static const char waittime[] = "/v1/waittime/";
    if (!strncmp(path, waittime, sizeof(waittime) - 1)) {
        const char *title = path + sizeof(waittime) - 1;
        size_t len = strlen(title);
        if (!len || len > 95) return ROUTE_NONE;
        for (size_t i = 0; i < len; ++i)
            if (!alphanumeric((unsigned char)title[i]) && title[i] != '-' && title[i] != '_') return ROUTE_NONE;
        return ROUTE_WAITTIME;
    }
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
    if (!strcmp(tail, "/connect")) return ROUTE_CONNECT;
    if (!strcmp(tail, "/sdp")) return ROUTE_SDP;
    if (!strcmp(tail, "/ice")) return ROUTE_ICE;
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
        else if (!strcmp(name, header_forwarded_for))
            allowed = d == DEST_LOGIN && x4_access_country_valid_ip(value);
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
    o->accept = "application/json";
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
    uint64_t deadline = sceKernelGetProcessTime() + (o->deadline_us ? o->deadline_us : X4_HTTP_DEADLINE_USEC);
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
    OPTION("cabeceras", h->fn.add_header(req, "Accept", o->accept, X4_HTTP_HEADER_OVERWRITE));
    OPTION("cabeceras", h->fn.add_header(req, "Accept-Encoding", "identity", X4_HTTP_HEADER_OVERWRITE));
    if (o->user_agent)
        OPTION("cabeceras", h->fn.add_header(req, "User-Agent", o->user_agent, X4_HTTP_HEADER_OVERWRITE));
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
    /* The method and body rule are both part of the route allowlist. */
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
    case ROUTE_CONNECT:
        allowed = method == X4_SESSION_HTTP_POST && json && json[0];
        native = X4_HTTP_METHOD_POST;
        break;
    case ROUTE_SDP:
    case ROUTE_ICE:
        allowed = (method == X4_SESSION_HTTP_GET && !json) ||
            (method == X4_SESSION_HTTP_POST && json && json[0]);
        native = method == X4_SESSION_HTTP_POST ? X4_HTTP_METHOD_POST : X4_HTTP_METHOD_GET;
        break;
    case ROUTE_STATE:
    case ROUTE_WAITTIME:
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
    if (json && json[0]) {
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

/* Fixed destination: only the form comes from the caller. */
static int perform_passport(X4Http *h, const char *form, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel)
{
    h->stage = "argumentos HTTPS";
    if (!form || !body || capacity == 0 || !length || !status) return X4_HTTP_ARGUMENT;
    body[0] = 0;
    *length = 0;
    *status = 0;
    if (capacity > X4_HTTP_PASSPORT_BODY_MAX + 1) return X4_HTTP_ARGUMENT;
    if (h->unusable) { h->stage = "HTTPS inutilizable"; return X4_HTTP_UNUSABLE; }
    h->stage = "formulario rechazado";
    size_t form_size = 0;
    int rc = check_form(form, &form_size);
    if (rc) return rc;
    if (form_size == 0) return X4_HTTP_ARGUMENT;
    Owned *o = own(X4_HTTP_METHOD_POST, passport_url, form, form_size, "application/x-www-form-urlencoded",
        NULL, 0);
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

int x4_http_passport_request(X4Http *h, const char *form, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel)
{
    if (!h) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform_passport(h, form, body, capacity, length, status, cancel);
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

#if X4_PUBLIC
static const char releases_fixed_url[] = X4_HTTP_RELEASES_URL_PUBLIC;
#else
static const char releases_fixed_url[] = X4_HTTP_RELEASES_URL_LAB;
#endif

static int perform_releases(X4Http *h, const char *url, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    h->stage = "argumentos HTTPS";
    if (!url || strcmp(url, releases_fixed_url) != 0) return X4_HTTP_URL;
    if (!body || capacity == 0 || capacity > X4_HTTP_RELEASES_BODY_MAX + 1 || !length || !status)
        return X4_HTTP_ARGUMENT;
    body[0] = 0;
    *length = 0;
    *status = 0;
    if (h->unusable) return X4_HTTP_UNUSABLE;
    Owned *o = own(X4_HTTP_METHOD_GET, url, NULL, 0, NULL, NULL, 0);
    if (!o) return X4_HTTP_ALLOCATION;
    o->accept = "application/vnd.github+json";
    o->user_agent = "XCloud4/" X4_PRODUCT_VERSION;
    o->deadline_us = 10ull * 1000 * 1000;
    return transfer(h, o, body, capacity, length, status, cancel);
}

int x4_http_releases_request(X4Http *h, const char *url, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    if (!h) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform_releases(h, url, body, capacity, length, status, cancel);
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

static void free_public_owned(Owned **owned)
{
    if (!*owned) return;
    x4_secure_clear(*owned, sizeof(**owned) + (*owned)->size);
    free(*owned);
    *owned = NULL;
}

/* The caller has the sole worker gate and has already deleted its request. */
static int public_connection_reset(X4Http *h, PublicConnection *slot)
{
    int32_t no_request = -1;
    if (!release_request(h, &slot->tmpl, &slot->conn, &no_request)) return X4_HTTP_UNUSABLE;
    free_public_owned(&slot->anchor);
    return 0;
}

int x4_http_public_reset(X4Http *h)
{
    if (!h) return X4_HTTP_ARGUMENT;
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = h->unusable ? X4_HTTP_UNUSABLE : 0;
    for (size_t i = 0; !rc && i < 2; ++i) rc = public_connection_reset(h, &h->public_connection[i]);
    h->public_retry_after_ms = 0;
    atomic_store(&h->busy, false);
    return rc;
}

uint64_t x4_http_public_retry_after_ms(const X4Http *h)
{
    return h ? h->public_retry_after_ms : 0;
}

static unsigned date_digits(const char *p, size_t n)
{
    unsigned value = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return UINT32_MAX;
        value = value * 10 + (unsigned)(p[i] - '0');
    }
    return value;
}

/* Parse the standard, current HTTP date representation without timegm or
 * locale-dependent strptime. time() is used only for the HTTP-date delta;
 * deadlines and the worker retry schedule remain monotonic. */
static uint64_t retry_after_value(const char *p, size_t n, int64_t now_seconds)
{
    if (!n) return 0;
    uint64_t seconds = 0;
    bool digits = true;
    for (size_t i = 0; i < n; ++i) if (p[i] < '0' || p[i] > '9') { digits = false; break; }
    if (digits) {
        for (size_t i = 0; i < n; ++i) {
            unsigned digit = (unsigned)(p[i] - '0');
            if (seconds > (UINT64_MAX - digit) / 10) return UINT64_MAX;
            seconds = seconds * 10 + digit;
        }
        return seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000;
    }
    if (n != 29 || p[3] != ',' || p[4] != ' ' || p[7] != ' ' || p[11] != ' ' ||
        p[16] != ' ' || p[19] != ':' || p[22] != ':' || memcmp(p + 25, " GMT", 4) || now_seconds < 0)
        return 0;
    static const char weekdays[] = "MonTueWedThuFriSatSun", months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    bool weekday = false;
    for (size_t i = 0; i < 7; ++i) if (!memcmp(p, weekdays + 3 * i, 3)) weekday = true;
    if (!weekday) return 0;
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i) if (!memcmp(p + 8, months + 3 * i, 3)) month = i + 1;
    unsigned day = date_digits(p + 5, 2), year = date_digits(p + 12, 4);
    unsigned hour = date_digits(p + 17, 2), minute = date_digits(p + 20, 2), second = date_digits(p + 23, 2);
    if (!month || year < 1970 || year > 9999 || hour > 23 || minute > 59 || second > 59) return 0;
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (!day || day > month_days[month - 1] + (month == 2 && leap ? 1u : 0u)) return 0;
    /* Gregorian civil date to days since 1970, with only bounded arithmetic. */
    unsigned adjusted_year = year - (month <= 2), era = adjusted_year / 400;
    unsigned yoe = adjusted_year - era * 400;
    unsigned mp = month > 2 ? month - 3 : month + 9;
    unsigned doy = (153 * mp + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    int64_t target = days * 86400 + hour * 3600 + minute * 60 + second;
    return target > now_seconds ? (uint64_t)(target - now_seconds) * 1000 : 0;
}

static bool ascii_header_equal(const char *p, size_t n, const char *expected)
{
    if (n != strlen(expected)) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)expected[i]) return false;
    }
    return true;
}

/* Borrowed native memory is inspected before DeleteRequest, never retained
 * or logged. Size and each value are bounded; duplicates are rejected. */
static uint64_t retry_after_headers(const char *headers, size_t size, int64_t now_seconds)
{
    if (!headers || !size || size > 64u * 1024) return 0;
    if (!headers[size - 1]) --size;
    bool found = false;
    uint64_t result = 0;
    size_t start = 0;
    while (start < size) {
        size_t end = start;
        while (end < size && headers[end] != '\n') ++end;
        size_t length = end - start;
        if (length && headers[start + length - 1] == '\r') --length;
        const char *line = headers + start;
        const char *colon = memchr(line, ':', length);
        if (colon && ascii_header_equal(line, (size_t)(colon - line), "retry-after")) {
            if (found) return 0;
            found = true;
            const char *value = colon + 1, *limit = line + length;
            while (value < limit && (*value == ' ' || *value == '\t')) ++value;
            while (limit > value && (limit[-1] == ' ' || limit[-1] == '\t')) --limit;
            size_t n = (size_t)(limit - value);
            if (n > 64) return 0;
            result = retry_after_value(value, n, now_seconds);
        }
        start = end < size ? end + 1 : size;
    }
    return result;
}

/* Only public GETs enter here. Connections are never shared with credential
 * exchanges, even when those use the same X4Http owner. At most one fresh
 * connection retry is made after a native error on an already reused slot. */
static int transfer_public(X4Http *h, Owned *o, unsigned which, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    PublicConnection *slot = &h->public_connection[which];
    uint64_t deadline = sceKernelGetProcessTime() + (o->deadline_us ? o->deadline_us : X4_HTTP_DEADLINE_USEC);
    h->public_retry_after_ms = 0;
    int rc = 0;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bool reused = slot->conn >= 0;
        int32_t req = -1, code = 0;
        size_t total = 0;
#define PUBLIC_CHECKPOINT() do { rc = checkpoint(cancel, deadline); if (rc) goto public_done; } while (0)
#define PUBLIC_CREATE(text, target, expr) do { h->stage = text; PUBLIC_CHECKPOINT(); \
        rc = (expr); if (rc < 0) goto public_done; target = rc; } while (0)
#define PUBLIC_OPTION(text, expr) do { h->stage = text; rc = (expr); \
        if (rc != 0) { if (rc > 0) rc = X4_HTTP_PROTOCOL; goto public_done; } } while (0)
        PUBLIC_CHECKPOINT();
        if (!reused) {
            slot->anchor = own(X4_HTTP_METHOD_GET, o->url, NULL, 0, NULL, NULL, 0);
            if (!slot->anchor) { rc = X4_HTTP_ALLOCATION; goto public_done; }
            PUBLIC_CREATE("plantilla publica HTTPS", slot->tmpl,
                h->fn.create_template(h->http, "XCloud4", X4_HTTP_VERSION_1_1, 0));
            PUBLIC_OPTION("tiempo de resolucion", h->fn.resolve_timeout(slot->tmpl, X4_HTTP_TIMEOUT_USEC));
            PUBLIC_OPTION("tiempo de conexion", h->fn.connect_timeout(slot->tmpl, X4_HTTP_TIMEOUT_USEC));
            PUBLIC_OPTION("tiempo de envio", h->fn.send_timeout(slot->tmpl, X4_HTTP_TIMEOUT_USEC));
            PUBLIC_OPTION("tiempo de recepcion", h->fn.recv_timeout(slot->tmpl, X4_HTTP_TIMEOUT_USEC));
            PUBLIC_OPTION("redirecciones publicas", h->fn.auto_redirect(slot->tmpl, 0));
            PUBLIC_OPTION("cookies publicas", h->fn.cookie_enabled(slot->tmpl, 0));
            PUBLIC_OPTION("verificacion TLS publica", h->fn.https_enable(slot->tmpl, X4_HTTPS_VERIFY_ALL));
            PUBLIC_CREATE("conexion publica HTTPS", slot->conn,
                h->fn.create_connection(slot->tmpl, slot->anchor->url, true));
        }
        PUBLIC_CREATE("solicitud publica HTTPS", req,
            h->fn.create_request(slot->conn, X4_HTTP_METHOD_GET, o->url, 0));
        PUBLIC_OPTION("cabeceras publicas", h->fn.add_header(req, "Accept", o->accept, X4_HTTP_HEADER_OVERWRITE));
        PUBLIC_OPTION("cabeceras publicas", h->fn.add_header(req, "Accept-Encoding", "identity", X4_HTTP_HEADER_OVERWRITE));
        h->stage = "envio publico HTTPS/TLS";
        PUBLIC_CHECKPOINT();
        rc = h->fn.send(req, NULL, 0);
        if (rc != 0) { if (rc > 0) rc = X4_HTTP_PROTOCOL; goto public_done; }
        PUBLIC_CHECKPOINT();
        PUBLIC_OPTION("codigo publico de estado", h->fn.status(req, &code));
        if (code < 100 || code > 599) { rc = X4_HTTP_PROTOCOL; goto public_done; }
        if (h->fn.all_response_headers) {
            char *headers = NULL;
            size_t headers_size = 0;
            if (!h->fn.all_response_headers(req, &headers, &headers_size))
                h->public_retry_after_ms = retry_after_headers(headers, headers_size, (int64_t)time(NULL));
        }
        h->stage = "lectura publica de respuesta";
        for (;;) {
            PUBLIC_CHECKPOINT();
            size_t room = capacity - 1 - total;
            unsigned char probe = 0;
            void *target = room ? (void *)(body + total) : (void *)&probe;
            uint32_t ask = !room ? 1u : room < X4_HTTP_CHUNK ? (uint32_t)room : X4_HTTP_CHUNK;
            rc = h->fn.read(req, target, ask);
            x4_secure_clear(&probe, sizeof(probe));
            if (rc < 0) goto public_done;
            int got = rc;
            PUBLIC_CHECKPOINT();
            if (!got) break;
            if ((uint32_t)got > ask) { rc = X4_HTTP_PROTOCOL; goto public_done; }
            if (!room) { rc = X4_HTTP_BOUND; goto public_done; }
            total += (size_t)got;
        }
        rc = 0;
public_done:
#undef PUBLIC_OPTION
#undef PUBLIC_CREATE
#undef PUBLIC_CHECKPOINT
        /* Delete only the request after success. Its copied URL may then be
         * released; the slot anchor remains until public_reset/close. */
        bool released = true;
        if (req >= 0) {
            int delete_rc = h->fn.delete_request(req);
            if (delete_rc != 0) {
                printf("XCloud4: HTTP publico liberar solicitud 0x%08x; cuarentena\n", (unsigned)delete_rc);
                h->unusable = h->quarantined = true;
                released = false;
                rc = X4_HTTP_UNUSABLE;
            }
        }
        if (!released) break; /* retain o and anchor while native may use them */
        bool reconnect = rc < 0 && reused && attempt == 0 && rc != X4_HTTP_CANCELLED &&
            rc != X4_HTTP_DEADLINE && rc != X4_HTTP_BOUND && rc != X4_HTTP_PROTOCOL &&
            rc != X4_HTTP_ALLOCATION && rc != X4_HTTP_UNUSABLE;
        if (rc && public_connection_reset(h, slot)) rc = X4_HTTP_UNUSABLE;
        if (rc == X4_HTTP_UNUSABLE) break;
        if (reconnect) {
            int stop = checkpoint(cancel, deadline);
            if (!stop) {
                x4_secure_clear(body, capacity);
                h->public_retry_after_ms = 0;
                continue;
            }
            rc = stop;
        }
        if (!rc) {
            body[total] = 0;
            *length = total;
            *status = code;
            h->stage = "respuesta publica recibida";
        } else {
            x4_secure_clear(body, capacity);
            h->public_retry_after_ms = 0;
        }
        free_public_owned(&o);
        return rc;
    }
    x4_secure_clear(body, capacity);
    h->public_retry_after_ms = 0;
    return rc;
}

static int public_request(X4Http *h, const char *url, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel, bool image)
{
    if (!h || !url || !body || !capacity || !length || !status ||
        capacity > (image ? X4_HTTP_IMAGE_MAX : X4_HTTP_RESPONSE_MAX) + 1)
        return X4_HTTP_ARGUMENT;
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    body[0] = 0; *length = 0; *status = 0;
    h->public_retry_after_ms = 0;
    size_t n = bounded_length(url, X4_HTTP_JSON_URL_MAX + 1);
    int rc = X4_HTTP_URL;
    bool valid = image ? x4_http_public_image_url(url) != 0 : n <= X4_HTTP_JSON_URL_MAX && store_url(url, n);
    if (valid) {
        rc = X4_HTTP_UNUSABLE;
        if (!h->unusable) {
            Owned *o = own(X4_HTTP_METHOD_GET, url, NULL, 0, NULL, NULL, 0);
            if (!o) rc = X4_HTTP_ALLOCATION;
            else {
                if (image) o->accept = "image/jpeg,image/png";
                rc = transfer_public(h, o, image ? 1u : 0u, body, capacity, length, status, cancel);
            }
        }
    }
    atomic_store(&h->busy, false);
    return rc;
}

int x4_http_public_metadata_request(X4Http *h, const char *url, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    return public_request(h, url, body, capacity, length, status, cancel, false);
}

int x4_http_public_image_request(X4Http *h, const char *url, unsigned char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    return public_request(h, url, (char *)body, capacity, length, status, cancel, true);
}
