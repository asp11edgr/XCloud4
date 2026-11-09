/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only declarations/constants: never an SDK/PS4 ABI definition. */
#pragma once
#include <stdint.h>
enum { ORBIS_VIDEO_USER_MAIN=0, ORBIS_VIDEO_OUT_BUS_MAIN=0,
    ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB=1, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR=0,
    ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9=0, ORBIS_VIDEO_OUT_FLIP_60HZ=0,
    ORBIS_VIDEO_OUT_FLIP_VSYNC=1, ORBIS_VIDEO_OUT_FLIP_HSYNC=2 };
typedef struct { unsigned width,height,pitch; } OrbisVideoOutBufferAttribute;
typedef struct { uint64_t num; int64_t flipArg; uint64_t ptime; int numFlipPending,numGpuFlipPending; } OrbisVideoOutFlipStatus;
int sceVideoOutOpen(int,int,int,const void *);
int sceVideoOutClose(int);
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *,int,int,int,unsigned,unsigned,unsigned);
int sceVideoOutRegisterBuffers(int,int,void *const *,int,const OrbisVideoOutBufferAttribute *);
int sceVideoOutSetFlipRate(int,int);
int sceVideoOutSubmitFlip(int,int,int,int64_t);
int sceVideoOutGetFlipStatus(int,OrbisVideoOutFlipStatus *);
