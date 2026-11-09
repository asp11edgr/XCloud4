/* SPDX-License-Identifier: GPL-3.0-only */
#include "release_check.h"
#include "http_client.h"
#include "json.h"
#include "../core/app_identity.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

int x4_version_parse(const char *str, int *major, int *minor, int *patch)
{
    if (!str || !str[0] || !major || !minor || !patch) return -1;
    const char *p = str;
    if (*p == 'v' || *p == 'V') ++p;
    else if (!strncmp(p, "lab-", 4) || !strncmp(p, "LAB-", 4)) p += 4;

    if (*p < '0' || *p > '9') return -1;
    *major = 0;
    while (*p >= '0' && *p <= '9') {
        if (*major > (INT_MAX - (*p - '0')) / 10) return -1;
        *major = *major * 10 + (*p - '0');
        ++p;
    }
    if (*p != '.') return -1;
    ++p;

    if (*p < '0' || *p > '9') return -1;
    *minor = 0;
    while (*p >= '0' && *p <= '9') {
        if (*minor > (INT_MAX - (*p - '0')) / 10) return -1;
        *minor = *minor * 10 + (*p - '0');
        ++p;
    }
    if (*p != '.') return -1;
    ++p;

    if (*p < '0' || *p > '9') return -1;
    *patch = 0;
    while (*p >= '0' && *p <= '9') {
        if (*patch > (INT_MAX - (*p - '0')) / 10) return -1;
        *patch = *patch * 10 + (*p - '0');
        ++p;
    }
    /* Suffix is ignored */
    return 0;
}

int x4_version_compare(const char *v1, const char *v2)
{
    int maj1, min1, pat1;
    int maj2, min2, pat2;
    if (x4_version_parse(v1, &maj1, &min1, &pat1) != 0) return -2;
    if (x4_version_parse(v2, &maj2, &min2, &pat2) != 0) return 2;
    if (maj1 != maj2) return maj1 > maj2 ? 1 : -1;
    if (min1 != min2) return min1 > min2 ? 1 : -1;
    if (pat1 != pat2) return pat1 > pat2 ? 1 : -1;
    return 0;
}

static int release_entry(const char *json_body, size_t length, bool is_public,
    char tag[64], X4JsonSpan *entry)
{
    if (!json_body || length == 0) return -1;
    X4JsonSpan root;
    if (x4_json_parse(json_body, length, &root) != 0) return -1;

    X4JsonSpan tag_span;
    if (is_public) {
        if (x4_json_member(root, "tag_name", &tag_span) != 1 ||
                x4_json_token(tag_span, tag, 64) != 0 || !tag[0]) return -1;
        *entry = root;
        return 0;
    }
    size_t cursor = 0;
    X4JsonSpan item;
    while (x4_json_item(root, &cursor, &item) == 1) {
        if (x4_json_member(item, "tag_name", &tag_span) == 1 &&
                x4_json_token(tag_span, tag, 64) == 0 &&
                (!strncmp(tag, "lab-", 4) || !strncmp(tag, "LAB-", 4))) {
            *entry = item;
            return 0;
        }
    }
    return -1;
}

static const char *release_version(const char *tag, bool is_public)
{
    if (!is_public) return tag + 4;
    return tag + (tag[0] == 'v' || tag[0] == 'V');
}

int x4_release_parse_tag(const char *json_body, size_t length, bool is_public,
    char *tag_out, size_t tag_cap, char *ver_out, size_t ver_cap)
{
    char tag[64];
    X4JsonSpan entry;
    if (release_entry(json_body, length, is_public, tag, &entry)) return -1;
    if (tag_out && tag_cap) snprintf(tag_out, tag_cap, "%s", tag);
    if (ver_out && ver_cap) snprintf(ver_out, ver_cap, "%s", release_version(tag, is_public));
    return 0;
}

