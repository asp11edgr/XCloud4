/* SPDX-License-Identifier: GPL-3.0-only */
/* Worker-owned HTTPS transport shared by both products. The host fixtures
 * exercise the same verification, deadline and ownership implementation. */
#define _POSIX_C_SOURCE 200809L
#include "tls_conn.h"
#include "ca_bundle.h"
#include "../update/github_root_x1_der.h"
#include "../core/x4_log.h"
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/version.h>
#include <psa/crypto.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef X4_OPENORBIS
#include "../streaming/rtc_native.h"
#include <orbis/libkernel.h>
#else
#include <fcntl.h>
#endif

enum { DNS_ENTRIES=8, DNS_TTL_US=60000000, PHASE_US=5000000, WAIT_US=100000 };
#if MBEDTLS_VERSION_NUMBER != 0x03060700
#error The narrow verification-result repair below is audited for Mbed TLS 3.6.7 only
#endif
typedef struct { char host[254];struct in_addr address;uint64_t expires; } DnsEntry;
struct X4TlsHttp {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config config;
    DnsEntry dns[DNS_ENTRIES];
    unsigned next_dns,connections;
    X4TlsInfo info;
    bool unusable, update_ca;
    X4TlsConn *verifying; /* owner-thread only, non-NULL during handshake */
#ifdef X4_HTTP_TEST
    X4TlsSocketOptionTest socket_option_test;
#endif
};
struct X4TlsConn {
    X4TlsHttp *owner;
    mbedtls_ssl_context ssl;
    X4TlsBudget budget;
    X4TlsInfo info;
    uint64_t phase;
    int fd,io_error;
    bool quarantined,error_logged;
    uint32_t original_verify_flags;
};
static pthread_once_t global_once=PTHREAD_ONCE_INIT;
static int global_error;
static atomic_flag nosigpipe_logged=ATOMIC_FLAG_INIT;
#if defined(X4_OPENORBIS) && !X4_PUBLIC
static _Atomic uint64_t tls_full,tls_reused,tls_reopened,tls_summary_at;

static void report_tls_activity(void)
{
    uint64_t now=x4_tls_now_us();
    uint64_t previous=atomic_load_explicit(&tls_summary_at,memory_order_relaxed);
    if(previous&&now-previous<UINT64_C(60000000))return;
    if(!atomic_compare_exchange_strong_explicit(&tls_summary_at,&previous,now,
        memory_order_relaxed,memory_order_relaxed)||!previous)return;
    x4_log_printf("XCloud4: HTTPS resumen completos=%llu reutilizados=%llu nuevas_por_cierre=%llu\n",
        (unsigned long long)atomic_load_explicit(&tls_full,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&tls_reused,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&tls_reopened,memory_order_relaxed));
}
#endif

static void publish_stage(const X4TlsBudget *budget,const char *literal)
{if(budget&&budget->stage)atomic_store_explicit(budget->stage,literal,memory_order_release);}
static void connection_stage(X4TlsConn *c,const char *literal)
{c->info.stage=literal;publish_stage(&c->budget,literal);}

uint64_t x4_tls_now_us(void)
{
#ifdef X4_OPENORBIS
    return sceKernelGetProcessTime();
#else
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
#endif
}
/* Native errno is already BSD. Host fixtures translate Linux errno to that
 * same public SceNet convention, notably ECONNRESET 104 -> 54. */
