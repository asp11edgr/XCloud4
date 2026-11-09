/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_h264.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_v2_abi_check.h"
#endif
#include "videodec2_abi.h"
#include "probe_case.h"
#if X4_STREAM_PROFILE
#include "decoder_depth.h"
#endif
#if X4_VIDEO_PROBE
#include "probe_reference.h"
#include "probe_hash.h"
#include "probe_contract.h"
#endif

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

enum { RING = 4, INPUT_RING = 2,
    OUTPUT_STORAGE = X4_STREAM_PROFILE ? 5 : RING,
    INPUT_STORAGE = X4_STREAM_PROFILE ? 4 : INPUT_RING, MAX_MEMORY = 64 * 1024 * 1024,
    COPY_HELPERS = X4_LIVE_COPY_READERS - 1,
#if X4_VIDEO_PROBE
    /* Probe ON only: the console query planned 106807296 bytes. */
    NATIVE_BUDGET = X4_VIDEO_CAPACITY_1080 ? 128 * 1024 * 1024 : X4_VIDEO_LEGACY_NATIVE_CEILING };
#else
    NATIVE_BUDGET = X4_VIDEO_CAPACITY_1080 ? X4_VIDEO_NATIVE_BUDGET : X4_VIDEO_LEGACY_NATIVE_CEILING };
#endif
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
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
    X4ProbeThread probe_thread;
#endif
    bool running;
    atomic_bool stop;
    atomic_uint_fast64_t submitted, completed;
    CopyJob job;
    int result;
    uint64_t elapsed_us;
#if X4_VIDEO_PROBE
    uint64_t copy_loop_us;
#endif
} CopyHelper;

struct VideoState {
#if X4_VIDEO_PROBE && X4_PROBE_T26
    bool t26;
    X4T26NativeOptions t26_options;
#endif
#if X4_STREAM_PROFILE
    bool stream_raw;
    const atomic_bool *stream_cancel;
    X4DecoderDepthLedger stream_depth;
#endif
#if X4_VIDEO_PROBE
    bool raw_pipeline;
    const X4ProbeCase *probe;
    X4ProbeStackReport *stack_reports;
    const atomic_bool *probe_cancel;
    X4ProbeReference reference;
    X4ProbeMetrics *probe_metrics;
    unsigned probe_layout_storage, probe_layout_successes;
    X4VideoNv12Layout probe_layout;
    unsigned probe_native_width, probe_native_height, probe_native_pitch;
#endif
    int32_t (*QueryComputeMemoryInfo)(OrbisVideodec2ComputeMemoryInfo *);
    int32_t (*AllocateComputeQueue)(const OrbisVideodec2ComputeConfigInfo *, const OrbisVideodec2ComputeMemoryInfo *, void **);
    int32_t (*ReleaseComputeQueue)(void *);
    int32_t (*QueryDecoderMemoryInfo)(const OrbisVideodec2DecoderConfigInfo *, OrbisVideodec2DecoderMemoryInfo *);
    int32_t (*CreateDecoder)(const OrbisVideodec2DecoderConfigInfo *, const OrbisVideodec2DecoderMemoryInfo *, void **);
    int32_t (*DeleteDecoder)(void *);
    int32_t (*Decode)(void *, const OrbisVideodec2InputData *, OrbisVideodec2FrameBuffer *, OrbisVideodec2OutputInfo *);
    int32_t (*Flush)(void *, OrbisVideodec2FrameBuffer *, OrbisVideodec2OutputInfo *);
    void *queue, *decoder;
    Memory compute, cpu, gpu, shared, input[INPUT_STORAGE], output[OUTPUT_STORAGE];
    size_t frame_size;
    X4VideoCapacity capacity;
    size_t rgb_pixel_capacity;
    unsigned input_slot, slot;
    OrbisVideodec2OutputInfo pending;
    X4VideoNv12Layout pending_layout;
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

static unsigned owned_input_count(const VideoState *s)
{
#if X4_STREAM_PROFILE
    if(s->stream_raw)return s->stream_depth.input_count;
#else
    (void)s;
#endif
    return INPUT_RING;
}
static unsigned owned_output_count(const VideoState *s)
{
#if X4_STREAM_PROFILE
    if(s->stream_raw)return s->stream_depth.output_count;
#else
    (void)s;
#endif
    return RING;
}

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
        if (!rc) {
            helper->running = false;
#if X4_STREAM_PROFILE
            x4_probe_thread_joined(&helper->probe_thread,rc);
#endif
#if X4_VIDEO_PROBE
            if (s->probe) {
                x4_probe_thread_joined(&helper->probe_thread, rc);
                s->stack_reports[i] = helper->probe_thread.report;
            }
#endif
        }
        else if (!failure) failure = rc < 0 ? rc : -rc;
    }
    if (failure) s->copy_join_error = failure;
    return failure;
}

static int stage(X4LiveVideo *v, const char *name, int rc)
{
#if X4_VIDEO_PROBE
    VideoState *s = v->state;
    if (s && s->probe) {
        uint64_t allocated = 0;
        Memory *blocks[] = {&s->compute, &s->cpu, &s->gpu, &s->shared,
            &s->input[0], &s->input[1], &s->output[0], &s->output[1], &s->output[2], &s->output[3]};
        for (unsigned i = 0; i < sizeof(blocks)/sizeof(*blocks); ++i)
            if (blocks[i]->allocated) allocated += blocks[i]->size;
        v->probe_status.allocated_native_bytes = allocated;
        if (allocated > v->probe_status.peak_native_allocated_bytes)
            v->probe_status.peak_native_allocated_bytes = allocated;
    }
#endif
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
    if (x4_video_size_align(size, align, &m->size) || m->size > MAX_MEMORY) return -2;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), m->size, align, type, &m->offset);
#if X4_VIDEO_PROBE
    printf("XCloud4 probe: native_allocate bytes=%zu alignment=%zu type=%d rc=0x%08x\n", m->size, align, type, (unsigned)rc);
#endif
    if (rc < 0) return rc;
    m->allocated = 1;
    rc = sceKernelMapDirectMemory(&m->p, m->size, 0x33, 0, m->offset, align);
#if X4_VIDEO_PROBE
    printf("XCloud4 probe: native_map bytes=%zu rc=0x%08x\n", m->size, (unsigned)rc);
#endif
    if (rc >= 0) memset(m->p, 0, m->size);
    else m->p = NULL;
    return rc;
}

static int release(Memory *m)
{
    if (m->p) {
        int rc = sceKernelMunmap(m->p, m->size);
#if X4_VIDEO_PROBE
        printf("XCloud4 probe: native_unmap bytes=%zu rc=0x%08x\n", m->size, (unsigned)rc);
#endif
        if (rc < 0) return rc;
        m->p = NULL;
    }
    if (m->allocated) {
        int rc = sceKernelReleaseDirectMemory(m->offset, m->size);
#if X4_VIDEO_PROBE
        printf("XCloud4 probe: native_release bytes=%zu rc=0x%08x\n", m->size, (unsigned)rc);
#endif
        if (rc < 0) return rc;
        m->allocated = 0;
    }
    return 0;
}
#if X4_STREAM_PROFILE
static int release_stream(Memory *m)
{
    if(m->p){int rc=sceKernelMunmap(m->p,m->size);if(rc)return rc<0?rc:-rc;m->p=NULL;}
    if(m->allocated){int rc=sceKernelReleaseDirectMemory(m->offset,m->size);if(rc)return rc<0?rc:-rc;m->allocated=0;}
    return 0;
}
static int release_owned(VideoState *s,Memory *m)
{return s->stream_raw?release_stream(m):release(m);}
#else
#define release_owned(s,m) release(m)
#endif

int x4_live_video_stop(X4LiveVideo *v)
{
    VideoState *s = v->state;
    if (!s) return 0;
    x4_trace_record(v->trace, VT_STOP, 0, v->trace_decoder_epoch, 0);
    /* A failed join leaves the helper lifetime unknown. Never retry native
     * deletion or release its source/staging while that lifetime is unknown. */
    int helper_rc = stop_copy_helpers(s);
    if (helper_rc < 0) return stage(v, "CERRAR COPIA CPU", helper_rc);
#if X4_STREAM_PROFILE
    if(s->stream_raw&&v->stream_info.creation_uncertain){
        /* Create claimed success without a usable handle. No verified native
         * operation can retire its possible references. Retain, do not retry. */
        v->stream_info.close_result=-7;v->stream_info.retained=true;
        return stage(v,"CREATE SIN HANDLE: RECURSOS RETENIDOS",-7);
    }
#endif
    /* Do not free buffers still owned by the decoder if teardown fails. */
    if (s->decoder) {
#if X4_VIDEO_PROBE && X4_PROBE_T26
        if(s->t26&&s->t26_options.before){
            int marker=s->t26_options.before(s->t26_options.context,"DeleteDecoder");
            if(marker){v->stream_info.close_result=marker;v->stream_info.retained=true;
                return stage(v,"MARCADOR DELETE T26",marker);}
        }
#endif
        int rc = s->DeleteDecoder(s->decoder);
#if X4_STREAM_PROFILE
        if(s->stream_raw) { v->stream_info.delete_decoder_result=rc; v->stream_info.retained=rc!=0; }
        if(s->stream_raw&&rc){v->stream_info.close_result=rc<0?rc:-rc;return stage(v,"CERRAR DECODER",rc<0?rc:-rc);}
#endif
#if X4_VIDEO_PROBE
        if (s->probe) v->probe_status.delete_decoder_result = rc;
        printf("XCloud4 probe: DeleteDecoder rc=0x%08x\n", (unsigned)rc);
#endif
        if (rc < 0) return stage(v, "CERRAR DECODER", rc);
        s->decoder = NULL;
#if X4_STREAM_PROFILE
        /* No streaming Flush: successful Delete is the only close/restart
         * ownership proof. Caller has quiesced conversion before stop. */
        if(s->stream_raw)x4_decoder_depth_delete_complete(&s->stream_depth);
#endif
    }
    if (s->queue) {
        int rc = s->ReleaseComputeQueue(s->queue);
#if X4_STREAM_PROFILE
        if(s->stream_raw) { v->stream_info.release_queue_result=rc; v->stream_info.retained=rc!=0; }
        if(s->stream_raw&&rc){v->stream_info.close_result=rc<0?rc:-rc;return stage(v,"CERRAR COLA",rc<0?rc:-rc);}
#endif
#if X4_VIDEO_PROBE
        if (s->probe) v->probe_status.release_queue_result = rc;
        printf("XCloud4 probe: ReleaseComputeQueue rc=0x%08x\n", (unsigned)rc);
#endif
        if (rc < 0) return stage(v, "CERRAR COLA", rc);
        s->queue = NULL;
    }
#if X4_VIDEO_PROBE
    if (s->probe) {
        int reference_rc = x4_probe_reference_close(&s->reference);
        v->probe_status.reference_close_result = reference_rc;
        printf("XCloud4 probe: reference_close rc=%d\n", reference_rc);
        /* fclose destroys this CPU-only reader state even on failure. No
         * helper/decoder/compute lease remains here: retain the return code
         * but release native allocations whose lifetimes are now known. */
    }
#endif
    /* Keep the context and any unreleased reservations on native failure,
     * including after a successful unmap but failed direct-memory release. */
    for (unsigned i = 0; i < owned_output_count(s); ++i) {
        int rc = release_owned(s,&s->output[i]); if (rc < 0) {
#if X4_STREAM_PROFILE
            if(s->stream_raw){v->stream_info.close_result=rc;v->stream_info.retained=true;}
#endif
            return stage(v, "LIBERAR IMAGEN", rc);
        }
    }
    for (unsigned i = 0; i < owned_input_count(s); ++i) {
        int rc = release_owned(s,&s->input[i]); if (rc < 0) {
#if X4_STREAM_PROFILE
            if(s->stream_raw){v->stream_info.close_result=rc;v->stream_info.retained=true;}
#endif
            return stage(v, "LIBERAR ENTRADA", rc);
        }
    }
    Memory *remaining[] = {&s->shared, &s->gpu, &s->cpu, &s->compute};
    for (unsigned i = 0; i < sizeof(remaining) / sizeof(*remaining); ++i) {
        int rc = release_owned(s,remaining[i]); if (rc < 0) {
#if X4_STREAM_PROFILE
            if(s->stream_raw){v->stream_info.close_result=rc;v->stream_info.retained=true;}
#endif
            return stage(v, "LIBERAR MEMORIA", rc);
        }
    }
    free(s->staging_allocation); s->staging_allocation = NULL; s->staging = NULL;
    /* RGB mailbox storage belongs to media and survives teardown failure. */
    v->pixels = NULL;
#if X4_STREAM_PROFILE
    if(s->stream_raw && v->stream_info.attempt_count)
        v->stream_info.attempts[v->stream_info.active_attempt].cleanup=0;
#endif
    free(s); v->state = NULL;
#if X4_STREAM_PROFILE
    v->stream_info.close_result=0; v->stream_info.retained=false;
#endif
#if X4_VIDEO_PROBE
    v->probe_status.close_result = 0;
    v->probe_status.allocated_native_bytes = 0;
#endif
    return 0;
}

