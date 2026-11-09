/* SPDX-License-Identifier: GPL-3.0-only */
/* Complete production HTTP policy/wire with a deterministic TLS link double. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *policy_malloc(size_t);
static void *policy_calloc(size_t,size_t);
static void policy_free(void *);
#define malloc policy_malloc
#define calloc policy_calloc
#define free policy_free
#include "../src/auth/http_client.c"
#undef free
#undef calloc
#undef malloc
#include "tls_transport_fake.h"
static void *policy_allocations[128];
static size_t policy_allocation_count;
static void *remember_allocation(void *p)
{if(p){assert(policy_allocation_count<128);policy_allocations[policy_allocation_count++]=p;}return p;}
static void *policy_malloc(size_t bytes){return remember_allocation(malloc(bytes));}
static void *policy_calloc(size_t count,size_t bytes){return remember_allocation(calloc(count,bytes));}
static void policy_free(void *p)
{
 if(!p)return;
 for(size_t i=0;i<policy_allocation_count;++i)if(policy_allocations[i]==p){
  policy_allocations[i]=policy_allocations[--policy_allocation_count];free(p);return;
 }
 assert(!"free outside the production-owner allocation registry");
}
static void reclaim_fake_quarantine(void)
{
 /* Only after the fake kernel proves no actual socket/reference remains.
  * The real product intentionally retains these owners until process exit. */
 x4tf_reclaim_retained();
 while(policy_allocation_count)policy_free(policy_allocations[policy_allocation_count-1]);
}
static unsigned checks,public_connections,private_connections;
static bool seen_connection[2048],exercise_reentry;
static X4Http *reentry_http;
static int response_status=200;
static const char *response_body="{}",*response_headers="HTTP/1.1 200 OK\r\n\r\n";
static uint64_t fake_clock;
#define CHECK(x) do { ++checks;if(!(x)){fprintf(stderr,"HTTP check failed line=%d\n",__LINE__);abort();} }while(0)
uint64_t sceKernelGetProcessTime(void){return fake_clock;}
int sceKernelUsleep(unsigned us){(void)us;return 0;}
const char *x4_lang_store_suffix(void){return "&market=MX&languages=es-MX&fieldsTemplate=Details";}
static const char *market = "MX", *locale = "es-MX";
const char *x4_lang_market(void){return market;}
const char *x4_lang_locale(void){return locale;}
static const char *store="https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details";
static const char *image="https://store-images.s-microsoft.com/image/apps.test?w=356&h=200&q=85&format=jpg";
static void reply(const X4TfRequest *request,X4TfResponse *response)
{
 CHECK(request->connection_id<2048);
 if(!seen_connection[request->connection_id]){
  seen_connection[request->connection_id]=true;
  if(request->keepalive)++public_connections;else ++private_connections;
 }
 CHECK(!x4tf_header(request,"Cookie"));
 CHECK(x4tf_header_count(request,"Host")==1&&x4tf_header_count(request,"Accept-Encoding")==1);
 if (!strncmp(request->url, X4_SIGL_URL_PREFIX, sizeof(X4_SIGL_URL_PREFIX)-1)) {
  CHECK(!request->keepalive && request->method==0 && !request->body_size);
  CHECK(!x4tf_header(request,"Authorization") && !x4tf_header(request,"X-MS-Device-Info"));
 }
 if(request->keepalive){
  CHECK(request->method==0&&!request->body_size);
  CHECK(!strncmp(request->url,store_prefix,sizeof(store_prefix)-1)||x4_http_public_image_url(request->url));
  CHECK(!x4tf_header(request,"Authorization")&&!x4tf_header(request,"X-MS-Device-Info"));
  for(size_t i=0;i<request->header_count;++i){
   const char *name=request->headers[i].name;
   CHECK(!strcmp(name,"Host")||!strcmp(name,"Connection")||!strcmp(name,"Accept")||!strcmp(name,"Accept-Encoding")||!strcmp(name,"User-Agent"));
   CHECK(!strstr(request->headers[i].value,"Bearer")&&!strstr(request->headers[i].value,"credential"));
  }
 }
 CHECK(x4tf_header(request,"User-Agent")&&!strcmp(x4tf_header(request,"User-Agent"),"XCloud4"));
 response->status=response_status;response->body=response_body;response->headers=response_headers;
 if(exercise_reentry){
  exercise_reentry=false;char body[64];size_t length=0;int status=0;unsigned before=x4tf.connections;
  CHECK(x4_http_public_reset(reentry_http)==X4_HTTP_BUSY);
  CHECK(x4_http_public_metadata_request(reentry_http,store,body,sizeof(body),&length,&status,NULL)==X4_HTTP_BUSY);
  CHECK(x4tf.connections==before);
 }
}
static X4Http *open_context(void)
{
 CHECK(!policy_allocation_count);
 x4tf_reset();x4tf.on_request=reply;memset(seen_connection,0,sizeof(seen_connection));
 public_connections=private_connections=0;int error=-1;X4Http *h=x4_http_open(&error);
 CHECK(h&&!error&&x4tf.verified_opens==1);return h;
}
static void public_exchange(X4Http *h,bool cover)
{
 char body[64];size_t length=99;int status=99;
 int result=cover?x4_http_public_image_request(h,image,(unsigned char *)body,sizeof(body),&length,&status,NULL):
  x4_http_public_metadata_request(h,store,body,sizeof(body),&length,&status,NULL);
 CHECK(!result&&length==strlen(response_body)&&status==response_status&&!strcmp(body,response_body));
}
static void test_reuse_isolation_cleanup(void)
{
 X4Http *h=open_context();unsigned before=x4tf.connections;
 for(unsigned i=0;i<6;++i){public_exchange(h,false);public_exchange(h,true);}
 CHECK(x4tf.connections-before==2&&public_connections==2&&!private_connections&&x4tf.live_connections==2);
 char body[64];size_t length;int status;before=x4tf.connections;
 CHECK(!x4_http_request(h,"https://login.microsoftonline.com/consumers/oauth2","test=credential",body,sizeof(body),&length,&status,NULL));
 CHECK(x4tf.connections==before+1&&private_connections==1&&x4tf.live_connections==2);
 CHECK(!x4_http_public_reset(h)&&!x4tf.live_connections);
 CHECK(x4tf.connection_closes==3);public_exchange(h,false);x4_http_close(h);
 CHECK(!x4tf.live_connections&&!x4tf.live_contexts);
}
static void test_retry_and_cancellation(void)
{
 X4Http *h=open_context();public_exchange(h,false);unsigned before=x4tf.writes;
 x4tf.fail_write=true;public_exchange(h,false);CHECK(x4tf.writes-before==2);
 before=x4tf.requests;x4tf.fail_read=true;public_exchange(h,false);CHECK(x4tf.requests-before==2);
 CHECK(!x4_http_public_reset(h));char body[64];size_t length=0;int status=0;
 x4tf.fail_write=true;before=x4tf.writes;
 CHECK(x4_http_public_metadata_request(h,store,body,sizeof(body),&length,&status,NULL)==X4_TF_RESET);
 CHECK(x4tf.writes-before==1&&!body[0]&&!length&&!status);
 public_exchange(h,false);
 /* A byte of HTTP status is enough to forbid replay; it need not be body. */
 x4tf.read_fragment=1;x4tf.fail_read_after=1;before=x4tf.requests;
 CHECK(x4_http_public_metadata_request(h,store,body,sizeof(body),&length,&status,NULL)==X4_TF_RESET);
 CHECK(x4tf.requests-before==1&&!body[0]&&!length&&!status);
 x4tf.read_fragment=0;public_exchange(h,false);
 _Atomic int cancel=0;x4tf.cancel_during_write=&cancel;before=x4tf.writes;
 CHECK(x4_http_public_metadata_request(h,store,body,sizeof(body),&length,&status,&cancel)==X4_HTTP_CANCELLED);
 CHECK(x4tf.writes-before==1&&!length&&!status&&!body[0]&&!x4tf.live_connections);
 x4tf.cancel_during_write=NULL;public_exchange(h,false);
 response_body="an oversized response";before=x4tf.requests;
 CHECK(x4_http_public_metadata_request(h,store,body,4,&length,&status,NULL)==X4_HTTP_BOUND);
 CHECK(x4tf.requests-before==1&&!length&&!status&&!body[0]);response_body="{}";
 /* A POST is never replayed, irrespective of a prior public connection. */
 x4tf.fail_write=true;before=x4tf.writes;
 CHECK(x4_http_request(h,"https://login.microsoftonline.com/consumers/oauth2","test=credential",body,sizeof(body),&length,&status,NULL)==X4_TF_RESET);
 CHECK(x4tf.writes-before==1&&!length&&!status&&!body[0]);x4_http_close(h);
 CHECK(!x4tf.live_connections&&!x4tf.live_contexts);
}
static void test_metadata_bound_closes(void)
{
 const size_t cap=1024u*1024u+1u;
 char *oversized=malloc(cap+1),*body=malloc(cap);
 CHECK(oversized&&body);
 memset(oversized,'a',cap);oversized[cap]=0;
 X4Http *h=open_context();size_t length=9;int status=9;
 response_body=oversized;
 CHECK(x4_http_public_metadata_request(h,store,body,cap,&length,&status,NULL)==X4_HTTP_BOUND);
 CHECK(!length&&!status&&!body[0]&&!x4tf.live_connections);
 unsigned connected=x4tf.connections;
 response_body="{}";public_exchange(h,false);
 CHECK(x4tf.connections==connected+1);
 x4_http_close(h);free(body);free(oversized);
 CHECK(!x4tf.live_connections&&!x4tf.live_contexts);
}

