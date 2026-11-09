/* SPDX-License-Identifier: GPL-3.0-only */
/* HOST ONLY: synthetic AU/storage/clock/backend/hash. Matcher, statistics,
 * CPU delta and cached-AU indexing are the real project implementations. */
#ifdef NDEBUG
#error "This test requires active assertions"
#endif
#include "video/probe_t26_runner.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t now, first_decode, measure_start;
    unsigned delay, decode_count, flush_count, close_count, destroy_count;
    unsigned hash_count, sleep_count, before_count;
    unsigned cancel_after, budget_after;
    uint32_t protected_slots;
    bool query_rejected, close_failed, empty_flush;
    atomic_bool cancel;
    X4T26Backend *backend;
} Fixture;

struct X4T26Backend {
    Fixture *f;
    X4T26BackendOptions options;
    X4T26BackendInfo info;
    uint32_t held;
    unsigned pending, head;
    unsigned slots[1024];
    uint64_t ordinals[1024], pixels[5][8];
    bool closed;
};
static Fixture *current;
static unsigned cases;
static const uint64_t expected_hashes[3] = {181, 480, 780};
enum { DECODE_US = 1000, FLUSH_US = 700, HASH_US = 123, REJECT = -9201, DELETE_FAILED = -9202 };

static uint64_t clock_us(void *context) { return ((Fixture *)context)->now; }
static void sleep_us(void *context, unsigned us)
{
    Fixture *f = context;
    assert(us && us <= 5000); f->now += us; ++f->sleep_count;
}
static bool budget_expired(void *context)
{
    Fixture *f = context;
    return f->budget_after && f->decode_count >= f->budget_after;
}
static int before(void *context, const char *operation)
{
    Fixture *f = context; assert(operation && *operation); ++f->before_count;
    if (!strcmp(operation, "Decode")) { assert(f->decode_count == 180); f->measure_start = f->now; }
    return 0;
}

int x4_t26_backend_create(X4T26Backend **out, const X4T26BackendOptions *options)
{
    assert(out && !*out && options && options->context == current);
    X4T26Backend *b = calloc(1, sizeof(*b)); assert(b);
    b->f = options->context; b->options = *options; b->info.config = options->config;
    b->info.v2.query_result = b->info.v2.create_result = X4_PROBE_STACK_NOT_RUN;
    b->info.output_count = options->config.api == X4_T26_API_V1 ? 5 : options->config.depth + 3;
    b->f->backend = b; *out = b; return 0;
}
int x4_t26_backend_open(X4T26Backend *b)
{
    b->info.attempted = true;
    if (b->info.config.api == X4_T26_API_V1) {
        b->info.v1.query_called_mask = 1;
        b->info.v1.query_rc[0] = b->f->query_rejected ? REJECT : 0;
    } else b->info.v2.query_result = b->f->query_rejected ? REJECT : 0;
    if (b->f->query_rejected) { b->info.open_result = REJECT; return REJECT; }
    b->info.v2.create_result = 0; b->info.opened = true; return 0;
}

