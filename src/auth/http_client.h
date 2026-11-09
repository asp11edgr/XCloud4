/* SPDX-License-Identifier: GPL-3.0-only */
/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

/* Local errors occupy -1001..-1099; socket/resolver errors retain SceNet's
 * native convention. There is no native TLS or HTTP fallback. */
enum {
    X4_HTTP_CANCELLED = -1001,
    X4_HTTP_BOUND = -1002,
    X4_HTTP_URL = -1003,
    X4_HTTP_DEADLINE = -1004,
    X4_HTTP_ALLOCATION = -1005,
    X4_HTTP_ARGUMENT = -1006,
    X4_HTTP_UNUSABLE = -1007,
    X4_HTTP_PROTOCOL = -1008,
    X4_HTTP_BUSY = -1009,
    X4_HTTP_TLS_ALERT = -1010,
    X4_HTTP_TLS_CERTIFICATE = -1011,
    X4_HTTP_TLS_CERTIFICATE_TIME = -1012,
};

typedef struct X4Http X4Http;

/* Every call, including open and close, belongs to one worker thread. Close
 * only after that worker has finished; it never runs a global NetTerm. */
X4Http *x4_http_open(int *error);
void x4_http_close(X4Http *http);

/* Encoded forms are visible ASCII of at most this many bytes; a refresh token
 * of 16 KiB still fits once percent-encoded. */
#define X4_HTTP_FORM_MAX (64u * 1024)

/* HTTPS only to https://login.microsoftonline.com/consumers/<path>. GET when
 * form is NULL, otherwise POST application/x-www-form-urlencoded. Returns 0
 * once any HTTP status and its whole body were read (body NUL terminated,
 * at most capacity - 1 bytes). On error body is wiped, length and status 0.
 * cancel may be NULL; nonblocking socket waits check it at most every 100 ms.
 * Certificate trust, dates and the destination hostname are mandatory. */
int x4_http_request(X4Http *http, const char *url, const char *form, char *body,
    size_t capacity, size_t *length, int *status, const _Atomic int *cancel);

#define X4_HTTP_PASSPORT_BODY_MAX (64u * 1024)

/* POST application/x-www-form-urlencoded to exactly
 * https://login.live.com/oauth20_token.srf; the destination is fixed here and
 * never chosen by the caller. form is non-empty visible ASCII of at most
 * X4_HTTP_FORM_MAX bytes; capacity is at most X4_HTTP_PASSPORT_BODY_MAX + 1.
 * Same TLS, timeout, redirect, cookie, quarantine and result rules as
 * x4_http_request. */
int x4_http_passport_request(X4Http *http, const char *form, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel);

#define X4_HTTP_JSON_BODY_MAX (64u * 1024)
#define X4_HTTP_RESPONSE_MAX (2u * 1024 * 1024)
#define X4_HTTP_HEADERS_MAX 4u
#define X4_HTTP_HEADER_VALUE_MAX 16384u

typedef struct { const char *name, *value; } X4HttpHeader;

/* Xbox Live and Microsoft Store exchanges, under the same TLS, timeout,
 * redirect, cookie and quarantine rules as x4_http_request. Only these
 * destinations are accepted:
 *   POST JSON  https://user.auth.xboxlive.com/user/authenticate
 *   POST JSON  https://xsts.auth.xboxlive.com/xsts/authorize
 *   POST JSON  https://xgpuweb[f2p].gssv-play-prod.xboxlive.com/v2/login/user
 *   GET        https://<labels>.gssv-play-prod.xboxlive.com/v2/titles
 *   GET        https://<labels>.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25
 *   GET        https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=
 *              <1..8 alphanumeric IDs>&market=<market>&languages=<locale>&fieldsTemplate=Details
 * json selects POST (printable ASCII, at most X4_HTTP_JSON_BODY_MAX bytes);
 * NULL selects GET. Each destination has its own short header allowlist;
 * Authorization is accepted only as "Bearer <token68>" to a /v2/titles
 * region. X-Forwarded-For is optional only for the two login POSTs and must
 * match the closed access-country IP table; it is forbidden elsewhere.
 * capacity is at most X4_HTTP_RESPONSE_MAX + 1. */
