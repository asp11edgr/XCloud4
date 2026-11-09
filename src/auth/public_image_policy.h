/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>

/* Construct the same canonical URL accepted by x4_http_public_image_url.
 * Only the Store image origin and a single safe filename are accepted. */
int x4_public_image_format(const char *uri, unsigned width, unsigned height,
    char *out, size_t capacity);
