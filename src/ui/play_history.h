/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "product_metadata.h"

#define X4_HISTORY_PATH "/data/xcloud4/history-v1.bin"
enum { X4_HISTORY_MAX = 500, X4_HISTORY_LABEL = 48 };

typedef struct {
    char product_id[X4_PRODUCT_ID_CAP];
    uint64_t last_session_s, played_s;
} X4HistoryEntry;

typedef struct {
    X4HistoryEntry entries[X4_HISTORY_MAX];
    unsigned count;
    uint64_t revision;
} X4History;

/* Main/UI owner only. Missing or corrupt files leave an empty history. */
bool x4_history_load(X4History *history, const char *path);
/* On failure returns false and preserves the first I/O errno through cleanup. */
bool x4_history_record(X4History *history, const char *product_id,
    uint64_t last_session_s, uint64_t added_seconds, const char *path);
const X4HistoryEntry *x4_history_find(const X4History *history, const char *product_id);
uint64_t x4_history_now(void);
/* Optional localized values, without the screen's label. */
void x4_history_text(const X4HistoryEntry *entry, uint64_t now, bool english,
    char last[X4_HISTORY_LABEL], char played[X4_HISTORY_LABEL]);