int x4_http_json_request(X4Http *http, const char *url, const X4HttpHeader *headers,
    size_t header_count, const char *json, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel);

/* Private GET for the exact content access host and two fixed parameters.
 * A binary body may contain NUL bytes. Only XBL3.0 authorization and MS-CV
 * are accepted, and neither header is shared with another destination. */
#define X4_HTTP_CLOUD_TIME_PREFIX "https://contentaccess.exp.xboxservices.com/all/v1?market="
#define X4_HTTP_CLOUD_TIME_SUFFIX "&offering=xgpuweb"
int x4_http_cloud_time_request(X4Http *http, const char *url, const X4HttpHeader *headers,
    size_t header_count, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel);

/* Public cover GET only. No caller headers, cookies, redirects or Xbox tokens.
 * Exactly the inspected Microsoft Store image host, /image/apps.<safe asset>,
 * and the fixed bounded-thumbnail query are accepted. Binary body has an
 * extra trailing NUL for transfer compatibility; length excludes it. */
#define X4_HTTP_IMAGE_MAX (2u * 1024 * 1024)
int x4_http_public_image_url(const char *url);
int x4_http_public_image_request(X4Http *http, const char *url, unsigned char *body,
    size_t capacity, size_t *length, int *status, const _Atomic int *cancel);

/* Public-only Store GET, fixed market/language/Details policy above, 1..8 IDs.
 * No caller headers or credentials. This and public_image_request share two
 * bounded keep-alive connections (one per exact public host), owned by the
 * same sequential worker. Auth/session requests keep their existing path. */
int x4_http_public_metadata_request(X4Http *http, const char *url, char *body,
    size_t capacity, size_t *length, int *status, const _Atomic int *cancel);

/* Last public response's valid Retry-After, milliseconds; zero if absent,
 * invalid or in the past. Delta-seconds and IMF-fixdate are supported; huge
 * valid delta values saturate UINT64_MAX, not a shorter retry interval.
 * Worker-only: call immediately after its public request, before another. */
uint64_t x4_http_public_retry_after_ms(const X4Http *http);

/* Worker-only suspension boundary. Closes both public TLS connections
 * before returning 0; caller may declare idle only afterwards.
 * Does not terminate Net or touch auth exchanges. Concurrent calls return
 * BUSY. Native deletion failures quarantine the context, as elsewhere. */
int x4_http_public_reset(X4Http *http);

#define X4_HTTP_RELEASES_URL_PUBLIC "https://api.github.com/repos/asp11edgr/XCloud4/releases/latest"
#define X4_HTTP_RELEASES_URL_LAB "https://api.github.com/repos/asp11edgr/XCloud4/releases?per_page=10"
#define X4_HTTP_RELEASES_BODY_MAX (256u * 1024)

/* GitHub Releases check, under the same TLS (KNOWN_CA), redirect (none) and
 * cookie rules. Accepts exactly the fixed release endpoint for the active
 * build edition:
 *   GET https://api.github.com/repos/asp11edgr/XCloud4/releases/latest     (public)
 *   GET https://api.github.com/repos/asp11edgr/XCloud4/releases?per_page=10 (lab)
 * Fixed headers: User-Agent "XCloud4/<X4_PRODUCT_VERSION>" and
 * Accept "application/vnd.github+json". capacity is at most
 * X4_HTTP_RELEASES_BODY_MAX + 1. Total timeout 10 s. */
int x4_http_releases_request(X4Http *http, const char *url, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel);

enum X4HttpSessionMethod { X4_SESSION_HTTP_GET, X4_SESSION_HTTP_POST, X4_SESSION_HTTP_DELETE };

/* Owned by the session worker. Enable after /play, disable and close at
 * reconnect, cancellation or cleanup. Diagnostic/reconnect APIs remain
 * one-shot regardless of this switch. */
int x4_http_session_persistence(X4Http *http, bool enabled);

