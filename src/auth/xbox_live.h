/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "http_client.h"
#include "xbox_catalog.h"
#include "xbox_session.h"
#include "../input/gamepad.h"
#include "../streaming/stream_profile.h"
#include "../ui/access_country.h"
#include <stdatomic.h>
#include <stddef.h>

/* Internal to src/auth: the Xbox Live / cloud catalog exchanges run by the
 * device_auth worker. Xbox tokens live only inside the workspace. */
typedef struct X4XboxWork X4XboxWork;

/* stage, offering and region are literals or copies safe for the UI. */
typedef void (*X4XboxProgress)(void *context, const char *stage, const char *offering, const char *region);

/* Large heap workspace; free wipes it, tokens included. */
X4XboxWork *x4_xbox_work_new(void);
/* Captured before the worker exists; used only for both GSSV login POSTs.
 * A fresh workspace defaults to Automatic. No preference is read in login. */
bool x4_xbox_work_set_access_country(X4XboxWork *, X4AccessCountry);
/* Worker-owned setup before any session request. Copies the validated public
 * request snapshot; later menu changes cannot alter this session's identity. */
bool x4_xbox_work_set_request_config(X4XboxWork *, const X4StreamRequestConfig *);
typedef struct X4StreamStartup X4StreamStartup;
bool x4_xbox_work_set_stream_startup(X4XboxWork *, X4StreamStartup *);
typedef struct X4KeyframeLimiter X4KeyframeLimiter;
/* New streaming route only: non-NULL borrowed object, bound before HTTP/RTC.
 * The session worker is its sole reservation owner until RTC joins. */
bool x4_xbox_work_set_keyframe_limiter(X4XboxWork *, X4KeyframeLimiter *);
void x4_xbox_work_free(X4XboxWork *work);

/* Microsoft token -> Xbox user token -> XSTS (gssv) -> xgpuweb or, when that
 * offering is refused, xgpuwebf2p -> default region /v2/titles -> server MRU
 * using the same regional credential, then wipe it -> public Store names
 * for the first titles. Opens and closes its own HTTPS context.
 * Returns final READY/ERROR/CANCELLED catalog stored inside the workspace;
 * MRU failures are reported separately and do not invalidate that catalog. */
const X4CatalogSnapshot *x4_xbox_catalog(X4XboxWork *work, const char *microsoft_token,
    const _Atomic int *cancel, X4XboxProgress progress, void *context);
/* Queries only /v2/titles on the chosen server, searching specifically for Fortnite.
 * Does not query Store details, MRU, or covers. Returns READY (count=1) or ERROR
 * with X4_AUTH_E_FORTNITE_NOT_FOUND. */
const X4CatalogSnapshot *x4_xbox_fortnite_query(X4XboxWork *work, const char *microsoft_token,
    const _Atomic int *cancel, X4XboxProgress progress, void *context);
/* Final temporary MRU result from the same private regional credentials as
 * the last catalog call. No header/token is returned. */
const X4RecentSnapshot *x4_xbox_catalog_recent(const X4XboxWork *work);
/* Reacquires private regional credentials and fetches only server MRU. It
 * never requests all titles, alters a catalog or starts a cloud session. */
const X4RecentSnapshot *x4_xbox_recents(X4XboxWork *work, const char *microsoft_token,
    const _Atomic int *cancel);

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
 * ReadyToConnect/Provisioned (180 s outside the queue; WaitingForResources
 * has a separate 15 min limit, then a fresh 180 s provision window) -> passport provider -> POST
 * .../connect once -> real RTC SDP/ICE exchange -> receive authenticated
 * RTP and keep the session alive until cancel or failure -> DELETE.
 * The DELETE is attempted once on every path that obtained a validated
 * session path, cancelled or not. Opens and closes its own HTTPS context and
 * wipes every credential before returning the final view kept in the
 * workspace. passport is required. */
typedef struct X4Trace X4Trace;
typedef struct X4Rumble X4Rumble;
typedef struct X4NackGraceInbox X4NackGraceInbox;
const X4SessionSnapshot *x4_xbox_session(X4XboxWork *work, const char *microsoft_token,
    const X4CatalogTitle *title, const char *offering, const _Atomic int *cancel,
    X4SessionProgress progress, void *context, X4XboxPassport passport, void *passport_context,
    X4SessionMediaCallback media_callback, void *media_user,
    _Atomic int *keyframe_requested, X4GamepadSource gamepad_source, void *gamepad_user,
    X4Trace *trace, X4Rumble *rumble, X4NackGraceInbox *grace_inbox);