static int start_configured(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity,
    const X4ProbeCase *probe, const atomic_bool *cancel
#if X4_VIDEO_PROBE
    , X4ProbeHashContext *hash_context, int output_memory_type, X4ProbeMetrics *metrics, bool raw_pipeline
#endif
    )
{
    if (!v || !pixels || pixel_capacity < (size_t)X4_LIVE_RGB_PIXELS) return -7;
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
#if X4_VIDEO_PROBE
    s->raw_pipeline = raw_pipeline;
    s->probe = probe; s->probe_cancel = cancel; s->stack_reports = v->probe_stacks;
    v->probe_status = x4_probe_status_pending();
    v->probe_status.native_policy_budget = raw_pipeline ? 128u*1024u*1024u - 3u*0x800000u : NATIVE_BUDGET;
    v->probe_status.output_memory_type = output_memory_type;
    v->probe_status.output_memory_protection = 0x33;
    s->probe_metrics = metrics;
#else
    (void)probe; (void)cancel;
#endif
    if (x4_video_capacity_plan(X4_VIDEO_CAPACITY_1080, &s->capacity)) {
        int failed = stage(v, "CAPACIDAD VIDEO", -6); x4_live_video_stop(v); return failed;
    }
    s->rgb_pixel_capacity = pixel_capacity;
#if X4_VIDEO_PROBE
    if (probe) {
        /* The validated local descriptor never changes SDP. */
        int contract = x4_probe_case_validate(probe, &s->capacity);
        if (contract) { int failed = stage(v, "CONTRATO SONDA", contract); x4_live_video_stop(v); return failed; }
        int reference = x4_probe_reference_open_verified(&s->reference, probe->reference_path,
            probe->visible_width, probe->visible_height, probe->reference_count,
            probe->reference_sha256, x4_probe_hash_file, hash_context, cancel);
        v->probe_status.reference_result = reference;
        if (reference) { int failed = stage(v, "REFERENCIA SONDA", reference); x4_live_video_stop(v); return failed; }
        v->probe_status.rgb_bytes = raw_pipeline ? s->capacity.rgb_bytes : s->capacity.all_rgb_bytes;
    }
#endif
    printf("XCloud4: video capacity storage_experimental=%u rgb_pixels=%zu rgb_bytes=%zu normal_stream_admission=%ux%u profile=66 level=31\n",
        (unsigned)X4_VIDEO_CAPACITY_1080, s->capacity.rgb_pixels, s->capacity.all_rgb_bytes,
        (unsigned)X4_LIVE_WIDTH, (unsigned)X4_LIVE_HEIGHT);
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
#if X4_VIDEO_PROBE
    if (probe) { v->probe_status.compute_query_result = rc; v->probe_status.queried_compute_bytes = cm.cpuGpuMemorySize; }
#endif
    if (stage(v, "MEMORIA COLA", rc) < 0) goto fail;
    X4VideoMemoryBlock initial_memory[] = {
        {X4_LIVE_AU_MAX, 0x4000}, {X4_LIVE_AU_MAX, 0x4000}, {cm.cpuGpuMemorySize, 0x4000}
    };
    size_t native_bytes = 0;
#if X4_VIDEO_PROBE
    if (probe) {
        size_t planned = 0;
        if (!x4_video_native_memory_total_with_budget(initial_memory, 3,
            X4_VIDEO_LEGACY_NATIVE_CEILING, &planned)) v->probe_status.rounded_native_bytes = planned;
    }
#endif
    if (x4_video_native_memory_total_with_budget(initial_memory, 3, NATIVE_BUDGET, &native_bytes)) {
        rc = stage(v, "PRESUPUESTO COLA", -6); goto fail;
    }
    rc = allocate(&s->compute, cm.cpuGpuMemorySize, 0, 0x4000);
    if (stage(v, "RESERVAR COLA", rc) < 0) goto fail;
    cm.pCpuGpuMemory = s->compute.p;
    OrbisVideodec2ComputeConfigInfo cc = {.thisSize = sizeof(cc), .computePipeId = 0, .computeQueueId = 0};
    rc = s->AllocateComputeQueue(&cc, &cm, &s->queue);
#if X4_VIDEO_PROBE
    if (probe) v->probe_status.allocate_queue_result = rc;
#endif
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
#if X4_VIDEO_PROBE
    if (probe) {
        config.profile = probe->profile_idc; config.maxLevel = probe->level_idc;
        config.maxFrameWidth = probe->coded_width; config.maxFrameHeight = probe->coded_height;
        config.maxDpbFrameCount = probe->chosen_dpb_frames;
        printf("XCloud4 probe: config case=%s profile=%u level=%u coded=%ux%u visible=%ux%u refs=%u max_dec=%u chosen_dpb=%u\n",
            probe->id, probe->profile_idc, probe->level_idc, probe->coded_width, probe->coded_height,
            probe->visible_width, probe->visible_height, probe->max_num_ref_frames,
            probe->max_dec_frame_buffering, probe->chosen_dpb_frames);
    }
#endif
    rc = s->QueryDecoderMemoryInfo(&config, &memory);
#if X4_VIDEO_PROBE
    if (probe) {
        v->probe_status.query_result = rc;
        v->probe_status.queried_cpu_bytes = memory.cpuMemorySize;
        v->probe_status.queried_gpu_bytes = memory.gpuMemorySize;
        v->probe_status.queried_shared_bytes = memory.cpuGpuMemorySize;
        v->probe_status.queried_frame_bytes = memory.maxFrameBufferSize;
    }
#endif
    if (stage(v, "MEMORIA DECODER", rc) < 0) goto fail;
    printf("XCloud4: memoria video CPU=%zu GPU=%zu SHARED=%zu FRAME=%zu ALIGN=%u\n",
        memory.cpuMemorySize, memory.gpuMemorySize, memory.cpuGpuMemorySize,
        memory.maxFrameBufferSize, memory.frameBufferAlignment);
    if (memory.cpuMemorySize > MAX_MEMORY || memory.gpuMemorySize > MAX_MEMORY ||
        memory.cpuGpuMemorySize > MAX_MEMORY || !memory.maxFrameBufferSize || memory.maxFrameBufferSize > MAX_MEMORY / RING) {
        rc = stage(v, "LIMITES MEMORIA", -6); goto fail;
    }
    X4VideoMemoryBlock native_memory[] = {
        {X4_LIVE_AU_MAX, 0x4000}, {X4_LIVE_AU_MAX, 0x4000}, {cm.cpuGpuMemorySize, 0x4000},
        {memory.cpuMemorySize, 0x4000}, {memory.gpuMemorySize, 0x4000},
        {memory.cpuGpuMemorySize, 0x4000},
        {memory.maxFrameBufferSize, memory.frameBufferAlignment},
        {memory.maxFrameBufferSize, memory.frameBufferAlignment},
        {memory.maxFrameBufferSize, memory.frameBufferAlignment},
        {memory.maxFrameBufferSize, memory.frameBufferAlignment}
    };
#if X4_VIDEO_PROBE
    if (probe) {
        size_t planned = 0;
        if (!x4_video_native_memory_total_with_budget(native_memory, sizeof(native_memory)/sizeof(*native_memory),
            X4_VIDEO_LEGACY_NATIVE_CEILING, &planned)) v->probe_status.rounded_native_bytes = planned;
    }
#endif
    if (x4_video_native_memory_total_with_budget(native_memory, sizeof(native_memory) / sizeof(*native_memory), NATIVE_BUDGET, &native_bytes)) {
        rc = stage(v, "PRESUPUESTO DECODER", -6); goto fail;
    }
#if X4_VIDEO_PROBE
    /* New pipeline also owns three aligned 1080p scanouts. Include them in
     * its 128 MiB admission, even in decode-only (conservative admission).
     * CPU heap/reference scratch and explicit thread stacks are separate. */
    if (raw_pipeline && (native_bytes > 128u*1024u*1024u - 3u*0x800000u)) {
        rc = stage(v, "PRESUPUESTO DECODER Y VIDEOOUT", -6); goto fail;
    }
#endif
    printf("XCloud4: video native reservations rounded_bytes=%zu limit=%u legacy_policy=%u\n",
        native_bytes, (unsigned)NATIVE_BUDGET, (unsigned)!X4_VIDEO_CAPACITY_1080);
#if X4_VIDEO_PROBE
    if (probe) v->probe_status.rounded_native_bytes = native_bytes;
#endif
    rc = allocate(&s->cpu, memory.cpuMemorySize, 0, 0x4000);
    if (stage(v, "RESERVAR CPU", rc) < 0) goto fail;
    rc = allocate(&s->gpu, memory.gpuMemorySize, 3, 0x4000);
    if (stage(v, "RESERVAR GPU", rc) < 0) goto fail;
    rc = allocate(&s->shared, memory.cpuGpuMemorySize, 0, 0x4000);
    if (stage(v, "RESERVAR COMPARTIDA", rc) < 0) goto fail;
    memory.pCpuMemory = s->cpu.p; memory.pGpuMemory = s->gpu.p; memory.pCpuGpuMemory = s->shared.p;
    s->frame_size = memory.maxFrameBufferSize;
    for (unsigned i = 0; i < RING; ++i) {
#if X4_VIDEO_PROBE
        rc = allocate(&s->output[i], s->frame_size, probe ? output_memory_type : 3, memory.frameBufferAlignment);
#else
        rc = allocate(&s->output[i], s->frame_size, 3, memory.frameBufferAlignment);
#endif
        if (stage(v, "RESERVAR IMAGEN", rc) < 0) goto fail;
    }
    /* The ordinary path keeps WC output; the probe selects only its output buffers.
     * CPU color conversion reads
     * one persistent cached heap copy bounded by the validated output size. */
    s->staging_size = s->frame_size;
    if (s->staging_size > SIZE_MAX - 63) { rc = stage(v, "LIMITES NV12 CPU", -6); goto fail; }
#if X4_VIDEO_PROBE
    if (!raw_pipeline) {
#endif
    s->staging_allocation = malloc(s->staging_size + 63);
    if (!s->staging_allocation) { rc = stage(v, "IMAGEN NV12 CPU", -5); goto fail; }
    s->staging = (uint8_t *)(((uintptr_t)s->staging_allocation + 63) & ~(uintptr_t)63);
#if X4_VIDEO_PROBE
    if (probe) v->probe_status.staging_bytes = s->staging_size + 63;
#endif
#if X4_VIDEO_PROBE
    }
#endif
    unsigned eax, ebx, ecx, edx;
    s->streaming_copy = __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 19));
    printf("XCloud4: video NV12 copy mode=%u capacity=%zu source_alignment=16\n",
        s->streaming_copy ? 1u : 0u, s->staging_size);
    v->pixels = pixels;
    printf("XCloud4: video RGB convert mode=2 block_pixels=8 scalar_tail_max=7\n");
    rc = s->CreateDecoder(&config, &memory, &s->decoder);
