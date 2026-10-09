/* SPDX-License-Identifier: GPL-3.0-only */
#include "h264_demo.h"
#include "videodec2_abi.h"
#include "../core/module.h"
#include <orbis/libkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RING = 4, MAX_AU = 300, MAX_CLIP = 4 * 1024 * 1024, MAX_MEMORY = 64 * 1024 * 1024 };
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
    Memory compute, cpu, gpu, shared, clip, output[RING];
    size_t boundaries[MAX_AU + 1], clip_size, frame_size;
    unsigned next, slot, flushes;
    uint64_t deadline;
} VideoState;

static int stage(X4DemoVideo *v, const char *name, int rc)
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

static void release(Memory *m)
{
    if (m->p) { sceKernelMunmap(m->p, m->size); m->p = NULL; }
    if (m->allocated) { sceKernelReleaseDirectMemory(m->offset, m->size); m->allocated = 0; }
}

static int read_clip(X4DemoVideo *v, VideoState *s)
{
    FILE *f = fopen("/app0/assets/sample.h264", "rb");
    if (!f) return -3;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -3; }
    long length = ftell(f);
    if (length <= 0 || length > MAX_CLIP || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -3; }
    int rc = allocate(&s->clip, (size_t)length, 0, 0x4000);
    if (rc < 0) { fclose(f); return rc; }
    size_t got = fread(s->clip.p, 1, (size_t)length, f);
    fclose(f);
    if (got != (size_t)length) return -3;
    s->clip_size = got;
    const uint8_t *bytes = s->clip.p;
    unsigned auds = 0;
    /* This fixed, locally generated clip uses one AUD per access unit. */
    for (size_t i = 0; i + 4 < got;) {
        size_t prefix = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0) {
            if (bytes[i + 2] == 1) prefix = 3;
            else if (bytes[i + 2] == 0 && bytes[i + 3] == 1) prefix = 4;
        }
        if (prefix) {
            if ((bytes[i + prefix] & 31) == 9) {
                if (auds >= MAX_AU) return -4;
                s->boundaries[auds] = auds == 0 ? 0 : i;
                ++auds;
            }
            i += prefix + 1;
        } else ++i;
    }
    if (!auds) return -4;
    s->boundaries[auds] = got;
    v->total = auds;
    return 0;
}

int x4_video_stop(X4DemoVideo *v)
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
    for (unsigned i = 0; i < RING; ++i) release(&s->output[i]);
    release(&s->clip); release(&s->shared); release(&s->gpu);
    release(&s->cpu); release(&s->compute);
    free(v->pixels); v->pixels = NULL;
    free(s); v->state = NULL;
    return 0;
}

int x4_video_start(X4DemoVideo *v)
{
    if (v->state) { int rc = x4_video_stop(v); if (rc < 0) return rc; }
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
    rc = read_clip(v, s);
    if (stage(v, "ARCHIVO H264", rc) < 0) goto fail;
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
        .profile = 66, .maxLevel = 30, .maxFrameWidth = X4_SAMPLE_WIDTH,
        .maxFrameHeight = X4_SAMPLE_HEIGHT, .maxDpbFrameCount = 4,
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
    v->pixels = calloc(X4_SAMPLE_WIDTH * X4_SAMPLE_HEIGHT, sizeof(*v->pixels));
    if (!v->pixels) { rc = stage(v, "IMAGEN RGB", -5); goto fail; }
    rc = s->CreateDecoder(&config, &memory, &s->decoder);
    if (stage(v, "CREAR DECODER", rc) < 0) goto fail;
    s->deadline = sceKernelGetProcessTime();
    stage(v, "REPRODUCIENDO", 0);
    return 0;
fail:
    /* Keep the original failure label unless cleanup itself fails. */
    x4_video_stop(v);
    return rc;
}

static unsigned clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned)v; }

static int picture(X4DemoVideo *v, VideoState *s, const OrbisVideodec2OutputInfo *o)
{
    if (!o->isValid) return 0;
    if (o->isErrorFrame || o->codecType != 1 || o->frameWidth != X4_SAMPLE_WIDTH ||
        o->frameHeight != X4_SAMPLE_HEIGHT || o->framePitch < X4_SAMPLE_WIDTH || o->framePitch > 8192) return -7;
    size_t length = (size_t)o->framePitch * o->frameHeight * 3 / 2;
    if (length > o->frameBufferSize) return -7;
    int owned = 0;
    for (unsigned i = 0; i < RING; ++i)
        if (o->pFrameBuffer == s->output[i].p && length <= s->output[i].size) owned = 1;
    if (!owned) return -7;
    const uint8_t *yplane = o->pFrameBuffer;
    const uint8_t *uvplane = yplane + (size_t)o->framePitch * o->frameHeight;
    for (unsigned y = 0; y < X4_SAMPLE_HEIGHT; ++y) {
        for (unsigned x = 0; x < X4_SAMPLE_WIDTH; ++x) {
            int luma = yplane[y * o->framePitch + x] - 16;
            if (luma < 0) luma = 0;
            size_t uv = (y / 2) * o->framePitch + (x & ~1u);
            int u = uvplane[uv] - 128, w = uvplane[uv + 1] - 128;
            unsigned r = clamp((298 * luma + 409 * w + 128) >> 8);
            unsigned g = clamp((298 * luma - 100 * u - 208 * w + 128) >> 8);
            unsigned b = clamp((298 * luma + 516 * u + 128) >> 8);
            v->pixels[y * X4_SAMPLE_WIDTH + x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }
    if (++v->frames == 1) printf("XCloud4: primera imagen H264 %ux%u pitch=%u\n", o->frameWidth, o->frameHeight, o->framePitch);
    return 0;
}

void x4_video_tick(X4DemoVideo *v)
{
    VideoState *s = v->state;
    if (!s || !s->decoder || v->error || v->complete) return;
    uint64_t now = sceKernelGetProcessTime();
    if (now < s->deadline) return;
    s->deadline += 33333;
    Memory *m = &s->output[s->slot];
    OrbisVideodec2FrameBuffer frame = {.thisSize = sizeof(frame), .pFrameBuffer = m->p, .frameBufferSize = s->frame_size};
    OrbisVideodec2OutputInfo output = {.thisSize = sizeof(output)};
    int rc;
    if (s->next < v->total) {
        size_t start = s->boundaries[s->next];
        OrbisVideodec2InputData input = {.thisSize = sizeof(input),
            .pAuData = (uint8_t *)s->clip.p + start,
            .auSize = s->boundaries[s->next + 1] - start,
            .ptsData = s->next * 33333ULL, .dtsData = s->next * 33333ULL};
        rc = s->Decode(s->decoder, &input, &frame, &output);
        ++s->next;
    } else {
        rc = s->Flush(s->decoder, &frame, &output);
        ++s->flushes;
    }
    if (rc < 0) { stage(v, "DECODIFICAR H264", rc); return; }
    if (frame.isAccepted) s->slot = (s->slot + 1) % RING;
    rc = picture(v, s, &output);
    if (rc < 0) { stage(v, "FORMATO DE IMAGEN", rc); return; }
    if (s->next == v->total && s->flushes && (!output.isValid || s->flushes >= RING)) {
        v->complete = 1;
        if (!v->frames) stage(v, "SIN IMAGEN DECODIFICADA", -8);
        else stage(v, "MUESTRA TERMINADA", 0);
    }
}
