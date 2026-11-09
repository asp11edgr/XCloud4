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
#if !X4_PUBLIC && !X4_VIDEO_PROBE
static const char *xsts_override;
static int xsts_status;
static bool quota_request;
static X4XstsDiagSnapshot claim_snapshot;
#endif
static const char *const user_claims[] = {
    "{\"Token\":\"user-token\",\"DisplayClaims\":{\"xui\":[{\"gtg\":\"ClassicPlayer\",\"mgt\":\"ModernPlayer\"}]}}",
    "{\"Token\":\"user-token\",\"DisplayClaims\":{\"xui\":[{\"mgt\":\"Jugador \\u00d1\"}]}}",
    "{\"Token\":\"user-token\",\"DisplayClaims\":{\"xui\":[{\"gtg\":{},\"mgt\":\"   \"}]}}",
    "{\"Token\":\"user-token\"}",
    "{\"Token\":\"user-token\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"987\"}]}}",
};
static const char *const xsts_claims[] = {
    "{\"Token\":\"xsts-token\",\"DisplayClaims\":{\"xui\":[{\"gtg\":\"OtherName\","
        "\"uhs\":\"987\",\"\\u0078id\":\"1234567890\",\"extra_claim\":{\"nested\":\"hidden\"},"
        "\"unsafe\\u0022key\":\"ignored\"},{\"second_user\":\"hidden\"}]}}",
    "{\"Token\":\"xsts-token\"}",
    "{\"Token\":\"xsts-token\",\"DisplayClaims\":{\"xui\":[{\"gtg\":\"\",\"mgt\":\"RestoredPlayer\"}]}}",
    "{\"Token\":\"xsts-token\"}",
    "{\"Token\":\"xsts-token\",\"DisplayClaims\":{\"xui\":[{\"gtg\":null,\"mgt\":false}]}}",
};
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
    if (!strcmp(url, X4_XBOX_USER_URL)) {
        CHECK(stage++ == 0);
        CHECK(json && strstr(json, "d=private-host-token"));
        response = user_claims[mode];
    } else if (!strcmp(url, X4_XBOX_XSTS_URL)) {
        CHECK(stage++ == 1);
        CHECK(json && strstr(json, "user-token"));
        response = xsts_claims[mode];
#if !X4_PUBLIC && !X4_VIDEO_PROBE
        CHECK(strstr(json, quota_request ? "http://mp.microsoft.com/" : "http://gssv.xboxlive.com/"));
        if (xsts_override) {
            response = xsts_override;
            *status = xsts_status;
        }
#endif
    }
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

#if !X4_PUBLIC && !X4_VIDEO_PROBE
static void diag_exchange(const char *body, int status, bool market)
{
    new_run(0);
    stage = 1;
    xsts_override = body;
    xsts_status = status;
    quota_request = market;
    current_work->cancel = &cancel_flag;
    current_work->http = x4_http_open(NULL);
    CHECK(current_work->http);
    strcpy(current_work->user_token, "user-token");
    char hash[21] = {0};
    CHECK((xsts_request(current_work, market ? &hash : NULL) == 0) == (status == 200));
    CHECK(!market || !strcmp(hash, "987"));
    CHECK(zero_buffer(current_work->user_token, sizeof(current_work->user_token)));
    CHECK(zero_buffer(current_work->response, sizeof(current_work->response)));
    x4_http_close(current_work->http);
    x4_xbox_work_free(current_work);
    current_work = NULL;
    xsts_override = NULL;
    quota_request = false;
}

