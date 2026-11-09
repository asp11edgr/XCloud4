/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "cloud_time.h"
#include "http_client.h"

/* One private XBL/XSTS/content-access exchange on the quota worker's HTTP
 * context. Credentials and response bodies are consumed here, never returned. */
int x4_xbox_cloud_time_query(X4Http *http, const char *microsoft_token, const char *market,
    const _Atomic int *cancel, X4CloudTime *result, X4CloudTimeShape *shape, int *status);
