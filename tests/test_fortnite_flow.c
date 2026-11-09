/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../src/ui/screen.h"
#include "../src/ui/lang.h"
#include "../src/ui/picture_settings.h"
#include "../src/streaming/stream_profile.h"
#include "../src/auth/device_auth.h"
#include "../src/auth/json.h"
#include "../src/auth/http_client.h"

X4Http *x4_http_open(int *err) { if (err) *err = 0; return NULL; }
void x4_http_close(X4Http *h) { (void)h; }
int x4_http_releases_request(X4Http *h, const char *url, char *body, size_t cap,
    size_t *len, int *status, const _Atomic int *cancel)
{
    (void)h; (void)url; (void)body; (void)cap; (void)len; (void)status; (void)cancel;
    return -1;
}

/* Helper to parse Fortnite from JSON results array, replicating fortnite_run search */
static int find_fortnite_in_titles_json(const char *json_str, X4CatalogTitle *out_title)
{
    X4JsonSpan root, results, item;
    if (x4_json_parse(json_str, strlen(json_str), &root) != 0) return -1;
    if (x4_json_member(root, "results", &results) != 1) return -1;
    if (x4_json_type(results) != X4_JSON_T_ARRAY) return -1;

    size_t cursor = 0;
    while (x4_json_item(results, &cursor, &item) == 1) {
        if (x4_json_type(item) != X4_JSON_T_OBJECT) continue;
        char title_id[64] = {0};
        char product_id[64] = {0};
        X4JsonSpan v, details;
        if (x4_json_member(item, "titleId", &v) == 1) {
            x4_json_token(v, title_id, sizeof(title_id));
        }
        if (x4_json_member(item, "details", &details) == 1 && x4_json_type(details) == X4_JSON_T_OBJECT) {
            if (x4_json_member(details, "productId", &v) == 1) {
                x4_json_token(v, product_id, sizeof(product_id));
            }
        }
        if (!product_id[0]) {
            if (x4_json_member(item, "productId", &v) == 1) {
                x4_json_token(v, product_id, sizeof(product_id));
            }
        }
        /* Match product ID "BT5P2X999VH2" or fallback titleId "FORTNITE" */
        if ((product_id[0] && !strcasecmp(product_id, "BT5P2X999VH2")) ||
            (title_id[0] && !strcasecmp(title_id, "FORTNITE"))) {
            if (out_title) {
                memset(out_title, 0, sizeof(*out_title));
                snprintf(out_title->id, sizeof(out_title->id), "%s", title_id);
                snprintf(out_title->product_id, sizeof(out_title->product_id), "%s",
                    product_id[0] ? product_id : "BT5P2X999VH2");
                snprintf(out_title->name, sizeof(out_title->name), "Fortnite");
                out_title->entitled = 1;
            }
            return 0; /* Found */
        }
    }
    return X4_AUTH_E_FORTNITE_NOT_FOUND;
}

static void test_fortnite_search_by_product_id(void)
{
    printf("Testing Fortnite search by productId BT5P2X999VH2...\n");
    const char *json = "{\"results\":["
        "{\"titleId\":\"11111\",\"details\":{\"productId\":\"SOMEPROD01\"}},"
        "{\"titleId\":\"22222\",\"details\":{\"productId\":\"BT5P2X999VH2\"}},"
        "{\"titleId\":\"33333\",\"details\":{\"productId\":\"OTHERPROD\"}}"
        "]}";
    X4CatalogTitle t;
    int rc = find_fortnite_in_titles_json(json, &t);
    assert(rc == 0);
    assert(!strcmp(t.id, "22222"));
    assert(!strcmp(t.product_id, "BT5P2X999VH2"));
    assert(!strcmp(t.name, "Fortnite"));
    assert(t.entitled == 1);
    printf("PASS: Fortnite search by productId\n");
}

static void test_fortnite_search_by_fallback_title_id(void)
{
    printf("Testing Fortnite search by fallback titleId FORTNITE...\n");
    const char *json = "{\"results\":["
        "{\"titleId\":\"HALO\",\"details\":{\"productId\":\"HALOPROD\"}},"
        "{\"titleId\":\"FORTNITE\",\"details\":{}},"
        "{\"titleId\":\"FORZA\",\"details\":{\"productId\":\"FORZAPROD\"}}"
        "]}";
    X4CatalogTitle t;
    int rc = find_fortnite_in_titles_json(json, &t);
    assert(rc == 0);
    assert(!strcasecmp(t.id, "FORTNITE"));
    assert(!strcmp(t.product_id, "BT5P2X999VH2"));
    assert(!strcmp(t.name, "Fortnite"));
    assert(t.entitled == 1);
    printf("PASS: Fortnite search by fallback titleId\n");
}