static void test_policy_and_owner(void)
{
 X4Http *h=open_context();char body[64];size_t length;int status;unsigned before=x4tf.connections;
 const char *bad[]={"http://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details",
  "https://displaycatalog.mp.microsoft.com:443/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details",
  "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1&market=MX&languages=es-MX&fieldsTemplate=Details&token=x",
  "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=ID1,,ID2&market=MX&languages=es-MX&fieldsTemplate=Details",
  "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=I1,I2,I3,I4,I5,I6,I7,I8,I9&market=MX&languages=es-MX&fieldsTemplate=Details",
  "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=../bad&market=MX&languages=es-MX&fieldsTemplate=Details",
  "https://region.gssv-play-prod.xboxlive.com/v2/titles","https://login.live.com/oauth20_token.srf"};
 for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i)CHECK(x4_http_public_metadata_request(h,bad[i],body,sizeof(body),&length,&status,NULL)==X4_HTTP_URL);
 CHECK(x4tf.connections==before);
 const char *batch="https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=I1,I2,I3,I4,I5,I6,I7,I8&market=MX&languages=es-MX&fieldsTemplate=Details";
 CHECK(!x4_http_public_metadata_request(h,batch,body,sizeof(body),&length,&status,NULL));
 CHECK(x4_http_public_image_request(h,store,(unsigned char *)body,sizeof(body),&length,&status,NULL)==X4_HTTP_URL);
 reentry_http=h;exercise_reentry=true;public_exchange(h,false);CHECK(!exercise_reentry);
 response_status=302;before=x4tf.requests;public_exchange(h,false);
 CHECK(x4tf.requests==before+1);response_status=200;x4_http_close(h);
}
static uint64_t retry_headers(const char *headers,int64_t now)
{
 char wire[1024],body[8];X4HttpWire parser;
 int count=snprintf(wire,sizeof(wire),"HTTP/1.1 429 Fake\r\nContent-Length: 0\r\n%s\r\n",headers);
 CHECK(count>0&&(size_t)count<sizeof(wire));x4_http_wire_init(&parser,body,sizeof(body),now);
 size_t used=0;CHECK(x4_http_wire_feed(&parser,wire,(size_t)count,&used)==X4_HTTP_WIRE_DONE);
 CHECK(used==(size_t)count);return x4_http_wire_result(&parser).retry_after_ms;
}

