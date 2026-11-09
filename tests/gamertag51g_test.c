/* SPDX-License-Identifier: GPL-3.0-only */
/* Production worker, JSON and HTTPS serializer with a local TLS double. */
#include <orbis/libkernel.h>
#include <sched.h>
#include <strings.h>

#define ORBIS_PTHREAD_CREATE_DETACHED PTHREAD_CREATE_DETACHED
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr *, int);
#include "../src/auth/xbox_live.c"
#include "tls_transport_fake.h"

enum Scenario { WITH_GTG, NO_GTG, LATE, CANCELLED_AT_DEADLINE, SIGNED_OUT };
static enum Scenario scenario;
static _Atomic uint64_t clock_us;
static atomic_bool allow_worker, profile_posted, allow_reply, cancel_observed;
static atomic_int cancel_quota;
static pthread_t test_thread;
static bool worker_created;
static void *(*test_entry)(void *);
static void *test_argument;
static unsigned old_calls;

uint64_t sceKernelGetProcessTime(void) { return atomic_load(&clock_us); }
int sceKernelUsleep(unsigned usec) { (void)usec; return 0; }
const char *x4_lang_store_suffix(void)
{ return "&market=MX&languages=es-MX&fieldsTemplate=Details"; }
int x4_native_random(void *data, size_t length) { memset(data, 0, length); return 0; }
int32_t scePthreadAttrInit(OrbisPthreadAttr *attr) { return pthread_attr_init(attr); }
int32_t scePthreadAttrSetstacksize(OrbisPthreadAttr *attr, size_t size)
{ return pthread_attr_setstacksize(attr, size); }
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr *attr, int state)
{ return pthread_attr_setdetachstate(attr, state); }
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr) { return pthread_attr_destroy(attr); }

static void *test_worker(void *unused)
{
    (void)unused;
    while (!atomic_load(&allow_worker)) sched_yield();
    return test_entry(test_argument);
}

int scePthreadCreate(OrbisPthread *thread, const pthread_attr_t *attr,
    void *(*entry)(void *), void *opaque, const char *name)
{
    size_t stack = 0;
    int state = 0;
    assert(!worker_created && !strcmp(name, "x4-gamertag"));
    assert(!pthread_attr_getstacksize(attr, &stack) && stack == 256u * 1024u);
    assert(!pthread_attr_getdetachstate(attr, &state) && state == PTHREAD_CREATE_DETACHED);
    test_entry = entry;
    test_argument = opaque;
    /* The host joins only to check cleanup after the real entry returns. */
    assert(!pthread_create(thread, NULL, test_worker, NULL));
    test_thread = *thread;
    worker_created = true;
    return 0;
}

static void reply(const X4TfRequest *request, X4TfResponse *response)
{
    const char expected_profile[] = "{\"Properties\":{\"SandboxId\":\"RETAIL\","
        "\"UserTokens\":[\"fixture-user-51g\"]},\"RelyingParty\":\"http://xboxlive.com\",\"TokenType\":\"JWT\"}";
    assert(!request->keepalive && !x4tf_header(request, "Cookie") &&
        !x4tf_header(request, "X-Forwarded-For"));
    switch (x4tf.requests) {
    case 1:
        ++old_calls;
        assert(request->method == 1 && !strcmp(request->url, X4_XBOX_USER_URL));
        assert(strstr(request->body, "\"RpsTicket\":\"d=fixture-microsoft-51g\""));
        response->body = "{\"Token\":\"fixture-user-51g\"}";
        break;
    case 2:
        ++old_calls;
        assert(request->method == 1 && !strcmp(request->url, X4_XBOX_XSTS_URL));
        assert(!strcmp(request->body, "{\"Properties\":{\"SandboxId\":\"RETAIL\","
            "\"UserTokens\":[\"fixture-user-51g\"]},\"RelyingParty\":\"http://mp.microsoft.com/\",\"TokenType\":\"JWT\"}"));
        response->body = "{\"Token\":\"fixture-market-51g\",\"DisplayClaims\":{\"xui\":[{\"uhs\":\"123456\"}]}}";
        break;
    case 3:
        ++old_calls;
        assert(request->method == 0 && !request->body_size &&
            !strcmp(request->url, X4_HTTP_CLOUD_TIME_PREFIX "MX" X4_HTTP_CLOUD_TIME_SUFFIX));
        assert(!strcmp(x4tf_header(request, "Authorization"), "XBL3.0 x=123456;fixture-market-51g"));
        response->body = "{\"remainingGameplayTimeInSeconds\":1200,\"maxGameplayTimeInSeconds\":7200}";
        break;
    case 4:
        assert(old_calls == 3 && request->method == 1 && !strcmp(request->url, X4_XBOX_XSTS_URL));
        assert(request->body_size == sizeof(expected_profile) - 1 && !strcmp(request->body, expected_profile));
        assert(request->header_count == 8 && x4tf_header_count(request, "x-xbl-contract-version") == 1 &&
            !strcmp(x4tf_header(request, "x-xbl-contract-version"), "1") &&
            !strcmp(x4tf_header(request, "Content-Type"), "application/json") &&
            !x4tf_header(request, "Authorization") && !x4tf_header(request, "MS-CV"));
        response->body = scenario == NO_GTG ?
            "{\"DisplayClaims\":{\"xui\":[{\"mgt\":\"fixture-ignore-name-51g\",\"xid\":\"fixture-ignore-xid-51g\"}]}}" :
            "{\"Token\":\"fixture-ignore-returned-token-51g\",\"DisplayClaims\":{\"xui\":["
                "{\"gtg\":\"\\u00f1ube51g\",\"xid\":\"fixture-ignore-xid-51g\"}]}}";
        break;
    default:
        assert(!"unexpected extra request");
    }
}

