/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define X4_SESSION_REFRESH_CAPACITY 16384u
typedef struct X4SessionStore X4SessionStore;
typedef struct { bool enabled, available; int error; } X4SessionStoreSnapshot;
/* The native implementation uses only the current title's encrypted SaveData.
 * The caller serializes operations; snapshot is safe while a worker operates. */
X4SessionStore *x4_session_store_new(void);
/* 0 = released. A native unmount failure retains the object so the owner can
 * retry safe closure; no firmware-success guarantee is inferred from it. */
int x4_session_store_free(X4SessionStore *store);
void x4_session_store_snapshot(const X4SessionStore *store, X4SessionStoreSnapshot *out);
/* 1 = restored, 0 = absent/disabled/corrupt discarded, negative = unavailable.
 * The only credential crossing this private interface is the MS refresh token. */
int x4_session_store_restore(X4SessionStore *store, char *refresh, size_t capacity);
/* true means the account warning was accepted. No warning acceptance = no call.
 * Enabling before sign-in records only the in-memory choice until a token exists. */
int x4_session_store_set_enabled(X4SessionStore *store, bool enabled, const char *refresh);
int x4_session_store_rotate(X4SessionStore *store, const char *refresh);
int x4_session_store_erase(X4SessionStore *store);

#ifdef X4_SESSION_STORE_TEST
/* In-memory SaveData simulation only; no alternate production storage path. */
typedef struct {
    void *context;
    int (*exists)(void *, bool *);
    int (*mount)(void *, bool create);
    int (*read)(void *, void *, size_t, size_t *); /* 1 = no credential file */
    int (*write)(void *, const void *, size_t);
    int (*erase)(void *); /* includes absence verification */
    int (*unmount)(void *);
} X4SessionStoreTestBackend;
X4SessionStore *x4_session_store_test_new(const X4SessionStoreTestBackend *backend);
#endif
