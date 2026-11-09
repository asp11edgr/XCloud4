/* SPDX-License-Identifier: GPL-3.0-only */
/* Real XBL/XSTS constructors and HTTPS wire policy with a local TLS double. */
#define _POSIX_C_SOURCE 200809L
#include <strings.h>
#include "../src/auth/xbox_live.c"
#include "tls_transport_fake.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"quota HTTP check failed at %d\n",__LINE__); abort(); } } while (0)
static const char quota_url[]=X4_HTTP_CLOUD_TIME_PREFIX "MX" X4_HTTP_CLOUD_TIME_SUFFIX;
static const char authorization[]="XBL3.0 x=2468101214;fixtureXstsPrivate";
static const char cv[]="AAECAwQFBgcICQoL.0";
static unsigned user_requests, xsts_requests, quota_requests;
static _Atomic int cancel_flag;

uint64_t sceKernelGetProcessTime(void) { return 1000000; }
int sceKernelUsleep(unsigned usec) { (void)usec; return 0; }
const char *x4_lang_store_suffix(void)
{ return "&market=MX&languages=es-MX&fieldsTemplate=Details"; }
int x4_native_random(void *data,size_t n)
{
    unsigned char *out=data;
    for (size_t i=0; i<n; ++i) out[i]=(unsigned char)i;
    return 0;
}

static void reply(const X4TfRequest *r,X4TfResponse *response)
{
    CHECK(!r->keepalive && !x4tf_header(r,"Cookie"));
    if (!strcmp(r->url,X4_XBOX_USER_URL)) {
        ++user_requests;
        CHECK(r->method==1 && strstr(r->body,"d=fixtureMicrosoftPrivate"));
        CHECK(!x4tf_header(r,"Authorization") && !x4tf_header(r,"MS-CV"));
        response->body="{\"Token\":\"fixtureUserPrivate\",\"DisplayClaims\":{\"xui\":[{\"gtg\":\"fixtureGamertagPrivate\"}]}}";
    } else if (!strcmp(r->url,X4_XBOX_XSTS_URL)) {
        ++xsts_requests;
        CHECK(r->method==1 && strstr(r->body,"\"RelyingParty\":\"http://mp.microsoft.com/\""));
        CHECK(strstr(r->body,"fixtureUserPrivate"));
        CHECK(!x4tf_header(r,"Authorization") && !x4tf_header(r,"MS-CV"));
        response->body="{\"Token\":\"fixtureXstsPrivate\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"2468101214\","
            "\"gtg\":\"fixtureGamertagPrivate\",\"xid\":\"fixtureXuidPrivate\"}]}}";
    } else {
        ++quota_requests;
        CHECK(!strcmp(r->url,quota_url) && r->method==0 && !r->body_size);
        CHECK(!strcmp(x4tf_header(r,"Authorization"),authorization));
        CHECK(!strcmp(x4tf_header(r,"MS-CV"),cv));
        CHECK(!strcmp(x4tf_header(r,"Accept"),"application/x-protobuf, application/json"));
        response->body="{\"remainingGameplayTimeInSeconds\":1200,\"maxGameplayTimeInSeconds\":7200,"
            "\"token\":\"fixtureQuotaBodyPrivate\",\"puid\":\"fixturePuidPrivate\",\"gamertag\":\"fixtureGamertagPrivate\"}";
    }
    response->status=200;
}

