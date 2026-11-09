/* SPDX-License-Identifier: GPL-3.0-only */
#include "image_area.h"
#include <string.h>

static uint64_t overlap(uint64_t start, uint64_t end, unsigned cell, unsigned step)
{
    uint64_t left = (uint64_t)cell * step;
    uint64_t right = left + step;
    if (left < start) left = start;
    if (right > end) right = end;
    return right - left;
}

static int valid_dimensions(const unsigned char *source, size_t source_bytes,
    unsigned sw, unsigned sh, uint32_t *out, size_t out_pixels,
    unsigned dw, unsigned dh, unsigned stride)
{
    return source && out && sw && sh && dw && dh &&
        sw <= 2048 && sh <= 2048 && dw <= 2048 && dh <= 2048 &&
        stride >= dw && stride <= 2048 &&
        source_bytes >= (size_t)sw * sh * 4 &&
        out_pixels >= (size_t)(dh - 1) * stride + dw;
}

static void sample_region(const unsigned char *source, unsigned sw, unsigned sh,
    unsigned scaled_w, unsigned scaled_h, unsigned ox, unsigned oy,
    uint32_t *out, unsigned dw, unsigned dh, unsigned stride)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (sw == scaled_w && sh == scaled_h) {
        for (unsigned y = 0; y < dh; ++y)
            memcpy(out + (size_t)y * stride,
                source + ((size_t)(y + oy) * sw + ox) * 4, (size_t)dw * 4);
        return;
    }
#endif
    /* Each destination rectangle has exact area sw*sh in integer units.
     * Source cells occupy scaled_w*scaled_h of those units. Use the overlap weights
     * directly so odd sizes and enlargement follow the same rule. */
    const uint64_t area = (uint64_t)sw * sh;
    for (unsigned y = 0; y < dh; ++y) {
        uint64_t top = (uint64_t)(y + oy) * sh, bottom = top + sh;
        unsigned sy_first = (unsigned)(top / scaled_h);
        unsigned sy_last = (unsigned)((bottom - 1) / scaled_h);
        for (unsigned x = 0; x < dw; ++x) {
            uint64_t left = (uint64_t)(x + ox) * sw, right = left + sw;
            unsigned sx_first = (unsigned)(left / scaled_w);
            unsigned sx_last = (unsigned)((right - 1) / scaled_w);
            uint64_t sum[4] = {0};
            for (unsigned sy = sy_first; sy <= sy_last; ++sy) {
                uint64_t wy = overlap(top, bottom, sy, scaled_h);
                for (unsigned sx = sx_first; sx <= sx_last; ++sx) {
                    uint64_t weight = wy * overlap(left, right, sx, scaled_w);
                    const unsigned char *pixel = source + ((size_t)sy * sw + sx) * 4;
                    for (unsigned c = 0; c < 4; ++c) sum[c] += weight * pixel[c];
                }
            }
            uint32_t packed = 0;
            for (unsigned c = 0; c < 4; ++c)
                packed |= (uint32_t)((sum[c] + area / 2) / area) << (8 * c);
            out[(size_t)y * stride + x] = packed;
        }
    }
}

int x4_image_area(const unsigned char *source, size_t source_bytes,
    unsigned sw, unsigned sh, uint32_t *out, size_t out_pixels,
    unsigned dw, unsigned dh, unsigned stride)
{
    if (!valid_dimensions(source, source_bytes, sw, sh, out, out_pixels, dw, dh, stride))
        return -1;
    sample_region(source, sw, sh, dw, dh, 0, 0, out, dw, dh, stride);
    return 0;
}

int x4_image_area_cover(const unsigned char *source, size_t source_bytes,
    unsigned sw, unsigned sh, uint32_t *out, size_t out_pixels,
    unsigned dw, unsigned dh, unsigned stride)
{
    if (!valid_dimensions(source, source_bytes, sw, sh, out, out_pixels, dw, dh, stride))
        return -1;
    unsigned scaled_w = sw, scaled_h = sh;
    if (sw < dw || sh < dh) {
        if (sw * dh >= sh * dw) {
            scaled_w = (sw * dh + sh - 1) / sh;
            scaled_h = dh;
        } else {
            scaled_w = dw;
            scaled_h = (sh * dw + sw - 1) / sw;
        }
    }
    sample_region(source, sw, sh, scaled_w, scaled_h, (scaled_w - dw) / 2,
        (scaled_h - dh) / 2, out, dw, dh, stride);
    return 0;
}
