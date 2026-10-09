/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_h264.h"
#include "videodec2_abi.h"
#include "../core/module.h"
#include <orbis/libkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RING = 4, INPUT_RING = 2, MAX_MEMORY = 64 * 1024 * 1024 };
typedef struct { void *p; off_t offset; size_t size; int allocated; } Memory;
typedef struct {
    int32_t (*QueryComputeMemoryInfo)(OrbisVideodec2ComputeMemoryInfo *);
    int32_t (*AllocateComputeQueue)(const OrbisVideodec2ComputeConfigInfo *, const OrbisVideodec2ComputeMemoryInfo *, void **);
    int32_t (*ReleaseComputeQueue)(void *);
    int32_t (*QueryDecoderMemoryInfo)(const OrbisVideodec2DecoderConfigInfo *, OrbisVideodec2DecoderMemoryInfo *);
    int32_t (*CreateDecoder)(const OrbisVideodec2DecoderConfigInfo *, const OrbisVideodec2DecoderMemoryInfo *, void **);
    int32_t (*DeleteDecoder)(void *);
    int32_t (*Decode)(void *, const OrbisVideodec2InputData *, OrbisVideodec2FrameBuffer *, OrbisVideodec2OutputInfo *);
    int32_t (*Flush)(void *, OrbisVideodec2FrameBuffer *, OrbisVideodec2OutputInfo *);
    void *queue, *decoder;
    Memory compute, cpu, gpu, shared, input[INPUT_RING], output[RING];
    size_t frame_size;
    unsigned input_slot, slot;
} VideoState;

static int stage(X4LiveVideo *v, const char *name, int rc)
{
    snprintf(v->stage, sizeof(v->stage), "%s", name);
    if (rc < 0) v->error = rc;
    printf("XCloud4: video %s -> 0x%08x\n", name, (unsigned)rc);
    return rc;
}

static int allocate(Memory *m, size_t size, int type, size_t align)
{
    if (!size) return 0;
    if (size > MAX_MEMORY || align > 0x200000 || (align && (align & (align - 1)))) return -2;
    if (align < 0x4000) align = 0x4000;
    m->size = (size + align - 1) & ~(align - 1);
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), m->size, align, type, &m->offset);
    if (rc < 0) return rc;
    m->allocated = 1;
    rc = sceKernelMapDirectMemory(&m->p, m->size, 0x33, 0, m->offset, align);
    if (rc >= 0) memset(m->p, 0, m->size);
    else m->p = NULL;
    return rc;
}

static int release(Memory *m)
{
    if (m->p) {
        int rc = sceKernelMunmap(m->p, m->size);
        if (rc < 0) return rc;
        m->p = NULL;
    }
    if (m->allocated) {
        int rc = sceKernelReleaseDirectMemory(m->offset, m->size);
        if (rc < 0) return rc;
        m->allocated = 0;
    }
    return 0;
}

int x4_live_video_stop(X4LiveVideo *v)
{
    VideoState *s = v->state;
    if (!s) return 0;
    /* Do not free buffers still owned by the decoder if teardown fails. */
    if (s->decoder) {
        int rc = s->DeleteDecoder(s->decoder);
        if (rc < 0) return stage(v, "CERRAR DECODER", rc);
        s->decoder = NULL;
    }
    if (s->queue) {
        int rc = s->ReleaseComputeQueue(s->queue);
        if (rc < 0) return stage(v, "CERRAR COLA", rc);
        s->queue = NULL;
    }
    /* Keep the context and any unreleased reservations on native failure,
     * including after a successful unmap but failed direct-memory release. */
    for (unsigned i = 0; i < RING; ++i) {
        int rc = release(&s->output[i]); if (rc < 0) return stage(v, "LIBERAR IMAGEN", rc);
    }
    for (unsigned i = 0; i < INPUT_RING; ++i) {
        int rc = release(&s->input[i]); if (rc < 0) return stage(v, "LIBERAR ENTRADA", rc);
    }
    Memory *remaining[] = {&s->shared, &s->gpu, &s->cpu, &s->compute};
    for (unsigned i = 0; i < sizeof(remaining) / sizeof(*remaining); ++i) {
        int rc = release(remaining[i]); if (rc < 0) return stage(v, "LIBERAR MEMORIA", rc);
    }
    free(v->pixels); v->pixels = NULL;
    free(s); v->state = NULL;
    return 0;
}

