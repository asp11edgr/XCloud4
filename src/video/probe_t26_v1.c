/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_v1.h"
#if defined(X4_VIDEO_PROBE) && X4_VIDEO_PROBE && \
    defined(X4_PROBE_T26) && X4_PROBE_T26

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { CPU_MEMORY = 0, CPU_GPU_MEMORY = 1, INPUT_MEMORY = 2, OUTPUT_MEMORY = 3 };
#define DIRECT_ALIGNMENT UINT64_C(16384)
#define INPUT_ALIGNMENT UINT64_C(256)

struct X4T26V1 {
    X4T26V1Ops ops;
    X4T26V1Options options;
    X4T26V1Info info;
    X4T26V1Memory memory[X4_T26_V1_MEMORY_COUNT];
    X4T26V1Ctrl ctrl;
    X4T26V1QueryFn query;
    X4T26V1CreateFn create;
    X4T26V1DecodeFn decode;
    X4T26V1FlushFn flush;
    X4T26V1DeleteFn delete_decoder;
    uint64_t arena_cursor;
    uint64_t output_call;
    bool attempted_open;
    bool decode_marked;
    bool flush_marked;
    bool release_contract_failed;
};

_Static_assert(sizeof(X4T26V1QueryFn) == sizeof(void *), "v1 query address size");
_Static_assert(sizeof(X4T26V1CreateFn) == sizeof(void *), "v1 create address size");
_Static_assert(sizeof(X4T26V1DecodeFn) == sizeof(void *), "v1 decode address size");
_Static_assert(sizeof(X4T26V1FlushFn) == sizeof(void *), "v1 flush address size");
_Static_assert(sizeof(X4T26V1DeleteFn) == sizeof(void *), "v1 delete address size");

static int result(X4T26V1 *v, int rc)
{
    if (v) v->info.local_result = rc;
    return rc;
}

static bool rounded(uint64_t value, uint64_t alignment, uint64_t *out)
{
    if (!alignment || (alignment & (alignment - 1)) ||
        value > UINT64_MAX - (alignment - 1)) return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static void refresh_allocations(X4T26V1 *v)
{
    uint64_t bytes = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < X4_T26_V1_MEMORY_COUNT; ++i) {
        if (!v->memory[i].allocated) continue;
        ++count;
        if (bytes > UINT64_MAX - v->memory[i].bytes) bytes = UINT64_MAX;
        else bytes += v->memory[i].bytes;
    }
    v->info.allocation_count = count;
    v->info.allocated_bytes = bytes;
    if (bytes > v->info.peak_allocated_bytes) v->info.peak_allocated_bytes = bytes;
}

static int release_allocations(X4T26V1 *v)
{
    int first = 0;
    if (v->release_contract_failed) return X4_T26_V1_CONTRACT;
    for (unsigned end = X4_T26_V1_MEMORY_COUNT; end; --end) {
        X4T26V1Memory *m = &v->memory[end - 1];
        if (!m->allocated) continue;
        X4T26V1Memory before = *m;
        int rc = v->ops.release(v->ops.context, m);
        if (!rc) {
            memset(m, 0, sizeof(*m));
        } else {
            if (!m->allocated) {
                /* A failed callback may not hide a physical reservation.
                 * Its mapping state is now unknown; do not retry a stale VA.
                 */
                *m = before;
                v->release_contract_failed = true;
                rc = X4_T26_V1_CONTRACT;
            }
            if (!first) first = rc;
        }
    }
    refresh_allocations(v);
    return first;
}

static int close_resources(X4T26V1 *v)
{
    v->info.opened = false;
    if (v->info.decoder_live) {
        int guard = v->ops.before_native(v->ops.context, "v1.delete");
        if (guard) {
            v->info.retained = true;
            return guard;
        }
        v->info.delete_called = true;
        ++v->info.delete_calls;
        ++v->info.native_calls;
        v->info.delete_rc = v->delete_decoder(&v->ctrl);
        if (v->info.delete_rc) {
            v->info.retained = true;
            return v->info.delete_rc;
        }
        v->info.decoder_live = false;
        v->info.held_mask = 0;
        v->arena_cursor = 0;
        memset(&v->ctrl, 0, sizeof(v->ctrl));
    }
    int rc = release_allocations(v);
    v->info.retained = rc != 0 || v->info.allocation_count != 0;
    return rc;
}

