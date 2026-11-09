/* SPDX-License-Identifier: GPL-3.0-only */
#include "update_manifest.h"
#include "update_io51.h"
#include "pkg_header.h"
#include "../auth/json.h"
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#if !defined(MBEDTLS_ECDSA_C) || !defined(MBEDTLS_ECP_DP_SECP256R1_ENABLED)
#error XCloud4 update manifests require ECDSA P-256 verification
#endif

typedef struct {
    const char *begin;
    size_t len;
} Span;

typedef struct {
    Span component[3];
    const char *prerelease;
} Version;

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool alnum_ascii(char c)
{
    return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool numeric(Span value)
{
    for (size_t i = 0; i < value.len; ++i) {
        if (!digit(value.begin[i])) return false;
    }
    return true;
}

static Span next_identifier(const char **cursor)
{
    const char *start = *cursor;
    while (**cursor && **cursor != '.') ++*cursor;
    Span value = {start, (size_t)(*cursor - start)};
    return value;
}

static bool version_parse(const char *text, Version *out)
{
    const char *p = text;
    for (size_t i = 0; i < 3; ++i) {
        const char *start = p;
        while (digit(*p)) ++p;
        const size_t len = (size_t)(p - start);
        if (!len || (len > 1 && *start == '0')) return false;
        out->component[i] = (Span){start, len};
        if (i < 2 && *p++ != '.') return false;
    }
    out->prerelease = NULL;
    if (!*p) return true;
    if (*p++ != '-') return false;
    out->prerelease = p;
    do {
        const Span id = next_identifier(&p);
        if (!id.len || (id.len > 1 && id.begin[0] == '0' && numeric(id))) return false;
        for (size_t i = 0; i < id.len; ++i) {
            if (!alnum_ascii(id.begin[i]) && id.begin[i] != '-') return false;
        }
    } while (*p && *p++ == '.');
    return true;
}

static int compare_span(Span a, Span b, bool numbers)
{
    if (numbers && a.len != b.len) return a.len > b.len ? 1 : -1;
    int result = memcmp(a.begin, b.begin, a.len < b.len ? a.len : b.len);
    if (result) return result;
    return (a.len > b.len) - (a.len < b.len);
}

static int version_compare(const Version *a, const Version *b)
{
    for (size_t i = 0; i < 3; ++i) {
        int result = compare_span(a->component[i], b->component[i], true);
        if (result) return result;
    }
    const char *ap = a->prerelease, *bp = b->prerelease;
    if (!ap || !bp) return (!ap) - (!bp);
    for (;;) {
        const Span ai = next_identifier(&ap), bi = next_identifier(&bp);
        bool an = numeric(ai), bn = numeric(bi);
        if (an != bn) return an ? -1 : 1;
        int result = compare_span(ai, bi, an);
        if (result) return result;
        if (!*ap || !*bp) return (*ap != 0) - (*bp != 0);
        ++ap;
        ++bp;
    }
}

static bool valid_app_ver(const char *text)
{
    return strlen(text) == 5 && digit(text[0]) && digit(text[1]) && text[2] == '.' &&
        digit(text[3]) && digit(text[4]);
}

bool x4_update_manifest_is_applied(const X4UpdateManifest *manifest,
    const char *current_version, const char *current_app_ver)
{
    Version current, target;
    return manifest && current_version && current_app_ver &&
        valid_app_ver(current_app_ver) && valid_app_ver(manifest->app_ver) &&
        strcmp(current_app_ver, manifest->app_ver) >= 0 &&
        version_parse(current_version, &current) && version_parse(manifest->version, &target) &&
        version_compare(&current, &target) >= 0;
}

static bool valid_pkg_name(const char *text)
{
    const size_t len = strlen(text);
    if (len < 5 || !alnum_ascii(text[0]) || strcmp(text + len - 4, ".pkg")) return false;
    for (size_t i = 1; i < len; ++i) {
        if (!alnum_ascii(text[i]) && text[i] != '.' && text[i] != '_' && text[i] != '-') return false;
    }
    return true;
}

static int hex_digit(char c)
{
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_digest(const char *hex, unsigned char out[32])
{
    if (strlen(hex) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int upper = hex_digit(hex[i * 2]), lower = hex_digit(hex[i * 2 + 1]);
        if (upper < 0 || lower < 0) return false;
        out[i] = (unsigned char)(upper * 16 + lower);
    }
    return true;
}

static bool valid_signature(const void *json, size_t json_len,
    const unsigned char *signature, size_t sig_len, const unsigned char *pub_der, size_t pub_der_len)
{
    if (!signature || !pub_der || sig_len < 8 || sig_len > X4_UPDATE_SIGNATURE_MAX ||
        pub_der_len > 128 || !pub_der_len || pub_der[0] != 0x30) return false;
    unsigned char hash[32];
    if (mbedtls_sha256(json, json_len, hash, 0)) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool valid = !mbedtls_pk_parse_public_key(&key, pub_der, pub_der_len) &&
        mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA);
    mbedtls_ecp_keypair *ec = valid ? mbedtls_pk_ec(key) : NULL;
    valid = ec && mbedtls_ecp_keypair_get_group_id(ec) == MBEDTLS_ECP_DP_SECP256R1 &&
        !mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, sig_len);
    mbedtls_pk_free(&key);
    return valid;
}

