/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#ifndef X4_PROBE_T26
#define X4_PROBE_T26 0
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { X4_T26_METRIC_CAPACITY=1024, X4_T26_DELAY_BUCKETS=4 };
typedef struct {uint64_t values[X4_T26_METRIC_CAPACITY];unsigned count;} X4T26Samples;
typedef struct {uint64_t n,p50_us,p95_us,p99_us,max_us;double mean_us;} X4T26Stats;
void x4_t26_samples_reset(X4T26Samples *);
int x4_t26_samples_add(X4T26Samples *,uint64_t value_us);
/* Exact nearest rank. Allocates bounded sorting scratch after measurement,
 * frees it before return; never mutates input. Empty is a valid zeroed stats.
 * On error *out remains unchanged. */
int x4_t26_samples_stats(const X4T26Samples *,X4T26Stats *out);
/* Entire empty object is JSON null. Non-finite/inconsistent input is rejected
 * before any write. Return0 on success; negative on bad input/write failure. */
int x4_t26_stats_json(FILE *,const X4T26Stats *);

enum { X4_T26_MATCH_NOT_VERIFIED=1, X4_T26_MATCH_ORDER=2,
    X4_T26_MATCH_NATIVE_ERROR=4, X4_T26_MATCH_COUNT=8,
    X4_T26_MATCH_CLOCK=16, X4_T26_MATCH_CAPACITY=32,
    X4_T26_MATCH_LOCAL_CONTRACT=64 };
typedef struct {
    /* Decode index starts at1. Flush uses the last Decode index and sets
     * flushing=true; it is never presented as a further Decode call. */
    uint64_t call_index,au_ordinal,au_begin_us,output_ready_us;
    bool has_au,output_valid,is_error_frame,flushing;
    int native_rc;
    unsigned picture_count;
} X4T26CallRecord;
typedef struct {
    bool has_output,inferred,from_flush;
    uint64_t au_ordinal,submitted_call,returned_call,call_delay,latency_us;
} X4T26Association;
/* Heap-owned context recommended. The FIFO is only an INFERENCE justified
 * by verified no-B/POC2/one-picture-per-AU fixtures, never by an offered slot.
 * Any error/count/order mismatch permanently invalidates and clears inferred
 * samples/histogram, so partial associations cannot masquerade as600 samples. */
typedef struct {
    bool inference_valid,have_last_output;
    unsigned invalid_reasons,head,pending;
    uint64_t first_measured,last_measured,submitted,outputs,decode_calls;
    uint64_t no_output_calls,flush_calls,drained_outputs,measured_outputs,last_output_us,last_au_us;
    uint64_t ordinal[X4_T26_METRIC_CAPACITY],began_us[X4_T26_METRIC_CAPACITY],call[X4_T26_METRIC_CAPACITY];
    X4T26Samples au_latency,output_interval;
    /* [0]=N, [1]=N+1, [2]=N+2, [3]=N+3 or later. Flush excluded. */
    uint64_t call_delay_hist[X4_T26_DELAY_BUCKETS],measured_flush_outputs;
} X4T26Matcher;
int x4_t26_matcher_reset(X4T26Matcher *,bool verified_no_b_poc2,
    uint64_t first_measured_au,uint64_t last_measured_au);
/* 0 processed with valid inference; 1 inference invalid (reasons retained),
 * negative invalid pointer arguments. Empty valid=0 calls are ordinary. */
int x4_t26_matcher_call(X4T26Matcher *,const X4T26CallRecord *,X4T26Association *);
/* Local pre-Decode/layout/control rejection, without fabricating a native rc,
 * error-frame flag or picture count. Clears inferred measurements only. */
void x4_t26_matcher_invalidate(X4T26Matcher *,unsigned reason);
/* After bounded EOF drain. Requires balanced all-input/output counts and no
 * pending AU, plus the full requested measured range. False invalidates. */
bool x4_t26_matcher_finish(X4T26Matcher *,uint64_t expected_inputs,uint64_t expected_outputs);
#endif
