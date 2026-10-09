/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum { X4_JSON_STRING, X4_JSON_UINT } X4JsonKind;
typedef struct {
    const char *name;
    X4JsonKind kind;
    char *text;
    size_t capacity;
    uint32_t number;
    int found;
} X4JsonField;
/* Strict bounded object parser. Requested strings must be printable ASCII. */
int x4_json_fields(const char *json, size_t length, X4JsonField *fields, size_t count);

/* Span reader for large documents. x4_json_parse validates one whole value
 * (at most X4_JSON_DOCUMENT_MAX bytes, nesting X4_JSON_DEPTH_MAX, strict
 * UTF-8 and escapes, no trailing data) and returns it as a span; the other
 * calls only accept spans obtained from it or from each other. */
#define X4_JSON_DOCUMENT_MAX (2u * 1024 * 1024)
#define X4_JSON_DEPTH_MAX 32u
typedef struct { const char *data; size_t length; } X4JsonSpan;
typedef enum {
    X4_JSON_T_INVALID, X4_JSON_T_OBJECT, X4_JSON_T_ARRAY, X4_JSON_T_STRING,
    X4_JSON_T_NUMBER, X4_JSON_T_BOOL, X4_JSON_T_NULL
} X4JsonType;
int x4_json_parse(const char *json, size_t length, X4JsonSpan *root);
X4JsonType x4_json_type(X4JsonSpan value);
/* 1 found, 0 absent, -1 when not an object or the name appears twice. */
int x4_json_member(X4JsonSpan object, const char *name, X4JsonSpan *out);
/* Array iteration; *cursor starts at 0. 1 item, 0 end, -1 not an array. */
int x4_json_item(X4JsonSpan array, size_t *cursor, X4JsonSpan *item);
int x4_json_bool(X4JsonSpan value, int *out);
int x4_json_uint32(X4JsonSpan value, uint32_t *out);
/* Strict: printable ASCII that fits, otherwise -1 and an empty output. */
int x4_json_token(X4JsonSpan value, char *out, size_t capacity);
/* Display text: common Latin letters and punctuation are transliterated to
 * printable ASCII, anything else becomes '?', and the result is truncated. */
int x4_json_text(X4JsonSpan value, char *out, size_t capacity);