static int reserve_at(X4T26V1 *v, unsigned index, uint64_t bytes, uint64_t alignment)
{
    if (!bytes) return 0;
    X4T26V1Memory *m = &v->memory[index];
    int rc = v->ops.allocate(v->ops.context, bytes, alignment, 0, m);
    refresh_allocations(v);
    if (rc) return rc;
    if (!m->allocated || !m->pointer || m->bytes < bytes || m->offset < 0 ||
        ((uintptr_t)m->pointer & (alignment - 1)) ||
        ((uint64_t)m->offset & (alignment - 1))) return X4_T26_V1_CONTRACT;
    if (v->info.allocated_bytes > v->options.native_budget_bytes) return X4_T26_V1_BUDGET;
    return 0;
}

static int reserve_memory(X4T26V1 *v, X4T26V1Resource *resources)
{
    uint64_t sizes[X4_T26_V1_MEMORY_COUNT] = {0};
    uint64_t alignment = resources->frameBufferAlignment;
    if (!resources->maxFrameBufferSize) return X4_T26_V1_CONTRACT;
    if (!alignment || alignment < DIRECT_ALIGNMENT) alignment = DIRECT_ALIGNMENT;
    if (alignment & (alignment - 1)) return X4_T26_V1_CONTRACT;
    if (!rounded(resources->cpuMemorySize, DIRECT_ALIGNMENT, &sizes[CPU_MEMORY]) ||
        !rounded(resources->cpuGpuMemorySize, DIRECT_ALIGNMENT, &sizes[CPU_GPU_MEMORY]) ||
        !rounded(v->options.input_arena_bytes, DIRECT_ALIGNMENT, &sizes[INPUT_MEMORY]))
        return X4_T26_V1_BUDGET;
    for (unsigned i = 0; i < X4_T26_V1_OUTPUTS; ++i)
        if (!rounded(resources->maxFrameBufferSize, DIRECT_ALIGNMENT, &sizes[OUTPUT_MEMORY + i]))
            return X4_T26_V1_BUDGET;
    uint64_t total = 0;
    for (unsigned i = 0; i < X4_T26_V1_MEMORY_COUNT; ++i) {
        if (sizes[i] > v->options.native_budget_bytes - total) return X4_T26_V1_BUDGET;
        total += sizes[i];
    }
    for (unsigned i = 0; i < X4_T26_V1_MEMORY_COUNT; ++i) {
        int rc = reserve_at(v, i, sizes[i], i < OUTPUT_MEMORY ? DIRECT_ALIGNMENT : alignment);
        if (rc) return rc;
    }
    resources->pCpuMemory = v->memory[CPU_MEMORY].pointer;
    resources->pCpuGpuMemory = v->memory[CPU_GPU_MEMORY].pointer;
    return 0;
}

static int resolve_functions(X4T26V1 *v)
{
    static const char *const names[5] = {
        X4_T26_V1_SYMBOL_QUERY, X4_T26_V1_SYMBOL_CREATE,
        X4_T26_V1_SYMBOL_DECODE, X4_T26_V1_SYMBOL_FLUSH, X4_T26_V1_SYMBOL_DELETE
    };
    for (unsigned i = 0; i < 5; ++i) {
        void *address = NULL;
        v->info.resolve_called_mask |= 1u << i;
        int rc = v->ops.resolve(v->ops.context, names[i], &address);
        v->info.resolve_rc[i] = rc;
        if (rc || !address) return X4_T26_V1_RESOLVE;
        switch (i) {
        case 0: memcpy(&v->query, &address, sizeof(v->query)); break;
        case 1: memcpy(&v->create, &address, sizeof(v->create)); break;
        case 2: memcpy(&v->decode, &address, sizeof(v->decode)); break;
        case 3: memcpy(&v->flush, &address, sizeof(v->flush)); break;
        default: memcpy(&v->delete_decoder, &address, sizeof(v->delete_decoder)); break;
        }
    }
    return 0;
}

