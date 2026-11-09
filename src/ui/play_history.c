/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "play_history.h"
#include "lang.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef X4_OPENORBIS
#include <sys/stat.h>
#endif

enum { HEADER_SIZE = 16, RECORD_SIZE = 48, LAST_OFFSET = 32, PLAYED_OFFSET = 40 };
static const unsigned char magic[8] = {'X','4','H','I','S','T','1',0};
_Static_assert(X4_PRODUCT_ID_CAP <= LAST_OFFSET, "History ID field must fit the fixed disk format");

static void put_le(unsigned char *out, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) out[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t get_le(const unsigned char *in, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)in[i] << (8 * i);
    return value;
}

static uint32_t checksum(uint32_t hash, const unsigned char *in, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ in[i]) * UINT32_C(16777619);
    return hash;
}

static bool normalize_id(char out[X4_PRODUCT_ID_CAP], const char *id)
{
    if (!id) return false;
    memset(out, 0, X4_PRODUCT_ID_CAP);
    unsigned i = 0;
    for (; i < X4_PRODUCT_ID_CAP - 1 && id[i]; ++i) {
        char c = id[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        out[i] = c;
    }
    return i > 0 && !id[i];
}

static unsigned find_id(const X4History *history, const char *id)
{
    unsigned i = 0;
    for (; i < history->count; ++i)
        if (!strcmp(history->entries[i].product_id, id)) break;
    return i;
}

const X4HistoryEntry *x4_history_find(const X4History *history, const char *product_id)
{
    char id[X4_PRODUCT_ID_CAP];
    if (!history || history->count > X4_HISTORY_MAX || !normalize_id(id, product_id)) return NULL;
    unsigned i = find_id(history, id);
    return i < history->count ? &history->entries[i] : NULL;
}

static void encode(unsigned char out[RECORD_SIZE], const X4HistoryEntry *entry)
{
    memset(out, 0, RECORD_SIZE);
    memcpy(out, entry->product_id, X4_PRODUCT_ID_CAP);
    put_le(out + LAST_OFFSET, entry->last_session_s, 8);
    put_le(out + PLAYED_OFFSET, entry->played_s, 8);
}

bool x4_history_load(X4History *history, const char *path)
{
    if (!history) return false;
    memset(history, 0, sizeof(*history));
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return false;
    unsigned char header[HEADER_SIZE], raw[RECORD_SIZE];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) &&
        !memcmp(header, magic, sizeof(magic));
    uint64_t count = ok ? get_le(header + 8, 4) : 0;
    ok = ok && count <= X4_HISTORY_MAX;
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned i = 0; ok && i < count; ++i) {
        X4HistoryEntry *entry = &history->entries[i];
        ok = fread(raw, 1, sizeof(raw), file) == sizeof(raw) &&
            memchr(raw, 0, X4_PRODUCT_ID_CAP) && normalize_id(entry->product_id, (const char *)raw) &&
            !memcmp(raw, entry->product_id, X4_PRODUCT_ID_CAP) &&
            find_id(history, entry->product_id) == i;
        if (!ok) break;
        for (unsigned j = X4_PRODUCT_ID_CAP; j < LAST_OFFSET; ++j)
            if (raw[j]) ok = false;
        entry->last_session_s = get_le(raw + LAST_OFFSET, 8);
        entry->played_s = get_le(raw + PLAYED_OFFSET, 8);
        ++history->count;
        hash = checksum(hash, raw, sizeof(raw));
    }
    ok = ok && hash == get_le(header + 12, 4) && fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) ok = false;
    if (!ok) memset(history, 0, sizeof(*history));
    else history->revision = 1;
    return ok;
}

static int write_exact(FILE *file, const unsigned char *bytes, size_t size)
{
    errno = 0;
    if (fwrite(bytes, 1, size, file) == size) return 0;
    return errno ? errno : EIO;
}

static bool save(const X4History *history, const char *path)
{
    if (!path) return true;
    char temporary[512];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return false;
    }
#ifdef X4_OPENORBIS
    if (!strcmp(path, X4_HISTORY_PATH)) (void)mkdir("/data/xcloud4", 0777);
