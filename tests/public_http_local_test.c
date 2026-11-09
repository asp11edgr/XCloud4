/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the TLS localhost fixture. Its ephemeral keys never leave RAM. */
#define main tls_fixture_main
#define x4_secure_clear tls_fixture_secure_clear
#include "tls_conn_test.c"
#undef x4_secure_clear
#undef main

enum { CANCEL_LANE, RATE_LANE, CLOSE_LANE, LANES };
#define STORE_HOST "store-images.s-microsoft.com"
#define IMAGE_QUERY "?w=384&h=216&q=85&format=jpg"
#define IMAGE_PREFIX "https://" STORE_HOST "/image/apps."
static const char *const ids[LANES][2]={
    {"cancel","aftercancel"}, {"rate","afterrate"}, {"close","afterclose"}
};

typedef struct {
    Server server;
    pthread_t client_thread;
    unsigned index, connects, accepted;
    bool first;
} Lane;
static Lane lanes[LANES];
static _Thread_local Lane *client_lane;
static pthread_mutex_t handshake_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint first_requests,requests;
static atomic_bool release_responses,partial_received,cancel_returned;
static _Atomic int cancel_image;

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
int __wrap_x4_tls_conn_connect(X4TlsHttp *h,const char *host,const X4TlsBudget *budget,X4TlsConn **out)
{
    CHECK(client_lane&&!strcmp(host,STORE_HOST));
    ++client_lane->connects;
    return x4_tls_conn_test_connect(h,"localhost",client_lane->server.port,budget,out);
}
int __real_x4_tls_conn_read(X4TlsConn *,void *,size_t,size_t *,const X4TlsBudget *);
int __wrap_x4_tls_conn_read(X4TlsConn *c,void *data,size_t size,size_t *got,const X4TlsBudget *budget)
{
    int rc=__real_x4_tls_conn_read(c,data,size,got,budget);
    if(!rc&&*got&&client_lane&&client_lane->index==CANCEL_LANE&&client_lane->first)
        atomic_store(&partial_received,true);
    return rc;
}
static void wait_public_flag(const atomic_bool *flag)
{
    uint64_t deadline=monotonic_us()+UINT64_C(6000000);
    while(!atomic_load(flag)&&monotonic_us()<deadline)pause_ms(1);
    CHECK(atomic_load(flag));
}

