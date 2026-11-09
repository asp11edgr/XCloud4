/* SPDX-License-Identifier: GPL-3.0-only */
/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
#include "http_client.h"
#include "http_wire.h"
#include "tls_conn.h"
#include "../ui/lang.h"
#include "../ui/home_rows.h"
#include "../ui/access_country.h"
#include "../core/build_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <orbis/libkernel.h>
#ifdef X4_OPENORBIS
#include "../core/module.h"
#include "../streaming/rtc_native.h"
#endif

#define X4_NET_POOL_SIZE (256 * 1024)
#define X4_HTTP_DEADLINE_USEC (30ull * 1000 * 1000)
#define X4_HTTP_TIMEOUT_USEC (5u * 1000 * 1000)
#define X4_HTTP_CHUNK 4096u
#define X4_HTTP_URL_MAX 256
#define X4_HTTP_METHOD_GET 0
#define X4_HTTP_METHOD_POST 1
#define X4_HTTP_METHOD_DELETE 5
/* Header names/values remain bounded by the unchanged request policy. */
#define X4_HTTP_WIRE_REQUEST_MAX (X4_HTTP_HEADERS_MAX * (X4_HTTP_HEADER_VALUE_MAX + 128u) + 2048u)

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
static const char header_ms_cv[] = "MS-CV";
static const char sessions_path[] = "/v5/sessions/cloud/";
#define X4_HTTP_SESSION_ID_MAX 128u
#define X4_HTTP_SESSION_HEADERS 3u

/* DEST_SESSION is produced only by classify_session, never by classify. */
enum Destination { DEST_NONE, DEST_XBOX_USER, DEST_XSTS, DEST_LOGIN, DEST_REGION, DEST_STORE,
    DEST_SESSION, DEST_CLOUD_TIME };
enum SessionRoute { ROUTE_NONE, ROUTE_WAITTIME, ROUTE_PLAY, ROUTE_STATE, ROUTE_KEEPALIVE, ROUTE_CONNECT, ROUTE_SDP, ROUTE_ICE, ROUTE_RESOURCE };

typedef struct {
    int method; /* native GET 0, POST 1, DELETE 5 */
    const char *url, *payload, *content_type;
    const char *accept;
    const char *user_agent;
    uint64_t deadline_us;
    bool short_state; /* Explicitly selected only by diagnostic GET /state. */
    uint64_t absolute_deadline_us; /* Scoped reconnect request, never mutable context state. */
    size_t payload_size, header_count, size;
    X4HttpHeader headers[X4_HTTP_HEADERS_MAX];
    char text[];
} Owned;

typedef struct {
    X4TlsConn *conn;
    Owned *anchor;
    bool reopen_due_close;
} PublicConnection;

typedef struct {
    X4TlsConn *conn;
    char host[254], id[X4_HTTP_SESSION_ID_MAX + 1u];
    bool reopen_due_close;
} SessionConnection;

struct X4Http {
    X4TlsHttp *tls;
    bool unusable, quarantined, update_only;
    atomic_bool busy;
    PublicConnection public_connection[2];
    SessionConnection session_connection;
    bool session_persistent;
    uint64_t public_retry_after_ms;
    _Atomic(const char *) stage;
#ifdef X4_OPENORBIS
    int net_module;
    int32_t pool;
    struct {
        int32_t (*net_init)(void);
        int32_t (*pool_create)(const char *,int32_t,int32_t);
        int32_t (*pool_destroy)(int32_t);
    } fn;
#endif
};
#ifdef X4_OPENORBIS
enum { X4_NET_INIT_UNSET, X4_NET_INIT_BUSY, X4_NET_INIT_READY };
static _Atomic int net_init_state=X4_NET_INIT_UNSET;
#endif

void x4_secure_clear(void *data,size_t size)
{
    volatile unsigned char *p=data;
    if(!p)return;
    while(size--)*p++=0;
    atomic_signal_fence(memory_order_seq_cst);
}
const char *x4_http_stage(const X4Http *h)
{
    return h?atomic_load_explicit(&h->stage,memory_order_acquire):"sin contexto HTTPS";
}

/* TLS children close before their CA/DRBG and the existing Net pool. A
 * failed close retains the entire owner; no live child can use freed state. */
static bool terminate_contexts(X4Http *h)
{
    if(h->tls){
        int rc=x4_tls_http_close(h->tls);
        if(rc){h->unusable=h->quarantined=true;return false;}
        h->tls=NULL;
    }
#ifdef X4_OPENORBIS
    if(h->pool>=0){
        int rc=h->fn.pool_destroy(h->pool);
        if(rc){printf("XCloud4: HTTPS NetPoolDestroy codigo=0x%08x\n",(unsigned)rc);h->quarantined=true;return false;}
        h->pool=-1;
    }
#endif
    return true;
}
X4Http *x4_http_open(int *error)
{
    int rc=X4_HTTP_ALLOCATION;
    X4Http *h=calloc(1,sizeof(*h));
    if(!h){if(error)*error=rc;return NULL;}
    atomic_init(&h->busy,false);atomic_init(&h->stage,"modulos de red");
#ifdef X4_OPENORBIS
    h->pool=-1;h->net_module=-1;
    rc=h->net_module=x4_module_open("libSceNet");if(rc<0)goto fail;
#define RESOLVE(name,field) do {void *address=NULL;rc=x4_module_symbol(h->net_module,name,&address); \
    if(rc<0)goto fail;h->fn.field=(typeof(h->fn.field))address;} while(0)
    RESOLVE("sceNetInit",net_init);
    RESOLVE("sceNetPoolCreate",pool_create);
    RESOLVE("sceNetPoolDestroy",pool_destroy);
