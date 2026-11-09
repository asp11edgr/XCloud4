/* SPDX-License-Identifier: GPL-3.0-only */
#include "video/probe_t26_v1.h"
#if !defined(X4_VIDEO_PROBE) || !X4_VIDEO_PROBE || \
    !defined(X4_PROBE_T26) || !X4_PROBE_T26
#error "T26 v1 backend host test requires both probe build switches"
#endif
#ifdef NDEBUG
#error "This acceptance test requires assertions enabled"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_REJECT (-2134835199) /* 0x80c10001: preserves a signed SCE rc. */
enum { MAX_INPUTS = 64 };
typedef struct {
    unsigned allocs, frees, active, load_calls, module[4];
    unsigned queries, creates, decodes, flushes, deletes, guards;
    int query_failures, create_failures, delete_failures, release_failures;
    unsigned fail_allocate_call, fail_module;
    bool oversized_query, unknown_output, native_decode_error;
    const char *denied_operation;
    bool live, pending;
    void *pending_buffer;
    uint64_t pending_pts;
    uint64_t now, next_offset;
    const uint8_t *inputs[MAX_INPUTS];
    uint8_t input_copy[MAX_INPUTS][32];
    size_t input_size[MAX_INPUTS];
    unsigned input_count;
    X4T26V1Config first_config;
} Harness;
static Harness *current;

static void verify_inputs(Harness *h)
{
    for (unsigned i = 0; i < h->input_count; ++i)
        assert(memcmp(h->inputs[i], h->input_copy[i], h->input_size[i]) == 0);
}

static int allocate_memory(void *context, uint64_t bytes, uint64_t alignment,
    int type, X4T26V1Memory *memory)
{
    Harness *h = context;
    assert(type == 0 && bytes && alignment >= 16384 && !(alignment & (alignment - 1)));
    assert(bytes % 16384 == 0 && bytes <= X4_T26_V1_NATIVE_LIMIT);
    void *pointer = aligned_alloc((size_t)alignment, (size_t)bytes);
    assert(pointer);
    memset(pointer, 0x5a, (size_t)bytes);
    h->next_offset = (h->next_offset + alignment - 1) & ~(alignment - 1);
    *memory = (X4T26V1Memory){pointer, bytes, (int64_t)h->next_offset, true};
    h->next_offset += bytes;
    ++h->allocs;
    ++h->active;
    return h->fail_allocate_call == h->allocs ? -87 : 0;
}

static int release_memory(void *context, X4T26V1Memory *memory)
{
    Harness *h = context;
    assert(!h->live && memory->allocated && memory->pointer && h->active);
    if (h->release_failures) {
        --h->release_failures;
        return -88;
    }
    free(memory->pointer);
    *memory = (X4T26V1Memory){0};
    ++h->frees;
    --h->active;
    return 0;
}

static int load_module(void *context, unsigned module)
{
    Harness *h = context;
    assert(h->load_calls < 4);
    h->module[h->load_calls++] = module;
    if (module == 0xfc) assert(!h->active && !h->live);
    return h->fail_module == module ? -90 : 0;
}

static int before_native(void *context, const char *operation)
{
    Harness *h = context;
    ++h->guards;
    if (h->denied_operation && !strcmp(h->denied_operation, operation)) return -91;
    return 0;
}

static uint64_t clock_us(void *context)
{
    Harness *h = context;
    h->now += 10;
    return h->now;
}

static int32_t query(const X4T26V1Config *config, X4T26V1Resource *resources)
{
    Harness *h = current;
    assert(!h->live && config->thisSize == 40 && resources->thisSize == 56);
    assert(config->codecType == 0 && config->profile == 77 && config->maxLevel == 42);
    assert(config->maxFrameWidth == 32 && config->maxFrameHeight == 16);
    assert(config->maxDpbFrameCount == 4 && config->videodecFlags == 0);
    if (!h->queries) h->first_config = *config;
    else assert(!memcmp(config, &h->first_config, sizeof(*config)));
    ++h->queries;
    if (h->query_failures) {
        if (h->query_failures > 0) --h->query_failures;
        return NATIVE_REJECT;
    }
    resources->cpuMemorySize = h->oversized_query ? X4_T26_V1_NATIVE_LIMIT + 1 : 16384;
    resources->cpuGpuMemorySize = 16384;
    resources->maxFrameBufferSize = 1024;
    resources->frameBufferAlignment = 256;
    return 0;
}

