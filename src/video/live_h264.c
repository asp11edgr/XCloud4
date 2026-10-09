/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_h264.h"
#include "videodec2_abi.h"
#include "../core/module.h"
#include <orbis/libkernel.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>
#include <emmintrin.h>
#include <smmintrin.h>

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
    OrbisVideodec2OutputInfo pending;
    bool pending_valid;
    uint8_t *staging;
    size_t staging_size;
    bool streaming_copy;
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
    free(s->staging); s->staging = NULL;
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
    /* Keep the native decoder's WC output type. CPU color conversion reads
     * one persistent cached heap copy bounded by the validated output size. */
    s->staging_size = s->frame_size;
    s->staging = malloc(s->staging_size);
    if (!s->staging) { rc = stage(v, "IMAGEN NV12 CPU", -5); goto fail; }
    unsigned eax, ebx, ecx, edx;
    s->streaming_copy = __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 19));
    printf("XCloud4: video NV12 copy mode=%u capacity=%zu source_alignment=16\n",
        s->streaming_copy ? 1u : 0u, s->staging_size);
    v->pixels = calloc(X4_LIVE_WIDTH * X4_LIVE_HEIGHT, sizeof(*v->pixels));
    if (!v->pixels) { rc = stage(v, "IMAGEN RGB", -5); goto fail; }
    printf("XCloud4: video RGB convert mode=2 block_pixels=8 scalar_tail_max=7\n");
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

/* Original SSE2 conversion using Intel's documented PMADDWD, PSRAD and
 * saturation contracts. Keep the scalar BT.601 coefficients and rounding:
 * products/sums use signed 32 bits; after >>8 every result lies within
 * [-258,534], so the signed 16-bit pack cannot alter it. The unsigned byte
 * pack then implements clamp(0,255). No external converter is copied. */
__attribute__((target("sse2"), noinline))
static void convert_nv12_sse2(uint32_t *pixels, const uint8_t *nv12,
    unsigned width, unsigned height, unsigned pitch)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i ybias = _mm_set1_epi16(16);
    const __m128i uvbias = _mm_set1_epi16(128);
    const __m128i rounding = _mm_set1_epi32(128);
    const __m128i rcoeff = _mm_setr_epi16(298,409,298,409,298,409,298,409);
    const __m128i bcoeff = _mm_setr_epi16(298,516,298,516,298,516,298,516);
    const __m128i gcoeff = _mm_setr_epi16(-100,-208,-100,-208,-100,-208,-100,-208);
    const __m128i ycoeff = _mm_set1_epi32(298);
    const __m128i alpha = _mm_set1_epi8((char)-1);
    const uint8_t *uvplane = nv12 + (size_t)pitch * height;
    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *yrow = nv12 + (size_t)y * pitch;
        const uint8_t *uvrow = uvplane + (size_t)(y / 2) * pitch;
        uint32_t *out = pixels + (size_t)y * width;
        unsigned x = 0;
        /* MOVQ loads exactly eight bytes, including on odd-pitch rows.
         * Stores write exactly eight RGBA pixels; no row padding is read. */
        for (; width - x >= 8; x += 8) {
            __m128i luma = _mm_subs_epu16(
                _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(yrow + x)), zero), ybias);
            __m128i uv = _mm_sub_epi16(
                _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(uvrow + x)), zero), uvbias);
            /* [U0,V0,U1,V1,U2,V2,U3,V3] becomes each chroma value twice,
             * matching NV12's two adjacent pixels per U/V pair. */
            __m128i u = _mm_shufflehi_epi16(
                _mm_shufflelo_epi16(uv, _MM_SHUFFLE(2,2,0,0)), _MM_SHUFFLE(2,2,0,0));
            __m128i w = _mm_shufflehi_epi16(
                _mm_shufflelo_epi16(uv, _MM_SHUFFLE(3,3,1,1)), _MM_SHUFFLE(3,3,1,1));
            __m128i rlo = _mm_srai_epi32(_mm_add_epi32(rounding,
                _mm_madd_epi16(_mm_unpacklo_epi16(luma, w), rcoeff)), 8);
            __m128i rhi = _mm_srai_epi32(_mm_add_epi32(rounding,
                _mm_madd_epi16(_mm_unpackhi_epi16(luma, w), rcoeff)), 8);
            __m128i blo = _mm_srai_epi32(_mm_add_epi32(rounding,
                _mm_madd_epi16(_mm_unpacklo_epi16(luma, u), bcoeff)), 8);
            __m128i bhi = _mm_srai_epi32(_mm_add_epi32(rounding,
                _mm_madd_epi16(_mm_unpackhi_epi16(luma, u), bcoeff)), 8);
            __m128i glo = _mm_srai_epi32(_mm_add_epi32(rounding, _mm_add_epi32(
                _mm_madd_epi16(_mm_unpacklo_epi16(luma, zero), ycoeff),
                _mm_madd_epi16(_mm_unpacklo_epi16(u, w), gcoeff))), 8);
            __m128i ghi = _mm_srai_epi32(_mm_add_epi32(rounding, _mm_add_epi32(
                _mm_madd_epi16(_mm_unpackhi_epi16(luma, zero), ycoeff),
                _mm_madd_epi16(_mm_unpackhi_epi16(u, w), gcoeff))), 8);
            __m128i r = _mm_packus_epi16(_mm_packs_epi32(rlo, rhi), zero);
            __m128i g = _mm_packus_epi16(_mm_packs_epi32(glo, ghi), zero);
            __m128i b = _mm_packus_epi16(_mm_packs_epi32(blo, bhi), zero);
            __m128i rg = _mm_unpacklo_epi8(r, g);
            __m128i ba = _mm_unpacklo_epi8(b, alpha);
            _mm_storeu_si128((__m128i *)(out + x), _mm_unpacklo_epi16(rg, ba));
            _mm_storeu_si128((__m128i *)(out + x + 4), _mm_unpackhi_epi16(rg, ba));
        }
        /* Bounded scalar fallback for a short row or its final 0..7 pixels. */
        for (; x < width; ++x) {
            int luma = yrow[x] - 16;
            if (luma < 0) luma = 0;
            size_t uv = x & ~1u;
            int u = uvrow[uv] - 128, w = uvrow[uv + 1] - 128;
            unsigned r = clamp((298 * luma + 409 * w + 128) >> 8);
            unsigned g = clamp((298 * luma - 100 * u - 208 * w + 128) >> 8);
            unsigned b = clamp((298 * luma + 516 * u + 128) >> 8);
            out[x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }
}

