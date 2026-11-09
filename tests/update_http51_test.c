/* SPDX-License-Identifier: GPL-3.0-only */
#define main tls_fixture_main
#define x4_secure_clear tls_fixture_secure_clear
#include "tls_conn_test.c"
#undef x4_secure_clear
#undef main
#include <mbedtls/sha256.h>

#define RELEASE_TAG "v1.2.1-beta"
#define RELEASE_ASSET "XCloud4-1.2.1-beta-publico.pkg"
#define RELEASE_SIZE 16646144u

typedef struct {
    Server socket;
    const char *const *responses;
    unsigned count, served;
} UpdateServer;

static UpdateServer *localhost_server;
static unsigned outbound, verified_connections, live_github, live_assets;

uint64_t sceKernelGetProcessTime(void)
{
    return x4_tls_now_us();
}

X4TlsHttp *__real_x4_tls_http_open(int *);
X4TlsHttp *__wrap_x4_tls_http_open(int *error)
{
    if (!localhost_server) return __real_x4_tls_http_open(error);
    const uint8_t *const roots[] = {fixture.ca.bytes};
    const size_t sizes[] = {fixture.ca.length};
    return x4_tls_http_test_open(roots, sizes, 1, error);
}

int __real_x4_tls_conn_connect(X4TlsHttp *, const char *, const X4TlsBudget *, X4TlsConn **);
int __wrap_x4_tls_conn_connect(X4TlsHttp *http, const char *host, const X4TlsBudget *budget, X4TlsConn **out)
{
    CHECK(!strcmp(host, "github.com") || !strcmp(host, "objects.githubusercontent.com") ||
        !strcmp(host, "release-assets.githubusercontent.com"));
    int rc = localhost_server ? x4_tls_conn_test_connect(http, "localhost",
        localhost_server->socket.port, budget, out) : __real_x4_tls_conn_connect(http, host, budget, out);
    if (!rc) {
        const X4TlsInfo info = x4_tls_conn_info(*out);
        CHECK(info.verified && !info.verify_flags);
        ++verified_connections;
        if (!localhost_server) {
            if (!strcmp(host, "github.com")) ++live_github;
            if (!strcmp(host, "release-assets.githubusercontent.com")) ++live_assets;
            printf("UPDATE51_VERIFIED_TLS host=%s protocol=%s anchor=%s\n",
                host, info.version, info.anchor_name);
        }
    }
    return rc;
}

int __real_x4_tls_conn_write(X4TlsConn *, const void *, size_t, const X4TlsBudget *);
int __wrap_x4_tls_conn_write(X4TlsConn *conn, const void *bytes, size_t size, const X4TlsBudget *budget)
{
    /* Keep presigned Location/query parameters out of the console or logs.
     * Every emitted header name/value must match this closed wire contract. */
    const char *begin = bytes;
    char *wire = malloc(size + 1);
    CHECK(wire);
    memcpy(wire, bytes, size);
    wire[size] = 0;
    CHECK(size >= 4 && !memcmp(begin, "GET ", 4));
    char *headers = strstr(wire, "\r\n");
    CHECK(headers && headers[-1] == '1');
    headers += 2;
    const char *hosts[] = {"github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"};
    const char *host = NULL;
    for (unsigned i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i) {
        char expected[300];
        int n = snprintf(expected, sizeof(expected), "Host: %s\r\nAccept: application/octet-stream\r\n"
            "Accept-Encoding: identity\r\nUser-Agent: XCloud4\r\nConnection: close\r\n\r\n", hosts[i]);
        CHECK(n > 0 && (size_t)n < sizeof(expected));
        if ((size_t)n == size - (size_t)(headers - wire) && !memcmp(headers, expected, (size_t)n))
            host = hosts[i];
    }
    CHECK(host);
    if (!localhost_server) printf("UPDATE51_OUTBOUND_HEADERS\n%s", headers);
    tls_fixture_secure_clear(wire, size + 1);
    free(wire);
    ++outbound;
    return __real_x4_tls_conn_write(conn, bytes, size, budget);
}

