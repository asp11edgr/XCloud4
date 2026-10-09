/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "xbox_catalog.h"
#include <stdatomic.h>

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