#undef RESOLVE
    h->stage="NetInit";
    for(;;){
        int expected=X4_NET_INIT_UNSET;
        if(atomic_compare_exchange_strong(&net_init_state,&expected,X4_NET_INIT_BUSY)){
            rc=h->fn.net_init();
            if(rc<0&&(unsigned)rc!=0x80410103u){atomic_store(&net_init_state,X4_NET_INIT_UNSET);goto fail;}
            /* Serialize the legacy resolver preparation before publishing
             * readiness; no HTTP request enters its preparation spin. */
            if(x4_native_net_prepare()<0){rc=X4_HTTP_UNUSABLE;atomic_store(&net_init_state,X4_NET_INIT_UNSET);goto fail;}
            atomic_store(&net_init_state,X4_NET_INIT_READY);break;
        }
        if(expected==X4_NET_INIT_READY)break;
        sceKernelUsleep(1000);
    }
    h->stage="pool de red";
    rc=h->fn.pool_create("x4-http-net",X4_NET_POOL_SIZE,0);if(rc<0)goto fail;h->pool=rc;
#endif
    h->stage="inicio HTTPS (red/TLS)";
    h->tls=x4_tls_http_open(&rc);if(!h->tls)goto fail;
    h->stage="HTTPS listo";if(error)*error=0;return h;
