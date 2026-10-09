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