static int open_attempt(X4T26V1 *v, unsigned attempt, bool *native_rejection)
{
    X4T26V1Config config = v->info.config;
    X4T26V1Resource resources = {.thisSize = sizeof(resources)};
    *native_rejection = false;
    int rc = v->ops.before_native(v->ops.context, "v1.query");
    if (rc) return rc;
    v->info.query_called_mask |= 1u << attempt;
    ++v->info.query_calls;
    ++v->info.native_calls;
    rc = v->query(&config, &resources);
    v->info.query_rc[attempt] = rc;
    v->info.query_memory = resources;
    if (rc) {
        *native_rejection = true;
        return rc;
    }
    if (memcmp(&config, &v->info.config, sizeof(config)) ||
        resources.thisSize != sizeof(resources)) return X4_T26_V1_CONTRACT;
    rc = reserve_memory(v, &resources);
    if (rc) return rc;
    rc = v->ops.before_native(v->ops.context, "v1.create");
    if (rc) return rc;
    memset(&v->ctrl, 0, sizeof(v->ctrl));
    v->ctrl.thisSize = sizeof(v->ctrl);
    v->info.create_called_mask |= 1u << attempt;
    ++v->info.create_calls;
    ++v->info.native_calls;
    rc = v->create(&config, &resources, &v->ctrl);
    v->info.create_rc[attempt] = rc;
    /* A reported successful Create or a returned handle requires Delete.
     * A failed Create returning no handle is treated as no instance created,
     * matching the injected API contract; this is not an input-retention rule.
     */
    v->info.decoder_live = rc == 0 || v->ctrl.handle != NULL;
    if (rc) {
        *native_rejection = true;
        return rc;
    }
    if (!v->ctrl.handle || v->ctrl.thisSize != sizeof(v->ctrl) ||
        memcmp(&config, &v->info.config, sizeof(config))) return X4_T26_V1_CONTRACT;
    v->info.opened = true;
    return 0;
}

int x4_t26_v1_create(X4T26V1 **out, const X4T26V1Ops *ops, const X4T26V1Options *options)
{
    if (!out) return X4_T26_V1_ARGUMENT;
    *out = NULL;
    if (!ops || !options || !ops->allocate || !ops->release || !ops->load_module ||
        !ops->resolve || !ops->before_native || !ops->clock_us ||
        !options->width || options->width > 1920 ||
        !options->coded_height || options->coded_height > 1088 ||
        !options->input_arena_bytes || options->native_budget_bytes > X4_T26_V1_NATIVE_LIMIT)
        return X4_T26_V1_ARGUMENT;
    X4T26V1 *v = calloc(1, sizeof(*v));
    if (!v) return X4_T26_V1_NO_MEMORY;
    v->ops = *ops;
    v->options = *options;
    if (!v->options.native_budget_bytes) v->options.native_budget_bytes = X4_T26_V1_NATIVE_LIMIT;
    /* Match the existing v2 baseline's DPB floor of four. The stream's SPS
     * still has one reference; changing only the API keeps this comparison
     * separate from a DPB-budget experiment. */
    v->info.config = (X4T26V1Config){
        .thisSize = sizeof(X4T26V1Config), .codecType = X4_T26_V1_CODEC_AVC,
        .profile = X4_T26_V1_PROFILE_MAIN, .maxLevel = X4_T26_V1_LEVEL_4_2,
        .maxFrameWidth = (int32_t)options->width,
        .maxFrameHeight = (int32_t)options->coded_height,
        .maxDpbFrameCount = 4, .videodecFlags = X4_T26_V1_FLAGS_DEFAULT
    };
    *out = v;
    return 0;
}