#if X4_VIDEO_PROBE
    if (probe) v->probe_status.create_result = rc;
#endif
    if (stage(v, "CREAR DECODER", rc) < 0) goto fail;
#if X4_VIDEO_PROBE
    if (raw_pipeline) {
        v->probe_status.staging_bytes = 0;
        v->probe_status.rgb_bytes = s->capacity.rgb_bytes; /* reference scratch only */
        stage(v, "DECODER SONDA DIRECTO", 0); return 0;
    }
#endif
    /* Persistent helpers receive only bounded CPU-copy jobs. Their worker
     * routine performs no decoder, RGB publication or native teardown calls. */
    static const char *const names[COPY_HELPERS] = {"x4-copy0", "x4-copy1", "x4-copy2"};
    for (unsigned i = 0; i < COPY_HELPERS; ++i) {
        CopyHelper *helper = &s->helpers[i];
#if X4_VIDEO_PROBE
        int helper_rc;
        if (probe) {
            helper_rc = x4_probe_thread_create(&helper->thread, &helper->probe_thread, copy_worker, helper, names[i]);
            helper->running = helper->probe_thread.created;
            if (!helper->running) s->stack_reports[i] = helper->probe_thread.report;
            if (helper_rc) { rc = stage(v, "PILA LECTOR SONDA", helper_rc < 0 ? helper_rc : -helper_rc); goto fail; }
        } else {
            helper_rc = scePthreadCreate(&helper->thread, NULL, copy_worker, helper, names[i]);
            helper->running = helper_rc == 0;
        }
#elif X4_STREAM_PROFILE
        int helper_rc=x4_probe_thread_create(&helper->thread,&helper->probe_thread,copy_worker,helper,names[i]);
        helper->running=helper->probe_thread.created;
        if(helper_rc){rc=stage(v,"PILA LECTOR",helper_rc<0?helper_rc:-helper_rc);goto fail;}
#else
        int helper_rc = scePthreadCreate(&helper->thread, NULL, copy_worker, helper, names[i]);
        helper->running = helper_rc == 0;
#endif
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

int x4_live_video_start(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity)
{
    return start_configured(v, pixels, pixel_capacity, NULL, NULL
#if X4_VIDEO_PROBE
        , NULL, 3, NULL, false
#endif
        );
}
#if X4_VIDEO_PROBE
int x4_live_video_probe_start(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity,
    const X4ProbeCase *probe, X4ProbeHashContext *hash_context, const atomic_bool *cancel)
{
    return x4_live_video_probe_start_memory(v, pixels, pixel_capacity, probe, hash_context, cancel, 3, NULL);
}
int x4_live_video_probe_start_memory(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity,
    const X4ProbeCase *probe, X4ProbeHashContext *hash_context, const atomic_bool *cancel,
    int output_memory_type, X4ProbeMetrics *metrics)
{
    if (!probe || !cancel || !hash_context || !hash_context->buffer ||
        hash_context->capacity != X4_PROBE_READ_CHUNK ||
        (output_memory_type != 0 && output_memory_type != 3)) return -7;
    return start_configured(v, pixels, pixel_capacity, probe, cancel, hash_context, output_memory_type, metrics, false);
}
#endif

int x4_live_video_set_target(X4LiveVideo *v, uint32_t *pixels, size_t pixel_capacity)
{
    if (!v || !v->state || !pixels || pixel_capacity < (size_t)X4_LIVE_RGB_PIXELS) return -7;
    v->pixels = pixels;
    ((VideoState *)v->state)->rgb_pixel_capacity = pixel_capacity;
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
    const X4VideoNv12Layout *layout)
{
    unsigned width = layout->visible_width, height = layout->visible_height, pitch = layout->pitch;
    const __m128i zero = _mm_setzero_si128();
    const __m128i ybias = _mm_set1_epi16(16);
    const __m128i uvbias = _mm_set1_epi16(128);
    const __m128i rounding = _mm_set1_epi32(128);
    const __m128i rcoeff = _mm_setr_epi16(298,409,298,409,298,409,298,409);
    const __m128i bcoeff = _mm_setr_epi16(298,516,298,516,298,516,298,516);
    const __m128i gcoeff = _mm_setr_epi16(-100,-208,-100,-208,-100,-208,-100,-208);
    const __m128i ycoeff = _mm_set1_epi32(298);
    const __m128i alpha = _mm_set1_epi8((char)-1);
    const uint8_t *uvplane = nv12 + layout->visible_uv_offset;
    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *yrow = nv12 + layout->visible_y_offset + (size_t)y * pitch;
        const uint8_t *uvrow = uvplane + (size_t)(y / 2) * pitch;
        uint32_t *out = pixels + (size_t)y * width;
        unsigned x = 0;
        /* MOVQ loads exactly eight bytes within the visible row; the layout
         * rejects odd pitch. Stores write eight pixels without row padding. */
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
#if X4_VIDEO_PROBE
            uint64_t loop_begin = sceKernelGetProcessTime();
#endif
            if (!result) copy_span(job.destination, job.source, job.length, job.streaming);
#if X4_VIDEO_PROBE
            helper->copy_loop_us = sceKernelGetProcessTime() - loop_begin;
#endif
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
#if X4_VIDEO_PROBE
    uint64_t owner_loop_us = 0, completion_wait_us = 0;
#endif
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
            
#if X4_VIDEO_PROBE
            uint64_t loop_begin = sceKernelGetProcessTime();
#endif
            copy_span(jobs[0].destination, jobs[0].source, jobs[0].length, jobs[0].streaming);
#if X4_VIDEO_PROBE
            owner_loop_us = sceKernelGetProcessTime() - loop_begin;
#endif
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
#if X4_VIDEO_PROBE
        completion_wait_us = wait_us;
#endif
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
        
#if X4_VIDEO_PROBE
            uint64_t loop_begin = sceKernelGetProcessTime();
#endif
            copy_span(s->staging, source, length, s->streaming_copy);
#if X4_VIDEO_PROBE
            owner_loop_us = sceKernelGetProcessTime() - loop_begin;
#endif
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
#if X4_VIDEO_PROBE
    if (s->probe && s->probe_metrics) {
        /* Immediate copy-span timers exclude trace calls, dispatch and waits.
         * They are wall spans (may include preemption), not CPU cycle times.
         * The max of overlapping readers is NOT their sum or whole-copy wall. */
        uint64_t now = sceKernelGetProcessTime(), maximum = owner_loop_us;
        x4_probe_metrics_record_duration(s->probe_metrics, X4_PROBE_STAGE_COPY_OWNER_LOOP, now, owner_loop_us);
        if (parallel) for (unsigned i = 0; i < COPY_HELPERS; ++i) {
            uint64_t duration = s->helpers[i].copy_loop_us; /* completion acquire above */
            if (duration > maximum) maximum = duration;
            x4_probe_metrics_record_duration(s->probe_metrics,
                (X4ProbeStage)(X4_PROBE_STAGE_COPY_HELPER0_LOOP + i), now, duration);
        }
        x4_probe_metrics_record_duration(s->probe_metrics, X4_PROBE_STAGE_COPY_READERS_MAX_LOOP, now, maximum);
        x4_probe_metrics_record_duration(s->probe_metrics, X4_PROBE_STAGE_COPY_COMPLETION_WAIT, now, completion_wait_us);
    }
#endif
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

static int validate_picture(VideoState *s, const OrbisVideodec2OutputInfo *o, unsigned *reason, X4VideoNv12Layout *layout)
{
    *reason = 0;
    if (o->isErrorFrame) { *reason = 1; return -7; }
    if (!o->isValid) return 0;
#if X4_VIDEO_PROBE
    if (s->probe) {
        if (o->codecType != 1) { *reason = 6; return -7; }
        if (o->pictureCount != 1) { *reason = 5; return -7; }
        if (!o->pFrameBuffer || ((uintptr_t)o->pFrameBuffer & 15)) { *reason = 7; return -7; }
        size_t owned_bytes = 0;
        for (unsigned i = 0; i < RING; ++i)
            if (o->pFrameBuffer == s->output[i].p) owned_bytes = s->output[i].size;
        if (!owned_bytes) { *reason = 4; return -7; }
        X4VideoNv12Layout candidates[2];
        int count = x4_probe_layout_candidates(s->probe, o->frameWidth, o->frameHeight,
            o->framePitch, o->frameBufferSize, owned_bytes, s->staging_size,
            s->rgb_pixel_capacity, &s->capacity, candidates);
        if (count <= 0) { *reason = 3; return -7; }
        *layout = candidates[0];
        return 1;
    }
#endif
    if (o->codecType != 1 || !o->frameWidth || o->frameWidth > X4_LIVE_WIDTH || (o->frameWidth & 1) ||
        !o->frameHeight || o->frameHeight > X4_LIVE_HEIGHT || (o->frameHeight & 1) || o->framePitch < o->frameWidth || o->framePitch > 8192) { *reason = 2; return -7; }
    /* The proven 48-byte ABI does not expose format/crop/storage-height. Do
     * not promote the prepared allocation capacity into native admission. */
    X4VideoNv12Request request = {
        .visible_width = o->frameWidth, .visible_height = o->frameHeight,
        .coded_width = o->frameWidth, .coded_height = o->frameHeight,
        .storage_height = o->frameHeight, .pitch = o->framePitch,
        .format = X4_VIDEO_FORMAT_NV12, .mapping = X4_VIDEO_MAPPING_CURRENT_UNCROPPED,
        .source_bytes = o->frameBufferSize, .staging_bytes = s->staging_size
    };
    if (x4_video_nv12_layout(&request, &s->capacity, s->rgb_pixel_capacity, layout)) { *reason = 3; return -7; }
    size_t length = layout->total_nv12_bytes;
    if (length > o->frameBufferSize || length > s->staging_size || !s->staging || ((uintptr_t)o->pFrameBuffer & 15)) { *reason = 3; return -7; }
    int owned = 0;
    for (unsigned i = 0; i < RING; ++i)
        if (o->pFrameBuffer == s->output[i].p && length <= s->output[i].size) owned = 1;
    if (!owned) { *reason = 4; return -7; }
    return 1;
}

#if X4_VIDEO_PROBE
static void probe_output_rejected(X4LiveVideo *v, unsigned reason)
{
    v->probe_status.output_reject_reason = reason;
    /* Decode error-frame/ownership/address rejection is distinct from the
     * previously established NV12 mapping. Pipeline_result still rejects
     * the case; no new output is published. Unknown picture metadata makes
     * the current interpretation indeterminate, including during Flush. */
    if (reason == 5) v->probe_status.layout_verdict = X4_PROBE_LAYOUT_INDETERMINATE;
    else if (reason == 2 || reason == 3 || reason == 6)
        v->probe_status.layout_verdict = X4_PROBE_LAYOUT_FAILED;
}
static bool same_probe_mapping(const X4VideoNv12Layout *a, const X4VideoNv12Layout *b)
{
    return a->pitch && b->pitch && a->visible_width == b->visible_width &&
        a->visible_height == b->visible_height && a->coded_width == b->coded_width &&
        a->coded_height == b->coded_height && a->storage_height == b->storage_height &&
        a->visible_y_offset / a->pitch == b->visible_y_offset / b->pitch &&
        a->visible_y_offset % a->pitch == b->visible_y_offset % b->pitch;
}
#endif
int x4_live_video_convert_pending(X4LiveVideo *v)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->pending_valid) return 0;
    if (v->error) return v->error;
    const OrbisVideodec2OutputInfo *o = &s->pending;
    uint64_t output_id = s->pending_output;
    const X4VideoNv12Layout *layout = &s->pending_layout;
#if X4_VIDEO_PROBE
    if (s->probe) {
        if (s->probe_layout_successes && (o->frameWidth != s->probe_native_width ||
            o->frameHeight != s->probe_native_height || o->framePitch != s->probe_native_pitch)) {
            /* The ABI has no plane/crop format declaration. A different
             * observed geometry invalidates this fixture's established
             * interpretation; bounded old arithmetic is not new evidence. */
            v->probe_status.layout_verdict = X4_PROBE_LAYOUT_INDETERMINATE;
            return stage(v, "METADATOS LAYOUT SONDA CAMBIARON", -7);
        }
        size_t owned_bytes = 0;
        for (unsigned i = 0; i < RING; ++i)
            if (o->pFrameBuffer == s->output[i].p) owned_bytes = s->output[i].size;
        X4VideoNv12Layout candidates[2];
        int count = x4_probe_layout_candidates(s->probe, o->frameWidth, o->frameHeight,
            o->framePitch, o->frameBufferSize, owned_bytes, s->staging_size,
            s->rgb_pixel_capacity, &s->capacity, candidates);
        if (count <= 0) return stage(v, "LAYOUT SONDA FUERA DE LIMITES", -7);
        if (v->probe_status.layout_verdict != X4_PROBE_LAYOUT_VERIFIED) {
            unsigned accepted = 0, chosen = 0;
            for (int i = 0; i < count; ++i) {
                int rc = 0;
                const uint8_t *reference_source = o->pFrameBuffer;
                if (!s->raw_pipeline) {
                    rc = copy_picture(v, s, o->pFrameBuffer, candidates[i].total_nv12_bytes);
                    if (rc < 0) return rc;
                    reference_source = s->staging;
                }
                convert_nv12_sse2(v->pixels, reference_source, &candidates[i]);
                X4ProbeReferenceScore score = {0};
                rc = x4_probe_reference_match(&s->reference, v->pixels, s->rgb_pixel_capacity,
                    s->probe->visible_width, s->probe->visible_height, &score);
                if (rc) return stage(v, "VALIDAR REFERENCIA SONDA", rc);
                printf("XCloud4 probe: output=%llu hypothesis=%d native=%ux%u pitch=%u storage=%u uv=%zu visible_y_offset=%zu returned_bytes=%zu owned_bytes=%zu copy_span=%zu matches=%u best=%u mae=%.6f max=%u alpha_fail=%llu identity_ambiguous=%u\n",
                    (unsigned long long)output_id, i, o->frameWidth, o->frameHeight, o->framePitch,
                    candidates[i].storage_height, candidates[i].uv_plane_offset,
                    candidates[i].visible_y_offset, o->frameBufferSize, owned_bytes,
                    candidates[i].total_nv12_bytes, score.accepted_references, score.best_reference_index,
                    score.mean_abs, score.max_channel_delta, (unsigned long long)score.alpha_failures,
                    score.frame_identity_ambiguous ? 1u : 0u);
                if (s->raw_pipeline && s->probe_layout_successes < 3 && i < 2) {
                    unsigned row = s->probe_layout_successes;
                    v->probe_raw_scores[row][i] = score;
                    v->probe_raw_score_counts[row] = (unsigned)count;
                }
                ++v->probe_status.layout_checks;
                if (score.accepted_references) { ++accepted; chosen = (unsigned)i; }
            }
            if (accepted != 1) {
                v->probe_status.layout_verdict = accepted ? X4_PROBE_LAYOUT_INDETERMINATE : X4_PROBE_LAYOUT_FAILED;
                v->probe_status.layout_ambiguous += accepted > 1;
                return stage(v, accepted ? "LAYOUT SONDA INDETERMINADO" : "LAYOUT SONDA NO COINCIDE", -7);
            }
            ++v->probe_status.layout_reference_matches;
            unsigned storage = candidates[chosen].storage_height;
            if (s->probe_layout_successes && !same_probe_mapping(&candidates[chosen], &s->probe_layout)) {
                v->probe_status.layout_verdict = X4_PROBE_LAYOUT_INDETERMINATE;
                return stage(v, "LAYOUT SONDA CAMBIO", -7);
            }
            s->probe_layout_storage = storage;
            s->probe_layout = candidates[chosen];
            s->probe_native_width = o->frameWidth; s->probe_native_height = o->frameHeight;
            s->probe_native_pitch = o->framePitch;
            v->probe_status.selected_storage_height = storage;
            v->probe_status.selected_crop_y = (unsigned)(candidates[chosen].visible_y_offset / o->framePitch);
            if (++s->probe_layout_successes < 3) { s->pending_valid = false; return 0; }
            v->probe_status.layout_verdict = X4_PROBE_LAYOUT_VERIFIED;
            s->pending_layout = candidates[chosen];
            /* The last trial may be the rejected hypothesis. Convert the
             * uniquely validated candidate again before any publication. */
        } else {
            int selected = -1;
            for (int i = 0; i < count; ++i)
                if (same_probe_mapping(&candidates[i], &s->probe_layout)) selected = i;
            if (selected < 0) {
                v->probe_status.layout_verdict = X4_PROBE_LAYOUT_INDETERMINATE;
                return stage(v, "LAYOUT SONDA CAMBIO", -7);
            }
            s->pending_layout = candidates[selected];
        }
        layout = &s->pending_layout;
        if (s->raw_pipeline) { s->pending_valid = false; return 0; }
    }
#endif
    size_t length = layout->total_nv12_bytes;
    int rc = copy_picture(v, s, o->pFrameBuffer, length);
    if (rc < 0) return rc;
    uint64_t begin = sceKernelGetProcessTime();
    x4_trace_record(v->trace, VT_CONVERT_BEGIN, 0, output_id, 0);
    convert_nv12_sse2(v->pixels, s->staging, layout);
    s->pending_valid = false;
    v->width = layout->visible_width; v->height = layout->visible_height;
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
#if X4_VIDEO_PROBE
    if (s->probe) v->probe_status.decode_result = rc;
#endif
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
    X4VideoNv12Layout layout = {0};
    rc = validate_picture(s, &output, &reason, &layout);
    if (rc < 0) {
#if X4_VIDEO_PROBE
        if (s->probe) probe_output_rejected(v, reason);
#endif
        x4_trace_record(v->trace, VT_OUTPUT_INVALID, (uint16_t)reason, call_id, v->trace_decoder_epoch);
        return stage(v, "FORMATO DE IMAGEN", rc);
    }
    if (rc > 0) {
        uint64_t output_id = ++v->trace_output_next;
        if (s->pending_valid) x4_trace_record(v->trace, VT_OUTPUT_SUPERSEDE, 0, s->pending_output, output_id);
        s->pending = output; s->pending_layout = layout; s->pending_valid = true; s->pending_output = output_id; ++v->decoded_frames;
#if X4_VIDEO_PROBE
        if (s->probe) ++v->probe_status.native_outputs;
#endif
        /* This call observed the picture. Output ABI contains no returned PTS;
         * it is not proof that the call's current AU produced this picture. */
        x4_trace_record(v->trace, VT_OUTPUT, X4_TRACE_F_OBSERVED_METADATA, output_id, v->trace_decoder_epoch);
        x4_trace_record(v->trace, VT_GEOMETRY, 0, output_id,
            ((uint64_t)output.framePitch << 32) | ((uint64_t)output.frameHeight << 16) | output.frameWidth);
#if X4_VIDEO_PROBE
        /* Validate the first three leased outputs against known references,
         * before batching can move beyond the eight warmup reference frames. */
        if (s->probe && v->probe_status.layout_verdict != X4_PROBE_LAYOUT_VERIFIED)
            return x4_live_video_convert_pending(v);
#endif
    }
    else { ++v->no_picture_calls; x4_trace_record(v->trace, VT_NOFRAME, 0, call_id, v->trace_decoder_epoch); }
    return 0;
}

