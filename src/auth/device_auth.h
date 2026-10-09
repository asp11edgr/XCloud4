/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

/* Microsoft account sign-in (OAuth 2.0 device authorization grant) on one
 * background worker. AUTHORIZED only means a Microsoft token was acquired;
 * it implies no Xbox Live session or game access. Tokens stay private. */

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

enum X4AuthAction { X4_AUTH_SIGN_IN, X4_AUTH_CHECK_CONNECTION };

typedef struct {
    enum X4AuthState state;
    int error, http_status;
    unsigned seconds_left;
    char stage[80], user_code[32], verification_uri[256];
} X4AuthSnapshot;

/* Everything except x4_auth_cancel belongs to the main thread. */
X4Auth *x4_auth_create(void);
/* 0 when the worker was started. Refuses while busy, and refuses a
 * connection check while a token is held. Sign-in discards any old token. */
int x4_auth_start(X4Auth *auth, enum X4AuthAction action);
/* Sets the cancel flag only; poll x4_auth_busy until it reports false. */
void x4_auth_cancel(X4Auth *auth);
int x4_auth_busy(X4Auth *auth);
void x4_auth_snapshot(X4Auth *auth, X4AuthSnapshot *out);
/* Wipes the local token only; no global Microsoft sign-out. */
int x4_auth_forget(X4Auth *auth);
/* Call once busy is false. On a join error the context is kept. */
int x4_auth_close(X4Auth *auth);
