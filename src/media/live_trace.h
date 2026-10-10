/* SPDX-License-Identifier: GPL-3.0-only */
/* Optional bounded numeric diagnostics. No playback decisions or live I/O. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct X4Trace X4Trace;

typedef struct {
    uint64_t t_us, a, b;
    uint32_t ordinal;
    uint16_t event, flags;
} X4TraceRecord;
_Static_assert(sizeof(X4TraceRecord) == 32, "trace record ABI");

enum {
    X4_TRACE_AU_BEGIN = 0x100, X4_TRACE_AU_COMPLETE, X4_TRACE_AU_RESET,
    X4_TRACE_WAIT_IDR, X4_TRACE_IDR_ACCEPT, X4_TRACE_PLI_REQUEST,
    X4_TRACE_PLI_DISPATCH, X4_TRACE_QUEUE_POP, X4_TRACE_QUEUE_ACCEPT,
    X4_TRACE_QUEUE_DROP, X4_TRACE_REORDER_INSERT, X4_TRACE_REORDER_EMIT,
    X4_TRACE_REORDER_HOLE, X4_TRACE_REORDER_DROP, X4_TRACE_WORKER_YIELD,
    X4_TRACE_MEDIA_START, X4_TRACE_MEDIA_STOP, X4_TRACE_SOURCE_EPOCH,

    X4_TRACE_DECODE_BEGIN = 0x200, X4_TRACE_DECODE_END,
    X4_TRACE_OUTPUT_VALID, X4_TRACE_OUTPUT_NONE, X4_TRACE_OUTPUT_REJECT,
    X4_TRACE_OUTPUT_SUPERSEDED, X4_TRACE_COPY_BEGIN, X4_TRACE_COPY_END,
    X4_TRACE_COPY_OWNER_ELAPSED, X4_TRACE_COPY_HELPER_ELAPSED,
    X4_TRACE_COPY_WAIT_ELAPSED, X4_TRACE_COPY_CHECK,
    X4_TRACE_CONVERT_BEGIN, X4_TRACE_CONVERT_END,
    X4_TRACE_RGB_PUBLICATION, X4_TRACE_RGB_SUPERSEDED,

    X4_TRACE_DRAW_BEGIN = 0x300, X4_TRACE_DRAW_END, X4_TRACE_DRAW_SKIP,
    X4_TRACE_FLIP_SUBMIT, X4_TRACE_GNM_DONE, X4_TRACE_FLIP_MATCH,
    X4_TRACE_PRESENT_NEW, X4_TRACE_PRESENT_REPEAT, X4_TRACE_FLIP_FAIL,

    X4_TRACE_RX_VALID = 0x400, X4_TRACE_RX_REJECT, X4_TRACE_RX_CALLBACK,

    X4_TRACE_SESSION_START = 0x500, X4_TRACE_ACTIVE_CHANGE,
    X4_TRACE_GAP_BEGIN, X4_TRACE_GAP_CLOSED, X4_TRACE_WINDOW_OPEN,
    X4_TRACE_WINDOW_FROZEN, X4_TRACE_SESSION_END
};

enum {
    X4_TRACE_F_MARKER = 1, X4_TRACE_F_IDR = 2,
    X4_TRACE_F_NEW = 4, X4_TRACE_F_REPEAT = 8,
    X4_TRACE_F_RECONSTRUCTED = 16, X4_TRACE_F_LATE_DETECTION = 32,
    X4_TRACE_F_OBSERVED_METADATA = 64, X4_TRACE_F_UI = 128
};

enum {
    X4_TRACE_R_NONE = 0, X4_TRACE_R_RTP_INVALID, X4_TRACE_R_SIZE,
    X4_TRACE_R_QUEUE_FULL, X4_TRACE_R_PUSH_CONTENDED, X4_TRACE_R_POP_CONTENDED,
    X4_TRACE_R_FOREIGN, X4_TRACE_R_DUPLICATE_LATE, X4_TRACE_R_REORDER_JUMP,
    X4_TRACE_R_SEQUENCE_HOLE, X4_TRACE_R_INGRESS_GAP,
    X4_TRACE_R_TIMESTAMP_CHANGE, X4_TRACE_R_FRAGMENT, X4_TRACE_R_NAL,
    X4_TRACE_R_AU_SIZE, X4_TRACE_R_EMPTY_AU, X4_TRACE_R_WAIT_IDR,
    X4_TRACE_R_IDR_PARAMETERS, X4_TRACE_R_DECODE, X4_TRACE_R_PICTURE,
    X4_TRACE_R_NO_RGB_SLOT, X4_TRACE_R_COPY_UNAVAILABLE, X4_TRACE_R_COPY_BOUNDS,
    X4_TRACE_R_COPY_MISMATCH, X4_TRACE_R_COPY_JOB,
    X4_TRACE_R_BUDGET_TIME, X4_TRACE_R_BUDGET_DECODE,
    X4_TRACE_R_USER_STOP, X4_TRACE_R_NATIVE,
    X4_TRACE_R_EVENT_CAP, X4_TRACE_R_TIME_CAP, X4_TRACE_R_STORAGE_EXHAUSTED,
    X4_TRACE_R_PREHISTORY_SHORT, X4_TRACE_R_CLOCK, X4_TRACE_R_NO_OUTPUT,
    X4_TRACE_R_FILE, X4_TRACE_R_SOURCE_CHANGE, X4_TRACE_R_SESSION_END,
    X4_TRACE_R_SUBMIT, X4_TRACE_R_GNM, X4_TRACE_R_STATUS, X4_TRACE_R_TIMEOUT,
    X4_TRACE_R_IDENTITY
};
#define X4_TRACE_REASON(reason) ((uint16_t)((unsigned)(reason) << 8))

enum {
    X4_TRACE_C_RECORD_DROP = 1u, X4_TRACE_C_SHORT_PRE = 2u,
    X4_TRACE_C_EVENT_CAP = 4u, X4_TRACE_C_TIME_CAP = 8u,
    X4_TRACE_C_STORAGE_CAP = 16u, X4_TRACE_C_SNAPSHOT_UNSTABLE = 32u,
    X4_TRACE_C_MISSING_GAP_DETAIL = 64u, X4_TRACE_C_OPEN_AT_STOP = 128u,
    X4_TRACE_C_ORDINAL_WRAP = 256u, X4_TRACE_C_IDENTITY = 512u
};

enum { X4_TRACE_DUMP_OK = 0, X4_TRACE_DUMP_DISABLED = 1,
    X4_TRACE_DUMP_ARGUMENT = -1, X4_TRACE_DUMP_NOT_QUIESCED = -2,
    X4_TRACE_DUMP_IO = -3 };

/* Optional one-time allocation before producers start; NULL means disabled. */
/* Configured reader count describes the experiment, not a guarantee that
 * helper startup/bytecheck selected the parallel path. Copy events say that. */
