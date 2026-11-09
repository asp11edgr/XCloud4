/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#if defined(X4_VIDEO_PROBE) && X4_VIDEO_PROBE && \
    defined(X4_PROBE_T26) && X4_PROBE_T26

#include <stddef.h>
#include <stdint.h>

/* Independent declarations of PS4 x86-64 interface facts; no decoder
 * implementation is copied. Natural alignment is required, never packed.
 *
 * Layout/signature reference: shadPS4, GPL-2.0-or-later, fixed at
 * 860d73b7e99559106a3ffa3d3d69f0f9237e115e,
 * src/core/libraries/videodec/videodec.h:12-99.
 * AVC=0 and default flags=0 are documented by OpenOrbis PR #213,
 * Backporter commit 9b9e82a2ec4e8cd3c34a086ca82339032cf69da0,
 * include/orbis/_types/Videodec.h:3-6,32-56.
 *
 * These compile-time facts do not establish input/output retention rules,
 * firmware acceptance, or which physical decoder engine is used. T26 must
 * record every native result and retain memory while ownership is uncertain.
 * SDK Videodec.h is deliberately not included: its shipped void prototypes
 * do not describe this interface. Resolve the typed functions in the probe.
 */

#define X4_T26_V1_CODEC_AVC       UINT32_C(0)
#define X4_T26_V1_PROFILE_MAIN    UINT32_C(77)
#define X4_T26_V1_LEVEL_4_2       UINT32_C(42)
#define X4_T26_V1_FLAGS_DEFAULT  UINT64_C(0)

#define X4_T26_V1_SYMBOL_QUERY   "sceVideodecQueryResourceInfo"
#define X4_T26_V1_SYMBOL_CREATE  "sceVideodecCreateDecoder"
#define X4_T26_V1_SYMBOL_DECODE  "sceVideodecDecode"
#define X4_T26_V1_SYMBOL_FLUSH   "sceVideodecFlush"
#define X4_T26_V1_SYMBOL_DELETE  "sceVideodecDeleteDecoder"

/* NID/name facts: the same shadPS4 commit, videodec.cpp:179-187. */
#define X4_T26_V1_NID_QUERY      "leCAscipfFY"
#define X4_T26_V1_NID_CREATE     "qkgRiwHyheU"
#define X4_T26_V1_NID_DECODE     "q0W5GJMovMs"
#define X4_T26_V1_NID_FLUSH      "jeigLlKdp5I"
#define X4_T26_V1_NID_DELETE     "U0kpGF1cl90"

typedef struct X4T26V1Config {
    uint64_t thisSize;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    int32_t maxFrameWidth;
    int32_t maxFrameHeight;
    int32_t maxDpbFrameCount;
    uint64_t videodecFlags;
} X4T26V1Config;

typedef struct X4T26V1Resource {
    uint64_t thisSize;
    uint64_t cpuMemorySize;
    void *pCpuMemory;
    uint64_t cpuGpuMemorySize;
    void *pCpuGpuMemory;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment;
    /* Natural tail padding: bytes 52-55. */
} X4T26V1Resource;

typedef struct X4T26V1Ctrl {
    uint64_t thisSize;
    void *handle;
    uint64_t version;
} X4T26V1Ctrl;

typedef struct X4T26V1FrameBuffer {
    uint64_t thisSize;
    void *pFrameBuffer;
    uint64_t frameBufferSize;
} X4T26V1FrameBuffer;

typedef struct X4T26V1Input {
    uint64_t thisSize;
    void *pAuData;
    uint64_t auSize;
    uint64_t ptsData;
    uint64_t dtsData;
    uint64_t attachedData;
} X4T26V1Input;

typedef struct X4T26V1Avc {
    uint32_t numUnitsInTick;
    uint32_t timeScale;
    uint8_t fixedFrameRateFlag;
    uint8_t aspectRatioIdc;
    uint16_t sarWidth;
    uint16_t sarHeight;
    uint8_t colourPrimaries;
    uint8_t transferCharacteristics;
    uint8_t matrixCoefficients;
    uint8_t videoFullRangeFlag;
    /* Natural padding: bytes 18-19 before the first uint32_t crop. */
    uint32_t frameCropLeftOffset;
    uint32_t frameCropRightOffset;
    uint32_t frameCropTopOffset;
    uint32_t frameCropBottomOffset;
} X4T26V1Avc;

typedef union X4T26V1Codec {
    uint8_t reserved[64];
    X4T26V1Avc avc;
} X4T26V1Codec;