int x4_live_video_start(X4LiveVideo *v)
{
    if (v->state) { int rc = x4_live_video_stop(v); if (rc < 0) return rc; }
    memset(v, 0, sizeof(*v));
    VideoState *s = calloc(1, sizeof(*s));
    if (!s) return stage(v, "MEMORIA", -5);
    v->state = s;
    int handle = x4_module_open("libSceVideodec2"), rc = handle;
    if (stage(v, "MODULO H264", rc) < 0) goto fail;
#define SYMBOL(field) do { rc = x4_module_symbol(handle, "sceVideodec2" #field, (void **)&s->field); if (stage(v, #field, rc) < 0) goto fail; } while (0)
    SYMBOL(QueryComputeMemoryInfo); SYMBOL(AllocateComputeQueue); SYMBOL(ReleaseComputeQueue);
    SYMBOL(QueryDecoderMemoryInfo); SYMBOL(CreateDecoder); SYMBOL(DeleteDecoder); SYMBOL(Decode); SYMBOL(Flush);
#undef SYMBOL
    /* The configured decode pipeline depth is one. Rotate two persistent
     * ONION AU buffers so the immediately previous submission is untouched.
     * Keep every input reservation until decoder deletion succeeds. */
    for (unsigned i = 0; i < INPUT_RING; ++i) {
        rc = allocate(&s->input[i], X4_LIVE_AU_MAX, 0, 0x4000);
        if (stage(v, "ENTRADA H264", rc) < 0) goto fail;
    }
    OrbisVideodec2ComputeMemoryInfo cm = {.thisSize = sizeof(cm)};
    rc = s->QueryComputeMemoryInfo(&cm);
    if (stage(v, "MEMORIA COLA", rc) < 0) goto fail;
    rc = allocate(&s->compute, cm.cpuGpuMemorySize, 0, 0x4000);
    if (stage(v, "RESERVAR COLA", rc) < 0) goto fail;
    cm.pCpuGpuMemory = s->compute.p;
    OrbisVideodec2ComputeConfigInfo cc = {.thisSize = sizeof(cc), .computePipeId = 0, .computeQueueId = 0};
    rc = s->AllocateComputeQueue(&cc, &cm, &s->queue);
    if (stage(v, "CREAR COLA", rc) < 0) goto fail;
    OrbisVideodec2DecoderConfigInfo config = {
        .thisSize = sizeof(config), .resourceType = 1, .codecType = 1,
        .profile = 66, .maxLevel = 31, .maxFrameWidth = X4_LIVE_WIDTH,
        .maxFrameHeight = X4_LIVE_HEIGHT, .maxDpbFrameCount = 4,
        .decodePipelineDepth = 1, .computeQueue = s->queue,
        .cpuAffinityMask = 0x3f, .cpuThreadPriority = 700,
        .optimizeProgressiveVideo = true, .checkMemoryType = false
    };
    OrbisVideodec2DecoderMemoryInfo memory = {.thisSize = sizeof(memory)};
    rc = s->QueryDecoderMemoryInfo(&config, &memory);
    if (stage(v, "MEMORIA DECODER", rc) < 0) goto fail;
    printf("XCloud4: memoria video CPU=%zu GPU=%zu SHARED=%zu FRAME=%zu ALIGN=%u\n",
        memory.cpuMemorySize, memory.gpuMemorySize, memory.cpuGpuMemorySize,
        memory.maxFrameBufferSize, memory.frameBufferAlignment);
    if (memory.cpuMemorySize > MAX_MEMORY || memory.gpuMemorySize > MAX_MEMORY ||
        memory.cpuGpuMemorySize > MAX_MEMORY || !memory.maxFrameBufferSize || memory.maxFrameBufferSize > MAX_MEMORY / RING) {
        rc = stage(v, "LIMITES MEMORIA", -6); goto fail;
    }
    rc = allocate(&s->cpu, memory.cpuMemorySize, 0, 0x4000);
    if (stage(v, "RESERVAR CPU", rc) < 0) goto fail;
    rc = allocate(&s->gpu, memory.gpuMemorySize, 3, 0x4000);
    if (stage(v, "RESERVAR GPU", rc) < 0) goto fail;
    rc = allocate(&s->shared, memory.cpuGpuMemorySize, 0, 0x4000);
    if (stage(v, "RESERVAR COMPARTIDA", rc) < 0) goto fail;
    memory.pCpuMemory = s->cpu.p; memory.pGpuMemory = s->gpu.p; memory.pCpuGpuMemory = s->shared.p;
    s->frame_size = memory.maxFrameBufferSize;
    for (unsigned i = 0; i < RING; ++i) {
        rc = allocate(&s->output[i], s->frame_size, 3, memory.frameBufferAlignment);
        if (stage(v, "RESERVAR IMAGEN", rc) < 0) goto fail;
    }
    v->pixels = calloc(X4_LIVE_WIDTH * X4_LIVE_HEIGHT, sizeof(*v->pixels));
    if (!v->pixels) { rc = stage(v, "IMAGEN RGB", -5); goto fail; }
    rc = s->CreateDecoder(&config, &memory, &s->decoder);
    if (stage(v, "CREAR DECODER", rc) < 0) goto fail;

    stage(v, "REPRODUCIENDO", 0);
    return 0;
fail:
    /* Keep the original failure label unless cleanup itself fails. */
    x4_live_video_stop(v);
    return rc;
}

