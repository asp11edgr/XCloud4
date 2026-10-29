/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define X4_RUMBLE 0
#include "../src/auth/device_auth.c"
#include <pthread.h>
#include <time.h>

static _Atomic unsigned checks, catalog_runs, recent_runs;
static _Atomic int block_recent, recent_entered;
#ifdef X4_AUTH_STARTUP_TEST
static int fixture_join_error;
static unsigned fixture_join_calls;
static _Atomic int fixture_session_entered,fixture_block_session;
static X4StreamStartup *fixture_expected_startup;
static X4KeyframeLimiter *fixture_expected_limiter;
static X4SessionMediaCallback fixture_expected_callback;
static void *fixture_expected_user;
static X4SessionSnapshot fixture_session_result;
#endif
static uint64_t clock_us=1000000;
#define CHECK(x) do{atomic_fetch_add(&checks,1);if(!(x)){fprintf(stderr,"auth MRU check failed line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
uint64_t sceKernelGetProcessTime(void){return clock_us;}
int sceKernelUsleep(unsigned us){struct timespec t={us/1000000,(long)(us%1000000)*1000};nanosleep(&t,NULL);return 0;}
int scePthreadCreate(OrbisPthread *t,const pthread_attr_t *a,void*(*f)(void*),void *u,const char *n){(void)n;return pthread_create(t,a,f,u);}
int scePthreadJoin(OrbisPthread t,void **v){
#ifdef X4_AUTH_STARTUP_TEST
    ++fixture_join_calls;if(fixture_join_error)return fixture_join_error;
#endif
    return pthread_join(t,v);}