#if X4_VIDEO_PROBE
int x4_live_video_probe_flush(X4LiveVideo *v)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->probe || !s->decoder || v->error) return -7;
    if (atomic_load_explicit(s->probe_cancel, memory_order_acquire)) return X4_PROBE_CANCELLED;
    Memory *m = &s->output[s->slot];
    if (s->pending_valid && s->pending.pFrameBuffer == m->p) {
        int preserve = x4_live_video_convert_pending(v);
        if (preserve || s->pending_valid) return preserve ? preserve : -7;
    }
    OrbisVideodec2FrameBuffer frame = {.thisSize=sizeof(frame), .pFrameBuffer=m->p, .frameBufferSize=s->frame_size};
    OrbisVideodec2OutputInfo output = {.thisSize=sizeof(output)};
    uint64_t id = ++v->trace_decode_next, begin = sceKernelGetProcessTime();
    x4_trace_record(v->trace, VT_DECODE_BEGIN, 8, id, 0);
    int rc = s->Flush(s->decoder, &frame, &output);
    uint64_t elapsed = sceKernelGetProcessTime() - begin;
    x4_trace_record(v->trace, VT_DECODE_END, 8 | (output.isValid ? 2 : 0), id, (uint32_t)rc);
    v->probe_status.flush_result = rc;
    ++v->decode_calls; v->decode_us += elapsed;
    if (elapsed > v->decode_max_us) v->decode_max_us = elapsed;
    if (rc < 0) return stage(v, "DRENAR SONDA H264", rc);
    if (frame.isAccepted) s->slot = (s->slot + 1) % RING;
    unsigned reason = 0; X4VideoNv12Layout layout = {0};
    int valid = validate_picture(s, &output, &reason, &layout);
    if (valid < 0) { probe_output_rejected(v, reason); return stage(v, "FORMATO DRENAJE SONDA", valid); }
    if (!valid) { ++v->no_picture_calls; x4_trace_record(v->trace, VT_NOFRAME, 8, id, 0); return 0; }
    uint64_t output_id = ++v->trace_output_next;
    if (s->pending_valid) x4_trace_record(v->trace, VT_OUTPUT_SUPERSEDE, 0, s->pending_output, output_id);
    s->pending = output; s->pending_layout = layout; s->pending_valid = true; s->pending_output = output_id;
    ++v->decoded_frames;
    ++v->probe_status.native_outputs;
    x4_trace_record(v->trace, VT_OUTPUT, X4_TRACE_F_OBSERVED_METADATA, output_id, v->trace_decoder_epoch);
    if (v->probe_status.layout_verdict != X4_PROBE_LAYOUT_VERIFIED) {
        rc = x4_live_video_convert_pending(v);
        if (rc < 0) return rc;
    }
    return 1;
}
#endif