X4Trace *x4_trace_create(uint64_t local_session_ordinal, unsigned configured_readers);
uint64_t x4_trace_now_us(void);
/* Any producer. One try-only gate; records may be dropped, playback may not. */
void x4_trace_record(X4Trace *trace, uint16_t event, uint16_t flags,
                     uint64_t a, uint64_t b);
/* Any producer; bounded one-attempt coherent cadence read and try-only gate. */
void x4_trace_poll(X4Trace *trace);
/* Main only. Intentional inactive/closing periods disarm cadence. */
void x4_trace_set_active(X4Trace *trace, bool active);
/* Main/display only, at actual status.flipArg==submitted_flip_arg. NEW
 * cadence updates independently of whether a record is admitted. Repeat/UI
 * matches do not reset it. observed_status_num has no invented FPS meaning. */
void x4_trace_present_complete(X4Trace *trace, uint64_t drawn_generation,
    bool new_generation, uint64_t observed_status_num, uint64_t submitted_flip_arg);
/* Main only AFTER all RTC/video/other producers have actually stopped/joined.
 * If a producer lifetime is retained, retain trace and do not call these. */
void x4_trace_end_session(X4Trace *trace);
int x4_trace_dump_file(X4Trace *trace, const char *numeric_local_path);
/* Returns false and retains storage if end_session was not called. */
bool x4_trace_free(X4Trace *trace);