static void offer(X4T26Backend *b, unsigned slot)
{
    assert(slot < b->info.output_count);
    uint32_t bit = UINT32_C(1) << slot;
    assert(!(b->held & bit));
    /* This independent semantic lease catches reuse even after native
     * ownership retires, before the deferred hash observer reads the slot. */
    assert(!(b->f->protected_slots & bit));
    b->held |= bit;
}
static void output(X4T26Backend *b, bool flush, uint64_t started, X4T26Output *out)
{
    *out = (X4T26Output){.native_called = true, .layout_verified = true,
        .from_flush = flush, .decode_begin_us = started, .decode_end_us = b->f->now,
        .decode_us = b->f->now - started, .decode_call = b->f->decode_count,
        .native_call = b->f->decode_count + b->f->flush_count};
    if (!b->pending || (!flush && b->pending <= b->f->delay) || (flush && b->f->empty_flush)) return;
    unsigned slot = b->slots[b->head]; uint64_t ordinal = b->ordinals[b->head];
    b->head = (b->head + 1u) % 1024u; --b->pending;
    assert(b->held & (UINT32_C(1) << slot)); b->held &= ~(UINT32_C(1) << slot);
    out->valid = true; out->native_valid = 1; out->picture_count = 1;
    out->slot = slot; out->pointer = (const uint8_t *)b->pixels[slot];
    out->native_pointer = out->pointer; out->owned_bytes = out->returned_bytes = sizeof(b->pixels[slot]);
    out->native_bytes = out->owned_bytes; out->width = out->height = out->pitch = 2;
    out->layout = (X4VideoNv12Layout){.visible_width = 2, .visible_height = 2,
        .coded_width = 2, .coded_height = 2, .storage_height = 2, .pitch = 2,
        .y_bytes = 4, .uv_bytes = 2, .uv_plane_offset = 4, .total_nv12_bytes = 6};
    assert(b->pixels[slot][0] == ordinal);
    for (unsigned i = 0; i < 3; ++i)
        if (ordinal == expected_hashes[i]) b->f->protected_slots |= UINT32_C(1) << slot;
}
int x4_t26_backend_decode(X4T26Backend *b, unsigned slot, const void *au,
    size_t size, uint64_t pts, X4T26Output *out)
{
    assert(b && b->info.opened && !b->closed && !b->f->hash_count && size == sizeof(uint32_t));
    uint32_t ordinal; memcpy(&ordinal, au, sizeof(ordinal));
    assert(ordinal == b->f->decode_count + 1u); /* real cache's index0 is AU1 */
    assert(pts == (uint64_t)(ordinal - 1u) * 1000000u / 60u);
    offer(b, slot);
    b->pixels[slot][0] = ordinal;
    unsigned tail = (b->head + b->pending) % 1024u;
    b->slots[tail] = slot; b->ordinals[tail] = ordinal; ++b->pending;
    assert(b->pending < 1024);
    if (!b->f->decode_count) b->f->first_decode = b->f->now;
    uint64_t started = b->f->now; b->f->now += DECODE_US; ++b->f->decode_count;
    output(b, false, started, out);
    if (b->f->cancel_after && b->f->decode_count == b->f->cancel_after)
        atomic_store_explicit(&b->f->cancel, true, memory_order_release);
    return 0;
}
int x4_t26_backend_flush(X4T26Backend *b, unsigned slot, X4T26Output *out)
{
    assert(b && b->info.opened && !b->closed); offer(b, slot);
    uint64_t started = b->f->now; b->f->now += FLUSH_US; ++b->f->flush_count;
    assert(b->f->flush_count <= 8);
    output(b, true, started, out); return 0;
}
uint32_t x4_t26_backend_held_mask(const X4T26Backend *b) { return b->held; }
void x4_t26_backend_info(X4T26Backend *b, X4T26BackendInfo *out) { *out = b->info; }
void x4_t26_backend_cpu(X4T26Backend *b, X4T26CpuSnapshot *out)
{
    *out = (X4T26CpuSnapshot){.process_available = true, .thread_available = true,
        .process_us = b->f->now / 2u, .thread_us = b->f->now / 4u};
}
int x4_t26_backend_close(X4T26Backend *b)
{
    ++b->f->close_count;
    if (b->f->close_failed) {
        b->info.retained = true; b->info.close_result = DELETE_FAILED;
        return DELETE_FAILED;
    }
    b->held = 0; b->pending = 0; b->f->protected_slots = 0; b->closed = true;
    b->info.closed = b->info.owned_clean = true; b->info.retained = false;
    b->info.close_result = 0; return 0;
}
int x4_t26_backend_destroy(X4T26Backend **b)
{
    assert(b && *b && (*b)->closed && !(*b)->held);
    Fixture *f = (*b)->f; ++f->destroy_count; f->backend = NULL;
    free(*b); *b = NULL; return 0;
}
int x4_t26_result_hash_nv12(const X4T26Output *out, char hash[65])
{
    Fixture *f = current;
    assert(f && f->backend && !f->backend->closed && f->hash_count < 3);
    uint64_t ordinal; memcpy(&ordinal, out->pointer, sizeof(ordinal));
    assert(ordinal == expected_hashes[f->hash_count]);
    assert(f->protected_slots & (UINT32_C(1) << out->slot));
    f->protected_slots &= ~(UINT32_C(1) << out->slot);
    /* Deliberately synthetic hex, not a SHA256 or color-validation claim. */
    assert(snprintf(hash, 65, "%064llu", (unsigned long long)ordinal) == 64);
    ++f->hash_count; f->now += HASH_US; return 0;
}

