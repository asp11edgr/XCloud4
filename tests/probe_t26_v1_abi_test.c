/* SPDX-License-Identifier: GPL-3.0-only */
#include "video/probe_t26_v1_abi.h"

#if !defined(X4_VIDEO_PROBE) || !X4_VIDEO_PROBE || \
    !defined(X4_PROBE_T26) || !X4_PROBE_T26
#error "T26 v1 ABI host test requires both probe build switches"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Header asserts validate every ABI field. These checks independently pin
 * the const/mutable argument contract used by the probe's dynamic resolver.
 */
_Static_assert(_Generic((X4T26V1QueryFn)0,
    int32_t (*)(const X4T26V1Config *, X4T26V1Resource *): 1,
    default: 0), "v1 query function type");
_Static_assert(_Generic((X4T26V1CreateFn)0,
    int32_t (*)(const X4T26V1Config *, const X4T26V1Resource *, X4T26V1Ctrl *): 1,
    default: 0), "v1 create function type");
_Static_assert(_Generic((X4T26V1DecodeFn)0,
    int32_t (*)(X4T26V1Ctrl *, const X4T26V1Input *,
        X4T26V1FrameBuffer *, X4T26V1Picture *): 1,
    default: 0), "v1 decode function type");
_Static_assert(_Generic((X4T26V1FlushFn)0,
    int32_t (*)(X4T26V1Ctrl *, X4T26V1FrameBuffer *, X4T26V1Picture *): 1,
    default: 0), "v1 flush function type");
_Static_assert(_Generic((X4T26V1DeleteFn)0,
    int32_t (*)(X4T26V1Ctrl *): 1,
    default: 0), "v1 delete function type");

int main(void)
{
    assert(X4_T26_V1_CODEC_AVC == 0);
    assert(X4_T26_V1_PROFILE_MAIN == 77);
    assert(X4_T26_V1_LEVEL_4_2 == 42);
    assert(X4_T26_V1_FLAGS_DEFAULT == 0);
    assert(strcmp(X4_T26_V1_SYMBOL_QUERY, "sceVideodecQueryResourceInfo") == 0);
    assert(strcmp(X4_T26_V1_SYMBOL_CREATE, "sceVideodecCreateDecoder") == 0);
    assert(strcmp(X4_T26_V1_SYMBOL_DECODE, "sceVideodecDecode") == 0);
    assert(strcmp(X4_T26_V1_SYMBOL_FLUSH, "sceVideodecFlush") == 0);
    assert(strcmp(X4_T26_V1_SYMBOL_DELETE, "sceVideodecDeleteDecoder") == 0);
    assert(strcmp(X4_T26_V1_NID_QUERY, "leCAscipfFY") == 0);
    assert(strcmp(X4_T26_V1_NID_CREATE, "qkgRiwHyheU") == 0);
    assert(strcmp(X4_T26_V1_NID_DECODE, "q0W5GJMovMs") == 0);
    assert(strcmp(X4_T26_V1_NID_FLUSH, "jeigLlKdp5I") == 0);
    assert(strcmp(X4_T26_V1_NID_DELETE, "U0kpGF1cl90") == 0);
    puts("PASS T26 v1 ABI sizes, offsets, function types and symbol facts; no native decoder called");
    return 0;
}
