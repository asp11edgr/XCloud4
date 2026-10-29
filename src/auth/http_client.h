/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

/* Local errors; native Net/Ssl/Http errors are returned unchanged. */
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
 * cancel may be NULL; it is polled between stages and reads, so cancelling
 * waits for the current native operation under its configured timeouts. */
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

/* Worker-only suspension boundary. Deletes public connections and their
 * templates before returning 0; caller may declare idle only afterwards.
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

/* Constant literal naming the last stage reached; safe to show in the UI. */
const char *x4_http_stage(const X4Http *http);

void x4_secure_clear(void *data, size_t size);
