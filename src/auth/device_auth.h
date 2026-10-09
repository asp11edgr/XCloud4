/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "xbox_catalog.h"
#include "xbox_session.h"
#include "../input/gamepad.h"

/* Microsoft account sign-in (OAuth 2.0 device authorization grant) on one
 * background worker. AUTHORIZED only means a Microsoft token was acquired;
 * it implies no Xbox Live session or game access. Tokens stay private.
 * The same worker reads the Xbox cloud catalog with that token; Xbox
 * credentials exist only during that run and never reach a snapshot. A
 * session run also renews the Microsoft access with its refresh token (a
 * rotated refresh token is kept) to obtain the console-transfer token for
 * the session's /connect; none of these tokens reaches a snapshot. */

/* Local errors; native Net/Ssl/Http/pthread errors are reported unchanged. */
enum {
    X4_AUTH_E_ARGUMENT = -2001,
    X4_AUTH_E_BUSY = -2002,
    X4_AUTH_E_ALLOCATION = -2003,
    X4_AUTH_E_AUTHORIZED = -2004, /* connection check refused: token held */
    X4_AUTH_E_FORM = -2005,
    X4_AUTH_E_RESPONSE = -2006,   /* malformed or out-of-bounds response */
    X4_AUTH_E_STATUS = -2007,     /* unexpected HTTP status */
    X4_AUTH_E_REJECTED = -2008,   /* OAuth error returned by Microsoft */
    X4_AUTH_E_SIGNED_OUT = -2009, /* catalog refused: no valid Microsoft token */
    X4_AUTH_E_XBOX = -2010,       /* Xbox Live or cloud gaming refused the account */
    X4_AUTH_E_REGION = -2011,     /* no usable default streaming region */
    X4_AUTH_E_SESSION = -2012,    /* Xbox reported the cloud session failed */
    X4_AUTH_E_DEADLINE = -2013,   /* session not ready within its deadline */
    X4_AUTH_E_UNCONFIRMED = -2014, /* a remote session may exist; not confirmed closed */
};

typedef struct X4Auth X4Auth;

enum X4AuthState {
    X4_AUTH_IDLE,
    X4_AUTH_CONNECTING,
    X4_AUTH_WAITING,
    X4_AUTH_CONNECTED,
    X4_AUTH_AUTHORIZED,
    X4_AUTH_CANCELLED,
    X4_AUTH_EXPIRED,
    X4_AUTH_DENIED,
    X4_AUTH_ERROR,
};

/* X4_AUTH_XBOX_SESSION is dispatched only through x4_auth_start_session;
 * x4_auth_start refuses it. */
enum X4AuthAction { X4_AUTH_SIGN_IN, X4_AUTH_CHECK_CONNECTION, X4_AUTH_XBOX_CATALOG, X4_AUTH_XBOX_SESSION };

typedef struct {
    enum X4AuthState state;
    int error, http_status;
    unsigned seconds_left;
    char stage[80], user_code[32], verification_uri[256];
} X4AuthSnapshot;

/* Public operations belong to main except the synchronized input source and
 * cancel/keyframe-request flags. */
X4Auth *x4_auth_create(void);
/* Main thread only, while no worker is busy. Registration persists across
 * sign-ins. A completed worker is joined before replacing the registration;
 * a join error preserves the old registration. The receiver context must
 * outlive every session worker. */
int x4_auth_set_media_callback(X4Auth *auth, X4SessionMediaCallback callback, void *user);
/* Thread-safe request flag. The session owner consumes it and requests PLI;
 * this never accesses the transport from the main/decoder thread. */
void x4_auth_request_keyframe(X4Auth *auth);
/* Main publishes a copied normalized controller state each UI iteration.
 * The RTC sender reads it under a short gate; stale states (>250 ms) and
 * cancellation produce neutral reports. No controller or token pointer is
 * shared with the sender. */
void x4_auth_set_gamepad(X4Auth *auth, const X4GamepadFrame *frame);
/* 0 when the worker was started. Refuses while busy, refuses a connection
 * check while a token is held, and refuses the catalog without a valid one.
 * Sign-in discards any old token and catalog. The catalog run publishes
 * LOADING, then READY, ERROR or CANCELLED; the account stays AUTHORIZED
 * through it while the Microsoft token remains valid. */
int x4_auth_start(X4Auth *auth, enum X4AuthAction action);
/* Sets the cancel flag only; poll x4_auth_busy until it reports false. */
void x4_auth_cancel(X4Auth *auth);
int x4_auth_busy(X4Auth *auth);
void x4_auth_snapshot(X4Auth *auth, X4AuthSnapshot *out);
/* Copy of the last catalog view; it holds no token, user hash or body. */
void x4_auth_catalog_snapshot(X4Auth *auth, X4CatalogSnapshot *out);
/* Prepares an Xbox cloud session for catalog title catalog_index with the
 * catalog's offering and authorizes its connection (see xbox_session.h). A
 * registered receiver obtains actual RTP once WebRTC connects. Requires a READY catalog, a valid index, a
 * valid Microsoft token and no running worker. Publishes STARTING before the
 * worker starts. The account stays AUTHORIZED and the catalog view is kept,
 * except when Microsoft refuses the renewal with invalid_grant: then the
 * local token and catalog are dropped and the account turns EXPIRED (no
 * global sign-out). x4_auth_cancel stops it; busy stays true through
 * negotiation, streaming and remote cleanup until every credential is wiped and
 * HTTPS is closed. */
int x4_auth_start_session(X4Auth *auth, unsigned catalog_index);
/* Copy of the last session view; no token, session path or ID. */
void x4_auth_session_snapshot(X4Auth *auth, X4SessionSnapshot *out);
/* Wipes the local token, catalog and session view only; no global Microsoft
 * sign-out. */
int x4_auth_forget(X4Auth *auth);
/* Call once busy is false. On a join error the context is kept. */
int x4_auth_close(X4Auth *auth);
