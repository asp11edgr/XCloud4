/* SPDX-License-Identifier: GPL-3.0-only */
/* Real quota worker and main poll against private HTTP doubles. */
#define _POSIX_C_SOURCE 200809L
#define X4_RUMBLE 0
#include "../src/auth/device_auth.c"
#include <assert.h>
#include <pthread.h>
#include <time.h>

static _Atomic uint64_t clock_us = 1000000;
static _Atomic unsigned calls, renewals;
static _Atomic bool block_query, fail_query, entered;
static _Atomic unsigned worker_opens, worker_closes;

uint64_t sceKernelGetProcessTime(void) { return atomic_load(&clock_us); }
int sceKernelUsleep(unsigned us)
{
    struct timespec t = {us / 1000000, (long)(us % 1000000) * 1000};
    return nanosleep(&t, NULL);
}
int scePthreadAttrInit(OrbisPthreadAttr *a) { return pthread_attr_init(a); }
int scePthreadAttrSetstacksize(OrbisPthreadAttr *a,size_t n) { return pthread_attr_setstacksize(a,n); }
int scePthreadAttrDestroy(OrbisPthreadAttr *a) { return pthread_attr_destroy(a); }
int scePthreadCreate(OrbisPthread *t, const OrbisPthreadAttr *a, void *(*run)(void *),
    void *u, const char *name)
{
    assert(!strcmp(name,"x4-quota"));
    return pthread_create(t,a,run,u);
}
int scePthreadJoin(OrbisPthread t,void **out) { return pthread_join(t,out); }
void x4_secure_clear(void *p,size_t n)
{
    volatile unsigned char *v=p;
    while (n--) *v++=0;
}

struct X4Http { pthread_t owner; };
X4Http *x4_http_open(int *error)
{
    X4Http *http=malloc(sizeof(*http));
    assert(http);
    http->owner=pthread_self();
    *error=0;
    atomic_fetch_add(&worker_opens,1);
    return http;
}
void x4_http_close(X4Http *http)
{
    assert(pthread_equal(http->owner,pthread_self()));
    atomic_fetch_add(&worker_closes,1);
    free(http);
}
int x4_http_request(X4Http *http,const char *url,const char *form,char *body,size_t capacity,
    size_t *length,int *status,const _Atomic int *cancel)
{
    assert(pthread_equal(http->owner,pthread_self()) && cancel);
    assert(!strcmp(url,X4_AUTH_TOKEN_URL) && strstr(form,"refresh_token="));
    static const char answer[]="{\"token_type\":\"Bearer\",\"access_token\":\"fixtureRenewedPrivate\","
        "\"refresh_token\":\"fixtureRotatedPrivate\",\"expires_in\":3600}";
    assert(sizeof(answer)<=capacity);
    memcpy(body,answer,sizeof(answer));
    *length=sizeof(answer)-1;
    *status=200;
    atomic_fetch_add(&renewals,1);
    return 0;
}
int x4_http_auth_reconnect_request(X4Http *http,bool passport,const char *form,char *body,size_t cap,
    size_t *len,int *status,const _Atomic int *cancel,uint64_t deadline)
{
    (void)http;(void)passport;(void)form;(void)body;(void)cap;(void)len;(void)status;(void)cancel;(void)deadline;
    assert(!"quota used a session reconnect deadline");
    return X4_HTTP_ARGUMENT;
}
int x4_xbox_cloud_time_query(X4Http *http,const char *token,const char *market,
    const _Atomic int *cancel,X4CloudTime *value,X4CloudTimeShape *shape,int *status)
{
    assert(pthread_equal(http->owner,pthread_self()) && cancel && !strcmp(market,"MX"));
    unsigned n=atomic_fetch_add(&calls,1)+1;
    assert(!strcmp(token,"fixtureMicrosoftPrivate") || !strcmp(token,"fixtureRenewedPrivate"));
    if (atomic_load(&renewals)) assert(!strcmp(token,"fixtureRenewedPrivate"));
    atomic_store(&entered,true);
    while (atomic_load(&block_query)) sceKernelUsleep(100);
    /* A late success deliberately ignores cancellation in the double. The
     * production mailbox must still reject the stale account's value. */
    if (atomic_load(&fail_query)) { *status=503; return X4_AUTH_E_STATUS; }
    *value=(X4CloudTime){1200+n,7200,false,X4_CLOUD_TIME_ACCOUNT};
    if (shape) {
        shape->length=7;
        shape->count=1;
        shape->fields[0]=(X4CloudTimeField){.number=3,.level=1,.wire=2};
    }
    *status=200;
    return 0;
}