/* Original fixed-block copy based on the instruction contracts in Intel's
 * SSE4 programming reference, section 2.2.3. No external project helper is
 * copied. Owned GPU output bases are 16-byte aligned; heap stores may be
 * unaligned. Every vector load stays inside the validated NV12 region. */
__attribute__((noinline))
static void copy_nv12_sse2(uint8_t *destination, const uint8_t *source, size_t length)
{
    size_t at = 0;
    for (; length - at >= 64; at += 64) {
        __m128i a = _mm_load_si128((const __m128i *)(source + at));
        __m128i b = _mm_load_si128((const __m128i *)(source + at + 16));
        __m128i c = _mm_load_si128((const __m128i *)(source + at + 32));
        __m128i d = _mm_load_si128((const __m128i *)(source + at + 48));
        _mm_storeu_si128((__m128i *)(destination + at), a);
        _mm_storeu_si128((__m128i *)(destination + at + 16), b);
        _mm_storeu_si128((__m128i *)(destination + at + 32), c);
        _mm_storeu_si128((__m128i *)(destination + at + 48), d);
    }
    for (; length - at >= 16; at += 16)
        _mm_storeu_si128((__m128i *)(destination + at), _mm_load_si128((const __m128i *)(source + at)));
    for (; at < length; ++at) destination[at] = source[at];
}
__attribute__((target("sse4.1"), noinline))
static void copy_nv12_sse41(uint8_t *destination, const uint8_t *source, size_t length)
{
    size_t at = 0;
    for (; length - at >= 64; at += 64) {
        __m128i a = _mm_stream_load_si128((__m128i *)(uintptr_t)(source + at));
        __m128i b = _mm_stream_load_si128((__m128i *)(uintptr_t)(source + at + 16));
        __m128i c = _mm_stream_load_si128((__m128i *)(uintptr_t)(source + at + 32));
        __m128i d = _mm_stream_load_si128((__m128i *)(uintptr_t)(source + at + 48));
        _mm_storeu_si128((__m128i *)(destination + at), a);
        _mm_storeu_si128((__m128i *)(destination + at + 16), b);
        _mm_storeu_si128((__m128i *)(destination + at + 32), c);
        _mm_storeu_si128((__m128i *)(destination + at + 48), d);
    }
    for (; length - at >= 16; at += 16)
        _mm_storeu_si128((__m128i *)(destination + at),
            _mm_stream_load_si128((__m128i *)(uintptr_t)(source + at)));
    for (; at < length; ++at) destination[at] = source[at];
}