/* Xbox cloud session exchanges on the validated default region host
 * (https://<labels>.gssv-play-prod.xboxlive.com, same label rules as
 * /v2/titles), under the same TLS, timeout, redirect, cookie and quarantine
 * rules. Exactly these method/path pairs are accepted:
 *   POST JSON  /v5/sessions/cloud/play            (non-empty JSON body)
 *   GET        /v5/sessions/cloud/<id>/state      (json NULL)
 *   GET        /v1/waittime/<titleId>             (json NULL; optional queue ETA)
 *   POST       /v5/sessions/cloud/<id>/keepalive  (json "", empty body)
 *   POST JSON  /v5/sessions/cloud/<id>/connect    (non-empty JSON body)
 *   GET        /v5/sessions/cloud/<id>/sdp        (json NULL)
 *   POST JSON  /v5/sessions/cloud/<id>/sdp        (non-empty JSON body)
 *   GET        /v5/sessions/cloud/<id>/ice        (json NULL)
 *   POST JSON  /v5/sessions/cloud/<id>/ice        (non-empty JSON body)
 *   DELETE     /v5/sessions/cloud/<id>            (json NULL)
 * <id> is 1..128 of [A-Za-z0-9_-] (GUIDs included) and never "play" or
 * "active". <titleId> is 1..95 of [A-Za-z0-9_-]. No query, fragment,
 * escape, dot segment, port or userinfo.
 * Exactly three headers are required, once each: Authorization
 * ("Bearer <token68>"), x-gssv-client and X-MS-Device-Info. Header values
 * and bodies stay private. capacity is at most X4_HTTP_RESPONSE_MAX + 1. */
int x4_http_session_request(X4Http *http, enum X4HttpSessionMethod method, const char *url,
    const X4HttpHeader *headers, size_t header_count, const char *json, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel);

/* One best-effort diagnostic GET /state after an established RTC connection
 * is lost. No retry; only the already validated session /state route. Its
 * 2 s transfer deadline clamps every phase to the remaining time. Ordinary
 * requests retain their existing 30 s deadline and 5 s phase timeouts. */
int x4_http_session_state_request(X4Http *http, const char *url,
    const X4HttpHeader *headers, size_t header_count, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel);

/* Reconnection only: immutable per-request monotonic absolute deadline.
 * Accepts solely the existing /state,/connect,/sdp,/ice,/keepalive routes;
 * /state additionally retains its 2 s maximum. No /play or DELETE. */
int x4_http_session_reconnect_request(X4Http *http, enum X4HttpSessionMethod method, const char *url,
    const X4HttpHeader *headers, size_t header_count, const char *json, char *body, size_t capacity,
    size_t *length, int *status, const _Atomic int *cancel, uint64_t absolute_deadline_us);
/* The two fixed Microsoft authorization endpoints, only during a reconnect
 * ReadyToConnect step. false=refresh, true=console-transfer Passport. */
int x4_http_auth_reconnect_request(X4Http *http, bool passport, const char *form, char *body,
    size_t capacity, size_t *length, int *status, const _Atomic int *cancel,
    uint64_t absolute_deadline_us);

/* Constant literal naming the last stage reached; safe to show in the UI. */
const char *x4_http_stage(const X4Http *http);

void x4_secure_clear(void *data, size_t size);

enum {
    X4_HTTP_UPDATE_REDIRECT = -1013,
    X4_HTTP_UPDATE_STATUS = -1014,
};
#define X4_HTTP_UPDATE_BODY_MAX (64u * 1024u * 1024u)
/* Dedicated context for release assets. Its extra CA is confined to
 * the fixed GitHub hosts, before DNS or a TLS handshake. */
X4Http *x4_http_update_open(int *error);

typedef int (*X4HttpUpdateSink)(void *context, const void *bytes, size_t size);
/* Worker-owned HTTPS GET of a fixed XCloud4 GitHub release asset. Neither
 * arguments nor redirects can supply credentials, a caller header, or an
 * arbitrary origin. The sink may receive part of the body on failure and
 * must return zero or a negative error. `received` is zero on failure. */
int x4_http_update_asset(X4Http *http, const char *tag, const char *asset_name,
    size_t maximum, X4HttpUpdateSink sink, void *context, size_t *received,
    const _Atomic int *cancel);
