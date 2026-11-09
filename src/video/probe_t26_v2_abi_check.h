/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#ifndef X4_PROBE_T26
#define X4_PROBE_T26 0
#endif

#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include "videodec2_abi.h"

/* Check the existing types; do not introduce a second ABI or change them.
 * Their source is Backporter's OpenOrbis PR #213, fixed at commit
 * 9b9e82a2ec4e8cd3c34a086ca82339032cf69da0:
 * https://github.com/Backporter/OpenOrbis-PS4-Toolchain/blob/
 * 9b9e82a2ec4e8cd3c34a086ca82339032cf69da0/include/orbis/_types/Videodec.h
 *
 * Layout derivation on the project's native 64-bit ABI: size_t and pointers
 * occupy/alignment 8, uint64_t alignment 8, uint32_t alignment 4, uint16_t
 * alignment 2, and bool/uint8_t occupy/alignment 1. Each field starts at the
 * next boundary required by its type; each struct ends at a multiple of 8.
 * The FIELD checks below cover every declared field, including reserved ones.
 *
 * OutputInfo deliberately stays 48 bytes, the current production form used
 * in the owner's console-validated decoder sessions. A newer emulator's
 * 56-byte extension is not adopted. These checks prove C layout only; they
 * do not constitute a new firmware acceptance or console execution result.
 */
_Static_assert(CHAR_BIT == 8, "T26 Videodec2 needs 8-bit bytes");
_Static_assert(sizeof(size_t) == 8 && _Alignof(size_t) == 8,
    "T26 Videodec2 needs 64-bit size_t aligned to 8");
_Static_assert(sizeof(void *) == 8 && _Alignof(void *) == 8,
    "T26 Videodec2 needs 64-bit pointers aligned to 8");
_Static_assert(sizeof(uint64_t) == 8 && _Alignof(uint64_t) == 8,
    "T26 Videodec2 uint64_t ABI");
_Static_assert(sizeof(uint32_t) == 4 && _Alignof(uint32_t) == 4,
    "T26 Videodec2 uint32_t ABI");
_Static_assert(sizeof(int32_t) == 4 && _Alignof(int32_t) == 4,
    "T26 Videodec2 int32_t ABI");
_Static_assert(sizeof(uint16_t) == 2 && _Alignof(uint16_t) == 2,
    "T26 Videodec2 uint16_t ABI");
_Static_assert(sizeof(uint8_t) == 1 && _Alignof(uint8_t) == 1,
    "T26 Videodec2 uint8_t ABI");
_Static_assert(sizeof(bool) == 1 && _Alignof(bool) == 1,
    "T26 Videodec2 bool ABI");

#define X4_T26_V2_STRUCT(type, bytes) \
    _Static_assert(sizeof(type) == (bytes), "T26 " #type " size"); \
    _Static_assert(_Alignof(type) == 8, "T26 " #type " alignment")
#define X4_T26_V2_FIELD(type, member, offset, bytes) \
    _Static_assert(offsetof(type, member) == (offset), \
        "T26 " #type "." #member " offset"); \
    _Static_assert(sizeof(((type *)0)->member) == (bytes), \
        "T26 " #type "." #member " size")

/* 0..39: scalar config; 40..55: queue/affinity; 56..63: priority and
 * byte flags/reserved; 64..71: optional config pointer. No hidden tail. */
X4_T26_V2_STRUCT(OrbisVideodec2DecoderConfigInfo, 72);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, resourceType, 8, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, codecType, 12, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, profile, 16, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, maxLevel, 20, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, maxFrameWidth, 24, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, maxFrameHeight, 28, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, maxDpbFrameCount, 32, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, decodePipelineDepth, 36, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, computeQueue, 40, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, cpuAffinityMask, 48, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, cpuThreadPriority, 56, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, optimizeProgressiveVideo, 60, 1);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, checkMemoryType, 61, 1);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, reserved0, 62, 1);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, reserved1, 63, 1);
X4_T26_V2_FIELD(OrbisVideodec2DecoderConfigInfo, extraConfigInfo, 64, 8);