static int net_error(int e)
{
#ifndef X4_OPENORBIS
    switch(e){
    case EINTR:e=4;break;case EIO:e=5;break;case EBADF:e=9;break;
    case ENOMEM:e=12;break;case EACCES:e=13;break;case EINVAL:e=22;break;
    case EPIPE:e=32;break;case EAGAIN:e=35;break;case EINPROGRESS:e=36;break;
    case EALREADY:e=37;break;case ENOTSOCK:e=38;break;case EADDRINUSE:e=48;break;
    case ENOPROTOOPT:e=42;break;
    case EADDRNOTAVAIL:e=49;break;case ENETDOWN:e=50;break;case ENETUNREACH:e=51;break;
    case ECONNABORTED:e=53;break;case ECONNRESET:e=54;break;case ENOBUFS:e=55;break;
    case ENOTCONN:e=57;break;case ETIMEDOUT:e=60;break;case ECONNREFUSED:e=61;break;
    case EHOSTUNREACH:e=65;break;case ECANCELED:e=85;break;default:e=5;break;
    }
#endif
    return (int)(uint32_t)(UINT32_C(0x80410100)|(unsigned)e);
}
static void initialize_global(void)
{
    /* Based on PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later, v1.5.5:
     * src/app/shell_ps4.cpp:27-35. Process-wide, once, before any HTTPS send;
     * an unsupported per-socket option must not turn a dropped peer into a
     * process-killing signal. No request ever restores the old disposition. */
    struct sigaction action={0};action.sa_handler=SIG_IGN;
    if(sigemptyset(&action.sa_mask)||sigaction(SIGPIPE,&action,NULL)){
        printf("XCloud4: HTTPS SIGPIPE sigaction no disponible errno=%d; se mantiene la protección por socket\n",errno);
    }
#ifdef X4_OPENORBIS
    if(x4_native_entropy_prepare()<0){global_error=X4_HTTP_UNUSABLE;return;}
#endif
    /* Mbed TLS 3.6 with THREADING_C serializes its global PSA initialization.
     * libdatachannel may initialize it too; never free this process-global
     * subsystem from an individual HTTP context. */
    if(psa_crypto_init()!=PSA_SUCCESS)global_error=X4_HTTP_UNUSABLE;
}
static uint64_t add_time(uint64_t a,uint64_t b)
{return UINT64_MAX-a<b?UINT64_MAX:a+b;}
static uint64_t phase_deadline(const X4TlsBudget *b)
{
    uint64_t p=b->phase_deadline_us?b->phase_deadline_us:add_time(x4_tls_now_us(),PHASE_US);
    return p<b->deadline_us?p:b->deadline_us;
}
static int budget_check(const X4TlsBudget *b,uint64_t phase)
{
    if(b->cancel&&atomic_load_explicit(b->cancel,memory_order_relaxed))return X4_HTTP_CANCELLED;
    uint64_t now=x4_tls_now_us();
    if(!b->deadline_us||now>=b->deadline_us)return X4_HTTP_DEADLINE;
    if(now>=phase)return net_error(ETIMEDOUT);
    return 0;
}
static int wait_socket(X4TlsConn *c,short events)
{
    for(;;){
        int rc=budget_check(&c->budget,c->phase);if(rc)return c->io_error=rc;
        uint64_t now=x4_tls_now_us();if(now>=c->phase)continue;
        uint64_t remaining=c->phase-now;
        if(remaining>WAIT_US)remaining=WAIT_US;
        int ms=(int)((remaining+999)/1000);if(ms<1)ms=1;
        struct pollfd p={.fd=c->fd,.events=events};
        rc=poll(&p,1,ms);
        if(rc<0){if(errno==EINTR)continue;return c->io_error=net_error(errno);}
        if(!rc)continue;
        if(p.revents&POLLNVAL)return c->io_error=net_error(EBADF);
        /* HUP/ERR must be consumed by recv or SO_ERROR so a reset is not
         * confused with graceful EOF or silently converted to a TLS error. */
        if(p.revents&(events|POLLHUP|POLLERR))return 0;
    }
}
static int bio_send(void *opaque,const unsigned char *data,size_t size)
{
    X4TlsConn *c=opaque;
    int rc=budget_check(&c->budget,c->phase);
    if(rc){c->io_error=rc;return MBEDTLS_ERR_NET_SEND_FAILED;}
    if(size>INT_MAX)size=INT_MAX;
#ifdef X4_OPENORBIS
    ssize_t n=send(c->fd,data,size,0);
#else
    ssize_t n=send(c->fd,data,size,MSG_NOSIGNAL);
#endif
    if(n>=0)return (int)n;
    if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return MBEDTLS_ERR_SSL_WANT_WRITE;
    c->io_error=net_error(errno);return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int bio_receive(void *opaque,unsigned char *data,size_t size)
{
    X4TlsConn *c=opaque;
    int rc=budget_check(&c->budget,c->phase);
    if(rc){c->io_error=rc;return MBEDTLS_ERR_NET_RECV_FAILED;}
    if(size>INT_MAX)size=INT_MAX;
    ssize_t n=recv(c->fd,data,size,0);
    if(n>=0)return (int)n;
    if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return MBEDTLS_ERR_SSL_WANT_READ;
    c->io_error=net_error(errno);return MBEDTLS_ERR_NET_RECV_FAILED;
}
static void publish(X4TlsConn *c)
{c->info.dns_hits=c->owner->info.dns_hits;c->info.dns_misses=c->owner->info.dns_misses;c->owner->info=c->info;}
static void log_failure(const X4TlsInfo *i)
{
    /* Library lookup accepts a number only, never peer/request material. */
    const char *name=i->mbed_error?mbedtls_high_level_strerr(i->mbed_error):NULL;
    printf("XCloud4: HTTPS etapa=%s codigo=0x%08x mbed=%.96s flags=0x%08x unordered=%d reordered=%d anchor=%d\n",
        i->stage,(unsigned)i->error,name?name:"IO_OR_BUDGET",(unsigned)i->verify_flags,
        i->chain_unordered,i->chain_reordered,i->anchor_index);
}
static int io_failure(X4TlsConn *c,int error)
{
    c->info.error=error;publish(c);
    if(!c->error_logged){log_failure(&c->info);c->error_logged=true;}
    return error;
}
static int failure(X4TlsConn *c,int rc)
{
    int error=c->io_error;
    c->info.mbed_error=rc;
    c->info.verify_flags=mbedtls_ssl_get_verify_result(&c->ssl);
    /* -1 means verification has not run, e.g. an early protocol alert. */
    if(c->info.verify_flags==UINT32_MAX)c->info.verify_flags=0;
    if(!error){
        if(c->info.verify_flags&(MBEDTLS_X509_BADCERT_EXPIRED|MBEDTLS_X509_BADCERT_FUTURE)){
            error=X4_HTTP_TLS_CERTIFICATE_TIME;connection_stage(c,"certificado TLS: revisa fecha y hora");
        }else if(c->info.verify_flags||rc==MBEDTLS_ERR_X509_CERT_VERIFY_FAILED){
            error=X4_HTTP_TLS_CERTIFICATE;connection_stage(c,"certificado TLS");
        }else if(rc==MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE)error=X4_HTTP_TLS_ALERT;
        else if(rc==MBEDTLS_ERR_SSL_ALLOC_FAILED)error=X4_HTTP_ALLOCATION;
        else if(rc==MBEDTLS_ERR_SSL_CONN_EOF)error=net_error(ECONNRESET);
        else error=X4_HTTP_PROTOCOL;
    }
    return io_failure(c,error);
}

#ifndef X4_OPENORBIS
/* getaddrinfo itself has no portable timeout. A detached host-only job owns
 * its input/output and two references; a cancelled waiter never leaves a
 * pointer into an HTTP context or a worker stack behind. The PS4 path below
 * uses its native bounded resolver and never starts this host helper. */
typedef struct {
    pthread_mutex_t mutex;pthread_cond_t cond;unsigned refs;
    bool done;int result;char host[254];struct in_addr address;
} HostDns;
static void dns_release(HostDns *j)
{
    pthread_mutex_lock(&j->mutex);bool last=!--j->refs;pthread_mutex_unlock(&j->mutex);
    if(last){pthread_cond_destroy(&j->cond);pthread_mutex_destroy(&j->mutex);free(j);}
}
static void *host_dns_worker(void *opaque)
{
    HostDns *j=opaque;struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*a=NULL;
    int rc=getaddrinfo(j->host,NULL,&hints,&a);struct in_addr address={0};
    if(!rc&&a)address=((struct sockaddr_in *)a->ai_addr)->sin_addr;
    if(a)freeaddrinfo(a);
    pthread_mutex_lock(&j->mutex);j->address=address;j->result=rc;j->done=true;
    pthread_cond_signal(&j->cond);pthread_mutex_unlock(&j->mutex);dns_release(j);return NULL;
}
static int resolve_host(const char *host,struct in_addr *out,const X4TlsBudget *b,uint64_t phase)
{
    if(inet_pton(AF_INET,host,out)==1)return budget_check(b,phase);
    HostDns *j=calloc(1,sizeof(*j));if(!j)return X4_HTTP_ALLOCATION;
    snprintf(j->host,sizeof(j->host),"%s",host);
    if(pthread_mutex_init(&j->mutex,NULL)){free(j);return X4_HTTP_UNUSABLE;}
    pthread_condattr_t attr;int rc=pthread_condattr_init(&attr);
    if(rc){pthread_mutex_destroy(&j->mutex);free(j);return X4_HTTP_UNUSABLE;}
    rc=pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    if(!rc)rc=pthread_cond_init(&j->cond,&attr);pthread_condattr_destroy(&attr);
    if(rc){pthread_mutex_destroy(&j->mutex);free(j);return X4_HTTP_UNUSABLE;}
    j->refs=2;pthread_t worker;rc=pthread_create(&worker,NULL,host_dns_worker,j);
    if(rc){j->refs=1;dns_release(j);return X4_HTTP_UNUSABLE;}
    pthread_detach(worker);
    pthread_mutex_lock(&j->mutex);
    while(!j->done){
        rc=budget_check(b,phase);if(rc)break;
        uint64_t until=add_time(x4_tls_now_us(),WAIT_US);if(until>phase)until=phase;
        struct timespec ts={(time_t)(until/1000000u),(long)(until%1000000u)*1000};
        int w=pthread_cond_timedwait(&j->cond,&j->mutex,&ts);
        if(w&&w!=ETIMEDOUT){rc=X4_HTTP_UNUSABLE;break;}
    }
    if(j->done){
        if(!j->result){*out=j->address;rc=budget_check(b,phase);}
        else rc=(int)UINT32_C(0x804101e6); /* native resolver ENOHOST */
    }
    pthread_mutex_unlock(&j->mutex);dns_release(j);return rc;
}
#endif
static int resolve(X4TlsHttp *h,const char *host,struct in_addr *out,const X4TlsBudget *b)
{
    uint64_t phase=phase_deadline(b);int rc=budget_check(b,phase);if(rc)return rc;
    uint64_t now=x4_tls_now_us();
    for(unsigned i=0;i<DNS_ENTRIES;++i)if(h->dns[i].expires>now&&!strcmp(h->dns[i].host,host)){
        ++h->info.dns_hits;*out=h->dns[i].address;return 0;
    }
    ++h->info.dns_misses;
#ifdef X4_OPENORBIS
    rc=x4_native_resolve_ipv4_deadline(host,out,phase,b->cancel,X4_HTTP_CANCELLED);
#else
    rc=resolve_host(host,out,b,phase);
#endif
    int after=budget_check(b,phase);if(after)return after;
    if(rc)return rc;
    DnsEntry *d=&h->dns[h->next_dns++%DNS_ENTRIES];snprintf(d->host,sizeof(d->host),"%s",host);
    d->address=*out;d->expires=add_time(x4_tls_now_us(),DNS_TTL_US);return 0;
}
static const int ciphers[]={
    MBEDTLS_TLS1_3_AES_128_GCM_SHA256,MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,0
};
static const uint16_t groups[]={MBEDTLS_SSL_IANA_TLS_GROUP_X25519,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,0};

static int selected_anchor(X4TlsHttp *h,const mbedtls_x509_crt *crt)
{
    /* Only an object selected from the configured trust list is an anchor.
     * A peer-supplied certificate is never promoted to trusted by its name. */
    bool trusted=false;
    for(const mbedtls_x509_crt *ca=&h->ca;ca;ca=ca->next)if(ca==crt){trusted=true;break;}
    if(!trusted)return -1;
    for(size_t i=0;i<x4_ca_bundle_count();++i){const X4CaCertEntry *entry=x4_ca_bundle_entry(i);
        if(entry&&entry->der_size==crt->raw.len&&!memcmp(entry->der_data,crt->raw.p,crt->raw.len))return (int)i;
    }
    if (h->update_ca && crt->raw.len == sizeof(x4_isrg_root_x1_der) &&
        !memcmp(crt->raw.p, x4_isrg_root_x1_der, sizeof(x4_isrg_root_x1_der)))
        return (int)X4_CA_BUNDLE_COUNT;
    return -1; /* A RAM-only fixture anchor is outside the pinned CA list. */
}
static void record_anchor(X4TlsConn *c,int index)
{
    if (index == (int)X4_CA_BUNDLE_COUNT) {
        c->info.anchor_index = index;
        snprintf(c->info.anchor_name, sizeof(c->info.anchor_name), "ISRG Root X1");
        snprintf(c->info.anchor_sha256, sizeof(c->info.anchor_sha256),
            "96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6");
        return;
    }
    if(index<0)return;const X4CaCertEntry *entry=x4_ca_bundle_entry((size_t)index);if(!entry)return;
    c->info.anchor_index=index;
    snprintf(c->info.anchor_name,sizeof(c->info.anchor_name),"%s",entry->name);
    snprintf(c->info.anchor_sha256,sizeof(c->info.anchor_sha256),"%s",entry->sha256_fingerprint);
}
typedef struct {X4TlsHttp *owner;int index;} AnchorAudit;
static int audit_anchor(void *opaque,mbedtls_x509_crt *crt,int depth,uint32_t *flags)
{
    (void)depth;(void)flags;AnchorAudit *audit=opaque;
    if(audit->index<0)audit->index=selected_anchor(audit->owner,crt);
    return 0; /* Passive only: cannot alter flags or recurse into TLS. */
}
static bool same_name(const mbedtls_x509_buf *a,const mbedtls_x509_buf *b)
{return a->len==b->len&&a->len&&!memcmp(a->p,b->p,a->len);}
/* Based on the issuer-ordering research in PsBox (C) 2026 RafaelNGP,
 * GPL-3.0-or-later, dbf20786afc23207b461b2203dff25df86ff4804,
 * src/net/http.cpp:497-560. Independent C implementation: unlike that
 * reference, NO intermediate flag is cleared/deferred before full success. */
static int verify_peer_chain(void *opaque,mbedtls_x509_crt *crt,int depth,uint32_t *flags)
{
    X4TlsHttp *h=opaque;X4TlsConn *c=h?h->verifying:NULL;
    if(!c||!crt||!flags)return 0;
    c->original_verify_flags|=*flags;
    if(c->info.anchor_index<0)record_anchor(c,selected_anchor(h,crt));
    if(depth!=0||!crt->next)return 0;
    enum { MAX_PEER_CHAIN=16 };
    const mbedtls_x509_crt *all[MAX_PEER_CHAIN],*ordered[MAX_PEER_CHAIN];
    bool used[MAX_PEER_CHAIN]={0};size_t count=0;
    for(const mbedtls_x509_crt *node=crt;node;node=node->next){
        if(count==MAX_PEER_CHAIN){c->info.chain_reorder_error=MBEDTLS_ERR_X509_BAD_INPUT_DATA;return 0;}
        all[count++]=node;
    }
    size_t placed=1;ordered[0]=all[0];used[0]=true;
    while(placed<count){
        const mbedtls_x509_crt *child=ordered[placed-1];
        if(same_name(&child->issuer_raw,&child->subject_raw))break;
        size_t found=count;
        for(size_t i=1;i<count;++i)if(!used[i]&&same_name(&child->issuer_raw,&all[i]->subject_raw)){found=i;break;}
        if(found==count)break;
        ordered[placed++]=all[found];used[found]=true;
    }
    /* Keep all peer DER objects, including unrelated extra certificates.
     * Mbed TLS still chooses and verifies the actual path to the same CA list. */
    for(size_t i=1;i<count;++i)if(!used[i])ordered[placed++]=all[i];
    bool changed=false;for(size_t i=0;i<count;++i)if(ordered[i]!=all[i])changed=true;
    c->info.chain_unordered=changed;
    if(!changed||!(c->original_verify_flags&MBEDTLS_X509_BADCERT_NOT_TRUSTED))return 0;
    int stop=budget_check(&c->budget,c->phase);
    if(stop){c->io_error=stop;return MBEDTLS_ERR_X509_FATAL_ERROR;}
    mbedtls_x509_crt copy;mbedtls_x509_crt_init(&copy);int result=0;
    for(size_t i=0;i<count;++i){
        result=mbedtls_x509_crt_parse_der(&copy,ordered[i]->raw.p,ordered[i]->raw.len);
        if(result)break;
    }
    uint32_t checked_flags=UINT32_MAX;AnchorAudit audit={h,-1};
    if(!result)result=mbedtls_x509_crt_verify_with_profile(&copy,&h->ca,NULL,
        h->config.MBEDTLS_PRIVATE(cert_profile),c->info.host,&checked_flags,audit_anchor,&audit);
    mbedtls_x509_crt_free(&copy); /* Copy owns its DER; original SSL nodes untouched. */
    c->info.chain_reorder_error=result;
    stop=budget_check(&c->budget,c->phase);
    if(stop){c->io_error=stop;return MBEDTLS_ERR_X509_FATAL_ERROR;}
    if(result||checked_flags)return 0; /* Preserve ALL original flags on failure. */
    /* Narrow, pinned-3.6.7 access: x509_crt.c:2801-2824 already OR-ed the
     * depth>0 flags before calling depth0; ssl_tls.c:9224-9230 passes this
     * exact session accumulator. Only a FULL same-profile/CA/host/time
     * re-verification above can clear it. TLS keyUsage/EKU/curve checks run
     * afterwards in ssl_tls.c:9242-9275 and remain completely unchanged. */
    mbedtls_ssl_session *session=c->ssl.MBEDTLS_PRIVATE(session_negotiate);
    if(!session)return 0;
    session->MBEDTLS_PRIVATE(verify_result)=0;*flags=0;
    c->info.chain_reordered=true;record_anchor(c,audit.index);
    return 0;
}
static X4TlsHttp *context_open(const uint8_t *const *der,const size_t *sizes,size_t count,int *error)
{
    int rc=X4_HTTP_UNUSABLE;X4TlsHttp *h=NULL;
    if(pthread_once(&global_once,initialize_global)||global_error)goto fail;
    h=calloc(1,sizeof(*h));if(!h){rc=X4_HTTP_ALLOCATION;goto fail;}
    mbedtls_entropy_init(&h->entropy);mbedtls_ctr_drbg_init(&h->random);
    mbedtls_x509_crt_init(&h->ca);mbedtls_ssl_config_init(&h->config);
    /* No caller seed or counter. Both platforms supply hardware_poll from
     * the secure OS RNG; an entropy failure makes this context unusable. */
    if(mbedtls_ctr_drbg_seed(&h->random,mbedtls_entropy_func,&h->entropy,NULL,0))goto fail;
    for(size_t i=0;i<count;++i)if(mbedtls_x509_crt_parse_der(&h->ca,der[i],sizes[i]))goto fail;
    if(!count)goto fail;
    if(mbedtls_ssl_config_defaults(&h->config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT))goto fail;
    mbedtls_ssl_conf_authmode(&h->config,MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&h->config,&h->ca,NULL);
    mbedtls_ssl_conf_verify(&h->config,verify_peer_chain,h);
    mbedtls_ssl_conf_rng(&h->config,mbedtls_ctr_drbg_random,&h->random);
    mbedtls_ssl_conf_min_tls_version(&h->config,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&h->config,MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_ciphersuites(&h->config,ciphers);
    mbedtls_ssl_conf_groups(&h->config,groups);
    mbedtls_ssl_conf_session_tickets(&h->config,MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    mbedtls_ssl_conf_tls13_key_exchange_modes(&h->config,MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
    h->info.stage="HTTPS listo";h->info.anchor_index=-1;if(error)*error=0;return h;
fail:
    if(h){mbedtls_ssl_config_free(&h->config);mbedtls_x509_crt_free(&h->ca);
        mbedtls_ctr_drbg_free(&h->random);mbedtls_entropy_free(&h->entropy);free(h);}
    if(error)*error=rc;return NULL;
}
X4TlsHttp *x4_tls_http_open(int *error)
{
    const uint8_t *der[X4_CA_BUNDLE_COUNT];size_t sizes[X4_CA_BUNDLE_COUNT];
    if(x4_ca_bundle_count()!=X4_CA_BUNDLE_COUNT){if(error)*error=X4_HTTP_UNUSABLE;return NULL;}
    for(size_t i=0;i<X4_CA_BUNDLE_COUNT;++i){const X4CaCertEntry *e=x4_ca_bundle_entry(i);der[i]=e->der_data;sizes[i]=e->der_size;}
    return context_open(der,sizes,X4_CA_BUNDLE_COUNT,error);
}
int x4_tls_http_enable_update_ca(X4TlsHttp *h)
{
    if (!h || h->connections || h->verifying || h->update_ca) return X4_HTTP_ARGUMENT;
    if (mbedtls_x509_crt_parse_der(&h->ca, x4_isrg_root_x1_der, sizeof(x4_isrg_root_x1_der)))
        return X4_HTTP_UNUSABLE;
    h->update_ca = true;
    return 0;
}

int x4_tls_http_close(X4TlsHttp *h)
{
    if(!h)return 0;if(h->connections)return X4_HTTP_BUSY;
    mbedtls_ssl_config_free(&h->config);mbedtls_x509_crt_free(&h->ca);
    mbedtls_ctr_drbg_free(&h->random);mbedtls_entropy_free(&h->entropy);free(h);return 0;
}
int x4_tls_conn_close(X4TlsConn *c)
{
    if(!c)return 0;if(c->quarantined)return X4_HTTP_UNUSABLE;
    /* No close_notify write: it could block after cancellation/deadline.
     * TLS state is freed only after our socket has actually closed. */
    if(c->fd>=0&&close(c->fd)){
        c->quarantined=c->owner->unusable=true;return net_error(errno);
    }
    c->fd=-1;mbedtls_ssl_free(&c->ssl);--c->owner->connections;free(c);return 0;
}
static bool safe_host(const char *host)
{
    size_t n=host?strnlen(host,254):0;if(!n||n>=254)return false;
    for(size_t i=0;i<n;++i)if(!((host[i]>='a'&&host[i]<='z')||(host[i]>='A'&&host[i]<='Z')||
        (host[i]>='0'&&host[i]<='9')||host[i]=='-'||host[i]=='.'))return false;
    return true;
}
static void socket_nosigpipe(X4TlsConn *c)
{
    enum { X4_BSD_SO_NOSIGPIPE=0x0800 };int yes=1,rc=0;
#ifdef X4_OPENORBIS
    c->info.nosigpipe_attempted=true;
    rc=setsockopt(c->fd,SOL_SOCKET,X4_BSD_SO_NOSIGPIPE,&yes,sizeof(yes));
#elif defined(X4_HTTP_TEST)
    if(c->owner->socket_option_test){
        c->info.nosigpipe_attempted=true;
        rc=c->owner->socket_option_test(c->fd,SOL_SOCKET,X4_BSD_SO_NOSIGPIPE,&yes,sizeof(yes));
    }
#else
    (void)yes;
#endif
    if(rc){
        c->info.nosigpipe_error=net_error(errno);
        if(!atomic_flag_test_and_set_explicit(&nosigpipe_logged,memory_order_relaxed))
            printf("XCloud4: HTTPS SO_NOSIGPIPE no disponible codigo=0x%08x; SIGPIPE ignorado por proceso\n",
                (unsigned)c->info.nosigpipe_error);
    }
}
static int connect_to(X4TlsHttp *h,const char *host,unsigned port,const X4TlsBudget *b,X4TlsConn **out)
{
    if(out)*out=NULL;
    if(!h||!out||!b||!safe_host(host)||!port||port>65535)return X4_HTTP_ARGUMENT;
    if(h->unusable)return X4_HTTP_UNUSABLE;
    h->info=(X4TlsInfo){.stage="resolucion DNS",.anchor_index=-1,.dns_hits=h->info.dns_hits,.dns_misses=h->info.dns_misses};
    publish_stage(b,"resolucion DNS");
    snprintf(h->info.host,sizeof(h->info.host),"%s",host);
    struct in_addr address;int rc=resolve(h,host,&address,b);
    if(rc){h->info.error=rc;log_failure(&h->info);return rc;}
    X4TlsConn *c=calloc(1,sizeof(*c));if(!c)return X4_HTTP_ALLOCATION;
    c->owner=h;c->fd=-1;c->budget=*b;c->info=h->info;++h->connections;
    mbedtls_ssl_init(&c->ssl);connection_stage(c,"conexion HTTPS");c->phase=phase_deadline(b);
    if((rc=budget_check(b,c->phase)))goto fail;
    c->fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(c->fd<0){rc=net_error(errno);goto fail;}
#ifdef X4_OPENORBIS
    if(x4_native_socket_nonblock(c->fd)){rc=net_error(errno);goto fail;}
#else
    int f=fcntl(c->fd,F_GETFL,0);
    if(f<0||fcntl(c->fd,F_SETFL,f|O_NONBLOCK)){rc=net_error(errno);goto fail;}
#endif
    socket_nosigpipe(c); /* Global SIG_IGN protects sends even if this fails. */
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr=address};
#ifdef X4_OPENORBIS
    a.sin_len=sizeof(a);
#endif
    if(connect(c->fd,(struct sockaddr *)&a,sizeof(a))){
        if(errno!=EINPROGRESS&&errno!=EWOULDBLOCK){rc=net_error(errno);goto fail;}
        if((rc=wait_socket(c,POLLOUT)))goto fail;
        int e=0;socklen_t n=sizeof(e);
        if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&e,&n)){rc=net_error(errno);goto fail;}
        if(e){rc=net_error(e);goto fail;}
    }
    connection_stage(c,"negociacion TLS");c->phase=phase_deadline(b);c->io_error=0;
    if((rc=mbedtls_ssl_setup(&c->ssl,&h->config))){rc=failure(c,rc);goto fail;}
    if((rc=mbedtls_ssl_set_hostname(&c->ssl,host))){rc=failure(c,rc);goto fail;}
    mbedtls_ssl_set_bio(&c->ssl,c,bio_send,bio_receive,NULL);
    uint64_t started=x4_tls_now_us();
    for(;;){
        rc=budget_check(b,c->phase);if(rc)goto fail;
        h->verifying=c;
        rc=mbedtls_ssl_handshake(&c->ssl);
        h->verifying=NULL;
        if(!rc)break;
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE){
            int w=wait_socket(c,rc==MBEDTLS_ERR_SSL_WANT_READ?POLLIN:POLLOUT);
            if(!w)continue;rc=w;goto fail;
        }
        rc=failure(c,rc);goto fail;
    }
    if((rc=budget_check(b,c->phase)))goto fail;
    c->info.handshake_us=x4_tls_now_us()-started;
    c->info.verify_flags=mbedtls_ssl_get_verify_result(&c->ssl);
    if(c->info.verify_flags){rc=failure(c,MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);goto fail;}
    c->info.verified=true;c->info.error=0;connection_stage(c,"HTTPS listo");
    snprintf(c->info.version,sizeof(c->info.version),"%s",mbedtls_ssl_get_version(&c->ssl));
    snprintf(c->info.suite,sizeof(c->info.suite),"%s",mbedtls_ssl_get_ciphersuite(&c->ssl));
    publish(c);
#if !X4_PUBLIC
    printf("XCloud4: HTTPS host=%s tls=%s suite=%s handshake_ms=%.3f reused=0 unordered=%d reordered=%d anchor=%d anchor_name=%.95s anchor_sha256=%.95s\n",
        c->info.host,c->info.version,c->info.suite,(double)c->info.handshake_us/1000.0,
        c->info.chain_unordered,c->info.chain_reordered,c->info.anchor_index,c->info.anchor_name,c->info.anchor_sha256);
#endif
#if defined(X4_OPENORBIS) && !X4_PUBLIC
    atomic_fetch_add_explicit(&tls_full,1,memory_order_relaxed);
    report_tls_activity();
#endif
    *out=c;return 0;
fail:
    io_failure(c,rc);
    {int closed=x4_tls_conn_close(c);if(closed)h->unusable=true;}
    return rc;
}
static bool update_ca_host(const char *host)
{
    return host && (!strcmp(host, "github.com") || !strcmp(host, "objects.githubusercontent.com") ||
        !strcmp(host, "release-assets.githubusercontent.com"));
}

