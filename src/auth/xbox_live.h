/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "http_client.h"
#include "xbox_catalog.h"
#include "xbox_session.h"
#include <stdatomic.h>
#include <stddef.h>

/* Internal to src/auth: the Xbox Live / cloud catalog exchanges run by the
 * device_auth worker. Xbox tokens live only inside the workspace. */
typedef struct X4XboxWork X4XboxWork;

/* stage, offering and region are literals or copies safe for the UI. */
typedef void (*X4XboxProgress)(void *context, const char *stage, const char *offering, const char *region);

/* Large heap workspace; free wipes it, tokens included. */
X4XboxWork *x4_xbox_work_new(void);
void x4_xbox_work_free(X4XboxWork *work);

/* Microsoft token -> Xbox user token -> XSTS (gssv) -> xgpuweb or, when that
 * offering is refused, xgpuwebf2p -> default region /v2/titles -> Store names
 * for the first titles. Opens and closes its own HTTPS context. Returns the
 * final READY/ERROR/CANCELLED view stored inside the workspace. */
const X4CatalogSnapshot *x4_xbox_catalog(X4XboxWork *work, const char *microsoft_token,
    const _Atomic int *cancel, X4XboxProgress progress, void *context);

/* Receives a complete public copy at every transition and countdown second. */
typedef void (*X4SessionProgress)(void *context, const X4SessionSnapshot *snapshot);

/* Private connection-authorization provider, owned by device_auth. Called
 * once per session on the session worker with the session's own HTTPS
 * context. It writes only the console-transfer token (NUL terminated,
 * private) to out, sets *http_status to the status of its last Microsoft
 * exchange (0 when none returned one) and *stage to a constant literal safe
 * for the UI. Returns 0 with a token, X4_XBOX_PASSPORT_CANCELLED when it
 * honoured cancel, otherwise an error code; out is wiped unless it returns 0.
 * Any other private token stays behind context. */
#define X4_XBOX_PASSPORT_CANCELLED 1
typedef int (*X4XboxPassport)(void *context, X4Http *http, char *out, size_t capacity, int *http_status,
    const char **stage, const _Atomic int *cancel);

/* Session preparation and connection authorization, on a fresh workspace:
 * Microsoft token -> Xbox user token -> XSTS -> credentials for exactly
 * `offering` ("xgpuweb" or "xgpuwebf2p", no fallback) -> default region POST
 * /v5/sessions/cloud/play for title->id -> GET .../state every 2 s until
 * ReadyToConnect/Provisioned (180 s) -> passport provider -> POST
 * .../connect once -> AUTHORIZED held at most 45 s -> DELETE. No SDP or ICE.
 * The DELETE is attempted once on every path that obtained a validated
 * session path, cancelled or not. Opens and closes its own HTTPS context and
 * wipes every credential before returning the final view kept in the
 * workspace. passport is required. */
const X4SessionSnapshot *x4_xbox_session(X4XboxWork *work, const char *microsoft_token,
    const X4CatalogTitle *title, const char *offering, const _Atomic int *cancel,
    X4SessionProgress progress, void *context, X4XboxPassport passport, void *passport_context);
