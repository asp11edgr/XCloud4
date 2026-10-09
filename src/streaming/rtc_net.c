/* SPDX-License-Identifier: GPL-3.0-only */
/* IPv4 resolver/interface adapters. OpenOrbis musl's Linux netlink-based
 * getifaddrs and resolver must not be used on the native BSD kernel. */
#include "../core/module.h"
#include "rtc_native.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <orbis/NetCtl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct sockaddr_storage)==128 &&
               offsetof(struct sockaddr_storage,ss_family)==1,
               "Compile RTC with the OpenOrbis BSD sockaddr overlay");

static atomic_flag dns_lock = ATOMIC_FLAG_INIT;
static int dns_pool = -1;
static int32_t (*pool_create)(const char *, int32_t, int32_t);
static int32_t (*resolver_create)(const char *, int32_t, int32_t);
static int32_t (*resolver_destroy)(int32_t);
static int32_t (*resolver_ntoa)(int32_t, const char *, struct in_addr *, int32_t, int32_t, int32_t);
static int32_t (*ctl_get_info)(int32_t, OrbisNetCtlInfo *);
static int32_t (*ctl_init)(void);

int x4_native_net_prepare(void)
{
    if (dns_pool >= 0) return 0;
    int net = x4_module_open("libSceNet");
    int ctl = x4_module_open("libSceNetCtl");
    if (net < 0 || ctl < 0) return -1;
    void *p = NULL;
#define RESOLVE(m, symbol, dst) do { \
    if (x4_module_symbol(m, symbol, &p) < 0) return -1; \
    dst = (typeof(dst))p; \
} while (0)
    RESOLVE(net, "sceNetPoolCreate", pool_create);
    RESOLVE(net, "sceNetResolverCreate", resolver_create);
    RESOLVE(net, "sceNetResolverDestroy", resolver_destroy);
    RESOLVE(net, "sceNetResolverStartNtoa", resolver_ntoa);
    RESOLVE(ctl, "sceNetCtlGetInfo", ctl_get_info);
    RESOLVE(ctl, "sceNetCtlInit", ctl_init);
#undef RESOLVE
    int init_rc = ctl_init();
    OrbisNetCtlInfo info = {0};
    /* A negative Init may mean the app already initialized NetCtl. Adopt
     * that context only when a real GetInfo operation succeeds; no guessed
     * firmware-specific already-initialized error code is accepted. */
    if (ctl_get_info(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) < 0) return -1;
    printf("XCloud4: RTC NetCtl preparado init=0x%08x\n", (unsigned)init_rc);
    /* HTTPS initialized the process-global Net API before session creation.
     * This dedicated resolver pool remains alive until native process exit. */
    dns_pool = pool_create("x4-rtc-dns", 64 * 1024, 0);
    return dns_pool >= 0 ? 0 : -1;
}

typedef struct {
    struct addrinfo ai;
    struct sockaddr_in address;
    char name[256];
} NativeAddress;

