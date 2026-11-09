/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_hooks.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_platform.h"
#include "live_h264.h"
#include "stream_sps.h"
#include <stdatomic.h>

enum { X4_T26_BACKEND_ARGUMENT=-7640, X4_T26_BACKEND_MEMORY=-7641,
    X4_T26_BACKEND_LAYOUT=-7642, X4_T26_BACKEND_RETAINED=-7643,
    X4_T26_BACKEND_CANCELLED=3, X4_T26_BACKEND_SPS_MAX=512 };
typedef struct {
    X4T26Config config;
    const uint8_t *sps;size_t sps_bytes;
    const atomic_bool *cancel;
    X4T26BeforeNative before;void *context;
    uint64_t input_arena_bytes;
} X4T26BackendOptions;
typedef struct {
    int native_rc,local_result;
    bool native_called,valid,error,from_flush,layout_verified,picture_count_native;
    uint32_t native_valid,native_error,picture_count;
    const void *native_pointer;size_t native_bytes;
    /* pointer is the authorized known reservation, distinct from the raw
     * native address above when a native/local contract validation fails. */
    const uint8_t *pointer;
    unsigned slot,width,height,pitch;
    size_t owned_bytes,returned_bytes;
    X4VideoNv12Layout layout;
    uint64_t decode_begin_us,decode_end_us,decode_us,decode_call,native_call;
    bool v1_metadata;
    uint64_t v1_pts,v1_attached;
    X4T26V1Avc v1_avc;
} X4T26Output;
typedef struct {
    X4T26Allocation allocation;
    bool before_called,after_called,before_valid,after_valid,release_evidence;
    int before_rc,after_rc;
    X4T26DirectQuery before,after;
} X4T26AllocationCheck;
typedef struct {
    X4T26Config config;X4StreamSps sps;
    bool attempted,opened,closed,retained,owned_clean;
    int open_result,close_result,local_result;
    unsigned output_count,allocations_before,allocations_after;
    uint64_t owned_bytes_before,owned_bytes_after;
    bool largest_before_called,largest_after_called;
    int largest_before_rc,largest_after_rc;
    uint64_t largest_before_bytes,largest_after_bytes;
    X4T26AllocationCheck allocation_checks[X4_T26_ALLOCATION_MAX];
    X4T26PlatformInfo platform;
    X4StreamVideoInfo v2;
    X4T26V1Info v1;
} X4T26BackendInfo;
typedef struct X4T26Backend X4T26Backend;
/* create only copies/parses immutable options. open is the native operation.
 * One owner serializes all functions and keeps cancel/context alive. */
int x4_t26_backend_create(X4T26Backend **,const X4T26BackendOptions *);
int x4_t26_backend_open(X4T26Backend *);
int x4_t26_backend_decode(X4T26Backend *,unsigned offered,const void *,size_t,uint64_t pts,X4T26Output *);
int x4_t26_backend_flush(X4T26Backend *,unsigned offered,X4T26Output *);
uint32_t x4_t26_backend_held_mask(const X4T26Backend *);
void x4_t26_backend_info(X4T26Backend *,X4T26BackendInfo *);
void x4_t26_backend_cpu(X4T26Backend *,X4T26CpuSnapshot *);
/* Geometry/bounds only, no pixel reads. Default v1 AVC output is interpreted
 * as NV12; exact native-baseline hashes still have to confirm that assumption.
 * Only an exact SPS coded geometry is accepted; no guessed crop from size. */
int x4_t26_backend_nv12_layout(const X4StreamSps *,unsigned width,unsigned height,
    unsigned pitch,size_t returned_bytes,size_t owned_bytes,X4VideoNv12Layout *);
/* Caller quiesces every GPU/hash reader BEFORE close. A failure retains the
 * whole object. destroy never retries a failed close or frees live native
 * allocations; an explicit successful close is required after any open. */
int x4_t26_backend_close(X4T26Backend *);
int x4_t26_backend_destroy(X4T26Backend **);
#endif