#if X4_VIDEO_PROBE
int x4_live_video_probe_raw_start(X4LiveVideo *v, uint32_t *reference_pixels,
    size_t pixel_capacity, const X4ProbeCase *c, X4ProbeHashContext *hash,
    const atomic_bool *cancel)
{
    if (!c || !hash || !hash->buffer || !cancel) return -7;
    return start_configured(v, reference_pixels, pixel_capacity, c, cancel, hash, 0, NULL, true);
}
int x4_live_video_probe_raw_decode(X4LiveVideo *v, unsigned offered_slot,
    const uint8_t *bytes, size_t size, uint64_t pts, bool flush,
    X4ProbeNativeOutput *out)
{
    VideoState *s = v ? v->state : NULL;
    if (!s || !s->raw_pipeline || !s->decoder || v->error || !out || offered_slot >= RING ||
        (!flush && (!bytes || !size || size > X4_LIVE_AU_MAX))) return -7;
    if (atomic_load_explicit(s->probe_cancel, memory_order_acquire)) return X4_PROBE_CANCELLED;
    memset(out, 0, sizeof(*out)); out->slot = RING;
    Memory *m = &s->output[offered_slot];
    OrbisVideodec2FrameBuffer frame = {.thisSize=sizeof(frame), .pFrameBuffer=m->p, .frameBufferSize=s->frame_size};
    OrbisVideodec2OutputInfo native = {.thisSize=sizeof(native)};
    OrbisVideodec2InputData input = {.thisSize=sizeof(input), .ptsData=pts, .dtsData=pts};
    if (!flush) {
        Memory *compressed = &s->input[s->input_slot];
        memcpy(compressed->p, bytes, size); input.pAuData=compressed->p; input.auSize=size;
    }
    uint64_t begin=sceKernelGetProcessTime();
    int rc=flush ? s->Flush(s->decoder,&frame,&native) : s->Decode(s->decoder,&input,&frame,&native);
    out->decode_end_us=sceKernelGetProcessTime(); out->decode_us=out->decode_end_us-begin;
    out->accepted=frame.isAccepted; out->native_result=rc;
    if (flush) v->probe_status.flush_result=rc; else v->probe_status.decode_result=rc;
    ++v->decode_calls; v->decode_us+=out->decode_us;
    if (out->decode_us>v->decode_max_us) v->decode_max_us=out->decode_us;
    if(rc) return stage(v,"DECODER DIRECTO",rc<0?rc:-7);
    if(!flush) s->input_slot=(s->input_slot+1)%INPUT_RING;
    unsigned reason=0; X4VideoNv12Layout layout={0};
    int valid=validate_picture(s,&native,&reason,&layout);
    if(valid<0){probe_output_rejected(v,reason);return stage(v,"LAYOUT DIRECTO",valid);}
    if(!valid){++v->no_picture_calls;return 0;}
    for(unsigned i=0;i<RING;++i) if(native.pFrameBuffer==s->output[i].p){out->slot=i;out->owned_bytes=s->output[i].size;}
    if(out->slot>=RING) return stage(v,"PROPIEDAD DIRECTA",-7);
    s->pending=native; s->pending_layout=layout; s->pending_valid=true;
    s->pending_output=++v->trace_output_next; ++v->decoded_frames; ++v->probe_status.native_outputs;
    rc=x4_live_video_convert_pending(v); /* references only, never staged copy */
    s->pending_valid=false;
    if(rc)return rc;
    out->valid=true; out->output_id=s->pending_output; out->pixels=native.pFrameBuffer;
    out->layout=s->probe_layout; out->native_width=native.frameWidth; out->native_height=native.frameHeight;
    out->pitch=native.framePitch; out->returned_bytes=native.frameBufferSize;
    out->layout_verified=v->probe_status.layout_verdict==X4_PROBE_LAYOUT_VERIFIED;
    return 0;
}
#endif

#if X4_STREAM_PROFILE
#include "stream_cpu.h"
/* Depth-two extends only this route's allocation ceiling. The sum below is
 * checked BEFORE allocations; each aligned native block remains bounded by
 * that native remainder. Depth one keeps its original 128 MiB native policy. */
static int stream_allocate(VideoState *s,Memory *m,size_t size,int type,size_t align)
{
    if(s->stream_depth.depth!=2)return allocate(m,size,type,align);
    const size_t limit=192u*1024u*1024u-3u*0x800000u-8u*1024u*1024u;
    if(!size)return 0;
    if(size>limit||align>0x200000||(align&&(align&(align-1))))return -2;
    if(align<0x4000)align=0x4000;
    if(x4_video_size_align(size,align,&m->size)||m->size>limit)return -2;
    int rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),m->size,align,type,&m->offset);
    if(rc)return rc<0?rc:-rc;
    m->allocated=1;
    rc=sceKernelMapDirectMemory(&m->p,m->size,0x33,0,m->offset,align);
    if(rc){m->p=NULL;return rc<0?rc:-rc;}
    memset(m->p,0,m->size);return 0;
}
/* Separate SPS-driven raw route. The legacy/probe configuration above stays
 * intact; Query/Create share each attempt's exact configuration and depth. */
static int stream_start_attempt(X4LiveVideo *v,const X4StreamSps *parsed,
    const atomic_bool *cancel,X4StreamDecoderAttempt *a,bool *retryable
#if X4_VIDEO_PROBE && X4_PROBE_T26
    ,const X4T26NativeOptions *t26
#endif
    )
{
    *retryable=false;
    a->compute_query=a->allocate_queue=a->query=a->create=a->cleanup=INT32_MIN;
    v->stream_info.compute_query_result=v->stream_info.allocate_queue_result=
        v->stream_info.query_result=v->stream_info.create_result=
        v->stream_info.delete_decoder_result=v->stream_info.release_queue_result=
        v->stream_info.close_result=INT32_MIN;
    v->stream_info.planned_native_bytes=0;v->stream_info.queried_compute_bytes=0;
    v->stream_info.queried_cpu_bytes=v->stream_info.queried_gpu_bytes=0;
    v->stream_info.queried_shared_bytes=v->stream_info.queried_frame_bytes=0;
    VideoState *s=calloc(1,sizeof(*s));if(!s)return stage(v,"MEMORIA DIRECTA",-5);
    v->state=s;s->stream_raw=true;s->stream_cancel=cancel;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26){s->t26=true;s->t26_options=*t26;}
#endif
    int rc=x4_decoder_depth_init(&s->stream_depth,a->depth);if(rc)return stage(v,"PROFUNDIDAD STREAM",rc);
    v->stream_info.input_count=s->stream_depth.input_count;
    v->stream_info.output_count=s->stream_depth.output_count;
    v->stream_info.active_depth=a->depth;
    v->stream_info.input_lifetime_assumption=a->depth==2;
    v->stream_info.native_budget_bytes=(a->depth==2?192u:128u)*1024u*1024u;
    v->stream_info.budget_auxiliary_bytes=a->depth==2?3u*0x800000u+8u*1024u*1024u:0;
    rc=x4_video_capacity_plan(1,&s->capacity);if(rc)return stage(v,"CAPACIDAD STREAM",rc);
    int handle=x4_module_open("libSceVideodec2");if(handle<0)return stage(v,"MODULO DECODER",handle);
#define STREAM_SYMBOL(field) do {rc=x4_module_symbol(handle,"sceVideodec2" #field,(void**)&s->field);if(rc<0)return stage(v,"SIMBOLO DECODER",rc);}while(0)
    STREAM_SYMBOL(QueryComputeMemoryInfo);STREAM_SYMBOL(AllocateComputeQueue);STREAM_SYMBOL(ReleaseComputeQueue);
    STREAM_SYMBOL(QueryDecoderMemoryInfo);STREAM_SYMBOL(CreateDecoder);STREAM_SYMBOL(DeleteDecoder);STREAM_SYMBOL(Decode);STREAM_SYMBOL(Flush);
#undef STREAM_SYMBOL
    OrbisVideodec2ComputeMemoryInfo cm={.thisSize=sizeof(cm)};
#if X4_VIDEO_PROBE && X4_PROBE_T26
    /* Unknown resource types are queried without inventing a compute queue.
     * The same queue-less configuration reaches Create only after Query=0. */
    if(!t26||t26->config.resource_type==1){
    if(t26&&t26->before&&(rc=t26->before(t26->context,"v2.QueryComputeMemoryInfo")))
        return stage(v,"T26 MARCADOR QUERY COMPUTE",rc);
#endif
    rc=s->QueryComputeMemoryInfo(&cm);a->compute_query=v->stream_info.compute_query_result=rc;
    a->compute_bytes=v->stream_info.queried_compute_bytes=cm.cpuGpuMemorySize;if(rc)return stage(v,"QUERY COMPUTE",rc<0?rc:-rc);
    if(cm.cpuGpuMemorySize>MAX_MEMORY){*retryable=true;a->failure_reason=X4_STREAM_POLICY_QUERY_BUDGET;return stage(v,"PRESUPUESTO COMPUTE",-6);}
    rc=stream_allocate(s,&s->compute,cm.cpuGpuMemorySize,0,0x4000);if(rc<0)return stage(v,"MEMORIA COMPUTE",rc);
    cm.pCpuGpuMemory=s->compute.p;
    OrbisVideodec2ComputeConfigInfo cc={.thisSize=sizeof(cc),.computePipeId=0,.computeQueueId=0};
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26){cc.computePipeId=(uint16_t)t26->config.pipe_id;cc.computeQueueId=(uint16_t)t26->config.queue_id;}
    if(t26&&t26->before&&(rc=t26->before(t26->context,"v2.AllocateComputeQueue")))
        return stage(v,"T26 MARCADOR COLA",rc);
#endif
    rc=s->AllocateComputeQueue(&cc,&cm,&s->queue);a->allocate_queue=v->stream_info.allocate_queue_result=rc;if(rc)return stage(v,"COLA COMPUTE",rc<0?rc:-rc);
#if X4_VIDEO_PROBE && X4_PROBE_T26
    }
