/* SPDX-License-Identifier: GPL-3.0-only */
#include "pkg_header.h"
#include "update_io51.h"
#include "update_manifest.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool id_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool x4_pkg_header_parse(const unsigned char *bytes, size_t size, X4PkgHeader *header)
{
    static const unsigned char magic[] = {0x7f, 'C', 'N', 'T'};
    if (!header) return false;
    *header = (X4PkgHeader){0};
    if (!bytes || size < X4_PKG_PREFIX_BYTES || memcmp(bytes, magic, sizeof(magic)))
        return false;

    /* CNT content ID at 0x40: XXXXXX-TTTT00000_00-XXXXXXXXXXXXXXXX. */
    const unsigned char *id = bytes + 0x40;
    if (id[6] != '-' || id[16] != '_' || id[17] != '0' || id[18] != '0' || id[19] != '-')
        return false;
    for (size_t i = 0; i < X4_PKG_CONTENT_ID_LEN; ++i) {
        if (i != 6 && i != 16 && i != 19 && !id_char(id[i])) return false;
    }
    for (size_t i = 11; i < 16; ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    for (size_t i = X4_PKG_CONTENT_ID_LEN; i < 0x30; ++i) {
        if (id[i] != 0) return false;
    }
    memcpy(header->content_id, id, X4_PKG_CONTENT_ID_LEN);
    memcpy(header->title_id, id + 7, X4_PKG_TITLE_ID_LEN);
    return true;
}

enum {
    PKG_ENTRY_COUNT_OFFSET = 0x10,
    PKG_ENTRY_TABLE_OFFSET = 0x18,
    PKG_ENTRY_BYTES = 0x20,
    PKG_PARAM_SFO = 0x1000,
    SFO_HEADER_BYTES = 20,
    SFO_INDEX_BYTES = 16,
    SFO_TEXT = 0x0204,
};

static uint32_t u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t u32le(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static unsigned u16le(const unsigned char *p)
{
    return p[0] | ((unsigned)p[1] << 8);
}

typedef struct {
    const char *name;
    char *value;
    size_t capacity;
    bool found;
} SfoText;

static bool read_sfo(const unsigned char *sfo, size_t len, X4PkgSfo *out)
{
    static const unsigned char prefix[] = {0, 'P', 'S', 'F', 1, 1, 0, 0};
    if (len < SFO_HEADER_BYTES || memcmp(sfo, prefix, sizeof(prefix))) return false;
    const size_t keys = u32le(sfo + 8), data = u32le(sfo + 12), count = u32le(sfo + 16);
    if (count > 128 || SFO_HEADER_BYTES + count * SFO_INDEX_BYTES > keys ||
        keys >= data || data >= len) return false;
    char category[3];
    SfoText fields[] = {
        {"CONTENT_ID", out->content_id, sizeof(out->content_id), false},
        {"TITLE_ID", out->title_id, sizeof(out->title_id), false},
        {"APP_VER", out->app_ver, sizeof(out->app_ver), false},
        {"X4_PRODUCT_VERSION", out->version, sizeof(out->version), false},
        {"CATEGORY", category, sizeof(category), false},
    };
    const size_t field_count = sizeof(fields) / sizeof(fields[0]);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *entry = sfo + SFO_HEADER_BYTES + i * SFO_INDEX_BYTES;
        const size_t key = u16le(entry), value = u32le(entry + 12);
        const size_t used = u32le(entry + 4), capacity = u32le(entry + 8);
        if (key >= data - keys || value > len - data || capacity > len - data - value ||
            used > capacity) return false;
        const char *name = (const char *)sfo + keys + key;
        if (!*name || !memchr(name, 0, data - keys - key)) return false;
        for (size_t j = 0; j < field_count; ++j) {
            SfoText *field = &fields[j];
            if (strcmp(name, field->name)) continue;
            if (field->found || u16le(entry + 2) != SFO_TEXT || used < 2 || used > field->capacity)
                return false;
            const unsigned char *text = sfo + data + value;
            if (text[used - 1]) return false;
            for (size_t k = 0; k < used - 1; ++k) {
                if (text[k] < 0x20 || text[k] > 0x7e) return false;
            }
            memcpy(field->value, text, used);
            field->found = true;
            break;
        }
    }
    for (size_t j = 0; j < field_count; ++j) {
        if (!fields[j].found) return false;
    }
    return !strcmp(category, "gd");
}

bool x4_pkg_read_identity(FILE *file, size_t size, X4PkgHeader *header, X4PkgSfo *sfo)
{
    if (!file || !header || !sfo || size < X4_PKG_PREFIX_BYTES ||
        size > X4_UPDATE_PACKAGE_MAX || fseek(file, 0, SEEK_SET)) return false;
    unsigned char prefix[X4_PKG_PREFIX_BYTES];
    if (x4_update51_read_all(file, prefix, sizeof(prefix)) ||
        !x4_pkg_header_parse(prefix, sizeof(prefix), header)) return false;

    /* Only the clear PARAM.SFO entry is needed. Ignore the PFS payload. */
    const size_t entries = u32be(prefix + PKG_ENTRY_COUNT_OFFSET);
    const size_t table = u32be(prefix + PKG_ENTRY_TABLE_OFFSET);
    if (!entries || entries > 4096 || table < sizeof(prefix) || table > size ||
        entries > (size - table) / PKG_ENTRY_BYTES || fseek(file, (long)table, SEEK_SET))
        return false;
    const size_t table_end = table + entries * PKG_ENTRY_BYTES;
    size_t sfo_offset = 0, sfo_size = 0;
    for (size_t i = 0; i < entries; ++i) {
        unsigned char entry[PKG_ENTRY_BYTES];
        if (x4_update51_read_all(file, entry, sizeof(entry))) return false;
        if (u32be(entry) != PKG_PARAM_SFO) continue;
        if (sfo_size || u32be(entry + 8) || u32be(entry + 12)) return false;
        sfo_offset = u32be(entry + 16);
        sfo_size = u32be(entry + 20);
        if (sfo_offset < table_end || sfo_offset > size || sfo_size > size - sfo_offset ||
            sfo_size < SFO_HEADER_BYTES || sfo_size > 65536) return false;
    }
    if (!sfo_size || fseek(file, (long)sfo_offset, SEEK_SET)) return false;
    unsigned char *data = malloc(sfo_size);
    if (!data) return false;
    *sfo = (X4PkgSfo){0};
    bool valid = !x4_update51_read_all(file, data, sfo_size) && read_sfo(data, sfo_size, sfo);
    free(data);
    return valid;
}