static void check_closed_policy(X4Http *http)
{
    const X4HttpHeader good[]={{"Authorization",authorization},{"MS-CV",cv}};
    char body[256];
    size_t length;
    int status;
    const char *const rejected[]={
        "https://contentaccess.exp.xboxservices.com.evil.invalid/all/v1?market=MX&offering=xgpuweb",
        "https://contentaccess.exp.xboxservices.com:443/all/v1?market=MX&offering=xgpuweb",
        "http://contentaccess.exp.xboxservices.com/all/v1?market=MX&offering=xgpuweb",
        "https://contentaccess.exp.xboxservices.com/all/v2?market=MX&offering=xgpuweb",
        "https://contentaccess.exp.xboxservices.com/all/v1?offering=xgpuweb&market=MX",
        "https://contentaccess.exp.xboxservices.com/all/v1?market=mx&offering=xgpuweb",
        "https://contentaccess.exp.xboxservices.com/all/v1?market=MX&offering=xgpuwebf2p",
        "https://contentaccess.exp.xboxservices.com/all/v1?market=MX&offering=xgpuweb&extra=1",
        "https://contentaccess.exp.xboxservices.com/all/v1?market=MX&offering=xgpuweb#fragment",
        "https://contentaccess.exp.xboxservices.com/all/v1?market=M%58&offering=xgpuweb",
        "https://contentaccess.exp.xboxservices.com/all/v1?market=MX&offering=xgpuweb&market=US"
    };
    unsigned before=x4tf.requests;
    for (size_t i=0; i<sizeof(rejected)/sizeof(rejected[0]); ++i) {
        length=99;
        status=99;
        CHECK(x4_http_cloud_time_request(http,rejected[i],good,2,body,sizeof(body),&length,&status,
            &cancel_flag)==X4_HTTP_URL);
        CHECK(!length && !status && x4tf.requests==before);
    }
    const X4HttpHeader rejected_headers[][2]={
        {{"Authorization","Bearer fixtureBadPrivate"},{"MS-CV",cv}},
        {{"Authorization","XBL3.0 x=abcd;fixtureBadPrivate"},{"MS-CV",cv}},
        {{"Authorization",authorization},{"MS-CV","short.0"}},
        {{"Authorization",authorization},{"Cookie","fixtureBadPrivate"}},
        {{"Authorization",authorization},{"Authorization",authorization}},
        {{"Authorization",authorization},{"MS-CV","AAECAwQFBgcICQoL\r\n"}}
    };
    for (size_t i=0; i<sizeof(rejected_headers)/sizeof(rejected_headers[0]); ++i) {
        CHECK(x4_http_cloud_time_request(http,quota_url,rejected_headers[i],2,body,sizeof(body),
            &length,&status,&cancel_flag)!=0 && x4tf.requests==before);
    }
    CHECK(x4_http_cloud_time_request(http,quota_url,good,1,body,sizeof(body),&length,&status,
        &cancel_flag)==X4_HTTP_ARGUMENT);
    CHECK(x4_http_json_request(http,quota_url,good,2,NULL,body,sizeof(body),&length,&status,
        &cancel_flag)==X4_HTTP_URL);
    CHECK(x4_http_json_request(http,"https://fixture.gssv-play-prod.xboxlive.com/v2/titles",good,2,NULL,
        body,sizeof(body),&length,&status,&cancel_flag)==X4_HTTP_ARGUMENT);
    CHECK(x4tf.requests==before);
}

int main(void)
{
    x4tf_reset();
    x4tf.on_request=reply;
    int rc=-1, status=0;
    X4Http *http=x4_http_open(&rc);
    CHECK(http && !rc);
    check_closed_policy(http);
    X4CloudTime value={0};
    X4CloudTimeShape shape={0};
    CHECK(!x4_xbox_cloud_time_query(http,"fixtureMicrosoftPrivate","MX",&cancel_flag,&value,&shape,&status));
    CHECK(status==200 && user_requests==1 && xsts_requests==1 && quota_requests==1);
    CHECK(value.source==X4_CLOUD_TIME_JSON && value.remaining_s==1200 && value.max_s==7200);
    CHECK(shape.length>0 && shape.count==0);
    x4_http_close(http);
    CHECK(x4tf.connections==3 && x4tf.connection_closes==3 && !x4tf.live_contexts);
    puts("PASS quota HTTP: exact URL and header policy, XSTS relying party and sanitized export");
    return 0;
}