#endif
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    unsigned char header[HEADER_SIZE], raw[RECORD_SIZE];
    memcpy(header, magic, sizeof(magic));
    put_le(header + 8, history->count, 4);
    uint32_t hash = UINT32_C(2166136261);
    for (unsigned i = 0; i < history->count; ++i) {
        encode(raw, &history->entries[i]);
        hash = checksum(hash, raw, sizeof(raw));
    }
    put_le(header + 12, hash, 4);
    int error = write_exact(file, header, sizeof(header));
    for (unsigned i = 0; !error && i < history->count; ++i) {
        encode(raw, &history->entries[i]);
        error = write_exact(file, raw, sizeof(raw));
    }
    if (fclose(file) && !error) error = errno ? errno : EIO;
    if (!error) {
        if (!rename(temporary, path)) return true;
        error = errno ? errno : EIO;
    }
    (void)remove(temporary);
    errno = error;
    return false;
}

bool x4_history_record(X4History *history, const char *product_id,
    uint64_t last_session_s, uint64_t added_seconds, const char *path)
{
    X4HistoryEntry next = {0};
    if (!history || history->count > X4_HISTORY_MAX || !normalize_id(next.product_id, product_id)) {
        errno = EINVAL;
        return false;
    }
    unsigned count = history->count, slot = find_id(history, next.product_id);
    if (slot < count) next = history->entries[slot];
    else if (count < X4_HISTORY_MAX) ++history->count;
    else {
        slot = 0;
        for (unsigned i = 1; i < count; ++i)
            if (history->entries[i].last_session_s < history->entries[slot].last_session_s) slot = i;
    }
    X4HistoryEntry before = history->entries[slot];
    if (last_session_s >= next.last_session_s) next.last_session_s = last_session_s;
    next.played_s += added_seconds > UINT64_MAX - next.played_s ?
        UINT64_MAX - next.played_s : added_seconds;
    history->entries[slot] = next;
    if (!save(history, path)) {
        history->entries[slot] = before;
        history->count = count;
        return false;
    }
    ++history->revision;
    return true;
}

uint64_t x4_history_now(void)
{
    time_t now = time(NULL);
    return now > 0 ? (uint64_t)now : 0;
}

static uint64_t local_day(uint64_t epoch)
{
    if (epoch > INT64_MAX) return 0;
    time_t at = (time_t)epoch;
    struct tm date;
    if ((uint64_t)at != epoch || !localtime_r(&at, &date) || date.tm_year < 0) return 0;
    uint64_t year = (uint64_t)date.tm_year + 1899;
    return year * 365 + year / 4 - year / 100 + year / 400 + (unsigned)date.tm_yday;
}

void x4_history_text(const X4HistoryEntry *entry, uint64_t now, bool english,
    char last[X4_HISTORY_LABEL], char played[X4_HISTORY_LABEL])
{
    last[0] = played[0] = '\0';
    if (!entry) return;
    enum X4Lang language = x4_lang_current();
    if (language == X4_LANG_ES || language == X4_LANG_EN)
        language = english ? X4_LANG_EN : X4_LANG_ES;
    if (entry->last_session_s && now) {
        const uint64_t current_day = local_day(now), last_day = local_day(entry->last_session_s);
        const uint64_t days = current_day && last_day ?
            (current_day > last_day ? current_day - last_day : 0) :
            (now > entry->last_session_s ? (now - entry->last_session_s) / 86400u : 0);
        if (days <= 1) {
            const enum X4TextId date = days ? X4_TR_50_HISTORY_YESTERDAY : X4_TR_50_HISTORY_TODAY;
            snprintf(last, X4_HISTORY_LABEL, "%s", x4_tr_for_lang(language, date));
        } else {
            snprintf(last, X4_HISTORY_LABEL, x4_tr_for_lang(language, X4_TR_50_HISTORY_DAYS_FMT),
                (unsigned long long)days);
        }
    }
    if (entry->played_s >= 3600)
        snprintf(played, X4_HISTORY_LABEL, x4_tr_for_lang(language, X4_TR_50_HISTORY_HOURS_FMT),
            (unsigned long long)(entry->played_s / 3600));
    else if (entry->played_s >= 60)
        snprintf(played, X4_HISTORY_LABEL, x4_tr_for_lang(language, X4_TR_50_HISTORY_MINUTES_FMT),
            (unsigned long long)(entry->played_s / 60));
    else if (entry->played_s)
        snprintf(played, X4_HISTORY_LABEL, x4_tr_for_lang(language, X4_TR_50_HISTORY_SECONDS_FMT),
            (unsigned long long)entry->played_s);
}