enum { RELEASE_MANIFEST = 1u, RELEASE_SIGNATURE = 2u };
static unsigned release_asset_bit(const char *name)
{
    if (!name) return 0;
#if X4_PUBLIC
    if (!strcmp(name, X4_UPDATE_PUBLIC_MANIFEST)) return RELEASE_MANIFEST;
    if (!strcmp(name, X4_UPDATE_PUBLIC_SIGNATURE)) return RELEASE_SIGNATURE;
#else
    if (!strcmp(name, X4_UPDATE_LAB_MANIFEST)) return RELEASE_MANIFEST;
    if (!strcmp(name, X4_UPDATE_LAB_SIGNATURE)) return RELEASE_SIGNATURE;
#endif
    return 0;
}

static unsigned release_assets(X4JsonSpan entry)
{
    X4JsonSpan array, item, field;
    if (x4_json_member(entry, "assets", &array) != 1) return 0;
    unsigned seen = 0, duplicates = 0;
    size_t cursor = 0;
    while (x4_json_item(array, &cursor, &item) == 1) {
        char name[64];
        if (x4_json_member(item, "name", &field) != 1 || x4_json_token(field, name, sizeof(name))) continue;
        const unsigned bit = release_asset_bit(name);
        duplicates |= seen & bit;
        seen |= bit;
    }
    return seen & ~duplicates;
}

static struct {
    _Atomic int status;
    char latest_version[32];
    char latest_tag[64]; /* validated before publication through status */
    unsigned latest_assets;
    _Atomic int cancel;
    atomic_bool done;
    bool started;
    bool mock_active;
#ifdef X4_OPENORBIS
    OrbisPthread thread;
#else
    pthread_t thread;
#endif
} check_state = {
    .status = X4_UPDATE_CHECKING,
    .latest_version = {0},
    .latest_tag = {0},
    .cancel = 0,
    .done = true,
    .started = false,
    .mock_active = false
};

/* The main thread can read a result or change the mock while the worker
 * publishes. No lock is held during TLS, allocation or a join. */
static atomic_flag release_guard = ATOMIC_FLAG_INIT;
static void release_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&release_guard, memory_order_acquire)) {}
}

static void release_unlock(void)
{
    atomic_flag_clear_explicit(&release_guard, memory_order_release);
}

static bool release_tag_valid(const char *tag)
{
    if (!tag || !tag[0] || !strcmp(tag, ".") || !strcmp(tag, "..")) return false;
    for (size_t i = 0; i < 64; ++i) {
        unsigned char c = (unsigned char)tag[i];
        if (!c) return i > 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return false;
}

static void release_store_tag(const char *tag)
{
    check_state.latest_tag[0] = 0;
    if (release_tag_valid(tag))
        memcpy(check_state.latest_tag, tag, strlen(tag) + 1u);
}

static void *release_check_worker(void *arg)
{
    (void)arg;
    enum X4UpdateStatus result = X4_UPDATE_FAILED;
    char tag[64] = {0};
    char ver[32] = {0};
    unsigned assets = 0;
    int err = 0;
    X4Http *http = x4_http_open(&err);
    char *body = http ? malloc(X4_HTTP_RELEASES_BODY_MAX + 1) : NULL;
    if (body) {
        size_t length = 0;
        int status_code = 0;
#if X4_PUBLIC
        const char *url = X4_HTTP_RELEASES_URL_PUBLIC;
#else
        const char *url = X4_HTTP_RELEASES_URL_LAB;
#endif
        const int rc = x4_http_releases_request(http, url, body, X4_HTTP_RELEASES_BODY_MAX + 1,
            &length, &status_code, &check_state.cancel);
        X4JsonSpan entry;
        if (rc == 0 && status_code == 200 && !release_entry(body, length, X4_PUBLIC != 0, tag, &entry)) {
            const char *version = release_version(tag, X4_PUBLIC != 0);
            if (x4_version_compare(version, X4_PRODUCT_VERSION) > 0) {
                snprintf(ver, sizeof(ver), "%s", version);
                result = X4_UPDATE_AVAILABLE;
                if (release_tag_valid(tag)) assets = release_assets(entry);
            } else {
                result = X4_UPDATE_LATEST;
            }
        }
        free(body);
    }
    if (http) x4_http_close(http);
    release_lock();
    if (!check_state.mock_active) {
        memcpy(check_state.latest_version, ver, sizeof(ver));
        release_store_tag(result == X4_UPDATE_AVAILABLE ? tag : NULL);
        check_state.latest_assets = assets;
        atomic_store(&check_state.status, result);
    }
    release_unlock();
    atomic_store_explicit(&check_state.done, true, memory_order_release);
    return NULL;
}

static bool release_start(void)
{
    atomic_store(&check_state.cancel, 0);
    atomic_store(&check_state.done, false);
    release_lock();
    if (!check_state.mock_active) {
        check_state.latest_version[0] = 0;
        check_state.latest_tag[0] = 0;
        check_state.latest_assets = 0;
        atomic_store(&check_state.status, X4_UPDATE_CHECKING);
    }
    release_unlock();

#ifdef X4_OPENORBIS
    OrbisPthreadAttr attr;
    int rc = scePthreadAttrInit(&attr);
    if (!rc) {
        rc = scePthreadAttrSetstacksize(&attr, 256u * 1024);
        if (!rc) rc = scePthreadCreate(&check_state.thread, &attr, release_check_worker, NULL, "x4-update");
        scePthreadAttrDestroy(&attr);
    }
#else
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setstacksize(&attr, 256u * 1024);
        if (!rc) rc = pthread_create(&check_state.thread, &attr, release_check_worker, NULL);
        pthread_attr_destroy(&attr);
    }
#endif
    if (rc) {
        atomic_store(&check_state.done, true);
        release_lock();
        if (!check_state.mock_active) atomic_store(&check_state.status, X4_UPDATE_FAILED);
        release_unlock();
        return false;
    }
    check_state.started = true;
    return true;
}

