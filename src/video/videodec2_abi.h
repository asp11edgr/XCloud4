/* SPDX-License-Identifier: GPL-3.0-only */
/* ABI types adapted from OpenOrbis PR #213, Backporter, commit
 * 9b9e82a2ec4e8cd3c34a086ca82339032cf69da0. See THIRD_PARTY_NOTICES.md. */
#include <stddef.h>
#include <stdbool.h>
#pragma once
#include <stdint.h>

#define ORBIS_VIDEODEC2_RESOURCE_TYPE_COMPUTE (1)
#define ORBIS_VIDEODEC2_TIMESTAMP_INVALID  (0xffffffffffffffffULL)
#define ORBIS_VIDEODEC2_AUTO_FRAME_SETTING (-1)
#define ORBIS_VIDEODEC2_INHERIT_AFFINITY_MASK (0)
#define ORBIS_VIDEODEC2_INHERIT_THREAD_PRIORITY (-1)

typedef struct OrbisVideodec2DecoderConfigInfo 
{
  size_t    thisSize; // = sizeof(OrbisVideodec2DecoderConfigInfo)
  uint32_t  resourceType;
  uint32_t  codecType;
  uint32_t  profile;
  uint32_t  maxLevel;
  int32_t   maxFrameWidth;
  int32_t   maxFrameHeight;
  int32_t   maxDpbFrameCount;
  uint32_t  decodePipelineDepth;
  void*     computeQueue;
  uint64_t  cpuAffinityMask;
  int32_t   cpuThreadPriority;
  bool      optimizeProgressiveVideo;
  bool      checkMemoryType;
  uint8_t   reserved0;
  uint8_t   reserved1;
  void*     extraConfigInfo;
} OrbisVideodec2DecoderConfigInfo;

typedef struct OrbisVideodec2DecoderMemoryInfo 
{
  size_t    thisSize; // = sizeof(OrbisVideodec2DecoderMemoryInfo)
  size_t    cpuMemorySize;
  void*     pCpuMemory;
  size_t    gpuMemorySize;
  void*     pGpuMemory;
  size_t    cpuGpuMemorySize;
  void*     pCpuGpuMemory;
  size_t    maxFrameBufferSize;
  uint32_t  frameBufferAlignment;
  uint32_t  reserved0;
} OrbisVideodec2DecoderMemoryInfo;


typedef struct OrbisVideodec2InputData 
{
  size_t    thisSize; // = sizeof(OrbisVideodec2InputData)
  void*     pAuData;
  size_t    auSize;
  uint64_t  ptsData;
  uint64_t  dtsData;
  uint64_t  attachedData;
} OrbisVideodec2InputData;

typedef struct OrbisVideodec2OutputInfo 
{
  size_t    thisSize; // = sizeof(OrbisVideodec2OutputInfo)
  bool      isValid;
  bool      isErrorFrame;
  uint8_t   pictureCount;
  uint32_t  codecType;
  uint32_t  frameWidth;
  uint32_t  framePitch;
  uint32_t  frameHeight;
  void*     pFrameBuffer;
  size_t    frameBufferSize;
} OrbisVideodec2OutputInfo;

typedef struct OrbisVideodec2FrameBuffer 
{
  size_t    thisSize; // = sizeof(OrbisVideodec2FrameBuffer)
  void*     pFrameBuffer;
  size_t    frameBufferSize;
  bool      isAccepted;
} OrbisVideodec2FrameBuffer;

typedef struct OrbisVideodec2ComputeMemoryInfo 
{
  size_t thisSize; // = sizeof(OrbisVideodec2ComputeMemoryInfo)
  size_t cpuGpuMemorySize;
  void*  pCpuGpuMemory;
} OrbisVideodec2ComputeMemoryInfo;

typedef struct OrbisVideodec2ComputeConfigInfo 
{
  size_t    thisSize; // = sizeof(OrbisVideodec2ComputeConfigInfo)
  uint16_t  computePipeId;
  uint16_t  computeQueueId;
  bool      checkMemoryType;
  uint8_t   reserved0;
  uint16_t  reserved1;
} OrbisVideodec2ComputeConfigInfo;
_Static_assert(sizeof(OrbisVideodec2DecoderConfigInfo) == 72, "Videodec2 config ABI" );
_Static_assert(sizeof(OrbisVideodec2DecoderMemoryInfo) == 72, "Videodec2 memory ABI" );
_Static_assert(sizeof(OrbisVideodec2OutputInfo) == 48, "Videodec2 output ABI" );

_Static_assert(sizeof(OrbisVideodec2InputData) == 48, "Videodec2 input ABI" );
_Static_assert(sizeof(OrbisVideodec2FrameBuffer) == 32, "Videodec2 framebuffer ABI" );
_Static_assert(sizeof(OrbisVideodec2ComputeMemoryInfo) == 24, "Videodec2 compute memory ABI" );
_Static_assert(sizeof(OrbisVideodec2ComputeConfigInfo) == 16, "Videodec2 compute config ABI" );
