/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_h264.h"
#include "videodec2_abi.h"
#include "../core/module.h"
#include <orbis/libkernel.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <cpuid.h>
#include <emmintrin.h>
#include <smmintrin.h>

enum { RING = 4, INPUT_RING = 2, MAX_MEMORY = 64 * 1024 * 1024,
    COPY_HELPERS = X4_LIVE_COPY_READERS - 1 };
/* VIDEO trace namespace. IDs identify observed calls/leased outputs, never
 * correlate the current input PTS/AU with a returned native picture. */
enum {
    VT_DECODE_BEGIN = X4_TRACE_DECODE_BEGIN, VT_DECODE_END = X4_TRACE_DECODE_END,
    VT_OUTPUT_INVALID = X4_TRACE_OUTPUT_REJECT, VT_OUTPUT = X4_TRACE_OUTPUT_VALID,
    VT_OUTPUT_SUPERSEDE = X4_TRACE_OUTPUT_SUPERSEDED,
    VT_COPY_BEGIN = X4_TRACE_COPY_BEGIN, VT_COPY_END = X4_TRACE_COPY_END,
    VT_CONVERT_BEGIN = X4_TRACE_CONVERT_BEGIN, VT_CONVERT_END = X4_TRACE_CONVERT_END,
    VT_NOFRAME = X4_TRACE_OUTPUT_NONE,
    VT_OWNER_BEGIN = 0x220, VT_OWNER_END, VT_HELPER_BEGIN, VT_HELPER_END,
    VT_WAIT_BEGIN, VT_WAIT_END, VT_CHECK_BEGIN, VT_CHECK_END, VT_GEOMETRY,
    VT_ERROR, VT_START, VT_STOP, VT_FEED_REJECT, VT_PRESERVE_BEGIN, VT_PRESERVE_END,
    VT_COPY_CONFIG
};
typedef struct { void *p; off_t offset; size_t size; int allocated; } Memory;
typedef struct {
    const uint8_t *source;
    uint8_t *destination;
    size_t length;
    bool streaming;
    uint64_t trace_copy;
} CopyJob;

typedef struct VideoState VideoState;
typedef struct {
    VideoState *owner;
    unsigned index; /* Immutable for this persistent worker's lifetime. */
    OrbisPthread thread;
    bool running;
    atomic_bool stop;
    atomic_uint_fast64_t submitted, completed;
    CopyJob job;
    int result;
    uint64_t elapsed_us;
} CopyHelper;

struct VideoState {
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
    X4Trace *trace;
    uint64_t pending_output;
    void *staging_allocation;
    uint8_t *staging;
    size_t staging_size;
    bool streaming_copy;
    CopyHelper helpers[COPY_HELPERS];
    bool parallel_enabled, copy_check_done;
    unsigned reported_readers;
    int copy_join_error;
    uint64_t copy_sequence;
};

static void *copy_worker(void *context);

static unsigned copy_helper_count(const VideoState *s)
{
    unsigned count = 0;
    for (unsigned i = 0; i < COPY_HELPERS; ++i) if (s->helpers[i].running) ++count;
    return count;
}

/* Only configuration transitions are reported. A four-reader initial setting
 * is available capacity, pending the first-valid-picture comparison; COPY_END
 * still identifies the selected path for each copy. */
static void report_copy_config(VideoState *s, unsigned readers, unsigned reason)
{
    if (s->reported_readers == readers) return;
    s->reported_readers = readers;
    x4_trace_record(s->trace, VT_COPY_CONFIG, (uint16_t)reason, X4_LIVE_COPY_READERS, readers);
    printf("XCloud4: video copy config requested_readers=%u configured_available_readers=%u helper_count=%u reason=%u\n",
        (unsigned)X4_LIVE_COPY_READERS, readers, copy_helper_count(s), reason);
}