static void check_budget(unsigned operation, const X4TlsBudget *budget)
{
    if (x4tf.requests != 4 || operation != 2) return;
    if (scenario == LATE) atomic_fetch_add(&clock_us, UINT64_C(1100000));
    if (scenario == CANCELLED_AT_DEADLINE || scenario == SIGNED_OUT) {
        atomic_store(&profile_posted, true);
        while (!atomic_load(&allow_reply)) sched_yield();
        atomic_store(&cancel_observed, budget->cancel && atomic_load(budget->cancel));
    }
}

static void begin(void)
{
    x4tf_reset();
    x4tf.on_request = reply;
    x4tf.on_budget = check_budget;
    old_calls = 0;
    worker_created = false;
    atomic_store(&allow_worker, false);
    atomic_store(&profile_posted, false);
    atomic_store(&allow_reply, false);
    atomic_store(&cancel_observed, false);
    atomic_store(&clock_us, UINT64_C(1000000));
}

static void quota(void)
{
    int rc = -1, status = 0;
    X4Http *http = x4_http_open(&rc);
    assert(http && !rc);
    X4CloudTime result;
    assert(!x4_xbox_cloud_time_query(http, "fixture-microsoft-51g", "MX", &cancel_quota,
        &result, NULL, &status));
    assert(status == 200 && result.remaining_s == 1200 && result.max_s == 7200);
    x4_http_close(http);
    /* No profile socket has run yet. Quota returned its original result. */
    assert(old_calls == 3 && x4tf.requests == 3 && !x4tf.live_contexts);
}

static void run_scenario(enum Scenario next)
{
    scenario = next;
    begin();
    char name[32];
    x4_xbox_gamertag(false, name);
    x4_xbox_gamertag(true, name);
    assert(!name[0]);
    quota();
    assert(worker_created);
    if (next == LATE) atomic_fetch_add(&clock_us, UINT64_C(4000000));
    atomic_store(&allow_worker, true);
    if (next == CANCELLED_AT_DEADLINE || next == SIGNED_OUT) {
        while (!atomic_load(&profile_posted)) sched_yield();
        if (next == CANCELLED_AT_DEADLINE) atomic_fetch_add(&clock_us, X4_GAMERTAG_LIMIT_US);
        x4_xbox_gamertag(next != SIGNED_OUT, name);
        if (next == SIGNED_OUT) x4_xbox_gamertag(true, name);
        assert(!name[0]);
        atomic_store(&allow_reply, true);
    }
    assert(!pthread_join(test_thread, NULL));
    assert(x4tf.requests == 4 && !x4tf.live_connections && !x4tf.live_contexts);
    if (next == CANCELLED_AT_DEADLINE || next == SIGNED_OUT) assert(atomic_load(&cancel_observed));
    x4_xbox_gamertag(true, name);
    assert(!strcmp(name, next == WITH_GTG ? "\303\261ube51g" : ""));
    if (next != SIGNED_OUT) {
        begin();
        quota();
        assert(!worker_created); /* Once per authorized account session. */
    }
}

int main(void)
{
    run_scenario(WITH_GTG);
    run_scenario(NO_GTG);
    run_scenario(LATE);
    run_scenario(CANCELLED_AT_DEADLINE);
    run_scenario(SIGNED_OUT);
    puts("T51G_GAMERTAG_PASS old_quota=3 new_xsts=1 exact_body=1 exact_headers=1 gtg=1 absent=1 late=1 cancelled=1 old_account=ignored");
    return 0;
}