static void serve_reply(Server *server, mbedtls_ssl_config *config, const char *response)
{
    const uint64_t deadline = monotonic_us() + UINT64_C(8000000);
    struct pollfd listener = {.fd = server->listener, .events = POLLIN};
    CHECK(poll(&listener, 1, 8000) == 1);
    server->peer = accept(server->listener, NULL, NULL);
    CHECK(server->peer >= 0);
    CHECK(fcntl(server->peer, F_SETFL, fcntl(server->peer, F_GETFL, 0) | O_NONBLOCK) == 0);
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    MBED_OK(mbedtls_ssl_setup(&ssl, config));
    mbedtls_ssl_set_bio(&ssl, server, server_send, server_receive, NULL);
    for (;;) {
        int rc = mbedtls_ssl_handshake(&ssl);
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            MBED_OK(rc);
            break;
        }
        CHECK(server_wait(server, server->peer, rc, deadline));
    }
    char request[512] = {0};
    size_t used = 0;
    while (!strstr(request, "\r\n\r\n")) {
        CHECK(used < sizeof(request) - 1);
        int rc = mbedtls_ssl_read(&ssl, (unsigned char *)request + used, sizeof(request) - 1 - used);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            CHECK(server_wait(server, server->peer, rc, deadline));
            continue;
        }
        CHECK(rc > 0);
        used += (size_t)rc;
        request[used] = 0;
    }
    CHECK(!strncmp(request, "GET ", 4));
    for (size_t offset = 0, length = strlen(response); offset < length;) {
        int rc = mbedtls_ssl_write(&ssl, (const unsigned char *)response + offset, length - offset);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            CHECK(server_wait(server, server->peer, rc, deadline));
            continue;
        }
        CHECK(rc > 0);
        offset += (size_t)rc;
    }
    (void)mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    CHECK(close(server->peer) == 0);
    server->peer = -1;
}

static void *serve_updates(void *context)
{
    UpdateServer *local = context;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_ssl_config config;
    mbedtls_x509_crt crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&rng);
    mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&crt);
    static const unsigned char purpose[] = "XCloud4 T51 update loopback fixture";
    MBED_OK(mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy, purpose, sizeof(purpose) - 1));
    MBED_OK(mbedtls_x509_crt_parse_der(&crt, fixture.valid.bytes, fixture.valid.length));
    MBED_OK(mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &rng);
    mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
    MBED_OK(mbedtls_ssl_conf_own_cert(&config, &crt, &fixture.server_key));
    while (local->served < local->count) {
        serve_reply(&local->socket, &config, local->responses[local->served]);
        ++local->served;
    }
    CHECK(close(local->socket.listener) == 0);
    mbedtls_x509_crt_free(&crt);
    mbedtls_ssl_config_free(&config);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&entropy);
    return NULL;
}

