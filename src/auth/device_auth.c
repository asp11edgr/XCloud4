/* SPDX-License-Identifier: GPL-3.0-only */
#include "device_auth.h"
#include "auth_profile.h"
#include "http_client.h"
#include "json.h"
#include "xbox_live.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

/* X4_AUTH_CLIENT_ID comes from auth_profile.h: one compile-time profile
 * (own registration or the temporary reference client) for the whole
 * process. Device code, polling, renewal and Passport all use it. */
#define X4_AUTH_SCOPE "xboxlive.signin openid profile offline_access"
#define X4_AUTH_GRANT "urn:ietf:params:oauth:grant-type:device_code"
#define X4_AUTH_DEVICE_URL "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode"
#define X4_AUTH_TOKEN_URL "https://login.microsoftonline.com/consumers/oauth2/v2.0/token"
#define X4_AUTH_METADATA_URL "https://login.microsoftonline.com/consumers/v2.0/.well-known/openid-configuration"
#define X4_AUTH_REFRESH_GRANT "refresh_token"
/* Service scope of the console-transfer token sent to a session's /connect;
 * requested with the same profile client ID that obtained the refresh token
 * through this process's own device sign-in; no cross-client refresh. */
#define X4_AUTH_PASSPORT_SCOPE "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN"
#define X4_AUTH_USEC 1000000ull
#define X4_AUTH_STEP_USEC 100000u
#define X4_AUTH_DEFAULT_INTERVAL 5u
#define X4_AUTH_TOKEN_SIZE 16384

/* The only verification pages ever shown; anything else is refused. */
static const char *const verification_uris[] = {
    "https://microsoft.com/devicelogin",
    "https://www.microsoft.com/link",
    "https://www.microsoft.com/devicelogin",
};

/* Heap workspace, one per worker run, so the native default stack suffices. */
typedef struct {
    char response[65536];
    char device_code[2048];
    char form[8193];
} X4AuthWork;

/* Heap workspace of one connection authorization; wiped and freed on every
 * path. Candidates hold a renewal until it is fully validated. */
typedef struct {
    char response[X4_HTTP_PASSPORT_BODY_MAX + 1];
    char form[X4_HTTP_FORM_MAX + 1];
    char access[X4_AUTH_TOKEN_SIZE], refresh[X4_AUTH_TOKEN_SIZE];
} X4PassportWork;

typedef struct {
    X4AuthSnapshot view;
    uint64_t code_deadline; /* monotonic usec, WAITING only */
} Shared;

typedef struct {
    enum X4AuthState state;
    int error, http_status;
    char stage[80];
} Outcome;

struct X4Auth {
    /* Main thread only. */
    OrbisPthread thread;
    int running;
    /* Set by the main thread while no worker exists, then owned by the worker
     * until it releases finished; main reads them only after acquiring it. */
    enum X4AuthAction action;
    X4AuthWork *work;
    X4XboxWork *xwork; /* catalog and session runs only */
    /* Private session run arguments, copied from the catalog before start. */
    X4CatalogTitle session_title;
    char session_offering[24];
    uint64_t token_expiry; /* monotonic usec, 0 when no token is held */
    char access_token[X4_AUTH_TOKEN_SIZE], refresh_token[X4_AUTH_TOKEN_SIZE];
    _Atomic int cancel, finished;
    /* Guards copies of shared/catalog/session and the input mailbox only. */
    atomic_flag lock;
    Shared shared;
    X4CatalogSnapshot catalog;
    X4SessionSnapshot session;
    X4GamepadFrame gamepad;
    uint64_t gamepad_updated;
    X4SessionMediaCallback media_callback;
    void *media_user;
    _Atomic int keyframe_requested;
};

static uint64_t now(void)
{
    return sceKernelGetProcessTime();
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    size_t n = 0;
    if (src) while (n + 1 < capacity && src[n]) { dst[n] = src[n]; ++n; }
    dst[n] = 0;
}

static void lock(X4Auth *a)
{
    while (atomic_flag_test_and_set_explicit(&a->lock, memory_order_acquire)) sceKernelUsleep(50);
}

static void unlock(X4Auth *a)
{
    atomic_flag_clear_explicit(&a->lock, memory_order_release);
}

void x4_auth_set_gamepad(X4Auth *a, const X4GamepadFrame *frame)
{
    if (!a) return;
    X4GamepadFrame next = {0};
    if (frame) next = *frame;
    uint64_t t = now();
    lock(a);
    a->gamepad = next;
    a->gamepad_updated = t;
    unlock(a);
}

/* Called only by the RTC-owned sender, outside its gate. Auth stays alive
 * until that sender has joined and the session worker has completed. */
static bool gamepad_source(void *user, X4GamepadFrame *out)
{
    X4Auth *a = user;
    if (!a || !out) return false;
    uint64_t t = now(), updated;
    lock(a);
    *out = a->gamepad;
    updated = a->gamepad_updated;
    unlock(a);
    if (!updated || t < updated || t - updated > 250000 || atomic_load(&a->cancel)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return out->connected;
}

static void publish(X4Auth *a, enum X4AuthState state, int error, int status, const char *stage,
    const char *user_code, const char *uri, uint64_t code_deadline)
{
    Shared next = {0};
    next.view.state = state;
    next.view.error = error;
    next.view.http_status = status;
    copy_text(next.view.stage, sizeof(next.view.stage), stage);
    copy_text(next.view.user_code, sizeof(next.view.user_code), user_code);
    copy_text(next.view.verification_uri, sizeof(next.view.verification_uri), uri);
    next.code_deadline = code_deadline;
    lock(a);
    a->shared = next;
    unlock(a);
}

static void wipe_tokens(X4Auth *a)
{
    x4_secure_clear(a->access_token, sizeof(a->access_token));
    x4_secure_clear(a->refresh_token, sizeof(a->refresh_token));
    a->token_expiry = 0;
}

/* Empty catalog view in one state. */
static void reset_catalog(X4Auth *a, enum X4CatalogState state, int error, const char *stage)
{
    lock(a);
    memset(&a->catalog, 0, sizeof(a->catalog));
    a->catalog.state = state;
    a->catalog.error = error;
    copy_text(a->catalog.stage, sizeof(a->catalog.stage), stage);
    unlock(a);
}

static void publish_catalog(X4Auth *a, const X4CatalogSnapshot *next)
{
    lock(a);
    a->catalog = *next;
    unlock(a);
}

/* Worker progress: stage, offering and region only. */
static void catalog_progress(void *context, const char *stage, const char *offering, const char *region)
{
    X4Auth *a = context;
    lock(a);
    copy_text(a->catalog.stage, sizeof(a->catalog.stage), stage);
    copy_text(a->catalog.offering, sizeof(a->catalog.offering), offering);
    copy_text(a->catalog.region, sizeof(a->catalog.region), region);
    unlock(a);
}

/* Empty session view in one state; title, offering and region optional. */
static void reset_session(X4Auth *a, enum X4SessionState state, int error, const char *stage,
    const char *title, const char *offering, const char *region)
{
    X4SessionSnapshot next;
    memset(&next, 0, sizeof(next));
    next.state = state;
    next.error = error;
    copy_text(next.stage, sizeof(next.stage), stage);
    copy_text(next.title_name, sizeof(next.title_name), title);
    copy_text(next.offering, sizeof(next.offering), offering);
    copy_text(next.region, sizeof(next.region), region);
    lock(a);
    a->session = next;
    unlock(a);
}

/* Worker progress and final view: a whole public copy, taken under lock. */
static void session_progress(void *context, const X4SessionSnapshot *snapshot)
{
    X4Auth *a = context;
    lock(a);
    memcpy(&a->session, snapshot, sizeof(a->session));
    unlock(a);
}

/* Main thread, only while no worker runs: token and catalog go together. */
static void discard_session(X4Auth *a)
{
    wipe_tokens(a);
    reset_catalog(a, X4_CATALOG_IDLE, 0, "sin catalogo");
}

/* Records a terminal outcome; returns -1 so callers can `return set(...)`. */
static int set(Outcome *o, enum X4AuthState state, int error, int status, const char *stage)
{
    o->state = state;
    o->error = error;
    o->http_status = status;
    copy_text(o->stage, sizeof(o->stage), stage);
    return -1;
}

static bool unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~';
}