typedef struct X4T26V1Picture {
    uint64_t thisSize;
    uint32_t isValid;
    uint32_t codecType;
    uint32_t frameWidth;
    uint32_t framePitch;
    uint32_t frameHeight;
    uint32_t isErrorPic;
    uint64_t ptsData;
    uint64_t attachedData;
    X4T26V1Codec codec;
} X4T26V1Picture;

/* The native target uses the x86-64 System V calling convention. Unlike
 * Videodec2, Create writes a 24-byte Ctrl rather than an opaque void**.
 * Decode/Flush return framebuffer identity through their in/out argument;
 * Picture carries metadata and uint32_t validity/error fields, not a pointer.
 */
typedef int32_t (*X4T26V1QueryFn)(const X4T26V1Config *, X4T26V1Resource *);
typedef int32_t (*X4T26V1CreateFn)(const X4T26V1Config *,
    const X4T26V1Resource *, X4T26V1Ctrl *);
typedef int32_t (*X4T26V1DecodeFn)(X4T26V1Ctrl *, const X4T26V1Input *,
    X4T26V1FrameBuffer *, X4T26V1Picture *);
typedef int32_t (*X4T26V1FlushFn)(X4T26V1Ctrl *, X4T26V1FrameBuffer *,
    X4T26V1Picture *);
typedef int32_t (*X4T26V1DeleteFn)(X4T26V1Ctrl *);

_Static_assert(sizeof(void *) == 8, "T26 v1 requires 64-bit pointers");
_Static_assert(_Alignof(uint64_t) == 8, "T26 v1 uint64_t alignment");

/* Each offset is stated explicitly so a packing/model change fails before
 * an undocumented native function can observe an incorrectly sized object.
 */
_Static_assert(sizeof(X4T26V1Config) == 40, "T26 v1 config size");
_Static_assert(_Alignof(X4T26V1Config) == 8, "T26 v1 config alignment");
_Static_assert(offsetof(X4T26V1Config, thisSize) == 0, "v1 config thisSize");
_Static_assert(offsetof(X4T26V1Config, codecType) == 8, "v1 config codecType");
_Static_assert(offsetof(X4T26V1Config, profile) == 12, "v1 config profile");
_Static_assert(offsetof(X4T26V1Config, maxLevel) == 16, "v1 config maxLevel");
_Static_assert(offsetof(X4T26V1Config, maxFrameWidth) == 20, "v1 config width");
_Static_assert(offsetof(X4T26V1Config, maxFrameHeight) == 24, "v1 config height");
_Static_assert(offsetof(X4T26V1Config, maxDpbFrameCount) == 28, "v1 config DPB");
_Static_assert(offsetof(X4T26V1Config, videodecFlags) == 32, "v1 config flags");

_Static_assert(sizeof(X4T26V1Resource) == 56, "T26 v1 resource size");
_Static_assert(_Alignof(X4T26V1Resource) == 8, "T26 v1 resource alignment");
_Static_assert(offsetof(X4T26V1Resource, thisSize) == 0, "v1 resource thisSize");
_Static_assert(offsetof(X4T26V1Resource, cpuMemorySize) == 8, "v1 resource CPU size");
_Static_assert(offsetof(X4T26V1Resource, pCpuMemory) == 16, "v1 resource CPU pointer");
_Static_assert(offsetof(X4T26V1Resource, cpuGpuMemorySize) == 24, "v1 resource shared size");
_Static_assert(offsetof(X4T26V1Resource, pCpuGpuMemory) == 32, "v1 resource shared pointer");
_Static_assert(offsetof(X4T26V1Resource, maxFrameBufferSize) == 40, "v1 resource framebuffer size");
_Static_assert(offsetof(X4T26V1Resource, frameBufferAlignment) == 48, "v1 resource framebuffer alignment");

_Static_assert(sizeof(X4T26V1Ctrl) == 24, "T26 v1 ctrl size");
_Static_assert(_Alignof(X4T26V1Ctrl) == 8, "T26 v1 ctrl alignment");
_Static_assert(offsetof(X4T26V1Ctrl, thisSize) == 0, "v1 ctrl thisSize");
_Static_assert(offsetof(X4T26V1Ctrl, handle) == 8, "v1 ctrl handle");
_Static_assert(offsetof(X4T26V1Ctrl, version) == 16, "v1 ctrl version");

_Static_assert(sizeof(X4T26V1FrameBuffer) == 24, "T26 v1 framebuffer size");
_Static_assert(_Alignof(X4T26V1FrameBuffer) == 8, "T26 v1 framebuffer alignment");
_Static_assert(offsetof(X4T26V1FrameBuffer, thisSize) == 0, "v1 framebuffer thisSize");
_Static_assert(offsetof(X4T26V1FrameBuffer, pFrameBuffer) == 8, "v1 framebuffer pointer");
_Static_assert(offsetof(X4T26V1FrameBuffer, frameBufferSize) == 16, "v1 framebuffer bytes");