static void test_sigl_policy(void)
{
 X4Http *h = open_context();
 char body[64];
 size_t length = 0;
 int status = 0;
 const char *const allowed[] = {
  X4_SIGL_URL_PREFIX "d8f4afcd-882a-49e3-86b3-f61fa0172b75&market=MX&language=es-MX",
  X4_SIGL_URL_PREFIX "06323672-b8c8-43cc-b0de-32d5a9834749&market=MX&language=es-MX",
  X4_SIGL_URL_PREFIX "6a589fa0-d493-472b-8e20-3813699d7056&market=MX&language=es-MX",
  X4_SIGL_URL_PREFIX "31ff2361-2772-4622-849b-f4f1abb4ad1b&market=MX&language=es-MX",
 };
 public_exchange(h, false);
 unsigned before = x4tf.requests;
 for (size_t i=0; i<sizeof(allowed)/sizeof(*allowed); ++i) {
  CHECK(!x4_http_public_metadata_request(h, allowed[i], body, sizeof(body), &length, &status, NULL));
  CHECK(status==200 && length==2 && !strcmp(body, "{}"));
 }
 CHECK(x4tf.requests==before+4 && x4tf.live_connections==1);
 const char *const rejected[] = {
  X4_SIGL_URL_PREFIX "af206485-e87d-4624-9007-cb7f6d0cc42e&market=MX&language=es-MX",
  X4_SIGL_URL_PREFIX "00000000-0000-0000-0000-000000000000&market=MX&language=es-MX",
  X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=US&language=es-MX",
  X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=MX&language=en-US",
  X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=MX&language=es-MX&extra=1",
  X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&language=es-MX&market=MX",
  X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=MX&languages=es-MX",
  X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=MX&language=es-MX#extra",
  "https://catalog.gamepass.com.invalid/sigls/v2?id=" X4_SIGL_GUID_FREE "&market=MX&language=es-MX",
  "https://catalog.gamepass.com:443/sigls/v2?id=" X4_SIGL_GUID_FREE "&market=MX&language=es-MX",
  "https://catalog.gamepass.com/sigls/v1?id=" X4_SIGL_GUID_FREE "&market=MX&language=es-MX",
 };
 before = x4tf.connections;
 for (size_t i=0; i<sizeof(rejected)/sizeof(*rejected); ++i) {
  CHECK(x4_http_public_metadata_request(h, rejected[i], body, sizeof(body), &length, &status, NULL)
    == X4_HTTP_URL);
 }
 CHECK(x4_http_public_image_request(h, allowed[0], (unsigned char *)body, sizeof(body),
   &length, &status, NULL) == X4_HTTP_URL);
 CHECK(x4tf.connections==before);

 market = "GB";
 locale = "en-GB";
 const char *british = X4_SIGL_URL_PREFIX X4_SIGL_GUID_FREE "&market=GB&language=en-GB";
 CHECK(!x4_http_public_metadata_request(h, british, body, sizeof(body), &length, &status, NULL));
 CHECK(x4_http_public_metadata_request(h, allowed[0], body, sizeof(body), &length, &status, NULL)
   == X4_HTTP_URL);
 market = "MX";
 locale = "es-MX";

 x4tf.fail_read = true;
 before = x4tf.requests;
 CHECK(x4_http_public_metadata_request(h, allowed[0], body, sizeof(body), &length, &status, NULL)
   == X4_TF_RESET);
 CHECK(x4tf.requests==before+1 && !length && !status && !body[0] && x4tf.live_connections==1);
 before = x4tf.connections;
 public_exchange(h, false);
 CHECK(x4tf.connections==before);
 x4_http_close(h);
 CHECK(!x4tf.live_connections && !x4tf.live_contexts);
}
static void test_retry_after(void)
{
 CHECK(x4_http_wire_retry_after("15",2,0)==15000);
 CHECK(x4_http_wire_retry_after("-1",2,0)==0);CHECK(x4_http_wire_retry_after("1.5",3,0)==0);
 CHECK(x4_http_wire_retry_after("999999999999999999999",21,0)==UINT64_MAX);
 CHECK(x4_http_wire_retry_after("Sun, 06 Nov 1994 08:49:37 GMT",29,784111767)==10000);
 CHECK(x4_http_wire_retry_after("Sun, 06 Nov 1994 08:49:37 GMT",29,784111778)==0);
 CHECK(x4_http_wire_retry_after("Tue, 29 Feb 2000 00:00:00 GMT",29,951782390)==10000);
 CHECK(x4_http_wire_retry_after("Tue, 29 Feb 2001 00:00:00 GMT",29,0)==0);
 CHECK(retry_headers("retry-after:\t5 \r\n",0)==5000);
 CHECK(retry_headers("Retry-After: 5\r\nRetry-After: 6\r\n",0)==0);
 CHECK(retry_headers("Retry-After: invalid\r\n",0)==0);
 X4Http *h=open_context();response_status=429;response_headers="HTTP/1.1 429\r\nRetry-After: 15\r\n\r\n";
 unsigned before=x4tf.requests;public_exchange(h,false);CHECK(x4tf.requests-before==1&&x4_http_public_retry_after_ms(h)==15000);
 response_status=200;response_headers="HTTP/1.1 200\r\n\r\n";public_exchange(h,true);
 CHECK(!x4_http_public_retry_after_ms(h));x4_http_close(h);
 /* No optional sceHttp header API exists now: an absent wire field is the
  * equivalent unknown Retry-After result and cannot fail authentication. */
 h=open_context();public_exchange(h,false);CHECK(!x4_http_public_retry_after_ms(h));x4_http_close(h);
}
static void test_quarantine(void)
{
 X4Http *h=open_context();public_exchange(h,false);unsigned before=x4tf.connection_closes;
 x4tf.fail_close=true;CHECK(x4_http_public_reset(h)==X4_HTTP_UNUSABLE);
 char body[64];size_t length;int status;
 CHECK(x4_http_public_metadata_request(h,store,body,sizeof(body),&length,&status,NULL)==X4_HTTP_UNUSABLE);
 x4_http_close(h);CHECK(x4tf.connection_closes==before&&x4tf.live_connections==1&&x4tf.live_contexts==1);
 reclaim_fake_quarantine();
 h=open_context();public_exchange(h,false);x4tf.fail_read=x4tf.fail_close=true;
 CHECK(x4_http_public_metadata_request(h,store,body,sizeof(body),&length,&status,NULL)==X4_HTTP_UNUSABLE);
 CHECK(!length&&!status&&!body[0]);x4_http_close(h);CHECK(x4tf.live_connections&&x4tf.live_contexts);
 reclaim_fake_quarantine();
}
int main(void)
{
 test_reuse_isolation_cleanup();test_retry_and_cancellation();test_metadata_bound_closes();test_policy_and_owner();test_sigl_policy();
 test_retry_after();test_quarantine();
 printf("production public HTTP PASS: %u policy checks; real request wire with deterministic TLS, no network\n",checks);
 return 0;
}