static int stop_copy_helpers(VideoState *s)
{
    if (s->copy_join_error) return s->copy_join_error;
    /* Signal every started worker before joining any of them. A failure must
     * not prevent join attempts for other workers or free any shared storage. */
    for (unsigned i = 0; i < COPY_HELPERS; ++i)
        if (s->helpers[i].running) atomic_store_explicit(&s->helpers[i].stop, true, memory_order_release);
    int failure = 0;
    for (unsigned i = 0; i < COPY_HELPERS; ++i) {
        CopyHelper *helper = &s->helpers[i];
        if (!helper->running) continue;
        int rc = scePthreadJoin(helper->thread, NULL);
        printf("XCloud4: video copy helper join index=%u rc=0x%08x\n", i, (unsigned)rc);
        if (!rc) helper->running = false;
        else if (!failure) failure = rc < 0 ? rc : -rc;
    }
    if (failure) s->copy_join_error = failure;
    return failure;
}

static int stage(X4LiveVideo *v, const char *name, int rc)
{
    snprintf(v->stage, sizeof(v->stage), "%s", name);
    if (rc < 0) v->error = rc;
    if (rc < 0) x4_trace_record(v->trace, VT_ERROR, 0, v->trace_decoder_epoch, (uint32_t)rc);
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
    x4_trace_record(v->trace, VT_STOP, 0, v->trace_decoder_epoch, 0);
    /* A failed join leaves the helper lifetime unknown. Never retry native
     * deletion or release its source/staging while that lifetime is unknown. */
    int helper_rc = stop_copy_helpers(s);
    if (helper_rc < 0) return stage(v, "CERRAR COPIA CPU", helper_rc);
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
    free(s->staging_allocation); s->staging_allocation = NULL; s->staging = NULL;
    /* RGB mailbox storage belongs to media and survives teardown failure. */
    v->pixels = NULL;
    free(s); v->state = NULL;
    return 0;
}

int x4_live_video_start(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity)
{
    if (!v || !pixels || pixel_capacity < (size_t)X4_LIVE_WIDTH * X4_LIVE_HEIGHT) return -7;
    if (v->state) { int rc = x4_live_video_stop(v); if (rc < 0) return rc; }
    /* Diagnostic identity is session-scoped, unlike the restarted decoder's
     * ordinary counters. The pointer stays alive through helper/owner joins. */
    X4Trace *trace = v->trace;
    uint64_t decode_id = v->trace_decode_next, output_id = v->trace_output_next;
    uint64_t copy_id = v->trace_copy_next, epoch = v->trace_decoder_epoch + 1;
    memset(v, 0, sizeof(*v));
    v->trace = trace; v->trace_decode_next = decode_id; v->trace_output_next = output_id;
    v->trace_copy_next = copy_id; v->trace_decoder_epoch = epoch;
    x4_trace_record(trace, VT_START, 0, epoch, 0);
    VideoState *s = calloc(1, sizeof(*s));
    if (!s) return stage(v, "MEMORIA", -5);
    for (unsigned i = 0; i < COPY_HELPERS; ++i) {
        CopyHelper *helper = &s->helpers[i];
        helper->owner = s; helper->index = i;
        atomic_init(&helper->stop, false);
        atomic_init(&helper->submitted, 0);
        atomic_init(&helper->completed, 0);
    }
    v->state = s;
    s->trace = trace;
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
    if (s->staging_size > SIZE_MAX - 63) { rc = stage(v, "LIMITES NV12 CPU", -6); goto fail; }
    s->staging_allocation = malloc(s->staging_size + 63);
    if (!s->staging_allocation) { rc = stage(v, "IMAGEN NV12 CPU", -5); goto fail; }
    s->staging = (uint8_t *)(((uintptr_t)s->staging_allocation + 63) & ~(uintptr_t)63);
    unsigned eax, ebx, ecx, edx;
    s->streaming_copy = __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 19));
    printf("XCloud4: video NV12 copy mode=%u capacity=%zu source_alignment=16\n",
        s->streaming_copy ? 1u : 0u, s->staging_size);
    v->pixels = pixels;
    printf("XCloud4: video RGB convert mode=2 block_pixels=8 scalar_tail_max=7\n");
    rc = s->CreateDecoder(&config, &memory, &s->decoder);
    if (stage(v, "CREAR DECODER", rc) < 0) goto fail;
    /* Persistent helpers receive only bounded CPU-copy jobs. Their worker
     * routine performs no decoder, RGB publication or native teardown calls. */
    static const char *const names[COPY_HELPERS] = {"x4-copy0", "x4-copy1", "x4-copy2"};
    for (unsigned i = 0; i < COPY_HELPERS; ++i) {
        CopyHelper *helper = &s->helpers[i];
        int helper_rc = scePthreadCreate(&helper->thread, NULL, copy_worker, helper, names[i]);
        helper->running = helper_rc == 0;
        printf("XCloud4: video copy helper create index=%u rc=0x%08x enabled=%u staging_alignment=64\n",
            i, (unsigned)helper_rc, helper->running ? 1u : 0u);
    }
    s->parallel_enabled = copy_helper_count(s) == COPY_HELPERS;
    if (!s->parallel_enabled) {
        /* No job is dispatched into a partial pool. Serial fallback is safe
         * only after every started helper is successfully joined. */
        rc = stop_copy_helpers(s);
        if (rc < 0) { stage(v, "CERRAR COPIA CPU", rc); goto fail; }
    }
    report_copy_config(s, s->parallel_enabled ? X4_LIVE_COPY_READERS : 1u,
        s->parallel_enabled ? 0u : 1u);

    stage(v, "REPRODUCIENDO", 0);
    return 0;
