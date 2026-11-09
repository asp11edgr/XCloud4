/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/update/update_manifest.h"
#include <mbedtls/sha256.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static FILE *open_fixture(const char *dir, const char *name, const char *ext)
{
    char path[1024];
    int len = snprintf(path, sizeof(path), "%s/%s%s", dir, name, ext);
    assert(len > 0 && (size_t)len < sizeof(path));
    FILE *file = fopen(path, "rb");
    assert(file);
    return file;
}

static size_t read_fixture(const char *dir, const char *name, const char *ext,
    unsigned char *out, size_t cap)
{
    FILE *file = open_fixture(dir, name, ext);
    size_t size = fread(out, 1, cap, file);
    assert(size && size < cap && !ferror(file));
    assert(!fclose(file));
    return size;
}

static X4UpdateManifest verify(const char *dir, const char *fixture, const char *edition,
    const char *title, const char *current_version, const char *current_app_ver,
    bool mutate, X4UpdateVerifyResult expected)
{
    unsigned char json[X4_UPDATE_MANIFEST_MAX + 1], sig[X4_UPDATE_SIGNATURE_MAX + 1], der[128];
    const size_t size = read_fixture(dir, fixture, ".json", json, sizeof(json));
    const size_t sig_size = read_fixture(dir, fixture, ".sig", sig, sizeof(sig));
    const size_t der_size = read_fixture(dir, "public", ".der", der, sizeof(der));
    if (mutate) json[size - 1] = ' ';
    X4UpdateManifest result;
    X4UpdateVerifyResult actual = x4_update_manifest_verify(json, size, sig, sig_size, der, der_size,
        edition, title, current_version, current_app_ver, &result);
    if (actual != expected) fprintf(stderr, "%s: expected %d, observed %d\n", fixture, expected, actual);
    assert(actual == expected);
    assert((result.pkg_size != 0) == (actual == X4_UPDATE_VERIFY_OK));
    return result;
}

static void package_verify(const char *dir, const char *name, X4UpdateManifest *manifest,
    bool refresh_hash, X4UpdateVerifyResult expected)
{
    X4UpdateManifest copy = *manifest;
    if (refresh_hash) {
        unsigned char bytes[2048];
        copy.pkg_size = (uint32_t)read_fixture(dir, name, ".pkg", bytes, sizeof(bytes));
        assert(!mbedtls_sha256(bytes, copy.pkg_size, copy.pkg_sha256, 0));
    }
    FILE *file = open_fixture(dir, name, ".pkg");
    X4UpdateVerifyResult actual = x4_update_pkg_verify(file, &copy, "XCLD00001");
    if (actual != expected) fprintf(stderr, "PKG %s: expected %d, observed %d\n", name, expected, actual);
    assert(actual == expected);
    assert(!fclose(file));
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    const char *dir = argv[1];
    const char *lab = "lab", *lab_title = "XCLD00001", *lab_version = "0.7.99-lab", *lab_app = "01.67";
    X4UpdateManifest manifest = verify(dir, "valid", lab, lab_title, lab_version, lab_app,
        false, X4_UPDATE_VERIFY_OK);
    assert(!strcmp(manifest.version, "0.7.100-lab") && !strcmp(manifest.app_ver, "01.68"));
    verify(dir, "valid", lab, lab_title, "0.7.100-lab", "01.68", false, X4_UPDATE_VERIFY_VERSION);
    verify(dir, "valid", lab, lab_title, lab_version, lab_app, true, X4_UPDATE_VERIFY_SIGNATURE);
    verify(dir, "wrong_key", lab, lab_title, lab_version, lab_app, false, X4_UPDATE_VERIFY_SIGNATURE);
    const struct { const char *name; X4UpdateVerifyResult expected; } cases[] = {
        {"edition", X4_UPDATE_VERIFY_MANIFEST}, {"title", X4_UPDATE_VERIFY_MANIFEST},
        {"same_version", X4_UPDATE_VERIFY_VERSION}, {"older_version", X4_UPDATE_VERIFY_VERSION},
        {"same_app_ver", X4_UPDATE_VERIFY_VERSION}, {"older_app_ver", X4_UPDATE_VERIFY_VERSION},
        {"too_big", X4_UPDATE_VERIFY_MANIFEST}, {"unsafe_name", X4_UPDATE_VERIFY_MANIFEST},
        {"bad_digest", X4_UPDATE_VERIFY_MANIFEST},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        verify(dir, cases[i].name, lab, lab_title, lab_version, lab_app, false, cases[i].expected);
    verify(dir, "public_stable", "public", "XCLD00003", "1.3.0-beta", "01.41",
        false, X4_UPDATE_VERIFY_OK);
    verify(dir, "beta_order", "public", "XCLD00003", "1.3.0-beta.2", "01.41",
        false, X4_UPDATE_VERIFY_OK);
    package_verify(dir, "valid", &manifest, false, X4_UPDATE_VERIFY_OK);
    for (size_t i = 0; i < 4; ++i) {
        const char *bad[] = {"wrong_magic", "other_title", "wrong_app_ver", "wrong_version"};
        package_verify(dir, bad[i], &manifest, true, X4_UPDATE_VERIFY_PACKAGE);
    }
    ++manifest.pkg_size;
    package_verify(dir, "valid", &manifest, false, X4_UPDATE_VERIFY_PACKAGE);
    --manifest.pkg_size;
    manifest.pkg_sha256[0] ^= 1;
    package_verify(dir, "valid", &manifest, false, X4_UPDATE_VERIFY_HASH);
    if (argc == 3) {
        X4UpdateManifest real = verify(dir, "real_pkg", lab, lab_title, lab_version, lab_app,
            false, X4_UPDATE_VERIFY_OK);
        FILE *file = fopen(argv[2], "rb");
        assert(file);
        assert(x4_update_pkg_verify(file, &real, lab_title) == X4_UPDATE_VERIFY_OK);
        assert(!fclose(file));
        printf("PKG real validado: %s APP_VER %s\n", real.version, real.app_ver);
    }
    puts("PASS update signature, signed fields, version order, package identity, length and SHA-256");
    return 0;
}