void x4_release_check_start(void)
{
    if (!check_state.started) (void)release_start();
}

void x4_release_check_stop(void)
{
    if (!check_state.started) return;
    atomic_store(&check_state.cancel, 1);
#ifdef X4_OPENORBIS
    scePthreadJoin(check_state.thread, NULL);
#else
    pthread_join(check_state.thread, NULL);
#endif
    check_state.started = false;
}

bool x4_release_check_restart(void)
{
    if (check_state.started && !atomic_load_explicit(&check_state.done, memory_order_acquire)) return false;
    x4_release_check_stop();
    return release_start();
}

enum X4UpdateStatus x4_release_check_status(char *version_out, size_t cap)
{
    if (!version_out || !cap) return (enum X4UpdateStatus)atomic_load(&check_state.status);
    release_lock();
    enum X4UpdateStatus s = (enum X4UpdateStatus)atomic_load(&check_state.status);
    snprintf(version_out, cap, "%s", check_state.latest_version);
    release_unlock();
    return s;
}

bool x4_release_check_tag(char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    out[0] = 0;
    release_lock();
    const size_t length = strlen(check_state.latest_tag);
    const bool valid = atomic_load(&check_state.status) == X4_UPDATE_AVAILABLE && length && length < capacity;
    if (valid) memcpy(out, check_state.latest_tag, length + 1u);
    release_unlock();
    return valid;
}

bool x4_release_check_asset(const char *asset_name)
{
    const unsigned bit = release_asset_bit(asset_name);
    if (!bit) return false;
    release_lock();
    const bool found = atomic_load(&check_state.status) == X4_UPDATE_AVAILABLE &&
        (check_state.latest_assets & bit) != 0;
    release_unlock();
    return found;
}

void x4_release_check_set_mock(enum X4UpdateStatus status, const char *version)
{
    x4_release_check_set_mock_tag(status, version, NULL);
}

void x4_release_check_set_mock_tag(enum X4UpdateStatus status, const char *version, const char *tag)
{
    release_lock();
    check_state.mock_active = true;
    if (version) snprintf(check_state.latest_version, sizeof(check_state.latest_version), "%s", version);
    else check_state.latest_version[0] = 0;
    release_store_tag(tag);
    check_state.latest_assets = 0;
    atomic_store(&check_state.status, (int)status);
    release_unlock();
}