fail:
    /* Keep the original failure label unless cleanup itself fails. */
    x4_live_video_stop(v);
    return rc;
}

int x4_live_video_set_target(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity)
{
    if (!v || !v->state || !pixels || pixel_capacity < (size_t)X4_LIVE_WIDTH * X4_LIVE_HEIGHT) return -7;
    v->pixels = pixels;
    return 0;
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

static void copy_span(uint8_t *destination, const uint8_t *source, size_t length, bool streaming)
{
    /* Each reader orders its own WC loads and cached stores. These fences
     * preserve the existing CPU ordering; they are not a new GPU contract. */
    _mm_mfence();
    if (streaming) copy_nv12_sse41(destination, source, length);
    else copy_nv12_sse2(destination, source, length);
    _mm_mfence();
}

static bool valid_copy_job(const VideoState *s, const CopyJob *job)
{
    if (!job->source || !job->destination || !job->length || job->length > s->staging_size ||
        ((uintptr_t)job->source & 15) || ((uintptr_t)job->destination & 63)) return false;
    uintptr_t destination = (uintptr_t)job->destination, staging = (uintptr_t)s->staging;
    if (destination < staging || destination - staging > s->staging_size - job->length) return false;
    uintptr_t source = (uintptr_t)job->source;
    for (unsigned i = 0; i < RING; ++i) {
        uintptr_t base = (uintptr_t)s->output[i].p;
        if (s->output[i].p && job->length <= s->output[i].size && source >= base &&
            source - base <= s->output[i].size - job->length) return true;
    }
    return false;
}

static bool make_copy_jobs(const VideoState *s, const uint8_t *source, size_t length,
    uint64_t copy_id, CopyJob jobs[X4_LIVE_COPY_READERS])
{
    if (!source || !s->staging || !length || length > s->staging_size ||
        ((uintptr_t)source & 63) || ((uintptr_t)s->staging & 63)) return false;
    /* The first three spans contain floor(length/256)*64 bytes. Their starts
     * and ends are cache-line aligned. The last span owns the exact remainder,
     * including the leaf routine's bounded 16-byte/scalar tail. No gaps or
     * overlap: offsets are 0,span,2*span,3*span and the final end is length. */
    size_t span = (length / X4_LIVE_COPY_READERS) & ~(size_t)63;
    if (span < 64) return false;
    size_t at = 0;
    for (unsigned i = 0; i < X4_LIVE_COPY_READERS; ++i) {
        size_t bytes = i + 1 == X4_LIVE_COPY_READERS ? length - at : span;
        if (bytes < 64 || bytes > length - at) return false;
        jobs[i] = (CopyJob){source + at, s->staging + at, bytes, s->streaming_copy, copy_id};
        if (!valid_copy_job(s, &jobs[i])) return false;
        at += bytes;
    }
    return at == length;
}

static void wait_copy_helpers(const VideoState *s, const uint64_t sequences[COPY_HELPERS])
{
    /* Acquire every submitted completion before reading any result or touching
     * leased native/staging storage. Never return early for a failed job. */
    for (;;) {
        bool complete = true;
        for (unsigned i = 0; i < COPY_HELPERS; ++i)
            if (atomic_load_explicit(&s->helpers[i].completed, memory_order_acquire) != sequences[i])
                complete = false;
        if (complete) return;
        sceKernelUsleep(50);
    }
}

static void *copy_worker(void *context)
{
    CopyHelper *helper = context;
    VideoState *s = helper->owner;
    const uint16_t index_flags = (uint16_t)(helper->index << 8);
    uint64_t previous = 0;
    for (;;) {
        uint64_t sequence = atomic_load_explicit(&helper->submitted, memory_order_acquire);
        if (sequence != previous) {
            /* The owner does not rewrite this job until completion acquire.
             * Storage/reservations remain alive until this thread is joined. */
            CopyJob job = helper->job;
            int result = valid_copy_job(s, &job) ? 0 : -7;
            x4_trace_record(s->trace, VT_HELPER_BEGIN, index_flags, job.trace_copy, job.length);
            uint64_t begin = sceKernelGetProcessTime();
            if (!result) copy_span(job.destination, job.source, job.length, job.streaming);
            helper->result = result;
            helper->elapsed_us = sceKernelGetProcessTime() - begin;
            previous = sequence;
            atomic_store_explicit(&helper->completed, sequence, memory_order_release);
            /* Local IDs remain valid even if completion lets owner publish
             * its next job before this diagnostic record is captured. */
            x4_trace_record(s->trace, VT_HELPER_END, index_flags | (result ? 1u : 0u),
                job.trace_copy, (uint32_t)result);
            continue;
        }
        /* A published job is completed before stop can be honored. There is
         * no timeout fallback that might overwrite an in-flight helper span. */
        if (atomic_load_explicit(&helper->stop, memory_order_acquire)) break;
        sceKernelUsleep(1000);
    }
    return NULL;
}

enum { COPY_CHECK_NOT_CHECKED = 0, COPY_CHECK_PASS = 1, COPY_CHECK_MISMATCH = 2 };
enum { COPY_REASON_NONE = 0, COPY_REASON_NO_HELPER = 1, COPY_REASON_SPANS = 2,
    COPY_REASON_MEMORY = 3, COPY_REASON_JOB = 4 };

static void report_copy_check(unsigned result, unsigned reason, size_t length, uint64_t elapsed)
{
    printf("XCloud4: video copy bytecheck result=%u reason=%u bytes=%zu check_us=%llu\n",
        result, reason, length, (unsigned long long)elapsed);
}

static int copy_picture(X4LiveVideo *v, VideoState *s, const uint8_t *source, size_t length)
{
    if (!source || !s->staging || !length || length > s->staging_size || ((uintptr_t)source & 15))
        return stage(v, "LIMITES COPIA CPU", -7);
    uint64_t copy_id = ++v->trace_copy_next;
    CopyJob jobs[X4_LIVE_COPY_READERS];
    bool eligible = make_copy_jobs(s, source, length, copy_id, jobs);
    bool pool_ready = copy_helper_count(s) == COPY_HELPERS;
    bool parallel = s->parallel_enabled && pool_ready && eligible;
    bool check = !s->copy_check_done;
    x4_trace_record(v->trace, VT_COPY_BEGIN, parallel ? 1u : 0u, copy_id, s->pending_output);
    uint8_t *reference = NULL;
    uint64_t check_pre_us = 0;
    if (check) {
        x4_trace_record(v->trace, VT_CHECK_BEGIN, 0, copy_id, length);
        s->copy_check_done = true;
        ++v->copy_check_attempts;
        uint64_t begin = sceKernelGetProcessTime();
        unsigned reason = !pool_ready ? COPY_REASON_NO_HELPER : COPY_REASON_SPANS;
        if (parallel) {
            reference = malloc(length);
            if (!reference) { reason = COPY_REASON_MEMORY; parallel = false; }
        }
        check_pre_us = sceKernelGetProcessTime() - begin;
        if (!parallel) {
            /* The first valid picture is the only verification opportunity.
             * Never enable an unverified parallel path later in this start. */
            s->parallel_enabled = false;
            ++v->copy_check_not_checked;
            v->copy_check_us += check_pre_us;
            report_copy_check(COPY_CHECK_NOT_CHECKED, reason, length, check_pre_us);
            x4_trace_record(v->trace, VT_CHECK_END, (uint16_t)reason, copy_id, COPY_CHECK_NOT_CHECKED);
        }
    }
    report_copy_config(s, parallel ? X4_LIVE_COPY_READERS : 1u, parallel ? 0u : 2u);

    uint64_t begin = sceKernelGetProcessTime();
    int result = 0;
    if (parallel) {
        uint64_t sequences[COPY_HELPERS];
        bool idle = true;
        for (unsigned i = 0; i < COPY_HELPERS; ++i) {
            CopyHelper *helper = &s->helpers[i];
            sequences[i] = atomic_load_explicit(&helper->submitted, memory_order_acquire);
            if (sequences[i] != s->copy_sequence ||
                atomic_load_explicit(&helper->completed, memory_order_acquire) != sequences[i]) idle = false;
        }
        if (!idle) result = -7;
        else {
            /* All four jobs were validated before publication. Write all
             * private job records before allowing any helper to start. */
            for (unsigned i = 0; i < COPY_HELPERS; ++i) s->helpers[i].job = jobs[i + 1];
            uint64_t sequence = ++s->copy_sequence;
            for (unsigned i = 0; i < COPY_HELPERS; ++i) {
                sequences[i] = sequence;
                atomic_store_explicit(&s->helpers[i].submitted, sequence, memory_order_release);
            }
            uint64_t owner_begin = sceKernelGetProcessTime();
            x4_trace_record(v->trace, VT_OWNER_BEGIN, 1u, copy_id, jobs[0].length);
            copy_span(jobs[0].destination, jobs[0].source, jobs[0].length, jobs[0].streaming);
            v->copy_owner_us += sceKernelGetProcessTime() - owner_begin;
            x4_trace_record(v->trace, VT_OWNER_END, 1u, copy_id, 0);
        }
        /* Even a preflight inconsistency waits for every previously submitted
         * job before returning an error; it never permits lease reuse. */
        uint64_t wait_begin = sceKernelGetProcessTime();
        x4_trace_record(v->trace, VT_WAIT_BEGIN, 0, copy_id, s->pending_output);
        wait_copy_helpers(s, sequences);
        uint64_t wait_us = sceKernelGetProcessTime() - wait_begin;
        x4_trace_record(v->trace, VT_WAIT_END, 0, copy_id, wait_us);
        v->copy_wait_us += wait_us;
        if (wait_us > v->copy_wait_max_us) v->copy_wait_max_us = wait_us;
        if (idle) {
            for (unsigned i = 0; i < COPY_HELPERS; ++i) {
                v->copy_helper_us += s->helpers[i].elapsed_us;
                if (!result && s->helpers[i].result) result = s->helpers[i].result;
            }
        }
        _mm_mfence();
    } else {
        uint64_t owner_begin = sceKernelGetProcessTime();
        x4_trace_record(v->trace, VT_OWNER_BEGIN, 0, copy_id, length);
        copy_span(s->staging, source, length, s->streaming_copy);
        v->copy_owner_us += sceKernelGetProcessTime() - owner_begin;
        x4_trace_record(v->trace, VT_OWNER_END, 0, copy_id, 0);
    }
    uint64_t elapsed = sceKernelGetProcessTime() - begin;
    x4_trace_record(v->trace, VT_COPY_END, (parallel ? 1u : 0u) | (result ? 2u : 0u), copy_id, elapsed);
    if (result) {
        if (reference) {
            free(reference);
            ++v->copy_check_not_checked; v->copy_check_us += check_pre_us;
            report_copy_check(COPY_CHECK_NOT_CHECKED, COPY_REASON_JOB, length, check_pre_us);
            x4_trace_record(v->trace, VT_CHECK_END, COPY_REASON_JOB, copy_id, COPY_CHECK_NOT_CHECKED);
        }
        s->parallel_enabled = false;
        report_copy_config(s, 1u, 3u);
        /* Caller retains pending_valid and cannot Decode/reuse this output. */
        return stage(v, "COPIAR IMAGEN CPU", result);
    }
    ++v->copy_calls; v->copy_bytes += length; v->copy_us += elapsed;
    if (elapsed > v->copy_max_us) v->copy_max_us = elapsed;
    if (parallel) ++v->copy_parallel_calls; else ++v->copy_serial_calls;
    if (reference) {
        /* All four spans are complete. The same native picture is still leased;
         * the extra serial reference is excluded from selected-copy timing. */
        uint64_t check_begin = sceKernelGetProcessTime();
        copy_span(reference, source, length, s->streaming_copy);
        bool equal = memcmp(s->staging, reference, length) == 0;
        if (equal) ++v->copy_check_pass;
        else {
            ++v->copy_check_mismatch;
            memcpy(s->staging, reference, length);
            s->parallel_enabled = false;
        }
        free(reference);
        uint64_t check_us = check_pre_us + sceKernelGetProcessTime() - check_begin;
        v->copy_check_bytes += length; v->copy_check_us += check_us;
        report_copy_check(equal ? COPY_CHECK_PASS : COPY_CHECK_MISMATCH, COPY_REASON_NONE, length, check_us);
        x4_trace_record(v->trace, VT_CHECK_END, COPY_REASON_NONE, copy_id,
            equal ? COPY_CHECK_PASS : COPY_CHECK_MISMATCH);
        if (!equal) report_copy_config(s, 1u, 3u);
    }
    return 0;
}

static int validate_picture(VideoState *s, const OrbisVideodec2OutputInfo *o, unsigned *reason)
{
    *reason = 0;
    if (o->isErrorFrame) { *reason = 1; return -7; }
    if (!o->isValid) return 0;
    if (o->codecType != 1 || !o->frameWidth || o->frameWidth > X4_LIVE_WIDTH || (o->frameWidth & 1) ||
        !o->frameHeight || o->frameHeight > X4_LIVE_HEIGHT || (o->frameHeight & 1) || o->framePitch < o->frameWidth || o->framePitch > 8192) { *reason = 2; return -7; }
    size_t length = (size_t)o->framePitch * o->frameHeight * 3 / 2;
    if (length > o->frameBufferSize || length > s->staging_size || !s->staging || ((uintptr_t)o->pFrameBuffer & 15)) { *reason = 3; return -7; }
    int owned = 0;
    for (unsigned i = 0; i < RING; ++i)
        if (o->pFrameBuffer == s->output[i].p && length <= s->output[i].size) owned = 1;
    if (!owned) { *reason = 4; return -7; }
    return 1;
}

int x4_live_video_convert_pending(X4LiveVideo *v)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->pending_valid) return 0;
    if (v->error) return v->error;
    const OrbisVideodec2OutputInfo *o = &s->pending;
    uint64_t output_id = s->pending_output;
    size_t length = (size_t)o->framePitch * o->frameHeight * 3 / 2;
    int rc = copy_picture(v, s, o->pFrameBuffer, length);
    if (rc < 0) return rc;
    uint64_t begin = sceKernelGetProcessTime();
    x4_trace_record(v->trace, VT_CONVERT_BEGIN, 0, output_id, 0);
    convert_nv12_sse2(v->pixels, s->staging, o->frameWidth, o->frameHeight, o->framePitch);
    s->pending_valid = false;
    v->width = o->frameWidth; v->height = o->frameHeight;
    uint64_t completed = sceKernelGetProcessTime(), elapsed = completed - begin;
    v->convert_us += elapsed;
    if (elapsed > v->convert_max_us) v->convert_max_us = elapsed;
    if (v->last_picture_time_us && completed - v->last_picture_time_us > v->picture_gap_max_us)
        v->picture_gap_max_us = completed - v->last_picture_time_us;
    v->last_picture_time_us = completed;
    v->trace_converted_output = output_id;
    x4_trace_record(v->trace, VT_CONVERT_END, 0, output_id, elapsed);
    if (++v->frames == 1) printf("XCloud4: primera imagen H264 %ux%u pitch=%u\n", o->frameWidth, o->frameHeight, o->framePitch);
    return 0;
}