int x4_t26_v1_open(X4T26V1 *v)
{
    if (!v || v->attempted_open) return result(v, X4_T26_V1_ARGUMENT);
    v->attempted_open = true;
    int rc = v->ops.before_native(v->ops.context, "v1.load.008e");
    if (rc) return result(v, rc);
    v->info.module_called = true;
    v->info.module_rc = v->ops.load_module(v->ops.context, 0x008e);
    if (v->info.module_rc) return result(v, v->info.module_rc);
    rc = resolve_functions(v);
    if (rc) return result(v, rc);
    bool native_rejection = false;
    rc = open_attempt(v, 0, &native_rejection);
    if (!rc) return result(v, 0);
    int cleanup = close_resources(v);
    if (cleanup) return result(v, cleanup);
    if (!native_rejection) return result(v, rc);
    v->info.arbitration_retry = true;
    int guard = v->ops.before_native(v->ops.context, "v1.load.00fc");
    if (guard) return result(v, guard);
    v->info.arbitration_called = true;
    v->info.arbitration_rc = v->ops.load_module(v->ops.context, 0x00fc);
    if (v->info.arbitration_rc) return result(v, v->info.arbitration_rc);
    rc = open_attempt(v, 1, &native_rejection);
    if (rc) {
        cleanup = close_resources(v);
        if (cleanup) return result(v, cleanup);
    }
    return result(v, rc);
}

static void init_output(X4T26V1Output *out, bool flush)
{
    memset(out, 0, sizeof(*out));
    out->slot = UINT_MAX;
    out->was_flush = flush;
}

static int decode_or_flush(X4T26V1 *v, unsigned slot, const void *au,
    size_t bytes, uint64_t pts, bool flush, X4T26V1Output *out)
{
    if (!out) return result(v, X4_T26_V1_ARGUMENT);
    init_output(out, flush);
    if (!v || !v->info.opened) return out->result = result(v, X4_T26_V1_NOT_OPEN);
    if (slot >= X4_T26_V1_OUTPUTS || (!flush && (!au || !bytes)))
        return out->result = result(v, X4_T26_V1_ARGUMENT);
    if (v->info.held_mask & (1u << slot))
        return out->result = result(v, X4_T26_V1_SLOT_HELD);
    uint64_t input_offset = 0;
    if (!flush && (!rounded(v->arena_cursor, INPUT_ALIGNMENT, &input_offset) ||
        input_offset > v->options.input_arena_bytes ||
        (uint64_t)bytes > v->options.input_arena_bytes - input_offset))
        return out->result = result(v, X4_T26_V1_ARENA_FULL);
    bool *marked = flush ? &v->flush_marked : &v->decode_marked;
    if (!*marked) {
        int guard = v->ops.before_native(v->ops.context, flush ? "v1.flush" : "v1.decode");
        if (guard) return out->result = result(v, guard);
        *marked = true;
    }
    X4T26V1Input input = {.thisSize = sizeof(input), .ptsData = pts,
        .dtsData = pts, .attachedData = pts};
    if (!flush) {
        input.pAuData = (uint8_t *)v->memory[INPUT_MEMORY].pointer + input_offset;
        input.auSize = bytes;
        memcpy(input.pAuData, au, bytes);
        v->arena_cursor = input_offset + bytes;
        v->info.input_used_bytes = v->arena_cursor;
    }
    X4T26V1FrameBuffer frame = {.thisSize = sizeof(frame),
        .pFrameBuffer = v->memory[OUTPUT_MEMORY + slot].pointer,
        .frameBufferSize = v->info.query_memory.maxFrameBufferSize};
    X4T26V1Picture picture = {.thisSize = sizeof(picture)};
    /* Never infer native ownership from a validity flag or offered-slot
     * timing. The offered buffer is retained before entering the call.
     */
    v->info.held_mask |= 1u << slot;
    ++v->info.native_calls;
    out->native_call = ++v->output_call;
    if (flush) ++v->info.flush_calls;
    else ++v->info.decode_calls;
    out->decode_call = v->info.decode_calls;
    out->decode_begin_us = v->ops.clock_us(v->ops.context);
    int rc = flush ? v->flush(&v->ctrl, &frame, &picture) :
        v->decode(&v->ctrl, &input, &frame, &picture);
    out->decode_end_us = v->ops.clock_us(v->ops.context);
    out->native_rc = rc;
    out->native_valid = picture.isValid;
    out->native_error = picture.isErrorPic;
    if (flush) v->info.last_flush_rc = rc;
    else v->info.last_decode_rc = rc;
    if (out->decode_end_us >= out->decode_begin_us)
        out->decode_us = out->decode_end_us - out->decode_begin_us;
    else return out->result = result(v, X4_T26_V1_CONTRACT);
    if (rc) return out->result = result(v, rc);
    if (!picture.isValid) {
        if (flush) ++v->info.flush_without_output;
        else ++v->info.without_output;
        return out->result = result(v, 0);
    }
    unsigned returned = X4_T26_V1_OUTPUTS;
    for (unsigned i = 0; i < X4_T26_V1_OUTPUTS; ++i)
        if (frame.pFrameBuffer == v->memory[OUTPUT_MEMORY + i].pointer) returned = i;
    if (returned == X4_T26_V1_OUTPUTS || !(v->info.held_mask & (1u << returned)) ||
        !frame.frameBufferSize || frame.frameBufferSize > v->memory[OUTPUT_MEMORY + returned].bytes ||
        !picture.frameWidth || !picture.frameHeight || picture.framePitch < picture.frameWidth)
        return out->result = result(v, X4_T26_V1_CONTRACT);
    v->info.held_mask &= ~(1u << returned);
    out->valid = true;
    out->error = picture.isErrorPic != 0;
    out->pointer = frame.pFrameBuffer;
    out->bytes = frame.frameBufferSize;
    out->slot = returned;
    out->width = picture.frameWidth;
    out->height = picture.frameHeight;
    out->pitch = picture.framePitch;
    out->pts = picture.ptsData;
    out->attached = picture.attachedData;
    out->avc = picture.codec.avc;
    ++v->info.valid_outputs;
    if (out->error) ++v->info.error_outputs;
    return out->result = result(v, 0);
}

