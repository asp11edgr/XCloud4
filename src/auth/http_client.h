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

/* Constant literal naming the last stage reached; safe to show in the UI. */
const char *x4_http_stage(const X4Http *http);

void x4_secure_clear(void *data, size_t size);