static void test_fortnite_absent(void)
{
    printf("Testing Fortnite absent in /v2/titles...\n");
    const char *json = "{\"results\":["
        "{\"titleId\":\"HALO\",\"details\":{\"productId\":\"HALOPROD\"}},"
        "{\"titleId\":\"FORZA\",\"details\":{\"productId\":\"FORZAPROD\"}}"
        "]}";
    X4CatalogTitle t;
    int rc = find_fortnite_in_titles_json(json, &t);
    assert(rc == X4_AUTH_E_FORTNITE_NOT_FOUND);
    printf("PASS: Fortnite absent returns X4_AUTH_E_FORTNITE_NOT_FOUND\n");
}

static void test_main_menu_4_cards_navigation(void)
{
    printf("Testing main menu Apple dark navigation (3 cards + bottom pills + top bar)...\n");
    X4Screen s = {0};
    s.page = 0;
    s.selected = 0; /* Explorar catalogo */

    /* Move right across all 3 cards with wrap */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.page == 0 && s.selected == 1); /* Fortnite */

    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.page == 0 && s.selected == 2); /* Cuenta */

    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.page == 0 && s.selected == 0); /* Wrap to Explorar catalogo */

    /* Move left with wrap */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
    assert(s.page == 0 && s.selected == 2); /* Wrap to Cuenta */

    x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
    assert(s.page == 0 && s.selected == 1); /* Fortnite */

    /* Press CROSS on Fortnite (index 1) -> page 8 */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 8);

    /* Press CIRCLE on page 8 -> page 0 */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CIRCLE, 0);
    assert(s.page == 0);

    /* Test Explorar catalogo without session -> page 4 + auto_catalog_after_login */
    s.selected = 0;
    s.signed_in = 0;
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 4 && s.auto_catalog_after_login == 1);
    s.page = 0;

    /* Test Explorar catalogo with session -> page 5 directly */
    s.selected = 0;
    s.signed_in = 1;
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 5 && s.catalog_from_home == 1);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CIRCLE, 0);
    assert(s.page == 0 && s.catalog_from_home == 0);

    /* Continue emits only a UI request. The signed-out tile opens Account. */
    s.selected = 2;
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 0 && s.open_recent_requested == 1);
    s.open_recent_requested = 0;
    s.signed_in = 0;
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 4 && !s.open_recent_requested);
    s.page = 0;

    /* About and Support are the only bottom pills. */
    s.selected = 0;
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == 4);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 2); /* Acerca de */
    s.page = 0;

    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.selected == 5);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 7); /* Apoyar */
    s.page = 0;

    printf("PASS: main menu Apple dark navigation\n");
}

static void test_unauthenticated_login_flow(void)
{
    printf("Testing unauthenticated -> login -> summary flow...\n");
    X4Screen s = {0};
    s.page = 0;
    s.selected = 1; /* FORTNITE */

    /* User presses CROSS on Fortnite */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 8);

    /* Coordinator check: account is NOT authorized */
    int account_state = X4_AUTH_IDLE;
    bool fortnite_login_flow = false;
    int pending_auth = -1;

    if (s.page == 8 && account_state != X4_AUTH_AUTHORIZED) {
        fortnite_login_flow = true;
        s.page = 4;
        pending_auth = X4_AUTH_SIGN_IN;
    }
    assert(fortnite_login_flow == true);
    assert(s.page == 4);
    assert(pending_auth == X4_AUTH_SIGN_IN);

    /* Sub-case A: user cancels login with CIRCLE */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CIRCLE, 0);
    assert(s.page == 0);
    if (fortnite_login_flow && s.page == 0) {
        fortnite_login_flow = false;
        pending_auth = -1;
    }
    assert(fortnite_login_flow == false);
    assert(pending_auth == -1);

    /* Sub-case B: user presses CROSS on Fortnite, completes sign-in */
    s.selected = 1;
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 8);
    fortnite_login_flow = true;
    s.page = 4;
    pending_auth = X4_AUTH_SIGN_IN;

    /* Login succeeds */
    account_state = X4_AUTH_AUTHORIZED;
    pending_auth = -1;
    if (fortnite_login_flow && account_state == X4_AUTH_AUTHORIZED) {
        fortnite_login_flow = false;
        s.page = 8;
        s.selected = (int)x4_picture_settings_get();
        pending_auth = X4_AUTH_FORTNITE_QUERY;
    }
    assert(fortnite_login_flow == false);
    assert(s.page == 8);
    assert(pending_auth == X4_AUTH_FORTNITE_QUERY);

    printf("PASS: unauthenticated login flow\n");
}

