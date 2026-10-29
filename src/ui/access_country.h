/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum X4AccessCountry {
    X4_ACCESS_AUTO = 0,
    X4_ACCESS_US,
    X4_ACCESS_BR,
    X4_ACCESS_JP,
    X4_ACCESS_KR,
    X4_ACCESS_PL,
    X4_ACCESS_COUNTRY_COUNT
} X4AccessCountry;

static inline bool x4_access_country_valid(X4AccessCountry country)
{
    return (unsigned)country < X4_ACCESS_COUNTRY_COUNT;
}

static inline const char *x4_access_country_code(X4AccessCountry country)
{
    static const char *const codes[] = {"auto", "us", "br", "jp", "kr", "pl"};
    return codes[x4_access_country_valid(country) ? (unsigned)country : 0u];
}

/* Country/IP list from Better xCloud, MIT, (c) redphx.
 * redphx/better-xcloud f8397043f6d2148d2345d508902a38c69cf1ee20,
 * src/enums/bypass-servers.ts. See THIRD_PARTY_NOTICES.md. These are header
 * values for the GSSV login, never proxy or video transport destinations. */
static inline const char *x4_access_country_ip(X4AccessCountry country)
{
    static const char *const addresses[] = {
        NULL, "143.244.47.65", "169.150.198.66", "138.199.21.239",
        "121.125.60.151", "45.134.212.66"
    };
    return x4_access_country_valid(country) ? addresses[(unsigned)country] : NULL;
}

static inline bool x4_access_country_valid_ip(const char *ip)
{
    if (!ip) return false;
    for (unsigned i = 1; i < X4_ACCESS_COUNTRY_COUNT; ++i)
        if (!strcmp(ip, x4_access_country_ip((X4AccessCountry)i))) return true;
    return false;
}

/* Main-thread-only file I/O. Readers capture one atomic preference word. */
void x4_access_country_init(void);
bool x4_access_country_set_path(const char *path);
X4AccessCountry x4_access_country_get(void);
bool x4_access_country_warning_accepted(void);
bool x4_access_country_apply(X4AccessCountry country, bool accept_warning, const char *origin);
const char *x4_access_country_label(X4AccessCountry country);

enum {
    X4_ACCESS_UI_LEFT = 1u,
    X4_ACCESS_UI_RIGHT = 2u,
    X4_ACCESS_UI_CONFIRM = 4u,
    X4_ACCESS_UI_CANCEL = 8u,
    X4_ACCESS_UI_HELP = 16u
};
typedef struct {
    X4AccessCountry selected;
    bool confirming, help, save_error;
} X4AccessCountryUi;
typedef enum {
    X4_ACCESS_UI_NONE = 0,
    X4_ACCESS_UI_APPLIED,
    X4_ACCESS_UI_BACK,
    X4_ACCESS_UI_CANCEL_QUERY
} X4AccessCountryUiResult;
void x4_access_country_ui_open(X4AccessCountryUi *ui);
X4AccessCountryUiResult x4_access_country_ui_handle(X4AccessCountryUi *ui,
    unsigned actions, bool busy, const char *origin);

typedef enum {
    X4_ACCESS_CARD_SELECT = 0,
    X4_ACCESS_CARD_TRYING,
    X4_ACCESS_CARD_SUCCESS
} X4AccessCountryCardPhase;
/* Main publishes a value copy for the renderer; no network or disk work in
 * draw functions. success visibility is timed by main, not the renderer. */
void x4_access_country_card_set(const X4AccessCountryUi *ui, X4AccessCountryCardPhase phase);
void x4_access_country_card_get(X4AccessCountryUi *ui, X4AccessCountryCardPhase *phase);
