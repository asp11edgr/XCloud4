/* SPDX-License-Identifier: GPL-3.0-only */
#include "json.h"
#include <string.h>
#include <limits.h>

typedef struct { const unsigned char *p, *end; unsigned limit; } Reader;
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
    if (depth > r->limit) return -1;
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
    Reader r = {(const unsigned char *)json, (const unsigned char *)json + length, 16};
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

static Reader span_reader(X4JsonSpan v)
{
    const unsigned char *p = (const unsigned char *)v.data;
    return (Reader){p, p ? p + v.length : p, X4_JSON_DEPTH_MAX};
}
static X4JsonSpan span(const unsigned char *start, const unsigned char *end)
{
    return (X4JsonSpan){(const char *)start, (size_t)(end - start)};
}
int x4_json_parse(const char *json, size_t length, X4JsonSpan *root)
{
    if (!root) return -1;
    *root = (X4JsonSpan){0};
    if (!json || length > X4_JSON_DOCUMENT_MAX) return -1;
    Reader r = {(const unsigned char *)json, (const unsigned char *)json + length, X4_JSON_DEPTH_MAX};
    space(&r);
    const unsigned char *start = r.p;
    if (value(&r, 1) < 0) return -1;
    const unsigned char *end = r.p;
    space(&r);
    if (r.p != r.end) return -1;
    *root = span(start, end);
    return 0;
}
X4JsonType x4_json_type(X4JsonSpan v)
{
    if (!v.data || !v.length) return X4_JSON_T_INVALID;
    switch (v.data[0]) {
    case '{': return X4_JSON_T_OBJECT;
    case '[': return X4_JSON_T_ARRAY;
    case '"': return X4_JSON_T_STRING;
    case 't': case 'f': return X4_JSON_T_BOOL;
    case 'n': return X4_JSON_T_NULL;
    default: return (v.data[0] == '-' || (v.data[0] >= '0' && v.data[0] <= '9')) ? X4_JSON_T_NUMBER : X4_JSON_T_INVALID;
    }
}
/* Decodes a key and compares it with an ASCII name without copying it. */
static int key_is(Reader *r, const char *name, int *match)
{
    if (take(r, '"') < 0) return -1;
    size_t i = 0;
    int same = 1;
    while (r->p < r->end && *r->p != '"') {
        uint32_t n;
        if (codepoint(r, &n) < 0) return -1;
        if (same && name[i] && (uint32_t)(unsigned char)name[i] == n) ++i;
        else same = 0;
    }
    if (r->p == r->end) return -1;
    ++r->p;
    *match = same && !name[i];
    return 0;
}
int x4_json_member(X4JsonSpan object, const char *name, X4JsonSpan *out)
{
    if (out) *out = (X4JsonSpan){0};
    if (!name || !out) return -1;
    Reader r = span_reader(object);
    if (take(&r, '{') < 0) return -1;
    space(&r);
    if (r.p < r.end && *r.p == '}') return 0;
    int found = 0;
    for (;;) {
        int match = 0;
        if (key_is(&r, name, &match) < 0 || take(&r, ':') < 0) break;
        space(&r);
        const unsigned char *start = r.p;
        if (value(&r, 1) < 0) break;
        if (match) {
            /* A queried name present twice is ambiguous: refuse it. */
            if (found) break;
            found = 1;
            *out = span(start, r.p);
        }
        space(&r);
        if (r.p == r.end) break;
        unsigned char c = *r.p++;
        if (c == '}') return found;
        if (c != ',') break;
    }
    *out = (X4JsonSpan){0};
    return -1;
}
int x4_json_item(X4JsonSpan array, size_t *cursor, X4JsonSpan *item)
{
    if (item) *item = (X4JsonSpan){0};
    if (!cursor || !item || x4_json_type(array) != X4_JSON_T_ARRAY || *cursor > array.length) return -1;
    if (*cursor == array.length) return 0;
    const unsigned char *base = (const unsigned char *)array.data;
    Reader r = span_reader(array);
    if (*cursor == 0) {
        ++r.p;
        space(&r);
        if (r.p < r.end && *r.p == ']') { *cursor = array.length; return 0; }
    } else r.p = base + *cursor;
    space(&r);
    const unsigned char *start = r.p;
    if (value(&r, 1) < 0) return -1;
    const unsigned char *end = r.p;
    space(&r);
    if (r.p == r.end) return -1;
    unsigned char c = *r.p++;
    if (c == ']') *cursor = array.length;
    else if (c == ',') *cursor = (size_t)(r.p - base);
    else return -1;
    *item = span(start, end);
    return 1;
}
int x4_json_bool(X4JsonSpan v, int *out)
{
    if (!out || !v.data) return -1;
    if (v.length == 4 && !memcmp(v.data, "true", 4)) { *out = 1; return 0; }
    if (v.length == 5 && !memcmp(v.data, "false", 5)) { *out = 0; return 0; }
    return -1;
}
int x4_json_uint32(X4JsonSpan v, uint32_t *out)
{
    if (!out || !v.data || !v.length || (v.length > 1 && v.data[0] == '0')) return -1;
    uint32_t n = 0;
    for (size_t i = 0; i < v.length; ++i) {
        unsigned char c = (unsigned char)v.data[i];
        if (c < '0' || c > '9' || n > (UINT32_MAX - (c - '0')) / 10) return -1;
        n = n * 10 + (c - '0');
    }
    *out = n;
    return 0;
}
int x4_json_token(X4JsonSpan v, char *out, size_t capacity)
{
    if (!out || !capacity) return -1;
    out[0] = 0;
    if (x4_json_type(v) != X4_JSON_T_STRING) return -1;
    Reader r = span_reader(v);
    if (string(&r, out, capacity, 1) < 0 || r.p != r.end) { out[0] = 0; return -1; }
    return 0;
}
/* ASCII spelling for U+00C0..U+00FF. */
static const char *const latin1[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "TH", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};
static const char *ascii_for(uint32_t n, char *one)
{
    if (n >= 0x20 && n <= 0x7e) { one[0] = (char)n; one[1] = 0; return one; }
    if (n < 0x20) return " ";
    if (n >= 0xc0 && n <= 0xff) return latin1[n - 0xc0];
    switch (n) {
    case 0xa0: return " ";
    case 0xa1: return "!";
    case 0xbf: return "?";
    case 0xb7: case 0x2022: return "-";
    case 0xa9: case 0xae: case 0x2122: return "";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0x2018: case 0x2019: case 0x201a: case 0x2032: return "'";
    case 0x201c: case 0x201d: case 0x201e: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: return "-";
    case 0x2026: return "...";
    default: return "?";
    }
}
int x4_json_text(X4JsonSpan v, char *out, size_t capacity)
{
    if (!out || !capacity) return -1;
    out[0] = 0;
    if (x4_json_type(v) != X4_JSON_T_STRING) return -1;
    Reader r = span_reader(v);
    ++r.p;
    size_t used = 0;
    while (r.p < r.end && *r.p != '"') {
        uint32_t n;
        char one[2];
        if (codepoint(&r, &n) < 0) { out[0] = 0; return -1; }
        for (const char *s = ascii_for(n, one); *s && used + 1 < capacity; ++s) out[used++] = *s;
    }
    if (r.p == r.end || r.p + 1 != r.end) { out[0] = 0; return -1; }
    out[used] = 0;
    return 0;
}