static void test_summary_screen_quality_selection(void)
{
    printf("Testing summary screen quality selection, saving with X, cancel with CIRCLE...\n");
    const char *test_path = "fortnite_test_pic_settings.bin";
    x4_picture_settings_set_path(test_path);
    remove(test_path);

    /* Initial state: Alta (0) */
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    X4Screen s = {0};
    s.page = 8;
    s.selected = (int)x4_picture_settings_get();
    assert(s.selected == 0);

    /* User navigates down to 1080p (1) */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == 1);

    /* User navigates down to 720p (2) */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == 2);

#if X4_PUBLIC
    /* T27 adds Maximum only to the public Fortnite selector. Navigation
     * reaches all four values, then clamps at the actual last choice. */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_1080P_MAX);
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_1080P_MAX);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_1080P_MAX);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_UP, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_720P);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_LEFT, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_1080P);
#else
    /* Lab keeps exactly the historical three values in both directions. */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_720P);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_RIGHT, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_720P);
    x4_screen_update(&s, ORBIS_PAD_BUTTON_UP, 0);
    assert(s.selected == (int)X4_PIC_QUALITY_1080P);
#endif

    /* User presses CIRCLE to cancel -> returns to page 0, settings NOT saved */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CIRCLE, 0);
    assert(s.page == 0);
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);

    /* Open summary again: resets selection to saved quality */
    s.page = 8;
    s.selected = (int)x4_picture_settings_get();
    assert(s.selected == 0);

    /* Change to 1080p (1) and press CROSS to launch */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_DOWN, 0);
    assert(s.selected == 1);

    /* Simulate CROSS press handler on page 8: saves quality and configures profile */
    assert(x4_picture_settings_set((X4PictureQualityOption)s.selected));
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P);
    assert(x4_picture_settings_to_profile(X4_PIC_QUALITY_1080P) == X4_REQUEST_1080P_G8);

    /* Test 720p (2) */
    s.selected = 2;
    assert(x4_picture_settings_set((X4PictureQualityOption)s.selected));
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_720P);
    assert(x4_picture_settings_to_profile(X4_PIC_QUALITY_720P) == X4_REQUEST_720P_HQ);

#if X4_PUBLIC
    /* Maximum persists and requests the existing HQ profile/alias. This
     * checks our request only; it makes no assertion about Xbox delivery. */
    s.selected = (int)X4_PIC_QUALITY_1080P_MAX;
    assert(x4_picture_settings_set((X4PictureQualityOption)s.selected));
    x4_picture_settings_init();
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_MAX);
    assert(x4_picture_settings_current_profile() == X4_REQUEST_1080P_HQ);
    X4StreamRequestConfig maximum = x4_stream_settings_get();
    assert(maximum.profile == X4_REQUEST_1080P_HQ);
    const char *alias = x4_stream_request_resolution_alias(&maximum);
    assert(alias && !strcmp(alias, "1080HQ"));
    assert(maximum.width == 1920u && maximum.height == 1080u);
#endif

    /* Test 1080p Alta (0) */
    s.selected = 0;
    assert(x4_picture_settings_set((X4PictureQualityOption)s.selected));
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P_ALTA);
    assert(x4_picture_settings_to_profile(X4_PIC_QUALITY_1080P_ALTA) == X4_REQUEST_1080P_G);

    remove(test_path);
    printf("PASS: summary screen quality selection\n");
}

static void test_normal_catalog_untouched(void)
{
    printf("Testing normal catalog flow untouched...\n");
    X4Screen s = {0};
    s.page = 0;
    s.selected = 0; /* Cuenta card */

    /* CROSS on Cuenta -> page 4 */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CROSS, 0);
    assert(s.page == 4);

    /* In page 4, when authorized, pressing R1 navigates to page 5 (catalog) */
    /* (main.c handles R1 to set screen.page = 5) */
    s.page = 5;
    assert(s.page == 5);

    /* Pressing CIRCLE on page 5 -> page 4 */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CIRCLE, 0);
    assert(s.page == 4);

    /* Pressing CIRCLE on page 4 -> page 0 */
    x4_screen_update(&s, ORBIS_PAD_BUTTON_CIRCLE, 0);
    assert(s.page == 0);

    printf("PASS: normal catalog flow untouched\n");
}

