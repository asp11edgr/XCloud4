/* SPDX-License-Identifier: GPL-3.0-only */
/* Production login constructors and full production HTTP/TLS ownership run
 * against the existing native ABI fixture. No live request is made. */
#define main access_base_main
#define x4_module_symbol access_base_symbol
#include "stream_os_profile_http_test.c"
#undef x4_module_symbol
#undef main

static X4AccessCountry expected_country;
static unsigned forwarded[128], forwarded_total, login_total, login_main, login_f2p;
static bool is_f2p[128], refuse_main;

static int32_t access_request(int32_t connection,int32_t method,const char *url,uint64_t bytes)
{
    if (strcmp(url,offerings[1].url)) return native_request(connection,method,url,bytes);
    CHECK(resource[connection].kind==TEST_CONNECTION&&method==1);
    int32_t id=test_new_resource(TEST_REQUEST,connection);
    resource[id].method=method;resource[id].borrowed_url=url;
    resource[id].payload_size=bytes;resource[id].route=TEST_LOGIN;is_f2p[id]=true;
    return id;
}
static int32_t access_header(int32_t id,const char *name,const char *value,uint32_t mode)
{
    if (strcmp(name,"X-Forwarded-For")) return native_header(id,name,value,mode);
    CHECK(resource[id].kind==TEST_REQUEST&&resource[id].route==TEST_LOGIN&&resource[id].method==1&&!mode);
    CHECK(x4_access_country_ip(expected_country)&&!strcmp(value,x4_access_country_ip(expected_country)));
    CHECK(++forwarded[id]==1);++forwarded_total;return 0;
}
static int32_t access_send(int32_t id,const void *body,size_t bytes)
{
    CHECK(resource[id].route==TEST_LOGIN ? forwarded[id]==(expected_country!=X4_ACCESS_AUTO) : !forwarded[id]);
    int32_t rc=native_send(id,body,bytes);
    if(resource[id].route==TEST_LOGIN){
        ++login_total;
        if(is_f2p[id])++login_f2p;else ++login_main;
        if(refuse_main&&!is_f2p[id]) resource[id].response="{}";
    }
    return rc;
}
static int32_t access_status(int32_t id,int32_t *status)
{
    if(resource[id].route==TEST_LOGIN&&refuse_main&&!is_f2p[id]){*status=403;return 0;}
    return native_status(id,status);
}
static int32_t access_delete_template(int32_t id)
{
    int32_t rc=native_delete_template(id);
    for(unsigned i=1;i<=next_resource;++i)CHECK(resource[i].kind==TEST_NONE);
    memset(resource,0,sizeof(resource));memset(forwarded,0,sizeof(forwarded));memset(is_f2p,0,sizeof(is_f2p));
    next_resource=0;return rc;
}
int x4_module_symbol(int handle,const char *name,void **address)
{
    if(!strcmp(name,"sceHttpCreateRequestWithURL")){*address=(void *)access_request;return 0;}
    if(!strcmp(name,"sceHttpAddRequestHeader")){*address=(void *)access_header;return 0;}
    if(!strcmp(name,"sceHttpSendRequest")){*address=(void *)access_send;return 0;}
    if(!strcmp(name,"sceHttpGetStatusCode")){*address=(void *)access_status;return 0;}
    if(!strcmp(name,"sceHttpDeleteTemplate")){*address=(void *)access_delete_template;return 0;}
    return access_base_symbol(handle,name,address);
}
static void begin_country(X4AccessCountry country,bool fallback)
{
    expected_country=country;refuse_main=fallback;
    forwarded_total=login_total=login_main=login_f2p=0;
    memset(route_calls,0,sizeof(route_calls));memset(resource,0,sizeof(resource));
    next_resource=templates=terminated=public_device_headers=0;
    memset(forwarded,0,sizeof(forwarded));memset(is_f2p,0,sizeof(is_f2p));
    atomic_store(&cancel_flag,0);expected_contract();
}
static X4XboxWork *country_work(void)
{
    X4XboxWork *w=x4_xbox_work_new();CHECK(w&&w->access_country==X4_ACCESS_AUTO);
    CHECK(!x4_xbox_work_set_access_country(NULL,expected_country));
    CHECK(!x4_xbox_work_set_access_country(w,(X4AccessCountry)-1));
    CHECK(!x4_xbox_work_set_access_country(w,X4_ACCESS_COUNTRY_COUNT));
    CHECK(x4_xbox_work_set_access_country(w,expected_country));
#if X4_STREAM_PROFILE
    X4StreamRequestConfig request=x4_stream_request_config((X4StreamRequestProfile)runtime_profile,X4_COLOR_AUTO_709);
    CHECK(x4_xbox_work_set_request_config(w,&request));
#endif
    return w;
}
static void complete_country(X4XboxWork *w)
{
    CHECK(!w->http&&!w->user_token[0]&&!w->xsts_token[0]&&!w->gs_token[0]&&!w->authorization[0]);
    CHECK(login_total==(refuse_main?2u:1u)&&login_main==1&&login_f2p==(unsigned)refuse_main);
    CHECK(forwarded_total==(expected_country==X4_ACCESS_AUTO?0u:login_total));
    CHECK(w->access_country==expected_country&&terminated==1);
    x4_xbox_work_free(w);
}
static void catalog_routes(X4AccessCountry country,bool fallback,unsigned kind)
{
    begin_country(country,fallback);X4XboxWork *w=country_work();
    if(kind==0){const X4CatalogSnapshot *view=x4_xbox_catalog(w,"fixture-not-a-real-access-token",&cancel_flag,NULL,NULL);
        CHECK(view->state==X4_CATALOG_READY&&!view->count&&route_calls[TEST_TITLES]==1&&route_calls[TEST_MRU]==1);}
    else if(kind==1){const X4CatalogSnapshot *view=x4_xbox_fortnite_query(w,"fixture-not-a-real-access-token",&cancel_flag,NULL,NULL);
        CHECK(view->state==X4_CATALOG_ERROR&&view->error==X4_AUTH_E_FORTNITE_NOT_FOUND&&route_calls[TEST_TITLES]==1&&!route_calls[TEST_MRU]);}
    else {const X4RecentSnapshot *view=x4_xbox_recents(w,"fixture-not-a-real-access-token",&cancel_flag);
        CHECK(view->state==X4_CATALOG_READY&&!view->count&&!route_calls[TEST_TITLES]&&route_calls[TEST_MRU]==1);}
    complete_country(w);
}
static void session_constructors(X4AccessCountry country)
{
    begin_country(country,false);X4XboxWork *w=country_work();
    int error=-1;w->http=x4_http_open(&error);CHECK(w->http&&!error);w->cancel=&cancel_flag;
    CHECK(!x4_xbox_work_set_access_country(w,X4_ACCESS_AUTO)&&w->access_country==country);
    /* Same production user/XSTS/login used by session_run. The real session
     * constructors and transport run next; RTC streaming is not simulated. */
    CHECK(!user_token(w,"fixture-not-a-real-access-token")&&!xsts(w)&&!login(w,0));
    x4_secure_clear(w->xsts_token,sizeof(w->xsts_token));
    snprintf(w->authorization,sizeof(w->authorization),"Bearer %s",w->gs_token);
    x4_secure_clear(w->gs_token,sizeof(w->gs_token));
    w->session_start=session_now();w->passport_provider=test_passport;
    X4CatalogTitle title={0};copy_text(title.id,sizeof(title.id),"HOST_GAME");
    CHECK(session_create(w,&title)==1);SessionEnd end={0};uint64_t deadline=session_now()+90000000;
    CHECK(session_provision(w,deadline,&end)&&session_authorize(w,&end));
    size_t length=0;int status=0;copy_text(w->sdp,sizeof(w->sdp),"v=0\r\n");CHECK(signal_offer(w));
    CHECK(signal_call(w,"/sdp",X4_SESSION_HTTP_POST,w->request,deadline,&end,&length,&status));
    CHECK(signal_call(w,"/sdp",X4_SESSION_HTTP_GET,NULL,deadline,&end,&length,&status));
    copy_text(w->request,sizeof(w->request),"{\"fixture\":true}");
    CHECK(signal_call(w,"/ice",X4_SESSION_HTTP_POST,w->request,deadline,&end,&length,&status));
    CHECK(signal_call(w,"/ice",X4_SESSION_HTTP_GET,NULL,deadline,&end,&length,&status));
    CHECK(signal_keepalive(w,deadline,&end));end_with(&end,X4_SESSION_CLOSED,0,0,"host fixture complete");
    session_cleanup(w,&end);CHECK(!w->session.cleanup_failed&&!w->session_id[0]);
    x4_http_close(w->http);w->http=NULL;x4_secure_clear(w->user_token,sizeof(w->user_token));
    x4_secure_clear(w->authorization,sizeof(w->authorization));
    complete_country(w);
}
static void denied_headers(void)
{
    begin_country(X4_ACCESS_US,false);int error=-1;X4Http *h=x4_http_open(&error);CHECK(h&&!error);
    char response[256];size_t length;int status;
    X4HttpHeader header={"X-Forwarded-For",x4_access_country_ip(X4_ACCESS_US)};
    const char *urls[]={X4_XBOX_USER_URL,X4_XBOX_XSTS_URL,
        "https://fixture-region.gssv-play-prod.xboxlive.com/v2/titles",
        "https://fixture-region.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25",
        "https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=TEST&market=MX&languages=es-MX&fieldsTemplate=Details"};
    for(unsigned i=0;i<sizeof(urls)/sizeof(*urls);++i){
        length=99;status=99;unsigned before=templates;
        CHECK(x4_http_json_request(h,urls[i],&header,1,i<2?"{}":NULL,response,sizeof(response),&length,&status,&cancel_flag)==X4_HTTP_ARGUMENT);
        CHECK(!length&&!status&&templates==before);
    }
    const char *bad_values[]={"1.2.3.4","143.244.47.65\r\nAuthorization: invalid"," 143.244.47.65","143.244.47.65 ","143.244.47.065","143.244.47.65, 169.150.198.66",""};
    for(unsigned i=0;i<sizeof(bad_values)/sizeof(*bad_values);++i){
        header.value=bad_values[i];CHECK(x4_http_json_request(h,offerings[0].url,&header,1,"{}",response,sizeof(response),&length,&status,&cancel_flag)!=0);
    }
    header.value=x4_access_country_ip(X4_ACCESS_US);X4HttpHeader twice[]={header,header};
    CHECK(x4_http_json_request(h,offerings[0].url,twice,2,"{}",response,sizeof(response),&length,&status,&cancel_flag)==X4_HTTP_ARGUMENT);
    const char *bad_names[]={"x-forwarded-for","X-FORWARDED-FOR","X-Forwarded-For\r\nOther","X-Forwarded-For "};
    for(unsigned i=0;i<sizeof(bad_names)/sizeof(*bad_names);++i){
        header.name=bad_names[i];CHECK(x4_http_json_request(h,offerings[0].url,&header,1,"{}",response,sizeof(response),&length,&status,&cancel_flag)==X4_HTTP_ARGUMENT);
    }
    header.name="X-Forwarded-For";
    CHECK(x4_http_json_request(h,offerings[0].url,&header,1,NULL,response,sizeof(response),&length,&status,&cancel_flag)==X4_HTTP_ARGUMENT);
    const char *paths[]={"play","HOST_SESSION/state","HOST_SESSION/connect","HOST_SESSION/sdp","HOST_SESSION/ice","HOST_SESSION/keepalive","HOST_SESSION","/v1/waittime/HOST_GAME"};
    X4HttpHeader session_headers[]={{"Authorization","Bearer fixture-not-a-real-token"},{"x-gssv-client","XboxComBrowser"},header};
    for(unsigned i=0;i<sizeof(paths)/sizeof(*paths);++i){
        char url[256];if(paths[i][0]=='/')snprintf(url,sizeof(url),"%s%s",origin,paths[i]);else snprintf(url,sizeof(url),"%s%s",session_prefix,paths[i]);
        for(unsigned method=0;method<3;++method){
            const char *body=method==X4_SESSION_HTTP_POST?(i==5?"":"{}"):NULL;
            CHECK(x4_http_session_request(h,(enum X4HttpSessionMethod)method,url,session_headers,3,body,response,sizeof(response),&length,&status,&cancel_flag)!=0);
        }
    }
    CHECK(!templates&&!forwarded_total);x4_http_close(h);
}
int main(void)
{
    const char *path=getenv("X4_SESSION_FIXTURE_SETTINGS_PATH");CHECK(path);x4_server_settings_set_path(path);
#if X4_STREAM_PROFILE
    runtime_profile=X4_REQUEST_720P60;
#endif
    for(unsigned country=0;country<X4_ACCESS_COUNTRY_COUNT;++country){
        for(unsigned kind=0;kind<3;++kind){catalog_routes((X4AccessCountry)country,false,kind);catalog_routes((X4AccessCountry)country,true,kind);}
        session_constructors((X4AccessCountry)country);
    }
    denied_headers();printf("PASS access-country production login/HTTP: %u checks; six countries, catalog/Fortnite/MRU/session constructors, same-country fallback, exact host+POST isolation, no live network\n",checks);
    return 0;
}
