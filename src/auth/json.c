/* SPDX-License-Identifier: GPL-3.0-only */
#include "json.h"
#include <string.h>
#include <limits.h>

typedef struct { const unsigned char *p, *end; } Reader;
static void space(Reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n')) ++r->p;
}
static int take(Reader *r, unsigned char c)
{
    space(r);
    if (r->p == r->end || *r->p != c) return -1;
    ++r->p; return 0;
}
static int hex4(Reader *r, uint32_t *n)
{
    *n = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (r->p == r->end) return -1;
        unsigned char c = *r->p++;
        unsigned x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (x == 16) return -1;
        *n = *n * 16 + x;
    }
    return 0;
}
static int codepoint(Reader *r, uint32_t *n)
{
    if (r->p == r->end) return -1;
    unsigned char c = *r->p++;
    if (c < 0x20) return -1;
    if (c == '\\') {
        if (r->p == r->end) return -1;
        c = *r->p++;
        switch (c) {
        case '"': case '\\': case '/': *n = c; return 0;
        case 'b': *n = 8; return 0;
        case 'f': *n = 12; return 0;
        case 'n': *n = 10; return 0;
        case 'r': *n = 13; return 0;
        case 't': *n = 9; return 0;
        case 'u': {
            if (hex4(r, n) < 0) return -1;
            if (*n >= 0xdc00 && *n <= 0xdfff) return -1;
            if (*n >= 0xd800 && *n <= 0xdbff) {
                if (r->end - r->p < 2 || r->p[0] != '\\' || r->p[1] != 'u') return -1;
                r->p += 2;
                uint32_t low;
                if (hex4(r, &low) < 0 || low < 0xdc00 || low > 0xdfff) return -1;
                *n = 0x10000 + ((*n - 0xd800) << 10) + low - 0xdc00;
            }
            return 0;
        }
        default: return -1;
        }
    }
    if (c < 0x80) { *n = c; return 0; }
    unsigned extra;
    uint32_t min;
    if (c >= 0xc2 && c <= 0xdf) { extra = 1; min = 0x80; *n = c & 31; }
    else if (c >= 0xe0 && c <= 0xef) { extra = 2; min = 0x800; *n = c & 15; }
    else if (c >= 0xf0 && c <= 0xf4) { extra = 3; min = 0x10000; *n = c & 7; }
    else return -1;
    for (unsigned i = 0; i < extra; ++i) {
        if (r->p == r->end || (*r->p & 0xc0) != 0x80) return -1;
        *n = (*n << 6) | (*r->p++ & 63);
    }
    return *n < min || *n > 0x10ffff || (*n >= 0xd800 && *n <= 0xdfff) ? -1 : 0;
}
static int string(Reader *r, char *out, size_t cap, int strict)
{
    if (take(r, '"') < 0) return -1;
    size_t used = 0;
    int fits = 1;
    while (r->p < r->end && *r->p != '"') {
        uint32_t n;
        if (codepoint(r, &n) < 0) return -1;
        if (out) {
            if (n < 0x20 || n > 0x7e || used + 1 >= cap) fits = 0;
            if (fits) out[used++] = (char)n;
        }
    }
    if (r->p == r->end) return -1;
    ++r->p;
    if (out && cap) out[fits ? used : 0] = 0;
    return !fits && strict ? -1 : 0;
}
static int value(Reader *r, unsigned depth);
static int number(Reader *r)
{
    const unsigned char *start = r->p;
    if (r->p < r->end && *r->p == '-') ++r->p;
    if (r->p == r->end) return -1;
    if (*r->p == '0') ++r->p;
    else {
        if (*r->p < '1' || *r->p > '9') return -1;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') ++r->p;
    }
    if (r->p < r->end && *r->p == '.') {
        ++r->p;
        const unsigned char *digits = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') ++r->p;
        if (digits == r->p) return -1;
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        ++r->p;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) ++r->p;
        const unsigned char *digits = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') ++r->p;
        if (digits == r->p) return -1;
    }
    return r->p == start ? -1 : 0;
}
static int container(Reader *r, unsigned depth, int object)
{
    ++r->p; space(r);
    unsigned char end = object ? '}' : ']';
    if (r->p < r->end && *r->p == end) { ++r->p; return 0; }
    for (;;) {
        if (object && (string(r, NULL, 0, 0) < 0 || take(r, ':') < 0)) return -1;
        if (value(r, depth + 1) < 0) return -1;
        space(r);
        if (r->p == r->end) return -1;
        unsigned char c = *r->p++;
        if (c == end) return 0;
        if (c != ',') return -1;
    }
}
static int value(Reader *r, unsigned depth)
{
    if (depth > 16) return -1;
    space(r);
    if (r->p == r->end) return -1;
    if (*r->p == '"') return string(r, NULL, 0, 0);
    if (*r->p == '{' || *r->p == '[') return container(r, depth, *r->p == '{');
    const char *literal = *r->p == 't' ? "true" : *r->p == 'f' ? "false" : *r->p == 'n' ? "null" : NULL;
    if (literal) {
        size_t n = strlen(literal);
        if ((size_t)(r->end - r->p) < n || memcmp(r->p, literal, n)) return -1;
        r->p += n; return 0;
    }
    return number(r);
}
int x4_json_fields(const char *json, size_t length, X4JsonField *fields, size_t count)
{
    if (!json || length > 65536 || count > 32 || (!fields && count)) return -1;
    for (size_t i = 0; i < count; ++i) {
        fields[i].found = 0; fields[i].number = 0;
        if (fields[i].text && fields[i].capacity) fields[i].text[0] = 0;
    }
    Reader r = {(const unsigned char *)json, (const unsigned char *)json + length};
    if (take(&r, '{') < 0) return -1;
    space(&r);
    if (r.p < r.end && *r.p == '}') ++r.p;
    else for (;;) {
        char key[128] = {0};
        if (string(&r, key, sizeof(key), 0) < 0 || take(&r, ':') < 0) return -1;
        X4JsonField *f = NULL;
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(key, fields[i].name)) { f = &fields[i]; break; }
        if (f) {
            if (f->found) return -1;
            space(&r);
            if (f->kind == X4_JSON_STRING) {
                if (!f->text || !f->capacity || string(&r, f->text, f->capacity, 1) < 0) return -1;
            } else {
                const unsigned char *start = r.p;
                if (number(&r) < 0) return -1;
                uint32_t n = 0;
                for (const unsigned char *p = start; p < r.p; ++p) {
                    if (*p < '0' || *p > '9' || n > (UINT32_MAX - (*p - '0')) / 10) return -1;
                    n = n * 10 + *p - '0';
                }
                f->number = n;
            }
            f->found = 1;
        } else if (value(&r, 1) < 0) return -1;
        space(&r);
        if (r.p == r.end) return -1;
        unsigned char c = *r.p++;
        if (c == '}') break;
        if (c != ',') return -1;
    }
    space(&r);
    return r.p == r.end ? 0 : -1;
}
