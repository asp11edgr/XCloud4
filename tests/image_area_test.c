/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/image_area.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double intersection(double lo, double hi, unsigned pixel)
{
    return fmax(0.0, fmin(hi, pixel + 1.0) - fmax(lo, pixel));
}

static uint32_t expected(const unsigned char *src, unsigned sw, unsigned sh,
    unsigned dw, unsigned dh, unsigned x, unsigned y)
{
    const double width = (double)sw / dw, height = (double)sh / dh;
    double sum[4] = {0};
    for (unsigned sy = 0; sy < sh; ++sy) {
        double wy = intersection(y * height, (y + 1) * height, sy);
        for (unsigned sx = 0; sx < sw; ++sx) {
            double weight = wy * intersection(x * width, (x + 1) * width, sx);
            for (unsigned c = 0; c < 4; ++c)
                sum[c] += weight * src[((size_t)sy * sw + sx) * 4 + c];
        }
    }
    uint32_t value = 0;
    for (unsigned c = 0; c < 4; ++c)
        value |= (uint32_t)floor(sum[c] / (width * height) + 0.50000001) << (8 * c);
    return value;
}

static void test_size(unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
    unsigned char src[7 * 7 * 4];
    uint32_t memory[2 + 7 * 9];
    for (size_t i = 0; i < sizeof(src); ++i) src[i] = (unsigned char)(i * 53u + i / 3u);
    const uint32_t sentinel = UINT32_C(0xcafeadd5);
    for (size_t i = 0; i < sizeof(memory) / sizeof(memory[0]); ++i) memory[i] = sentinel;
    const unsigned stride = dw + 2;
    size_t count = (size_t)(dh - 1) * stride + dw;
    assert(x4_image_area(src, sw * sh * 4, sw, sh, memory + 1, count, dw, dh, stride) == 0);
    assert(memory[0] == sentinel && memory[count + 1] == sentinel);
    for (unsigned y = 0; y < dh; ++y) {
        for (unsigned x = 0; x < dw; ++x)
            assert(memory[1 + y * stride + x] == expected(src, sw, sh, dw, dh, x, y));
        if (y + 1 < dh) {
            assert(memory[1 + y * stride + dw] == sentinel);
            assert(memory[1 + y * stride + dw + 1] == sentinel);
        }
    }
    assert(x4_image_area(src, sw * sh * 4 - 1, sw, sh, memory + 1, count, dw, dh, stride) < 0);
    assert(x4_image_area(src, sw * sh * 4, sw, sh, memory + 1, count - 1, dw, dh, stride) < 0);
}

static void test_pure_crop(unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
    size_t source_size = (size_t)sw * sh * 4;
    size_t output_size = (size_t)dw * dh;
    unsigned char *source = malloc(source_size);
    uint32_t *output = malloc(output_size * sizeof(*output));
    assert(source && output);
    for (unsigned y = 0; y < sh; ++y) {
        for (unsigned x = 0; x < sw; ++x) {
            unsigned char *pixel = source + ((size_t)y * sw + x) * 4;
            pixel[0] = (unsigned char)x;
            pixel[1] = (unsigned char)y;
            pixel[2] = (unsigned char)(x ^ y);
            pixel[3] = 255;
        }
    }
    assert(x4_image_area_cover(source, source_size, sw, sh, output, output_size, dw, dh, dw) == 0);
    for (unsigned y = 0; y < dh; ++y) {
        for (unsigned x = 0; x < dw; ++x) {
            unsigned sx = x + (sw - dw) / 2, sy = y + (sh - dh) / 2;
            uint32_t pixel = UINT32_C(0xff000000) | ((sx ^ sy) & 255u) << 16 |
                (sy & 255u) << 8 | (sx & 255u);
            assert(output[(size_t)y * dw + x] == pixel);
        }
    }
    free(output);
    free(source);
}

static void test_single_area_cover(unsigned sw, unsigned sh, unsigned dw, unsigned dh,
    unsigned scaled_w, unsigned scaled_h)
{
    unsigned char source[7 * 7 * 4];
    uint32_t output[7 * 9];
    const uint32_t sentinel = UINT32_C(0xcafeadd5);
    unsigned stride = dw + 2;
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i * 53u + i / 3u);
    for (size_t i = 0; i < sizeof(output) / sizeof(output[0]); ++i) output[i] = sentinel;
    assert(x4_image_area_cover(source, sw * sh * 4, sw, sh, output, dh * stride, dw, dh, stride) == 0);
    for (unsigned y = 0; y < dh; ++y) {
        for (unsigned x = 0; x < dw; ++x)
            assert(output[y * stride + x] == expected(source, sw, sh, scaled_w, scaled_h,
                x + (scaled_w - dw) / 2, y + (scaled_h - dh) / 2));
        assert(output[y * stride + dw] == sentinel && output[y * stride + dw + 1] == sentinel);
    }
    assert(x4_image_area_cover(source, sw * sh * 4 - 1, sw, sh, output, dh * stride, dw, dh, stride) < 0);
    assert(x4_image_area_cover(source, sw * sh * 4, sw, sh, output,
        (dh - 1) * stride + dw - 1, dw, dh, stride) < 0);
}

int main(void)
{
    for (unsigned sw = 1; sw <= 7; ++sw)
        for (unsigned sh = 1; sh <= 7; ++sh)
            for (unsigned dw = 1; dw <= 7; ++dw)
                for (unsigned dh = 1; dh <= 7; ++dh)
                    test_size(sw, sh, dw, dh);
    unsigned char pixel[4] = {1, 2, 3, 4};
    uint32_t out = 0;
    assert(x4_image_area(pixel, sizeof(pixel), 0, 1, &out, 1, 1, 1, 1) < 0);
    assert(x4_image_area(pixel, sizeof(pixel), 2049, 1, &out, 1, 1, 1, 1) < 0);
    assert(x4_image_area(pixel, sizeof(pixel), 1, 1, &out, 1, 1, 1, 0) < 0);
    assert(!out);
    test_pure_crop(852, 479, 852, 448);
    test_pure_crop(796, 448, 420, 448);
    test_pure_crop(550, 825, 550, 770);
    test_pure_crop(770, 770, 550, 770);
    test_single_area_cover(7, 3, 5, 5, 12, 5);
    test_single_area_cover(3, 7, 5, 5, 5, 12);
    test_single_area_cover(3, 2, 5, 4, 6, 4);
    puts("image area PASS: 2401 pairs, four 1:1 crop geometries, one-pass cover and row canaries");
}
