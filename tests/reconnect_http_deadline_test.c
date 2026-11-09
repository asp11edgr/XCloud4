/* SPDX-License-Identifier: GPL-3.0-only */
#include "session_state_http_test.c"
#include <assert.h>
static uint64_t fixture_clock=1000000,protected_deadline,last_recv,last_send;
static bool exceed_send;
uint64_t sceKernelGetProcessTime(void){return fixture_clock;}
void t29_state_http_clock_advance(uint64_t us){fixture_clock+=us;}
static void reconnect_request(const X4TfRequest *request,X4TfResponse *response)
{
 HCHECK((request->method==0||request->method==1)&&!request->keepalive);
 ++http_calls;response->status=test_status;response->body=test_response;
}
static void reconnect_budget(unsigned operation,const X4TlsBudget *budget)
{
 state_budget(operation,budget);
 assert(fixture_clock<protected_deadline&&budget->deadline_us<=protected_deadline);
 uint64_t remaining=effective_budget(budget_count-1);
 assert(remaining&&remaining<=5000000&&remaining<=protected_deadline-fixture_clock);
 if(operation==1){last_send=remaining;fixture_clock+=exceed_send?remaining:100000;}
 if(operation==2){last_recv=remaining;fixture_clock+=100000;}
}
static void setup(uint64_t relative)
{
 fixture_clock=1000000;t29_state_http_init();protected_deadline=fixture_clock+relative;
 last_recv=last_send=0;exceed_send=false;x4tf.on_budget=reconnect_budget;x4tf.on_request=reconnect_request;
}
int main(void)
{
 test_session_state_http();
 const X4HttpHeader headers[]={{"Authorization","Bearer FAKE_HTTP_30"},
  {"x-gssv-client","XboxComBrowser"},{"X-MS-Device-Info","{}"}};
 const char *paths[]={"state","connect","sdp","ice","keepalive"};
 char body[512];size_t length;int status;_Atomic int cancel=0;
 for(unsigned i=0;i<5;++i){
  setup(1500000);char url[200];snprintf(url,sizeof(url),"https://fixture.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/%s",paths[i]);
  bool post=i==1||i==4;const char *payload=i==1?"{}":i==4?"":NULL;
  assert(!x4_http_session_reconnect_request(test_http,post?X4_SESSION_HTTP_POST:X4_SESSION_HTTP_GET,
   url,headers,3,payload,body,sizeof(body),&length,&status,&cancel,protected_deadline));
  assert(http_calls==1&&status==200&&budget_count>=3&&last_recv<1500000);t29_state_http_finish();
 }
 setup(2000000);uint64_t later=fixture_clock+10000000;
 assert(!x4_http_session_reconnect_request(test_http,X4_SESSION_HTTP_GET,
  "https://fixture.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/state",headers,3,NULL,
  body,sizeof(body),&length,&status,&cancel,later));
 assert(observed[0].budget.deadline_us-observed[0].at==2000000);t29_state_http_finish();
 const char *rejects[]={
  "https://fixture.gssv-play-prod.xboxlive.com/v5/sessions/cloud/play",
  "https://fixture.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION",
  "https://fixture.gssv-play-prod.xboxlive.com/v1/waittime/FAKE_GAME",
  "https://example.invalid/v5/sessions/cloud/FAKE_SESSION/sdp"};
 for(unsigned i=0;i<4;++i){setup(1500000);
  assert(x4_http_session_reconnect_request(test_http,X4_SESSION_HTTP_GET,rejects[i],headers,3,NULL,
   body,sizeof(body),&length,&status,&cancel,protected_deadline)==X4_HTTP_URL);
  assert(!http_calls&&!x4tf.connections);t29_state_http_finish();}
 for(unsigned passport=0;passport<2;++passport){setup(1500000);
  assert(!x4_http_auth_reconnect_request(test_http,passport,"grant_type=refresh_token&refresh_token=FAKE_REFRESH_30",
   body,sizeof(body),&length,&status,&cancel,protected_deadline));
  assert(http_calls==1&&observed[0].budget.deadline_us-observed[0].at==1500000);t29_state_http_finish();}
 setup(1500000);exceed_send=true;
 assert(x4_http_session_reconnect_request(test_http,X4_SESSION_HTTP_GET,
  "https://fixture.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/ice",headers,3,NULL,
  body,sizeof(body),&length,&status,&cancel,protected_deadline)==X4_HTTP_DEADLINE);
 assert(fixture_clock<=protected_deadline&&length==0&&status==0);t29_state_http_finish();
 setup(1500000);atomic_store(&cancel,1);
 assert(x4_http_session_reconnect_request(test_http,X4_SESSION_HTTP_GET,
  "https://fixture.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/sdp",headers,3,NULL,
  body,sizeof(body),&length,&status,&cancel,protected_deadline)==X4_HTTP_CANCELLED);
 assert(!http_calls);t29_state_http_finish();atomic_store(&cancel,0);
 test_session_state_http();
 puts("PASS real HTTP reconnect routes/auth; absolute budget clamps; state max2s; cancel/deadline; ordinary phase5s");
 return 0;
}