fail:
    printf("XCloud4: HTTPS etapa=%s codigo=0x%08x\n",x4_http_stage(h),(unsigned)rc);
    if(terminate_contexts(h)){x4_secure_clear(h,sizeof(*h));free(h);}
    if(error)*error=rc;return NULL;
}
void x4_http_close(X4Http *h)
{
    if(!h)return;
    if(!h->quarantined)(void)x4_http_session_persistence(h,false);
    if(!h->quarantined)(void)x4_http_public_reset(h);
    if(h->quarantined||!terminate_contexts(h)){
        printf("XCloud4: HTTPS recursos retenidos tras fallo de cierre\n");return;
    }
    x4_secure_clear(h,sizeof(*h));free(h);
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

static bool public_sigl_url(const char *url, size_t length)
{
    static const char prefix[] = X4_SIGL_URL_PREFIX;
    static const char *const allowed[] = {
        X4_SIGL_GUID_FREE, X4_SIGL_GUID_NEW, X4_SIGL_GUID_POPULAR, X4_SIGL_GUID_LEAVING
    };
    enum { GUID_LENGTH = sizeof(X4_SIGL_GUID_FREE) - 1 };
    const size_t prefix_length = sizeof(prefix) - 1;
    if (length <= prefix_length + GUID_LENGTH || memcmp(url, prefix, prefix_length))
        return false;
    const char *guid = url + prefix_length;
    bool known = false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(*allowed); ++i) {
        if (!memcmp(guid, allowed[i], GUID_LENGTH)) {
            known = true;
            break;
        }
    }
    const char *market = x4_lang_market(), *locale = x4_lang_locale();
    if (!known || !x4_sigl_locale_valid(market, locale)) return false;
    char suffix[32];
    int count = snprintf(suffix, sizeof(suffix), X4_SIGL_LOCALE_FORMAT, market, locale);
    return count > 0 && (size_t)count < sizeof(suffix) && !strcmp(guid + GUID_LENGTH, suffix);
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

static bool cloud_time_url(const char *url)
{
    static const char prefix[] = X4_HTTP_CLOUD_TIME_PREFIX;
    static const char suffix[] = X4_HTTP_CLOUD_TIME_SUFFIX;
    size_t prefix_length = sizeof(prefix) - 1;
    size_t length = sizeof(prefix) + sizeof(suffix);
    if (!url || bounded_length(url, length + 1) != length ||
        memcmp(url, prefix, prefix_length)) return false;
    const char *market = url + prefix_length;
    return market[0] >= 'A' && market[0] <= 'Z' && market[1] >= 'A' && market[1] <= 'Z' &&
        !strcmp(market + 2, suffix);
}

static bool cloud_time_authorization(const char *value)
{
    static const char prefix[] = "XBL3.0 x=";
    if (strncmp(value, prefix, sizeof(prefix) - 1)) return false;
    const char *hash = value + sizeof(prefix) - 1;
    size_t n = 0;
    while (hash[n] >= '0' && hash[n] <= '9') ++n;
    return n > 0 && n <= 20 && hash[n] == ';' && token68(hash + n + 1);
}

static bool cloud_time_cv(const char *value, size_t length)
{
    if (length != 18 || value[16] != '.' || value[17] != '0') return false;
    for (size_t i = 0; i < 16; ++i)
        if (!alphanumeric((unsigned char)value[i]) && value[i] != '+' && value[i] != '/')
            return false;
    return true;
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
            allowed = d == DEST_CLOUD_TIME ? cloud_time_authorization(value) :
                (d == DEST_REGION || d == DEST_SESSION) && !strncmp(value, "Bearer ", 7) &&
                    token68(value + 7);
        else if (!strcmp(name, header_ms_cv))
            allowed = d == DEST_CLOUD_TIME && cloud_time_cv(value, n);
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

static int checkpoint(const _Atomic int *cancel,uint64_t deadline)
{
    if(cancel&&atomic_load(cancel))return X4_HTTP_CANCELLED;
    if(sceKernelGetProcessTime()>=deadline)return X4_HTTP_DEADLINE;
    return 0;
}
static void free_owned(Owned **owned)
{
    if(!*owned)return;
    x4_secure_clear(*owned,sizeof(**owned)+(*owned)->size);free(*owned);*owned=NULL;
}
static bool release_connection(X4Http *h,X4TlsConn **conn)
{
    if(!*conn)return true;
    int rc=x4_tls_conn_close(*conn);
    if(rc){
        h->unusable=h->quarantined=true;
        printf("XCloud4: HTTPS cierre codigo=0x%08x; recursos retenidos\n",(unsigned)rc);return false;
    }
    *conn=NULL;return true;
}
static X4TlsBudget budget(X4Http *h,uint64_t deadline,const _Atomic int *cancel)
{return (X4TlsBudget){.deadline_us=deadline,.cancel=cancel,.stage=&h->stage};}
static int request_host(const Owned *o,char host[254],const char **target)
{
    const char *start=o->url+sizeof(https_scheme)-1,*slash=strchr(start,'/');
    if(!slash||slash==start||(size_t)(slash-start)>=254)return X4_HTTP_URL;
    memcpy(host,start,(size_t)(slash-start));host[slash-start]=0;*target=slash;return 0;
}
typedef struct {unsigned char *data;size_t size,capacity;} RequestBuffer;
static int collect_header(void *context,const void *data,size_t size)
{
    RequestBuffer *b=context;
    if(size>b->capacity-b->size)return X4_HTTP_BOUND;
    memcpy(b->data+b->size,data,size);b->size+=size;return 0;
}
/* One header write avoids a TLS record per serializer fragment. No socket
 * option or pacing change is needed; payload follows with the same budget. */
static int exchange(X4Http *h,Owned *o,X4TlsConn *conn,const char *host,const char *target,
    uint64_t deadline,const _Atomic int *cancel,bool keepalive,char *body,size_t capacity,
    size_t *length,int *status,bool *started,bool *reusable,uint64_t *retry_after)
{
    *started=*reusable=false;
    X4TlsBudget b=budget(h,deadline,cancel);
    const char *method=o->method==X4_HTTP_METHOD_POST?"POST":o->method==X4_HTTP_METHOD_DELETE?"DELETE":"GET";
    X4HttpWireRequest request={.method=method,.host=host,.target=target,.accept=o->accept,
        .content_type=o->content_type,.user_agent=o->user_agent,.headers=o->headers,
        .header_count=o->header_count,.payload_size=o->payload_size,.keepalive=keepalive};
    RequestBuffer header={.data=malloc(X4_HTTP_WIRE_REQUEST_MAX),.capacity=X4_HTTP_WIRE_REQUEST_MAX};
    if(!header.data)return X4_HTTP_ALLOCATION;
    int rc=x4_http_wire_write_request(&request,collect_header,&header);
    if(!rc)rc=checkpoint(cancel,deadline);
    b.phase_deadline_us=sceKernelGetProcessTime()+X4_HTTP_TIMEOUT_USEC;
    if(!rc)rc=x4_tls_conn_write(conn,header.data,header.size,&b);
    x4_secure_clear(header.data,header.capacity);free(header.data);
    if(!rc&&o->payload_size)rc=x4_tls_conn_write(conn,o->payload,o->payload_size,&b);
    if(rc)return rc;
    X4HttpWire parser;x4_http_wire_init(&parser,body,capacity,(int64_t)time(NULL));
    unsigned char data[X4_HTTP_CHUNK];
    for(;;){
        rc=checkpoint(cancel,deadline);if(rc)break;
        size_t got=0,used=0;b=budget(h,deadline,cancel);
        rc=x4_tls_conn_read(conn,data,sizeof(data),&got,&b);
        if(rc)break;
        rc=checkpoint(cancel,deadline);if(rc)break;
        if(got>sizeof(data)){rc=X4_HTTP_PROTOCOL;break;}
        if(!got)rc=x4_http_wire_eof(&parser);
        else rc=x4_http_wire_feed(&parser,data,got,&used);
        if(got&&used!=got&&rc>=0){rc=X4_HTTP_PROTOCOL;break;}
        if(rc==X4_HTTP_WIRE_DONE){
            X4HttpWireResult result=x4_http_wire_result(&parser);
            *length=result.body_bytes;*status=result.status;*reusable=result.reusable;
            if(retry_after)*retry_after=result.retry_after_ms;
            rc=0;break;
        }
        if(rc<0)break;
    }
    *started=x4_http_wire_result(&parser).response_started;
    x4_secure_clear(data,sizeof(data));
    return rc;
}
/* Private requests never retry, including an ambiguous POST /play. */
static int transfer(X4Http *h,Owned *o,char *body,size_t capacity,size_t *length,int *status,
    const _Atomic int *cancel)
{
    uint64_t deadline=o->absolute_deadline_us?o->absolute_deadline_us:
        sceKernelGetProcessTime()+(o->deadline_us?o->deadline_us:X4_HTTP_DEADLINE_USEC);
    char host[254];const char *target=NULL;X4TlsConn *conn=NULL;bool started=false,reusable=false;
    int rc=checkpoint(cancel,deadline);
    if(!rc)rc=request_host(o,host,&target);
    X4TlsBudget b=budget(h,deadline,cancel);
    if(!rc)rc=x4_tls_conn_connect(h->tls,host,&b,&conn);
    if(!rc)rc=exchange(h,o,conn,host,target,deadline,cancel,false,body,capacity,length,status,&started,&reusable,NULL);
    bool released=release_connection(h,&conn);
    if(released)free_owned(&o);
    if(rc==X4_HTTP_CANCELLED)h->stage="cancelado";
    else if(rc==X4_HTTP_DEADLINE)h->stage="plazo agotado";
    if(rc){x4_secure_clear(body,capacity);*length=0;*status=0;}
    else h->stage=released?"respuesta recibida":"recursos HTTP en cuarentena";
    return rc;
}

static int reset_session_connection(X4Http *h)
{
    if(!release_connection(h,&h->session_connection.conn))return X4_HTTP_UNUSABLE;
    x4_secure_clear(&h->session_connection,sizeof(h->session_connection));
    return 0;
}

int x4_http_session_persistence(X4Http *h,bool enabled)
{
    if(!h)return X4_HTTP_ARGUMENT;
    if(atomic_exchange(&h->busy,true))return X4_HTTP_BUSY;
    int rc=enabled?(h->unusable?X4_HTTP_UNUSABLE:0):reset_session_connection(h);
    h->session_persistent=enabled&&!rc;
    atomic_store(&h->busy,false);
    return rc;
}

static int transfer_session(X4Http *h,Owned *o,char *body,size_t capacity,size_t *length,int *status,
    const _Atomic int *cancel)
{
    SessionConnection *slot=&h->session_connection;
    uint64_t deadline=sceKernelGetProcessTime()+X4_HTTP_DEADLINE_USEC;
    char host[254];const char *target=NULL;
    int rc=checkpoint(cancel,deadline);
    if(!rc)rc=request_host(o,host,&target);
    if(!rc){
        /* perform_session has already validated the exact resource and suffix. */
        const char *id=target+sizeof(sessions_path)-1;
        size_t id_len=(size_t)(strrchr(target,'/')-id);
        bool same=!strcmp(host,slot->host)&&strlen(slot->id)==id_len&&!memcmp(id,slot->id,id_len);
        if(!same)rc=reset_session_connection(h);
        if(!rc&&slot->conn&&!x4_tls_conn_idle_clean(slot->conn)){
            if(!release_connection(h,&slot->conn))rc=X4_HTTP_UNUSABLE;
            else slot->reopen_due_close=true;
        }
        if(!rc)rc=checkpoint(cancel,deadline);
        if(!rc&&slot->conn)x4_tls_conn_mark_reused(slot->conn);
        else if(!rc){
            X4TlsBudget b=budget(h,deadline,cancel);
            rc=x4_tls_conn_connect(h->tls,host,&b,&slot->conn);
            if(!rc){
                if(slot->reopen_due_close)x4_tls_count_reopen();
                slot->reopen_due_close=false;
                memcpy(slot->host,host,strlen(host)+1);
                memcpy(slot->id,id,id_len);
                slot->id[id_len]=0;
            }
        }
    }
    bool started=false,reusable=false;
    /* There is no retry path after entering exchange, even before a reply. */
    if(!rc)rc=exchange(h,o,slot->conn,host,target,deadline,cancel,true,body,capacity,
                       length,status,&started,&reusable,NULL);
    if((rc||!reusable)&&!h->quarantined){
        if(rc)(void)reset_session_connection(h);
        else if(release_connection(h,&slot->conn))slot->reopen_due_close=true;
    }
    free_owned(&o);
    if(rc){x4_secure_clear(body,capacity);*length=0;*status=0;}
    if(rc==X4_HTTP_CANCELLED)h->stage="cancelado";
    else if(rc==X4_HTTP_DEADLINE)h->stage="plazo agotado";
    else if(!rc)h->stage=h->quarantined?"recursos HTTP en cuarentena":"respuesta recibida";
    return rc;
}

static int perform(X4Http *h, const char *url, const char *form, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel, uint64_t reconnect_deadline)
{
    h->stage = "argumentos HTTPS";
    if (!url || !body || capacity == 0 || !length || !status) return X4_HTTP_ARGUMENT;
    body[0] = 0;
    *length = 0;
    *status = 0;
    if (h->unusable) { h->stage = "HTTPS inutilizable"; return X4_HTTP_UNUSABLE; }
    h->stage = "URL rechazada";
    if (reconnect_deadline && (!form || strcmp(url, "https://login.microsoftonline.com/consumers/oauth2/v2.0/token"))) return X4_HTTP_URL;
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
    o->absolute_deadline_us = reconnect_deadline;
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
    size_t *length, int *status, const _Atomic int *cancel, bool short_state, uint64_t reconnect_deadline)
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
    if (route == ROUTE_NONE || (short_state && route != ROUTE_STATE)) return X4_HTTP_URL;
    if (reconnect_deadline && route != ROUTE_STATE && route != ROUTE_CONNECT &&
        route != ROUTE_SDP && route != ROUTE_ICE && route != ROUTE_KEEPALIVE) return X4_HTTP_URL;
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
    o->short_state = short_state;
    if (short_state) o->deadline_us = 2000000;
    o->absolute_deadline_us = reconnect_deadline;
    if (reconnect_deadline && route == ROUTE_STATE) {
        uint64_t state_deadline = sceKernelGetProcessTime() + 2000000;
        if (state_deadline < o->absolute_deadline_us) o->absolute_deadline_us = state_deadline;
    }
    if(h->session_persistent&&!short_state&&!reconnect_deadline&&
       (route==ROUTE_STATE||route==ROUTE_KEEPALIVE))
        return transfer_session(h,o,body,capacity,length,status,cancel);
    return transfer(h, o, body, capacity, length, status, cancel);
}

/* Fixed destination: only the form comes from the caller. */
static int perform_passport(X4Http *h, const char *form, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel, uint64_t reconnect_deadline)
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
    o->absolute_deadline_us = reconnect_deadline;
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
    int rc = perform(h, url, form, body, capacity, length, status, cancel, 0);
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
    int rc = perform_passport(h, form, body, capacity, length, status, cancel, 0);
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

int x4_http_cloud_time_request(X4Http *h, const char *url, const X4HttpHeader *headers,
    size_t header_count, char *body, size_t capacity, size_t *length, int *status,
    const _Atomic int *cancel)
{
    if (length) *length = 0;
    if (status) *status = 0;
    if (body && capacity) body[0] = 0;
    if (!h || !body || !capacity || capacity > X4_HTTP_RESPONSE_MAX + 1 || !length || !status)
        return X4_HTTP_ARGUMENT;
    if (!cloud_time_url(url)) return X4_HTTP_URL;
    if (header_count != 2) return X4_HTTP_ARGUMENT;
    int rc = check_headers(DEST_CLOUD_TIME, headers, header_count);
    if (rc) return rc;
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    if (h->unusable) rc = X4_HTTP_UNUSABLE;
    else {
        Owned *o = own(X4_HTTP_METHOD_GET, url, NULL, 0, NULL, headers, header_count);
        if (!o) rc = X4_HTTP_ALLOCATION;
        else {
            o->accept = "application/x-protobuf, application/json";
            rc = transfer(h, o, body, capacity, length, status, cancel);
        }
    }
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
        cancel, false, 0);
    atomic_store(&h->busy, false);
    return rc;
}