int x4_live_video_feed(X4LiveVideo *v, const uint8_t *bytes, size_t size, uint64_t pts)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->decoder || !bytes || !size || size > X4_LIVE_AU_MAX || v->error) {
        if (v) x4_trace_record(v->trace, VT_FEED_REJECT, 0, v->trace_au, size);
        return -7;
    }
    Memory *compressed = &s->input[s->input_slot];
    memcpy(compressed->p, bytes, size);
    Memory *m = &s->output[s->slot];
    /* Native pictures live in our fixed output ring. Preserve the last valid
     * one before reusing its reservation, even after no-picture Decode calls.
     * Otherwise only the newest valid output of a batch needs conversion. */
    if (s->pending_valid && s->pending.pFrameBuffer == m->p) {
        ++v->forced_preserve_calls;
        uint64_t preserved_output = s->pending_output;
        x4_trace_record(v->trace, VT_PRESERVE_BEGIN, 0, preserved_output, v->trace_au);
        int rc = x4_live_video_convert_pending(v);
        x4_trace_record(v->trace, VT_PRESERVE_END, 0, preserved_output, (uint32_t)rc);
        if (rc < 0 || v->error || s->pending_valid)
            return rc < 0 ? rc : v->error ? v->error : stage(v, "PRESERVAR IMAGEN", -7);
    }
    OrbisVideodec2FrameBuffer frame = {.thisSize = sizeof(frame), .pFrameBuffer = m->p, .frameBufferSize = s->frame_size};
    OrbisVideodec2OutputInfo output = {.thisSize = sizeof(output)};
    OrbisVideodec2InputData input = {.thisSize = sizeof(input), .pAuData = compressed->p,
        .auSize = size, .ptsData = pts, .dtsData = pts};
    uint64_t call_id = ++v->trace_decode_next;
    x4_trace_record(v->trace, VT_DECODE_BEGIN, 0, call_id, v->trace_au);
    uint64_t begin = sceKernelGetProcessTime();
    int rc = s->Decode(s->decoder, &input, &frame, &output);
    uint64_t elapsed = sceKernelGetProcessTime() - begin;
    x4_trace_record(v->trace, VT_DECODE_END,
        (frame.isAccepted ? 1u : 0u) | (output.isValid ? 2u : 0u) | (output.isErrorFrame ? 4u : 0u),
        call_id, (uint32_t)rc);
    ++v->decode_calls; v->decode_us += elapsed;
    if (elapsed > v->decode_max_us) v->decode_max_us = elapsed;
    if (rc < 0) return stage(v, "DECODIFICAR JUEGO H264", rc);
    s->input_slot = (s->input_slot + 1) % INPUT_RING;
    if (frame.isAccepted) s->slot = (s->slot + 1) % RING;
    unsigned reason = 0;
    rc = validate_picture(s, &output, &reason);
    if (rc < 0) {
        x4_trace_record(v->trace, VT_OUTPUT_INVALID, (uint16_t)reason, call_id, v->trace_decoder_epoch);
        return stage(v, "FORMATO DE IMAGEN", rc);
    }
    if (rc > 0) {
        uint64_t output_id = ++v->trace_output_next;
        if (s->pending_valid) x4_trace_record(v->trace, VT_OUTPUT_SUPERSEDE, 0, s->pending_output, output_id);
        s->pending = output; s->pending_valid = true; s->pending_output = output_id; ++v->decoded_frames;
        /* This call observed the picture. Output ABI contains no returned PTS;
         * it is not proof that the call's current AU produced this picture. */
        x4_trace_record(v->trace, VT_OUTPUT, X4_TRACE_F_OBSERVED_METADATA, output_id, v->trace_decoder_epoch);
        x4_trace_record(v->trace, VT_GEOMETRY, 0, output_id,
            ((uint64_t)output.framePitch << 32) | ((uint64_t)output.frameHeight << 16) | output.frameWidth);
    }
    else { ++v->no_picture_calls; x4_trace_record(v->trace, VT_NOFRAME, 0, call_id, v->trace_decoder_epoch); }
    return 0;
}
