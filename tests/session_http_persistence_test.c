/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the existing localhost-only TLS fixture and its ephemeral RAM keys. */
#define main tls_fixture_main
#define x4_secure_clear tls_fixture_secure_clear
#include "tls_conn_test.c"
#undef x4_secure_clear
#undef main

static unsigned server_port,connect_calls,close_calls;
static X4TlsConn *last_connection;
static atomic_bool close_idle_peer,peer_closed,partial_received;
static atomic_uint request_count;

uint64_t sceKernelGetProcessTime(void)
{
    return x4_tls_now_us();
}

X4TlsHttp *__wrap_x4_tls_http_open(int *error)
{
    const uint8_t *roots[]={fixture.ca.bytes};
    const size_t sizes[]={fixture.ca.length};
    return x4_tls_http_test_open(roots,sizes,1,error);
}

int __wrap_x4_tls_conn_connect(X4TlsHttp *h,const char *host,const X4TlsBudget *b,X4TlsConn **out)
{
    CHECK(!strcmp(host,"fixture-region.gssv-play-prod.xboxlive.com"));
    ++connect_calls;
    int rc=x4_tls_conn_test_connect(h,"localhost",server_port,b,out);
    if(!rc)last_connection=*out;
    return rc;
}

int __real_x4_tls_conn_close(X4TlsConn *);
int __wrap_x4_tls_conn_close(X4TlsConn *conn)
{
    if(conn)++close_calls;
    return __real_x4_tls_conn_close(conn);
}

int __real_x4_tls_conn_read(X4TlsConn *,void *,size_t,size_t *,const X4TlsBudget *);
int __wrap_x4_tls_conn_read(X4TlsConn *c,void *data,size_t size,size_t *got,const X4TlsBudget *b)
{
    int rc=__real_x4_tls_conn_read(c,data,size,got,b);
    if(!rc&&*got&&atomic_load(&request_count)==12)
        atomic_store(&partial_received,true);
    return rc;
}

static void wait_flag(const atomic_bool *flag)
{
    uint64_t deadline=monotonic_us()+UINT64_C(3000000);
    while(!atomic_load(flag)&&monotonic_us()<deadline)pause_ms(1);
    CHECK(atomic_load(flag));
}

static void serve_requests(Server *s,mbedtls_ssl_context *ssl,unsigned requests)
{
    const uint64_t deadline=monotonic_us()+UINT64_C(4000000);
    for(unsigned i=0;i<requests;++i){
        char request[4096];
        size_t used=0;
        request[0]='\0';
        while(!strstr(request,"\r\n\r\n")){
            int n=mbedtls_ssl_read(ssl,(unsigned char *)request+used,sizeof(request)-used-1);
            if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
                CHECK(server_wait(s,s->peer,n,deadline));
                continue;
            }
            CHECK(n>0);used+=(size_t)n;
            CHECK(used<sizeof(request)-1);request[used]='\0';
        }
        unsigned sequence=atomic_fetch_add(&request_count,1)+1;
        const char *expected=sequence<12?
            "GET /v5/sessions/cloud/FAKE_SESSION/state HTTP/1.1\r\n":
            "POST /v5/sessions/cloud/FAKE_SESSION/keepalive HTTP/1.1\r\n";
        CHECK(!strncmp(request,expected,strlen(expected)));
        CHECK(strstr(request,"\r\nConnection: keep-alive\r\n"));
        static const char full[]="HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\n{}";
        static const char partial[]="HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n{";
        const char *reply=sequence==12?partial:full;
        for(size_t sent=0,length=strlen(reply);sent<length;){
            int n=mbedtls_ssl_write(ssl,(const unsigned char *)reply+sent,length-sent);
            if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
                CHECK(server_wait(s,s->peer,n,deadline));
                continue;
            }
            CHECK(n>0);sent+=(size_t)n;
        }
    }
}