void x4_secure_clear(void *p,size_t n){volatile unsigned char *b=p;while(n--)*b++=0;}
struct X4Http{int unused;};
X4Http *x4_http_open(int *error){if(error)*error=0;return calloc(1,sizeof(X4Http));}
void x4_http_close(X4Http *h){free(h);}
const char *x4_http_stage(const X4Http *h){(void)h;return "host controlled transport";}
int x4_http_request(X4Http *h,const char *url,const char *form,char *body,size_t cap,size_t *length,int *status,const _Atomic int *cancel)
{(void)h;(void)url;(void)form;(void)cancel;if(body&&cap)body[0]=0;*length=0;*status=0;return X4_HTTP_PROTOCOL;}
int x4_http_passport_request(X4Http *h,const char *form,char *body,size_t cap,size_t *length,int *status,const _Atomic int *cancel)
{return x4_http_request(h,"",form,body,cap,length,status,cancel);}
static _Atomic unsigned fixture_access_country;
X4AccessCountry x4_access_country_get(void){return (X4AccessCountry)atomic_load(&fixture_access_country);}
struct X4XboxWork{X4CatalogSnapshot catalog;X4RecentSnapshot recent;X4AccessCountry access_country;
#ifdef X4_AUTH_STARTUP_TEST
    X4StreamRequestConfig request;X4StreamStartup *startup;X4KeyframeLimiter *limiter;
#endif
};
X4XboxWork *x4_xbox_work_new(void){return calloc(1,sizeof(X4XboxWork));}
bool x4_xbox_work_set_access_country(X4XboxWork *w,X4AccessCountry country)
{CHECK(w&&x4_access_country_valid(country));w->access_country=country;return true;}
#ifdef X4_ACCESS_COUNTRY_AUTH_TEST
static _Atomic unsigned fixture_country_entered,fixture_country_block;
static X4AccessCountry fixture_country_expected;
static void fixture_country_record(X4XboxWork *w,unsigned route,const _Atomic int *cancel)
{
    CHECK(w->access_country==fixture_country_expected);
    atomic_store(&fixture_country_entered,route);
    while(atomic_load(&fixture_country_block)&&!atomic_load(cancel))sceKernelUsleep(1000);
    CHECK(w->access_country==fixture_country_expected);
}
#else
#define fixture_country_record(w,route,cancel) ((void)0)
#endif
void x4_xbox_work_free(X4XboxWork *w){if(w){x4_secure_clear(w,sizeof(*w));free(w);}}
#ifdef X4_AUTH_STARTUP_TEST
bool x4_xbox_work_set_request_config(X4XboxWork *w,const X4StreamRequestConfig *c)
{CHECK(w&&c);w->request=*c;return true;}
bool x4_xbox_work_set_stream_startup(X4XboxWork *w,X4StreamStartup *startup)
{CHECK(w);w->startup=startup;return true;}
bool x4_xbox_work_set_keyframe_limiter(X4XboxWork *w,X4KeyframeLimiter *limiter)
{CHECK(w&&limiter);w->limiter=limiter;return true;}
#endif
const X4CatalogSnapshot *x4_xbox_catalog(X4XboxWork *w,const char *token,const _Atomic int *cancel,X4XboxProgress progress,void *context)
{
    fixture_country_record(w,1,cancel);
    (void)cancel;CHECK(!strcmp(token,"private-host-token"));atomic_fetch_add(&catalog_runs,1);
    if(progress)progress(context,"host catalog", "xgpuweb","host-region");
    w->catalog.state=X4_CATALOG_READY;w->catalog.count=1;strcpy(w->catalog.titles[0].id,"CURRENT_TITLE");strcpy(w->catalog.titles[0].product_id,"PROD1");strcpy(w->catalog.offering,"xgpuweb");
    w->recent.state=X4_CATALOG_READY;w->recent.count=1;strcpy(w->recent.title_ids[0],"CURRENT_TITLE");return &w->catalog;
}
const X4CatalogSnapshot *x4_xbox_fortnite_query(X4XboxWork *w,const char *token,const _Atomic int *cancel,X4XboxProgress progress,void *context)
{
    fixture_country_record(w,2,cancel);
    (void)token; (void)cancel; (void)progress; (void)context;
    w->catalog.state=X4_CATALOG_READY;w->catalog.count=1;strcpy(w->catalog.titles[0].id,"FORTNITE");strcpy(w->catalog.titles[0].product_id,"BT5P2X999VH2");strcpy(w->catalog.titles[0].name,"Fortnite");strcpy(w->catalog.offering,"xgpuweb");strcpy(w->catalog.region,"host-region");
    return &w->catalog;
}
const X4RecentSnapshot *x4_xbox_catalog_recent(const X4XboxWork *w){return &w->recent;}
const X4RecentSnapshot *x4_xbox_recents(X4XboxWork *w,const char *token,const _Atomic int *cancel)
{
    fixture_country_record(w,3,cancel);
    CHECK(!strcmp(token,"private-host-token"));atomic_fetch_add(&recent_runs,1);atomic_store(&recent_entered,1);
    while(atomic_load(&block_recent)&&!atomic_load(cancel))sceKernelUsleep(1000);
    /* Keep cancellation cleanup pending until the main test observes its
     * immediate public cancellation stage, then explicitly release it. */
    while(atomic_load(&block_recent)&&atomic_load(cancel))sceKernelUsleep(1000);
    w->recent.state=atomic_load(cancel)?X4_CATALOG_CANCELLED:X4_CATALOG_READY;
    if(!atomic_load(cancel)){w->recent.count=1;strcpy(w->recent.title_ids[0],"ANOTHER_TITLE");}return &w->recent;
}
const X4SessionSnapshot *x4_xbox_session(X4XboxWork *w,const char *token,const X4CatalogTitle *title,const char *offering,const _Atomic int *cancel,
    X4SessionProgress progress,void *context,X4XboxPassport passport,void *passport_context,X4SessionMediaCallback callback,void *user,
    _Atomic int *keyframe,X4GamepadSource source,void *gamepad_user,X4Trace *trace,X4Rumble *rumble,X4NackGraceInbox *inbox)
{
    (void)w;(void)token;(void)title;(void)offering;(void)cancel;(void)progress;(void)context;(void)passport;(void)passport_context;(void)callback;(void)user;(void)keyframe;(void)source;(void)gamepad_user;(void)trace;(void)rumble;(void)inbox;
#ifdef X4_AUTH_STARTUP_TEST
    CHECK(w->startup==fixture_expected_startup&&callback==fixture_expected_callback&&user==fixture_expected_user);
    fixture_country_record(w,4,cancel);
    CHECK(w->limiter==fixture_expected_limiter);
    CHECK(w->request.profile==X4_REQUEST_720P_HQ&&w->request.width==1280&&w->request.height==720);
    atomic_store(&fixture_session_entered,1);
    while(atomic_load(&fixture_block_session)&&!atomic_load(cancel))sceKernelUsleep(1000);
    fixture_session_result.state=X4_SESSION_CLOSED;
    fixture_session_result.access_country=w->access_country;return &fixture_session_result;
#else
    CHECK(0);return NULL; /* A UI-only fixture must never start streaming. */
#endif
}
static void wait_finished(X4Auth *a){for(unsigned i=0;i<2000&&x4_auth_busy(a);++i)sceKernelUsleep(1000);CHECK(!x4_auth_busy(a));}
static void authorize_host(X4Auth *a){strcpy(a->access_token,"private-host-token");strcpy(a->refresh_token,"private-host-refresh");a->token_expiry=clock_us+10000000;publish(a,X4_AUTH_AUTHORIZED,0,200,"host authorized",NULL,NULL,0);}
int main(void)
{
    X4Auth *a=x4_auth_create();CHECK(a);X4RecentSnapshot r;X4AuthRecentCursor cursor={0};CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(r.state==X4_CATALOG_IDLE&&r.account_epoch==1);CHECK(!x4_auth_recent_snapshot_cached(a,&r,&cursor));
    CHECK(x4_auth_start(a,X4_AUTH_XBOX_RECENTS)==X4_AUTH_E_SIGNED_OUT);authorize_host(a);CHECK(!x4_auth_start(a,X4_AUTH_XBOX_CATALOG));wait_finished(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(r.state==X4_CATALOG_READY&&r.count==1&&r.request_generation==1);CHECK(!strcmp(r.title_ids[0],"CURRENT_TITLE"));CHECK(atomic_load(&catalog_runs)==1&&!atomic_load(&recent_runs));
    X4CatalogSnapshot *catalog=malloc(sizeof(*catalog));CHECK(catalog);x4_auth_catalog_snapshot(a,catalog);CHECK(catalog->count==1);uint64_t catalog_revision=a->catalog_revision;X4CatalogSnapshot *saved=malloc(sizeof(*saved));CHECK(saved);*saved=*catalog;
    CHECK(!x4_auth_start(a,X4_AUTH_XBOX_RECENTS));wait_finished(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(r.state==X4_CATALOG_READY&&r.request_generation==2&&!strcmp(r.title_ids[0],"ANOTHER_TITLE"));CHECK(a->catalog_revision==catalog_revision);x4_auth_catalog_snapshot(a,catalog);CHECK(!memcmp(saved,catalog,sizeof(*saved)));CHECK(atomic_load(&catalog_runs)==1&&atomic_load(&recent_runs)==1);
    /* Old-account/request publication cannot replace a fresh view. */
    X4RecentSnapshot stale=r;strcpy(stale.title_ids[0],"STALE_TITLE");a->recent_job_generation=1;publish_recent(a,&stale);CHECK(!x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(!strcmp(r.title_ids[0],"ANOTHER_TITLE"));a->recent_job_generation=a->recent_generation;a->recent_job_account=0;publish_recent(a,&stale);CHECK(!x4_auth_recent_snapshot_cached(a,&r,&cursor));a->recent_job_account=a->account_epoch;
    atomic_store(&block_recent,1);atomic_store(&recent_entered,0);CHECK(!x4_auth_start(a,X4_AUTH_XBOX_RECENTS));for(unsigned i=0;i<2000&&!atomic_load(&recent_entered);++i)sceKernelUsleep(1000);CHECK(atomic_load(&recent_entered));CHECK(x4_auth_start(a,X4_AUTH_XBOX_RECENTS)==X4_AUTH_E_BUSY);x4_auth_cancel(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(strstr(r.stage,"cancelando"));atomic_store(&block_recent,0);wait_finished(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(r.state==X4_CATALOG_CANCELLED&&!r.count);
    uint64_t epoch=r.account_epoch;CHECK(!x4_auth_forget(a));CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(!r.count&&r.state==X4_CATALOG_IDLE&&r.account_epoch>epoch);CHECK(!a->access_token[0]&&!a->refresh_token[0]);
    authorize_host(a);CHECK(!x4_auth_start(a,X4_AUTH_XBOX_RECENTS));wait_finished(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(r.count==1);epoch=r.account_epoch;clock_us=a->token_expiry+1;CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(!r.count&&r.state==X4_CATALOG_IDLE&&r.account_epoch>epoch);
    authorize_host(a);CHECK(!x4_auth_start(a,X4_AUTH_XBOX_RECENTS));wait_finished(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));epoch=r.account_epoch;CHECK(!x4_auth_start(a,X4_AUTH_SIGN_IN));wait_finished(a);CHECK(x4_auth_recent_snapshot_cached(a,&r,&cursor));CHECK(!r.count&&r.account_epoch>epoch);
    free(saved);free(catalog);CHECK(!x4_auth_close(a));CHECK(x4_auth_recent_snapshot_cached(NULL,&r,&cursor));CHECK(r.state==X4_CATALOG_ERROR);CHECK(!x4_auth_recent_snapshot_cached(NULL,&r,&cursor));printf("auth MRU lifecycle PASS: %u checks; no streaming started\n",atomic_load(&checks));return 0;
}
