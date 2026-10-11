/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "rtc_receive_diag.h"
#include <stdatomic.h>

enum { X4_GRACE_INBOX_CAP = 256, X4_GRACE_INBOX_TRIES = 16 };
typedef struct {
    atomic_bool ready;
    X4NackGraceControl control;
} X4NackGraceCell;
typedef struct X4NackGraceInbox {
    X4NackGraceCell cells[X4_GRACE_INBOX_CAP];
    atomic_uint_fast64_t enqueue, dequeue, admitted, omitted, stale;
    atomic_uint epoch;
} X4NackGraceInbox;
/* Fixed storage, independent of optional diagnostics. One worker consumes.
 * The RTC owner binds before producers and unbinds/joins before storage dies.
 * Callback producers do at most16 CAS attempts; no allocation/wait/I/O. */
void x4_nack_grace_inbox_init(X4NackGraceInbox *);
bool x4_nack_grace_inbox_pop(X4NackGraceInbox *, X4NackGraceControl *);
/* Single consumer only: observe the published head without releasing its
 * fixed cell. Media continues while an unproven request waits in this FIFO. */
bool x4_nack_grace_inbox_peek(X4NackGraceInbox *, X4NackGraceControl *);
void x4_nack_grace_inbox_bind(X4NackGraceInbox *, uint32_t transport_epoch);
void x4_nack_grace_inbox_unbind(X4NackGraceInbox *);