int x4_tls_conn_connect(X4TlsHttp *h,const char *host,const X4TlsBudget *b,X4TlsConn **out)
{
    if (h && h->update_ca && !update_ca_host(host)) {
        if (out) *out = NULL;
        return X4_HTTP_URL;
    }
    return connect_to(h,host,443,b,out);
}
static int start_io(X4TlsConn *c,const X4TlsBudget *b,const char *stage)
{
    if(!c||!b)return X4_HTTP_ARGUMENT;
    if(c->quarantined||c->owner->unusable)return X4_HTTP_UNUSABLE;
    c->budget=*b;c->phase=phase_deadline(b);c->io_error=0;connection_stage(c,stage);c->error_logged=false;
    int rc=budget_check(b,c->phase);return rc?io_failure(c,rc):0;
}
int x4_tls_conn_write(X4TlsConn *c,const void *data,size_t size,const X4TlsBudget *b)
{
    if(!data&&size)return X4_HTTP_ARGUMENT;
    int rc=start_io(c,b,"envio HTTPS/TLS");if(rc)return rc;
    const unsigned char *p=data;
    while(size){
        rc=budget_check(b,c->phase);if(rc)return io_failure(c,rc);
        int n=mbedtls_ssl_write(&c->ssl,p,size>INT_MAX?INT_MAX:size);
        if(n>0){p+=n;size-=(size_t)n;continue;}
        if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
            rc=wait_socket(c,n==MBEDTLS_ERR_SSL_WANT_READ?POLLIN:POLLOUT);if(!rc)continue;
            return io_failure(c,rc);
        }
        return failure(c,n);
    }
    rc=budget_check(b,c->phase);return rc?io_failure(c,rc):0;
}
int x4_tls_conn_read(X4TlsConn *c,void *data,size_t size,size_t *got,const X4TlsBudget *b)
{
    if(got)*got=0;if(!data||!size||!got)return X4_HTTP_ARGUMENT;
    int rc=start_io(c,b,"lectura de respuesta");if(rc)return rc;
    for(;;){
        rc=budget_check(b,c->phase);if(rc)return io_failure(c,rc);
        int n=mbedtls_ssl_read(&c->ssl,data,size>INT_MAX?INT_MAX:size);
        if(n>0){
            rc=budget_check(b,c->phase);
            if(rc){memset(data,0,(size_t)n);return io_failure(c,rc);}
            *got=(size_t)n;return 0;
        }
        if(!n||n==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY){
            rc=budget_check(b,c->phase);return rc?io_failure(c,rc):0;
        }
        if(n==MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)continue;
        if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
            rc=wait_socket(c,n==MBEDTLS_ERR_SSL_WANT_READ?POLLIN:POLLOUT);if(!rc)continue;
            return io_failure(c,rc);
        }
        return failure(c,n);
    }
}
bool x4_tls_conn_idle_clean(const X4TlsConn *c)
{
    if(!c||c->fd<0||c->quarantined||c->owner->unusable||mbedtls_ssl_check_pending(&c->ssl))return false;
    struct pollfd event={.fd=c->fd,.events=POLLIN};
    return poll(&event,1,0)==0;
}

