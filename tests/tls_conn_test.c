/* SPDX-License-Identifier: GPL-3.0-only */
/* Localhost-only TLS fixture. Private test keys are generated and retained
 * solely in Mbed TLS RAM contexts: never serialized, written, or printed. */
#define _POSIX_C_SOURCE 200809L
#include "../src/auth/tls_conn.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#ifndef X4_HTTP_TEST
#error Test-only CA/port/version injection is required
#endif

static _Atomic unsigned checks;
static bool reject_sigpipe_setup;
static unsigned sigpipe_setup_rejections;
int __real_sigaction(int signum,const struct sigaction *action,struct sigaction *old);
int __wrap_sigaction(int signum,const struct sigaction *action,struct sigaction *old)
{
    if(reject_sigpipe_setup && signum==SIGPIPE && action && action->sa_handler==SIG_IGN){
        ++sigpipe_setup_rejections;errno=ENOSYS;return -1;
    }
    return __real_sigaction(signum,action,old);
}
#define CHECK(condition) do { ++checks; if(!(condition)) { \
    fprintf(stderr,"TLS fixture check failed line=%d: %s\n",__LINE__,#condition); abort(); \
} } while(0)
static void mbed_ok(int result,unsigned line)
{
    if(result){fprintf(stderr,"TLS fixture setup failed line=%u code=%d\n",line,result);abort();}
}
#define MBED_OK(expression) mbed_ok((expression),__LINE__)

/* tls_conn deliberately depends only on the existing clear contract. */
void x4_secure_clear(void *memory,size_t length)
{
    volatile unsigned char *bytes=memory;
    while(length--)*bytes++=0;
    atomic_signal_fence(memory_order_seq_cst);
}
static uint64_t monotonic_us(void)
{
    struct timespec value;CHECK(clock_gettime(CLOCK_MONOTONIC,&value)==0);
    return (uint64_t)value.tv_sec*UINT64_C(1000000)+(uint64_t)value.tv_nsec/1000;
}
static void private_fixture_process(void)
{
    /* Linux core_pattern may be a pipe, so RLIMIT_CORE alone is not enough.
     * Disable dumps of this process before generating ephemeral test keys. */
    CHECK(prctl(PR_SET_DUMPABLE,0,0,0,0)==0);
}
static void pause_ms(unsigned ms)
{
    struct timespec value={(time_t)(ms/1000),(long)(ms%1000)*1000000};
    while(nanosleep(&value,&value)<0&&errno==EINTR){}
}

enum { DER_CAPACITY=4096 };
typedef struct { unsigned char bytes[DER_CAPACITY];size_t length; } Certificate;
static struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context ca_key,server_key,upper_key,lower_key;
    Certificate ca,valid,wrong_name,expired;
    Certificate chain_root,upper,lower,chain_valid,chain_wrong_name,chain_expired,chain_bad_usage,chain_bad_eku;
} fixture;

static void make_chain_certificate(Certificate *out,const char *subject,const char *issuer,
    mbedtls_pk_context *subject_key,mbedtls_pk_context *issuer_key,int path_length,
    bool expired,unsigned serial,unsigned bad_usage)
{
    mbedtls_x509write_cert writer;mbedtls_mpi number;
    mbedtls_x509write_crt_init(&writer);mbedtls_mpi_init(&number);
    mbedtls_x509write_crt_set_version(&writer,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer,subject_key);mbedtls_x509write_crt_set_issuer_key(&writer,issuer_key);
    MBED_OK(mbedtls_x509write_crt_set_subject_name(&writer,subject));
    MBED_OK(mbedtls_x509write_crt_set_issuer_name(&writer,issuer));
    MBED_OK(mbedtls_mpi_lset(&number,(mbedtls_mpi_sint)serial));
    MBED_OK(mbedtls_x509write_crt_set_serial(&writer,&number));
    MBED_OK(mbedtls_x509write_crt_set_validity(&writer,"20000101000000",expired?"20010101000000":"20491231235959"));
    bool ca=path_length>=0;
    MBED_OK(mbedtls_x509write_crt_set_basic_constraints(&writer,ca,ca?path_length:-1));
    unsigned usage=ca?MBEDTLS_X509_KU_KEY_CERT_SIGN|MBEDTLS_X509_KU_CRL_SIGN:
        bad_usage==1?MBEDTLS_X509_KU_KEY_ENCIPHERMENT:
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE|MBEDTLS_X509_KU_KEY_ENCIPHERMENT;
    MBED_OK(mbedtls_x509write_crt_set_key_usage(&writer,usage));
    if(bad_usage==2){
        /* DER SEQUENCE { OID id-kp-clientAuth }; never serverAuth. */
        static const unsigned char client_auth[]={0x30,0x0a,0x06,0x08,0x2b,0x06,0x01,0x05,0x05,0x07,0x03,0x02};
        MBED_OK(mbedtls_x509write_crt_set_extension(&writer,MBEDTLS_OID_EXTENDED_KEY_USAGE,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE),0,client_auth,sizeof(client_auth)));
    }
    int written=mbedtls_x509write_crt_der(&writer,out->bytes,sizeof(out->bytes),mbedtls_ctr_drbg_random,&fixture.rng);
    CHECK(written>0&&(size_t)written<=sizeof(out->bytes));out->length=(size_t)written;
    memmove(out->bytes,out->bytes+sizeof(out->bytes)-out->length,out->length);
    mbedtls_mpi_free(&number);mbedtls_x509write_crt_free(&writer);
}

