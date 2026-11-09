/* SPDX-License-Identifier: GPL-3.0-only */
#include "font.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../assets/fonts/font_fallback.h"

#ifndef X4_WIDTH
#define X4_WIDTH 1920
#endif
#ifndef X4_HEIGHT
#define X4_HEIGHT 1080
#endif
#define X4_FONT_BUDGET (4u * 1024u * 1024u)
#define X4_FONT_MAX_CODEPOINTS 4096u
#define X4_FONT_HEADER_BYTES 32u
#define X4_FONT_FACE_BYTES 32u
#define X4_FONT_GLYPH_BYTES 24u

typedef struct {
    const unsigned char *bytes, *faces, *glyphs, *alpha;
    size_t bytes_count;
    uint32_t face_count, glyph_count, alpha_count;
} Atlas;
typedef struct {
    uint32_t size, first, count, line_height;
} Face;
typedef struct {
    uint32_t codepoint, alpha_offset, advance;
    int width, height, left, top;
} Glyph;
static Atlas current;
static unsigned char *owned;
static int ready;

static uint32_t le32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static unsigned le16(const unsigned char *p)
{ return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static int signed16(const unsigned char *p)
{ unsigned n = le16(p); return n < 32768u ? (int)n : (int)n - 65536; }

static int validate(Atlas *a, const unsigned char *b, size_t size)
{
    uint32_t faces, glyphs, alpha;
    uint64_t glyph_offset, alpha_offset, total;
    if (size < X4_FONT_HEADER_BYTES || memcmp(b, "X4FNT01\0", 8) ||
        le32(b + 8) != 1 || le32(b + 24) != X4_FONT_FACE_BYTES ||
        le32(b + 28) != X4_FONT_GLYPH_BYTES) return -1;
    faces = le32(b + 12); glyphs = le32(b + 16); alpha = le32(b + 20);
    if (!faces || faces > 32u || !glyphs || glyphs > 16384u || !alpha) return -1;
    glyph_offset = X4_FONT_HEADER_BYTES + (uint64_t)faces * X4_FONT_FACE_BYTES;
    alpha_offset = glyph_offset + (uint64_t)glyphs * X4_FONT_GLYPH_BYTES;
    total = alpha_offset + alpha;
    if (total != size) return -1;
    for (uint32_t i = 0; i < faces; ++i) {
        const unsigned char *f = b + X4_FONT_HEADER_BYTES + i * X4_FONT_FACE_BYTES;
        uint32_t first = le32(f + 8), count = le32(f + 12), previous = 0;
        if (le32(f) < 8u || le32(f) > 96u || le32(f + 4) > 2u || !count ||
            (uint64_t)first + count > glyphs || le32(f + 20) > 160u ||
            !le32(f + 20) || le32(f + 24) || le32(f + 28)) return -1;
        for (uint32_t j = 0; j < count; ++j) {
            const unsigned char *g = b + (size_t)glyph_offset + (first + j) * X4_FONT_GLYPH_BYTES;
            uint32_t cp = le32(g), offset = le32(g + 4), advance = le32(g + 16);
            unsigned width = le16(g + 8), height = le16(g + 10);
            if (!cp || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu) ||
                (j && cp <= previous) || width > 192u || height > 192u ||
                (uint64_t)offset + (uint64_t)width * height > alpha ||
                advance > 160u * 64u || le32(g + 20)) return -1;
            previous = cp;
        }
    }
    *a = (Atlas){b, b + X4_FONT_HEADER_BYTES, b + (size_t)glyph_offset,
                 b + (size_t)alpha_offset, size, faces, glyphs, alpha};
    return 0;
}