static unsigned clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned)v; }

static int picture(X4LiveVideo *v, VideoState *s, const OrbisVideodec2OutputInfo *o)
{
    if (o->isErrorFrame) return -7;
    if (!o->isValid) return 0;
    if (o->codecType != 1 || !o->frameWidth || o->frameWidth > X4_LIVE_WIDTH || (o->frameWidth & 1) ||
        !o->frameHeight || o->frameHeight > X4_LIVE_HEIGHT || (o->frameHeight & 1) || o->framePitch < o->frameWidth || o->framePitch > 8192) return -7;
    size_t length = (size_t)o->framePitch * o->frameHeight * 3 / 2;
    if (length > o->frameBufferSize) return -7;
    int owned = 0;
    for (unsigned i = 0; i < RING; ++i)
        if (o->pFrameBuffer == s->output[i].p && length <= s->output[i].size) owned = 1;
    if (!owned) return -7;
    const uint8_t *yplane = o->pFrameBuffer;
    const uint8_t *uvplane = yplane + (size_t)o->framePitch * o->frameHeight;
    for (unsigned y = 0; y < o->frameHeight; ++y) {
        for (unsigned x = 0; x < o->frameWidth; ++x) {
            int luma = yplane[y * o->framePitch + x] - 16;
            if (luma < 0) luma = 0;
            size_t uv = (y / 2) * o->framePitch + (x & ~1u);
            int u = uvplane[uv] - 128, w = uvplane[uv + 1] - 128;
            unsigned r = clamp((298 * luma + 409 * w + 128) >> 8);
            unsigned g = clamp((298 * luma - 100 * u - 208 * w + 128) >> 8);
            unsigned b = clamp((298 * luma + 516 * u + 128) >> 8);
            v->pixels[y * o->frameWidth + x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }
    v->width = o->frameWidth; v->height = o->frameHeight;
    if (++v->frames == 1) printf("XCloud4: primera imagen H264 %ux%u pitch=%u\n", o->frameWidth, o->frameHeight, o->framePitch);
    return 0;
}


int x4_live_video_feed(X4LiveVideo *v, const uint8_t *bytes, size_t size, uint64_t pts)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->decoder || !bytes || !size || size > X4_LIVE_AU_MAX || v->error) return -7;
    Memory *compressed = &s->input[s->input_slot];
    memcpy(compressed->p, bytes, size);
    Memory *m = &s->output[s->slot];
    OrbisVideodec2FrameBuffer frame = {.thisSize = sizeof(frame), .pFrameBuffer = m->p, .frameBufferSize = s->frame_size};
    OrbisVideodec2OutputInfo output = {.thisSize = sizeof(output)};
    OrbisVideodec2InputData input = {.thisSize = sizeof(input), .pAuData = compressed->p,
        .auSize = size, .ptsData = pts, .dtsData = pts};
    int rc = s->Decode(s->decoder, &input, &frame, &output);
    if (rc < 0) return stage(v, "DECODIFICAR JUEGO H264", rc);
    s->input_slot = (s->input_slot + 1) % INPUT_RING;
    if (frame.isAccepted) s->slot = (s->slot + 1) % RING;
    rc = picture(v, s, &output);
    if (rc < 0) return stage(v, "FORMATO DE IMAGEN", rc);
    return 0;
}