static void initialize(Fixture *f, unsigned delay)
{
    *f = (Fixture){.now = 1000000, .delay = delay}; atomic_init(&f->cancel, false); current = f;
}
static int run(Fixture *f, unsigned api, unsigned depth, unsigned mode,
    const char baseline[3][65], X4T26ModeResult *result, X4T26Backend **retained)
{
    X4T26Config config = {.api = api, .resource_type = api == X4_T26_API_V2,
        .depth = depth, .affinity_mask = api == X4_T26_API_V2 ? UINT64_C(0x3f) : 0,
        .priority = api == X4_T26_API_V2 ? 700 : 0};
    X4T26RunOps ops = {.context = f, .now_us = clock_us, .sleep_us = sleep_us,
        .before = before, .budget_expired = budget_expired, .cancel = &f->cancel};
    X4T26ClipData *clip = calloc(1, sizeof(*clip)); assert(clip);
    clip->bytes = X4_T26_SOURCE_AUS * sizeof(uint32_t); clip->allocated_bytes = clip->bytes;
    clip->data = malloc(clip->bytes); assert(clip->data); clip->verified = true;
    clip->v1_input_arena_bytes = 256u * X4_T26_SOURCE_AUS;
    for (unsigned i = 0; i < X4_T26_SOURCE_AUS; ++i) {
        uint32_t ordinal = i + 1u;
        clip->offset[i] = (uint64_t)i * sizeof(ordinal); clip->size[i] = sizeof(ordinal);
        clip->pts_us[i] = (uint64_t)i * 1000000u / 60u;
        memcpy(clip->data + clip->offset[i], &ordinal, sizeof(ordinal));
    }
    int rc = x4_t26_run_raw(&config, mode, &x4_t26_generated_samples[0], clip,
        baseline, &ops, result, retained);
    free(clip->data); free(clip); return rc;
}
static void accepted(unsigned api, unsigned depth, unsigned delay, unsigned mode)
{
    Fixture f; initialize(&f, delay); X4T26ModeResult result; X4T26Backend *retained = NULL;
    assert(run(&f, api, depth, mode, NULL, &result, &retained) == 0);
    assert(!retained && !f.backend && f.close_count == 1 && f.destroy_count == 1);
    assert(result.accepted && result.completed && result.inference_valid && result.status == X4_T26_RESULT_PASS);
    assert(result.measured_calls == 600 && result.measured_outputs == 600 && result.decode.n == 600);
    assert(result.decode.p50_us == DECODE_US && result.decode.mean_us == DECODE_US);
    assert(result.au_latency.n == 600 && result.output_interval.n == 599);
    assert(result.no_output_calls == delay && result.error_outputs == 0 && result.invalid_reasons == 0);
    assert(result.measured_flush_outputs == (delay > 1 ? delay - 1u : 0));
    assert(result.drained_outputs == delay && f.flush_count <= 8);
    assert(f.decode_count == (delay ? 781u : 780u));
    assert(f.hash_count == 3 && result.hash_observer_us == HASH_US * 3u);
    assert(result.held_after_close == 0 && result.backend.owned_clean);
    assert(f.measure_start - f.first_decode >= UINT64_C(2983333));
    assert(f.measure_start - f.first_decode <= UINT64_C(3000000));
    assert(result.cpu.wall_us == result.measured_wall_us && result.cpu.process_available);
    for (unsigned i = 0; i < 3; ++i) {
        assert(result.hashes[i].observed && result.hashes[i].measured_ordinal == (i == 0 ? 1u : i == 1 ? 300u : 600u));
        assert(!result.hashes[i].compared);
    }
    if (!delay) assert(result.call_delay_hist[0] == 600);
    else if (delay == 1) assert(result.call_delay_hist[1] == 600);
    else assert(result.call_delay_hist[2] == 599 && result.measured_flush_outputs == 1);
    if (mode == X4_T26_MODE_MAXIMUM)
        assert(result.measured_wall_us == (delay ? 601u : 600u) * (uint64_t)DECODE_US + (delay > 1 ? FLUSH_US : 0));
    else assert(result.measured_wall_us >= UINT64_C(9980000));
    ++cases;
}
static void rejected_cancelled_retained(void)
{
    Fixture f; X4T26ModeResult result; X4T26Backend *retained = NULL;
    initialize(&f, 1); f.query_rejected = true;
    assert(run(&f, 2, 2, X4_T26_MODE_MAXIMUM, NULL, &result, &retained) == REJECT);
    assert(result.status == X4_T26_RESULT_QUERY_REJECTED && !result.accepted && !retained);
    assert(!f.decode_count && !f.hash_count && f.destroy_count == 1); ++cases;
    initialize(&f, 1); f.cancel_after = 200;
    assert(run(&f, 2, 2, X4_T26_MODE_MAXIMUM, NULL, &result, &retained) == 3);
    assert(result.status == X4_T26_RESULT_CANCELLED && !result.completed && !retained);
    assert(f.decode_count == 200 && f.hash_count == 0 && f.destroy_count == 1); ++cases;
    initialize(&f, 1); f.budget_after = 200;
    assert(run(&f, 2, 2, X4_T26_MODE_PACED, NULL, &result, &retained) == 4);
    assert(result.status == X4_T26_RESULT_BUDGET && !result.completed && !retained);
    assert(f.decode_count == 200 && f.destroy_count == 1); ++cases;
    initialize(&f, 1); f.close_failed = true;
    assert(run(&f, 2, 2, X4_T26_MODE_MAXIMUM, NULL, &result, &retained) == DELETE_FAILED);
    assert(retained == f.backend && result.backend.retained && !result.backend.owned_clean);
    assert(result.held_after_close != 0 && f.destroy_count == 0 && !result.accepted);
    f.close_failed = false;
    assert(x4_t26_backend_close(retained) == 0 && x4_t26_backend_destroy(&retained) == 0);
    assert(!retained && !f.backend && f.destroy_count == 1); ++cases;
    initialize(&f, 2); f.empty_flush = true;
    assert(run(&f, 1, 1, X4_T26_MODE_MAXIMUM, NULL, &result, &retained) != 0);
    assert(!result.accepted && f.flush_count <= 8 && f.destroy_count == 1 && !retained); ++cases;
}
int main(void)
{
    for (unsigned mode = 0; mode < X4_T26_MODE_COUNT; ++mode) {
        accepted(2, 1, 0, mode); accepted(2, 2, 1, mode); accepted(1, 1, 2, mode);
    }
    rejected_cancelled_retained();
    printf("PASS T26 raw runner %u HOST ONLY scenarios: real matcher/cache; synthetic backend/clock/hash\n", cases);
    return 0;
}