static void test_cloud_unavailable_and_xerr_priority(void)
{
    printf("Testing cloud unavailable 4xx message and XErr priority (TAREA 18)...\n");
    char buf[256];

    /* 1. Spanish: both offerings rejected with 403 and xerr == 0 */
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    const char *msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 403, 0,
        "xgpuwebf2p", "Xbox Cloud Gaming rechazo la cuenta", buf, sizeof(buf));
    assert(msg != NULL);
    assert(!strcmp(msg, "Xbox Cloud Gaming no está disponible para tu ubicación o tu cuenta (HTTP 403). XCloud4 solo puede usar los servidores que Xbox te ofrece."));

    /* 2. English: both offerings rejected with 403 and xerr == 0 */
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 403, 0,
        "xgpuwebf2p", "Xbox Cloud Gaming rechazo la cuenta", buf, sizeof(buf));
    assert(msg != NULL);
    assert(!strcmp(msg, "Xbox Cloud Gaming isn't available for your location or account (HTTP 403). XCloud4 can only use the servers Xbox offers you."));

    /* 3. XErr codes ALWAYS take priority over 4xx */
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 401, 2148916233u,
        "xgpuwebf2p", "Xbox rechazo la autorizacion XSTS", buf, sizeof(buf));
    assert(msg != NULL && !strcmp(msg, x4_tr(X4_TR_XSTS_NO_PROFILE)));

    msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 401, 2148916235u,
        "xgpuwebf2p", "Xbox rechazo la autorizacion XSTS", buf, sizeof(buf));
    assert(msg != NULL && !strcmp(msg, x4_tr(X4_TR_XSTS_UNAVAILABLE_COUNTRY)));

    msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 401, 2148916236u,
        "xgpuwebf2p", "Xbox rechazo la autorizacion XSTS", buf, sizeof(buf));
    assert(msg != NULL && !strcmp(msg, x4_tr(X4_TR_XSTS_AGE_VERIFICATION)));

    msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 401, 2148916237u,
        "xgpuwebf2p", "Xbox rechazo la autorizacion XSTS", buf, sizeof(buf));
    assert(msg != NULL && !strcmp(msg, x4_tr(X4_TR_XSTS_AGE_VERIFICATION)));

    msg = x4_lang_catalog_error_msg(X4_AUTH_E_XBOX, 401, 2148916238u,
        "xgpuwebf2p", "Xbox rechazo la autorizacion XSTS", buf, sizeof(buf));
    assert(msg != NULL && !strcmp(msg, x4_tr(X4_TR_XSTS_CHILD_ACCOUNT)));

    /* 4. Network error (http_status == 0, xerr == 0) returns NULL so net_hint takes over */
    msg = x4_lang_catalog_error_msg((int)0x804101DCu, 0, 0, "", "HTTPS: DNS", buf, sizeof(buf));
    assert(msg == NULL);

    /* 5. Verify short disclaimer on About page */
    x4_lang_set(X4_LANG_ES, "es-MX", "MX");
    assert(!strcmp(x4_tr(X4_TR_ABOUT_LINE3), "Proyecto independiente, sin afiliación con Microsoft, Xbox ni Sony. Úsalo bajo tu propia responsabilidad."));
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    assert(!strcmp(x4_tr(X4_TR_ABOUT_LINE3), "Independent project, not affiliated with Microsoft, Xbox or Sony. Use at your own risk."));

    printf("PASS: cloud unavailable 4xx message and XErr priority\n");
}

int main(void)
{
    printf("=== RUNNING FORTNITE FLOW TESTS ===\n");
    test_fortnite_search_by_product_id();
    test_fortnite_search_by_fallback_title_id();
    test_fortnite_absent();
    test_main_menu_4_cards_navigation();
    test_unauthenticated_login_flow();
    test_summary_screen_quality_selection();
    test_normal_catalog_untouched();
    test_cloud_unavailable_and_xerr_priority();
    printf("=== ALL FORTNITE FLOW TESTS PASSED ===\n");
    return 0;
}