static int32_t create_decoder(const X4T26V1Config *config,
    const X4T26V1Resource *resources, X4T26V1Ctrl *ctrl)
{
    Harness *h = current;
    assert(!h->live && !memcmp(config, &h->first_config, sizeof(*config)));
    assert(ctrl->thisSize == 24 && !ctrl->handle);
    assert(resources->pCpuMemory && resources->pCpuGpuMemory && h->active == 8);
    ++h->creates;
    if (h->create_failures) {
        --h->create_failures;
        return NATIVE_REJECT;
    }
    h->live = true;
    ctrl->handle = h;
    ctrl->version = 7; /* Do not invent a required firmware version value. */
    return 0;
}

static void output_picture(void *pointer, uint64_t pts,
    X4T26V1FrameBuffer *frame, X4T26V1Picture *picture)
{
    assert(pointer);
    memset(pointer, (int)(pts & 255), 1024);
    frame->pFrameBuffer = pointer;
    frame->frameBufferSize = 1024;
    picture->isValid = 1;
    picture->frameWidth = 32;
    picture->framePitch = 32;
    picture->frameHeight = 16;
    picture->ptsData = pts;
    picture->attachedData = pts;
}

static int32_t decode(X4T26V1Ctrl *ctrl, const X4T26V1Input *input,
    X4T26V1FrameBuffer *frame, X4T26V1Picture *picture)
{
    Harness *h = current;
    assert(h->live && ctrl->handle == h && input->thisSize == 48 && frame->thisSize == 24);
    assert(picture->thisSize == 112 && input->pAuData && input->auSize <= 32);
    assert(input->ptsData == input->dtsData && input->attachedData == input->ptsData);
    assert(((uintptr_t)input->pAuData & 255) == 0 && h->input_count < MAX_INPUTS);
    verify_inputs(h);
    for (unsigned i = 0; i < h->input_count; ++i) assert(input->pAuData != h->inputs[i]);
    h->inputs[h->input_count] = input->pAuData;
    h->input_size[h->input_count] = (size_t)input->auSize;
    memcpy(h->input_copy[h->input_count], input->pAuData, (size_t)input->auSize);
    ++h->input_count;
    ++h->decodes;
    bool old_pending = h->pending;
    void *old_buffer = h->pending_buffer;
    uint64_t old_pts = h->pending_pts;
    h->pending = true;
    h->pending_buffer = frame->pFrameBuffer;
    h->pending_pts = input->ptsData;
    if (h->native_decode_error) {
        picture->isValid = 1;
        frame->pFrameBuffer = (void *)(uintptr_t)1;
        return NATIVE_REJECT;
    }
    if (old_pending) output_picture(old_buffer, old_pts, frame, picture);
    else {
        /* An invalid output may mutate the in/out fields. It grants no
         * ownership proof and must not be mistaken for a corrupt valid output.
         */
        frame->pFrameBuffer = (void *)(uintptr_t)1;
        frame->frameBufferSize = UINT64_MAX;
    }
    if (h->unknown_output) {
        picture->isValid = 1;
        frame->pFrameBuffer = (void *)(uintptr_t)1;
    }
    return 0;
}

static int32_t flush(X4T26V1Ctrl *ctrl, X4T26V1FrameBuffer *frame, X4T26V1Picture *picture)
{
    Harness *h = current;
    assert(h->live && ctrl->handle == h);
    ++h->flushes;
    verify_inputs(h);
    if (h->pending) output_picture(h->pending_buffer, h->pending_pts, frame, picture);
    h->pending = false;
    return 0;
}

static int32_t delete_decoder(X4T26V1Ctrl *ctrl)
{
    Harness *h = current;
    assert(h->live && ctrl->handle == h && h->active == 8);
    verify_inputs(h);
    ++h->deletes;
    if (h->delete_failures) {
        --h->delete_failures;
        return NATIVE_REJECT;
    }
    h->live = false;
    h->pending = false;
    h->pending_buffer = NULL;
    h->input_count = 0;
    return 0;
}

