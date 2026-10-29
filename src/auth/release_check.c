/* SPDX-License-Identifier: GPL-3.0-only */
#include "release_check.h"
#include "http_client.h"
#include "json.h"
#include "../core/build_identity.h"
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
        *major = *major * 10 + (*p - '0');
        ++p;
    }
    if (*p != '.') return -1;
    ++p;

    if (*p < '0' || *p > '9') return -1;
    *minor = 0;
    while (*p >= '0' && *p <= '9') {
        *minor = *minor * 10 + (*p - '0');
        ++p;
    }
    if (*p != '.') return -1;
    ++p;

    if (*p < '0' || *p > '9') return -1;
    *patch = 0;
    while (*p >= '0' && *p <= '9') {
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

int x4_release_parse_tag(const char *json_body, size_t length, bool is_public,
    char *tag_out, size_t tag_cap, char *ver_out, size_t ver_cap)
{
    if (!json_body || length == 0) return -1;
    X4JsonSpan root;
    if (x4_json_parse(json_body, length, &root) != 0) return -1;

    if (is_public) {
        if (x4_json_type(root) != X4_JSON_T_OBJECT) return -1;
        X4JsonSpan tag_span;
        if (x4_json_member(root, "tag_name", &tag_span) != 1) return -1;
        char tag[64] = {0};
        if (x4_json_token(tag_span, tag, sizeof(tag)) != 0 || !tag[0]) return -1;
        if (tag_out && tag_cap) snprintf(tag_out, tag_cap, "%s", tag);
        const char *ver = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
        if (ver_out && ver_cap) snprintf(ver_out, ver_cap, "%s", ver);
        return 0;
    } else {
        if (x4_json_type(root) != X4_JSON_T_ARRAY) return -1;
        size_t cursor = 0;
        X4JsonSpan item, tag_span;
        while (x4_json_item(root, &cursor, &item) == 1) {
            if (x4_json_type(item) == X4_JSON_T_OBJECT &&
                x4_json_member(item, "tag_name", &tag_span) == 1) {
                char tag[64] = {0};
                if (x4_json_token(tag_span, tag, sizeof(tag)) == 0) {
                    if (!strncmp(tag, "lab-", 4) || !strncmp(tag, "LAB-", 4)) {
                        if (tag_out && tag_cap) snprintf(tag_out, tag_cap, "%s", tag);
                        const char *ver = tag + 4;
                        if (ver_out && ver_cap) snprintf(ver_out, ver_cap, "%s", ver);
                        return 0;
                    }
                }
            }
        }
        return -1;
    }
}

static struct {
    _Atomic int status;
    char latest_version[32];
    char latest_tag[64]; /* validated before publication through status */
    _Atomic int cancel;
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
    .started = false,
    .mock_active = false
};

static bool release_tag_valid(const char *tag)
{
    if (!tag || !tag[0]) return false;
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
    int err = 0;
    X4Http *http = x4_http_open(&err);
    if (!http) {
        if (!check_state.mock_active) atomic_store(&check_state.status, X4_UPDATE_FAILED);
        return NULL;
    }
    char *body = malloc(X4_HTTP_RELEASES_BODY_MAX + 1);
    if (!body) {
        x4_http_close(http);
        if (!check_state.mock_active) atomic_store(&check_state.status, X4_UPDATE_FAILED);
        return NULL;
    }
    size_t length = 0;
    int status_code = 0;
#if X4_PUBLIC
    const char *url = X4_HTTP_RELEASES_URL_PUBLIC;
#else
    const char *url = X4_HTTP_RELEASES_URL_LAB;
#endif
    int rc = x4_http_releases_request(http, url, body, X4_HTTP_RELEASES_BODY_MAX + 1,
        &length, &status_code, &check_state.cancel);
    if (!check_state.mock_active) {
        if (rc == 0 && status_code == 200 && length > 0) {
            char tag[64] = {0};
            char ver[32] = {0};
            if (x4_release_parse_tag(body, length, X4_PUBLIC != 0, tag, sizeof(tag), ver, sizeof(ver)) == 0) {
                if (x4_version_compare(ver, X4_PRODUCT_VERSION) > 0) {
                    snprintf(check_state.latest_version, sizeof(check_state.latest_version), "%s", ver);
                    release_store_tag(tag);
                    atomic_store(&check_state.status, X4_UPDATE_AVAILABLE);
                } else {
                    atomic_store(&check_state.status, X4_UPDATE_LATEST);
                }
            } else {
                atomic_store(&check_state.status, X4_UPDATE_FAILED);
            }
        } else {
            atomic_store(&check_state.status, X4_UPDATE_FAILED);
        }
    }
    free(body);
    x4_http_close(http);
    return NULL;
}

void x4_release_check_start(void)
{
    if (check_state.started) return;
    check_state.started = true;
    if (!check_state.mock_active) atomic_store(&check_state.status, X4_UPDATE_CHECKING);

#ifdef X4_OPENORBIS
    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, 256 * 1024);
    int rc = scePthreadCreate(&check_state.thread, &attr, release_check_worker, NULL, "x4-update");
    scePthreadAttrDestroy(&attr);
    if (rc != 0 && !check_state.mock_active) atomic_store(&check_state.status, X4_UPDATE_FAILED);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(&check_state.thread, &attr, release_check_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0 && !check_state.mock_active) atomic_store(&check_state.status, X4_UPDATE_FAILED);
#endif
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

enum X4UpdateStatus x4_release_check_status(char *version_out, size_t cap)
{
    enum X4UpdateStatus s = (enum X4UpdateStatus)atomic_load(&check_state.status);
    if (version_out && cap) {
        snprintf(version_out, cap, "%s", check_state.latest_version);
    }
    return s;
}

bool x4_release_check_tag(char *out, size_t capacity)
{
    if (out && capacity) out[0] = 0;
    if (!out || !capacity || atomic_load_explicit(&check_state.status, memory_order_acquire) != X4_UPDATE_AVAILABLE ||
        !release_tag_valid(check_state.latest_tag)) return false;
    size_t length = strlen(check_state.latest_tag);
    if (length >= capacity) return false;
    memcpy(out, check_state.latest_tag, length + 1u);
    return true;
}

void x4_release_check_set_mock(enum X4UpdateStatus status, const char *version)
{
    x4_release_check_set_mock_tag(status, version, NULL);
}

void x4_release_check_set_mock_tag(enum X4UpdateStatus status, const char *version, const char *tag)
{
    check_state.mock_active = true;
    if (version) snprintf(check_state.latest_version, sizeof(check_state.latest_version), "%s", version);
    else check_state.latest_version[0] = 0;
    release_store_tag(tag);
    atomic_store(&check_state.status, (int)status);
}
