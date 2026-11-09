/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdint.h>

#if !X4_PUBLIC && !X4_VIDEO_PROBE
enum {
    X4_XSTS_DIAG_RESPONSES = 32,
    X4_XSTS_DIAG_NAMES = 16,
    X4_XSTS_DIAG_NAME_BYTES = 32,
};

typedef enum {
    X4_XSTS_DIAG_GSSV,
    X4_XSTS_DIAG_MARKET,
} X4XstsDiagParty;

typedef struct {
    X4XstsDiagParty party;
    int status;
    unsigned count, omitted;
    char names[X4_XSTS_DIAG_NAMES][X4_XSTS_DIAG_NAME_BYTES];
} X4XstsDiagResponse;

typedef struct {
    uint64_t revision;
    unsigned count;
    X4XstsDiagResponse responses[X4_XSTS_DIAG_RESPONSES];
} X4XstsDiagSnapshot;

/* The app process keeps the most recent responses, in receipt order. */
uint64_t x4_xsts_diag_revision(void);
void x4_xsts_diag_snapshot(X4XstsDiagSnapshot *out);
#endif