#endif
    X4StreamCpuConfig cpu=x4_stream_cpu_config(v->stream_info.cpu_variant);
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26){cpu.decoder_priority=t26->config.priority;cpu.decoder_affinity=t26->config.affinity_mask;}
#endif
    v->stream_info.decoder_cpu_priority=cpu.decoder_priority;
    v->stream_info.decoder_cpu_affinity=cpu.decoder_affinity;
    OrbisVideodec2DecoderConfigInfo cfg={.thisSize=sizeof(cfg),.resourceType=1,.codecType=1,
        .profile=a->profile,.maxLevel=a->level,
        .maxFrameWidth=(int)a->width,.maxFrameHeight=(int)a->height,
        .maxDpbFrameCount=(int)a->dpb,.decodePipelineDepth=(int)a->depth,
        .computeQueue=s->queue,.cpuAffinityMask=cpu.decoder_affinity,.cpuThreadPriority=cpu.decoder_priority,
        .optimizeProgressiveVideo=true,.checkMemoryType=false};
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26)cfg.resourceType=t26->config.resource_type;
#endif
    OrbisVideodec2DecoderMemoryInfo mem={.thisSize=sizeof(mem)};
    printf("XCloud4: stream CPU-%u native decoder priority=%d affinity=0x%llx (same Query/Create configuration)\n",
        cpu.variant,cpu.decoder_priority,(unsigned long long)cpu.decoder_affinity);
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26&&t26->before&&(rc=t26->before(t26->context,"v2.QueryDecoderMemoryInfo")))
        return stage(v,"T26 MARCADOR QUERY DECODER",rc);
#endif
    rc=s->QueryDecoderMemoryInfo(&cfg,&mem);a->query=v->stream_info.query_result=rc;
    a->cpu_bytes=v->stream_info.queried_cpu_bytes=mem.cpuMemorySize;a->gpu_bytes=v->stream_info.queried_gpu_bytes=mem.gpuMemorySize;
    a->shared_bytes=v->stream_info.queried_shared_bytes=mem.cpuGpuMemorySize;a->frame_bytes=v->stream_info.queried_frame_bytes=mem.maxFrameBufferSize;
    printf("XCloud4: stream Query attempt=%u fallback=%u depth=%u profile=%u level=%u coded=%ux%u dpb=%u rc=%d frame=%zu\n",
        v->stream_info.attempt_count,a->fallback,a->depth,a->profile,a->level,a->width,a->height,a->dpb,rc,mem.maxFrameBufferSize);
    if(rc){*retryable=true;a->failure_reason=X4_STREAM_POLICY_NATIVE;return stage(v,"QUERY DECODER",rc<0?rc:-rc);}
    size_t native_limit=(size_t)(v->stream_info.native_budget_bytes-v->stream_info.budget_auxiliary_bytes);
    size_t block_limit=a->depth==2?native_limit:MAX_MEMORY;
    if(mem.cpuMemorySize>block_limit||mem.gpuMemorySize>block_limit||mem.cpuGpuMemorySize>block_limit||
        !mem.maxFrameBufferSize||mem.maxFrameBufferSize>block_limit/(a->depth==2?5u:4u)){
        *retryable=true;a->failure_reason=X4_STREAM_POLICY_QUERY_BUDGET;return stage(v,"PRESUPUESTO DECODER",-6);
    }
    /* Identical arithmetic as probe raw depth two: 4 persistent compressed
     * inputs + 5 aligned outputs. 5 = depth2 + conversion + READY + offer. */
    X4VideoMemoryBlock blocks[13];unsigned block_count=0;
    for(unsigned i=0;i<s->stream_depth.input_count;++i)
        blocks[block_count++]=(X4VideoMemoryBlock){X4_LIVE_AU_MAX,0x4000};
    blocks[block_count++]=(X4VideoMemoryBlock){cm.cpuGpuMemorySize,0x4000};
    blocks[block_count++]=(X4VideoMemoryBlock){mem.cpuMemorySize,0x4000};
    blocks[block_count++]=(X4VideoMemoryBlock){mem.gpuMemorySize,0x4000};
    blocks[block_count++]=(X4VideoMemoryBlock){mem.cpuGpuMemorySize,0x4000};
    for(unsigned i=0;i<s->stream_depth.output_count;++i)
        blocks[block_count++]=(X4VideoMemoryBlock){mem.maxFrameBufferSize,mem.frameBufferAlignment};
    size_t total=0;rc=x4_video_native_memory_total_with_budget(blocks,block_count,native_limit,&total);
    if(rc==X4_VIDEO_CAPACITY_MEMORY){
        /* Preserve rejected aligned request size for diagnostics only. */
        size_t rejected=0;
        if(!x4_video_native_memory_total_with_budget(blocks,block_count,X4_VIDEO_LEGACY_NATIVE_CEILING,&rejected))total=rejected;
    }
    a->planned_bytes=v->stream_info.planned_native_bytes=total;
    printf("XCloud4: stream native budget depth=%u native=%zu auxiliary=%llu combined=%llu limit=%llu input_retirement_calls=%u input_contract_sdk_verified=0\n",
        a->depth,total,(unsigned long long)v->stream_info.budget_auxiliary_bytes,
        (unsigned long long)(total+v->stream_info.budget_auxiliary_bytes),
        (unsigned long long)v->stream_info.native_budget_bytes,a->depth==2?3u:1u);
    if(rc){*retryable=true;a->failure_reason=X4_STREAM_POLICY_QUERY_BUDGET;return stage(v,"PRESUPUESTO DECODER",rc);}
    Memory *owned[]={&s->cpu,&s->gpu,&s->shared};size_t lengths[]={mem.cpuMemorySize,mem.gpuMemorySize,mem.cpuGpuMemorySize};
    for(unsigned i=0;i<3;++i){rc=stream_allocate(s,owned[i],lengths[i],i==1?3:0,0x4000);if(rc<0)return stage(v,"MEMORIA DECODER",rc);}
    for(unsigned i=0;i<s->stream_depth.input_count;++i){rc=stream_allocate(s,&s->input[i],X4_LIVE_AU_MAX,0,0x4000);if(rc<0)return stage(v,"MEMORIA ENTRADA",rc);}
#if X4_VIDEO_PROBE && X4_PROBE_T26
    for(unsigned i=0;i<s->stream_depth.output_count;++i){
        int output_type=0;
        if(t26)output_type=t26->config.output_memory_type;
        rc=stream_allocate(s,&s->output[i],mem.maxFrameBufferSize,output_type,mem.frameBufferAlignment);if(rc<0)return stage(v,"MEMORIA SALIDA",rc);
    }
#else
    for(unsigned i=0;i<s->stream_depth.output_count;++i){rc=stream_allocate(s,&s->output[i],mem.maxFrameBufferSize,0,mem.frameBufferAlignment);if(rc<0)return stage(v,"MEMORIA SALIDA",rc);}
#endif
    s->frame_size=mem.maxFrameBufferSize;mem.pCpuMemory=s->cpu.p;mem.pGpuMemory=s->gpu.p;mem.pCpuGpuMemory=s->shared.p;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26&&t26->before&&(rc=t26->before(t26->context,"v2.CreateDecoder")))
        return stage(v,"T26 MARCADOR CREATE",rc);
#endif
    rc=s->CreateDecoder(&cfg,&mem,&s->decoder);a->create=v->stream_info.create_result=rc;
    printf("XCloud4: stream Create attempt=%u fallback=%u depth=%u rc=%d\n",v->stream_info.attempt_count,a->fallback,a->depth,rc);
    if(rc){*retryable=true;a->failure_reason=X4_STREAM_POLICY_NATIVE;return stage(v,"CREATE DECODER",rc<0?rc:-rc);}
    if(!s->decoder){
        v->stream_info.creation_uncertain=true;v->stream_info.retained=true;
        return stage(v,"HANDLE DECODER INVALIDO",-7);
    }
    v->width=parsed->visible_width;v->height=parsed->visible_height;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(t26)printf("XCloud4 T26: native v2 depth=%u resource=%u pipe=%u queue=%u priority=%d affinity=0x%llx output_memory=%d query_bytes=%zu\n",
        a->depth,t26->config.resource_type,t26->config.pipe_id,t26->config.queue_id,
        t26->config.priority,(unsigned long long)t26->config.affinity_mask,
        t26->config.output_memory_type,total);
    else
