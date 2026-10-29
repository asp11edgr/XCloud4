/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <strings.h> /* strcasecmp used by the included production unit. */
/* The catalog Store URL now uses the production locale owner. Keep its real
 * implementation in this isolated harness; the original runner is unchanged. */
#include "../src/ui/lang.c"
#include "../src/auth/xbox_live.c"
static unsigned checks, stage, mode, mru_calls, store_calls, title_calls;
static X4XboxWork *current_work;
static _Atomic int cancel_flag;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"MRU exchange check failed line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
/* Server choice is a separate owner. Model a previously saved Automatic
 * choice; assert the real parser publishes the fixture's sole default region.
 * This MRU-only harness must neither open a first-choice panel nor wait. */
static unsigned region_updates,server_choice_queries;
bool x4_server_settings_update_regions(const X4ServerRegion *regions,unsigned count)
{
    CHECK(regions&&count==1&&!strcmp(regions[0].name,"Host Region")&&regions[0].is_default);
    CHECK(!strcmp(regions[0].api_host,"region.gssv-play-prod.xboxlive.com"));
    ++region_updates;return true;
}
bool x4_server_settings_has_saved_choice(void){++server_choice_queries;return true;}
const char *x4_server_settings_get_selected(void){return "";}
void x4_server_settings_reset_ui_drawn(void){CHECK(0&&"saved choice must skip first-choice UI");}
void x4_server_settings_set_awaiting_first_choice(bool awaiting){(void)awaiting;CHECK(0&&"no first-choice UI in MRU exchange");}
bool x4_server_settings_awaiting_first_choice(void){CHECK(0&&"no first-choice wait in MRU exchange");return false;}
bool x4_server_settings_is_ui_drawn(void){CHECK(0&&"no first-choice drawing in MRU exchange");return false;}
void x4_server_settings_notify_choice_done(void){CHECK(0&&"no synthetic confirmation in MRU exchange");}
uint64_t sceKernelGetProcessTime(void){return 1000000;}
int sceKernelUsleep(unsigned us){(void)us;return 0;}
void x4_secure_clear(void *p,size_t n){volatile unsigned char *b=p;while(n--)*b++=0;}
struct X4Http{int unused;};
X4Http *x4_http_open(int *error){if(error)*error=0;return calloc(1,sizeof(X4Http));}
void x4_http_close(X4Http *h){free(h);}
const char *x4_http_stage(const X4Http *h){(void)h;return "controlled host transport";}
int x4_http_json_request(X4Http *h,const char *url,const X4HttpHeader *headers,size_t count,const char *json,
    char *body,size_t cap,size_t *length,int *status,const _Atomic int *cancel)
{
    (void)h;CHECK(cancel==&cancel_flag);*status=200;
    const char *response=NULL;
    if(!strcmp(url,X4_XBOX_USER_URL)){CHECK(stage++==0);CHECK(json&&strstr(json,"d=private-host-token"));response="{\"Token\":\"user-token\"}";}
    else if(!strcmp(url,X4_XBOX_XSTS_URL)){CHECK(stage++==1);CHECK(json&&strstr(json,"user-token"));response="{\"Token\":\"xsts-token\"}";}
    else if(!strcmp(url,offerings[0].url)){CHECK(stage++==2);CHECK(json&&strstr(json,"xsts-token"));response="{\"gsToken\":\"gs-token\",\"offeringSettings\":{\"regions\":[{\"isDefault\":true,\"baseUri\":\"https://region.gssv-play-prod.xboxlive.com\",\"name\":\"Host Region\"}]}}";}
    else if(!strcmp(url,"https://region.gssv-play-prod.xboxlive.com/v2/titles")){
        CHECK(!json&&count==3);CHECK(!strcmp(headers[0].value,"Bearer gs-token"));CHECK(current_work->gs_token[0]);++title_calls;
        response="{\"results\":[{\"titleId\":\"CURRENT_TITLE\",\"details\":{\"productId\":\"PROD1\",\"hasEntitlement\":true}}]}";
        if(mode==4){*status=500;response="{}";}
    }else if(!strcmp(url,"https://region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25")){
        CHECK(!json&&count==3&&cap==X4_RECENT_RESPONSE_MAX+1u);CHECK(!strcmp(headers[0].name,"Authorization")&&!strcmp(headers[0].value,"Bearer gs-token"));CHECK(!current_work->gs_token[0]);++mru_calls;
        response="{\"results\":[{\"titleId\":\"CURRENT_TITLE\"}]}";
        if(mode==1){*status=403;response="{}";}
        if(mode==2)response="{\"results\":[{\"titleId\":\"bad/ID\"}]}";
        if(mode==3){atomic_store(&cancel_flag,1);body[0]=0;*length=0;*status=0;return X4_HTTP_CANCELLED;}
    }else if(!strncmp(url,X4_XBOX_STORE_PREFIX,strlen(X4_XBOX_STORE_PREFIX))){
        CHECK(!json&&!headers&&!count);CHECK(!current_work->gs_token[0]&&!current_work->authorization[0]);++store_calls;
        response="{\"Products\":[{\"ProductId\":\"PROD1\",\"LocalizedProperties\":[{\"ProductTitle\":\"Edición ñ\"}]}]}";
    }else{CHECK(0);return X4_HTTP_URL;}
    CHECK(response);size_t n=strlen(response);CHECK(n<cap);memcpy(body,response,n+1);*length=n;return 0;
}
static void new_run(unsigned m){mode=m;stage=title_calls=mru_calls=store_calls=region_updates=server_choice_queries=0;atomic_store(&cancel_flag,0);current_work=x4_xbox_work_new();CHECK(current_work);}
static int zero_buffer(const void *p,size_t n){const unsigned char *b=p;while(n--)if(*b++)return 0;return 1;}
static void verify_wiped(void){
    CHECK(region_updates==1&&server_choice_queries==1);
    CHECK(zero_buffer(current_work->user_token,sizeof(current_work->user_token))&&zero_buffer(current_work->xsts_token,sizeof(current_work->xsts_token))&&zero_buffer(current_work->gs_token,sizeof(current_work->gs_token))&&zero_buffer(current_work->authorization,sizeof(current_work->authorization)));
    CHECK(zero_buffer(current_work->response,sizeof(current_work->response))&&zero_buffer(current_work->request,sizeof(current_work->request))&&zero_buffer(current_work->url,sizeof(current_work->url))&&zero_buffer(current_work->origin,sizeof(current_work->origin)));
    x4_xbox_work_free(current_work);current_work=NULL;
}
int main(void)
{
    for(unsigned m=0;m<5;++m){new_run(m);const X4CatalogSnapshot *catalog=x4_xbox_catalog(current_work,"private-host-token",&cancel_flag,NULL,NULL);const X4RecentSnapshot *recent=x4_xbox_catalog_recent(current_work);
        if(m==3){CHECK(catalog->state==X4_CATALOG_CANCELLED&&recent->state==X4_CATALOG_CANCELLED);CHECK(!store_calls);}
        else if(m==4){CHECK(catalog->state==X4_CATALOG_ERROR&&recent->state==X4_CATALOG_ERROR);CHECK(!mru_calls&&!store_calls);}
        else{CHECK(catalog->state==X4_CATALOG_READY&&catalog->count==1);CHECK(!strcmp(catalog->titles[0].name,"Edición ñ"));CHECK(store_calls==1&&mru_calls==1&&title_calls==1);
            if(m==0)CHECK(recent->state==X4_CATALOG_READY&&recent->count==1);else CHECK(recent->state==X4_CATALOG_ERROR&&!recent->count);}
        verify_wiped();
    }
    new_run(0);const X4RecentSnapshot *recent=x4_xbox_recents(current_work,"private-host-token",&cancel_flag);CHECK(recent->state==X4_CATALOG_READY&&recent->count==1);CHECK(!title_calls&&!store_calls&&mru_calls==1);verify_wiped();
    printf("production Xbox MRU exchange PASS: %u checks; controlled host transport, no live request or streaming\n",checks);return 0;
}