void x4_tls_conn_mark_reused(X4TlsConn *c)
{
    if(!c)return;
    c->info.reused=true;publish(c);
#if defined(X4_OPENORBIS) && !X4_PUBLIC
    atomic_fetch_add_explicit(&tls_reused,1,memory_order_relaxed);
    x4_log_printf("XCloud4: HTTPS host=%s reused=1\n",c->info.host);
    report_tls_activity();
#endif
}

void x4_tls_count_reopen(void)
{
#if defined(X4_OPENORBIS) && !X4_PUBLIC
    atomic_fetch_add_explicit(&tls_reopened,1,memory_order_relaxed);
    report_tls_activity();
#endif
}
X4TlsInfo x4_tls_http_info(const X4TlsHttp *h)
{return h?h->info:(X4TlsInfo){.stage="sin contexto HTTPS",.error=X4_HTTP_ARGUMENT};}
X4TlsInfo x4_tls_conn_info(const X4TlsConn *c)
{return c?c->info:(X4TlsInfo){.stage="sin contexto HTTPS",.error=X4_HTTP_ARGUMENT};}
#ifdef X4_HTTP_TEST
X4TlsHttp *x4_tls_http_test_open(const uint8_t *const *der,const size_t *sizes,size_t count,int *error)
{return context_open(der,sizes,count,error);}
int x4_tls_conn_test_connect(X4TlsHttp *h,const char *host,unsigned port,const X4TlsBudget *b,X4TlsConn **out)
{return connect_to(h,host,port,b,out);}
int x4_tls_http_test_versions(X4TlsHttp *h,bool tls12,bool tls13,bool cbc)
{
    if(!h||h->connections||(!tls12&&!tls13)||(cbc&&(!tls12||tls13)))return X4_HTTP_ARGUMENT;
    static const int rsa_cbc[]={MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
        MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA256,MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA,0};
    mbedtls_ssl_conf_min_tls_version(&h->config,tls12?MBEDTLS_SSL_VERSION_TLS1_2:MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&h->config,tls13?MBEDTLS_SSL_VERSION_TLS1_3:MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&h->config,cbc?rsa_cbc:ciphers);return 0;
}
int x4_tls_http_test_reorder(X4TlsHttp *h,bool enabled)
{
    if(!h||h->connections)return X4_HTTP_ARGUMENT;
    mbedtls_ssl_conf_verify(&h->config,enabled?verify_peer_chain:NULL,enabled?h:NULL);return 0;
}
int x4_tls_http_test_socket_option(X4TlsHttp *h,X4TlsSocketOptionTest callback)
{if(!h||h->connections)return X4_HTTP_ARGUMENT;h->socket_option_test=callback;return 0;}
#endif