/* Appends key=value, both percent-encoded: RFC 3986 unreserved bytes pass,
 * every other byte (space included) becomes %HH. */
static bool form_add(char *form, size_t capacity, size_t *used, const char *key, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *parts[2] = {key, value};
    size_t n = *used;
    for (int i = 0; i < 2; ++i) {
        if (i || n) {
            if (n + 1 >= capacity) return false;
            form[n++] = i ? '=' : '&';
        }
        for (const unsigned char *p = (const unsigned char *)parts[i]; *p; ++p) {
            if (unreserved(*p)) {
                if (n + 1 >= capacity) return false;
                form[n++] = (char)*p;
            } else {
                if (n + 3 >= capacity) return false;
                form[n++] = '%';
                form[n++] = hex[*p >> 4];
                form[n++] = hex[*p & 15];
            }
        }
    }
    form[n] = 0;
    *used = n;
    return true;
}

/* Polls cancel and expiry every 100 ms; the UI thread is never involved. */
static int wait_until(X4Auth *a, uint64_t until, uint64_t deadline, Outcome *o)
{
    for (;;) {
        if (atomic_load(&a->cancel)) return set(o, X4_AUTH_CANCELLED, 0, 0, "acceso cancelado");
        uint64_t t = now();
        if (t >= deadline) return set(o, X4_AUTH_EXPIRED, 0, 0, "el codigo caduco");
        if (t >= until) return 0;
        sceKernelUsleep(X4_AUTH_STEP_USEC);
    }
}

/* One HTTPS exchange bracketed by cancel and expiry checks. Returns 0 with
 * the body in w->response; otherwise o holds the outcome and the response is
 * wiped. The form is wiped either way. */
static int exchange(X4Auth *a, X4AuthWork *w, X4Http *http, const char *url, bool post,
    uint64_t deadline, Outcome *o, size_t *length, int *status, uint64_t *done)
{
    int rc;
    *length = 0;
    *status = 0;
    if (atomic_load(&a->cancel)) rc = set(o, X4_AUTH_CANCELLED, 0, 0, "acceso cancelado");
    else if (now() >= deadline) rc = set(o, X4_AUTH_EXPIRED, 0, 0, "el codigo caduco");
    else {
        int native = x4_http_request(http, url, post ? w->form : NULL, w->response,
            sizeof(w->response), length, status, &a->cancel);
        *done = now();
        if (native == X4_HTTP_CANCELLED || atomic_load(&a->cancel))
            rc = set(o, X4_AUTH_CANCELLED, 0, 0, "acceso cancelado");
        else if (*done >= deadline) rc = set(o, X4_AUTH_EXPIRED, 0, 0, "el codigo caduco");
        else if (native) {
            /* Stage is a literal owned by http; copy it while http is alive. */
            char stage[80];
            snprintf(stage, sizeof(stage), "HTTPS: %s", x4_http_stage(http));
            rc = set(o, X4_AUTH_ERROR, native, 0, stage);
        } else rc = 0;
    }
    x4_secure_clear(w->form, sizeof(w->form));
    if (rc) {
        x4_secure_clear(w->response, sizeof(w->response));
        *length = 0;
    }
    return rc;
}

/* Reads only the "error" member; the caller wipes the body. */
static bool error_code(const char *json, size_t length, char *code, size_t capacity)
{
    X4JsonField f = {.name = "error", .kind = X4_JSON_STRING, .text = code, .capacity = capacity};
    bool ok = x4_json_fields(json, length, &f, 1) == 0 && f.found && code[0];
    if (!ok) code[0] = 0;
    return ok;
}

/* Reads only the "error" member; the body is wiped afterwards. */
static bool oauth_error(X4AuthWork *w, size_t length, char *code, size_t capacity)
{
    bool ok = error_code(w->response, length, code, capacity);
    x4_secure_clear(w->response, sizeof(w->response));
    return ok;
}

/* Only exact known codes get a message; everything else is generic. */
static const char *rejection(const char *code)
{
    if (!strcmp(code, "invalid_client") || !strcmp(code, "unauthorized_client"))
        return "Microsoft no acepta esta aplicacion";
    if (!strcmp(code, "invalid_scope")) return "Microsoft rechazo los permisos pedidos";
    return "Microsoft rechazo la solicitud";
}

