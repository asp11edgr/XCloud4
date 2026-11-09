/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only resource experiment. Reuses the localhost fixture without
 * running its unit-test main. Certificates and keys remain in RAM. */
#define _GNU_SOURCE
#define main x4_tls_fixture_unused_main
#include "tls_conn_test.c"
#undef main
#include "../src/auth/ca_bundle.h"
#include <limits.h>
#include <psa/crypto_extra.h>

#ifdef X4_TLS_RESOURCES_SANITIZED
enum { CLIENT_STACK_BYTES=512*1024 };
static const bool benchmark=false;
#else
enum { CLIENT_STACK_BYTES=32768 };
static const bool benchmark=true;
#endif

/* Linker wrapping catches explicit tls_conn allocations and allocations in
 * the static Mbed TLS libraries. Only allocations originating in the client
 * thread are enrolled. Cross-thread frees (the host DNS helper) still retire
 * enrolled pointers. Server/fixture allocations are excluded. The table is
 * BSS metadata, not a prefix added to any measured allocation. */
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void __real_free(void *);
enum { TRACKED_ALLOCATIONS=8192,RETAINED_LIMIT=1024,PSA_LAZY_BUCKET_BYTES=896 };
enum { HEAP_CONTEXT_OPEN=1,HEAP_CONNECT,HEAP_CONNECTION_CLOSE,HEAP_CONTEXT_CLOSE,HEAP_GLOBAL_CLOSE };
typedef struct {
    size_t bytes;
    uintptr_t allocation_site,free_site;
    unsigned allocation_kind,free_phase;
    bool released,global_psa;
} Retained;
typedef struct {
    void *pointer;size_t bytes;uintptr_t site;
    unsigned phase,kind,epoch;Retained *watch;
} Allocation;
typedef struct {size_t live,peak,allocations,frees;} Heap;
static Allocation allocation_table[TRACKED_ALLOCATIONS];
static Heap heap;
static pthread_mutex_t heap_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool measure_client;
static _Thread_local unsigned client_phase;
static _Thread_local unsigned client_epoch;
static atomic_bool heap_fault;

static void enroll(void *pointer,size_t bytes,uintptr_t site,unsigned kind)
{
    if(!pointer||!measure_client)return;
    pthread_mutex_lock(&heap_lock);
    unsigned i;
    for(i=0;i<TRACKED_ALLOCATIONS;++i)if(!allocation_table[i].pointer)break;
    if(i==TRACKED_ALLOCATIONS||SIZE_MAX-heap.live<bytes)atomic_store(&heap_fault,true);
    else{
        allocation_table[i]=(Allocation){.pointer=pointer,.bytes=bytes,.site=site,.phase=client_phase,.kind=kind,.epoch=client_epoch};
        heap.live+=bytes;++heap.allocations;
        if(heap.live>heap.peak)heap.peak=heap.live;
    }
    pthread_mutex_unlock(&heap_lock);
}
void *__wrap_malloc(size_t bytes)
{void *pointer=__real_malloc(bytes);enroll(pointer,bytes,(uintptr_t)__builtin_return_address(0),1);return pointer;}
void *__wrap_calloc(size_t count,size_t bytes)
{
    void *pointer=__real_calloc(count,bytes);
    if(!bytes||count<=SIZE_MAX/bytes)enroll(pointer,count*bytes,(uintptr_t)__builtin_return_address(0),2);
    else if(pointer&&measure_client)atomic_store(&heap_fault,true);
    return pointer;
}
void __wrap_free(void *pointer)
{
    if(pointer){
        pthread_mutex_lock(&heap_lock);
        for(unsigned i=0;i<TRACKED_ALLOCATIONS;++i)if(allocation_table[i].pointer==pointer){
            if(heap.live<allocation_table[i].bytes)atomic_store(&heap_fault,true);
            else heap.live-=allocation_table[i].bytes;
            if(allocation_table[i].watch){
                allocation_table[i].watch->released=true;
                allocation_table[i].watch->free_site=(uintptr_t)__builtin_return_address(0);
                allocation_table[i].watch->free_phase=client_phase;
            }
            allocation_table[i]=(Allocation){0};++heap.frees;break;
        }
        pthread_mutex_unlock(&heap_lock);
    }
    __real_free(pointer);
}
static Heap heap_snapshot(bool reset_peak)
{
    pthread_mutex_lock(&heap_lock);Heap result=heap;
    if(reset_peak)heap.peak=heap.live;
    pthread_mutex_unlock(&heap_lock);
    CHECK(!atomic_load(&heap_fault));return result;
}
static void heap_reset(void)
{
    pthread_mutex_lock(&heap_lock);CHECK(!heap.live);
    memset(allocation_table,0,sizeof(allocation_table));heap=(Heap){0};
    pthread_mutex_unlock(&heap_lock);CHECK(!atomic_load(&heap_fault));
}
static size_t begin_epoch(void)
{
    pthread_mutex_lock(&heap_lock);
    /* The only state allowed between experiments is the independently
     * identified 3.6.7 lazy PSA bucket, not a generic byte tolerance. */
    unsigned remaining=0;
    for(unsigned i=0;i<TRACKED_ALLOCATIONS;++i)if(allocation_table[i].pointer){
        ++remaining;CHECK(allocation_table[i].bytes==PSA_LAZY_BUCKET_BYTES);
        CHECK(allocation_table[i].watch&&allocation_table[i].watch->global_psa);
    }
    CHECK((!heap.live&&!remaining)||(heap.live==PSA_LAZY_BUCKET_BYTES&&remaining==1));
    size_t baseline=heap.live;heap=(Heap){.live=baseline,.peak=baseline};++client_epoch;
    pthread_mutex_unlock(&heap_lock);return baseline;
}
typedef struct {
    const Server *server;
    int version,result;bool warm;
    Heap context,connection,after_connection,after_context;
    size_t connection_live,connection_peak;
    size_t warmed_context_growth,retained_count,retained_bytes;
    size_t global_before,global_after,global_new;
    Retained retained[RETAINED_LIMIT];
    X4TlsInfo info;
} ConnectionResources;
typedef struct {
    size_t actual_stack,guard;
    Heap production_context,production_closed;
    ConnectionResources tls12,tls13,tls13_warm;
} Resources;