static void *serve_session(void *arg)
{
    Server *s=arg;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);
    mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&certificate);
    static const unsigned char purpose[]="XCloud4 T36 local session server";
    MBED_OK(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,purpose,sizeof(purpose)-1));
    MBED_OK(mbedtls_x509_crt_parse_der(&certificate,fixture.valid.bytes,fixture.valid.length));
    MBED_OK(mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
    mbedtls_ssl_conf_min_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);
    MBED_OK(mbedtls_ssl_conf_own_cert(&config,&certificate,&fixture.server_key));

    for(unsigned i=0;i<2;++i){
        const uint64_t deadline=monotonic_us()+UINT64_C(4000000);
        struct pollfd event={.fd=s->listener,.events=POLLIN};
        CHECK(poll(&event,1,4000)==1);
        s->peer=accept(s->listener,NULL,NULL);
        CHECK(s->peer>=0);
        CHECK(fcntl(s->peer,F_SETFL,fcntl(s->peer,F_GETFL,0)|O_NONBLOCK)==0);
        mbedtls_ssl_context ssl;
        mbedtls_ssl_init(&ssl);
        MBED_OK(mbedtls_ssl_setup(&ssl,&config));
        mbedtls_ssl_set_bio(&ssl,s,server_send,server_receive,NULL);
        for(;;){
            int rc=mbedtls_ssl_handshake(&ssl);
            if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE){
                CHECK(server_wait(s,s->peer,rc,deadline));
                continue;
            }
            MBED_OK(rc);
            break;
        }
        serve_requests(s,&ssl,i==0?10:2);
        if(i==0)wait_flag(&close_idle_peer);
        else{
            wait_flag(&partial_received);
            const struct linger reset={1,0};
            CHECK(setsockopt(s->peer,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset))==0);
        }
        CHECK(close(s->peer)==0);
        mbedtls_ssl_free(&ssl);
        if(i==0)atomic_store(&peer_closed,true);
    }
    CHECK(close(s->listener)==0);
    mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&certificate);
    mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
    return NULL;
}

int main(void)
{
    private_fixture_process();
    certificates_open();
    Server server={.peer=-1,.listener=socket(AF_INET,SOCK_STREAM,0)};
    CHECK(server.listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};
    CHECK(bind(server.listener,(struct sockaddr *)&address,sizeof(address))==0);
    socklen_t address_size=sizeof(address);
    CHECK(getsockname(server.listener,(struct sockaddr *)&address,&address_size)==0);
    server_port=ntohs(address.sin_port);
    CHECK(server_port&&listen(server.listener,1)==0);
    CHECK(pthread_create(&server.thread,NULL,serve_session,&server)==0);

    static const char state[]="https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/state";
    static const char keepalive[]="https://fixture-region.gssv-play-prod.xboxlive.com/v5/sessions/cloud/FAKE_SESSION/keepalive";
    const X4HttpHeader headers[]={{"Authorization","Bearer FAKE_TEST_ONLY"},
        {"x-gssv-client","XboxComBrowser"},{"X-MS-Device-Info","{\"fixture\":true}"}};
    int error=-1,status=0;
    size_t length=0;
    char body[256];
    X4Http *h=x4_http_open(&error);
    CHECK(h&&!error&&!x4_http_session_persistence(h,true));
    for(unsigned i=0;i<10;++i){
        CHECK(!x4_http_session_request(h,X4_SESSION_HTTP_GET,state,headers,3,NULL,
                                       body,sizeof(body),&length,&status,NULL));
        CHECK(status==200&&length==2&&!strcmp(body,"{}"));
        CHECK(connect_calls==1&&close_calls==0&&atomic_load(&request_count)==i+1);
    }
    atomic_store(&close_idle_peer,true);
    wait_flag(&peer_closed);
    const uint64_t deadline=monotonic_us()+UINT64_C(3000000);
    while(x4_tls_conn_idle_clean(last_connection)&&monotonic_us()<deadline)pause_ms(1);
    CHECK(!x4_tls_conn_idle_clean(last_connection));
    CHECK(!x4_http_session_request(h,X4_SESSION_HTTP_GET,state,headers,3,NULL,
                                   body,sizeof(body),&length,&status,NULL));
    CHECK(status==200&&length==2&&connect_calls==2&&close_calls==1&&atomic_load(&request_count)==11);
    CHECK(x4_http_session_request(h,X4_SESSION_HTTP_POST,keepalive,headers,3,"",
                                  body,sizeof(body),&length,&status,NULL)<0);
    CHECK(atomic_load(&request_count)==12&&atomic_load(&partial_received));
    CHECK(connect_calls==2&&close_calls==2&&!length&&!status&&!body[0]);
    CHECK(!x4_http_session_persistence(h,false));
    x4_http_close(h);
    CHECK(pthread_join(server.thread,NULL)==0);
    certificates_close();
    puts("PASS session HTTP localhost: 10 state / 1 socket; stale FIN replaced before write; POST partial reply not retried");
    return 0;
}
