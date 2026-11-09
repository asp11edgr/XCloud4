/* SPDX-License-Identifier: GPL-3.0-only */
/* Phase B: real production HTTP/TLS/wire. Link wrappers only count approved
 * destinations and collect TLS metadata. No native-ABI emulation remains. */
#define _POSIX_C_SOURCE 200809L
#include "../src/auth/http_client.h"
#include "../src/auth/tls_conn.h"
#include "../src/ui/lang.h"
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>

enum { BODY_CAPACITY=X4_HTTP_RESPONSE_MAX+1u };
static X4TlsHttp *active_tls;
static X4TlsInfo latest;
static bool regional_retry;
static const char *const hosts[]={
    "login.microsoftonline.com","login.live.com","user.auth.xboxlive.com","xsts.auth.xboxlive.com",
    "xgpuweb.gssv-play-prod.xboxlive.com","xgpuwebf2p.gssv-play-prod.xboxlive.com",
    "eastus.core.gssv-play-prod.xboxlive.com","westeurope.core.gssv-play-prod.xboxlive.com",
    "brazilsouth.core.gssv-play-prod.xboxlive.com","mexicocentral.core.gssv-play-prod.xboxlive.com",
    "cia.core.gssv-play-prod.xboxlive.com","displaycatalog.mp.microsoft.com",
    "store-images.s-microsoft.com","api.github.com"
};
static unsigned connections[sizeof(hosts)/sizeof(*hosts)];
int poc_quiet_printf(const char *format,...){(void)format;return 0;}
uint64_t sceKernelGetProcessTime(void){return x4_tls_now_us();}
X4TlsHttp *__real_x4_tls_http_open(int *);
X4TlsHttp *__wrap_x4_tls_http_open(int *error)
{active_tls=__real_x4_tls_http_open(error);return active_tls;}
int __real_x4_tls_http_close(X4TlsHttp *);
int __wrap_x4_tls_http_close(X4TlsHttp *h)
{int rc=__real_x4_tls_http_close(h);if(!rc)active_tls=NULL;return rc;}
int __real_x4_tls_conn_connect(X4TlsHttp *,const char *,const X4TlsBudget *,X4TlsConn **);
int __wrap_x4_tls_conn_connect(X4TlsHttp *h,const char *host,const X4TlsBudget *b,X4TlsConn **c)
{
    size_t i=0;while(i<sizeof(hosts)/sizeof(*hosts)&&strcmp(host,hosts[i]))++i;
    if(i==sizeof(hosts)/sizeof(*hosts)||connections[i]>=2||
       (regional_retry&&(i<6||i>9||connections[i]>=1))){*c=NULL;return X4_HTTP_ARGUMENT;}
    ++connections[i];int rc=__real_x4_tls_conn_connect(h,host,b,c);
    latest=x4_tls_http_info(h);return rc;
}
int __real_x4_tls_conn_close(X4TlsConn *);
int __wrap_x4_tls_conn_close(X4TlsConn *c)
{if(c)latest=x4_tls_conn_info(c);return __real_x4_tls_conn_close(c);}

