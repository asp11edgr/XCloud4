/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* RGBA bytes to XCloud4 0xAABBGGRR words. Destination stride and extent
 * are measured in pixels. Source and destination must not overlap. */
int x4_image_area(const unsigned char *source, size_t source_bytes,
    unsigned sw, unsigned sh, uint32_t *out, size_t out_pixels,
    unsigned dw, unsigned dh, unsigned stride);
/* Copy a centered crop at 1:1 when the source covers the destination.
 * Otherwise sample the centered region of a single area scale to cover.
 * The scaled image outside the crop is never allocated or rendered. */
int x4_image_area_cover(const unsigned char *source, size_t source_bytes,
    unsigned sw, unsigned sh, uint32_t *out, size_t out_pixels,
    unsigned dw, unsigned dh, unsigned stride);