_Static_assert(sizeof(X4T26V1Input) == 48, "T26 v1 input size");
_Static_assert(_Alignof(X4T26V1Input) == 8, "T26 v1 input alignment");
_Static_assert(offsetof(X4T26V1Input, thisSize) == 0, "v1 input thisSize");
_Static_assert(offsetof(X4T26V1Input, pAuData) == 8, "v1 input AU pointer");
_Static_assert(offsetof(X4T26V1Input, auSize) == 16, "v1 input AU size");
_Static_assert(offsetof(X4T26V1Input, ptsData) == 24, "v1 input PTS");
_Static_assert(offsetof(X4T26V1Input, dtsData) == 32, "v1 input DTS");
_Static_assert(offsetof(X4T26V1Input, attachedData) == 40, "v1 input attached data");

_Static_assert(sizeof(X4T26V1Avc) == 36, "T26 v1 AVC size");
_Static_assert(_Alignof(X4T26V1Avc) == 4, "T26 v1 AVC alignment");
_Static_assert(offsetof(X4T26V1Avc, numUnitsInTick) == 0, "v1 AVC numUnitsInTick");
_Static_assert(offsetof(X4T26V1Avc, timeScale) == 4, "v1 AVC timeScale");
_Static_assert(offsetof(X4T26V1Avc, fixedFrameRateFlag) == 8, "v1 AVC fixed rate");
_Static_assert(offsetof(X4T26V1Avc, aspectRatioIdc) == 9, "v1 AVC aspect ratio");
_Static_assert(offsetof(X4T26V1Avc, sarWidth) == 10, "v1 AVC SAR width");
_Static_assert(offsetof(X4T26V1Avc, sarHeight) == 12, "v1 AVC SAR height");
_Static_assert(offsetof(X4T26V1Avc, colourPrimaries) == 14, "v1 AVC primaries");
_Static_assert(offsetof(X4T26V1Avc, transferCharacteristics) == 15, "v1 AVC transfer");
_Static_assert(offsetof(X4T26V1Avc, matrixCoefficients) == 16, "v1 AVC matrix");
_Static_assert(offsetof(X4T26V1Avc, videoFullRangeFlag) == 17, "v1 AVC full range");
_Static_assert(offsetof(X4T26V1Avc, frameCropLeftOffset) == 20, "v1 AVC crop left");
_Static_assert(offsetof(X4T26V1Avc, frameCropRightOffset) == 24, "v1 AVC crop right");
_Static_assert(offsetof(X4T26V1Avc, frameCropTopOffset) == 28, "v1 AVC crop top");
_Static_assert(offsetof(X4T26V1Avc, frameCropBottomOffset) == 32, "v1 AVC crop bottom");

_Static_assert(sizeof(X4T26V1Codec) == 64, "T26 v1 codec union size");
_Static_assert(_Alignof(X4T26V1Codec) == 4, "T26 v1 codec union alignment");
_Static_assert(offsetof(X4T26V1Codec, reserved) == 0, "v1 codec reserved offset");
_Static_assert(offsetof(X4T26V1Codec, avc) == 0, "v1 codec AVC offset");

_Static_assert(sizeof(X4T26V1Picture) == 112, "T26 v1 picture size");
_Static_assert(_Alignof(X4T26V1Picture) == 8, "T26 v1 picture alignment");
_Static_assert(offsetof(X4T26V1Picture, thisSize) == 0, "v1 picture thisSize");
_Static_assert(offsetof(X4T26V1Picture, isValid) == 8, "v1 picture valid");
_Static_assert(offsetof(X4T26V1Picture, codecType) == 12, "v1 picture codec");
_Static_assert(offsetof(X4T26V1Picture, frameWidth) == 16, "v1 picture width");
_Static_assert(offsetof(X4T26V1Picture, framePitch) == 20, "v1 picture pitch");
_Static_assert(offsetof(X4T26V1Picture, frameHeight) == 24, "v1 picture height");
_Static_assert(offsetof(X4T26V1Picture, isErrorPic) == 28, "v1 picture error");
_Static_assert(offsetof(X4T26V1Picture, ptsData) == 32, "v1 picture PTS");
_Static_assert(offsetof(X4T26V1Picture, attachedData) == 40, "v1 picture attached data");
_Static_assert(offsetof(X4T26V1Picture, codec) == 48, "v1 picture codec info");

#endif /* X4_VIDEO_PROBE && X4_PROBE_T26 */
