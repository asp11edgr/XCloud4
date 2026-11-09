/* SPDX-License-Identifier: GPL-3.0-only */
/* Real HTTP policy/parser with deterministic TLS I/O and recorded budgets. */
#include <strings.h>
#include "../src/auth/http_client.c"
#include "tls_transport_fake.h"
static X4Http *test_http;
static unsigned http_checks,http_calls,budget_count;
static const char *test_response;
static int test_status;
static struct { X4TlsBudget budget;uint64_t at;unsigned operation; } observed[128];
#define HCHECK(x) do { ++http_checks; if(!(x)){fprintf(stderr,"state HTTP check failed line=%d\n",__LINE__);abort();} } while(0)
static void state_budget(unsigned operation,const X4TlsBudget *budget)
{
 HCHECK(budget&&budget_count<128);observed[budget_count].budget=*budget;
 observed[budget_count].at=x4_tls_now_us();observed[budget_count++].operation=operation;
}
static void state_request(const X4TfRequest *request,X4TfResponse *response)
{
 HCHECK(request->method==0&&!request->body_size&&!request->keepalive);
 HCHECK(strstr(request->target,"/state")&&x4tf_header(request,"Authorization"));
 ++http_calls;response->status=test_status;response->body=test_response;
}
void t29_state_http_init(void)
{
 HCHECK(!test_http);x4tf_reset();x4tf.on_request=state_request;x4tf.on_budget=state_budget;
 int error=-1;test_http=x4_http_open(&error);HCHECK(test_http&&!error);
 http_calls=budget_count=0;memset(observed,0,sizeof(observed));
 test_response="{\"state\":\"Closed\"}";test_status=200;
}
unsigned t29_state_http_calls(void){return http_calls;}
X4Http *t29_state_http_context(void){return test_http;}
void t29_state_http_response(const char *body,int status)
{HCHECK(body);test_response=body;test_status=status;}
void t29_state_http_finish(void)
{
 HCHECK(test_http&&!x4tf.live_connections&&!atomic_load(&test_http->busy));
 x4_http_close(test_http);test_http=NULL;HCHECK(!x4tf.live_contexts);
}
static uint64_t effective_budget(unsigned i)
{
 X4TlsBudget *b=&observed[i].budget;uint64_t now=observed[i].at;
 uint64_t remaining=b->deadline_us>now?b->deadline_us-now:0;
 uint64_t phase=b->phase_deadline_us?(b->phase_deadline_us>now?b->phase_deadline_us-now:0):5000000;
 return phase<remaining?phase:remaining;
}
static uint64_t phase_budget_value(unsigned i)
{
 X4TlsBudget *b=&observed[i].budget;
 return b->phase_deadline_us?b->phase_deadline_us-observed[i].at:5000000;
}
void test_session_state_http(void)
{
 static const char state_url[]="https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/state";
 static const char *const other_urls[]={
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/play",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/ice",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/sdp",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/connect",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/keepalive",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v1/waittime/FAKE_GAME",
  "https://example.invalid/v5/sessions/cloud/FAKE_SESSION/state",
  "https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/state?extra=1"};
 const X4HttpHeader headers[]={{"Authorization","Bearer FAKE_HTTP_STATE_29"},
  {"x-gssv-client","XboxComBrowser"},{"X-MS-Device-Info","{\"fixture\":true}"}};
 char body[256];size_t length=0;int status=0;t29_state_http_init();
 for(unsigned i=0;i<sizeof(other_urls)/sizeof(*other_urls);++i){
  HCHECK(x4_http_session_state_request(test_http,other_urls[i],headers,3,body,sizeof(body),&length,&status,NULL)==X4_HTTP_URL);
  HCHECK(!length&&!status&&!x4tf.connections&&!http_calls&&!budget_count);
 }
 HCHECK(!x4_http_session_state_request(test_http,state_url,headers,3,body,sizeof(body),&length,&status,NULL));
 HCHECK(http_calls==1&&x4tf.connections==1&&status==200&&length==strlen(test_response));
 HCHECK(budget_count>=3);
 for(unsigned i=0;i<budget_count;++i)HCHECK(effective_budget(i)>0&&effective_budget(i)<=2000000);
 t29_state_http_finish();t29_state_http_init();
 HCHECK(!x4_http_session_request(test_http,X4_SESSION_HTTP_GET,state_url,headers,3,NULL,body,sizeof(body),&length,&status,NULL));
 HCHECK(http_calls==1&&x4tf.connections==1&&status==200&&budget_count>=3);
 for(unsigned i=0;i<budget_count;++i)HCHECK(phase_budget_value(i)==5000000&&effective_budget(i)==5000000);
 /* An equal duration never implicitly selects short_state. Configured phases
  * remain 5s, while the absolute transfer deadline is independently honored. */
 Owned *ordinary=own(X4_HTTP_METHOD_GET,state_url,NULL,0,NULL,headers,3);
 HCHECK(ordinary&&!ordinary->short_state);ordinary->deadline_us=2000000;
 budget_count=0;length=0;status=0;
 HCHECK(!transfer(test_http,ordinary,body,sizeof(body),&length,&status,NULL));
 HCHECK(budget_count>=3);
 for(unsigned i=0;i<budget_count;++i)HCHECK(phase_budget_value(i)==5000000&&effective_budget(i)<=2000000);
 t29_state_http_finish();
 printf("PASS state HTTP API: %u checks; exact route, short2s, ordinary phase5s, explicit flag, wire and joined cleanup\n",http_checks);
}