static void fallback(void)
{
    /* This generated, trusted immutable atlas keeps errors readable. */
    (void)validate(&current, x4_font_fallback_data, sizeof(x4_font_fallback_data));
    ready = 1;
}
static int load_file(const char *path, Atlas *loaded, unsigned char **result)
{
    FILE *f = fopen(path, "rb"); long length; unsigned char *bytes;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) <= 0 ||
        (unsigned long)length > X4_FONT_BUDGET - sizeof(x4_font_fallback_data) ||
        fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    bytes = (unsigned char *)malloc((size_t)length);
    if (!bytes) { fclose(f); return -1; }
    if (fread(bytes, 1, (size_t)length, f) != (size_t)length ||
        validate(loaded, bytes, (size_t)length)) {
        fclose(f); free(bytes); return -1;
    }
    fclose(f); *result = bytes; return 0;
}
int x4_font_init(const char *asset_path)
{
    char path[1024]; Atlas loaded; unsigned char *bytes;
    x4_font_close();
    if (!asset_path || !*asset_path) return -1;
    if (strlen(asset_path) >= sizeof(path) - 24u) return -1;
    if (load_file(asset_path, &loaded, &bytes)) {
        /* fopen may accept a POSIX directory; retry the packaged child
         * after any direct-stream failure, including its length check. */
        snprintf(path, sizeof(path), "%s/noto_sans.x4font", asset_path);
        if (load_file(path, &loaded, &bytes)) return -1;
    }
    owned = bytes; current = loaded; return 0;
}
void x4_font_close(void)
{ free(owned); owned = NULL; fallback(); }
int x4_font_is_fallback(void)
{ return !owned; }
size_t x4_font_heap_bytes(void)
{ return owned ? current.bytes_count : 0u; }

uint32_t x4_utf8_next(const char **cursor)
{
    const unsigned char *p; uint32_t cp, minimum; unsigned count;
    if (!cursor || !*cursor) return 0;
    p = (const unsigned char *)*cursor;
    if (!*p) return 0;
    if (*p < 0x80u) { *cursor += 1; return *p; }
    if (*p >= 0xc2u && *p <= 0xdfu) { cp = *p & 0x1fu; count = 2; minimum = 0x80u; }
    else if (*p >= 0xe0u && *p <= 0xefu) { cp = *p & 0x0fu; count = 3; minimum = 0x800u; }
    else if (*p >= 0xf0u && *p <= 0xf4u) { cp = *p & 7u; count = 4; minimum = 0x10000u; }
    else { *cursor += 1; return 0xfffdu; }
    for (unsigned i = 1; i < count; ++i) {
        if (!p[i] || (p[i] & 0xc0u) != 0x80u) { *cursor += 1; return 0xfffdu; }
        cp = (cp << 6) | (p[i] & 0x3fu);
    }
    if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        *cursor += 1; return 0xfffdu;
    }
    *cursor += count; return cp;
}

static int bounded_size(int size)
{ return size < 8 ? 8 : size > 96 ? 96 : size; }
static Face select_face(int size, int semibold)
{
    unsigned best = 0, distance = UINT_MAX;
    unsigned target_w = semibold <= 0 ? 0u : (semibold >= 2 ? 2u : 1u);
    if (!ready) fallback();
    for (uint32_t i = 0; i < current.face_count; ++i) {
        const unsigned char *f = current.faces + i * X4_FONT_FACE_BYTES;
        unsigned px = le32(f), delta = px > (unsigned)size ? px - (unsigned)size : (unsigned)size - px;
        unsigned face_w = le32(f + 4);
        if (face_w != target_w) {
            unsigned w_diff = face_w > target_w ? face_w - target_w : target_w - face_w;
            delta += w_diff * 500u;
        }
        if (delta < distance) { distance = delta; best = i; }
    }
    const unsigned char *f = current.faces + best * X4_FONT_FACE_BYTES;
    return (Face){le32(f), le32(f + 8), le32(f + 12), le32(f + 20)};
}
static Glyph read_glyph(uint32_t index)
{
    const unsigned char *g = current.glyphs + index * X4_FONT_GLYPH_BYTES;
    return (Glyph){le32(g), le32(g + 4), le32(g + 16), (int)le16(g + 8),
                   (int)le16(g + 10), signed16(g + 12), signed16(g + 14)};
}
static Glyph glyph_for(Face f, uint32_t cp)
{
    uint32_t lo = 0, hi = f.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        Glyph g = read_glyph(f.first + mid);
        if (g.codepoint == cp) return g;
        if (g.codepoint < cp) lo = mid + 1u; else hi = mid;
    }
    if (cp != 0xfffdu) return glyph_for(f, 0xfffdu);
    return (Glyph){0, 0, 32u * 64u, 0, 0, 0, 0};
}
static int64_t scaled(int64_t n, int size, uint32_t basis)
{ return n * size / basis; }
static int advance_px64(Glyph g, int size, Face f)
{ return (int)scaled(g.advance, size, f.size); }
static int64_t right_px64(Glyph g, int size, Face f, int64_t pen)
{ return pen + scaled((int64_t)(g.left + g.width) * 64, size, f.size); }

