/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum UiBootEvent {
    BOOT_RESTORED, BOOT_GAMERTAG, BOOT_CATALOG_REQUESTED, BOOT_CATALOG_RECEIVED,
    BOOT_RECENTS_REQUESTED, BOOT_RECENTS_RECEIVED, BOOT_SIGLS_REQUESTED,
    BOOT_SIGLS_RECEIVED, BOOT_HOME_FIRST, BOOT_HOME_LAST, BOOT_LANGUAGE,
    BOOT_SPLASH_HIDDEN, BOOT_EVENT_COUNT
};

typedef struct {
    uint64_t started_us, event[BOOT_EVENT_COUNT], home_draws, next_write_us, xsts_revision;
    uint64_t saved_skipped;
    int write_errno;
    int32_t splash_code;
    bool saved, splash_called;
} UiBootDiag;

void ui_boot_event(UiBootDiag *diag, enum UiBootEvent event, uint64_t now);
/* A successful write latches the boot measurements. New XSTS responses
 * refresh the claim-name list. A failed write can retry after a second. */
bool ui_boot_diag_save(UiBootDiag *diag, uint64_t now, uint64_t skipped, bool exiting);