static int resolve(void *context, const char *name, void **address)
{
    (void)context;
    if (!strcmp(name, X4_T26_V1_SYMBOL_QUERY)) {
        X4T26V1QueryFn fn = query;
        memcpy(address, &fn, sizeof(fn));
    } else if (!strcmp(name, X4_T26_V1_SYMBOL_CREATE)) {
        X4T26V1CreateFn fn = create_decoder;
        memcpy(address, &fn, sizeof(fn));
    } else if (!strcmp(name, X4_T26_V1_SYMBOL_DECODE)) {
        X4T26V1DecodeFn fn = decode;
        memcpy(address, &fn, sizeof(fn));
    } else if (!strcmp(name, X4_T26_V1_SYMBOL_FLUSH)) {
        X4T26V1FlushFn fn = flush;
        memcpy(address, &fn, sizeof(fn));
    } else if (!strcmp(name, X4_T26_V1_SYMBOL_DELETE)) {
        X4T26V1DeleteFn fn = delete_decoder;
        memcpy(address, &fn, sizeof(fn));
    } else return -92;
    return 0;
}

static X4T26V1 *make(Harness *h, uint64_t arena)
{
    current = h;
    X4T26V1Ops ops = {h, allocate_memory, release_memory, load_module,
        resolve, before_native, clock_us};
    X4T26V1Options options = {32, 16, arena, 0};
    X4T26V1 *v = NULL;
    assert(x4_t26_v1_create(&v, &ops, &options) == 0 && v);
    return v;
}

static void finish(Harness *h, X4T26V1 **v)
{
    assert(x4_t26_v1_destroy(v) == 0 && !*v);
    assert(!h->active && h->frees == h->allocs && !h->live);
}

static void delayed_and_drain(void)
{
    Harness h = {0};
    X4T26V1 *v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0 && h.active == 8);
    struct { X4T26V1Memory memory[2]; uint64_t canary; } truncated = {.canary = 123};
    assert(x4_t26_v1_allocations(v, truncated.memory, 2) == 8 && truncated.canary == 123);
    uint8_t source[32];
    X4T26V1Output out;
    memset(source, 1, sizeof(source));
    assert(x4_t26_v1_decode(v, 0, source, sizeof(source), 1, &out) == 0);
    assert(!out.valid && out.native_call == 1 && out.decode_us == 10);
    assert(x4_t26_v1_held_mask(v) == 1);
    memset(source, 2, sizeof(source));
    assert(x4_t26_v1_decode(v, 0, source, sizeof(source), 2, &out) == X4_T26_V1_SLOT_HELD);
    assert(h.decodes == 1);
    assert(x4_t26_v1_decode(v, 1, source, sizeof(source), 2, &out) == 0);
    assert(out.valid && out.slot == 0 && out.pts == 1 && out.attached == 1 && out.native_call == 2);
    assert(x4_t26_v1_held_mask(v) == 2);
    memset(source, 3, sizeof(source));
    assert(x4_t26_v1_decode(v, 0, source, sizeof(source), 3, &out) == 0);
    assert(out.valid && out.slot == 1 && out.pts == 2 && out.native_call == 3);
    assert(x4_t26_v1_flush(v, 1, &out) == 0);
    assert(out.valid && out.slot == 0 && out.pts == 3 && out.was_flush && out.native_call == 4);
    assert(x4_t26_v1_flush(v, 0, &out) == 0 && !out.valid);
    X4T26V1Info info;
    x4_t26_v1_info(v, &info);
    assert(info.decode_calls == 3 && info.flush_calls == 2 && info.valid_outputs == 3);
    assert(info.without_output == 1 && info.flush_without_output == 1 && info.input_used_bytes == 544);
    assert(info.query_rc[0] == 0 && info.create_rc[0] == 0 && info.resolve_called_mask == 31);
    assert(x4_t26_v1_close(v) == 0 && !x4_t26_v1_held_mask(v));
    x4_t26_v1_info(v, &info);
    assert(!info.decoder_live && !info.allocation_count && !info.retained);
    finish(&h, &v);
}

static void query_and_create_retries(void)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        Harness h = {0};
        if (mode) h.create_failures = 1;
        else h.query_failures = 1;
        X4T26V1 *v = make(&h, 8192);
        assert(x4_t26_v1_open(v) == 0 && h.load_calls == 2);
        assert(h.module[0] == 0x8e && h.module[1] == 0xfc && h.queries == 2);
        X4T26V1Info info;
        x4_t26_v1_info(v, &info);
        assert(info.arbitration_retry && info.arbitration_called && info.query_called_mask == 3);
        assert(info.query_rc[1] == 0 && info.create_rc[1] == 0 && info.opened);
        assert(h.creates == (mode ? 2u : 1u));
        finish(&h, &v);
    }
    Harness h = {.query_failures = -1};
    X4T26V1 *v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == NATIVE_REJECT);
    assert(h.queries == 2 && !h.creates && !h.allocs && !h.active && h.load_calls == 2);
    finish(&h, &v);
}

