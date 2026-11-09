/* SPDX-License-Identifier: GPL-3.0-only */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Match the SDK's BSD errno when the host uses Linux errno numbering. */
#undef ETIMEDOUT
#define ETIMEDOUT 60

#define SCE_INVALID ((int32_t)UINT32_C(0x80410116))
#define SCE_RESOLVER_TIMEOUT ((int32_t)UINT32_C(0x804101e2))
#define SCE_NET_TIMEOUT ((int32_t)UINT32_C(0x8041013c))
enum { RESOLVER_ID = 7, CANCELLED = -1001 };

typedef struct { bool locked; } OrbisPthreadMutex;
static int dns_pool = 11;
static const char test_host[] = "https-dns.test.invalid";
static _Atomic int cancel;
static struct {
    uint64_t now;
    int create_result, ntoa_result[2];
    uint64_t ntoa_duration[2];
    bool cancel_after_create;
    unsigned creates, calls, destroys, locks, unlocks, logs;
    char log[256];
} mock;

static uint64_t sceKernelGetProcessTime(void) { return mock.now; }

static int scePthreadMutexInit(OrbisPthreadMutex *mutex, const void *attr, const char *name)
{
    assert(attr == NULL && strcmp(name, "x4-http-dns") == 0);
    mutex->locked = false;
    return 0;
}

static int scePthreadMutexTimedlock(OrbisPthreadMutex *mutex, unsigned timeout)
{
    assert(!mutex->locked && timeout > 0 && timeout <= 100000);
    ++mock.locks;
    mutex->locked = true;
    return 0;
}

static int scePthreadMutexUnlock(OrbisPthreadMutex *mutex)
{
    assert(mutex->locked);
    ++mock.unlocks;
    mutex->locked = false;
    return 0;
}

static int32_t create(const char *name, int32_t pool, int32_t flags)
{
    assert(strlen(name) <= 15 && strcmp(name, "x4-https-dns") == 0);
    assert(pool == dns_pool && flags == 0);
    ++mock.creates;
    if (mock.cancel_after_create) atomic_store(&cancel, 1);
    return mock.create_result;
}

static int32_t ntoa(int32_t id, const char *host, struct in_addr *address,
                    int32_t timeout, int32_t retries, int32_t flags)
{
    assert(id == RESOLVER_ID && strcmp(host, test_host) == 0);
    assert(timeout == 2000000 && retries == 1 && flags == 0);
    assert(mock.calls < 2);
    unsigned call = mock.calls++;
    mock.now += mock.ntoa_duration[call];
    if (!mock.ntoa_result[call]) address->s_addr = htonl(UINT32_C(0xc0000201));
    return mock.ntoa_result[call];
}

static int32_t destroy(int32_t id)
{
    assert(id == RESOLVER_ID);
    ++mock.destroys;
    return 0;
}

static int32_t (*resolver_create)(const char *, int32_t, int32_t) = create;
static int32_t (*resolver_ntoa)(int32_t, const char *, struct in_addr *, int32_t, int32_t, int32_t) = ntoa;
static int32_t (*resolver_destroy)(int32_t) = destroy;

__attribute__((format(printf, 1, 2)))
static int capture_log(const char *format, ...)
{
    ++mock.logs;
    va_list args;
    va_start(args, format);
    int size = vsnprintf(mock.log, sizeof(mock.log), format, args);
    va_end(args);
    return size;
}

#define printf capture_log
#include "rtc_net_https_under_test.inc"
#undef printf

static void reset(void)
{
    assert(!http_dns_mutex.locked);
    memset(&mock, 0, sizeof(mock));
    mock.now = 100;
    mock.create_result = RESOLVER_ID;
    atomic_store(&cancel, 0);
}

static void check(uint64_t deadline, int expected, const char *step,
                  unsigned creates, unsigned calls, unsigned destroys, unsigned locks)
{
    struct in_addr out = { .s_addr = UINT32_MAX };
    int rc = x4_native_resolve_ipv4_deadline(test_host, &out, deadline, &cancel, CANCELLED);
    assert(rc == expected);
    assert(out.s_addr == (expected ? 0 : htonl(UINT32_C(0xc0000201))));
    assert(mock.creates == creates && mock.calls == calls && mock.destroys == destroys);
    assert(mock.locks == locks && mock.unlocks == locks && !http_dns_mutex.locked);
    assert(mock.logs == (expected ? 1u : 0u));
    if (expected) {
        char expected_log[256];
        int len = snprintf(expected_log, sizeof(expected_log),
            "XCloud4: DNS https paso=%s codigo=0x%08x timeout_us=2000000 reintentos=1 transcurrido_us=%llu host=%s\n",
            step, (unsigned)expected, (unsigned long long)(mock.now - 100), test_host);
        assert(len >= 0 && (size_t)len < sizeof(expected_log));
        assert(strcmp(mock.log, expected_log) == 0);
    }
}

int main(void)
{
    const uint64_t phase = 5000100;
    reset();
    mock.ntoa_result[0] = SCE_RESOLVER_TIMEOUT;
    mock.ntoa_duration[0] = 4000000;
    check(phase, 0, NULL, 1, 2, 1, 1);

    reset();
    mock.create_result = SCE_INVALID;
    check(phase, SCE_INVALID, "crear", 1, 0, 0, 1);

    reset();
    mock.ntoa_result[0] = SCE_INVALID;
    check(phase, SCE_INVALID, "ntoa", 1, 1, 1, 1);

    reset();
    atomic_store(&cancel, 1);
    check(phase, CANCELLED, "control", 0, 0, 0, 0);

    reset();
    mock.cancel_after_create = true;
    check(phase, CANCELLED, "ntoa", 1, 0, 1, 1);

    reset();
    check(mock.now, SCE_NET_TIMEOUT, "control", 0, 0, 0, 0);

    reset();
    mock.ntoa_result[0] = mock.ntoa_result[1] = SCE_RESOLVER_TIMEOUT;
    mock.ntoa_duration[0] = mock.ntoa_duration[1] = 4000000;
    check(phase, SCE_NET_TIMEOUT, "ntoa", 1, 2, 1, 1);

    puts("PASS https DNS host stubs (7 scenarios)");
    return 0;
}
