/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef X4_UPDATE_MANIFEST_H
#define X4_UPDATE_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    X4_UPDATE_MANIFEST_MAX = 4096,
    X4_UPDATE_SIGNATURE_MAX = 72,
    X4_UPDATE_PACKAGE_MAX = 64 * 1024 * 1024,
};

typedef struct {
    char version[48];
    char app_ver[6];
    char pkg_name[129];
    uint32_t pkg_size;
    unsigned char pkg_sha256[32];
} X4UpdateManifest;

typedef enum {
    X4_UPDATE_VERIFY_OK = 0,
    X4_UPDATE_VERIFY_SIGNATURE = -1,
    X4_UPDATE_VERIFY_MANIFEST = -2,
    X4_UPDATE_VERIFY_VERSION = -3,
    X4_UPDATE_VERIFY_PACKAGE = -4,
    X4_UPDATE_VERIFY_HASH = -5,
    X4_UPDATE_VERIFY_IO = -6,
} X4UpdateVerifyResult;

/* Authenticate the exact bytes before accepting any signed metadata. */
X4UpdateVerifyResult x4_update_manifest_verify(const void *json, size_t json_len,
    const unsigned char *signature, size_t sig_len, const unsigned char *pub_der,
    size_t pub_der_len, const char *edition, const char *title_id,
    const char *current_version, const char *current_app_ver, X4UpdateManifest *out);

bool x4_update_manifest_is_applied(const X4UpdateManifest *manifest,
    const char *current_version, const char *current_app_ver);

/* File must be open for reading and seekable. Caller retains ownership. */
X4UpdateVerifyResult x4_update_pkg_verify(FILE *file, const X4UpdateManifest *manifest,
    const char *title_id);
X4UpdateVerifyResult x4_update_pkg_verify_title(FILE *file, const X4UpdateManifest *manifest,
    const char *title_id, char verified_title[10]);

#endif