int x4_http_session_state_request(X4Http *h, const char *url,
    const X4HttpHeader *headers, size_t header_count, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel)
{
    if (!h) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform_session(h, X4_SESSION_HTTP_GET, url, headers, header_count,
        NULL, body, capacity, length, status, cancel, true, 0);
    atomic_store(&h->busy, false);
    return rc;
}

int x4_http_session_reconnect_request(X4Http *h, enum X4HttpSessionMethod method, const char *url,
    const X4HttpHeader *headers, size_t header_count, const char *json, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel, uint64_t absolute_deadline_us)
{
    if (!h || !absolute_deadline_us) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = perform_session(h, method, url, headers, header_count, json, body, capacity,
        length, status, cancel, false, absolute_deadline_us);
    atomic_store(&h->busy, false);
    return rc;
}

int x4_http_auth_reconnect_request(X4Http *h, bool passport, const char *form, char *body,
    size_t capacity, size_t *length, int *status, const _Atomic int *cancel,
    uint64_t absolute_deadline_us)
{
    if (!h || !absolute_deadline_us) {
        if (length) *length = 0;
        if (status) *status = 0;
        if (body && capacity) body[0] = 0;
        return X4_HTTP_ARGUMENT;
    }
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = passport ? perform_passport(h, form, body, capacity, length, status, cancel, absolute_deadline_us) :
        perform(h, "https://login.microsoftonline.com/consumers/oauth2/v2.0/token", form,
            body, capacity, length, status, cancel, absolute_deadline_us);
    atomic_store(&h->busy, false);
    return rc;
}