static int validate_picture(VideoState *s, const OrbisVideodec2OutputInfo *o)
{
    if (o->isErrorFrame) return -7;
    if (!o->isValid) return 0;
    if (o->codecType != 1 || !o->frameWidth || o->frameWidth > X4_LIVE_WIDTH || (o->frameWidth & 1) ||
        !o->frameHeight || o->frameHeight > X4_LIVE_HEIGHT || (o->frameHeight & 1) || o->framePitch < o->frameWidth || o->framePitch > 8192) return -7;
    size_t length = (size_t)o->framePitch * o->frameHeight * 3 / 2;
    if (length > o->frameBufferSize || length > s->staging_size || !s->staging || ((uintptr_t)o->pFrameBuffer & 15)) return -7;
    int owned = 0;
    for (unsigned i = 0; i < RING; ++i)
        if (o->pFrameBuffer == s->output[i].p && length <= s->output[i].size) owned = 1;
    if (!owned) return -7;
    return 1;
}

int x4_live_video_convert_pending(X4LiveVideo *v)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->pending_valid || v->error) return 0;
    const OrbisVideodec2OutputInfo *o = &s->pending;
    size_t length = (size_t)o->framePitch * o->frameHeight * 3 / 2;
    uint64_t copy_begin = sceKernelGetProcessTime();
    /* Decode has reported a valid owned picture. MFENCE orders WC loads
     * around the independent burst copy before reuse of its native output. */
    _mm_mfence();
    if (s->streaming_copy) copy_nv12_sse41(s->staging, o->pFrameBuffer, length);
    else copy_nv12_sse2(s->staging, o->pFrameBuffer, length);
    _mm_mfence();
    uint64_t copy_elapsed = sceKernelGetProcessTime() - copy_begin;
    ++v->copy_calls; v->copy_bytes += length; v->copy_us += copy_elapsed;
    if (copy_elapsed > v->copy_max_us) v->copy_max_us = copy_elapsed;
    uint64_t begin = sceKernelGetProcessTime();
    convert_nv12_sse2(v->pixels, s->staging, o->frameWidth, o->frameHeight, o->framePitch);
    s->pending_valid = false;
    v->width = o->frameWidth; v->height = o->frameHeight;
    uint64_t completed = sceKernelGetProcessTime(), elapsed = completed - begin;
    v->convert_us += elapsed;
    if (elapsed > v->convert_max_us) v->convert_max_us = elapsed;
    if (v->last_picture_time_us && completed - v->last_picture_time_us > v->picture_gap_max_us)
        v->picture_gap_max_us = completed - v->last_picture_time_us;
    v->last_picture_time_us = completed;
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
    /* Native pictures live in our fixed output ring. Preserve the last valid
     * one before reusing its reservation, even after no-picture Decode calls.
     * Otherwise only the newest valid output of a batch needs conversion. */
    if (s->pending_valid && s->pending.pFrameBuffer == m->p)
        x4_live_video_convert_pending(v);
    OrbisVideodec2FrameBuffer frame = {.thisSize = sizeof(frame), .pFrameBuffer = m->p, .frameBufferSize = s->frame_size};
    OrbisVideodec2OutputInfo output = {.thisSize = sizeof(output)};
    OrbisVideodec2InputData input = {.thisSize = sizeof(input), .pAuData = compressed->p,
        .auSize = size, .ptsData = pts, .dtsData = pts};
    uint64_t begin = sceKernelGetProcessTime();
    int rc = s->Decode(s->decoder, &input, &frame, &output);
    uint64_t elapsed = sceKernelGetProcessTime() - begin;
    ++v->decode_calls; v->decode_us += elapsed;
    if (elapsed > v->decode_max_us) v->decode_max_us = elapsed;
    if (rc < 0) return stage(v, "DECODIFICAR JUEGO H264", rc);
    s->input_slot = (s->input_slot + 1) % INPUT_RING;
    if (frame.isAccepted) s->slot = (s->slot + 1) % RING;
    rc = validate_picture(s, &output);
    if (rc < 0) return stage(v, "FORMATO DE IMAGEN", rc);
    if (rc > 0) { s->pending = output; s->pending_valid = true; ++v->decoded_frames; }
    else ++v->no_picture_calls;
    return 0;
}