X4UpdateVerifyResult x4_update_manifest_verify(const void *json, size_t json_len,
    const unsigned char *signature, size_t sig_len, const unsigned char *pub_der,
    size_t pub_der_len, const char *edition, const char *title_id,
    const char *current_version, const char *current_app_ver, X4UpdateManifest *out)
{
    if (!out) return X4_UPDATE_VERIFY_MANIFEST;
    *out = (X4UpdateManifest){0};
    if (!json || !json_len || json_len > X4_UPDATE_MANIFEST_MAX || !edition || !title_id ||
        !current_version || !current_app_ver) return X4_UPDATE_VERIFY_MANIFEST;
    if (!valid_signature(json, json_len, signature, sig_len, pub_der, pub_der_len))
        return X4_UPDATE_VERIFY_SIGNATURE;

    X4UpdateManifest candidate = {0};
    char manifest_edition[7], manifest_title[10], digest[65];
    enum { SCHEMA, EDITION, TITLE_ID, VERSION, APP_VER, PKG_NAME, PKG_SIZE, SHA256 };
    X4JsonField fields[] = {
        [SCHEMA] = {.name = "schema", .kind = X4_JSON_UINT},
        [EDITION] = {.name = "edition", .kind = X4_JSON_STRING, .text = manifest_edition, .capacity = sizeof(manifest_edition)},
        [TITLE_ID] = {.name = "title_id", .kind = X4_JSON_STRING, .text = manifest_title, .capacity = sizeof(manifest_title)},
        [VERSION] = {.name = "version", .kind = X4_JSON_STRING, .text = candidate.version, .capacity = sizeof(candidate.version)},
        [APP_VER] = {.name = "app_ver", .kind = X4_JSON_STRING, .text = candidate.app_ver, .capacity = sizeof(candidate.app_ver)},
        [PKG_NAME] = {.name = "pkg_name", .kind = X4_JSON_STRING, .text = candidate.pkg_name, .capacity = sizeof(candidate.pkg_name)},
        [PKG_SIZE] = {.name = "pkg_size", .kind = X4_JSON_UINT},
        [SHA256] = {.name = "pkg_sha256", .kind = X4_JSON_STRING, .text = digest, .capacity = sizeof(digest)},
    };
    const size_t count = sizeof(fields) / sizeof(fields[0]);
    if (x4_json_fields(json, json_len, fields, count)) return X4_UPDATE_VERIFY_MANIFEST;
    for (size_t i = 0; i < count; ++i) {
        if (!fields[i].found) return X4_UPDATE_VERIFY_MANIFEST;
    }
    candidate.pkg_size = fields[PKG_SIZE].number;
    Version target, current;
    if (fields[SCHEMA].number != 1 || strcmp(manifest_edition, edition) ||
        strcmp(manifest_title, title_id) || candidate.pkg_size < X4_PKG_PREFIX_BYTES ||
        candidate.pkg_size > X4_UPDATE_PACKAGE_MAX || !valid_pkg_name(candidate.pkg_name) ||
        !parse_digest(digest, candidate.pkg_sha256) || !valid_app_ver(candidate.app_ver) ||
        !valid_app_ver(current_app_ver) || !version_parse(candidate.version, &target) ||
        !version_parse(current_version, &current)) return X4_UPDATE_VERIFY_MANIFEST;
    if (strcmp(candidate.app_ver, current_app_ver) <= 0 || version_compare(&target, &current) <= 0)
        return X4_UPDATE_VERIFY_VERSION;
    *out = candidate;
    return X4_UPDATE_VERIFY_OK;
}

X4UpdateVerifyResult x4_update_pkg_verify_title(FILE *file, const X4UpdateManifest *manifest,
    const char *title_id, char verified_title[10])
{
    if (!file || !manifest || !title_id) { errno = EINVAL; return X4_UPDATE_VERIFY_IO; }
    uint64_t file_size = 0;
    if (x4_update51_file_size(file, &file_size)) return X4_UPDATE_VERIFY_IO;
    if (manifest->pkg_size > X4_UPDATE_PACKAGE_MAX || file_size != manifest->pkg_size)
        return X4_UPDATE_VERIFY_PACKAGE;

    unsigned char block[4096], hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int rc = mbedtls_sha256_starts(&sha, 0);
    size_t remaining = manifest->pkg_size;
    while (!rc && remaining) {
        size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
        rc = x4_update51_read_all(file, block, count);
        if (rc) break;
        rc = mbedtls_sha256_update(&sha, block, count);
        remaining -= count;
    }
    if (!rc) rc = mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (rc || ferror(file)) return X4_UPDATE_VERIFY_IO;
    if (x4_update51_expect_eof(file))
        return ferror(file) ? X4_UPDATE_VERIFY_IO : X4_UPDATE_VERIFY_PACKAGE;
    if (memcmp(hash, manifest->pkg_sha256, sizeof(hash))) return X4_UPDATE_VERIFY_HASH;

    X4PkgHeader header;
    X4PkgSfo sfo;
    errno = 0;
    if (!x4_pkg_read_identity(file, manifest->pkg_size, &header, &sfo))
        return errno ? X4_UPDATE_VERIFY_IO : X4_UPDATE_VERIFY_PACKAGE;
    if (strcmp(header.title_id, title_id) || strcmp(sfo.title_id, title_id) ||
        strcmp(header.content_id, sfo.content_id) ||
        strcmp(sfo.app_ver, manifest->app_ver) || strcmp(sfo.version, manifest->version))
        return X4_UPDATE_VERIFY_PACKAGE;
    if (verified_title) memcpy(verified_title, header.title_id, sizeof(header.title_id));
    return X4_UPDATE_VERIFY_OK;
}

X4UpdateVerifyResult x4_update_pkg_verify(FILE *file, const X4UpdateManifest *manifest,
    const char *title_id)
{
    return x4_update_pkg_verify_title(file, manifest, title_id, NULL);
}