int x4_native_getaddrinfo(const char *node, const char *service,
                         const struct addrinfo *hints, struct addrinfo **out)
{
    if (!out) return EAI_FAIL;
    *out = NULL;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;
    int type = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_DGRAM;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return EAI_SOCKTYPE;
    unsigned port = 0;
    if (service) {
        if (!*service) return EAI_SERVICE;
        for (const char *p = service; *p; ++p) {
            if (*p < '0' || *p > '9' || port > 6553) return EAI_SERVICE;
            port = port * 10 + (unsigned)(*p - '0');
            if (port > 65535) return EAI_SERVICE;
        }
    }
    struct in_addr address = {0};
    if (!node) {
        if (!(hints && (hints->ai_flags & AI_PASSIVE))) address.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, node, &address) != 1) {
        size_t length = strnlen(node, 256);
        if (!length || length >= 256 || (hints && (hints->ai_flags & AI_NUMERICHOST)))
            return EAI_NONAME;
        if (dns_pool < 0 || !resolver_create || !resolver_ntoa) return EAI_FAIL;
        while (atomic_flag_test_and_set_explicit(&dns_lock, memory_order_acquire)) {}
        int resolver = resolver_create("x4-rtc-resolver", dns_pool, 0);
        int rc = resolver < 0 ? -1 : resolver_ntoa(resolver, node, &address, 2, 1, 0);
        if (resolver >= 0) resolver_destroy(resolver);
        atomic_flag_clear_explicit(&dns_lock, memory_order_release);
        if (rc < 0) return EAI_AGAIN;
    }
    NativeAddress *item = calloc(1, sizeof(*item));
    if (!item) return EAI_MEMORY;
    item->address.sin_len = sizeof(item->address);
    item->address.sin_family = AF_INET;
    item->address.sin_port = htons((uint16_t)port);
    item->address.sin_addr = address;
    item->ai.ai_family = AF_INET;
    item->ai.ai_socktype = type;
    item->ai.ai_protocol = type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    item->ai.ai_addrlen = sizeof(item->address);
    item->ai.ai_addr = (struct sockaddr *)&item->address;
    if (node && hints && (hints->ai_flags & AI_CANONNAME)) {
        memcpy(item->name, node, strlen(node) + 1);
        item->ai.ai_canonname = item->name;
    }
    *out = &item->ai;
    return 0;
}

void x4_native_freeaddrinfo(struct addrinfo *ai)
{
    while (ai) { struct addrinfo *next = ai->ai_next; free(ai); ai = next; }
}

int x4_native_getnameinfo(const struct sockaddr *sa, socklen_t length,
                         char *host, socklen_t host_length,
                         char *service, socklen_t service_length, int flags)
{
    if (!sa || sa->sa_family != AF_INET || length < sizeof(struct sockaddr_in)) return EAI_FAMILY;
    if (flags & NI_NAMEREQD) return EAI_NONAME;
    const struct sockaddr_in *address = (const struct sockaddr_in *)sa;
    if (host && (!host_length || !inet_ntop(AF_INET, &address->sin_addr, host, host_length)))
        return EAI_OVERFLOW;
    if (service) {
        int count = snprintf(service, service_length, "%u", ntohs(address->sin_port));
        if (count < 0 || (unsigned)count >= service_length) return EAI_OVERFLOW;
    }
    return 0;
}

typedef struct {
    struct ifaddrs interface;
    struct sockaddr_in address;
    char name[16];
} NativeInterface;

int x4_native_getifaddrs(struct ifaddrs **out)
{
    if (!out || !ctl_get_info) { x4_native_rtc_diagnostic(63,-EINVAL); errno = EINVAL; return -1; }
    *out = NULL;
    OrbisNetCtlInfo info = {0};
    int query_result = ctl_get_info(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
    x4_native_rtc_diagnostic(62,query_result);
    if (query_result < 0) { x4_native_rtc_diagnostic(63,-ENETDOWN); errno = ENETDOWN; return -1; }
    info.ip_address[sizeof(info.ip_address) - 1] = 0;
    NativeInterface *item = calloc(1, sizeof(*item));
    if (!item) { x4_native_rtc_diagnostic(63,-ENOMEM); errno = ENOMEM; return -1; }
    item->address.sin_len = sizeof(item->address);
    item->address.sin_family = AF_INET;
    if (inet_pton(AF_INET, info.ip_address, &item->address.sin_addr) != 1 || !item->address.sin_addr.s_addr) {
        free(item); x4_native_rtc_diagnostic(63,-ENETDOWN); errno = ENETDOWN; return -1;
    }
    memcpy(item->name, "x4-native-net", 14);
    item->interface.ifa_name = item->name;
    item->interface.ifa_flags = IFF_UP;
    item->interface.ifa_addr = (struct sockaddr *)&item->address;
    *out = &item->interface;
    x4_native_rtc_diagnostic(63,1);
    return 0;
}

void x4_native_freeifaddrs(struct ifaddrs *interface)
{
    while (interface) { struct ifaddrs *next = interface->ifa_next; free(interface); interface = next; }
}
