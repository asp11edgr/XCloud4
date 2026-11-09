/* SPDX-License-Identifier: GPL-3.0-only */
/* Deterministic link double for tls_conn.h. Requests are the actual bytes
 * emitted by production http_wire/http_client. No network, files, or logs. */
#pragma once
#include "../src/auth/tls_conn.h"
#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X4_TF_RESET ((int32_t)UINT32_C(0x80410136))
enum { X4_TF_REQUEST_MAX=1024*1024,X4_TF_HEADERS=16 };
typedef struct {
    unsigned connection_id;
    int method;
    const char *host,*target,*url,*body;
    size_t body_size,header_count;
    X4HttpHeader headers[X4_TF_HEADERS];
    bool keepalive;
} X4TfRequest;
typedef struct { int status;const char *body,*headers; } X4TfResponse;
typedef void (*X4TfRequestHook)(const X4TfRequest *,X4TfResponse *);
typedef void (*X4TfBudgetHook)(unsigned,const X4TlsBudget *);
typedef struct X4TfState {
    X4TfRequestHook on_request;
    X4TfBudgetHook on_budget;
    void (*on_context_close)(void),(*on_connection_close)(void);
    unsigned opens,closes,connections,connection_closes,writes,reads,requests;
    unsigned live_contexts,live_connections,verified_opens,reuses;
    bool fail_write,fail_read,fail_close;
    unsigned fail_read_after,read_fragment;
    _Atomic int *cancel_during_write;
    struct X4TlsHttp *contexts;
    struct X4TlsConn *retained;
} X4TfState;
static X4TfState x4tf;
struct X4TlsHttp { pthread_t owner;unsigned connections;X4TlsInfo info;struct X4TlsHttp *next; };
struct X4TlsConn {
    X4TlsHttp *owner;
    unsigned id,read_calls;
    bool reused,parsed,ready,retained;
    unsigned char *request,*response;
    size_t used,header_bytes,wanted,response_bytes,response_offset;
    X4TfRequest view;
    X4TlsInfo info;
    char url[1024];
    struct X4TlsConn *next;
};
uint64_t sceKernelGetProcessTime(void);
uint64_t x4_tls_now_us(void){return sceKernelGetProcessTime();}
static void x4tf_owner(const X4TlsHttp *h){assert(h&&pthread_equal(h->owner,pthread_self()));}
static int x4tf_budget(X4TlsConn *c,const X4TlsBudget *b,unsigned operation)
{
    if(x4tf.on_budget)x4tf.on_budget(operation,b);
    if(!b)return X4_HTTP_ARGUMENT;
    if(b->stage)atomic_store_explicit(b->stage,operation==0?"conexion HTTPS":
        operation==1?"envio HTTPS/TLS":"lectura de respuesta",memory_order_release);
    int result=0;
    if(b->cancel&&atomic_load(b->cancel))result=X4_HTTP_CANCELLED;
    else if(!b->deadline_us||x4_tls_now_us()>=b->deadline_us)result=X4_HTTP_DEADLINE;
    else if(b->phase_deadline_us&&x4_tls_now_us()>=b->phase_deadline_us)
        result=(int32_t)UINT32_C(0x8041013c);
    if(result&&c){c->info.error=result;c->owner->info=c->info;}
    return result;
}
static int x4tf_case_equal(const char *a,const char *b)
{
    while(*a&&*b){if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0;}
    return !*a&&!*b;
}
static const char *x4tf_header(const X4TfRequest *r,const char *name)
{
    for(size_t i=0;i<r->header_count;++i)if(x4tf_case_equal(r->headers[i].name,name))return r->headers[i].value;
    return NULL;
}
static unsigned x4tf_header_count(const X4TfRequest *r,const char *name)
{
    unsigned count=0;
    for(size_t i=0;i<r->header_count;++i)if(x4tf_case_equal(r->headers[i].name,name))++count;
    return count;
}
static void x4tf_clear_request(X4TlsConn *c)
{
    if(c->request){x4_secure_clear(c->request,X4_TF_REQUEST_MAX);free(c->request);}
    if(c->response){x4_secure_clear(c->response,c->response_bytes);free(c->response);}
    c->request=NULL;c->response=NULL;c->used=c->wanted=c->header_bytes=0;
    c->response_bytes=c->response_offset=0;c->parsed=c->ready=false;c->read_calls=0;
    c->view=(X4TfRequest){0};
}
static int x4tf_parse_request(X4TlsConn *c)
{
    if(c->parsed)return 0;
    char *raw=(char *)c->request,*end=strstr(raw,"\r\n\r\n");if(!end)return 0;
    c->header_bytes=(size_t)(end+4-raw);
    char *line_end=strstr(raw,"\r\n"),*first=strchr(raw,' ');
    assert(line_end&&first&&first<line_end);*first++=0;
    char *second=strchr(first,' ');assert(second&&second<line_end);*second++=0;
    assert(!strcmp(second,"HTTP/1.1\r\n")||!strncmp(second,"HTTP/1.1\r\n",10));
    c->view=(X4TfRequest){.connection_id=c->id,.host=c->info.host,.target=first};
    c->view.method=!strcmp(raw,"GET")?0:!strcmp(raw,"POST")?1:!strcmp(raw,"DELETE")?5:-1;
    assert(c->view.method>=0);
    char *line=line_end+2;
    while(line<end){
        line_end=strstr(line,"\r\n");assert(line_end&&line_end<=end);
        char *colon=memchr(line,':',(size_t)(line_end-line));assert(colon);
        *colon++=0;while(*colon==' '||*colon=='\t')++colon;
        *line_end=0;assert(c->view.header_count<X4_TF_HEADERS);
        c->view.headers[c->view.header_count++]=(X4HttpHeader){line,colon};line=line_end+2;
    }
    const char *host=x4tf_header(&c->view,"Host"),*length=x4tf_header(&c->view,"Content-Length");
    assert(host&&!strcmp(host,c->info.host));assert(!x4tf_header(&c->view,"Cookie"));
    assert(x4tf_header(&c->view,"Accept-Encoding")&&!strcmp(x4tf_header(&c->view,"Accept-Encoding"),"identity"));
    const char *connection=x4tf_header(&c->view,"Connection");assert(connection);
    c->view.keepalive=x4tf_case_equal(connection,"keep-alive");
    assert(c->view.keepalive||x4tf_case_equal(connection,"close"));
    size_t body=0;
    if(length){while(*length){assert(*length>='0'&&*length<='9');body=body*10+(unsigned)(*length++-'0');assert(body<X4_TF_REQUEST_MAX);}}
    if(c->view.method==1)assert(x4tf_header_count(&c->view,"Content-Length")==1);
    c->view.body=(char *)c->request+c->header_bytes;c->view.body_size=body;
    int written=snprintf(c->url,sizeof(c->url),"https://%s%s",c->info.host,c->view.target);
    assert(written>0&&(size_t)written<sizeof(c->url));c->view.url=c->url;
    c->wanted=c->header_bytes+body;assert(c->wanted<X4_TF_REQUEST_MAX);c->parsed=true;
    return 0;
}
static void x4tf_reply(X4TlsConn *c)
{
    X4TfResponse response={.status=200,.body="{}",.headers=""};
    ++x4tf.requests;if(x4tf.on_request)x4tf.on_request(&c->view,&response);
    assert(response.status>=100&&response.status<=599&&response.body);
    const char *headers=response.headers?response.headers:"";
    /* Historical fixtures expose a complete header block. Drop only their
     * status line; actual headers/trailers still reach production http_wire. */
    if(!strncmp(headers,"HTTP/",5)){const char *next=strstr(headers,"\r\n");assert(next);headers=next+2;}
    size_t header_length=strlen(headers);
    while(header_length>=2&&!memcmp(headers+header_length-2,"\r\n",2))header_length-=2;
    size_t body=strlen(response.body),capacity=header_length+body+160;
    c->response=malloc(capacity);assert(c->response);
    int n=snprintf((char *)c->response,capacity,"HTTP/1.1 %d Fake\r\nContent-Length: %zu\r\n%.*s%s\r\n",
        response.status,body,(int)header_length,headers,header_length?"\r\n":"");
    assert(n>0&&(size_t)n+body<capacity);memcpy(c->response+n,response.body,body);
    c->response_bytes=(size_t)n+body;c->ready=true;
}
X4TlsHttp *x4_tls_http_open(int *error)
{
    X4TlsHttp *h=calloc(1,sizeof(*h));assert(h);h->owner=pthread_self();h->info.stage="HTTPS listo";
    h->next=x4tf.contexts;x4tf.contexts=h;++x4tf.opens;++x4tf.live_contexts;++x4tf.verified_opens;
    if(error)*error=0;return h;
}
int x4_tls_http_enable_update_ca(X4TlsHttp *h)
{
    x4tf_owner(h);
    return X4_HTTP_UNUSABLE;
}
int x4_tls_http_close(X4TlsHttp *h)
{
    if(!h)return 0;x4tf_owner(h);if(h->connections)return X4_HTTP_BUSY;
    if(x4tf.on_context_close)x4tf.on_context_close();
    X4TlsHttp **p=&x4tf.contexts;while(*p&&*p!=h)p=&(*p)->next;assert(*p==h);*p=h->next;
    ++x4tf.closes;--x4tf.live_contexts;free(h);return 0;
}
int x4_tls_conn_connect(X4TlsHttp *h,const char *host,const X4TlsBudget *budget,X4TlsConn **out)
{
    x4tf_owner(h);assert(host&&out);*out=NULL;
    int result=x4tf_budget(NULL,budget,0);if(result)return result;
    X4TlsConn *c=calloc(1,sizeof(*c));assert(c);c->owner=h;c->id=++x4tf.connections;
    c->info=(X4TlsInfo){.stage="HTTPS listo",.verified=true,.handshake_us=1000};
    snprintf(c->info.host,sizeof(c->info.host),"%s",host);snprintf(c->info.version,sizeof(c->info.version),"TLSv1.3");
    snprintf(c->info.suite,sizeof(c->info.suite),"TLS1-3-AES-128-GCM-SHA256");
    ++h->connections;++x4tf.live_connections;h->info=c->info;*out=c;return 0;
}
int x4_tls_conn_write(X4TlsConn *c,const void *data,size_t size,const X4TlsBudget *budget)
{
    assert(c&&data);x4tf_owner(c->owner);++x4tf.writes;
    c->info.stage="envio HTTPS/TLS";
    int result=x4tf_budget(c,budget,1);if(result)return result;
    if(x4tf.cancel_during_write)atomic_store(x4tf.cancel_during_write,1);
    if(x4tf.fail_write){x4tf.fail_write=false;c->info.error=X4_TF_RESET;c->owner->info=c->info;return X4_TF_RESET;}
    if(c->ready){assert(c->response_offset==c->response_bytes);x4tf_clear_request(c);}
    if(!c->request){c->request=calloc(1,X4_TF_REQUEST_MAX);assert(c->request);}
    assert(c->used+size<X4_TF_REQUEST_MAX);memcpy(c->request+c->used,data,size);c->used+=size;c->request[c->used]=0;
    result=x4tf_parse_request(c);if(result)return result;
    if(c->parsed&&c->used==c->wanted)x4tf_reply(c);
    else if(c->parsed)assert(c->used<c->wanted);
    return 0;
}
int x4_tls_conn_read(X4TlsConn *c,void *data,size_t size,size_t *got,const X4TlsBudget *budget)
{
    assert(c&&data&&got&&size);x4tf_owner(c->owner);*got=0;++x4tf.reads;
    c->info.stage="lectura de respuesta";
    int result=x4tf_budget(c,budget,2);if(result)return result;
    if(x4tf.fail_read||(x4tf.fail_read_after&&c->read_calls>=x4tf.fail_read_after)){
        x4tf.fail_read=false;x4tf.fail_read_after=0;c->info.error=X4_TF_RESET;c->owner->info=c->info;return X4_TF_RESET;
    }
    assert(c->ready);size_t left=c->response_bytes-c->response_offset,take=left<size?left:size;
    if(x4tf.read_fragment&&take>x4tf.read_fragment)take=x4tf.read_fragment;
    memcpy(data,c->response+c->response_offset,take);c->response_offset+=take;++c->read_calls;*got=take;return 0;
}
int x4_tls_conn_close(X4TlsConn *c)
{
    if(!c)return 0;x4tf_owner(c->owner);
    if(x4tf.fail_close){
        if(!c->retained){c->next=x4tf.retained;x4tf.retained=c;c->retained=true;}
        return X4_TF_RESET;
    }
    if(x4tf.on_connection_close)x4tf.on_connection_close();
    x4tf_clear_request(c);--c->owner->connections;--x4tf.live_connections;++x4tf.connection_closes;free(c);return 0;
}
bool x4_tls_conn_idle_clean(const X4TlsConn *c)
{assert(c);x4tf_owner(c->owner);return true;}
void x4_tls_count_reopen(void){}
void x4_tls_conn_mark_reused(X4TlsConn *c)
{assert(c);x4tf_owner(c->owner);c->info.reused=c->reused=true;c->owner->info=c->info;++x4tf.reuses;}
X4TlsInfo x4_tls_http_info(const X4TlsHttp *h)
{x4tf_owner(h);return h->info;}
X4TlsInfo x4_tls_conn_info(const X4TlsConn *c)
{assert(c);x4tf_owner(c->owner);return c->info;}
/* Fixture-only reclamation after production refuses an unsafe close. No
 * real sockets or native references exist in this link double. */
static inline void x4tf_reclaim_retained(void)
{
    x4tf.fail_close=false;
    while(x4tf.retained){X4TlsConn *c=x4tf.retained;x4tf.retained=c->next;c->retained=false;(void)x4_tls_conn_close(c);}
    while(x4tf.contexts){assert(!x4tf.contexts->connections);(void)x4_tls_http_close(x4tf.contexts);}
}
static void x4tf_reset(void)
{
    assert(!x4tf.live_connections&&!x4tf.live_contexts&&!x4tf.contexts&&!x4tf.retained);
    x4tf=(X4TfState){0};
}