static void wait_for_entry(void)
{
    for (unsigned i=0; i<10000 && !atomic_load(&entered); ++i) sceKernelUsleep(100);
    assert(atomic_load(&entered));
}

static void *reissue_credentials(void *context)
{
    X4Auth *a=context;
    for (unsigned i=0; i<1000; ++i) quota_credentials(a,false);
    return NULL;
}

int main(void)
{
    X4Auth *a=calloc(1,sizeof(*a));
    CloudQuota q={0};
    assert(a);
    a->quota=&q;
    q.epoch=1;
    atomic_flag_clear(&q.gate);
    atomic_init(&q.cancel,0);
    atomic_init(&q.finished,1);
    X4CloudTime value;
    uint64_t epoch, revision;
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(!q.running && epoch==1 && !revision && value.source==X4_CLOUD_TIME_NONE);

    strcpy(a->access_token,"fixtureMicrosoftPrivate");
    strcpy(a->refresh_token,"fixtureRefreshPrivate");
    a->token_expiry=now()+2*X4_CLOUD_TIME_INTERVAL;
    quota_credentials(a,true);
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(q.running && !quota_join(&q));
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(!q.running && atomic_load(&calls)==1 && revision==1 && value.remaining_s==1201);
    quota_lock(&q);
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(!q.running && epoch==1 && revision==1 && value.remaining_s==1201);
    quota_unlock(&q);
    atomic_fetch_add(&clock_us,X4_CLOUD_TIME_INTERVAL-1);
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(!q.running);
    atomic_fetch_add(&clock_us,1);
    atomic_store(&fail_query,true);
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(q.running && !quota_join(&q));
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(atomic_load(&calls)==2 && revision==1 && value.remaining_s==1201);

    atomic_store(&fail_query,false);
    atomic_store(&block_query,true);
    atomic_store(&entered,false);
    x4_auth_cloud_time(a,"MX",true,&value,&epoch,&revision);
    wait_for_entry();
    pthread_t auth;
    assert(!pthread_create(&auth,NULL,reissue_credentials,a));
    for (unsigned i=0; i<1000; ++i) {
        x4_auth_cloud_time(a,"MX",i==1,&value,&epoch,&revision);
        assert(revision==1 && value.remaining_s==1201);
    }
    assert(!pthread_join(auth,NULL));
    quota_reset(&q);
    assert(atomic_load(&q.cancel));
    atomic_store(&block_query,false);
    assert(!quota_join(&q));
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(!q.running && epoch==2 && !revision && value.source==X4_CLOUD_TIME_NONE);

    quota_credentials(a,true);
    atomic_fetch_add(&clock_us,X4_CLOUD_TIME_INTERVAL+1);
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(q.running && !quota_join(&q));
    x4_auth_cloud_time(a,"MX",false,&value,&epoch,&revision);
    assert(atomic_load(&renewals)==1 && revision==1 && value.remaining_s==1204);
    assert(!strcmp(q.credentials.refresh,"fixtureRotatedPrivate"));
    assert(!strcmp(a->refresh_token,"fixtureRefreshPrivate"));
    x4_auth_cloud_time(a,"MX",true,&value,&epoch,&revision);
    assert(q.running && !quota_join(&q));
    assert(atomic_load(&renewals)==1 && atomic_load(&calls)==5);
    assert(atomic_load(&worker_opens)==atomic_load(&worker_closes));
    x4_secure_clear(a,sizeof(*a));
    x4_secure_clear(&q,sizeof(q));
    free(a);
    puts("PASS quota worker: first login, periodic and forced polls, TLS ownership, renew in RAM, stale results and cancellation");
    return 0;
}
