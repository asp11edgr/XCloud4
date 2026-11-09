/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef X4_PKG_HEADER_H
#define X4_PKG_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum { X4_PKG_PREFIX_BYTES = 0x70, X4_PKG_CONTENT_ID_LEN = 36, X4_PKG_TITLE_ID_LEN = 9 };

typedef struct {
    char content_id[X4_PKG_CONTENT_ID_LEN + 1];
    char title_id[X4_PKG_TITLE_ID_LEN + 1];
} X4PkgHeader;

typedef struct {
    char content_id[X4_PKG_CONTENT_ID_LEN + 1];
    char title_id[X4_PKG_TITLE_ID_LEN + 1];
    char app_ver[6];
    char version[48];
} X4PkgSfo;

bool x4_pkg_header_parse(const unsigned char *bytes, size_t size, X4PkgHeader *header);
bool x4_pkg_read_identity(FILE *file, size_t size, X4PkgHeader *header, X4PkgSfo *sfo);

#endif