#endif
    printf("XCloud4: stream SPS profile=%u constraints=%u level=%u visible=%ux%u coded=%ux%u dpb=%u native_bytes=%zu output_memory=WB_ONION depth=%u\n",
        parsed->profile_idc,parsed->constraints,parsed->level_idc,parsed->visible_width,parsed->visible_height,
        parsed->coded_width,parsed->coded_height,parsed->max_dec_frame_buffering,total,a->depth);
    return stage(v,"DECODER STREAM DIRECTO",0);
}
static bool stream_attempt_policy(const X4StreamSps *sp,unsigned kind,X4StreamDecoderAttempt *a)
{
    memset(a,0,sizeof(*a));a->kind=kind;a->fallback=kind!=X4_STREAM_ATTEMPT_SPS;
    a->profile=sp->profile_idc;a->level=sp->level_idc;
    a->width=sp->coded_width;a->height=sp->coded_height;
    a->compute_query=a->allocate_queue=a->query=a->create=a->cleanup=INT32_MIN;
    bool is720=sp->visible_width==1280&&sp->visible_height==720;
    bool is1080=sp->visible_width==1920&&sp->visible_height==1080;
    uint64_t refs=(uint64_t)sp->max_num_ref_frames+1;
    a->dpb_minimum=refs>UINT32_MAX?UINT32_MAX:(refs>4?(unsigned)refs:4);
    a->dpb_requested=sp->max_dec_frame_buffering>a->dpb_minimum?sp->max_dec_frame_buffering:a->dpb_minimum;
    if(kind==X4_STREAM_ATTEMPT_GEOMETRY){
        if(!is720&&!is1080){a->skipped=true;a->reason=X4_STREAM_POLICY_INELIGIBLE_GEOMETRY;return false;}
        a->level=is720?32:42;
    }else if(kind==X4_STREAM_ATTEMPT_LEGACY){
        a->profile=66;a->level=31;
        if(!is720){a->skipped=true;a->reason=X4_STREAM_POLICY_INELIGIBLE_GEOMETRY;return false;}
        if(refs>4){a->skipped=true;a->reason=X4_STREAM_POLICY_REF_CONFLICT;return false;}
        a->dpb_requested=4;
    }
    a->dpb_level_cap=x4_stream_sps_level_dpb_capacity(a->level,
        kind==X4_STREAM_ATTEMPT_SPS?sp->constraints:0,a->width,a->height,&a->level_known);
    if(a->dpb_level_cap<a->dpb_minimum){
        a->skipped=true;a->reason=X4_STREAM_POLICY_LEVEL_CONFLICT;return false;
    }
    a->dpb=a->dpb_requested;
    if(a->dpb>a->dpb_level_cap){a->dpb=a->dpb_level_cap;a->reason=X4_STREAM_POLICY_LEVEL_LIMITED;}
    else if(!a->level_known)a->reason=X4_STREAM_POLICY_UNKNOWN_LEVEL;
    else if(a->dpb>sp->max_dec_frame_buffering)a->reason=X4_STREAM_POLICY_FLOOR;
    return true;
}
int x4_live_video_stream_raw_start(X4LiveVideo *v,const uint8_t *sps,size_t size,
    const atomic_bool *cancel)
{ return x4_live_video_stream_raw_start_cpu(v,sps,size,cancel,X4_CPU_0); }
int x4_live_video_stream_raw_start_cpu(X4LiveVideo *v,const uint8_t *sps,size_t size,
    const atomic_bool *cancel,unsigned cpu_variant)
{ return x4_live_video_stream_raw_start_cpu_depth(v,sps,size,cancel,cpu_variant,1); }
int x4_live_video_stream_raw_start_cpu_depth(X4LiveVideo *v,const uint8_t *sps,size_t size,
    const atomic_bool *cancel,unsigned cpu_variant,unsigned depth)
{
    if(!v||!cancel||v->state||cpu_variant>X4_CPU_3||(depth!=1&&depth!=2))return -7;
    /* Native epochs change; session counters and diagnostic ordinals do not.
     * Root snapshots per-epoch stream_info before recreating this decoder. */
    X4LiveVideo previous=*v;memset(v,0,sizeof(*v));
    v->stream_info.cpu_variant=cpu_variant;v->stream_info.requested_depth=depth;
    v->trace=previous.trace;v->trace_au=previous.trace_au;
    v->trace_decode_next=previous.trace_decode_next;v->trace_output_next=previous.trace_output_next;
    v->trace_copy_next=previous.trace_copy_next;v->trace_decoder_epoch=previous.trace_decoder_epoch+1;
    v->decode_calls=previous.decode_calls;v->decoded_frames=previous.decoded_frames;
    v->no_picture_calls=previous.no_picture_calls;v->decode_us=previous.decode_us;
    v->decode_max_us=previous.decode_max_us;v->frames=previous.frames;
    x4_trace_record(v->trace,VT_START,0,v->trace_decoder_epoch,0);
    v->stream_info.compute_query_result=v->stream_info.allocate_queue_result=
        v->stream_info.query_result=v->stream_info.create_result=
        v->stream_info.delete_decoder_result=v->stream_info.release_queue_result=
        v->stream_info.close_result=INT32_MIN;
    int rc=x4_stream_sps_inspect_diagnostic(sps,size,&v->stream_info.sps,&v->stream_info.sps_diagnostics);
    const X4StreamSpsDiagnostics *d=&v->stream_info.sps_diagnostics;
    printf("XCloud4: SPS inspect rc=%d reason=%s stage=%s valid=%llu warnings=%u rbsp_bit=%zu ebsp_byte=%zu reader_error=%u complete=%u\n",
        rc,x4_stream_sps_error_name(rc),x4_stream_sps_stage_name(d->stage),(unsigned long long)d->valid_mask,d->warning_mask,d->rbsp_bit_offset,d->ebsp_byte_offset,d->reader_error,d->syntax_complete);
    const X4StreamSps *observed=&v->stream_info.sps;
    printf("XCloud4: SPS partial (consult valid mask) profile=%u constraints=%u level=%u chroma=%u depth=%u/%u coded=%ux%u visible=%ux%u frame_only=%u crop=%u/%u/%u/%u refs=%u dpb=%u matrix=%u full_range=%u\n",observed->profile_idc,observed->constraints,observed->level_idc,observed->chroma_format_idc,observed->bit_depth_luma,observed->bit_depth_chroma,observed->coded_width,observed->coded_height,observed->visible_width,observed->visible_height,observed->frame_mbs_only_flag,observed->crop_left,observed->crop_top,observed->crop_right,observed->crop_bottom,observed->max_num_ref_frames,observed->max_dec_frame_buffering,observed->matrix_coefficients,observed->full_range);
    if(rc)return stage(v,"SPS NO ADMITIDO",rc);
    int last_error=X4_SPS_LEVEL_DPB;
    unsigned active_depth=depth;
    for(unsigned kind=0;kind<3;++kind){
        bool retry_same_kind;
        do {
            retry_same_kind=false;
            if(atomic_load_explicit(cancel,memory_order_acquire))return X4_STREAM_VIDEO_CANCELLED;
            unsigned attempt=v->stream_info.attempt_count;
            if(attempt>=6)return stage(v,"LIMITE INTENTOS DECODER",-7);
            X4StreamDecoderAttempt *a=&v->stream_info.attempts[attempt];
            v->stream_info.active_attempt=attempt;v->stream_info.attempt_count=attempt+1;
            bool allowed=stream_attempt_policy(&v->stream_info.sps,kind,a);a->depth=active_depth;
            printf("XCloud4: decoder policy attempt=%u kind=%u depth=%u SPS_profile=%u SPS_level=%u SPS_dpb=%u refs=%u effective_profile=%u effective_level=%u dpb=%u requested=%u minimum=%u level_cap=%u known=%u skip=%u reason=%u coded=%ux%u\n",
                attempt+1,a->kind,a->depth,observed->profile_idc,observed->level_idc,observed->max_dec_frame_buffering,
                observed->max_num_ref_frames,a->profile,a->level,a->dpb,a->dpb_requested,a->dpb_minimum,
                a->dpb_level_cap,a->level_known,a->skipped,a->reason,a->width,a->height);
            if(!allowed){snprintf(a->failure_stage,sizeof(a->failure_stage),"OMITIDO POR POLITICA");break;}
            bool retryable=false;v->error=0;
            v->stream_info.fallback_used=kind!=0||active_depth!=depth;
            /* Record an executed depth fallback only after cancellation and
             * local admission checks. A rejected depth-two call may itself
             * publish cancel, so merely making a retry eligible is not use. */
            if(active_depth!=depth)v->stream_info.depth_fallback_used=true;
            rc=stream_start_attempt(v,&v->stream_info.sps,cancel,a,&retryable
#if X4_VIDEO_PROBE && X4_PROBE_T26
                ,NULL
#endif
                );
            if(!rc)return 0;
            snprintf(a->failure_stage,sizeof(a->failure_stage),"%s",v->stage);
            last_error=rc;
            int closed=x4_live_video_stop(v);a->cleanup=closed;
            if(closed||v->state){v->stream_info.close_result=closed?closed:-7;v->stream_info.retained=true;return rc;}
            if(!retryable)return rc;
            /* Rejection of depth two by either native call retries the SAME
             * effective SPS/config once at depth one, after safe cleanup.
             * Existing geometry/legacy attempts then continue at that depth. */
            if(active_depth==2&&((a->query!=INT32_MIN&&a->query!=0)||
                (a->create!=INT32_MIN&&a->create!=0))){
                active_depth=1;retry_same_kind=true;
                printf("XCloud4: depth fallback eligible requested=2 next_depth=1 Query=%d Create=%d cleanup=%d same_policy_kind=%u\n",
                    a->query,a->create,a->cleanup,kind);
            }
        } while(retry_same_kind);
    }
    return stage(v,"CONFIGURACIONES DECODER AGOTADAS",last_error);
}
#if X4_VIDEO_PROBE && X4_PROBE_T26
int x4_live_video_t26_raw_start(X4LiveVideo *v,const uint8_t *sps,size_t bytes,
    const atomic_bool *cancel,const X4T26NativeOptions *options)
{
    if(!v||v->state||!cancel||!options||!x4_t26_config_valid(&options->config)||
        options->config.api!=X4_T26_API_V2)return -7;
    memset(v,0,sizeof(*v));
    v->stream_info.requested_depth=options->config.depth;
    int rc=x4_stream_sps_inspect_diagnostic(sps,bytes,&v->stream_info.sps,
        &v->stream_info.sps_diagnostics);
    if(rc)return stage(v,"T26 SPS",rc);
    const X4StreamSps *sp=&v->stream_info.sps;
    if(sp->profile_idc!=77||sp->level_idc!=42||sp->max_num_ref_frames!=1||
        !sp->frame_mbs_only_flag)return stage(v,"T26 PERFIL DE MUESTRA",-7);
    X4StreamDecoderAttempt *a=&v->stream_info.attempts[0];
    *a=(X4StreamDecoderAttempt){.profile=sp->profile_idc,.level=sp->level_idc,
        .width=sp->coded_width,.height=sp->coded_height,
        /* One reference in the bitstream is not the native DPB allocation
         * ceiling. Preserve the production floor of four for this base. */
        .dpb=4,
        .depth=options->config.depth,.kind=X4_STREAM_ATTEMPT_SPS,
        .dpb_requested=4,.dpb_minimum=sp->max_num_ref_frames};
    v->stream_info.attempt_count=1;
    v->stream_info.active_attempt=0;
    bool ignored_retryable=false;
    /* A measurement never substitutes a different depth, engine, SPS profile
     * or geometry after rejection. Caller still performs safe close. */
    return stream_start_attempt(v,sp,cancel,a,&ignored_retryable,options);
}