static void request_from_peer(Lane *lane,mbedtls_ssl_context *ssl,unsigned index)
{
    Server *s=&lane->server;
    uint64_t deadline=monotonic_us()+UINT64_C(6000000);
    char request[1024]="";
    size_t used=0;
    while(!strstr(request,"\r\n\r\n")){
        int n=mbedtls_ssl_read(ssl,(unsigned char *)request+used,sizeof(request)-used-1);
        if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
            CHECK(server_wait(s,s->peer,n,deadline));
            continue;
        }
        CHECK(n>0);used+=(size_t)n;
        CHECK(used<sizeof(request)-1);request[used]='\0';
    }
    char expected[128];
    int count=snprintf(expected,sizeof(expected),"GET /image/apps.%s" IMAGE_QUERY " HTTP/1.1\r\n",
        ids[lane->index][index]);
    CHECK(count>0&&(size_t)count<sizeof(expected));
    CHECK(!strncmp(request,expected,(size_t)count));
    CHECK(strstr(request,"\r\nHost: " STORE_HOST "\r\n"));
    CHECK(!strstr(request,"\r\nAuthorization:"));
    atomic_fetch_add(&requests,1);
    if(!index){
        atomic_fetch_add(&first_requests,1);
        wait_public_flag(&release_responses);
    }
}
static void send_public_response(Server *s,mbedtls_ssl_context *ssl,const char *response)
{
    uint64_t deadline=monotonic_us()+UINT64_C(6000000);
    for(size_t sent=0,length=strlen(response);sent<length;){
        int n=mbedtls_ssl_write(ssl,(const unsigned char *)response+sent,length-sent);
        if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
            CHECK(server_wait(s,s->peer,n,deadline));
            continue;
        }
        CHECK(n>0);sent+=(size_t)n;
    }
}
static void accept_public_peer(Lane *lane,mbedtls_ssl_context *ssl,mbedtls_ssl_config *config)
{
    Server *s=&lane->server;
    struct pollfd event={.fd=s->listener,.events=POLLIN};
    CHECK(poll(&event,1,6000)==1);
    s->peer=accept(s->listener,NULL,NULL);
    CHECK(s->peer>=0);
    ++lane->accepted;
    CHECK(fcntl(s->peer,F_SETFL,fcntl(s->peer,F_GETFL,0)|O_NONBLOCK)==0);
    mbedtls_ssl_init(ssl);
    MBED_OK(mbedtls_ssl_setup(ssl,config));
    mbedtls_ssl_set_bio(ssl,s,server_send,server_receive,NULL);

    /* The borrowed RSA key is shared by the fixture. Serialize handshakes
     * while still holding all three established HTTP requests concurrently. */
    CHECK(pthread_mutex_lock(&handshake_lock)==0);
    uint64_t deadline=monotonic_us()+UINT64_C(6000000);
    for(;;){
        int rc=mbedtls_ssl_handshake(ssl);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE){
            CHECK(server_wait(s,s->peer,rc,deadline));
            continue;
        }
        MBED_OK(rc);
        break;
    }
    CHECK(pthread_mutex_unlock(&handshake_lock)==0);
}
static void close_public_peer(Server *s,mbedtls_ssl_context *ssl)
{
    CHECK(close(s->peer)==0);
    s->peer=-1;
    mbedtls_ssl_free(ssl);
}
static void *serve_public(void *arg)
{
    Lane *lane=arg;
    Server *s=&lane->server;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);
    mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&certificate);
    static const unsigned char purpose[]="XCloud4 T36 public HTTP loopback fixture";
    MBED_OK(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,purpose,sizeof(purpose)-1));
    MBED_OK(mbedtls_x509_crt_parse_der(&certificate,fixture.valid.bytes,fixture.valid.length));
    MBED_OK(mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
    mbedtls_ssl_conf_min_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&config,MBEDTLS_SSL_VERSION_TLS1_2);
    MBED_OK(mbedtls_ssl_conf_own_cert(&config,&certificate,&fixture.server_key));

    mbedtls_ssl_context ssl;
    accept_public_peer(lane,&ssl,&config);
    request_from_peer(lane,&ssl,0);
    switch(lane->index){
    case CANCEL_LANE:
        send_public_response(s,&ssl,"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: keep-alive\r\n\r\npartial");
        wait_public_flag(&cancel_returned);
        close_public_peer(s,&ssl);
        accept_public_peer(lane,&ssl,&config);
        break;
    case RATE_LANE:
        send_public_response(s,&ssl,"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                    "Connection: keep-alive\r\nRetry-After: 2\r\n\r\n");
        break;
    case CLOSE_LANE:
        send_public_response(s,&ssl,"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nJPEG");
        close_public_peer(s,&ssl);
        accept_public_peer(lane,&ssl,&config);
        break;
    }
    request_from_peer(lane,&ssl,1);
    send_public_response(s,&ssl,"HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nJPEG");
    close_public_peer(s,&ssl);
    CHECK(close(s->listener)==0);
    mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&certificate);
    mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
    return NULL;
}
static int request_public_image(Lane *lane,X4Http *http,unsigned index,
    unsigned char *body,size_t capacity,size_t *length,int *status)
{
    char url[128];
    int count=snprintf(url,sizeof(url),IMAGE_PREFIX "%s" IMAGE_QUERY,ids[lane->index][index]);
    CHECK(count>0&&(size_t)count<sizeof(url));
    return x4_http_public_image_request(http,url,body,capacity,length,status,
        lane->index==CANCEL_LANE&&!index?&cancel_image:NULL);
}
static void *fetch_public(void *arg)
{
    Lane *lane=arg;
    client_lane=lane;
    int error=-1,status=-1;
    unsigned char body[128];
    size_t length=0;
    X4Http *http=x4_http_open(&error);
    CHECK(http&&!error);
    lane->first=true;
    int rc=request_public_image(lane,http,0,body,sizeof(body),&length,&status);
    lane->first=false;
    CHECK(lane->connects==1);
    switch(lane->index){
    case CANCEL_LANE:
        CHECK(rc==X4_HTTP_CANCELLED&&atomic_load(&partial_received));
        CHECK(!length&&!status&&!body[0]);
        atomic_store(&cancel_returned,true);
        break;
    case RATE_LANE:
        CHECK(!rc&&status==503&&!length);
        CHECK(x4_http_public_retry_after_ms(http)==2000);
        /* The caller owns the cooldown; only the parsed value comes from HTTP. */
        pause_ms(2000);
        break;
    case CLOSE_LANE:
        CHECK(!rc&&status==200&&length==4&&!memcmp(body,"JPEG",4));
        break;
    }
    CHECK(!request_public_image(lane,http,1,body,sizeof(body),&length,&status));
    CHECK(status==200&&length==4&&!memcmp(body,"JPEG",4));
    CHECK(lane->connects==(lane->index==RATE_LANE?1u:2u));
    CHECK(!x4_http_public_retry_after_ms(http));
    CHECK(!x4_http_public_reset(http));
    x4_http_close(http);
    return NULL;
}

int main(void)
{
    private_fixture_process();
    certificates_open();
    for(unsigned i=0;i<LANES;++i){
        Lane *lane=&lanes[i];
        lane->index=i;
        Server *s=&lane->server;
        s->peer=-1;s->listener=socket(AF_INET,SOCK_STREAM,0);
        CHECK(s->listener>=0);
        struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};
        CHECK(bind(s->listener,(struct sockaddr *)&address,sizeof(address))==0);
        socklen_t size=sizeof(address);
        CHECK(getsockname(s->listener,(struct sockaddr *)&address,&size)==0);
        s->port=ntohs(address.sin_port);
        CHECK(s->port&&listen(s->listener,1)==0);
        CHECK(pthread_create(&s->thread,NULL,serve_public,lane)==0);
        CHECK(pthread_create(&lane->client_thread,NULL,fetch_public,lane)==0);
    }
    uint64_t deadline=monotonic_us()+UINT64_C(6000000);
    while(atomic_load(&first_requests)<LANES&&monotonic_us()<deadline)pause_ms(1);
    CHECK(atomic_load(&first_requests)==LANES&&atomic_load(&requests)==LANES);
    atomic_store(&release_responses,true);
    wait_public_flag(&partial_received);
    atomic_store(&cancel_image,1);
    unsigned connections=0;
    for(unsigned i=0;i<LANES;++i){
        Lane *lane=&lanes[i];
        CHECK(pthread_join(lane->client_thread,NULL)==0);
        CHECK(pthread_join(lane->server.thread,NULL)==0);
        CHECK(lane->accepted==lane->connects);
        connections+=lane->accepted;
    }
    CHECK(connections==5&&atomic_load(&requests)==6);
    certificates_close();
    puts("PASS public HTTP localhost: 3 simultaneous GETs, 6 requests / 5 sockets; "
         "cancel after partial body, Retry-After 2000 ms, close and reopen");
    return 0;
}
