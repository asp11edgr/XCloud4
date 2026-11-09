/* SPDX-License-Identifier: GPL-3.0-only */
/* HOST ONLY. Include the actual controller helpers. UI/native entry points
 * are not mocked or executed: linker section GC removes those unused paths. */
#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#error "This test requires active assertions"
#endif
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/video/probe_t26_controller.c"

static uint64_t host_now = UINT64_C(1000000);
static const char host_manifest[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static unsigned host_cases;
static Bank *host_stop_during_sync;
static unsigned host_stop_kind,host_sync_injections;
uint64_t sceKernelGetProcessTime(void) { return host_now; }
int __real_fsync(int);
int __wrap_fsync(int fd)
{
    int rc=__real_fsync(fd);
    if(!rc&&host_stop_during_sync){
        Bank *b=host_stop_during_sync;host_stop_during_sync=NULL;++host_sync_injections;
        if(host_stop_kind==1)atomic_store_explicit(&b->cancel,true,memory_order_release);
        else host_now=atomic_load_explicit(&b->deadline,memory_order_acquire)+1u;
    }
    return rc;
}

static Bank *host_bank(const char *base, const char *name)
{
    Bank *b = calloc(1, sizeof(*b)); assert(b);
    b->read_buffer = malloc(X4_T26_STORE_CELL_MAX + 1u); assert(b->read_buffer);
    atomic_init(&b->cancel, false); atomic_init(&b->finished, false);
    atomic_init(&b->command, 0); atomic_init(&b->deadline, UINT64_MAX);
    memcpy(b->manifest, host_manifest, sizeof(b->manifest));
    x4_t26_plan_init(&b->plan);
    char root[320]; int n = snprintf(root, sizeof(root), "%s/%s", base, name);
    assert(n > 0 && (size_t)n < sizeof(root));
    assert(x4_t26_store_open_at(&b->store, root, X4_BUILD_ID, b->manifest,
        host_now, &b->state, &b->resumed) == 0);
    return b;
}
static void host_free(Bank *b)
{
    assert(!b->store.cell_file && !b->store.summary_file);
    free(b->read_buffer); free(b);
}
static size_t host_read(const char *path, char *buffer, size_t capacity)
{
    FILE *f = fopen(path, "rb"); assert(f);
    size_t n = fread(buffer, 1, capacity - 1u, f);
    assert(!ferror(f) && fgetc(f) == EOF); buffer[n] = 0;
    assert(fclose(f) == 0); return n;
}
static void host_mode(X4T26ModeResult *m, unsigned mode, const X4T26Config *config)
{
    *m = (X4T26ModeResult){.present = true, .completed = true, .accepted = true,
        .inference_valid = true, .mode = mode, .native_calls = 781, .decode_calls = 781,
        .measured_calls = 600, .measured_outputs = 600, .elapsed_us = 13000000,
        .measured_wall_us = 10000000, .output_span_us = 9983333,
        .no_output_calls = 1, .flush_calls = 1, .drained_outputs = 1};
    m->decode = (X4T26Stats){600,1000,1000,1000,1000,1000};
    m->au_latency = (X4T26Stats){600,2000,2000,2000,2000,2000};
    m->output_interval = (X4T26Stats){599,16667,16667,16667,16667,16667};
    m->call_delay_hist[1] = 600;
    m->backend.config = *config; m->backend.closed = m->backend.owned_clean = true;
    m->backend.output_count = 5;
    for (unsigned h = 0; h < 3; ++h) {
        m->hashes[h].observed = true;
        m->hashes[h].measured_ordinal = h == 0 ? 1 : h == 1 ? 300 : 600;
        m->hashes[h].compared = config->api == 1 || mode == 1;
        m->hashes[h].matches_base = m->hashes[h].compared;
        memcpy(m->hashes[h].sha256, host_manifest, 65);
    }
}
static void host_durable_result(Bank *b, unsigned cell)
{
    const X4T26Cell *c = x4_t26_plan_cell(&b->plan, cell); assert(c);
    initialize_result(b, c); b->current.final = true;
    text(b->current.reason, sizeof(b->current.reason), "HOST ONLY synthetic result reader fixture");
    for (unsigned mode = 0; mode < 2; ++mode) host_mode(&b->current.modes[mode], mode, &c->config);
    FILE *f = x4_t26_store_cell_begin(&b->store, cell); assert(f);
    assert(x4_t26_result_write(f, &b->current) == 0);
    assert(x4_t26_store_cell_finish(&b->store, f) == 0);
}
static void host_active(Bank *b, unsigned cell)
{
    if (cell == X4_T26_V1_FIRST) {
        b->state.skipped_mask = (UINT64_C(1) << cell) - 1u;
        b->state.next_cell = cell;
        assert(x4_t26_state_valid(&b->state));
    }
    assert(x4_t26_state_begin(&b->state, cell, X4_T26_MODE_MAXIMUM,
        X4_T26_PHASE_QUERY_DECODER) == 0);
    assert(x4_t26_store_write_progress(&b->store, &b->state) == 0);
}
static void host_partial(Bank *b, unsigned cell)
{
    FILE *f = x4_t26_store_cell_begin(&b->store, cell); assert(f);
    assert(fputs("{\"interrupted\":", f) >= 0);
    assert(fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0);
    /* Process-loss fixture: ownership is deliberately abandoned after close. */
    b->store.cell_file = NULL;
}
static void host_phases(void)
{
    static const struct { const char *name; unsigned expected; } fixtures[] = {
        {"v2.QueryComputeMemoryInfo",X4_T26_PHASE_QUERY_COMPUTE},
        {"v2.AllocateComputeQueue",X4_T26_PHASE_ALLOCATE_QUEUE},
        {"v2.QueryDecoderMemoryInfo",X4_T26_PHASE_QUERY_DECODER},
        {"v2.CreateDecoder",X4_T26_PHASE_CREATE},
        {"DeleteDecoder",X4_T26_PHASE_DELETE},{"v2.delete",X4_T26_PHASE_DELETE},
        {"v1.query",X4_T26_PHASE_QUERY_DECODER},{"v1.create",X4_T26_PHASE_CREATE},
        {"v1.decode",X4_T26_PHASE_DECODE},{"v1.flush",X4_T26_PHASE_FLUSH},
        {"v1.delete",X4_T26_PHASE_DELETE},{"v1.load.008e",X4_T26_PHASE_QUERY_DECODER},
        {"v1.load.00fc",X4_T26_PHASE_ARBITRATION},{"warmup",X4_T26_PHASE_WARMUP},
        {"Decode",X4_T26_PHASE_DECODE},{"Flush",X4_T26_PHASE_FLUSH}
    };
    for (unsigned i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); ++i)
        assert(phase(fixtures[i].name) == fixtures[i].expected);
    ++host_cases;
}
static void host_stop_admission(const char *base)
{
    static const char *const work[]={"v2.QueryComputeMemoryInfo","v2.AllocateComputeQueue",
        "v2.QueryDecoderMemoryInfo","v2.CreateDecoder","v1.load.00fc","Flush","Decode"};
    for(unsigned cancellation=0;cancellation<2;++cancellation){
        Bank *b=host_bank(base,cancellation?"stop-cancel-before":"stop-deadline-before");
        host_active(b,0);b->current_mode=X4_T26_MODE_MAXIMUM;
        atomic_store_explicit(&b->deadline,b->state.deadline_us,memory_order_release);
        if(cancellation)atomic_store_explicit(&b->cancel,true,memory_order_release);
        else host_now=b->state.deadline_us;
        uint64_t generation=b->state.generation;
        for(unsigned i=0;i<sizeof(work)/sizeof(work[0]);++i){
            assert(before_native(b,work[i])==(cancellation?3:4));
            assert(b->state.generation==generation);
        }
        /* The helper only authorizes a Delete; no SDK function is executed. */
        assert(before_native(b,"DeleteDecoder")==0);
        assert(b->state.phase==X4_T26_PHASE_DELETE&&b->state.generation>generation);
        X4T26State durable;X4T26Store again;bool resumed;
        assert(x4_t26_store_open_at(&again,b->store.root,X4_BUILD_ID,b->manifest,host_now,&durable,&resumed)==0&&resumed);
        assert(durable.phase==X4_T26_PHASE_DELETE&&durable.generation==b->state.generation);
        host_free(b);++host_cases;
    }
    for(unsigned kind=1;kind<=2;++kind){
        Bank *b=host_bank(base,kind==1?"stop-cancel-during-sync":"stop-deadline-during-sync");
        host_active(b,0);b->current_mode=X4_T26_MODE_MAXIMUM;
        atomic_store_explicit(&b->deadline,b->state.deadline_us,memory_order_release);
        uint64_t generation=b->state.generation;
        host_stop_during_sync=b;host_stop_kind=kind;unsigned before=host_sync_injections;
        assert(before_native(b,"v2.CreateDecoder")== (kind==1?3:4));
        assert(host_sync_injections==before+1u&&!host_stop_during_sync);
        assert(b->state.generation>generation&&b->state.phase==X4_T26_PHASE_CREATE);
        X4T26State durable;X4T26Store again;bool resumed;
        assert(x4_t26_store_open_at(&again,b->store.root,X4_BUILD_ID,b->manifest,host_now,&durable,&resumed)==0&&resumed);
        assert(durable.generation==b->state.generation&&durable.phase==X4_T26_PHASE_CREATE);
        generation=b->state.generation;
        assert(before_native(b,"v1.delete")==0&&b->state.generation>generation);
        assert(b->state.phase==X4_T26_PHASE_DELETE);
        host_free(b);++host_cases;
    }
}
static void host_recovery(const char *base)
{
    Bank *b = host_bank(base, "durable-before-progress"); assert(!b->resumed);
    host_active(b, 0); host_durable_result(b, 0); host_free(b);
    host_now += 100000;
    b = host_bank(base, "durable-before-progress"); assert(b->resumed && b->state.active);
    assert(recover(b) == 0);
    assert(b->state.next_cell == 1 && b->state.completed_mask == 1 && !b->state.active);
    assert(b->recovered_valid[0] && b->base_valid[1] && b->recovered[0].outcome.accepted);
    assert(b->plan.factors[0].accepted && b->restart_charge_us == 0);
    assert(b->recovered[0].native_fps_available[0] && b->recovered[0].native_fps[0] == 60.0);
    host_free(b); ++host_cases;

    b = host_bank(base, "partial-reopened-twice"); host_active(b, 0); host_partial(b, 0); host_free(b);
    b = host_bank(base, "partial-reopened-twice");
    assert(recover(b) == 0 && b->state.next_cell == 1 && b->state.closed_app_mask == 1);
    assert(b->state.skipped_mask == 1 && !b->state.active && b->restart_charge_us == 2u*X4_T26_NOMINAL_MODE_US);
    uint64_t used = b->state.budget_used_us;
    host_free(b);
    b = host_bank(base, "partial-reopened-twice");
    assert(recover(b) == 0 && b->state.closed_app_mask == 1 && b->state.budget_used_us == used);
    assert(b->restart_charge_us == 0);
    host_free(b); ++host_cases;

    b = host_bank(base, "v1-interrupted-stop"); host_active(b, 38); host_partial(b, 38); host_free(b);
    b = host_bank(base, "v1-interrupted-stop");
    assert(recover(b) == 0 && b->state.terminal == X4_T26_TERMINAL_V1_CRASH);
    assert(b->state.next_cell == 43 && (b->state.closed_app_mask & (UINT64_C(1)<<38)));
    assert(!b->state.v1_passed_15 && !b->recovered_valid[38]); host_free(b); ++host_cases;

    b = host_bank(base, "v1-15-durable"); host_active(b, 38); host_durable_result(b, 38); host_free(b);
    b = host_bank(base, "v1-15-durable");
    assert(recover(b) == 0 && b->state.next_cell == 39 && b->state.v1_passed_15 && !b->state.terminal);
    assert(b->recovered_valid[38] && b->recovered[38].outcome.accepted); host_free(b); ++host_cases;
}
static void host_summaries(const char *base)
{
    Bank *b = host_bank(base, "summary-create-reuse");
    assert(x4_t26_state_finish_budget(&b->state) == 0);
    assert(x4_t26_store_write_progress(&b->store, &b->state) == 0);
    assert(summary(b) == 0 && !b->store.summary_file);
    char path[X4_T26_STORE_PATH_MAX]; assert(x4_t26_store_summary_path(&b->store,path,sizeof(path)) == 0);
    char *before = malloc(X4_T26_STORE_SUMMARY_MAX + 1u), *after = malloc(X4_T26_STORE_SUMMARY_MAX + 1u); assert(before && after);
    size_t bytes = host_read(path,before,X4_T26_STORE_SUMMARY_MAX+1u);
    X4JsonSpan root, cells, item; assert(!x4_json_parse(before,bytes,&root));
    assert(x4_json_member(root,"cells",&cells) == 1);
    unsigned count = 0;size_t cursor = 0;while(x4_json_item(cells,&cursor,&item) == 1)++count;
    assert(count == 43 && summary(b) == 0);
    assert(host_read(path,after,X4_T26_STORE_SUMMARY_MAX+1u) == bytes && !memcmp(before,after,bytes));
    host_free(b); ++host_cases;

    for (unsigned well_formed = 0; well_formed < 2; ++well_formed) {
        b = host_bank(base,well_formed ? "summary-identity-only" : "summary-partial");
        FILE *f = x4_t26_store_summary_begin(&b->store); assert(f);
        if (well_formed)
            assert(fprintf(f,"{\"schema\":\"X4T26_BANK1\",\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\"}",X4_BUILD_ID,b->manifest,b->store.session) > 0);
        else assert(fputs("{\"schema\":\"X4T26_BANK1\",",f) >= 0);
        assert(fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0);b->store.summary_file = NULL;
        assert(x4_t26_store_summary_path(&b->store,path,sizeof(path)) == 0);
        bytes = host_read(path,before,X4_T26_STORE_SUMMARY_MAX+1u);
        assert(summary(b) < 0);
        assert(host_read(path,after,X4_T26_STORE_SUMMARY_MAX+1u) == bytes && !memcmp(before,after,bytes));
        host_free(b); ++host_cases;
    }
    free(before);free(after);
}
int main(int argc,char **argv)
{
    assert(argc == 2);
    host_phases();host_stop_admission(argv[1]);host_recovery(argv[1]);host_summaries(argv[1]);
    printf("PASS %u actual controller helper scenarios; HOST fixtures preserved at %s; no UI/native decoder called\n",host_cases,argv[1]);
    return 0;
}