unsigned x4_live_video_t26_allocations(const X4LiveVideo *v,
    X4T26Allocation out[X4_T26_ALLOCATION_MAX])
{
    const VideoState *s=v?v->state:NULL;
    if(!s||!s->stream_raw||!s->t26||!out)return 0;
    const Memory *fixed[]={&s->compute,&s->cpu,&s->gpu,&s->shared};
    unsigned count=0;
    for(unsigned i=0;i<4;++i)if(fixed[i]->allocated)
        out[count++]=(X4T26Allocation){fixed[i]->offset,fixed[i]->size,i==2?3:0};
    for(unsigned i=0;i<s->stream_depth.input_count;++i)if(s->input[i].allocated)
        out[count++]=(X4T26Allocation){s->input[i].offset,s->input[i].size,0};
    for(unsigned i=0;i<s->stream_depth.output_count;++i)if(s->output[i].allocated)
        out[count++]=(X4T26Allocation){s->output[i].offset,s->output[i].size,
            s->t26_options.config.output_memory_type};
    return count;
}
#endif
static int stream_output_fault(X4LiveVideo *v,X4ProbeNativeOutput *out,
    const OrbisVideodec2OutputInfo *native,const char *reason,int rc)
{
    out->ownership_uncertain=true;
    out->control_result=rc;
    out->native_width=native->frameWidth;out->native_height=native->frameHeight;
    out->pitch=native->framePitch;
    printf("XCloud4: output rejected reason=%s rc=%d error_frame=%u valid=%u codec=%u pictures=%u native=%ux%u pitch=%u returned=%zu own_slot=%u own_bytes=%zu accepted=%u; Delete required\n",
        reason,rc,native->isErrorFrame,native->isValid,native->codecType,native->pictureCount,
        native->frameWidth,native->frameHeight,native->framePitch,native->frameBufferSize,
        out->slot,out->owned_bytes,out->accepted);
    if(native->isErrorFrame){
        out->frame_class=X4_STREAM_FRAME_RECOVERABLE_RESTART;
        snprintf(v->stage,sizeof(v->stage),"IMAGEN SIN PROPIEDAD RESUELTA");return 0;
    }
    out->frame_class=X4_STREAM_FRAME_FATAL;return stage(v,reason,rc);
}
int x4_live_video_stream_raw_decode(X4LiveVideo *v,unsigned offered,const uint8_t *bytes,size_t size,
    uint64_t pts,bool flush,X4ProbeNativeOutput *out)
{
    VideoState *s=v?v->state:NULL;
    if(out){memset(out,0,sizeof(*out));out->slot=s&&s->stream_raw?s->stream_depth.output_count:RING;
        out->input_slot=INPUT_STORAGE;out->frame_class=X4_STREAM_FRAME_FATAL;}
    if(!s||!s->stream_raw||!s->decoder||v->error||!out||offered>=s->stream_depth.output_count||
        (!flush&&(!bytes||!size||size>X4_LIVE_AU_MAX)))return -7;
    if(atomic_load_explicit(s->stream_cancel,memory_order_acquire))return X4_STREAM_VIDEO_CANCELLED;
    if(!flush){
        int checked=x4_stream_sps_check_au(bytes,size,&v->stream_info.sps);
        if(checked){
            out->frame_class=X4_STREAM_FRAME_RECONFIGURE;out->control_result=checked;
            snprintf(v->stage,sizeof(v->stage),"SPS REQUIERE RECONFIGURACION");
            printf("XCloud4: stream AU SPS control rc=%d before Decode; no native ownership transferred\n",checked);
            return 0;
        }
    }
    bool depth_two=s->stream_depth.depth==2;
    if(depth_two&&s->stream_depth.output_held[offered])return stage(v,"SALIDA AUN RETENIDA POR DECODER",-7);
    out->frame_class=X4_STREAM_FRAME_OK;
    Memory *m=&s->output[offered];
    OrbisVideodec2FrameBuffer frame={.thisSize=sizeof(frame),.pFrameBuffer=m->p,.frameBufferSize=s->frame_size};
    OrbisVideodec2OutputInfo o={.thisSize=sizeof(o)};
    OrbisVideodec2InputData input={.thisSize=sizeof(input),.ptsData=pts,.dtsData=pts};
    if(!flush){
        unsigned input_slot=s->input_slot;
        if(depth_two&&x4_decoder_depth_input_acquire(&s->stream_depth,&input_slot))
            return stage(v,"ENTRADA AUN RETENIDA POR DECODER",-7);
        out->input_slot=input_slot;
        Memory *a=&s->input[input_slot];memcpy(a->p,bytes,size);input.pAuData=a->p;input.auSize=size;
        /* Ported from the validated probe: retain BEFORE Decode regardless
         * of isAccepted, including native errors and calls with no output. */
        if(depth_two&&x4_decoder_depth_output_offer(&s->stream_depth,offered))
            return stage(v,"RESERVAR SALIDA NATIVA",-7);
    }
    uint64_t call=++v->trace_decode_next;
    x4_trace_record(v->trace,VT_DECODE_BEGIN,flush?8:0,call,flush?0:v->trace_au);
    uint64_t begin=sceKernelGetProcessTime();int rc=flush?s->Flush(s->decoder,&frame,&o):s->Decode(s->decoder,&input,&frame,&o);
    out->decode_end_us=sceKernelGetProcessTime();out->decode_us=out->decode_end_us-begin;out->accepted=frame.isAccepted;out->native_result=rc;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    out->t26_native_called=true;
    out->t26_native_valid=(uint32_t)o.isValid;
    out->t26_native_error=(uint32_t)o.isErrorFrame;
    out->t26_picture_count=o.pictureCount;
    out->t26_native_pointer=o.pFrameBuffer;
    out->t26_native_bytes=o.frameBufferSize;
#endif
    if(!flush){
        if(depth_two)x4_decoder_depth_decode_returned(&s->stream_depth);
        else ++s->stream_depth.decode_completed;
    }
    out->native_call=s->stream_depth.decode_completed;
    if(depth_two&&flush&&(frame.isAccepted||rc||(o.isValid&&o.pFrameBuffer==m->p)))
        if(x4_decoder_depth_output_offer(&s->stream_depth,offered))return stage(v,"RESERVAR SALIDA DRENAJE",-7);
    out->offered_retained=depth_two?s->stream_depth.output_held[offered]:frame.isAccepted;
    /* Observed metadata survives errors without claiming a verified layout. */
    out->native_width=o.frameWidth;out->native_height=o.frameHeight;out->pitch=o.framePitch;
    x4_trace_record(v->trace,VT_DECODE_END,(flush?8:0)|(frame.isAccepted?1:0)|(o.isValid?2:0)|(o.isErrorFrame?4:0),call,(uint32_t)rc);
    ++v->decode_calls;v->decode_us+=out->decode_us;if(out->decode_us>v->decode_max_us)v->decode_max_us=out->decode_us;
    if(rc){
        /* Neither SDK signatures nor the linked exports describe error/lease
         * ownership. Unknown native results are not guessed to be fatal
         * bitstream/resource codes. The owner must quiesce and Delete==0
         * before restarting; even isAccepted=false is not proof of release. */
        out->frame_class=X4_STREAM_FRAME_RECOVERABLE_RESTART;
        out->ownership_uncertain=true;
        snprintf(v->stage,sizeof(v->stage),"DECODER REQUIERE REINICIO");
        printf("XCloud4: Decode rc=%d accepted=%u valid=%u error_frame=%u; ownership uncertain, Delete required\n",rc,frame.isAccepted,o.isValid,o.isErrorFrame);
        return 0;
    }
#if X4_VIDEO_PROBE && X4_PROBE_T26
    if(s->t26&&!depth_two&&!flush&&!o.isValid){
        /* Depth1 has no delayed-output lease ledger. A missing picture is
         * not proof that the offered reservation is reusable. Stop this
         * T26 cell before rotating input/offering any buffer again; only a
         * successful Delete can establish release. Keep native rc/flags exact. */
        out->ownership_uncertain=true;out->control_result=-7;
        out->frame_class=X4_STREAM_FRAME_FATAL;++v->no_picture_calls;
        x4_trace_record(v->trace,VT_NOFRAME,0,call,v->trace_decoder_epoch);
        return stage(v,"T26 DEPTH1 SIN SALIDA: DELETE REQUERIDO",out->control_result);
    }
#endif
    if(!flush&&!depth_two)s->input_slot=(s->input_slot+1)%INPUT_RING;
    if(o.isErrorFrame&&!o.isValid){
        return stream_output_fault(v,out,&o,"ERROR FRAME SIN SALIDA VALIDA",-7);
    }
    if(!o.isValid){++v->no_picture_calls;x4_trace_record(v->trace,VT_NOFRAME,flush?8:0,call,v->trace_decoder_epoch);return 0;}
    out->ownership_uncertain=true;
    const X4StreamSps *sp=&v->stream_info.sps;
    if(o.codecType!=1||o.pictureCount!=1||!o.pFrameBuffer||((uintptr_t)o.pFrameBuffer&15))
        return stream_output_fault(v,out,&o,"FORMATO STREAM INVALIDO",-7);
    if(!o.frameWidth||!o.frameHeight||o.frameWidth>1920||o.frameHeight>1088)
        return stream_output_fault(v,out,&o,"GEOMETRIA STREAM FUERA DE LIMITE",X4_SPS_SIZE);
    for(unsigned i=0;i<s->stream_depth.output_count;++i)if(o.pFrameBuffer==s->output[i].p){out->slot=i;out->owned_bytes=s->output[i].size;}
    if(out->slot>=s->stream_depth.output_count||!o.frameBufferSize||o.frameBufferSize>out->owned_bytes)
        return stream_output_fault(v,out,&o,"PROPIEDAD STREAM",-7);
    bool matches_sps=o.frameWidth==sp->coded_width&&o.frameHeight==sp->coded_height;
    unsigned width=matches_sps?sp->visible_width:o.frameWidth;
    unsigned height=matches_sps?sp->visible_height:((o.frameWidth==1920&&o.frameHeight==1088)?1080:o.frameHeight);
    X4VideoNv12Request req={.visible_width=width,.visible_height=height,
        .coded_width=o.frameWidth,.coded_height=o.frameHeight,.storage_height=o.frameHeight,
        .pitch=o.framePitch,.crop_x=matches_sps?sp->crop_left:0,.crop_y=matches_sps?sp->crop_top:0,
        .format=X4_VIDEO_FORMAT_NV12,.mapping=X4_VIDEO_MAPPING_EXPLICIT_NV12,
        .source_bytes=o.frameBufferSize,.staging_bytes=out->owned_bytes};
    X4VideoCapacity capacity=s->capacity;
    /* RGB counts here are checked geometry arithmetic only; conversion writes
     * the fixed 1080 scanout, never an intermediate RGB allocation. */
    capacity.visible_height=1088;
    rc=x4_video_nv12_layout(&req,&capacity,(size_t)1920*1088,&out->layout);
    if(rc)return stream_output_fault(v,out,&o,"LAYOUT STREAM",rc);
    bool standard=(o.frameWidth==1280&&o.frameHeight==720)||
        (o.frameWidth==1920&&(o.frameHeight==1080||o.frameHeight==1088));
    if(!standard||!matches_sps){
        ++v->stream_info.geometry_warnings;
        if(!v->decoded_frames||v->width!=width||v->height!=height)
            printf("XCloud4: geometria alternativa NV12 native=%ux%u pitch=%u visible=%ux%u storage=%u; escalado escalar cuando no sea geometria estandar\n",
                o.frameWidth,o.frameHeight,o.framePitch,width,height,o.frameHeight);
    }
    if(depth_two&&x4_decoder_depth_output_return(&s->stream_depth,out->slot))
        return stream_output_fault(v,out,&o,"SALIDA NO RETENIDA POR DECODER",-7);
    out->offered_retained=depth_two?s->stream_depth.output_held[offered]:frame.isAccepted;
    v->width=width;v->height=height;
    out->completed_output=true;out->ownership_uncertain=false;
    out->valid=!o.isErrorFrame;out->layout_verified=true;out->pixels=o.pFrameBuffer;out->returned_bytes=o.frameBufferSize;
    out->native_width=o.frameWidth;out->native_height=o.frameHeight;out->pitch=o.framePitch;
    if(o.isErrorFrame){
        out->frame_class=X4_STREAM_FRAME_BAD_FRAME;
        snprintf(v->stage,sizeof(v->stage),"IMAGEN DANADA DESCARTADA");
        printf("XCloud4: damaged output slot=%u accepted=%u native=%ux%u pitch=%u; exact completed slot only\n",out->slot,out->accepted,o.frameWidth,o.frameHeight,o.framePitch);
        return 0;
    }
    out->output_id=++v->trace_output_next;++v->decoded_frames;
    x4_trace_record(v->trace,VT_OUTPUT,X4_TRACE_F_OBSERVED_METADATA,out->output_id,v->trace_decoder_epoch);
    x4_trace_record(v->trace,VT_GEOMETRY,0,out->output_id,((uint64_t)o.framePitch<<32)|((uint64_t)o.frameHeight<<16)|o.frameWidth);
    return 0;
}
unsigned x4_live_video_stream_output_held_mask(const X4LiveVideo *v)
{
    const VideoState *s=v?v->state:NULL;unsigned mask=0;
    if(s&&s->stream_raw&&s->stream_depth.depth==2)
        for(unsigned i=0;i<s->stream_depth.output_count;++i)
            if(s->stream_depth.output_held[i])mask|=1u<<i;
    return mask;
}
void x4_live_video_stream_info(const X4LiveVideo *v,X4StreamVideoInfo *out)
{
    if(v&&out){
        *out=v->stream_info;out->retained=v->stream_info.retained||(v->state!=NULL&&v->error!=0);
        const VideoState *s=v->state;
        if(s&&s->stream_raw){
            out->held_output_mask=x4_live_video_stream_output_held_mask(v);
            out->live_input_count=x4_decoder_depth_inputs_live(&s->stream_depth);
        }else{out->held_output_mask=0;out->live_input_count=0;}
    }
}
#endif