static void close_failure_and_cancel(void)
{
    Harness h = {.delete_failures = 2};
    X4T26V1 *v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0);
    uint8_t au[32] = {1};
    X4T26V1Output out;
    assert(x4_t26_v1_decode(v, 0, au, sizeof(au), 1, &out) == 0);
    assert(x4_t26_v1_close(v) == NATIVE_REJECT && h.active == 8 && !h.frees);
    assert(x4_t26_v1_destroy(&v) == NATIVE_REJECT && v && h.active == 8 && !h.frees);
    X4T26V1Info info;
    x4_t26_v1_info(v, &info);
    assert(info.retained && info.decoder_live && info.held_mask == 1);
    assert(x4_t26_v1_decode(v, 1, au, sizeof(au), 2, &out) == X4_T26_V1_NOT_OPEN);
    assert(h.decodes == 1);
    assert(x4_t26_v1_close(v) == 0 && h.deletes == 3 && !h.active);
    finish(&h, &v);

    h = (Harness){0};
    v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0);
    h.denied_operation = "v1.decode";
    assert(x4_t26_v1_decode(v, 0, au, sizeof(au), 1, &out) == -91 && !out.native_call);
    assert(!h.decodes && !x4_t26_v1_held_mask(v));
    finish(&h, &v); /* Cancellation does not prohibit the safe Delete marker. */

    h = (Harness){0};
    v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0);
    assert(x4_t26_v1_decode(v, 0, au, sizeof(au), 1, &out) == 0);
    h.denied_operation = "v1.delete";
    assert(x4_t26_v1_close(v) == -91 && h.active == 8 && !h.deletes && !h.frees);
    assert(x4_t26_v1_destroy(&v) == -91 && v && h.active == 8);
    h.denied_operation = NULL;
    finish(&h, &v);
}

static void local_failures_and_native_error(void)
{
    Harness h = {.oversized_query = true};
    X4T26V1 *v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == X4_T26_V1_BUDGET);
    assert(h.queries == 1 && !h.creates && h.load_calls == 1 && !h.allocs);
    finish(&h, &v);

    h = (Harness){.fail_allocate_call = 3};
    v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == -87 && h.load_calls == 1 && !h.creates);
    assert(h.allocs == 3 && h.frees == 3);
    finish(&h, &v);

    h = (Harness){0};
    v = make(&h, 256);
    assert(x4_t26_v1_open(v) == 0);
    uint8_t au[32] = {1};
    X4T26V1Output out;
    assert(x4_t26_v1_decode(v, 0, au, sizeof(au), 1, &out) == 0);
    assert(x4_t26_v1_decode(v, 1, au, sizeof(au), 2, &out) == X4_T26_V1_ARENA_FULL);
    assert(h.decodes == 1 && x4_t26_v1_held_mask(v) == 1);
    finish(&h, &v);

    h = (Harness){.native_decode_error = true};
    v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0);
    assert(x4_t26_v1_decode(v, 0, au, sizeof(au), 1, &out) == NATIVE_REJECT);
    assert(!out.valid && out.native_valid && x4_t26_v1_held_mask(v) == 1);
    finish(&h, &v);

    h = (Harness){.unknown_output = true};
    v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0);
    assert(x4_t26_v1_decode(v, 0, au, sizeof(au), 1, &out) == X4_T26_V1_CONTRACT);
    assert(!out.valid && x4_t26_v1_held_mask(v) == 1);
    finish(&h, &v);

    h = (Harness){.release_failures = 1};
    v = make(&h, 8192);
    assert(x4_t26_v1_open(v) == 0);
    assert(x4_t26_v1_close(v) == -88 && !h.live && h.active == 1 && h.deletes == 1);
    assert(x4_t26_v1_close(v) == 0 && !h.active && h.deletes == 1);
    finish(&h, &v);
}

int main(void)
{
    delayed_and_drain();
    query_and_create_retries();
    close_failure_and_cancel();
    local_failures_and_native_error();
    puts("PASS T26 v1 injected backend: delayed output, retained inputs, retry, drain, cancellation and safe cleanup; no native API called");
    return 0;
}
