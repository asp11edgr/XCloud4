/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
static int quiet_printf(const char *format,...);
#define printf quiet_printf
#include "../src/auth/device_auth.c"
#undef printf
#include <assert.h>
struct X4SessionStore { bool saved; unsigned erases, rotations; };
static struct X4SessionStore save;
static uint64_t clock_us, cancel_at;
static X4Auth *owner;
static int fixture_status, fixture_rc;
static unsigned refresh_calls;
static int quiet_printf(const char *format,...) { (void)format;return 0; }
uint64_t sceKernelGetProcessTime(void) { return clock_us; }
int sceKernelUsleep(unsigned us)
{clock_us+=us;if(cancel_at&&clock_us>=cancel_at)atomic_store(&owner->cancel,1);return 0;}
void x4_secure_clear(void *p,size_t n) {volatile unsigned char *b=p;while(n--)*b++=0;}
int x4_session_store_erase(X4SessionStore *s) {assert(s==&save);s->saved=false;++s->erases;return 0;}
int x4_session_store_rotate(X4SessionStore *s,const char *token)
{assert(s==&save&&token&&token[0]);s->saved=true;++s->rotations;return 0;}
int x4_http_request(X4Http *http,const char *url,const char *form,char *body,size_t capacity,
    size_t *length,int *status,const _Atomic int *cancel)
{
    (void)http;assert(!strcmp(url,X4_AUTH_TOKEN_URL)&&form&&cancel==NULL);++refresh_calls;
    *status=fixture_status;*length=0;body[0]=0;
    if(fixture_rc)return fixture_rc;
    const char *response=fixture_status==200?
        "{\"token_type\":\"Bearer\",\"access_token\":\"FAKE_ACCESS_30\",\"refresh_token\":\"FAKE_REFRESH_ROTATED_30\",\"expires_in\":3600}":
        "{\"error\":\"temporarily_unavailable\"}";
    assert(strlen(response)<capacity);strcpy(body,response);*length=strlen(response);return 0;
}
int x4_http_auth_reconnect_request(X4Http *http,bool passport,const char *form,char *body,size_t capacity,
    size_t *length,int *status,const _Atomic int *cancel,uint64_t deadline)
{
    assert(!passport&&deadline>clock_us);
    return x4_http_request(http,X4_AUTH_TOKEN_URL,form,body,capacity,length,status,cancel);
}
static X4Auth *fixture(void)
{
    owner=calloc(1,sizeof(*owner));assert(owner);atomic_flag_clear(&owner->lock);
    atomic_init(&owner->cancel,0);atomic_init(&owner->reload_requested,0);
    atomic_init(&owner->keyframe_requested,0);atomic_init(&owner->access_country,X4_ACCESS_AUTO);
    save=(struct X4SessionStore){.saved=true};owner->remember=&save;
    strcpy(owner->refresh_token,"FAKE_REFRESH_ORIGINAL_30");
    clock_us=1000000;cancel_at=0;fixture_status=200;fixture_rc=0;refresh_calls=0;return owner;
}
static void finish(X4PassportWork *p)
{x4_secure_clear(p,sizeof(*p));free(p);x4_secure_clear(owner,sizeof(*owner));free(owner);owner=NULL;}
int main(void)
{
    const int rejections[]={400,401};
    for(unsigned i=0;i<sizeof(rejections)/sizeof(rejections[0]);++i){
        X4Auth *a=fixture();X4PassportWork *p=calloc(1,sizeof(*p));assert(p);
        fixture_status=rejections[i];int status=0;const char *stage=NULL;Outcome out={0};
        int rc=restore_refresh(a,p,NULL,&status,&stage);restore_failure(a,&out,rc,status,stage);
        assert(!save.saved&&save.erases==1&&refresh_calls==1&&out.state==X4_AUTH_EXPIRED);finish(p);
    }
    const int temporary[]={408,429,503};
    for(unsigned i=0;i<sizeof(temporary)/sizeof(temporary[0]);++i){
        X4Auth *a=fixture();X4PassportWork *p=calloc(1,sizeof(*p));assert(p);
        fixture_status=temporary[i];int status=0;const char *stage=NULL;Outcome out={0};
        int rc=restore_refresh(a,p,NULL,&status,&stage);restore_failure(a,&out,rc,status,stage);
        assert(save.saved&&!save.erases&&refresh_calls==4&&clock_us==51000000&&out.state==X4_AUTH_ERROR);finish(p);
    }
    X4Auth *a=fixture();X4PassportWork *p=calloc(1,sizeof(*p));assert(p);
    fixture_status=0;fixture_rc=-7001;int status=0;const char *stage=NULL;Outcome out={0};
    int rc=restore_refresh(a,p,NULL,&status,&stage);restore_failure(a,&out,rc,status,stage);
    assert(save.saved&&!save.erases&&refresh_calls==4&&clock_us==51000000&&out.state==X4_AUTH_ERROR);finish(p);
    a=fixture();p=calloc(1,sizeof(*p));assert(p);fixture_status=0;fixture_rc=-7001;cancel_at=1500000;
    rc=restore_refresh(a,p,NULL,&status,&stage);assert(rc==X4_HTTP_CANCELLED&&refresh_calls==1&&clock_us<=1600000&&save.saved);finish(p);
    a=fixture();p=calloc(1,sizeof(*p));assert(p);
    assert(!restore_refresh(a,p,NULL,&status,&stage)&&refresh_calls==1&&save.rotations==1&&save.saved);finish(p);
    a=fixture();x4_auth_request_reload(a);assert(!atomic_load(&a->reload_requested));
    a->session.state=X4_SESSION_STREAMING;a->session.ever_connected=1;x4_auth_request_reload(a);assert(atomic_exchange(&a->reload_requested,0)==1);
    a->session.reconnecting=1;x4_auth_request_reload(a);assert(!atomic_load(&a->reload_requested));
    p=calloc(1,sizeof(*p));assert(p);finish(p);
    puts("PASS real RESTORE: only 400/401 erase; 408/429/503/network retain; three retries 5/15/30s; cancellable waits; rotation; manual reload gate");
    return 0;
}