static void make_certificate(Certificate *out,const char *subject,bool ca,
                             bool expired,unsigned serial)
{
    mbedtls_x509write_cert writer;mbedtls_mpi number;
    mbedtls_x509write_crt_init(&writer);mbedtls_mpi_init(&number);
    mbedtls_x509write_crt_set_version(&writer,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer,ca?&fixture.ca_key:&fixture.server_key);
    mbedtls_x509write_crt_set_issuer_key(&writer,&fixture.ca_key);
    MBED_OK(mbedtls_x509write_crt_set_subject_name(&writer,subject));
    MBED_OK(mbedtls_x509write_crt_set_issuer_name(&writer,"CN=XCloud4 local test CA"));
    MBED_OK(mbedtls_mpi_lset(&number,(mbedtls_mpi_sint)serial));
    MBED_OK(mbedtls_x509write_crt_set_serial(&writer,&number));
    MBED_OK(mbedtls_x509write_crt_set_validity(&writer,"20000101000000",
        expired?"20010101000000":"20491231235959"));
    MBED_OK(mbedtls_x509write_crt_set_basic_constraints(&writer,ca,ca?0:-1));
    MBED_OK(mbedtls_x509write_crt_set_key_usage(&writer,ca?
        MBEDTLS_X509_KU_KEY_CERT_SIGN|MBEDTLS_X509_KU_CRL_SIGN:
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE|MBEDTLS_X509_KU_KEY_ENCIPHERMENT));
    int written=mbedtls_x509write_crt_der(&writer,out->bytes,sizeof(out->bytes),
                                        mbedtls_ctr_drbg_random,&fixture.rng);
    CHECK(written>0&&(size_t)written<=sizeof(out->bytes));
    out->length=(size_t)written;
    memmove(out->bytes,out->bytes+sizeof(out->bytes)-out->length,out->length);
    mbedtls_mpi_free(&number);mbedtls_x509write_crt_free(&writer);
}
static void certificates_open(void)
{
    CHECK(psa_crypto_init()==PSA_SUCCESS);
    mbedtls_entropy_init(&fixture.entropy);mbedtls_ctr_drbg_init(&fixture.rng);
    mbedtls_pk_init(&fixture.ca_key);mbedtls_pk_init(&fixture.server_key);
    mbedtls_pk_init(&fixture.upper_key);mbedtls_pk_init(&fixture.lower_key);
    static const unsigned char purpose[]="XCloud4 T31 in-memory local fixture";
    MBED_OK(mbedtls_ctr_drbg_seed(&fixture.rng,mbedtls_entropy_func,&fixture.entropy,
                                purpose,sizeof(purpose)-1));
    MBED_OK(mbedtls_pk_setup(&fixture.ca_key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)));
    MBED_OK(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(fixture.ca_key),
                               mbedtls_ctr_drbg_random,&fixture.rng));
    MBED_OK(mbedtls_pk_setup(&fixture.server_key,mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)));
    MBED_OK(mbedtls_rsa_gen_key(mbedtls_pk_rsa(fixture.server_key),
                               mbedtls_ctr_drbg_random,&fixture.rng,2048,65537));
    make_certificate(&fixture.ca,"CN=XCloud4 local test CA",true,false,1);
    make_certificate(&fixture.valid,"CN=localhost",false,false,2);
    make_certificate(&fixture.wrong_name,"CN=wrong-name.invalid",false,false,3);
    make_certificate(&fixture.expired,"CN=localhost",false,true,4);
    MBED_OK(mbedtls_pk_setup(&fixture.upper_key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)));
    MBED_OK(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(fixture.upper_key),mbedtls_ctr_drbg_random,&fixture.rng));
    MBED_OK(mbedtls_pk_setup(&fixture.lower_key,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)));
    MBED_OK(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(fixture.lower_key),mbedtls_ctr_drbg_random,&fixture.rng));
    make_chain_certificate(&fixture.chain_root,"CN=XCloud4 chain root","CN=XCloud4 chain root",&fixture.ca_key,&fixture.ca_key,2,false,10,0);
    make_chain_certificate(&fixture.upper,"CN=XCloud4 upper intermediate","CN=XCloud4 chain root",&fixture.upper_key,&fixture.ca_key,1,false,11,0);
    make_chain_certificate(&fixture.lower,"CN=XCloud4 lower intermediate","CN=XCloud4 upper intermediate",&fixture.lower_key,&fixture.upper_key,0,false,12,0);
    make_chain_certificate(&fixture.chain_valid,"CN=localhost","CN=XCloud4 lower intermediate",&fixture.server_key,&fixture.lower_key,-1,false,13,0);
    make_chain_certificate(&fixture.chain_wrong_name,"CN=wrong-name.invalid","CN=XCloud4 lower intermediate",&fixture.server_key,&fixture.lower_key,-1,false,14,0);
    make_chain_certificate(&fixture.chain_expired,"CN=localhost","CN=XCloud4 lower intermediate",&fixture.server_key,&fixture.lower_key,-1,true,15,0);
    make_chain_certificate(&fixture.chain_bad_usage,"CN=localhost","CN=XCloud4 lower intermediate",&fixture.server_key,&fixture.lower_key,-1,false,16,1);
    make_chain_certificate(&fixture.chain_bad_eku,"CN=localhost","CN=XCloud4 lower intermediate",&fixture.server_key,&fixture.lower_key,-1,false,17,2);
}
static void certificates_close(void)
{
    mbedtls_pk_free(&fixture.server_key);mbedtls_pk_free(&fixture.ca_key);
    mbedtls_pk_free(&fixture.upper_key);mbedtls_pk_free(&fixture.lower_key);
    mbedtls_ctr_drbg_free(&fixture.rng);mbedtls_entropy_free(&fixture.entropy);
    x4_secure_clear(&fixture,sizeof(fixture));
}

