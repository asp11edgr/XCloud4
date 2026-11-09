/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#if defined(X4_VIDEO_PROBE) && X4_VIDEO_PROBE && \
    defined(X4_PROBE_T26) && X4_PROBE_T26

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "probe_t26_v1_abi.h"

enum {
    X4_T26_V1_OUTPUTS = 5,
    X4_T26_V1_MEMORY_COUNT = 8,
    X4_T26_V1_ARGUMENT = -7620,
    X4_T26_V1_NO_MEMORY = -7621,
    X4_T26_V1_BUDGET = -7622,
    X4_T26_V1_CONTRACT = -7623,
    X4_T26_V1_SLOT_HELD = -7624,
    X4_T26_V1_ARENA_FULL = -7625,
    X4_T26_V1_NOT_OPEN = -7626,
    X4_T26_V1_RETAINED = -7627,
    X4_T26_V1_RESOLVE = -7628
};
#define X4_T26_V1_NATIVE_LIMIT (UINT64_C(192) * 1024 * 1024)

typedef struct {
    void *pointer;
    uint64_t bytes;
    int64_t offset;
    bool allocated;
} X4T26V1Memory;

/* Allocate/release own the direct-memory bookkeeping. Allocate must report
 * any partial reservation through memory.allocated even when it fails.
 * Release returns 0 only after unmap and physical release both succeeded;
 * it must leave an unsuccessful reservation represented and retryable.
 * load_module returns 0 only after the requested module is known loaded.
 * before_native is a durable-marker guard, not a cancellable decoder wait.
 * The owner must continue to allow safe Delete markers during cancellation.
 */
typedef struct {
    void *context;
    int (*allocate)(void *, uint64_t size, uint64_t alignment, int type,
        X4T26V1Memory *);
    int (*release)(void *, X4T26V1Memory *);
    int (*load_module)(void *, unsigned module_id);
    int (*resolve)(void *, const char *symbol, void **address);
    int (*before_native)(void *, const char *operation);
    uint64_t (*clock_us)(void *);
} X4T26V1Ops;

typedef struct {
    unsigned width;
    unsigned coded_height;
    uint64_t input_arena_bytes;
    /* 0 selects 192 MiB. A nonzero value may lower, never raise, the cap. */
    uint64_t native_budget_bytes;
} X4T26V1Options;

typedef struct {
    X4T26V1Config config;
    X4T26V1Resource query_memory;
    bool module_called;
    int module_rc;
    bool arbitration_called;
    int arbitration_rc;
    bool arbitration_retry;
    unsigned query_called_mask;
    int query_rc[2];
    unsigned create_called_mask;
    int create_rc[2];
    unsigned resolve_called_mask;
    int resolve_rc[5];
    bool delete_called;
    int delete_rc;
    int last_decode_rc;
    int last_flush_rc;
    int local_result;
    uint64_t query_calls;
    uint64_t create_calls;
    uint64_t decode_calls;
    uint64_t flush_calls;
    uint64_t delete_calls;
    /* Decoder API calls only; module/resolver/allocator calls are separate. */
    uint64_t native_calls;
    uint64_t valid_outputs;
    /* Decode-only no-output count; EOF drain is counted separately. */
    uint64_t without_output;
    uint64_t flush_without_output;
    uint64_t error_outputs;
    uint64_t input_used_bytes;
    uint64_t allocated_bytes;
    uint64_t peak_allocated_bytes;
    unsigned allocation_count;
    uint32_t held_mask;
    bool opened;
    bool decoder_live;
    bool retained;
} X4T26V1Info;

typedef struct {
    int native_rc;
    int result;
    bool valid;
    bool error;
    bool was_flush;
    uint32_t native_valid;
    uint32_t native_error;
    void *pointer;
    uint64_t bytes;
    unsigned slot;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t pts;
    uint64_t attached;
    X4T26V1Avc avc;
    uint64_t decode_begin_us;
    uint64_t decode_end_us;
    uint64_t decode_us;
    /* Decode+Flush invocation ordinal, excluding Query/Create/Delete. */
    uint64_t native_call;
    uint64_t decode_call;
} X4T26V1Output;

typedef struct X4T26V1 X4T26V1;

/* Single-owner API. The bank serializes calls and observes cancellation
 * between native calls. No caller may close/destroy or read these snapshots
 * concurrently with an executing native call. A synchronous native Decode
 * cannot safely be interrupted by freeing its buffers from another thread.
 */
int x4_t26_v1_create(X4T26V1 **, const X4T26V1Ops *, const X4T26V1Options *);
int x4_t26_v1_open(X4T26V1 *);
int x4_t26_v1_decode(X4T26V1 *, unsigned offered_slot, const void *au,
    size_t bytes, uint64_t pts, X4T26V1Output *);
int x4_t26_v1_flush(X4T26V1 *, unsigned offered_slot, X4T26V1Output *);
uint32_t x4_t26_v1_held_mask(const X4T26V1 *);
void x4_t26_v1_info(const X4T26V1 *, X4T26V1Info *);
size_t x4_t26_v1_allocations(const X4T26V1 *, X4T26V1Memory *, size_t capacity);
/* Close is retryable on Delete/release failure. No allocation is freed while
 * the decoder can own it. Destroy refuses to free the wrapper on failure. */
int x4_t26_v1_close(X4T26V1 *);
int x4_t26_v1_destroy(X4T26V1 **);

#endif