static void json_string(const char *s)
{
    putchar('"');for(;s&&*s;++s){unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\')printf("\\%c",c);else if(c<32)printf("\\u%04x",c);else putchar(c);}
    putchar('"');
}
enum Api {OPENID,PASSPORT,USER_AUTH,XSTS_AUTH,LOGIN,TITLES,METADATA,IMAGE,RELEASES};
typedef struct {const char *host,*url;enum Api api;int expected;} Probe;
static int probe_one(X4Http *h,const Probe *p,unsigned char *body)
{
    size_t length=0;int status=0,rc=X4_HTTP_ARGUMENT;latest=(X4TlsInfo){.anchor_index=-1};
    const X4HttpHeader contract={"x-xbl-contract-version","1"};
    const X4HttpHeader login_headers[]={{"x-gssv-client","XboxComBrowser"},{"X-MS-Device-Info","{}"}};
    switch(p->api){
    case OPENID:rc=x4_http_request(h,p->url,NULL,(char *)body,BODY_CAPACITY,&length,&status,NULL);break;
    case PASSPORT:rc=x4_http_passport_request(h,"grant_type=refresh_token",(char *)body,X4_HTTP_PASSPORT_BODY_MAX+1u,&length,&status,NULL);break;
    case USER_AUTH:case XSTS_AUTH:rc=x4_http_json_request(h,p->url,&contract,1,"{}",(char *)body,BODY_CAPACITY,&length,&status,NULL);break;
    case LOGIN:rc=x4_http_json_request(h,p->url,login_headers,2,"{}",(char *)body,BODY_CAPACITY,&length,&status,NULL);break;
    case TITLES:rc=x4_http_json_request(h,p->url,NULL,0,NULL,(char *)body,BODY_CAPACITY,&length,&status,NULL);break;
    case METADATA:rc=x4_http_public_metadata_request(h,p->url,(char *)body,BODY_CAPACITY,&length,&status,NULL);break;
    case IMAGE:rc=x4_http_public_image_request(h,p->url,body,BODY_CAPACITY,&length,&status,NULL);break;
    case RELEASES:rc=x4_http_releases_request(h,p->url,(char *)body,X4_HTTP_RELEASES_BODY_MAX+1u,&length,&status,NULL);break;
    }
    if(active_tls)latest=x4_tls_http_info(active_tls);
    bool expected=p->expected==400?status>=400&&status<500:status==p->expected;
    printf("{\"host\":");json_string(p->host);printf(",\"status\":%d,\"result\":%d,\"verified\":%s,\"verify_flags\":%u,\"version\":",status,rc,latest.verified?"true":"false",latest.verify_flags);
    json_string(latest.version);printf(",\"suite\":");json_string(latest.suite);
    printf(",\"handshake_us\":%llu,\"chain_unordered\":%s,\"chain_reordered\":%s,\"anchor_index\":%d,\"anchor_name\":",(unsigned long long)latest.handshake_us,latest.chain_unordered?"true":"false",latest.chain_reordered?"true":"false",latest.anchor_index);
    json_string(latest.anchor_name);printf(",\"anchor_sha256\":");json_string(latest.anchor_sha256);
    printf(",\"stage\":");json_string(latest.stage?latest.stage:x4_http_stage(h));
    printf(",\"body_bytes\":%zu,\"expected_status\":%s,\"real_production_API\":true,\"credentials_sent\":false}\n",length,expected?"true":"false");
    x4_secure_clear(body,BODY_CAPACITY);
    if(latest.verify_flags&MBEDTLS_X509_BADCERT_NOT_TRUSTED)return 77;
    return !rc&&expected&&latest.verified&&!latest.verify_flags&&latest.anchor_index>=0?0:1;
}
static int cbc_control(X4Http *h)
{
    if(x4_http_public_reset(h)||!active_tls)return 1;
    int rc=x4_tls_http_test_versions(active_tls,true,false,true);if(rc)return 1;
    X4TlsBudget b={.deadline_us=x4_tls_now_us()+UINT64_C(15000000)};X4TlsConn *c=NULL;
    rc=__wrap_x4_tls_conn_connect(active_tls,hosts[2],&b,&c);latest=x4_tls_http_info(active_tls);
    bool expected=rc==X4_HTTP_TLS_ALERT||rc==(int32_t)UINT32_C(0x80410136);
    printf("{\"host\":\"user.auth.xboxlive.com\",\"control\":\"RSA_CBC\",\"result\":%d,\"expected_rejection\":%s}\n",rc,expected?"true":"false");
    if(c)(void)__wrap_x4_tls_conn_close(c);return expected?0:1;
}
int main(int argc,char **argv)
{
    if(argc!=2||(strcmp(argv[1],"--approved-integrated-hosts")&&
       strcmp(argv[1],"--approved-integrated-regional-retry"))||!getenv("X4_ALLOW_REAL_TLS_POC"))return 2;
    regional_retry=!strcmp(argv[1],"--approved-integrated-regional-retry");
    if(prctl(PR_SET_DUMPABLE,0,0,0,0))return 2;
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    const Probe probes[]={
        {hosts[0],"https://login.microsoftonline.com/consumers/v2.0/.well-known/openid-configuration",OPENID,200},
        {hosts[1],NULL,PASSPORT,400},
        {hosts[2],"https://user.auth.xboxlive.com/user/authenticate",USER_AUTH,400},
        {hosts[3],"https://xsts.auth.xboxlive.com/xsts/authorize",XSTS_AUTH,400},
        {hosts[4],"https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user",LOGIN,400},
        {hosts[5],"https://xgpuwebf2p.gssv-play-prod.xboxlive.com/v2/login/user",LOGIN,400},
        {hosts[6],"https://eastus.core.gssv-play-prod.xboxlive.com/v2/titles",TITLES,401},
        {hosts[7],"https://westeurope.core.gssv-play-prod.xboxlive.com/v2/titles",TITLES,401},
        {hosts[8],"https://brazilsouth.core.gssv-play-prod.xboxlive.com/v2/titles",TITLES,401},
        {hosts[9],"https://mexicocentral.core.gssv-play-prod.xboxlive.com/v2/titles",TITLES,401},
        {hosts[10],"https://cia.core.gssv-play-prod.xboxlive.com/v2/titles",TITLES,401},
        {hosts[11],"https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=9NKX70BBCDRN&market=MX&languages=es-MX&fieldsTemplate=Details",METADATA,200},
        {hosts[12],"https://store-images.s-microsoft.com/image/apps.43531.13718773309227929.bebdcc0e-1ed5-4778-8732-f4ef65a2f445.842884f6-9442-49d9-bd51-bdf56d9ed3ec?w=512&q=80&format=jpg",IMAGE,200},
        {hosts[13],X4_HTTP_RELEASES_URL_LAB,RELEASES,200}
    };
    int error=0;X4Http *h=x4_http_open(&error);if(!h)return 1;
    unsigned char *body=malloc(BODY_CAPACITY);if(!body){x4_http_close(h);return 1;}
    unsigned failed=0;bool stopped=false;
    for(size_t i=0;i<sizeof(probes)/sizeof(*probes);++i){
        if(regional_retry&&(i<6||i>9))continue;
        int rc=probe_one(h,&probes[i],body);
        if(rc==77){stopped=true;failed++;break;}failed+=(unsigned)rc;
    }
    if(!stopped&&!regional_retry)failed+=(unsigned)cbc_control(h);
    x4_secure_clear(body,BODY_CAPACITY);free(body);x4_http_close(h);
    for(size_t i=0;i<sizeof(connections)/sizeof(*connections);++i)if(connections[i]>2)failed++;
    printf("{\"summary\":\"phase_B_integrated_production_HTTP\",\"failed\":%u,\"stop_untrusted_chain\":%s,\"credentials_sent\":false,\"max_connections_per_host\":2}\n",failed,stopped?"true":"false");
    return stopped?77:failed?1:0;
}