/* Any non-200 answer: OAuth errors are trusted only in a 400 body. */
static void refused(X4AuthWork *w, size_t length, int status, Outcome *o)
{
    char code[64] = {0};
    if (status != 400) set(o, X4_AUTH_ERROR, X4_AUTH_E_STATUS, status, "estado HTTP inesperado");
    else if (!oauth_error(w, length, code, sizeof(code)))
        set(o, X4_AUTH_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta de error no valida");
    else set(o, X4_AUTH_ERROR, X4_AUTH_E_REJECTED, status, rejection(code));
    x4_secure_clear(w->response, sizeof(w->response));
}

static const char *known_uri(const char *uri)
{
    for (size_t i = 0; i < sizeof(verification_uris) / sizeof(verification_uris[0]); ++i)
        if (!strcmp(uri, verification_uris[i])) return verification_uris[i];
    return NULL;
}

static bool valid_user_code(const char *code)
{
    if (!code[0]) return false;
    for (const char *p = code; *p; ++p) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

/* Case-insensitive, as RFC 6749 token types are. */
static bool bearer(const char *type)
{
    static const char expected[] = "bearer";
    for (size_t i = 0; i < sizeof(expected); ++i) {
        char c = type[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != expected[i]) return false;
    }
    return true;
}

/* Host check only: metadata may substitute a tenant ID in the path, and no
 * returned endpoint is ever used. */
static bool microsoft_https(const char *url)
{
    static const char prefix[] = "https://login.microsoftonline.com";
    size_t n = sizeof(prefix) - 1;
    return !strncmp(url, prefix, n) && (url[n] == '/' || url[n] == 0);
}

/* Tokens are parsed straight into the private context buffers; any failure
 * wipes them before returning. */
static void token_response(X4Auth *a, X4AuthWork *w, size_t length, uint64_t sent, Outcome *o)
{
    char type[16] = {0};
    X4JsonField f[] = {
        {.name = "token_type", .kind = X4_JSON_STRING, .text = type, .capacity = sizeof(type)},
        {.name = "access_token", .kind = X4_JSON_STRING, .text = a->access_token,
            .capacity = sizeof(a->access_token)},
        {.name = "refresh_token", .kind = X4_JSON_STRING, .text = a->refresh_token,
            .capacity = sizeof(a->refresh_token)},
        {.name = "expires_in", .kind = X4_JSON_UINT},
    };
    int rc = x4_json_fields(w->response, length, f, sizeof(f) / sizeof(f[0]));
    x4_secure_clear(w->response, sizeof(w->response));
    if (rc || !f[0].found || !bearer(type) || !f[1].found || !a->access_token[0] ||
        !f[3].found || f[3].number < 1 || f[3].number > 86400) {
        wipe_tokens(a);
        set(o, X4_AUTH_ERROR, X4_AUTH_E_RESPONSE, 200, "respuesta de token no valida");
        return;
    }
    /* Lifetime counted from before the request was sent. */
    a->token_expiry = sent + f[3].number * X4_AUTH_USEC;
    set(o, X4_AUTH_AUTHORIZED, 0, 200, "cuenta Microsoft autorizada");
}

static void sign_in(X4Auth *a, X4AuthWork *w, X4Http *http, Outcome *o)
{
    /* Conservative: the code lifetime counts from before it was requested. */
    uint64_t start = now(), done = 0;
    size_t length = 0, used = 0;
    int status = 0;
    char user_code[32] = {0}, uri[256] = {0}, code[64] = {0};
    publish(a, X4_AUTH_CONNECTING, 0, 0, "solicitando codigo a Microsoft", NULL, NULL, 0);
    if (!form_add(w->form, sizeof(w->form), &used, "client_id", X4_AUTH_CLIENT_ID) ||
        !form_add(w->form, sizeof(w->form), &used, "scope", X4_AUTH_SCOPE)) {
        set(o, X4_AUTH_ERROR, X4_AUTH_E_FORM, 0, "formulario de codigo");
        return;
    }
    if (exchange(a, w, http, X4_AUTH_DEVICE_URL, true, UINT64_MAX, o, &length, &status, &done)) return;
    if (status != 200) { refused(w, length, status, o); return; }

    X4JsonField f[] = {
        {.name = "device_code", .kind = X4_JSON_STRING, .text = w->device_code,
            .capacity = sizeof(w->device_code)},
        {.name = "user_code", .kind = X4_JSON_STRING, .text = user_code, .capacity = sizeof(user_code)},
        {.name = "verification_uri", .kind = X4_JSON_STRING, .text = uri, .capacity = sizeof(uri)},
        {.name = "expires_in", .kind = X4_JSON_UINT},
        {.name = "interval", .kind = X4_JSON_UINT},
    };
    int rc = x4_json_fields(w->response, length, f, sizeof(f) / sizeof(f[0]));
    x4_secure_clear(w->response, sizeof(w->response));
    uint64_t interval = f[4].found ? f[4].number : X4_AUTH_DEFAULT_INTERVAL;
    if (rc || !f[0].found || !w->device_code[0] || !f[1].found || !valid_user_code(user_code) ||
        !f[2].found || !f[3].found || f[3].number < 1 || f[3].number > 3600 ||
        interval < 1 || interval > 120) {
        set(o, X4_AUTH_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta de codigo no valida");
        return;
    }
    const char *shown = known_uri(uri);
    if (!shown) {
        set(o, X4_AUTH_ERROR, X4_AUTH_E_RESPONSE, status, "pagina de verificacion desconocida");
        return;
    }
    uint64_t deadline = start + f[3].number * X4_AUTH_USEC;
    publish(a, X4_AUTH_WAITING, 0, status, "esperando autorizacion", user_code, shown, deadline);

    /* The interval runs from the end of one request to the start of the next,
     * so the first poll also waits one full interval. */
    uint64_t next = done + interval * X4_AUTH_USEC;
    for (;;) {
        if (wait_until(a, next, deadline, o)) return;
        used = 0;
        if (!form_add(w->form, sizeof(w->form), &used, "grant_type", X4_AUTH_GRANT) ||
            !form_add(w->form, sizeof(w->form), &used, "client_id", X4_AUTH_CLIENT_ID) ||
            !form_add(w->form, sizeof(w->form), &used, "device_code", w->device_code)) {
            set(o, X4_AUTH_ERROR, X4_AUTH_E_FORM, 0, "formulario de token");
            return;
        }
        uint64_t sent = now();
        if (exchange(a, w, http, X4_AUTH_TOKEN_URL, true, deadline, o, &length, &status, &done)) return;
        if (status == 200) { token_response(a, w, length, sent, o); return; }
        if (status != 400) { refused(w, length, status, o); return; }
        if (!oauth_error(w, length, code, sizeof(code))) {
            set(o, X4_AUTH_ERROR, X4_AUTH_E_RESPONSE, status, "respuesta de error no valida");
            return;
        }
        if (!strcmp(code, "authorization_pending")) {
        } else if (!strcmp(code, "slow_down")) {
            interval += 5;
        } else if (!strcmp(code, "authorization_declined") || !strcmp(code, "access_denied")) {
            set(o, X4_AUTH_DENIED, 0, status, "acceso rechazado en la cuenta");
            return;
        } else if (!strcmp(code, "expired_token")) {
            set(o, X4_AUTH_EXPIRED, 0, status, "el codigo caduco");
            return;
        } else if (!strcmp(code, "bad_verification_code")) {
            set(o, X4_AUTH_ERROR, X4_AUTH_E_REJECTED, status, "Microsoft no reconoce el codigo");
            return;
        } else {
            set(o, X4_AUTH_ERROR, X4_AUTH_E_REJECTED, status, rejection(code));
            return;
        }
        next = done + interval * X4_AUTH_USEC;
    }
}

/* Proves a verified TLS exchange with Microsoft; signs nobody in. */
static void check_connection(X4Auth *a, X4AuthWork *w, X4Http *http, Outcome *o)
{
    char issuer[256] = {0}, endpoint[256] = {0};
    size_t length = 0;
    int status = 0;
    uint64_t done = 0;
    if (exchange(a, w, http, X4_AUTH_METADATA_URL, false, UINT64_MAX, o, &length, &status, &done)) return;
    if (status != 200) {
        x4_secure_clear(w->response, sizeof(w->response));
        set(o, X4_AUTH_ERROR, X4_AUTH_E_STATUS, status, "estado HTTP inesperado");
        return;
    }
    X4JsonField f[] = {
        {.name = "issuer", .kind = X4_JSON_STRING, .text = issuer, .capacity = sizeof(issuer)},
        {.name = "token_endpoint", .kind = X4_JSON_STRING, .text = endpoint, .capacity = sizeof(endpoint)},
    };
    int rc = x4_json_fields(w->response, length, f, sizeof(f) / sizeof(f[0]));
    x4_secure_clear(w->response, sizeof(w->response));
    if (rc || !f[0].found || !f[1].found || !microsoft_https(issuer) || !microsoft_https(endpoint)) {
        set(o, X4_AUTH_ERROR, X4_AUTH_E_RESPONSE, status, "metadatos de Microsoft no validos");
        return;
    }
    set(o, X4_AUTH_CONNECTED, 0, status, "TLS con Microsoft verificado");
}

/* Non-empty visible ASCII (no space or control byte) inside capacity. */
static bool visible_token(const char *t, size_t capacity)
{
    size_t n = 0;
    while (n < capacity && t[n]) {
        unsigned char c = (unsigned char)t[n];
        if (c < 0x21 || c > 0x7e) return false;
        ++n;
    }
    return n > 0 && n < capacity;
}

/* Renews the Microsoft tokens with the stored refresh token. Once sent the
 * request is not cancellable (cancel NULL, bounded by the HTTP layer's 30 s
 * deadline and native timeouts): a rotated refresh token must be read and
 * kept, never dropped midway. The stored tokens are replaced only by a fully
 * validated answer; a transport error or malformed answer keeps them. Only
 * invalid_grant ends the local Microsoft session, never a global sign-out. */
static int refresh_tokens(X4Auth *a, X4PassportWork *p, X4Http *http, int *http_status, const char **stage)
{
    size_t used = 0, length = 0;
    int status = 0;
    *stage = "Microsoft no pudo renovar el acceso";
    if (!visible_token(a->refresh_token, sizeof(a->refresh_token))) {
        *stage = "Microsoft no entrego renovacion de acceso";
        return X4_AUTH_E_SIGNED_OUT;
    }
    if (!form_add(p->form, sizeof(p->form), &used, "client_id", X4_AUTH_CLIENT_ID) ||
        !form_add(p->form, sizeof(p->form), &used, "grant_type", X4_AUTH_REFRESH_GRANT) ||
        !form_add(p->form, sizeof(p->form), &used, "refresh_token", a->refresh_token) ||
        !form_add(p->form, sizeof(p->form), &used, "scope", X4_AUTH_SCOPE)) {
        x4_secure_clear(p->form, sizeof(p->form));
        return X4_AUTH_E_FORM;
    }
    /* Lifetime counted from before the request was sent. */
    uint64_t sent = now();
    int rc = x4_http_request(http, X4_AUTH_TOKEN_URL, p->form, p->response, sizeof(p->response), &length,
        &status, NULL);
    x4_secure_clear(p->form, sizeof(p->form));
    *http_status = status;
    if (rc) return rc;
    if (status != 200) {
        char code[64] = {0};
        bool known = status == 400 && error_code(p->response, length, code, sizeof(code));
        x4_secure_clear(p->response, sizeof(p->response));
        bool expired = known && !strcmp(code, "invalid_grant");
        x4_secure_clear(code, sizeof(code));
        printf("XCloud4: renovacion Microsoft rechazada, estado HTTP %d\n", status);
        if (!expired) return known ? X4_AUTH_E_REJECTED : X4_AUTH_E_STATUS;
        /* This worker owns the tokens until it releases finished; main only
         * reads them after acquiring it. The catalog goes with the token. */
        wipe_tokens(a);
        reset_catalog(a, X4_CATALOG_IDLE, 0, "sin catalogo");
        publish(a, X4_AUTH_EXPIRED, 0, status, "la sesion de Microsoft caduco", NULL, NULL, 0);
        *stage = "la sesion de Microsoft caduco";
        return X4_AUTH_E_SIGNED_OUT;
    }
    char type[16] = {0};
    X4JsonField f[] = {
        {.name = "token_type", .kind = X4_JSON_STRING, .text = type, .capacity = sizeof(type)},
        {.name = "access_token", .kind = X4_JSON_STRING, .text = p->access, .capacity = sizeof(p->access)},
        {.name = "refresh_token", .kind = X4_JSON_STRING, .text = p->refresh, .capacity = sizeof(p->refresh)},
        {.name = "expires_in", .kind = X4_JSON_UINT},
    };
    rc = x4_json_fields(p->response, length, f, sizeof(f) / sizeof(f[0]));
    x4_secure_clear(p->response, sizeof(p->response));
    /* A missing refresh_token keeps the current one; a present one must be
     * usable, since it replaces the current one. */
    bool valid = rc == 0 && f[0].found && bearer(type) && f[1].found &&
        visible_token(p->access, sizeof(p->access)) && f[3].found && f[3].number >= 1 &&
        f[3].number <= 86400 && (!f[2].found || visible_token(p->refresh, sizeof(p->refresh)));
    if (!valid) {
        x4_secure_clear(p->access, sizeof(p->access));
        x4_secure_clear(p->refresh, sizeof(p->refresh));
        *stage = "renovacion de Microsoft no valida";
        return X4_AUTH_E_RESPONSE;
    }
    memcpy(a->access_token, p->access, sizeof(a->access_token));
    if (f[2].found) memcpy(a->refresh_token, p->refresh, sizeof(a->refresh_token));
    a->token_expiry = sent + f[3].number * X4_AUTH_USEC;
    x4_secure_clear(p->access, sizeof(p->access));
    x4_secure_clear(p->refresh, sizeof(p->refresh));
    printf("XCloud4: acceso Microsoft renovado, estado HTTP %d\n", status);
    return 0;
}

/* Passport (login.live.com) refusals: only these exact OAuth codes are named;
 * each stage is a fixed literal. invalid_grant here does not prove the
 * Microsoft session expired: the renewal just before it succeeded. */
static const struct { const char *code, *stage; } passport_errors[] = {
    {"invalid_grant", "Passport: renovacion no aceptada (invalid_grant)"},
    {"invalid_scope", "Passport: permiso rechazado (invalid_scope)"},
    {"invalid_client", "Passport: aplicacion no aceptada (invalid_client)"},
    {"unauthorized_client", "Passport: aplicacion no autorizada (unauthorized_client)"},
    {"invalid_request", "Passport: solicitud no valida (invalid_request)"},
    {"access_denied", "Passport: acceso denegado (access_denied)"},
    {"unsupported_grant_type", "Passport: concesion no admitida (unsupported_grant_type)"},
    {"temporarily_unavailable", "Passport: servicio no disponible (temporarily_unavailable)"},
    {"server_error", "Passport: error del servidor (server_error)"},
};

/* Classifies a non-200 Passport body before the caller wipes it. Returns the
 * allowlisted code literal or "unknown" and sets *stage to a fixed literal.
 * *subcode is the first error_codes element when it is a strict uint32;
 * nothing else from the body is read, kept or logged. */
static const char *passport_refusal(const char *body, size_t length, const char **stage, uint32_t *subcode,
    bool *has_subcode)
{
    char code[32] = {0};
    const char *name = "unknown";
    *stage = "Passport: rechazo sin codigo conocido (unknown)";
    *subcode = 0;
    *has_subcode = false;
    if (error_code(body, length, code, sizeof(code))) {
        for (size_t i = 0; i < sizeof(passport_errors) / sizeof(passport_errors[0]); ++i)
            if (!strcmp(code, passport_errors[i].code)) {
                name = passport_errors[i].code;
                *stage = passport_errors[i].stage;
                break;
            }
    }
    x4_secure_clear(code, sizeof(code));
    X4JsonSpan root, codes, first;
    size_t cursor = 0;
    if (x4_json_parse(body, length, &root) == 0 && x4_json_type(root) == X4_JSON_T_OBJECT &&
        x4_json_member(root, "error_codes", &codes) == 1 && x4_json_item(codes, &cursor, &first) == 1 &&
        x4_json_uint32(first, subcode) == 0)
        *has_subcode = true;
    else *subcode = 0;
    return name;
}

/* Console-transfer token from the (possibly rotated) refresh token, written
 * to out only. Any refresh token in this answer is service specific and
 * ignored: it never replaces the Microsoft one. */
static int passport_token(X4Auth *a, X4PassportWork *p, X4Http *http, char *out, size_t capacity,
    int *http_status, const char **stage, const _Atomic int *cancel)
{
    size_t used = 0, length = 0;
    int status = 0;
    *stage = "Microsoft rechazo la autorizacion de conexion";
    if (!form_add(p->form, sizeof(p->form), &used, "client_id", X4_AUTH_CLIENT_ID) ||
        !form_add(p->form, sizeof(p->form), &used, "scope", X4_AUTH_PASSPORT_SCOPE) ||
        !form_add(p->form, sizeof(p->form), &used, "grant_type", X4_AUTH_REFRESH_GRANT) ||
        !form_add(p->form, sizeof(p->form), &used, "refresh_token", a->refresh_token)) {
        x4_secure_clear(p->form, sizeof(p->form));
        *stage = "formulario de autorizacion de conexion";
        return X4_AUTH_E_FORM;
    }
    int rc = x4_http_passport_request(http, p->form, p->response, sizeof(p->response), &length, &status, cancel);
    x4_secure_clear(p->form, sizeof(p->form));
    *http_status = status;
    if (rc == X4_HTTP_CANCELLED || atomic_load(cancel)) {
        x4_secure_clear(p->response, sizeof(p->response));
        *stage = "autorizacion cancelada";
        return X4_XBOX_PASSPORT_CANCELLED;
    }
    if (rc) {
        *stage = "Microsoft no pudo autorizar la conexion";
        return rc;
    }
    if (status != 200) {
        uint32_t subcode = 0;
        bool has_subcode = false;
        const char *name = passport_refusal(p->response, length, stage, &subcode, &has_subcode);
        x4_secure_clear(p->response, sizeof(p->response));
        if (has_subcode)
            printf("XCloud4: autorizacion de conexion rechazada, estado HTTP %d codigo %s subcodigo %u\n",
                status, name, (unsigned)subcode);
        else printf("XCloud4: autorizacion de conexion rechazada, estado HTTP %d codigo %s\n", status, name);
        return status == 400 ? X4_AUTH_E_REJECTED : X4_AUTH_E_STATUS;
    }
    X4JsonField f = {.name = "access_token", .kind = X4_JSON_STRING, .text = out,
        .capacity = capacity < X4_AUTH_TOKEN_SIZE ? capacity : X4_AUTH_TOKEN_SIZE};
    rc = x4_json_fields(p->response, length, &f, 1);
    x4_secure_clear(p->response, sizeof(p->response));
    if (rc || !f.found || !visible_token(out, f.capacity)) {
        x4_secure_clear(out, capacity);
        *stage = "autorizacion de conexion no valida";
        return X4_AUTH_E_RESPONSE;
    }
    printf("XCloud4: autorizacion de conexion obtenida, estado HTTP %d\n", status);
    return 0;
}

/* X4XboxPassport for session runs: same worker, same HTTPS context. The
 * Microsoft tokens stay in the private context; only the console-transfer
 * token reaches out, and only for the caller's /connect body. */
static int passport_provider(void *context, X4Http *http, char *out, size_t capacity, int *http_status,
    const char **stage, const _Atomic int *cancel)
{
    X4Auth *a = context;
    if (!stage) return X4_AUTH_E_ARGUMENT;
    *stage = "autorizacion de conexion no disponible";
    if (!http_status || !out || capacity < 2) return X4_AUTH_E_ARGUMENT;
    *http_status = 0;
    out[0] = 0;
    if (!a || !http || !cancel) return X4_AUTH_E_ARGUMENT;
    if (atomic_load(cancel)) { *stage = "autorizacion cancelada"; return X4_XBOX_PASSPORT_CANCELLED; }
    X4PassportWork *p = malloc(sizeof(*p));
    if (!p) {
        *stage = "sin memoria para autorizar la conexion";
        return X4_AUTH_E_ALLOCATION;
    }
    memset(p, 0, sizeof(*p));
    int rc = refresh_tokens(a, p, http, http_status, stage);
    /* A cancel during the renewal is honoured only after it was kept. */
    if (!rc && atomic_load(cancel)) {
        *stage = "autorizacion cancelada";
        rc = X4_XBOX_PASSPORT_CANCELLED;
    }
    if (!rc) rc = passport_token(a, p, http, out, capacity, http_status, stage, cancel);
    if (rc) x4_secure_clear(out, capacity);
    x4_secure_clear(p, sizeof(*p));
    free(p);
    return rc;
}

/* The Microsoft token is only read here; account state is never changed, so
 * it stays AUTHORIZED after any catalog outcome while the token is valid. */
static void catalog_run(X4Auth *a)
{
    X4XboxWork *w = a->xwork;
    if (!a->token_expiry || now() >= a->token_expiry) {
        reset_catalog(a, X4_CATALOG_ERROR, X4_AUTH_E_SIGNED_OUT, "token Microsoft caducado");
    } else {
        const X4CatalogSnapshot *r = x4_xbox_catalog(w, a->access_token, &a->cancel, catalog_progress, a);
        /* A cancel seen before the final publication wins over any result. */
        if (atomic_load(&a->cancel)) reset_catalog(a, X4_CATALOG_CANCELLED, 0, "consulta cancelada");
        else publish_catalog(a, r);
    }
    x4_xbox_work_free(w);
    a->xwork = NULL;
}

/* The Xbox credentials are reacquired inside the run and wiped by it. At
 * READY the run calls passport_provider on this same worker, which renews
 * (and may rotate) the Microsoft tokens; account and catalog views change
 * only when Microsoft answers invalid_grant. The final view is published as
 * returned: it already reflects any cancel, and a failed remote cleanup must
 * never be replaced by CANCELLED. */
static void session_run(X4Auth *a)
{
    X4XboxWork *w = a->xwork;
    if (!a->token_expiry || now() >= a->token_expiry) {
        reset_session(a, X4_SESSION_ERROR, X4_AUTH_E_SIGNED_OUT, "token Microsoft caducado",
            a->session_title.name[0] ? a->session_title.name : a->session_title.id, a->session_offering, NULL);
    } else {
        const X4SessionSnapshot *r = x4_xbox_session(w, a->access_token, &a->session_title, a->session_offering,
            &a->cancel, session_progress, a, passport_provider, a, a->media_callback, a->media_user,
            &a->keyframe_requested, gamepad_source, a);
        session_progress(a, r);
    }
    x4_xbox_work_free(w);
    a->xwork = NULL;
    x4_secure_clear(&a->session_title, sizeof(a->session_title));
    x4_secure_clear(a->session_offering, sizeof(a->session_offering));
}

static void *worker(void *opaque)
{
    X4Auth *a = opaque;
    if (a->action == X4_AUTH_XBOX_CATALOG || a->action == X4_AUTH_XBOX_SESSION) {
        if (a->action == X4_AUTH_XBOX_CATALOG) catalog_run(a);
        else session_run(a);
        atomic_store_explicit(&a->finished, 1, memory_order_release);
        return NULL;
    }
    X4AuthWork *w = a->work;
    Outcome o = {0};
    int rc = 0;
    X4Http *http = x4_http_open(&rc);
    if (!http) set(&o, X4_AUTH_ERROR, rc, 0, "inicio HTTPS (red/TLS)");
    else if (a->action == X4_AUTH_CHECK_CONNECTION) check_connection(a, w, http, &o);
    else sign_in(a, w, http, &o);
    if (o.state != X4_AUTH_AUTHORIZED) wipe_tokens(a);

    /* Terminal states hide the code now; a token waits for the final check. */
    if (o.state == X4_AUTH_AUTHORIZED)
        publish(a, X4_AUTH_CONNECTING, 0, o.http_status, "verificando token", NULL, NULL, 0);
    else publish(a, o.state, o.error, o.http_status, o.stage, NULL, NULL, 0);
    x4_http_close(http);
    x4_secure_clear(w, sizeof(*w));
    free(w);
    a->work = NULL;

    if (o.state == X4_AUTH_AUTHORIZED) {
        if (atomic_load(&a->cancel)) set(&o, X4_AUTH_CANCELLED, 0, 0, "acceso cancelado");
        else if (now() >= a->token_expiry) set(&o, X4_AUTH_EXPIRED, 0, 0, "token Microsoft caducado");
        if (o.state != X4_AUTH_AUTHORIZED) wipe_tokens(a);
        publish(a, o.state, o.error, o.http_status, o.stage, NULL, NULL, 0);
    }
    printf("XCloud4: acceso Microsoft fin estado=%d http=%d error=0x%08x\n",
        (int)o.state, o.http_status, (unsigned)o.error);
    /* Last action: every token write above is published by this release. */
    atomic_store_explicit(&a->finished, 1, memory_order_release);
    return NULL;
}

/* Main thread, only while no worker runs. */
static void expire_tokens(X4Auth *a)
{
    /* A sign-in cancellation accepted while busy can race the worker's final
     * finished store. Reconcile it after acquiring that store on main. A
     * catalog cancellation never touches a still valid token: a late one
     * leaves the completed catalog as published. */
    if (a->token_expiry && a->action == X4_AUTH_SIGN_IN && atomic_load(&a->cancel)) {
        discard_session(a);
        publish(a, X4_AUTH_CANCELLED, 0, 0, "acceso cancelado", NULL, NULL, 0);
    } else if (a->token_expiry && now() >= a->token_expiry) {
        discard_session(a);
        publish(a, X4_AUTH_EXPIRED, 0, 0, "token Microsoft caducado", NULL, NULL, 0);
    }
}

static int join_worker(X4Auth *a)
{
    if (!a->running) return 0;
    int rc = scePthreadJoin(a->thread, NULL);
    if (rc != 0) {
        if (rc > 0) rc = -rc;
        printf("XCloud4: acceso Microsoft join 0x%08x\n", (unsigned)rc);
        return rc;
    }
    a->running = 0;
    return 0;
}

X4Auth *x4_auth_create(void)
{
    X4Auth *a = calloc(1, sizeof(*a));
    if (!a) {
        printf("XCloud4: acceso Microsoft sin memoria\n");
        return NULL;
    }
    atomic_init(&a->cancel, 0);
    atomic_init(&a->finished, 1);
    atomic_init(&a->keyframe_requested, 0);
    atomic_flag_clear(&a->lock);
    publish(a, X4_AUTH_IDLE, 0, 0, "sin cuenta Microsoft", NULL, NULL, 0);
    reset_catalog(a, X4_CATALOG_IDLE, 0, "sin catalogo");
    reset_session(a, X4_SESSION_IDLE, 0, "sin sesion", NULL, NULL, NULL);
    return a;
}

int x4_auth_busy(X4Auth *a)
{
    return a && !atomic_load_explicit(&a->finished, memory_order_acquire);
}

/* Main thread, after join and expiry checks. Publishes LOADING first. */
static int start_catalog(X4Auth *a)
{
    if (!a->token_expiry) return X4_AUTH_E_SIGNED_OUT;
    a->xwork = x4_xbox_work_new();
    if (!a->xwork) {
        printf("XCloud4: catalogo Xbox sin memoria de trabajo\n");
        reset_catalog(a, X4_CATALOG_ERROR, X4_AUTH_E_ALLOCATION, "sin memoria para el catalogo");
        return X4_AUTH_E_ALLOCATION;
    }
    a->action = X4_AUTH_XBOX_CATALOG;
    atomic_store(&a->cancel, 0);
    reset_catalog(a, X4_CATALOG_LOADING, 0, "preparando consulta Xbox");
    atomic_store_explicit(&a->finished, 0, memory_order_release);
    int rc = scePthreadCreate(&a->thread, NULL, worker, a, "x4-auth");
    if (rc != 0) {
        if (rc > 0) rc = -rc;
        printf("XCloud4: catalogo Xbox crear hilo 0x%08x\n", (unsigned)rc);
        atomic_store(&a->finished, 1);
        x4_xbox_work_free(a->xwork);
        a->xwork = NULL;
        reset_catalog(a, X4_CATALOG_ERROR, rc, "no se pudo iniciar la consulta");
        return rc;
    }
    a->running = 1;
    return 0;
}

int x4_auth_start(X4Auth *a, enum X4AuthAction action)
{
    if (!a || (action != X4_AUTH_SIGN_IN && action != X4_AUTH_CHECK_CONNECTION &&
        action != X4_AUTH_XBOX_CATALOG)) return X4_AUTH_E_ARGUMENT;
    if (x4_auth_busy(a)) return X4_AUTH_E_BUSY;
    int rc = join_worker(a);
    if (rc) return rc;
    expire_tokens(a);
    /* The check would replace the AUTHORIZED snapshot, which already proves
     * a working connection. */
    if (action == X4_AUTH_CHECK_CONNECTION && a->token_expiry) return X4_AUTH_E_AUTHORIZED;
    if (action == X4_AUTH_XBOX_CATALOG) return start_catalog(a);
    /* A fresh sign-in deliberately discards the previous token, catalog and
     * session view; no worker runs here, so no cleanup is pending. */
    if (action == X4_AUTH_SIGN_IN) {
        discard_session(a);
        reset_session(a, X4_SESSION_IDLE, 0, "sin sesion", NULL, NULL, NULL);
    }
    a->work = malloc(sizeof(*a->work));
    if (!a->work) {
        printf("XCloud4: acceso Microsoft sin memoria de trabajo\n");
        publish(a, X4_AUTH_ERROR, X4_AUTH_E_ALLOCATION, 0, "sin memoria para el acceso", NULL, NULL, 0);
        return X4_AUTH_E_ALLOCATION;
    }
    memset(a->work, 0, sizeof(*a->work));
    a->action = action;
    atomic_store(&a->cancel, 0);
    publish(a, X4_AUTH_CONNECTING, 0, 0,
        action == X4_AUTH_SIGN_IN ? "conectando con Microsoft" : "comprobando conexion TLS", NULL, NULL, 0);
    atomic_store_explicit(&a->finished, 0, memory_order_release);
    rc = scePthreadCreate(&a->thread, NULL, worker, a, "x4-auth");
    if (rc != 0) {
        if (rc > 0) rc = -rc;
        printf("XCloud4: acceso Microsoft crear hilo 0x%08x\n", (unsigned)rc);
        atomic_store(&a->finished, 1);
        x4_secure_clear(a->work, sizeof(*a->work));
        free(a->work);
        a->work = NULL;
        publish(a, X4_AUTH_ERROR, rc, 0, "no se pudo iniciar el acceso", NULL, NULL, 0);
        return rc;
    }
    a->running = 1;
    return 0;
}

static bool valid_offering(const char *offering)
{
    return !strcmp(offering, "xgpuweb") || !strcmp(offering, "xgpuwebf2p");
}

int x4_auth_start_session(X4Auth *a, unsigned index)
{
    if (!a) return X4_AUTH_E_ARGUMENT;
    if (x4_auth_busy(a)) return X4_AUTH_E_BUSY;
    int rc = join_worker(a);
    if (rc) return rc;
    atomic_store(&a->keyframe_requested, 0);
    expire_tokens(a);
    if (!a->token_expiry || now() >= a->token_expiry) return X4_AUTH_E_SIGNED_OUT;
    /* Title and offering are copied privately before the worker exists. No
     * catalog or Store request is repeated for the session. */
    X4CatalogTitle title = {0};
    char offering[sizeof(a->session_offering)] = {0}, region[sizeof(a->catalog.region)] = {0};
    bool ready;
    lock(a);
    ready = a->catalog.state == X4_CATALOG_READY && index < a->catalog.count && index < X4_CATALOG_MAX;
    if (ready) {
        title = a->catalog.titles[index];
        copy_text(offering, sizeof(offering), a->catalog.offering);
        copy_text(region, sizeof(region), a->catalog.region);
    }
    unlock(a);
    if (!ready) return X4_AUTH_E_ARGUMENT;
    title.id[sizeof(title.id) - 1] = 0;
    title.name[sizeof(title.name) - 1] = 0;
    if (!title.id[0] || !valid_offering(offering)) {
        x4_secure_clear(&title, sizeof(title));
        return X4_AUTH_E_ARGUMENT;
    }
    const char *shown = title.name[0] ? title.name : title.id;
    a->xwork = x4_xbox_work_new();
    if (!a->xwork) {
        printf("XCloud4: sesion Xbox sin memoria de trabajo\n");
        reset_session(a, X4_SESSION_ERROR, X4_AUTH_E_ALLOCATION, "sin memoria para la sesion", shown, offering,
            region);
        x4_secure_clear(&title, sizeof(title));
        return X4_AUTH_E_ALLOCATION;
    }
    a->session_title = title;
    copy_text(a->session_offering, sizeof(a->session_offering), offering);
    a->action = X4_AUTH_XBOX_SESSION;
    atomic_store(&a->cancel, 0);
    reset_session(a, X4_SESSION_STARTING, 0, "preparando sesion Xbox", shown, offering, region);
    atomic_store_explicit(&a->finished, 0, memory_order_release);
    rc = scePthreadCreate(&a->thread, NULL, worker, a, "x4-auth");
    if (rc != 0) {
        if (rc > 0) rc = -rc;
        printf("XCloud4: sesion Xbox crear hilo 0x%08x\n", (unsigned)rc);
        atomic_store(&a->finished, 1);
        x4_xbox_work_free(a->xwork);
        a->xwork = NULL;
        x4_secure_clear(&a->session_title, sizeof(a->session_title));
        x4_secure_clear(a->session_offering, sizeof(a->session_offering));
        reset_session(a, X4_SESSION_ERROR, rc, "no se pudo iniciar la sesion", shown, offering, region);
        x4_secure_clear(&title, sizeof(title));
        return rc;
    }
    a->running = 1;
    x4_secure_clear(&title, sizeof(title));
    return 0;
}

void x4_auth_cancel(X4Auth *a)
{
    if (x4_auth_busy(a)) atomic_store(&a->cancel, 1);
}

void x4_auth_snapshot(X4Auth *a, X4AuthSnapshot *out)
{
    if (!out) return;
    if (!a) {
        *out = (X4AuthSnapshot){.state = X4_AUTH_ERROR, .error = X4_AUTH_E_ALLOCATION};
        copy_text(out->stage, sizeof(out->stage), "sin memoria para el acceso");
        return;
    }
    bool busy = x4_auth_busy(a);
    if (!busy) expire_tokens(a);
    Shared copy;
    lock(a);
    copy = a->shared;
    unlock(a);
    *out = copy.view;
    out->seconds_left = 0;
    if (out->state == X4_AUTH_WAITING) {
        uint64_t t = now();
        if (copy.code_deadline > t)
            out->seconds_left = (unsigned)((copy.code_deadline - t + X4_AUTH_USEC - 1) / X4_AUTH_USEC);
    }
    /* Only sign-in and the check own the account view; catalog and session
     * cancels never mark the account as cancelling. */
    if (busy && (a->action == X4_AUTH_SIGN_IN || a->action == X4_AUTH_CHECK_CONNECTION) &&
        atomic_load(&a->cancel)) {
        /* Hide the code at once; the worker wipes its private copy. */
        memset(out->user_code, 0, sizeof(out->user_code));
        memset(out->verification_uri, 0, sizeof(out->verification_uri));
        out->seconds_left = 0;
        copy_text(out->stage, sizeof(out->stage), "cancelando...");
    }
}

void x4_auth_catalog_snapshot(X4Auth *a, X4CatalogSnapshot *out)
{
    if (!out) return;
    if (!a) {
        memset(out, 0, sizeof(*out));
        out->state = X4_CATALOG_ERROR;
        out->error = X4_AUTH_E_ALLOCATION;
        copy_text(out->stage, sizeof(out->stage), "sin memoria para el acceso");
        return;
    }
    bool busy = x4_auth_busy(a);
    if (!busy) expire_tokens(a);
    lock(a);
    *out = a->catalog;
    unlock(a);
    if (busy && a->action == X4_AUTH_XBOX_CATALOG && atomic_load(&a->cancel))
        copy_text(out->stage, sizeof(out->stage), "cancelando...");
}

void x4_auth_session_snapshot(X4Auth *a, X4SessionSnapshot *out)
{
    if (!out) return;
    if (!a) {
        memset(out, 0, sizeof(*out));
        out->state = X4_SESSION_ERROR;
        out->error = X4_AUTH_E_ALLOCATION;
        copy_text(out->stage, sizeof(out->stage), "sin memoria para el acceso");
        return;
    }
    bool busy = x4_auth_busy(a);
    if (!busy) expire_tokens(a);
    lock(a);
    *out = a->session;
    unlock(a);
    /* A pending cancel shows STOPPING at once while the worker still runs;
     * a terminal view (CLOSED/CANCELLED/ERROR, including a failed cleanup)
     * is never replaced. */
    if (busy && a->action == X4_AUTH_XBOX_SESSION && atomic_load(&a->cancel) &&
        (out->state == X4_SESSION_STARTING || out->state == X4_SESSION_WAITING ||
            out->state == X4_SESSION_READY || out->state == X4_SESSION_AUTHORIZING ||
            out->state == X4_SESSION_AUTHORIZED || out->state == X4_SESSION_NEGOTIATING ||
            out->state == X4_SESSION_CONNECTING || out->state == X4_SESSION_STREAMING ||
            out->state == X4_SESSION_STOPPING)) {
        out->state = X4_SESSION_STOPPING;
        out->seconds_left = 0;
        copy_text(out->stage, sizeof(out->stage), "cerrando sesion...");
    }
}

int x4_auth_set_media_callback(X4Auth *a, X4SessionMediaCallback callback, void *user)
{
    if (!a) return X4_AUTH_E_ARGUMENT;
    if (x4_auth_busy(a)) return X4_AUTH_E_BUSY;
    /* The finished release already proves every RTC callback stopped. Join
     * the owner thread too before its registered receiver can be replaced
     * or freed; on a native join failure leave the registration intact. */
    int rc = join_worker(a);
    if (rc) return rc;
    a->media_callback = callback;
    a->media_user = callback ? user : NULL;
    return 0;
}

void x4_auth_request_keyframe(X4Auth *a)
{
    if (a) atomic_store(&a->keyframe_requested, 1);
}

int x4_auth_forget(X4Auth *a)
{
    if (!a) return X4_AUTH_E_ARGUMENT;
    if (x4_auth_busy(a)) return X4_AUTH_E_BUSY;
    int rc = join_worker(a);
    if (rc) return rc;
    discard_session(a);
    reset_session(a, X4_SESSION_IDLE, 0, "sin sesion", NULL, NULL, NULL);
    publish(a, X4_AUTH_IDLE, 0, 0, "sin cuenta Microsoft", NULL, NULL, 0);
    return 0;
}

int x4_auth_close(X4Auth *a)
{
    if (!a) return 0;
    if (x4_auth_busy(a)) return X4_AUTH_E_BUSY;
    int rc = join_worker(a);
    if (rc) return rc;
    x4_secure_clear(a, sizeof(*a));
    free(a);
    return 0;
}