static void start_server(UpdateServer *local)
{
    Server *server = &local->socket;
    atomic_init(&server->stop, false);
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(server->listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(server->listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t length = sizeof(addr);
    CHECK(getsockname(server->listener, (struct sockaddr *)&addr, &length) == 0);
    server->port = ntohs(addr.sin_port);
    CHECK(server->port && listen(server->listener, 1) == 0);
    CHECK(pthread_create(&server->thread, NULL, serve_updates, local) == 0);
}

typedef struct {
    mbedtls_sha256_context hash;
    size_t bytes;
    _Atomic int *cancel;
} UpdateSink;

static int update_write(void *context, const void *bytes, size_t count)
{
    UpdateSink *sink = context;
    MBED_OK(mbedtls_sha256_update(&sink->hash, bytes, count));
    sink->bytes += count;
    if (sink->cancel) atomic_store(sink->cancel, 1);
    return 0;
}

static void sink_init(UpdateSink *sink)
{
    mbedtls_sha256_init(&sink->hash);
    MBED_OK(mbedtls_sha256_starts(&sink->hash, 0));
}

static void sink_digest(UpdateSink *sink, char hex[65])
{
    unsigned char sha[32];
    MBED_OK(mbedtls_sha256_finish(&sink->hash, sha));
    mbedtls_sha256_free(&sink->hash);
    for (size_t i = 0; i < sizeof(sha); ++i) snprintf(hex + i * 2, 3, "%02x", sha[i]);
}

static void local_case(const char *label, const char *const *responses, unsigned count,
    int expected, size_t limit, size_t sink_bytes, bool cancel_after_write, const char *expected_sha)
{
    UpdateServer server = {.responses = responses, .count = count};
    start_server(&server);
    localhost_server = &server;
    const unsigned prior = outbound;
    int error = -1;
    X4Http *http = x4_http_update_open(&error);
    CHECK(http && !error);
    _Atomic int cancel = 0;
    UpdateSink sink = {.cancel = cancel_after_write ? &cancel : NULL};
    sink_init(&sink);
    size_t received = SIZE_MAX;
    const int rc = x4_http_update_asset(http, RELEASE_TAG, RELEASE_ASSET, limit,
        update_write, &sink, &received, &cancel);
    x4_http_close(http);
    CHECK(pthread_join(server.socket.thread, NULL) == 0);
    localhost_server = NULL;
    char sha[65];
    sink_digest(&sink, sha);
    CHECK(outbound - prior == count && server.served == count);
    CHECK(rc == expected && received == (expected ? 0 : sink_bytes) && sink.bytes == sink_bytes);
    if (expected_sha) CHECK(!strcmp(sha, expected_sha));
    printf("UPDATE51_LOCAL_PASS %s requests=%u result=%d sink_bytes=%zu\n", label, count, rc, sink.bytes);
}

static void local_suite(void)
{
    const char *const safe[] = {
        "HTTP/1.1 103 Early Hints\r\nLocation: https://example.invalid/ignored\r\n\r\n"
        "HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/a?x=%2F\r\n"
        "Content-Length: 9\r\n\r\nuntrusted",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhe\r\n"
        "3\r\nllo\r\n0\r\nLocation: https://example.invalid/ignored-trailer\r\n\r\n"
    };
    const char *const foreign[] = {
        "HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com.evil.invalid/a\r\n\r\n"
    };
    const char *const downgraded[] = {
        "HTTP/1.1 302 Found\r\nLocation: http://github.com/a\r\n\r\n"
    };
    const char *const userinfo[] = {
        "HTTP/1.1 302 Found\r\nLocation: https://github.com@objects.githubusercontent.com/a\r\n\r\n"
    };
    const char *const port[] = {
        "HTTP/1.1 302 Found\r\nLocation: https://github.com:443/a\r\n\r\n"
    };
    const char *const traversal[] = {
        "HTTP/1.1 302 Found\r\nLocation: https://github.com/a/%2e./b\r\n\r\n"
    };
    const char *const duplicate[] = {
        "HTTP/1.1 302 Found\r\nLocation: https://github.com/a\r\nlocation: https://github.com/b\r\n\r\n"
    };
    const char *const excessive[] = {
        "HTTP/1.1 301 Moved\r\nLocation: https://objects.githubusercontent.com/a\r\n\r\n",
        "HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/b\r\n\r\n",
        "HTTP/1.1 307 Temporary\r\nLocation: https://github.com/c\r\n\r\n",
        "HTTP/1.1 308 Permanent\r\nLocation: https://github.com/d\r\n\r\n",
    };
    const char *const at_limit[] = {
        excessive[0], excessive[1], excessive[2],
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
    };
    const char *const partial[] = {"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nhello"};
    const char *const limited[] = {"HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\n123456789"};
    const char *const bad_status[] = {"HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\n\r\nhello"};
    const char hello[] = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    local_case("verified-redirect-and-chunked", safe, 2, 0, 5, 5, false, hello);
    local_case("off-list-host", foreign, 1, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("https-only", downgraded, 1, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("userinfo", userinfo, 1, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("explicit-port", port, 1, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("dot-traversal", traversal, 1, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("duplicate-location", duplicate, 1, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("three-hops", at_limit, 4, 0, 5, 5, false, hello);
    local_case("fourth-hop-rejected", excessive, 4, X4_HTTP_UPDATE_REDIRECT, 8, 0, false, NULL);
    local_case("non-200", bad_status, 1, X4_HTTP_UPDATE_STATUS, 8, 0, false, NULL);
    local_case("partial-entity", partial, 1, X4_HTTP_PROTOCOL, 8, 5, false, hello);
    local_case("cancellation", partial, 1, X4_HTTP_CANCELLED, 8, 5, true, hello);
    local_case("content-limit", limited, 1, X4_HTTP_BOUND, 8, 0, false, NULL);
    CHECK(outbound == verified_connections);
}

static void live_suite(void)
{
    int error = -1;
    X4Http *http = x4_http_update_open(&error);
    CHECK(http && !error);
    UpdateSink sink = {0};
    sink_init(&sink);
    size_t bytes = 0;
    const int rc = x4_http_update_asset(http, RELEASE_TAG, RELEASE_ASSET, RELEASE_SIZE,
        update_write, &sink, &bytes, NULL);
    if (rc) fprintf(stderr, "UPDATE51_LIVE_FAIL code=%d stage=%s bytes=%zu\n", rc, x4_http_stage(http), sink.bytes);
    x4_http_close(http);
    char sha[65];
    sink_digest(&sink, sha);
    CHECK(rc == 0 && bytes == RELEASE_SIZE && sink.bytes == bytes);
    CHECK(!strcmp(sha, "69456fe3e5349a5f9eca12b16e1d94a02b5f9e481ff65eff84329981295c0e33"));
    CHECK(verified_connections == outbound && live_github == 1 && live_assets == 1);
    printf("UPDATE51_LIVE_PASS bytes=%zu sha256=%s verified_connections=%u\n", bytes, sha, verified_connections);
}

int main(int argc, char **argv)
{
    private_fixture_process();
    if (argc == 2 && !strcmp(argv[1], "--live")) {
        live_suite();
        return 0;
    }
    CHECK(argc == 1);
    int error = -1;
    X4TlsHttp *guard = __real_x4_tls_http_open(&error);
    CHECK(guard && !error && !x4_tls_http_enable_update_ca(guard));
    X4TlsConn *connection = NULL;
    const X4TlsBudget budget = {.deadline_us = x4_tls_now_us() + UINT64_C(1000000)};
    CHECK(__real_x4_tls_conn_connect(guard, "login.microsoftonline.com", &budget, &connection) == X4_HTTP_URL);
    CHECK(!connection && !x4_tls_http_close(guard));
    puts("UPDATE51_CA_SCOPE ordinary_host_rejected=1");
    certificates_open();
    local_suite();
    certificates_close();
    return 0;
}
