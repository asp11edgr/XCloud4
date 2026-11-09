/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "update_manifest.h"
#include <stdbool.h>

#define X4_UPDATE51_AUTH_PATH "/data/xcloud4/update/xcloud4-update.auth"
#define X4_UPDATE51_MANUAL_DIR "/data/pkg"
enum { X4_UPDATE51_MANUAL_PATH_CAP = sizeof(X4_UPDATE51_MANUAL_DIR "/") + sizeof(((X4UpdateManifest *)0)->pkg_name) };

typedef struct {
    size_t json_size, signature_size;
    unsigned char json[X4_UPDATE_MANIFEST_MAX], signature[X4_UPDATE_SIGNATURE_MAX];
} X4UpdateSigned51;

int x4_update51_auth_save(const X4UpdateSigned51 *signed_manifest, bool *created);
int x4_update51_auth_read(X4UpdateSigned51 *signed_manifest);
int x4_update51_manual_publish(const char *staged, const X4UpdateManifest *manifest,
    const char *title, const char *path);