enum ServerMode { SERVER_NORMAL,SERVER_STALL_HANDSHAKE,SERVER_STALL_READ,SERVER_RESET_BODY,SERVER_CBC };
typedef struct {
    pthread_t thread;
    int listener,peer,version,handshake_result;
    unsigned port;
    enum ServerMode mode;
    const Certificate *certificate;
    const Certificate *chain[3];size_t chain_count;
    atomic_bool stop,accepted,handshaken,reset_allowed;
} Server;
static int server_send(void *context,const unsigned char *bytes,size_t count)
{
    Server *server=context;
    ssize_t sent=send(server->peer,bytes,count,MSG_NOSIGNAL);
    if(sent>=0)return (int)sent;
    if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int server_receive(void *context,unsigned char *bytes,size_t count)
{
    Server *server=context;
    ssize_t got=recv(server->peer,bytes,count,0);
    if(got>=0)return (int)got;
    if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
static bool server_wait(Server *server,int fd,int result,uint64_t deadline)
{
    if(atomic_load(&server->stop)||monotonic_us()>=deadline)return false;
    struct pollfd event={.fd=fd,.events=result==MBEDTLS_ERR_SSL_WANT_WRITE?POLLOUT:POLLIN};
    int rc=poll(&event,1,20);
    return rc>=0||errno==EINTR;
}
static void *server_main(void *context)
{
    Server *server=context;
    mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;
    mbedtls_ssl_config config;mbedtls_ssl_context ssl;mbedtls_x509_crt certificate;
    mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);
    mbedtls_ssl_config_init(&config);mbedtls_ssl_init(&ssl);mbedtls_x509_crt_init(&certificate);
    uint64_t deadline=monotonic_us()+UINT64_C(5000000);
    while(server_wait(server,server->listener,MBEDTLS_ERR_SSL_WANT_READ,deadline)){
        server->peer=accept(server->listener,NULL,NULL);
        if(server->peer>=0)break;
        if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)goto done;
    }
    if(server->peer<0)goto done;
    CHECK(fcntl(server->peer,F_SETFL,fcntl(server->peer,F_GETFL,0)|O_NONBLOCK)==0);
    atomic_store(&server->accepted,true);
    if(server->mode==SERVER_STALL_HANDSHAKE){
        while(server_wait(server,server->peer,MBEDTLS_ERR_SSL_WANT_READ,deadline))pause_ms(5);
        goto done;
    }
    static const unsigned char purpose[]="XCloud4 T31 localhost server";
    MBED_OK(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,purpose,sizeof(purpose)-1));
    MBED_OK(mbedtls_x509_crt_parse_der(&certificate,server->certificate->bytes,server->certificate->length));
    for(size_t i=0;i<server->chain_count;++i)
        MBED_OK(mbedtls_x509_crt_parse_der(&certificate,server->chain[i]->bytes,server->chain[i]->length));
    /* Deliberately noncompliant peer fixture: Mbed TLS otherwise rejects its
     * own bad-usage leaf during server certificate selection, before sending
     * it. Change ONLY the server's parsed selection metadata. The original
     * signed DER is unchanged and the production client must parse/reject
     * its genuine KU/EKU after any issuer-order repair. */
    if(server->certificate==&fixture.chain_bad_usage)
        certificate.MBEDTLS_PRIVATE(key_usage)|=MBEDTLS_X509_KU_DIGITAL_SIGNATURE;
    if(server->certificate==&fixture.chain_bad_eku)
        certificate.MBEDTLS_PRIVATE(ext_types)&=~MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE;
    CHECK(certificate.raw.len==server->certificate->length&&
        !memcmp(certificate.raw.p,server->certificate->bytes,certificate.raw.len));
    MBED_OK(mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_SERVER,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_rng(&config,mbedtls_ctr_drbg_random,&rng);
    mbedtls_ssl_conf_min_tls_version(&config,(mbedtls_ssl_protocol_version)server->version);
    mbedtls_ssl_conf_max_tls_version(&config,(mbedtls_ssl_protocol_version)server->version);
    mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_NONE); /* No client certificate requested. */
    MBED_OK(mbedtls_ssl_conf_own_cert(&config,&certificate,&fixture.server_key));
    static const int modern12[]={MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,0};
    static const int cbc12[]={MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA,0};
    if(server->version==MBEDTLS_SSL_VERSION_TLS1_2)
        mbedtls_ssl_conf_ciphersuites(&config,server->mode==SERVER_CBC?cbc12:modern12);
    MBED_OK(mbedtls_ssl_setup(&ssl,&config));
    mbedtls_ssl_set_bio(&ssl,server,server_send,server_receive,NULL);
    int result;
    do{
        result=mbedtls_ssl_handshake(&ssl);
        if(result!=MBEDTLS_ERR_SSL_WANT_READ&&result!=MBEDTLS_ERR_SSL_WANT_WRITE)break;
    }while(server_wait(server,server->peer,result,deadline));
    server->handshake_result=result;
    if(result)goto done;
    atomic_store(&server->handshaken,true);
    if(server->mode==SERVER_RESET_BODY){
        static const unsigned char partial[]="HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial";
        size_t offset=0;
        while(offset<sizeof(partial)-1&&!atomic_load(&server->stop)){
            result=mbedtls_ssl_write(&ssl,partial+offset,sizeof(partial)-1-offset);
            if(result>0){offset+=(size_t)result;continue;}
            if((result!=MBEDTLS_ERR_SSL_WANT_READ&&result!=MBEDTLS_ERR_SSL_WANT_WRITE)||
               !server_wait(server,server->peer,result,deadline))goto done;
        }
        while(!atomic_load(&server->reset_allowed)&&!atomic_load(&server->stop)&&monotonic_us()<deadline)pause_ms(5);
        if(atomic_load(&server->reset_allowed)){
            struct linger reset={1,0};CHECK(setsockopt(server->peer,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset))==0);
            close(server->peer);server->peer=-1;goto done;
        }
    }
    while(!atomic_load(&server->stop)&&monotonic_us()<deadline)pause_ms(5);