int x4_text_width_px(int size, int semibold, const char *text)
{
    int64_t pen = 0, maximum = 0, right; Face f; unsigned count = 0;
    if (!text) return 0;
    size = bounded_size(size); f = select_face(size, semibold);
    while (*text && count++ < X4_FONT_MAX_CODEPOINTS) {
        uint32_t cp = x4_utf8_next(&text);
        if (cp == '\n') { if (pen > maximum) maximum = pen; pen = 0; continue; }
        if (cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        Glyph g = glyph_for(f, cp);
        right = right_px64(g, size, f, pen);
        if (right > maximum) maximum = right;
        pen += advance_px64(g, size, f);
    }
    if (pen > maximum) maximum = pen;
    if (maximum > (int64_t)INT_MAX * 64) return INT_MAX;
    return (int)((maximum + 63) / 64);
}

static unsigned sample_alpha(Glyph g, int dx, int dy, int width, int height)
{
    unsigned sx = (unsigned)((int64_t)dx * g.width * 256 / width);
    unsigned sy = (unsigned)((int64_t)dy * g.height * 256 / height);
    unsigned ix = sx >> 8, iy = sy >> 8, fx = sx & 255u, fy = sy & 255u;
    unsigned nx = ix + 1u < (unsigned)g.width ? ix + 1u : ix;
    unsigned ny = iy + 1u < (unsigned)g.height ? iy + 1u : iy;
    const unsigned char *a = current.alpha + g.alpha_offset;
    unsigned top = a[iy * (unsigned)g.width + ix] * (256u - fx) + a[iy * (unsigned)g.width + nx] * fx;
    unsigned bottom = a[ny * (unsigned)g.width + ix] * (256u - fx) + a[ny * (unsigned)g.width + nx] * fx;
    return (top * (256u - fy) + bottom * fy + 32768u) >> 16;
}
static void blit(uint32_t *pixels, int64_t x, int64_t y, Glyph g, Face f,
                 int size, uint32_t color)
{
    int w = (g.width * size + (int)f.size - 1) / (int)f.size;
    int h = (g.height * size + (int)f.size - 1) / (int)f.size;
    if (!pixels || !w || !h) return;
    x += scaled(g.left, size, f.size); y += scaled(g.top, size, f.size);
    if (x >= X4_WIDTH || y >= X4_HEIGHT || x + w <= 0 || y + h <= 0) return;
    int from_x = x < 0 ? (int)-x : 0, from_y = y < 0 ? (int)-y : 0;
    int to_x = x + w > X4_WIDTH ? (int)(X4_WIDTH - x) : w;
    int to_y = y + h > X4_HEIGHT ? (int)(X4_HEIGHT - y) : h;
    for (int row = from_y; row < to_y; ++row) {
        uint32_t *out = pixels + (size_t)(y + row) * X4_WIDTH + (size_t)(x + from_x);
        for (int col = from_x; col < to_x; ++col, ++out) {
            unsigned alpha = sample_alpha(g, col, row, w, h) * (color >> 24) / 255u;
            if (!alpha) continue;
            unsigned inverse = 255u - alpha; uint32_t old = *out;
            unsigned red = ((color & 255u) * alpha + (old & 255u) * inverse + 127u) / 255u;
            unsigned green = (((color >> 8) & 255u) * alpha + ((old >> 8) & 255u) * inverse + 127u) / 255u;
            unsigned blue = (((color >> 16) & 255u) * alpha + ((old >> 16) & 255u) * inverse + 127u) / 255u;
            *out = 0xff000000u | (blue << 16) | (green << 8) | red;
        }
    }
}
void x4_text_px(uint32_t *pixels, int x, int y, int size, int semibold,
                const char *text, uint32_t color)
{
    int64_t pen = 0, line_y = y; Face f; unsigned count = 0;
    if (!pixels || !text) return;
    size = bounded_size(size); f = select_face(size, semibold);
    while (*text && count++ < X4_FONT_MAX_CODEPOINTS) {
        uint32_t cp = x4_utf8_next(&text);
        if (cp == '\n') { pen = 0; line_y += scaled(f.line_height, size, f.size); continue; }
        if (cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        Glyph g = glyph_for(f, cp);
        blit(pixels, (int64_t)x + pen / 64, line_y, g, f, size, color);
        pen += advance_px64(g, size, f);
    }
}

static size_t encode(uint32_t cp, char out[4])
{
    if (cp < 0x80u) { out[0] = (char)cp; return 1; }
    if (cp < 0x800u) { out[0] = (char)(0xc0u | (cp >> 6)); out[1] = (char)(0x80u | (cp & 63u)); return 2; }
    if (cp < 0x10000u) { out[0] = (char)(0xe0u | (cp >> 12)); out[1] = (char)(0x80u | ((cp >> 6) & 63u)); out[2] = (char)(0x80u | (cp & 63u)); return 3; }
    out[0] = (char)(0xf0u | (cp >> 18)); out[1] = (char)(0x80u | ((cp >> 12) & 63u)); out[2] = (char)(0x80u | ((cp >> 6) & 63u)); out[3] = (char)(0x80u | (cp & 63u)); return 4;
}
static int word_space(const char *text)
{
    const uint32_t cp = x4_utf8_next(&text);
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xa0 || cp == 0x202f;
}

static size_t rewind_word(const char *text, size_t used, int drop_word)
{
    while (used) {
        size_t start = used - 1;
        while (start && ((unsigned char)text[start] & 0xc0u) == 0x80u) --start;
        if (word_space(text + start)) drop_word = 0;
        else if (!drop_word) break;
        used = start;
    }
    return used;
}

size_t x4_text_fit_utf8(char *out, size_t cap, int size, int semibold,
                       const char *text, int max_width)
{
    size_t used = 0; int truncated = 0; int64_t pen = 0, maximum = 0; unsigned count = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!text || max_width <= 0) return 0;
    size = bounded_size(size); Face f = select_face(size, semibold);
    const char *next_word = text;
    while (*text && count++ < X4_FONT_MAX_CODEPOINTS) {
        uint32_t cp = x4_utf8_next(&text); char encoded[4]; size_t bytes;
        if (cp == '\n' || cp == '\r') { truncated = *text != 0; break; }
        if (cp == '\t') cp = ' ';
        bytes = encode(cp, encoded); Glyph g = glyph_for(f, cp);
        int64_t right = right_px64(g, size, f, pen);
        int64_t next = pen + advance_px64(g, size, f);
        int64_t extent = next > right ? next : right;
        if (maximum > extent) extent = maximum;
        if (used + bytes >= cap || extent > (int64_t)max_width * 64) { truncated = 1; break; }
        memcpy(out + used, encoded, bytes); used += bytes; out[used] = 0;
        pen = next; maximum = extent;
        next_word = text;
    }
    if (*text) truncated = 1;
    if (truncated) {
        const char ellipsis[] = "\xe2\x80\xa6";
        int ellipsis_width = x4_text_width_px(size, semibold, ellipsis);
        if (ellipsis_width > max_width || cap < sizeof(ellipsis)) { out[0] = 0; return 0; }
        used = rewind_word(out, used, *next_word && !word_space(next_word));
        out[used] = 0;
        while (used) {
            if (cap - used >= sizeof(ellipsis)) {
                memcpy(out + used, ellipsis, sizeof(ellipsis));
                if (x4_text_width_px(size, semibold, out) <= max_width)
                    return used + sizeof(ellipsis) - 1;
            }
            used = rewind_word(out, used, 1);
            out[used] = 0;
        }
    }
    return used;
}
void x4_text_ellipsis_px(uint32_t *pixels, int x, int y, int size, int semibold,
                        const char *text, int max_width, uint32_t color)
{
    char fitted[2048];
    const int original_size = bounded_size(size);
    int fit_size = original_size;
    if (!x4_text_fit_utf8(fitted, sizeof(fitted), fit_size, semibold, text, max_width) &&
        text && *text && max_width > 0 && fit_size > 8) {
        /* An arbitrary title can start with one overlong word. Try two smaller
         * faces before omitting it. Never draw a fragment or a lone ellipsis. */
        fit_size = bounded_size(original_size * 3 / 4);
        if (!x4_text_fit_utf8(fitted, sizeof(fitted), fit_size, semibold, text, max_width) &&
            fit_size > 8) {
            fit_size = 8;
            (void)x4_text_fit_utf8(fitted, sizeof(fitted), fit_size, semibold, text, max_width);
        }
    }
    const int shift = (original_size - fit_size) / 2;
    if (y <= INT_MAX - shift)
        x4_text_px(pixels, x, y + shift, fit_size, semibold, fitted, color);
}
