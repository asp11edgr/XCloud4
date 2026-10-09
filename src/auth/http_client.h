/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdatomic.h>

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

/* HTTPS only to https://login.microsoftonline.com/consumers/<path>. GET when
 * form is NULL, otherwise POST application/x-www-form-urlencoded. Returns 0
 * once any HTTP status and its whole body were read (body NUL terminated,
 * at most capacity - 1 bytes). On error body is wiped, length and status 0.
 * cancel may be NULL; it is polled between stages and reads, so cancelling
 * waits for the current native operation under its configured timeouts. */
int x4_http_request(X4Http *http, const char *url, const char *form, char *body,
    size_t capacity, size_t *length, int *status, const _Atomic int *cancel);

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
 *   GET        https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=
 *              <1..8 alphanumeric IDs>&market=MX&languages=es-MX&fieldsTemplate=Details
 * json selects POST (printable ASCII, at most X4_HTTP_JSON_BODY_MAX bytes);
 * NULL selects GET. Each destination has its own short header allowlist;
 * Authorization is accepted only as "Bearer <token68>" to a /v2/titles
 * region. capacity is at most X4_HTTP_RESPONSE_MAX + 1. */
int x4_http_json_request(X4Http *http, const char *url, const X4HttpHeader *headers,
    size_t header_count, const char *json, char *body, size_t capacity, size_t *length,
    int *status, const _Atomic int *cancel);

/* Constant literal naming the last stage reached; safe to show in the UI. */
const char *x4_http_stage(const X4Http *http);

void x4_secure_clear(void *data, size_t size);