static void watch_handshake_survivors(ConnectionResources *out)
{
    /* A handshake can create context-owned precomputations and process-
     * global PSA state. Record ownership evidence; never print data pointers
     * or contents. An older global bucket keeps its original watch record. */
    pthread_mutex_lock(&heap_lock);
    for(unsigned i=0;i<TRACKED_ALLOCATIONS;++i){
        Allocation *entry=&allocation_table[i];
        if(!entry->pointer||entry->phase!=HEAP_CONNECT||entry->epoch!=client_epoch)continue;
        CHECK(out->retained_count<RETAINED_LIMIT);
        Retained *record=&out->retained[out->retained_count++];
        *record=(Retained){.bytes=entry->bytes,.allocation_site=entry->site,.allocation_kind=entry->kind};
        entry->watch=record;out->retained_bytes+=entry->bytes;
    }
    pthread_mutex_unlock(&heap_lock);
}

static void local_connection_resources(ConnectionResources *out)
{
    out->global_before=begin_epoch();client_phase=HEAP_CONTEXT_OPEN;int error=-1;
    /* Keep all 11 production DER roots and add one RAM-only fixture anchor.
     * This context is deliberately larger than the production root set. */
    const uint8_t *roots[X4_CA_BUNDLE_COUNT+1];size_t sizes[X4_CA_BUNDLE_COUNT+1];
    for(size_t i=0;i<X4_CA_BUNDLE_COUNT;++i){
        const X4CaCertEntry *entry=x4_ca_bundle_entry(i);CHECK(entry);
        roots[i]=entry->der_data;sizes[i]=entry->der_size;
    }
    roots[X4_CA_BUNDLE_COUNT]=fixture.ca.bytes;sizes[X4_CA_BUNDLE_COUNT]=fixture.ca.length;
    X4TlsHttp *http=x4_tls_http_test_open(roots,sizes,X4_CA_BUNDLE_COUNT+1,&error);
    CHECK(http&&error==0);
    CHECK(x4_tls_http_test_versions(http,out->version==MBEDTLS_SSL_VERSION_TLS1_2,
                                  out->version==MBEDTLS_SSL_VERSION_TLS1_3,false)==0);
    out->context=heap_snapshot(true);
    X4TlsBudget budget=budget_after(UINT64_C(4000000),NULL);X4TlsConn *connection=NULL;
    client_phase=HEAP_CONNECT;
    out->result=x4_tls_conn_test_connect(http,"localhost",out->server->port,&budget,&connection);
    CHECK(!out->result&&connection);out->info=x4_tls_conn_info(connection);
    CHECK(out->info.verified&&!out->info.verify_flags);
    out->connection=heap_snapshot(false);
    CHECK(out->connection.live>=out->context.live&&out->connection.peak>=out->context.live);
    out->connection_peak=out->connection.peak-out->context.live;
    client_phase=HEAP_CONNECTION_CLOSE;
    CHECK(x4_tls_conn_close(connection)==0);out->after_connection=heap_snapshot(false);
    CHECK(out->after_connection.live>=out->context.live);
    CHECK(out->connection.live>=out->after_connection.live);
    out->connection_live=out->connection.live-out->after_connection.live;
    watch_handshake_survivors(out);
    client_phase=HEAP_CONTEXT_CLOSE;
    CHECK(x4_tls_http_close(http)==0);out->after_context=heap_snapshot(false);
    out->global_after=out->after_context.live;
    CHECK(out->global_after>=out->global_before);out->global_new=out->global_after-out->global_before;
    size_t context_after_connection=out->after_connection.live-out->global_after;
    size_t context_initial=out->context.live-out->global_before;
    CHECK(context_after_connection>=context_initial);
    out->warmed_context_growth=context_after_connection-context_initial;
    bool released_all=true;unsigned global_candidates=0;
    for(size_t i=0;i<out->retained_count;++i){
        Retained *r=&out->retained[i];
        if(r->released){if(r->free_phase!=HEAP_CONTEXT_CLOSE)released_all=false;continue;}
        /* Root's prior addr2line evidence identified this precise lazy
         * allocation as psa_reserve_free_key_slot. The script independently
         * resolves its current code site and requires that exact function. */
        if(out->version==MBEDTLS_SSL_VERSION_TLS1_3&&!out->warm&&!out->global_before&&
           out->global_new==PSA_LAZY_BUCKET_BYTES&&r->bytes==PSA_LAZY_BUCKET_BYTES&&r->allocation_kind==2){
            r->global_psa=true;++global_candidates;
        }else released_all=false;
    }
    if(out->version==MBEDTLS_SSL_VERSION_TLS1_3){
        if(out->global_after!=PSA_LAZY_BUCKET_BYTES||global_candidates!=(out->warm?0u:1u))released_all=false;
    }else if(out->global_after||global_candidates)released_all=false;
    if(!released_all){
        measure_client=false; /* Diagnostics are excluded even on failure. */
        fprintf(stderr,"RESOURCE_OWNER_FAIL version=%d context_before=%zu after_connection=%zu after_context=%zu retained=%zu\n",
            out->version,out->context.live,out->after_connection.live,out->after_context.live,out->retained_count);
        for(size_t i=0;i<out->retained_count;++i){const Retained *r=&out->retained[i];
            fprintf(stderr,"RESOURCE_OWNER_META bytes=%zu allocation_code=0x%llx free_code=0x%llx released=%d free_phase=%u\n",
                r->bytes,(unsigned long long)r->allocation_site,(unsigned long long)r->free_site,r->released,r->free_phase);
        }
        measure_client=true;
    }
    CHECK(released_all);
}
static void *resource_client(void *context)
{
    Resources *out=context;pthread_attr_t attributes;
    CHECK(!pthread_getattr_np(pthread_self(),&attributes));
    CHECK(!pthread_attr_getstacksize(&attributes,&out->actual_stack));
    CHECK(!pthread_attr_getguardsize(&attributes,&out->guard));
    CHECK(!pthread_attr_destroy(&attributes));CHECK(out->actual_stack==CLIENT_STACK_BYTES);
    heap_reset();client_phase=HEAP_CONTEXT_OPEN;measure_client=true;
    int error=-1;X4TlsHttp *http=x4_tls_http_open(&error);CHECK(http&&error==0);
    out->production_context=heap_snapshot(false);CHECK(out->production_context.live>0);
    client_phase=HEAP_CONTEXT_CLOSE;
    CHECK(x4_tls_http_close(http)==0);out->production_closed=heap_snapshot(false);
    CHECK(!out->production_closed.live);
    local_connection_resources(&out->tls12);local_connection_resources(&out->tls13);
    local_connection_resources(&out->tls13_warm);
    measure_client=false;return NULL;
}
static void print_connection(const char *label,const ConnectionResources *r)
{
    printf("{\"case\":\"%s\",\"benchmark\":%s,\"anchors\":12,\"context_live_bytes\":%zu,\"context_peak_bytes\":%zu,"
        "\"connection_live_bytes\":%zu,\"connect_phase_peak_increment_bytes\":%zu,"
        "\"combined_peak_bytes\":%zu,\"after_connection_bytes\":%zu,\"warmed_context_growth_bytes\":%zu,"
        "\"retained_handshake_allocations\":%zu,\"retained_handshake_bytes\":%zu,\"after_context_bytes\":%zu,"
        "\"global_psa_before_bytes\":%zu,\"global_psa_after_bytes\":%zu,\"global_psa_new_bytes\":%zu,"
        "\"handshake_us\":%llu,\"verified\":true,\"result\":%d,\"version\":\"%s\",\"suite\":\"%s\"}\n",
        label,benchmark?"true":"false",r->context.live-r->global_before,r->context.peak-r->global_before,r->connection_live,r->connection_peak,
        r->connection.peak,r->after_connection.live,r->warmed_context_growth,r->retained_count,r->retained_bytes,r->after_context.live,
        r->global_before,r->global_after,r->global_new,
        (unsigned long long)r->info.handshake_us,r->result,r->info.version,r->info.suite);
    for(size_t i=0;i<r->retained_count;++i){
        const Retained *a=&r->retained[i];
        printf("{\"case\":\"%s_retained\",\"bytes\":%zu,\"allocator\":\"%s\",\"owner\":\"%s\","
            "\"allocation_code_site\":\"0x%llx\",\"free_code_site\":\"0x%llx\",\"released\":%s,\"free_phase\":%u}\n",
            label,a->bytes,a->allocation_kind==1?"malloc":"calloc",
            a->global_psa?"global_psa_lazy":"http_context",
            (unsigned long long)a->allocation_site,(unsigned long long)a->free_site,a->released?"true":"false",a->free_phase);
        if(a->global_psa)printf("TLS_RESOURCE_PSA_BUCKET_SITE 0x%llx\n",(unsigned long long)a->allocation_site);
        printf("TLS_RESOURCE_CODE_SITE 0x%llx\nTLS_RESOURCE_CODE_SITE 0x%llx\n",
            (unsigned long long)a->allocation_site,(unsigned long long)a->free_site);
    }
}
int main(void)
{
    private_fixture_process();
    /* All server key generation and PSA initialization happen before any
     * client measurement. The fixture/server threads never enable enrollment. */
    certificates_open();Server server12,server13,server13_warm;
    server_open(&server12,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_2,SERVER_NORMAL);
    server_open(&server13,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,SERVER_NORMAL);
    server_open(&server13_warm,&fixture.valid,MBEDTLS_SSL_VERSION_TLS1_3,SERVER_NORMAL);
    Resources result={.tls12={.server=&server12,.version=MBEDTLS_SSL_VERSION_TLS1_2},
                      .tls13={.server=&server13,.version=MBEDTLS_SSL_VERSION_TLS1_3},
                      .tls13_warm={.server=&server13_warm,.version=MBEDTLS_SSL_VERSION_TLS1_3,.warm=true}};
    pthread_attr_t attributes;CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setstacksize(&attributes,CLIENT_STACK_BYTES));
    long page=sysconf(_SC_PAGESIZE);CHECK(page>0);
    CHECK(!pthread_attr_setguardsize(&attributes,(size_t)page));
    pthread_t client;CHECK(!pthread_create(&client,&attributes,resource_client,&result));
    CHECK(!pthread_attr_destroy(&attributes));CHECK(!pthread_join(client,NULL));
    server_close(&server12);server_close(&server13);server_close(&server13_warm);certificates_close();
    /* Fixture process only, after every HTTP/server context and key is gone.
     * Production must NEVER do this per Http: libdatachannel shares PSA. */
    CHECK(heap_snapshot(false).live==PSA_LAZY_BUCKET_BYTES);
    client_phase=HEAP_GLOBAL_CLOSE;mbedtls_psa_crypto_free();
    CHECK(!heap_snapshot(false).live);
    unsigned global_blocks=0;
    for(size_t i=0;i<result.tls13.retained_count;++i)if(result.tls13.retained[i].global_psa){
        Retained *r=&result.tls13.retained[i];++global_blocks;
        CHECK(r->bytes==PSA_LAZY_BUCKET_BYTES&&r->released&&r->free_phase==HEAP_GLOBAL_CLOSE);
    }
    CHECK(global_blocks==1);
    printf("{\"case\":\"production_context\",\"benchmark\":%s,\"anchors\":11,\"live_bytes\":%zu,\"peak_bytes\":%zu,"
        "\"after_close_bytes\":%zu,\"allocations\":%zu,\"frees\":%zu}\n",
        benchmark?"true":"false",result.production_context.live,result.production_context.peak,result.production_closed.live,
        result.production_closed.allocations,result.production_closed.frees);
    printf("{\"case\":\"worker_stack\",\"benchmark\":%s,\"requested_bytes\":%u,\"actual_bytes\":%zu,\"guard_bytes\":%zu,"
        "\"ps4_null_stack_observed_bytes\":65536,\"release_check_configured_bytes\":262144}\n",
        benchmark?"true":"false",(unsigned)CLIENT_STACK_BYTES,result.actual_stack,result.guard);
    print_connection("local_tls12",&result.tls12);print_connection("local_tls13_first",&result.tls13);
    print_connection("local_tls13_warm",&result.tls13_warm);
    puts("PASS tls resource ownership: exactly one lazy PSA calloc bucket (896 bytes), released only at final fixture-global shutdown; all tracked bytes freed");
    if(benchmark)puts("PASS plain stack evidence: verified local TLS1.2/1.3 handshakes on a 32KiB worker stack");
    else puts("PASS ASan/UBSan resource coverage: explicit 512KiB worker stack; NOT 32KiB evidence and NOT heap/performance benchmark");
    puts("SCOPE: requested bytes only; allocator overhead, BSS tracker, libc-private allocations, server and prior global PSA initialization excluded; local fixture is not a worst-case chain bound");
    return 0;
}