static void verify_claim_diag(void)
{
    x4_xsts_diag_snapshot(&claim_snapshot);
    CHECK(claim_snapshot.count == 6 && claim_snapshot.revision == x4_xsts_diag_revision());
    const X4XstsDiagResponse *record = &claim_snapshot.responses[0];
    CHECK(record->party == X4_XSTS_DIAG_GSSV && record->status == 200);
    CHECK(record->count == 4 && record->omitted == 1);
    const char *const expected[] = {"gtg", "uhs", "xid", "extra_claim"};
    for (unsigned i = 0; i < record->count; ++i) CHECK(!strcmp(record->names[i], expected[i]));
    CHECK(claim_snapshot.responses[1].count == 0);
    CHECK(claim_snapshot.responses[2].count == 2 &&
        !strcmp(claim_snapshot.responses[2].names[1], "mgt"));

    diag_exchange("{\"Token\":\"xsts-token\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"987\",\"xid\":\"private-number\"}]}}", 200, true);
    diag_exchange("{\"XErr\":42,\"DisplayClaims\":{\"xui\":[{\"denied\":\"private-value\"}]}}", 401, false);
    diag_exchange("{malformed}", 503, false);
    x4_xsts_diag_snapshot(&claim_snapshot);
    CHECK(claim_snapshot.count == 9 && claim_snapshot.revision == 9);
    record = &claim_snapshot.responses[6];
    CHECK(record->party == X4_XSTS_DIAG_MARKET && record->status == 200 && record->count == 2);
    CHECK(!strcmp(record->names[0], "uhs") && !strcmp(record->names[1], "xid"));
    record = &claim_snapshot.responses[7];
    CHECK(record->party == X4_XSTS_DIAG_GSSV && record->status == 401 && record->count == 1);
    CHECK(!strcmp(record->names[0], "denied"));
    CHECK(claim_snapshot.responses[8].status == 503 && claim_snapshot.responses[8].count == 0);

    const char crowded[] = "{\"DisplayClaims\":{\"xui\":[{\"a0\":0,\"a1\":0,\"a2\":0,"
        "\"a3\":0,\"a4\":0,\"a5\":0,\"a6\":0,\"a7\":0,\"a8\":0,\"a9\":0,\"a10\":0,\"a11\":0,"
        "\"a12\":0,\"a13\":0,\"a14\":0,\"a15\":0,\"a16\":0}]}}";
    X4JsonSpan root;
    CHECK(x4_json_parse(crowded, sizeof(crowded) - 1, &root) == 0);
    for (unsigned i = 0; i < X4_XSTS_DIAG_RESPONSES; ++i) note_xsts_claims(X4_XSTS_DIAG_GSSV, (int)i, root);
    x4_xsts_diag_snapshot(&claim_snapshot);
    CHECK(claim_snapshot.count == X4_XSTS_DIAG_RESPONSES &&
        claim_snapshot.revision == X4_XSTS_DIAG_RESPONSES + 9);
    for (unsigned i = 0; i < claim_snapshot.count; ++i) {
        record = &claim_snapshot.responses[i];
        CHECK(record->status == (int)i && record->count == X4_XSTS_DIAG_NAMES && record->omitted == 1);
        CHECK(!strcmp(record->names[0], "a0") && !strcmp(record->names[15], "a15"));
    }
    puts("HOST51C_XSTS_PASS relying_parties=2 values=0 invalid_response=1 history_limit=32");
}
#endif

int main(void)
{
    const char *const gamertags[] = {"ClassicPlayer", "Jugador Ñ", "RestoredPlayer", "", ""};
    for(unsigned m=0;m<5;++m){new_run(m);const X4CatalogSnapshot *catalog=x4_xbox_catalog(current_work,"private-host-token",&cancel_flag,NULL,NULL);const X4RecentSnapshot *recent=x4_xbox_catalog_recent(current_work);
        CHECK(!strcmp(catalog->gamertag, gamertags[m]));
        if(m==3){CHECK(catalog->state==X4_CATALOG_CANCELLED&&recent->state==X4_CATALOG_CANCELLED);CHECK(!store_calls);}
        else if(m==4){CHECK(catalog->state==X4_CATALOG_ERROR&&recent->state==X4_CATALOG_ERROR);CHECK(!mru_calls&&!store_calls);}
        else{CHECK(catalog->state==X4_CATALOG_READY&&catalog->count==1);CHECK(!strcmp(catalog->titles[0].name,"Edición ñ"));CHECK(store_calls==1&&mru_calls==1&&title_calls==1);
            if(m==0)CHECK(recent->state==X4_CATALOG_READY&&recent->count==1);else CHECK(recent->state==X4_CATALOG_ERROR&&!recent->count);}
        verify_wiped();
    }
    new_run(0);const X4RecentSnapshot *recent=x4_xbox_recents(current_work,"private-host-token",&cancel_flag);CHECK(recent->state==X4_CATALOG_READY&&recent->count==1);CHECK(!title_calls&&!store_calls&&mru_calls==1);verify_wiped();
#if !X4_PUBLIC && !X4_VIDEO_PROBE
    verify_claim_diag();
#endif
    printf("production Xbox MRU exchange PASS: %u checks; gamertag claims classic/modern/absent, no live request or streaming\n",checks);return 0;
}
