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
#define X4_HTTP_HEADER_OVERWRITE 0u
/* SERVER_VERIFY | CN_CHECK | NOT_AFTER | NOT_BEFORE | KNOWN_CA | SNI */
#define X4_HTTPS_VERIFY_ALL 0xBDu

static const char allowed_prefix[] = "https://login.microsoftonline.com/consumers/";
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

    /* Keep request input alive through deletion, including failed cleanup.
     * In quarantine these copies remain owned by the process until exit. */
    char *owned_url = malloc(strlen(url) + 1);
    char *owned_form = form ? malloc(form_size + 1) : NULL;
    if (!owned_url || (form && !owned_form)) {
        free(owned_url); free(owned_form);
        h->stage = "memoria de solicitud";
        return X4_HTTP_ALLOCATION;
    }
    strcpy(owned_url, url);
    if (owned_form) memcpy(owned_form, form, form_size + 1);
    url = owned_url;
    form = owned_form;

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
    CREATE("solicitud HTTPS", req, h->fn.create_request(conn,
        form ? X4_HTTP_METHOD_POST : X4_HTTP_METHOD_GET, url, (uint64_t)form_size));
    OPTION("cabeceras", h->fn.add_header(req, "Accept", "application/json", X4_HTTP_HEADER_OVERWRITE));
    OPTION("cabeceras", h->fn.add_header(req, "Accept-Encoding", "identity", X4_HTTP_HEADER_OVERWRITE));
    if (form) OPTION("cabeceras", h->fn.add_header(req, "Content-Type",
        "application/x-www-form-urlencoded", X4_HTTP_HEADER_OVERWRITE));

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
    if (release_request(h, &tmpl, &conn, &req)) {
        x4_secure_clear(owned_form, form_size);
        free(owned_form); free(owned_url);
    } else if (rc == 0) h->stage = "recursos HTTP en cuarentena";
    return rc;
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