done:
    if(server->peer>=0)close(server->peer);
    server->peer=-1;close(server->listener);server->listener=-1;
    mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);
    mbedtls_x509_crt_free(&certificate);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
    return NULL;
}
static void server_open_chain(Server *server,const Certificate *certificate,int version,enum ServerMode mode,
                              const Certificate *const *chain,size_t chain_count)
{
    *server=(Server){.listener=-1,.peer=-1,.version=version,.mode=mode,.certificate=certificate};
    CHECK(chain_count<=3);server->chain_count=chain_count;
    for(size_t i=0;i<chain_count;++i)server->chain[i]=chain[i];
    atomic_init(&server->stop,false);atomic_init(&server->accepted,false);
    atomic_init(&server->handshaken,false);atomic_init(&server->reset_allowed,false);
    server->listener=socket(AF_INET,SOCK_STREAM,0);CHECK(server->listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0};
    CHECK(bind(server->listener,(struct sockaddr *)&address,sizeof(address))==0);
    socklen_t length=sizeof(address);CHECK(getsockname(server->listener,(struct sockaddr *)&address,&length)==0);
    server->port=ntohs(address.sin_port);CHECK(server->port!=0);
    CHECK(fcntl(server->listener,F_SETFL,fcntl(server->listener,F_GETFL,0)|O_NONBLOCK)==0);
    CHECK(listen(server->listener,1)==0);CHECK(pthread_create(&server->thread,NULL,server_main,server)==0);
}
static void server_open(Server *server,const Certificate *certificate,int version,enum ServerMode mode)
{server_open_chain(server,certificate,version,mode,NULL,0);}
static void server_close(Server *server)
{
    atomic_store(&server->stop,true);CHECK(pthread_join(server->thread,NULL)==0);
}
static X4TlsHttp *client_open(bool trust_test_ca)
{
    int error=-1;X4TlsHttp *http;
    if(trust_test_ca){
        const uint8_t *roots[]={fixture.ca.bytes};size_t sizes[]={fixture.ca.length};
        http=x4_tls_http_test_open(roots,sizes,1,&error);
    }else http=x4_tls_http_open(&error);
    CHECK(http&&error==0);return http;
}
static X4TlsBudget budget_after(uint64_t interval,const _Atomic int *cancel)
{
    return (X4TlsBudget){.deadline_us=x4_tls_now_us()+interval,.phase_deadline_us=0,.cancel=cancel};
}
static void connect_case(const char *label,const Certificate *certificate,int version,
                         bool trust_test_ca,enum ServerMode mode,int expected)
{
    Server server;server_open(&server,certificate,version,mode);
    X4TlsHttp *http=client_open(trust_test_ca);X4TlsConn *connection=NULL;
    CHECK(x4_tls_http_test_versions(http,version==MBEDTLS_SSL_VERSION_TLS1_2,
                                   version==MBEDTLS_SSL_VERSION_TLS1_3,false)==0);
    _Atomic(const char *) stage="fixture start";
    X4TlsBudget budget=budget_after(UINT64_C(4000000),NULL);budget.stage=&stage;
    int result=x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection);
    CHECK(result==expected);
    X4TlsInfo info=x4_tls_http_info(http);
    if(!expected){
        CHECK(connection);info=x4_tls_conn_info(connection);CHECK(info.verified&&info.verify_flags==0);
        CHECK(strstr(info.version,version==MBEDTLS_SSL_VERSION_TLS1_2?"1.2":"1.3")!=NULL);
        CHECK(strstr(info.suite,"GCM")||strstr(info.suite,"CHACHA20"));
        CHECK(x4_tls_http_close(http)==X4_HTTP_BUSY);
        x4_tls_conn_mark_reused(connection);CHECK(x4_tls_conn_info(connection).reused);
    }else{
        CHECK(info.error==expected&&info.stage);
        const char *stage=expected==X4_HTTP_TLS_CERTIFICATE_TIME?
            "certificado TLS: revisa fecha y hora":expected==X4_HTTP_TLS_CERTIFICATE?
            "certificado TLS":"negociacion TLS";
        CHECK(strcmp(info.stage,stage)==0);
        if(expected==X4_HTTP_TLS_CERTIFICATE||expected==X4_HTTP_TLS_CERTIFICATE_TIME)CHECK(info.verify_flags!=0);
    }
    CHECK(!strcmp(atomic_load_explicit(&stage,memory_order_acquire),info.stage));
    if(connection)CHECK(x4_tls_conn_close(connection)==0);
    CHECK(x4_tls_http_close(http)==0);server_close(&server);
    printf("PASS tls local %s\n",label);
}
typedef struct { _Atomic int *cancel;Server *server; } Cancel;
static void *cancel_later(void *context)
{
    Cancel *cancel=context;uint64_t deadline=monotonic_us()+UINT64_C(2000000);
    while(!atomic_load(&cancel->server->accepted)&&monotonic_us()<deadline)pause_ms(1);
    pause_ms(40);atomic_store(cancel->cancel,1);return NULL;
}
static void cancellation_case(bool handshake)
{
    Server server;server_open(&server,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,
                              handshake?SERVER_STALL_HANDSHAKE:SERVER_STALL_READ);
    X4TlsHttp *http=client_open(true);X4TlsConn *connection=NULL;
    _Atomic int cancel=0;X4TlsBudget budget=budget_after(UINT64_C(3000000),&cancel);
    if(!handshake)CHECK(x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection)==0);
    Cancel trigger={&cancel,&server};pthread_t thread;CHECK(pthread_create(&thread,NULL,cancel_later,&trigger)==0);
    uint64_t start=monotonic_us();int result;
    if(handshake)result=x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection);
    else{unsigned char buffer[64];size_t received=99;result=x4_tls_conn_read(connection,buffer,sizeof(buffer),&received,&budget);CHECK(received==0);}
    uint64_t elapsed=monotonic_us()-start;
    CHECK(result==X4_HTTP_CANCELLED&&elapsed<=250000);
    CHECK(pthread_join(thread,NULL)==0);
    if(connection)CHECK(x4_tls_conn_close(connection)==0);
    CHECK(x4_tls_http_close(http)==0);server_close(&server);
    printf("PASS tls cancel_%s elapsed_us=%llu\n",handshake?"handshake":"read",(unsigned long long)elapsed);
}
static void *cancel_connect_later(void *opaque)
{
    pause_ms(40);atomic_store((_Atomic int *)opaque,1);return NULL;
}
static void cancel_pending_connect(void)
{
    /* Linux localhost fixture: fill a backlog-zero listener's single accept
     * slot. The following nonblocking connect cannot finish until accept,
     * which this fixture deliberately never calls. No external destination. */
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(bind(listener,(struct sockaddr *)&address,sizeof(address))==0);
    socklen_t size=sizeof(address);CHECK(getsockname(listener,(struct sockaddr *)&address,&size)==0);
    CHECK(listen(listener,0)==0);
    int filler=socket(AF_INET,SOCK_STREAM,0);CHECK(filler>=0);
    CHECK(connect(filler,(struct sockaddr *)&address,sizeof(address))==0);
    X4TlsHttp *http=client_open(true);X4TlsConn *connection=NULL;_Atomic int cancel=0;
    X4TlsBudget budget=budget_after(UINT64_C(3000000),&cancel);pthread_t trigger;
    CHECK(pthread_create(&trigger,NULL,cancel_connect_later,&cancel)==0);
    uint64_t start=monotonic_us();
    int rc=x4_tls_conn_test_connect(http,"127.0.0.1",ntohs(address.sin_port),&budget,&connection);
    uint64_t elapsed=monotonic_us()-start;
    CHECK(rc==X4_HTTP_CANCELLED&&elapsed<=250000&&!connection);
    CHECK(!strcmp(x4_tls_http_info(http).stage,"conexion HTTPS"));
    CHECK(pthread_join(trigger,NULL)==0);CHECK(x4_tls_http_close(http)==0);
    CHECK(close(filler)==0&&close(listener)==0);
    printf("PASS tls cancel_pending_TCP_connect elapsed_us=%llu\n",(unsigned long long)elapsed);
}
static void deadline_case(uint64_t interval,const char *label)
{
    Server server;server_open(&server,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,SERVER_STALL_READ);
    X4TlsHttp *http=client_open(true);X4TlsConn *connection=NULL;
    X4TlsBudget budget=budget_after(UINT64_C(3000000),NULL);
    CHECK(x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection)==0);
    budget=budget_after(interval,NULL);unsigned char buffer[64];size_t received=99;
    uint64_t start=monotonic_us();int result=x4_tls_conn_read(connection,buffer,sizeof(buffer),&received,&budget);
    uint64_t elapsed=monotonic_us()-start;
    CHECK(result==X4_HTTP_DEADLINE&&received==0);
    CHECK(elapsed>=interval-20000&&elapsed<=interval+250000);
    CHECK(x4_tls_conn_close(connection)==0);CHECK(x4_tls_http_close(http)==0);server_close(&server);
    printf("PASS tls %s elapsed_us=%llu\n",label,(unsigned long long)elapsed);
}
static void phase_deadline_case(void)
{
    Server server;server_open(&server,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,SERVER_STALL_HANDSHAKE);
    X4TlsHttp *http=client_open(true);X4TlsConn *connection=NULL;
    X4TlsBudget budget=budget_after(UINT64_C(3000000),NULL);
    budget.phase_deadline_us=x4_tls_now_us()+120000;
    uint64_t start=monotonic_us();
    int result=x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection);
    uint64_t elapsed=monotonic_us()-start;
    CHECK(result==(int32_t)UINT32_C(0x8041013c)); /* BSD ETIMEDOUT=60. */
    CHECK(elapsed>=100000&&elapsed<=370000);
    if(connection)CHECK(x4_tls_conn_close(connection)==0);
    CHECK(x4_tls_http_close(http)==0);server_close(&server);
    printf("PASS tls explicit phase deadline elapsed_us=%llu\n",(unsigned long long)elapsed);
}
static void reset_after_partial_body(void)
{
    Server server;server_open(&server,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,SERVER_RESET_BODY);
    X4TlsHttp *http=client_open(true);X4TlsConn *connection=NULL;
    X4TlsBudget budget=budget_after(UINT64_C(3000000),NULL);
    CHECK(x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection)==0);
    unsigned char buffer[256];size_t received=0;
    CHECK(x4_tls_conn_read(connection,buffer,sizeof(buffer),&received,&budget)==0&&received>0);
    atomic_store(&server.reset_allowed,true);
    int result=0;size_t total=received;
    do{received=0;result=x4_tls_conn_read(connection,buffer,sizeof(buffer),&received,&budget);total+=received;}while(!result&&received);
    CHECK(total>0&&result==(int32_t)UINT32_C(0x80410136));
    CHECK(x4_tls_conn_close(connection)==0);CHECK(x4_tls_http_close(http)==0);server_close(&server);
    puts("PASS tls reset after partial body -> 0x80410136");
}
static unsigned rejected_socket_options;
static int reject_socket_option(int fd,int level,int option,const void *value,size_t length)
{
    CHECK(fd>=0&&level==SOL_SOCKET&&option==0x0800&&length==sizeof(int)&&*(const int *)value==1);
    ++rejected_socket_options;errno=ENOPROTOOPT;return -1;
}
static void socket_option_nonfatal(void)
{
    Server server;server_open(&server,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,SERVER_NORMAL);
    X4TlsHttp *http=client_open(true);X4TlsConn *connection=NULL;
    struct sigaction action;CHECK(!sigaction(SIGPIPE,NULL,&action)&&action.sa_handler==SIG_IGN);
    CHECK(!x4_tls_http_test_socket_option(http,reject_socket_option));
    X4TlsBudget budget=budget_after(UINT64_C(4000000),NULL);
    CHECK(!x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection));
    X4TlsInfo info=x4_tls_conn_info(connection);
    CHECK(info.verified&&!info.error&&info.nosigpipe_attempted&&info.nosigpipe_error==(int32_t)UINT32_C(0x8041012a));
    CHECK(rejected_socket_options==1);CHECK(!x4_tls_conn_close(connection));CHECK(!x4_tls_http_close(http));server_close(&server);
    puts("PASS TLS: rejected SO_NOSIGPIPE is nonfatal, process SIGPIPE remains ignored");
}
static void chain_case(const char *label,const Certificate *leaf,int version,bool ordered,bool repair,
                       bool trust,int expected,uint32_t expected_flags)
{
    const Certificate *good[]={&fixture.lower,&fixture.upper,&fixture.chain_root};
    const Certificate *bad[]={&fixture.chain_root,&fixture.upper,&fixture.lower};
    Server server;server_open_chain(&server,leaf,version,SERVER_NORMAL,ordered?good:bad,3);
    int error=-1;X4TlsHttp *http;
    if(trust){const uint8_t *roots[]={fixture.chain_root.bytes};size_t sizes[]={fixture.chain_root.length};
        http=x4_tls_http_test_open(roots,sizes,1,&error);
    }else http=x4_tls_http_open(&error);
    CHECK(http&&!error);CHECK(!x4_tls_http_test_reorder(http,repair));
    CHECK(!x4_tls_http_test_versions(http,version==MBEDTLS_SSL_VERSION_TLS1_2,version==MBEDTLS_SSL_VERSION_TLS1_3,false));
    _Atomic(const char *) stage="before chain";X4TlsBudget budget=budget_after(UINT64_C(4000000),NULL);budget.stage=&stage;
    X4TlsConn *connection=NULL;int result=x4_tls_conn_test_connect(http,"localhost",server.port,&budget,&connection);
    X4TlsInfo info=connection?x4_tls_conn_info(connection):x4_tls_http_info(http);
    if(result!=expected)
        fprintf(stderr,"TLS_CHAIN_RESULT label=%s version=%d result=0x%08x expected=0x%08x flags=0x%08x unordered=%d repaired=%d reorder_error=%d stage=%s\n",
            label,version,(unsigned)result,(unsigned)expected,(unsigned)info.verify_flags,
            info.chain_unordered,info.chain_reordered,info.chain_reorder_error,info.stage);
    CHECK(result==expected);
    CHECK(!strcmp(atomic_load_explicit(&stage,memory_order_acquire),info.stage));
    if(repair&&!ordered)CHECK(info.chain_unordered);
    if(!expected){CHECK(connection&&info.verified&&!info.verify_flags);CHECK(info.chain_reordered==(!ordered&&repair));}
    else{CHECK(!connection&&!info.verified);CHECK((info.verify_flags&expected_flags)==expected_flags);
        if(leaf==&fixture.chain_bad_usage||leaf==&fixture.chain_bad_eku)CHECK(info.chain_reordered);
        else CHECK(!info.chain_reordered);
    }
    if(connection)CHECK(!x4_tls_conn_close(connection));CHECK(!x4_tls_http_close(http));server_close(&server);
    printf("PASS local certificate chain %s unordered=%d repaired=%d flags=0x%08x\n",label,info.chain_unordered,info.chain_reordered,(unsigned)info.verify_flags);
}
int main(int argc,char **argv)
{
    private_fixture_process();
    CHECK(!setvbuf(stdout,NULL,_IOLBF,0));
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--sigaction-fail")));
    reject_sigpipe_setup=argc==2;
    certificates_open();
    if(reject_sigpipe_setup){
        connect_case("verified HTTPS with sigaction rejected",&fixture.valid,
            MBEDTLS_SSL_VERSION_TLS1_3,true,SERVER_NORMAL,0);
        CHECK(sigpipe_setup_rejections==1);
        certificates_close();
        puts("PASS sigaction rejection does not prevent verified HTTPS");
        return 0;
    }
    connect_case("TLS1.2 verified",&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_2,true,SERVER_NORMAL,0);
    connect_case("TLS1.3 verified",&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,true,SERVER_NORMAL,0);
    connect_case("wrong hostname",&fixture.wrong_name,MBEDTLS_SSL_VERSION_TLS1_3,true,SERVER_NORMAL,X4_HTTP_TLS_CERTIFICATE);
    connect_case("unknown CA",&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,false,SERVER_NORMAL,X4_HTTP_TLS_CERTIFICATE);
    connect_case("expired certificate",&fixture.expired,MBEDTLS_SSL_VERSION_TLS1_3,true,SERVER_NORMAL,X4_HTTP_TLS_CERTIFICATE_TIME);
    connect_case("CBC-only rejected",&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_2,true,SERVER_CBC,X4_HTTP_TLS_ALERT);
    socket_option_nonfatal();
    chain_case("ordered stock verifier",&fixture.chain_valid,MBEDTLS_SSL_VERSION_TLS1_3,true,false,true,0,0);
    chain_case("unordered stock rejected",&fixture.chain_valid,MBEDTLS_SSL_VERSION_TLS1_3,false,false,true,X4_HTTP_TLS_CERTIFICATE,MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    chain_case("unordered repaired TLS1.3",&fixture.chain_valid,MBEDTLS_SSL_VERSION_TLS1_3,false,true,true,0,0);
    chain_case("unordered repaired TLS1.2",&fixture.chain_valid,MBEDTLS_SSL_VERSION_TLS1_2,false,true,true,0,0);
    chain_case("unordered unknown root rejected",&fixture.chain_valid,MBEDTLS_SSL_VERSION_TLS1_3,false,true,false,X4_HTTP_TLS_CERTIFICATE,MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    chain_case("unordered wrong hostname rejected",&fixture.chain_wrong_name,MBEDTLS_SSL_VERSION_TLS1_3,false,true,true,X4_HTTP_TLS_CERTIFICATE,MBEDTLS_X509_BADCERT_CN_MISMATCH);
    chain_case("unordered expired rejected",&fixture.chain_expired,MBEDTLS_SSL_VERSION_TLS1_3,false,true,true,X4_HTTP_TLS_CERTIFICATE_TIME,MBEDTLS_X509_BADCERT_EXPIRED);
    chain_case("TLS keyUsage still enforced",&fixture.chain_bad_usage,MBEDTLS_SSL_VERSION_TLS1_3,false,true,true,X4_HTTP_TLS_CERTIFICATE,MBEDTLS_X509_BADCERT_KEY_USAGE);
    chain_case("TLS EKU still enforced",&fixture.chain_bad_eku,MBEDTLS_SSL_VERSION_TLS1_3,false,true,true,X4_HTTP_TLS_CERTIFICATE,MBEDTLS_X509_BADCERT_EXT_KEY_USAGE);
    cancel_pending_connect();cancellation_case(true);cancellation_case(false);
    deadline_case(UINT64_C(2000000),"2s request budget");
    deadline_case(UINT64_C(120000),"short absolute request budget");
    phase_deadline_case();
    reset_after_partial_body();certificates_close();
    printf("PASS tls_conn_test checks=%u; in-memory generated test CA/keys only; no external hosts, files or HTTP policy claims\n",atomic_load(&checks));
    return 0;
}