int x4_t26_v1_decode(X4T26V1 *v, unsigned slot, const void *au,
    size_t bytes, uint64_t pts, X4T26V1Output *out)
{
    return decode_or_flush(v, slot, au, bytes, pts, false, out);
}

int x4_t26_v1_flush(X4T26V1 *v, unsigned slot, X4T26V1Output *out)
{
    return decode_or_flush(v, slot, NULL, 0, 0, true, out);
}

uint32_t x4_t26_v1_held_mask(const X4T26V1 *v)
{
    return v ? v->info.held_mask : 0;
}

void x4_t26_v1_info(const X4T26V1 *v, X4T26V1Info *out)
{
    if (out) *out = v ? v->info : (X4T26V1Info){0};
}

size_t x4_t26_v1_allocations(const X4T26V1 *v, X4T26V1Memory *out, size_t capacity)
{
    if (!v) return 0;
    size_t count = 0;
    for (unsigned i = 0; i < X4_T26_V1_MEMORY_COUNT; ++i) {
        if (!v->memory[i].allocated) continue;
        if (out && count < capacity) out[count] = v->memory[i];
        ++count;
    }
    return count;
}

int x4_t26_v1_close(X4T26V1 *v)
{
    if (!v) return X4_T26_V1_ARGUMENT;
    return result(v, close_resources(v));
}

int x4_t26_v1_destroy(X4T26V1 **pointer)
{
    if (!pointer || !*pointer) return X4_T26_V1_ARGUMENT;
    X4T26V1 *v = *pointer;
    int rc = close_resources(v);
    if (rc || v->info.decoder_live || v->info.allocation_count)
        return result(v, rc ? rc : X4_T26_V1_RETAINED);
    free(v);
    *pointer = NULL;
    return 0;
}

#endif