static int public_connection_reset(X4Http *h,PublicConnection *slot)
{
    if(!release_connection(h,&slot->conn))return X4_HTTP_UNUSABLE;
    free_owned(&slot->anchor);slot->reopen_due_close=false;return 0;
}
int x4_http_public_reset(X4Http *h)
{
    if(!h)return X4_HTTP_ARGUMENT;
    if(atomic_exchange(&h->busy,true))return X4_HTTP_BUSY;
    int rc=h->unusable?X4_HTTP_UNUSABLE:0;
    for(size_t i=0;!rc&&i<2;++i)rc=public_connection_reset(h,&h->public_connection[i]);
    h->public_retry_after_ms=0;atomic_store(&h->busy,false);return rc;
}
uint64_t x4_http_public_retry_after_ms(const X4Http *h)
{return h?h->public_retry_after_ms:0;}
static int transfer_public(X4Http *h,Owned *o,unsigned which,char *body,size_t capacity,
    size_t *length,int *status,const _Atomic int *cancel)
{
    PublicConnection *slot=&h->public_connection[which];
    uint64_t deadline=sceKernelGetProcessTime()+(o->deadline_us?o->deadline_us:X4_HTTP_DEADLINE_USEC);
    h->public_retry_after_ms=0;
    char host[254];const char *target=NULL;
    int rc=request_host(o,host,&target);
    for(unsigned attempt=0;!rc&&attempt<2;++attempt){
        bool reused=slot->conn!=NULL,started=false,reusable=false;
        rc=checkpoint(cancel,deadline);
        if(!rc&&!reused){
            slot->anchor=own(X4_HTTP_METHOD_GET,o->url,NULL,0,NULL,NULL,0);
            if(!slot->anchor)rc=X4_HTTP_ALLOCATION;
            X4TlsBudget b=budget(h,deadline,cancel);
            if(!rc)rc=x4_tls_conn_connect(h->tls,host,&b,&slot->conn);
            if(!rc&&slot->reopen_due_close){x4_tls_count_reopen();slot->reopen_due_close=false;}
        }else if(!rc)x4_tls_conn_mark_reused(slot->conn);
        if(!rc)rc=exchange(h,o,slot->conn,host,target,deadline,cancel,true,body,capacity,length,status,&started,&reusable,
                               &h->public_retry_after_ms);
        bool retry=rc < -1099&&reused&&!attempt&&!started;
        if(rc||!reusable){
            bool closed=slot->conn&&(!rc||rc < -1099);
            if(public_connection_reset(h,slot))rc=X4_HTTP_UNUSABLE;
            else slot->reopen_due_close=closed;
        }
        if(rc==X4_HTTP_UNUSABLE||h->quarantined)break;
        if(retry){
            int stop=checkpoint(cancel,deadline);
            if(!stop){x4_secure_clear(body,capacity);*length=0;*status=0;h->public_retry_after_ms=0;rc=0;continue;}
            rc=stop;
        }
        break;
    }
    if(!h->quarantined)free_owned(&o);
    if(rc){x4_secure_clear(body,capacity);*length=0;*status=0;h->public_retry_after_ms=0;}
    if(rc==X4_HTTP_CANCELLED)h->stage="cancelado";
    else if(rc==X4_HTTP_DEADLINE)h->stage="plazo agotado";
    else if(!rc)h->stage="respuesta publica recibida";
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
    bool sigl = !image && n <= X4_HTTP_JSON_URL_MAX && public_sigl_url(url, n);
    bool valid = image ? x4_http_public_image_url(url) != 0 :
        n <= X4_HTTP_JSON_URL_MAX && (sigl || store_url(url, n));
    if (valid) {
        rc = X4_HTTP_UNUSABLE;
        if (!h->unusable) {
            Owned *o = own(X4_HTTP_METHOD_GET, url, NULL, 0, NULL, NULL, 0);
            if (!o) rc = X4_HTTP_ALLOCATION;
            else {
                if (image) o->accept = "image/jpeg,image/png";
                /* SIGLs are one-shot GETs on their own verified connection.
                 * The Store/cover pools retain their existing retry policy. */
                rc = sigl ? transfer(h, o, body, capacity, length, status, cancel) :
                    transfer_public(h, o, image ? 1u : 0u, body, capacity, length, status, cancel);
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

/* Separate from the existing Xbox, Store and release-check request paths.
 * The existing framing parser proves each complete decoded entity. Between
 * feeds only its caller-owned scratch output offset is reset. */
enum { UPDATE_URL_CAP = 4096, UPDATE_MAX_REDIRECTS = 3, UPDATE_FOLLOW = 1 };
typedef struct {
    unsigned char header[X4_HTTP_WIRE_HEADERS_MAX];
    unsigned char input[X4_HTTP_CHUNK], output[X4_HTTP_CHUNK + 1];
    char url[UPDATE_URL_CAP];
} UpdateWire;

static bool update_segment(const char *s, size_t limit)
{
    if (!s) return false;
    const size_t n = bounded_length(s, limit + 1);
    if (!n || n > limit || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (!alphanumeric(c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

static int update_hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool update_target(const char *s)
{
    bool query = false;
    size_t segment = 0, dots = 0;
    if (s[1] == '/') return false;
    for (size_t i = 1; ; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!c || (!query && (c == '/' || c == '?'))) {
            if ((segment == 1 || segment == 2) && segment == dots) return false;
            if (!c) return true;
            query = c == '?';
            segment = dots = 0;
            continue;
        }
        if (c <= 32 || c >= 127 || c == '#' || c == '\\') return false;
        if (c == '%') {
            if (!s[i + 1] || !s[i + 2]) return false;
            const int high = update_hex((unsigned char)s[i + 1]);
            const int low = update_hex((unsigned char)s[i + 2]);
            if (high < 0 || low < 0) return false;
            c = (unsigned char)(high * 16 + low);
            if (!query && (c <= 32 || c >= 127 || c == '/' || c == '\\' || c == '?' || c == '#' || c == '%'))
                return false;
            i += 2;
        }
        if (!query) {
            ++segment;
            if (c == '.') ++dots;
        }
    }
}

static const char *update_host(const char *url, const char **target)
{
    static const char *const hosts[] = {
        "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"
    };
    if (strncmp(url, https_scheme, sizeof(https_scheme) - 1)) return NULL;
    const char *host = url + sizeof(https_scheme) - 1;
    const char *slash = strchr(host, '/');
    if (!slash || !update_target(slash)) return NULL;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i) {
        if (same_word(host, (size_t)(slash - host), hosts[i])) {
            *target = slash;
            return hosts[i];
        }
    }
    return NULL;
}

static bool update_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static int update_read(X4Http *h, X4TlsConn *conn, UpdateWire *wire,
    uint64_t deadline, const _Atomic int *cancel, size_t *count)
{
    *count = 0;
    int rc = checkpoint(cancel, deadline);
    if (rc) return rc;
    const X4TlsBudget b = budget(h, deadline, cancel);
    rc = x4_tls_conn_read(conn, wire->input, sizeof(wire->input), count, &b);
    if (rc) return rc;
    if (*count > sizeof(wire->input)) return X4_HTTP_PROTOCOL;
    return checkpoint(cancel, deadline);
}

/* The temporary header copy locates Location in the final block while
 * retaining coalesced body bytes. http_wire validates all interim blocks. */
static int update_headers(X4Http *h, X4TlsConn *conn, UpdateWire *wire, X4HttpWire *parser,
    uint64_t deadline, const _Atomic int *cancel, size_t *header_size, size_t *begin, size_t *end)
{
    size_t saved = 0;
    *begin = *end = 0;
    for (;;) {
        if (*begin == *end) {
            int rc = update_read(h, conn, wire, deadline, cancel, end);
            if (rc) return rc;
            if (!*end) return X4_HTTP_PROTOCOL;
            *begin = 0;
        }
        if (saved == sizeof(wire->header)) return X4_HTTP_BOUND;
        wire->header[saved++] = wire->input[(*begin)++];
        if (saved < 4 || memcmp(wire->header + saved - 4, "\r\n\r\n", 4)) continue;
        size_t used = 0;
        int rc = x4_http_wire_feed(parser, wire->header, saved, &used);
        if (rc < 0) return rc;
        if (used != saved) return X4_HTTP_PROTOCOL;
        if (x4_http_wire_result(parser).status >= 200) {
            *header_size = saved;
            return 0;
        }
        saved = 0;
    }
}

static int update_location(UpdateWire *wire, size_t header_size)
{
    const char *line = (const char *)wire->header;
    const char *end = line + header_size;
    const char *value = NULL;
    size_t length = 0;
    while (line < end) {
        const char *cr = memchr(line, '\r', (size_t)(end - line));
        if (!cr || cr + 1 == end || cr[1] != '\n') return X4_HTTP_PROTOCOL;
        const char *colon = memchr(line, ':', (size_t)(cr - line));
        if (colon && same_word(line, (size_t)(colon - line), "location")) {
            if (value) return X4_HTTP_UPDATE_REDIRECT;
            value = colon + 1;
            while (value < cr && (*value == ' ' || *value == '\t')) ++value;
            const char *last = cr;
            while (last > value && (last[-1] == ' ' || last[-1] == '\t')) --last;
            length = (size_t)(last - value);
        }
        line = cr + 2;
    }
    if (!length || length >= sizeof(wire->url)) return X4_HTTP_UPDATE_REDIRECT;
    memcpy(wire->url, value, length);
    wire->url[length] = 0;
    const char *target = NULL;
    return update_host(wire->url, &target) ? UPDATE_FOLLOW : X4_HTTP_UPDATE_REDIRECT;
}

static int update_body(X4Http *h, X4TlsConn *conn, UpdateWire *wire, X4HttpWire *parser,
    size_t begin, size_t end, size_t maximum, X4HttpUpdateSink sink, void *context,
    uint64_t deadline, const _Atomic int *cancel, size_t *total)
{
    if (parser->cl_seen && parser->content_length > maximum) return X4_HTTP_BOUND;
    for (;;) {
        if (parser->complete && begin == end) return checkpoint(cancel, deadline);
        if (begin == end) {
            int rc = update_read(h, conn, wire, deadline, cancel, &end);
            if (rc) return rc;
            begin = 0;
        }
        size_t used = 0;
        const size_t available = end - begin;
        const int rc = available ? x4_http_wire_feed(parser, wire->input + begin, available, &used)
                                 : x4_http_wire_eof(parser);
        if (rc < 0) return rc;
        if (used != available) return X4_HTTP_PROTOCOL;
        const size_t bytes = parser->body_bytes;
        if (bytes > maximum - *total) return X4_HTTP_BOUND;
        if (bytes) {
            const int write_rc = sink(context, wire->output, bytes);
            if (write_rc) return write_rc < 0 ? write_rc : X4_HTTP_ARGUMENT;
            *total += bytes;
            parser->body_bytes = 0;
        }
        if (rc == X4_HTTP_WIRE_DONE) return checkpoint(cancel, deadline);
        begin = end;
    }
}

static int update_hop(X4Http *h, UpdateWire *wire, size_t maximum, X4HttpUpdateSink sink,
    void *context, uint64_t deadline, const _Atomic int *cancel, size_t *total)
{
    const char *target = NULL;
    const char *host = update_host(wire->url, &target);
    if (!host) return X4_HTTP_UPDATE_REDIRECT;
    X4TlsConn *conn = NULL;
    X4TlsBudget b = budget(h, deadline, cancel);
    int rc = checkpoint(cancel, deadline);
    if (!rc) rc = x4_tls_conn_connect(h->tls, host, &b, &conn);
    if (!rc) {
        const X4HttpWireRequest request = {.method = "GET", .host = host, .target = target,
            .accept = "application/octet-stream", .user_agent = "XCloud4"};
        RequestBuffer header = {.data = wire->header, .capacity = sizeof(wire->header)};
        rc = x4_http_wire_write_request(&request, collect_header, &header);
        if (!rc) rc = x4_tls_conn_write(conn, header.data, header.size, &b);
    }
    if (!rc) {
        X4HttpWire parser;
        size_t header_size = 0, begin = 0, end = 0;
        x4_http_wire_init(&parser, wire->output, sizeof(wire->output), 0);
        rc = update_headers(h, conn, wire, &parser, deadline, cancel, &header_size, &begin, &end);
        if (!rc) {
            const int status = x4_http_wire_result(&parser).status;
            if (status == 200)
                rc = update_body(h, conn, wire, &parser, begin, end, maximum, sink, context,
                    deadline, cancel, total);
            else if (update_redirect_status(status))
                rc = update_location(wire, header_size);
            else rc = X4_HTTP_UPDATE_STATUS;
        }
    }
    if (!release_connection(h, &conn)) return X4_HTTP_UNUSABLE;
    return rc;
}

X4Http *x4_http_update_open(int *error)
{
    X4Http *h = x4_http_open(error);
    if (!h) return NULL;
    const int rc = x4_tls_http_enable_update_ca(h->tls);
    if (rc) {
        x4_http_close(h);
        if (error) *error = rc;
        return NULL;
    }
    h->update_only = true;
    return h;
}

int x4_http_update_asset(X4Http *h, const char *tag, const char *asset_name,
    size_t maximum, X4HttpUpdateSink sink, void *context, size_t *received,
    const _Atomic int *cancel)
{
    if (received) *received = 0;
    if (!h || !h->update_only || !sink || !received || !maximum || maximum > X4_HTTP_UPDATE_BODY_MAX)
        return X4_HTTP_ARGUMENT;
    if (!update_segment(tag, 63) || !update_segment(asset_name, 128)) return X4_HTTP_URL;
    if (atomic_exchange(&h->busy, true)) return X4_HTTP_BUSY;
    int rc = h->unusable ? X4_HTTP_UNUSABLE : 0;
    UpdateWire *wire = NULL;
    if (!rc) {
        wire = malloc(sizeof(*wire));
        if (!wire) rc = X4_HTTP_ALLOCATION;
    }
    if (!rc) {
        const int size = snprintf(wire->url, sizeof(wire->url),
            "https://github.com/asp11edgr/XCloud4/releases/download/%s/%s", tag, asset_name);
        if (size < 0 || (size_t)size >= sizeof(wire->url)) rc = X4_HTTP_URL;
    }
    if (!rc) {
        const uint64_t interval = UINT64_C(15) * 60 * 1000000;
        const uint64_t now = sceKernelGetProcessTime();
        const uint64_t deadline = now > UINT64_MAX - interval ? UINT64_MAX : now + interval;
        size_t total = 0;
        for (unsigned hop = 0; ; ++hop) {
            rc = update_hop(h, wire, maximum, sink, context, deadline, cancel, &total);
            if (rc != UPDATE_FOLLOW) break;
            if (hop == UPDATE_MAX_REDIRECTS) {
                rc = X4_HTTP_UPDATE_REDIRECT;
                break;
            }
        }
        if (!rc) *received = total;
    }
    if (wire) {
        x4_secure_clear(wire, sizeof(*wire));
        free(wire);
    }
    atomic_store(&h->busy, false);
    return rc;
}
