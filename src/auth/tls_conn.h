/* SPDX-License-Identifier: GPL-3.0-only */
/* Worker-owned TCP/TLS primitives. No HTTP policy, cookies or credentials. */
#pragma once
#include "http_client.h"

typedef struct X4TlsHttp X4TlsHttp;
typedef struct X4TlsConn X4TlsConn;
typedef struct {
    uint64_t deadline_us; /* absolute monotonic request/reconnect deadline */
    uint64_t phase_deadline_us; /* 0: start a fresh phase of at most 5 s */
    const _Atomic int *cancel;
    _Atomic(const char *) *stage; /* optional UI-safe publication, literals only */
} X4TlsBudget;
typedef struct {
    const char *stage;
    int error, mbed_error;
    uint32_t verify_flags;
    uint64_t handshake_us;
    unsigned dns_hits, dns_misses;
    bool reused, verified, chain_unordered,chain_reordered,nosigpipe_attempted;
    int chain_reorder_error,nosigpipe_error,anchor_index;
    char anchor_name[96],anchor_sha256[96]; /* fixed bundled CA metadata only */
    char host[254], version[16], suite[96];
} X4TlsInfo;

uint64_t x4_tls_now_us(void);
/* PS4 integration first initializes Net and calls x4_native_net_prepare,
 * exactly as the existing process-wide RTC resolver requires. */
X4TlsHttp *x4_tls_http_open(int *error);
/* Convert a fresh context to update-only. Adds exactly ISRG Root X1 and
 * restricts this context to the three GitHub asset hosts before DNS. */
int x4_tls_http_enable_update_ca(X4TlsHttp *);
/* Fails BUSY while any connection (including quarantined ones) still exists. */
int x4_tls_http_close(X4TlsHttp *);
int x4_tls_conn_connect(X4TlsHttp *,const char *host,const X4TlsBudget *,X4TlsConn **);
int x4_tls_conn_write(X4TlsConn *,const void *,size_t,const X4TlsBudget *);
/* Zero bytes means TLS/transport EOF; framing belongs to http_wire. */
int x4_tls_conn_read(X4TlsConn *,void *,size_t,size_t *,const X4TlsBudget *);
int x4_tls_conn_close(X4TlsConn *);
/* Non-consuming preflight. True means poll found no event and TLS has no
 * buffered input. This cannot rule out a peer close after the check. */
bool x4_tls_conn_idle_clean(const X4TlsConn *);
void x4_tls_conn_mark_reused(X4TlsConn *);
void x4_tls_count_reopen(void);
X4TlsInfo x4_tls_http_info(const X4TlsHttp *);
X4TlsInfo x4_tls_conn_info(const X4TlsConn *);

#ifdef X4_HTTP_TEST
/* Test anchors/port/CBC mode exist only in host fixtures. VERIFY_REQUIRED
 * and hostname verification remain mandatory even in these test entrypoints. */
X4TlsHttp *x4_tls_http_test_open(const uint8_t *const *der,const size_t *sizes,
                              size_t count,int *error);
int x4_tls_conn_test_connect(X4TlsHttp *,const char *,unsigned port,
                            const X4TlsBudget *,X4TlsConn **);
int x4_tls_http_test_versions(X4TlsHttp *,bool tls12,bool tls13,bool rsa_cbc_only);
int x4_tls_http_test_reorder(X4TlsHttp *,bool enabled);
typedef int (*X4TlsSocketOptionTest)(int,int,int,const void *,size_t);
int x4_tls_http_test_socket_option(X4TlsHttp *,X4TlsSocketOptionTest);
#endif
