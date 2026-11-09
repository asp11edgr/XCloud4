/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#if X4_PUBLIC
#define X4_UPDATE51_MANIFEST "xcloud4-update-public.json"
#else
#define X4_UPDATE51_MANIFEST "xcloud4-update-lab.json"
#define X4_UPDATE51_LAB_ROW 24u
#define X4_UPDATE51_LAB_PENDING_ROW (X4_UPDATE51_LAB_ROW + 1u)
#endif
#define X4_UPDATE51_SIGNATURE X4_UPDATE51_MANIFEST ".sig"

typedef enum {
    X4_UPDATE51_IDLE,
    X4_UPDATE51_FETCHING,
    X4_UPDATE51_DOWNLOADING,
    X4_UPDATE51_VERIFYING,
    X4_UPDATE51_INSTALLING,
    X4_UPDATE51_ACCEPTED,
    X4_UPDATE51_CANCELLED,
    X4_UPDATE51_FAILED,
    X4_UPDATE51_CHOOSE,
    X4_UPDATE51_MANUAL,
    X4_UPDATE51_UPDATED,
} X4Update51Stage;

typedef enum {
    X4_UPDATE51_GENERAL,
    X4_UPDATE51_BAD_SIGNATURE,
    X4_UPDATE51_BAD_HASH,
    X4_UPDATE51_NO_SPACE,
    X4_UPDATE51_OLD_VERSION,
    X4_UPDATE51_READ,
    X4_UPDATE51_BAD_MANIFEST,
    X4_UPDATE51_BAD_PACKAGE,
    X4_UPDATE51_BUSY,
    X4_UPDATE51_NETWORK,
    X4_UPDATE51_INSTALL_ERROR,
    X4_UPDATE51_START_ERROR,
} X4Update51Error;

typedef struct {
    X4Update51Stage stage;
    X4Update51Error error;
    int32_t code;
    uint32_t received, total;
    bool busy;
    bool manual_selected;
    bool resuming;
    unsigned close_seconds;
} X4Update51View;

typedef struct {
    char version[48], pkg_name[129];
    int32_t code;
} X4Update51ManualView;

/* The caller owns menu authorization and copies the validated release tag. */
bool x4_update51_start(const char *tag, bool local);
/* Log every confirmation. A rejection is returned without replacing the
 * view owned by an already-running attempt. */
bool x4_update51_start_checked(const char *tag, bool local, bool allowed, X4Update51View *rejected);
void x4_update51_cancel(void);
void x4_update51_exit(void);
void x4_update51_boot_cleanup(void);
/* A pending marker is cancellable only by its own title when the worker is
 * idle. These calls also discard staging whose marker has expired. */
bool x4_update51_pending(void);
/* Interrupted installations are retried through the existing confirmation. */
bool x4_update51_retry_available(bool *local);
bool x4_update51_cancel_pending(void);
X4Update51View x4_update51_view(void);
#if !X4_PUBLIC
void x4_update51_select_install(bool manual);
void x4_update51_confirm_install(void);
#endif
bool x4_update51_manual_view(X4Update51ManualView *out);
bool x4_update51_applied_view(X4Update51ManualView *out);
bool x4_update51_take_manual_offer(void);
#if !X4_PUBLIC
bool x4_update51_diagnose_appinst(void);
#endif
