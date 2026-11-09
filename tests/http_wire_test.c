/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/auth/http_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"wire check %s:%d: %s\n",__FILE__,__LINE__,#x);abort();}}while(0)
static int fragmented(X4HttpWire *p,const void *data,size_t n,size_t step,size_t *total)
{
    const unsigned char *bytes=data;size_t at=0;int rc=0;
    while(at<n){size_t take=n-at<step?n-at:step,used=0;
        rc=x4_http_wire_feed(p,bytes+at,take,&used);CHECK(used<=take);at+=used;
        if(rc||used!=take)break;
    }
    if(total)*total=at;return rc;
}
static void success(const char *response,const char *expected,int status,bool keep,bool needs_eof)
{
    size_t length=strlen(response);
    for(size_t step=1;step<=length+1;++step){
        unsigned char guarded[260];memset(guarded,0xd3,sizeof(guarded));X4HttpWire p;
        x4_http_wire_init(&p,guarded+2,256,784111767);
        size_t used=0;int rc=fragmented(&p,response,length,step,&used);
        CHECK(used==length);
        if(needs_eof){CHECK(rc==X4_HTTP_WIRE_MORE&&!x4_http_wire_result(&p).complete);rc=x4_http_wire_eof(&p);}
        CHECK(rc==X4_HTTP_WIRE_DONE);X4HttpWireResult r=x4_http_wire_result(&p);
        CHECK(r.status==status&&r.complete&&r.response_started&&r.reusable==keep);
        CHECK(r.body_bytes==strlen(expected)&&!memcmp(guarded+2,expected,strlen(expected)+1));
        CHECK(guarded[0]==0xd3&&guarded[1]==0xd3&&guarded[258]==0xd3&&guarded[259]==0xd3);
        size_t probe=99;CHECK(x4_http_wire_feed(&p,NULL,0,&probe)==X4_HTTP_WIRE_DONE&&!probe);
    }
    /* Every possible two-fragment cut, including a split between CR and LF. */
    for(size_t cut=0;cut<=length;++cut){
        char body[256];X4HttpWire p;x4_http_wire_init(&p,body,sizeof(body),0);
        size_t first=0,second=0;int rc=x4_http_wire_feed(&p,response,cut,&first);CHECK(first==cut);
        if(!rc)rc=x4_http_wire_feed(&p,response+cut,length-cut,&second);
        CHECK(first+second==length);if(needs_eof)rc=x4_http_wire_eof(&p);
        CHECK(rc==1&&!strcmp(body,expected));
    }
}
static void framing(void)
{
    success("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello","hello",200,true,false);
    success("HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n","",200,true,false);
    success("HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\n{}","{}",200,true,false);
    success("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: anything, CLOSE, keep-alive\r\n\r\n{}","{}",200,false,false);
    success("HTTP/1.0 200 OK\r\n\r\nclosed","closed",200,false,true);
    success("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nclosed","closed",200,false,true);
    success("HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\nContent-Length: 005\r\nContent-Encoding: identity\r\n\r\nhello","hello",200,true,false);
    success("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n2; x=\"quoted;\\\" value\"\r\nhe\r\n3;flag; a=b\r\nllo\r\n0;done\r\nX-Trailer: ignored\r\nSet-Cookie: ignored\r\n\r\n","hello",200,true,false);
    success("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nConnection: keep-alive,\r\nRetry-After: 60\r\n\r\n","",200,false,false);
    success("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: ignored\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}","{}",200,true,false);
    success("HTTP/1.1 204 No Content\r\n\r\n","",204,true,false);
    success("HTTP/1.1 304 Not Modified\r\nContent-Length: 123456\r\n\r\n","",304,true,false);
    success("HTTP/1.1 302 Found\r\nLocation: https://example.invalid/elsewhere\r\nSet-Cookie: ignored\r\nContent-Length: 2\r\n\r\n{}","{}",302,true,false);
    /* Binary responses preserve embedded NULs and have a separate terminator. */
    static const unsigned char binary[]="HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b";
    unsigned char body[4];X4HttpWire p;x4_http_wire_init(&p,body,sizeof(body),0);
    CHECK(fragmented(&p,binary,sizeof(binary)-1,1,NULL)==1);
    CHECK(!memcmp(body,"a\0b\0",4)&&x4_http_wire_result(&p).body_bytes==3);
    /* No pipelining: bytes after this exact entity remain unconsumed. */
    const char *extra="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}EXTRA";
    char out[16];x4_http_wire_init(&p,out,sizeof(out),0);size_t used=0;
    CHECK(x4_http_wire_feed(&p,extra,strlen(extra),&used)==1&&used==strlen(extra)-5);
}
static void rejection(const char *response,int expected,bool premature_eof)
{
    for(size_t step=1;step<8;++step){
        unsigned char body[64];memset(body,0x7b,sizeof(body));X4HttpWire p;
        x4_http_wire_init(&p,body,sizeof(body),0);
        int rc=fragmented(&p,response,strlen(response),step,NULL);
        if(!rc&&premature_eof)rc=x4_http_wire_eof(&p);
        CHECK(rc==expected);X4HttpWireResult r=x4_http_wire_result(&p);
        CHECK(!r.complete&&!r.reusable&&!r.body_bytes&&!r.status);
        for(size_t i=0;i<sizeof(body);++i)CHECK(!body[i]);
    }
}
static void invalid(void)
{
    static const char *const bad[]={
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2, 3\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: +2\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2 2\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 2,\r\n\r\n",
        "HTTP/1.1 200 OK\r\n Folded: no\r\n\r\n",
        "HTTP/1.1 200 OK\r\nBad Header: no\r\n\r\n",
        "HTTP/1.1 200 OK\n\n",
        "HTTP/1.2 200 OK\r\n\r\n",
        "HTTP/1.1 099 Nope\r\n\r\n",
        "HTTP/1.1 600 Nope\r\n\r\n",
        "HTTP/1.1 200x\r\n\r\n",
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n+1\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1z\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;x=\"unterminated\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 0\r\n\r\n"
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i)rejection(bad[i],X4_HTTP_PROTOCOL,false);
    rejection("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhell",X4_HTTP_PROTOCOL,true);
    rejection("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nh",X4_HTTP_PROTOCOL,true);
    rejection("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: y\r\n",X4_HTTP_PROTOCOL,true);
    rejection("HTTP/1.1 200 OK\r\nX: no ending",X4_HTTP_PROTOCOL,true);
    unsigned char zero[]="HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n";char body[64];X4HttpWire p;
    x4_http_wire_init(&p,body,sizeof(body),0);CHECK(fragmented(&p,zero,sizeof(zero)-1,1,NULL)==X4_HTTP_PROTOCOL);
}
static void bounds(void)
{
    char *response=malloc(X4_HTTP_WIRE_HEADERS_MAX+2);CHECK(response);
    const char *prefix="HTTP/1.1 200 OK\r\nX: ";size_t n=strlen(prefix);
    memcpy(response,prefix,n);memset(response+n,'x',X4_HTTP_WIRE_HEADERS_MAX-n-4);
    memcpy(response+X4_HTTP_WIRE_HEADERS_MAX-4,"\r\n\r\n",4);
    response[X4_HTTP_WIRE_HEADERS_MAX]=0;
    char body[8];X4HttpWire p;x4_http_wire_init(&p,body,sizeof(body),0);
    CHECK(fragmented(&p,response,X4_HTTP_WIRE_HEADERS_MAX,3,NULL)==0&&x4_http_wire_eof(&p)==1);
    memmove(response+X4_HTTP_WIRE_HEADERS_MAX-3,response+X4_HTTP_WIRE_HEADERS_MAX-4,4);
    response[X4_HTTP_WIRE_HEADERS_MAX-4]='x';response[X4_HTTP_WIRE_HEADERS_MAX+1]=0;
    rejection(response,X4_HTTP_BOUND,false);free(response);
    char lines[2048];size_t at=(size_t)snprintf(lines,sizeof(lines),"HTTP/1.1 200 OK\r\n");
    for(unsigned i=0;i<98;++i){memcpy(lines+at,"X: y\r\n",6);at+=6;}
    memcpy(lines+at,"\r\n",3);at+=2;
    x4_http_wire_init(&p,body,sizeof(body),0);CHECK(fragmented(&p,lines,at,1,NULL)==0&&p.header_lines==100&&x4_http_wire_eof(&p)==1);
    memmove(lines+at-2+6,lines+at-2,3);memcpy(lines+at-2,"X: y\r\n",6);rejection(lines,X4_HTTP_BOUND,false);
    /* Trailer and interim budgets share the final response's limits. */
    const char *interim="HTTP/1.1 100 Continue\r\n\r\n";
    char flood[4096];at=0;for(unsigned i=0;i<51;++i){memcpy(flood+at,interim,strlen(interim));at+=strlen(interim);}flood[at]=0;
    rejection(flood,X4_HTTP_BOUND,false);
    /* Full output is not EOF until framing (or actual close) proves it. */
    const char *length="HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    char exact[4];x4_http_wire_init(&p,exact,sizeof(exact),0);
    CHECK(fragmented(&p,length,strlen(length),1,NULL)==1&&!strcmp(exact,"abc"));
    size_t probe=9;CHECK(x4_http_wire_feed(&p,NULL,0,&probe)==1&&!probe);
    length="HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabcd";
    x4_http_wire_init(&p,exact,sizeof(exact),0);
    CHECK(fragmented(&p,length,strlen(length),1,NULL)==X4_HTTP_BOUND&&!exact[0]);
    const char *close="HTTP/1.1 200 OK\r\n\r\nabc";
    x4_http_wire_init(&p,exact,sizeof(exact),0);CHECK(fragmented(&p,close,strlen(close),1,NULL)==0);
    CHECK(!x4_http_wire_result(&p).complete&&x4_http_wire_eof(&p)==1&&!strcmp(exact,"abc"));
    x4_http_wire_init(&p,exact,sizeof(exact),0);CHECK(fragmented(&p,close,strlen(close),1,NULL)==0);
    size_t used=0;CHECK(x4_http_wire_feed(&p,"d",1,&used)==X4_HTTP_BOUND&&used==1&&!exact[0]);
    const char *chunk="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n";
    x4_http_wire_init(&p,exact,sizeof(exact),0);CHECK(fragmented(&p,chunk,strlen(chunk),1,NULL)==0);
    CHECK(!x4_http_wire_result(&p).complete);
    CHECK(x4_http_wire_feed(&p,"0\r\n\r\n",5,&used)==1&&used==5&&!strcmp(exact,"abc"));
    x4_http_wire_init(&p,exact,sizeof(exact),0);CHECK(fragmented(&p,chunk,strlen(chunk),1,NULL)==0);
    CHECK(x4_http_wire_feed(&p,"1\r\nd",4,&used)==X4_HTTP_BOUND);
    char empty[1];x4_http_wire_init(&p,empty,1,0);
    CHECK(fragmented(&p,"HTTP/1.1 204 OK\r\n\r\n",19,1,NULL)==1&&!empty[0]);
    x4_http_wire_init(&p,empty,1,0);CHECK(fragmented(&p,"HTTP/1.1 200 OK\r\n\r\nx",20,1,NULL)==X4_HTTP_BOUND);
}
static void retry_after(void)
{
    CHECK(x4_http_wire_retry_after("15",2,0)==15000);
    CHECK(!x4_http_wire_retry_after("-1",2,0)&&!x4_http_wire_retry_after("1.5",3,0));
    CHECK(x4_http_wire_retry_after("999999999999999999999",21,0)==UINT64_MAX);
    CHECK(x4_http_wire_retry_after("Sun, 06 Nov 1994 08:49:37 GMT",29,784111767)==10000);
    CHECK(!x4_http_wire_retry_after("Sun, 06 Nov 1994 08:49:37 GMT",29,784111778));
    CHECK(x4_http_wire_retry_after("Tue, 29 Feb 2000 00:00:00 GMT",29,951782390)==10000);
    CHECK(!x4_http_wire_retry_after("Tue, 29 Feb 2001 00:00:00 GMT",29,0));
    char body[4];X4HttpWire p;const char *response="HTTP/1.1 429 Too Many\r\nRetry-After:\t5 \r\nContent-Length: 0\r\n\r\n";
    x4_http_wire_init(&p,body,sizeof(body),0);CHECK(fragmented(&p,response,strlen(response),1,NULL)==1&&x4_http_wire_result(&p).retry_after_ms==5000);
    response="HTTP/1.1 429 Wait\r\nRetry-After: 5\r\nRetry-After: 6\r\nContent-Length: 0\r\n\r\n";
    x4_http_wire_init(&p,body,sizeof(body),0);CHECK(fragmented(&p,response,strlen(response),1,NULL)==1&&!x4_http_wire_result(&p).retry_after_ms);
    response="HTTP/1.1 103 Early\r\nRetry-After: 5\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    x4_http_wire_init(&p,body,sizeof(body),0);CHECK(fragmented(&p,response,strlen(response),1,NULL)==1&&!x4_http_wire_result(&p).retry_after_ms);
}
typedef struct {char bytes[4096];size_t size;int failure;unsigned calls;} Capture;
static int sink(void *context,const void *bytes,size_t size)
{
    Capture *c=context;if(c->failure&&++c->calls==3)return c->failure;
    CHECK(size<sizeof(c->bytes)-c->size);memcpy(c->bytes+c->size,bytes,size);c->size+=size;c->bytes[c->size]=0;return 0;
}
static void requests(void)
{
    X4HttpHeader headers[]={{"Authorization","Bearer fake-test-only"},{"X-MS-Device-Info","{}"}};
    X4HttpWireRequest r={.method="POST",.host="example.invalid",.target="/test",.accept="application/json",
        .content_type="application/json",.headers=headers,.header_count=2,.payload_size=2};
    Capture c={0};CHECK(!x4_http_wire_write_request(&r,sink,&c));
    CHECK(!strcmp(c.bytes,"POST /test HTTP/1.1\r\nHost: example.invalid\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nUser-Agent: XCloud4\r\nContent-Type: application/json\r\nAuthorization: Bearer fake-test-only\r\nX-MS-Device-Info: {}\r\nContent-Length: 2\r\nConnection: close\r\n\r\n"));
    CHECK(!strstr(c.bytes,"Cookie")&&c.bytes[c.size-1]=='\n'); /* no payload serialized */
    r.payload_size=0;c=(Capture){0};CHECK(!x4_http_wire_write_request(&r,sink,&c)&&strstr(c.bytes,"Content-Length: 0\r\n"));
    r=(X4HttpWireRequest){.method="GET",.host="example.invalid",.target="/cover?a=b",.accept="image/*",.keepalive=true};
    c=(Capture){0};CHECK(!x4_http_wire_write_request(&r,sink,&c));
    CHECK(!strcmp(c.bytes,"GET /cover?a=b HTTP/1.1\r\nHost: example.invalid\r\nAccept: image/*\r\nAccept-Encoding: identity\r\nUser-Agent: XCloud4\r\nConnection: keep-alive\r\n\r\n"));
    CHECK(!strstr(c.bytes,"Authorization")&&!strstr(c.bytes,"Cookie")&&!strstr(c.bytes,"Content-Length"));
    r.method="DELETE";r.keepalive=false;r.user_agent="XCloud4/test";c=(Capture){0};CHECK(!x4_http_wire_write_request(&r,sink,&c));
    CHECK(strstr(c.bytes,"DELETE /cover?a=b HTTP/1.1\r\n")&&strstr(c.bytes,"User-Agent: XCloud4/test\r\n"));
    static const char *const reserved[]={"Cookie","HOST","Content-Length","Transfer-Encoding","Connection","Accept-Encoding"};
    r.headers=headers;r.header_count=1;
    for(size_t i=0;i<sizeof(reserved)/sizeof(*reserved);++i){headers[0].name=reserved[i];c=(Capture){0};
        CHECK(x4_http_wire_write_request(&r,sink,&c)==X4_HTTP_ARGUMENT&&!c.size);}
    headers[0]=(X4HttpHeader){"X-Test","bad\r\nInjected: yes"};c=(Capture){0};CHECK(x4_http_wire_write_request(&r,sink,&c)==X4_HTTP_ARGUMENT);
    r.header_count=0;r.target="/bad target";CHECK(x4_http_wire_write_request(&r,sink,&c)==X4_HTTP_ARGUMENT);
    r.target="/good";r.host="bad:443";CHECK(x4_http_wire_write_request(&r,sink,&c)==X4_HTTP_ARGUMENT);
    r.host="example.invalid";c=(Capture){.failure=-765};CHECK(x4_http_wire_write_request(&r,sink,&c)==-765);
    c=(Capture){.failure=7};CHECK(x4_http_wire_write_request(&r,sink,&c)==X4_HTTP_PROTOCOL);
}
int main(void)
{
    CHECK(sizeof(X4HttpWire)<=32768);framing();invalid();bounds();retry_after();requests();
    printf("PASS http_wire: %u checks; state=%zu bytes; CL/chunked/EOF, bounds, Retry-After and exact requests\n",checks,sizeof(X4HttpWire));
    return 0;
}