/* Every size/pointer pair occupies 16 bytes; the final two uint32_t fields
 * fill bytes 64..71. */
X4_T26_V2_STRUCT(OrbisVideodec2DecoderMemoryInfo, 72);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, cpuMemorySize, 8, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, pCpuMemory, 16, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, gpuMemorySize, 24, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, pGpuMemory, 32, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, cpuGpuMemorySize, 40, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, pCpuGpuMemory, 48, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, maxFrameBufferSize, 56, 8);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, frameBufferAlignment, 64, 4);
X4_T26_V2_FIELD(OrbisVideodec2DecoderMemoryInfo, reserved0, 68, 4);

X4_T26_V2_STRUCT(OrbisVideodec2InputData, 48);
X4_T26_V2_FIELD(OrbisVideodec2InputData, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2InputData, pAuData, 8, 8);
X4_T26_V2_FIELD(OrbisVideodec2InputData, auSize, 16, 8);
X4_T26_V2_FIELD(OrbisVideodec2InputData, ptsData, 24, 8);
X4_T26_V2_FIELD(OrbisVideodec2InputData, dtsData, 32, 8);
X4_T26_V2_FIELD(OrbisVideodec2InputData, attachedData, 40, 8);

/* Padding at 11 permits codecType at 12; padding at 28..31 permits the
 * returned framebuffer pointer at 32. This is the complete 48-byte form. */
X4_T26_V2_STRUCT(OrbisVideodec2OutputInfo, 48);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, isValid, 8, 1);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, isErrorFrame, 9, 1);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, pictureCount, 10, 1);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, codecType, 12, 4);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, frameWidth, 16, 4);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, framePitch, 20, 4);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, frameHeight, 24, 4);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, pFrameBuffer, 32, 8);
X4_T26_V2_FIELD(OrbisVideodec2OutputInfo, frameBufferSize, 40, 8);

/* The acceptance byte at 24 is followed by seven bytes of tail padding. */
X4_T26_V2_STRUCT(OrbisVideodec2FrameBuffer, 32);
X4_T26_V2_FIELD(OrbisVideodec2FrameBuffer, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2FrameBuffer, pFrameBuffer, 8, 8);
X4_T26_V2_FIELD(OrbisVideodec2FrameBuffer, frameBufferSize, 16, 8);
X4_T26_V2_FIELD(OrbisVideodec2FrameBuffer, isAccepted, 24, 1);

X4_T26_V2_STRUCT(OrbisVideodec2ComputeMemoryInfo, 24);
X4_T26_V2_FIELD(OrbisVideodec2ComputeMemoryInfo, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2ComputeMemoryInfo, cpuGpuMemorySize, 8, 8);
X4_T26_V2_FIELD(OrbisVideodec2ComputeMemoryInfo, pCpuGpuMemory, 16, 8);

X4_T26_V2_STRUCT(OrbisVideodec2ComputeConfigInfo, 16);
X4_T26_V2_FIELD(OrbisVideodec2ComputeConfigInfo, thisSize, 0, 8);
X4_T26_V2_FIELD(OrbisVideodec2ComputeConfigInfo, computePipeId, 8, 2);
X4_T26_V2_FIELD(OrbisVideodec2ComputeConfigInfo, computeQueueId, 10, 2);
X4_T26_V2_FIELD(OrbisVideodec2ComputeConfigInfo, checkMemoryType, 12, 1);
X4_T26_V2_FIELD(OrbisVideodec2ComputeConfigInfo, reserved0, 13, 1);
X4_T26_V2_FIELD(OrbisVideodec2ComputeConfigInfo, reserved1, 14, 2);

#undef X4_T26_V2_FIELD
#undef X4_T26_V2_STRUCT
#define X4_T26_V2_ABI_CHECKED 1
#endif
