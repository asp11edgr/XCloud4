/* SPDX-License-Identifier: GPL-3.0-only */
/* Real device_auth worker lifecycle; only Xbox exchange results are doubled.
 * Complements access_country_auth_http_test.c, which runs the real logins. */
#define X4_AUTH_STARTUP_TEST 1
#define X4_ACCESS_COUNTRY_AUTH_TEST 1
#define main access_lifecycle_base_main
#include "auth_recent_lifecycle_test.c"
#undef main
#include "../src/streaming/keyframe_limiter.h"

static void wait_country(unsigned route)
{
    for(unsigned i=0;i<2000&&!atomic_load(&fixture_country_entered);++i)sceKernelUsleep(1000);
    CHECK(atomic_load(&fixture_country_entered)==route);
}
int main(void)
{
    CHECK(x4_auth_access_country(NULL)==X4_ACCESS_AUTO);
    for(unsigned country=0;country<X4_ACCESS_COUNTRY_COUNT;++country){
        X4Auth *a=x4_auth_create();CHECK(a);authorize_host(a);
        CHECK(x4_auth_access_country(a)==X4_ACCESS_AUTO);
        for(unsigned route=1;route<=4;++route){
            atomic_store(&fixture_country_entered,0);atomic_store(&fixture_country_block,1);
            atomic_store(&fixture_access_country,country);fixture_country_expected=(X4AccessCountry)country;
            int result;
            if(route<4){
                enum X4AuthAction action=route==1?X4_AUTH_XBOX_CATALOG:route==2?X4_AUTH_FORTNITE_QUERY:X4_AUTH_XBOX_RECENTS;
                result=x4_auth_start(a,action);
            }else{
                X4KeyframeLimiter limiter;x4_keyframe_limiter_init(&limiter);
                CHECK(!x4_auth_set_keyframe_limiter(a,&limiter));fixture_expected_limiter=&limiter;
                X4StreamRequestConfig request=x4_stream_request_config(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709);
                result=x4_auth_start_session_request(a,0,&request);CHECK(!result);wait_country(route);
                atomic_store(&fixture_access_country,(country+1u)%X4_ACCESS_COUNTRY_COUNT);
                CHECK(x4_auth_access_country(a)==(X4AccessCountry)country);
                CHECK(x4_auth_start(a,X4_AUTH_XBOX_CATALOG)==X4_AUTH_E_BUSY);
                atomic_store(&fixture_country_block,0);wait_finished(a);
                X4SessionSnapshot session;x4_auth_session_snapshot(a,&session);
                CHECK(session.access_country==(X4AccessCountry)country);
                CHECK(!x4_auth_set_media_callback(a,NULL,NULL,NULL));
                fixture_expected_limiter=NULL;
            }
            CHECK(!result);
            if(route<4){
                wait_country(route);CHECK(x4_auth_access_country(a)==(X4AccessCountry)country);
                atomic_store(&fixture_access_country,(country+1u)%X4_ACCESS_COUNTRY_COUNT);
                CHECK(x4_auth_access_country(a)==(X4AccessCountry)country);
                CHECK(x4_auth_start(a,X4_AUTH_XBOX_CATALOG)==X4_AUTH_E_BUSY);
                atomic_store(&fixture_country_block,0);wait_finished(a);
            }
            X4AuthSnapshot account;x4_auth_snapshot(a,&account);
            CHECK(account.state==X4_AUTH_AUTHORIZED&&a->token_expiry);
            CHECK(!strcmp(a->access_token,"private-host-token")&&!strcmp(a->refresh_token,"private-host-refresh"));
            CHECK(x4_auth_access_country(a)==(X4AccessCountry)country);
        }
        /* Cancellation and a next explicit retry preserve the account. */
        atomic_store(&fixture_access_country,country);fixture_country_expected=(X4AccessCountry)country;
        atomic_store(&fixture_country_entered,0);atomic_store(&fixture_country_block,1);
        CHECK(!x4_auth_start(a,X4_AUTH_XBOX_RECENTS));wait_country(3);x4_auth_cancel(a);
        atomic_store(&fixture_country_block,0);wait_finished(a);
        CHECK(a->token_expiry&&!strcmp(a->access_token,"private-host-token"));
        CHECK(!x4_auth_start(a,X4_AUTH_XBOX_CATALOG));wait_finished(a);
        CHECK(a->catalog.state==X4_CATALOG_READY&&a->shared.view.state==X4_AUTH_AUTHORIZED);
        CHECK(!x4_auth_close(a));
    }
    printf("PASS access-country auth capture: %u checks; six countries x four worker routes, mutation while busy, duplicate refusal, cancel/retry without Microsoft logout; no network\n",atomic_load(&checks));
    return 0;
}
