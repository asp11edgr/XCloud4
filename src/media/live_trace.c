/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_trace.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HISTORY = 8192, WINDOWS = 12, WINDOW_RECORDS = 16384, STAGES = 8,
       EVENTS = 512, RESET_REASONS = 64 };
enum { GAP_US = 100000, PRE_US = 500000, POST_US = 500000, WINDOW_US = 2000000 };
enum { PRE_RECORD_LIMIT = 2048 };
enum {
    W_ID, W_COUNT, W_FLAGS, W_PRE_START, W_MIN_TIME, W_MAX_TIME,
    W_FIRST_GAP, W_LAST_GAP, W_GAP_START, W_OPENED, W_LAST_CLOSED,
    W_POST_DEADLINE, W_FROZEN, W_REASON, W_OVERWRITES, W_RESERVED,
    W_OMITTED, W_BEGINS, W_CLOSES, W_DROPS_OPEN, W_DROPS_FREEZE,
    W_CLOSED_OPEN, W_CLOSED_FREEZE, W_ONGOING_OPEN, W_ONGOING_FREEZE,
    W_GAP_OPEN, W_FIRST_ORDINAL, W_LAST_ORDINAL, W_PRE_RECORDS,
    W_APPENDED_RECORDS, W_PRE_US, W_POST_US
};
typedef struct {
    uint64_t h[32];
    X4TraceRecord records[WINDOW_RECORDS];
} TraceWindow;
typedef struct {
    bool active;
    uint64_t serial, last_us, generation, closed_count, gap_id, gap_start, gap_end;
} Cadence;

struct X4Trace {
    X4Monitor *monitor;
    atomic_flag gate;
    atomic_uint_fast64_t attempted[STAGES], admitted[STAGES], dropped[STAGES];
    atomic_uint_fast64_t event_attempted[EVENTS], event_admitted[EVENTS], event_dropped[EVENTS];
    atomic_uint_fast64_t reset_primary[RESET_REASONS];
    atomic_uint_fast64_t monitor_skips, unstable, identity_errors;
    atomic_uint_fast64_t version, serial, last_us, generation, closed_count;
    atomic_uint_fast64_t gap_id, gap_start, gap_end;
    atomic_bool active;
    /* These fields have one writer: the main/display thread. */
    uint64_t main_serial, main_last_us, main_generation, main_closed_count;
    uint64_t main_gap_id, main_gap_start, main_gap_end, main_first_us, main_last_ever_us;
    bool main_active;
    /* Remaining non-atomic fields are gate-owned, or quiescent-only. */
    uint64_t local_session, total_records, history_overwrites, coverage;
    uint64_t seen_closed, seen_closed_id, observed_open_id, open_gap_id, ongoing,
        last_attempted_gap;
    uint64_t retained_closed, late_closed, missing_details, storage_exhausted;
    uint64_t event_caps, time_caps, open_at_stop, detection_excess;
    unsigned configured_readers, history_head, history_count, window_count;
    int current_window;
    bool quiesced;
    X4TraceRecord history[HISTORY];
    TraceWindow windows[WINDOWS];
};
_Static_assert(sizeof(X4Trace) <= 8 * 1024 * 1024, "bounded trace allocation");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "trace needs lock-free 64-bit atomics");

uint64_t x4_trace_now_us(void) { return x4_monitor_now_us(); }
static unsigned stage_of(uint16_t event) { return (event >> 8) & (STAGES - 1); }
static unsigned event_index(uint16_t event)
{
    unsigned stage = event >> 8, item = event & 255;
    return stage < STAGES && item < 64 ? stage * 64 + item : 63;
}
static void count_attempt(X4Trace *t, uint16_t event, uint64_t b)
{
    atomic_fetch_add_explicit(&t->attempted[stage_of(event)], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->event_attempted[event_index(event)], 1, memory_order_relaxed);
    /* Media's primary-reset reason occupies b[63:48]. Generic flags describe
     * AU state/other event reasons and must not be mixed into this histogram. */
    if (event == X4_TRACE_AU_RESET) {
        unsigned reason = (unsigned)(b >> 48);
        if (reason >= RESET_REASONS - 1) reason = RESET_REASONS - 1;
        atomic_fetch_add_explicit(&t->reset_primary[reason], 1, memory_order_relaxed);
    }
}
static uint64_t drop_count(const X4Trace *t)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < STAGES; ++i)
        n += atomic_load_explicit(&t->dropped[i], memory_order_relaxed);
    return n;
}
static void publish_cadence(X4Trace *t)
{
    atomic_fetch_add_explicit(&t->version, 1, memory_order_acq_rel);
    /* Publish the odd sequence before any tuple field under the C11 model. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&t->active, t->main_active, memory_order_relaxed);
    atomic_store_explicit(&t->serial, t->main_serial, memory_order_relaxed);
    atomic_store_explicit(&t->last_us, t->main_last_us, memory_order_relaxed);
    atomic_store_explicit(&t->generation, t->main_generation, memory_order_relaxed);
    atomic_store_explicit(&t->closed_count, t->main_closed_count, memory_order_relaxed);
    atomic_store_explicit(&t->gap_id, t->main_gap_id, memory_order_relaxed);
    atomic_store_explicit(&t->gap_start, t->main_gap_start, memory_order_relaxed);
    atomic_store_explicit(&t->gap_end, t->main_gap_end, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->version, 1, memory_order_release);
}
static bool read_cadence(X4Trace *t, Cadence *c)
{
    uint64_t v = atomic_load_explicit(&t->version, memory_order_acquire);
    if (v & 1) goto unstable;
    c->active = atomic_load_explicit(&t->active, memory_order_relaxed);
    c->serial = atomic_load_explicit(&t->serial, memory_order_relaxed);
    c->last_us = atomic_load_explicit(&t->last_us, memory_order_relaxed);
    c->generation = atomic_load_explicit(&t->generation, memory_order_relaxed);
    c->closed_count = atomic_load_explicit(&t->closed_count, memory_order_relaxed);
    c->gap_id = atomic_load_explicit(&t->gap_id, memory_order_relaxed);
    c->gap_start = atomic_load_explicit(&t->gap_start, memory_order_relaxed);
    c->gap_end = atomic_load_explicit(&t->gap_end, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (v == atomic_load_explicit(&t->version, memory_order_acquire)) return true;
unstable:
    atomic_fetch_add_explicit(&t->unstable, 1, memory_order_relaxed);
    return false;
}

static bool append_locked(X4Trace *, uint64_t, uint16_t, uint16_t, uint64_t, uint64_t);
static bool append_admitted_locked(X4Trace *, uint64_t, uint16_t, uint16_t, uint64_t, uint64_t);
static void freeze_locked(X4Trace *t, uint64_t now, unsigned reason)
{
    if (t->current_window < 0) return;
    unsigned index = (unsigned)t->current_window;
    TraceWindow *w = &t->windows[index];
    w->h[W_FROZEN] = now; w->h[W_REASON] = reason;
    w->h[W_DROPS_FREEZE] = drop_count(t);
    w->h[W_CLOSED_FREEZE] = atomic_load_explicit(&t->closed_count, memory_order_relaxed);
    w->h[W_ONGOING_FREEZE] = t->ongoing;
    if (w->h[W_DROPS_FREEZE]) w->h[W_FLAGS] |= X4_TRACE_C_RECORD_DROP;
    if (reason == X4_TRACE_R_EVENT_CAP) {
        w->h[W_FLAGS] |= X4_TRACE_C_EVENT_CAP; ++t->event_caps;
    } else if (reason == X4_TRACE_R_TIME_CAP) {
        w->h[W_FLAGS] |= X4_TRACE_C_TIME_CAP; ++t->time_caps;
    } else if (reason == X4_TRACE_R_SESSION_END && w->h[W_GAP_OPEN]) {
        w->h[W_FLAGS] |= X4_TRACE_C_OPEN_AT_STOP;
    }
    t->coverage |= w->h[W_FLAGS];
    t->current_window = -1;
    append_locked(t, now, X4_TRACE_WINDOW_FROZEN, X4_TRACE_REASON(reason), index + 1, w->h[W_COUNT]);
}
/* Internal metadata records increment the same diagnostic attempt/admit
 * counters. Window omission is separate: the rolling history still admits it. */
static bool append_locked(X4Trace *t, uint64_t time, uint16_t event,
                          uint16_t flags, uint64_t a, uint64_t b)
{
    count_attempt(t, event, b);
    return append_admitted_locked(t, time, event, flags, a, b);
}
static bool append_admitted_locked(X4Trace *t, uint64_t time, uint16_t event,
                                   uint16_t flags, uint64_t a, uint64_t b)
{
    unsigned stage = stage_of(event);
    atomic_fetch_add_explicit(&t->admitted[stage], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->event_admitted[event_index(event)], 1, memory_order_relaxed);
    X4TraceRecord r = {time, a, b, (uint32_t)++t->total_records, event, flags};
    if (!r.ordinal) t->coverage |= X4_TRACE_C_ORDINAL_WRAP;
    t->history[t->history_head] = r;
    t->history_head = (t->history_head + 1) % HISTORY;
    if (t->history_count < HISTORY) ++t->history_count;
    else ++t->history_overwrites;
    if (t->current_window < 0) return false;
    TraceWindow *w = &t->windows[t->current_window];
    if (w->h[W_COUNT] == WINDOW_RECORDS) {
        ++w->h[W_OMITTED]; freeze_locked(t, x4_trace_now_us(), X4_TRACE_R_EVENT_CAP); return false;
    }
    w->records[w->h[W_COUNT]++] = r;
    ++w->h[W_APPENDED_RECORDS];
    if (!w->h[W_MIN_TIME] || time < w->h[W_MIN_TIME]) w->h[W_MIN_TIME] = time;
    if (time > w->h[W_MAX_TIME]) w->h[W_MAX_TIME] = time;
    if (!w->h[W_FIRST_ORDINAL]) w->h[W_FIRST_ORDINAL] = r.ordinal;
    w->h[W_LAST_ORDINAL] = r.ordinal;
    if (event == X4_TRACE_GAP_BEGIN) {
        ++w->h[W_BEGINS]; w->h[W_LAST_GAP] = a; w->h[W_GAP_OPEN] = 1;
    } else if (event == X4_TRACE_GAP_CLOSED) {
        ++w->h[W_CLOSES]; w->h[W_LAST_GAP] = a;
        w->h[W_LAST_CLOSED] = time; w->h[W_POST_DEADLINE] = time + POST_US;
        w->h[W_GAP_OPEN] = 0;
    }
    return true;
}
static bool open_window_locked(X4Trace *t, uint64_t id, uint64_t start, uint64_t now)
{
    if (t->current_window >= 0) return true;
    /* A frozen/saturated context is never reopened for the same pause. */
    for(unsigned i=0;i<t->window_count;++i)
        if(id>=t->windows[i].h[W_FIRST_GAP] && id<=t->windows[i].h[W_LAST_GAP])return false;
    if(t->last_attempted_gap==id)return false;
    t->last_attempted_gap=id;
    if (t->window_count == WINDOWS) {
        ++t->storage_exhausted; t->coverage |= X4_TRACE_C_STORAGE_CAP; return false;
    }
    unsigned index = t->window_count++;
    TraceWindow *w = &t->windows[index];
    w->h[W_ID] = index + 1; w->h[W_PRE_START] = start > PRE_US ? start - PRE_US : 0;
    w->h[W_FIRST_GAP] = w->h[W_LAST_GAP] = id; w->h[W_GAP_START] = start;
    w->h[W_OPENED] = now; w->h[W_OVERWRITES] = t->history_overwrites;
    w->h[W_DROPS_OPEN] = drop_count(t);
    w->h[W_CLOSED_OPEN] = atomic_load_explicit(&t->closed_count, memory_order_relaxed);
    w->h[W_ONGOING_OPEN] = t->ongoing;
    w->h[W_PRE_US] = PRE_US; w->h[W_POST_US] = POST_US;
    uint64_t earliest = UINT64_MAX;
    unsigned oldest = (t->history_head + HISTORY - t->history_count) % HISTORY;
    unsigned matching=0;
    for(unsigned n=0;n<t->history_count;++n){
        const X4TraceRecord*r=&t->history[(oldest+n)%HISTORY];
        if(r->t_us>=w->h[W_PRE_START] && r->t_us<=now)++matching;
    }
    unsigned omit=matching>PRE_RECORD_LIMIT?matching-PRE_RECORD_LIMIT:0;
    w->h[W_OMITTED]=omit;
    for (unsigned n = 0; n < t->history_count; ++n) {
        const X4TraceRecord *r = &t->history[(oldest + n) % HISTORY];
        if (r->t_us < earliest) earliest = r->t_us;
        if (r->t_us < w->h[W_PRE_START] || r->t_us > now) continue;
        if(omit){--omit;continue;}
        w->records[w->h[W_COUNT]++] = *r;
        if (!w->h[W_MIN_TIME] || r->t_us < w->h[W_MIN_TIME]) w->h[W_MIN_TIME] = r->t_us;
        if (r->t_us > w->h[W_MAX_TIME]) w->h[W_MAX_TIME] = r->t_us;
        if (!w->h[W_FIRST_ORDINAL]) w->h[W_FIRST_ORDINAL] = r->ordinal;
        w->h[W_LAST_ORDINAL] = r->ordinal;
    }
    w->h[W_PRE_RECORDS] = w->h[W_COUNT];
    if (earliest == UINT64_MAX || earliest > w->h[W_PRE_START] || w->h[W_OMITTED]) w->h[W_FLAGS] |= X4_TRACE_C_SHORT_PRE;
    if (w->h[W_DROPS_OPEN]) w->h[W_FLAGS] |= X4_TRACE_C_RECORD_DROP;
    t->current_window = (int)index;
    append_locked(t, now, X4_TRACE_WINDOW_OPEN, 0, index + 1, id);
    return true;
}
static void process_cadence_locked(X4Trace *t, uint64_t now)
{
    Cadence c;
    if (!read_cadence(t, &c)) { t->coverage |= X4_TRACE_C_SNAPSHOT_UNSTABLE; return; }
    if (c.closed_count > t->seen_closed) {
        uint64_t difference = c.closed_count - t->seen_closed;
        if (difference > 1) {
            t->missing_details += difference - 1;
            t->coverage |= X4_TRACE_C_MISSING_GAP_DETAIL;
        }
        t->seen_closed = c.closed_count;
        if (c.gap_id && c.gap_id != t->seen_closed_id && c.gap_end >= c.gap_start) {
            bool observed = t->observed_open_id == c.gap_id;
            uint16_t flags = X4_TRACE_F_RECONSTRUCTED;
            if (!observed) {
                ++t->late_closed; flags |= X4_TRACE_F_LATE_DETECTION;
                open_window_locked(t, c.gap_id, c.gap_start, now);
                append_locked(t, now, X4_TRACE_GAP_BEGIN, flags, c.gap_id, c.gap_start);
            }
            /* If an observed window already hit a cap, do not silently create
             * a second complete-looking window for the same interval. */
            if (append_locked(t, c.gap_end, X4_TRACE_GAP_CLOSED, flags,
                              c.gap_id, c.gap_end - c.gap_start)) ++t->retained_closed;
            /* The latest exact close also proves an older observed interval
             * has a later NEW completion. Its omitted boundary stays counted
             * as missing detail; do not later label it open at session stop. */
            if (t->open_gap_id && t->open_gap_id <= c.gap_id &&
                t->open_gap_id < c.serial) t->open_gap_id = 0;
            t->seen_closed_id = c.gap_id;
        } else { ++t->missing_details; t->coverage |= X4_TRACE_C_MISSING_GAP_DETAIL; }
    }
    if (!c.active) {
        if (t->open_gap_id) {
            ++t->open_at_stop; t->open_gap_id = 0; t->coverage |= X4_TRACE_C_OPEN_AT_STOP;
        }
        freeze_locked(t, now, X4_TRACE_R_SESSION_END); return;
    }
    if (c.last_us && now > c.last_us && now - c.last_us > GAP_US &&
        c.serial && t->observed_open_id != c.serial) {
        t->observed_open_id = t->open_gap_id = c.serial; ++t->ongoing;
        uint64_t excess = now - c.last_us - GAP_US;
        if (excess > t->detection_excess) t->detection_excess = excess;
        open_window_locked(t, c.serial, c.last_us, now);
        append_locked(t, now, X4_TRACE_GAP_BEGIN, 0, c.serial, c.last_us);
    }
    if (t->current_window >= 0) {
        TraceWindow *w = &t->windows[t->current_window];
        if (now >= w->h[W_OPENED] && now - w->h[W_OPENED] >= WINDOW_US)
            freeze_locked(t, now, X4_TRACE_R_TIME_CAP);
        else if (!w->h[W_GAP_OPEN] && w->h[W_POST_DEADLINE] && now >= w->h[W_POST_DEADLINE])
            freeze_locked(t, now, X4_TRACE_R_NONE);
    }
}
static void record_at(X4Trace *t, uint64_t time, uint16_t event,
                      uint16_t flags, uint64_t a, uint64_t b)
{
    if (!t) return;
    /* This cumulative hook and its ring have no dependency on the legacy
     * admission gate, capture windows, main poll or video-owner progress. */
    x4_monitor_record(t->monitor, time, event, flags, a, b);
    switch (event) {
    case X4_TRACE_DECODE_BEGIN: x4_monitor_state(t->monitor, X4_MON_ACTOR_WORKER, X4_MON_PHASE_DECODE, time); break;
    case X4_TRACE_COPY_BEGIN: x4_monitor_state(t->monitor, X4_MON_ACTOR_WORKER, X4_MON_PHASE_COPY, time); break;
    case X4_TRACE_CONVERT_BEGIN: x4_monitor_state(t->monitor, X4_MON_ACTOR_WORKER, X4_MON_PHASE_CONVERT, time); break;
    case X4_TRACE_DECODE_END: case X4_TRACE_COPY_END: case X4_TRACE_CONVERT_END:
        x4_monitor_state(t->monitor, X4_MON_ACTOR_WORKER, X4_MON_PHASE_UNKNOWN, time); break;
    case 0x224: x4_monitor_state(t->monitor, X4_MON_ACTOR_WORKER, X4_MON_PHASE_COPY_WAIT, time); break;
    case 0x225: x4_monitor_state(t->monitor, X4_MON_ACTOR_WORKER, X4_MON_PHASE_UNKNOWN, time); break;
    case 0x222: case 0x223: {
        unsigned index = flags >> 8;
        if (index < 3) x4_monitor_state(t->monitor, X4_MON_ACTOR_HELPER0 + index,
            event == 0x222 ? X4_MON_PHASE_COPY : X4_MON_PHASE_WAIT_DATA, time);
        break;
    }
    default: break;
    }
    unsigned stage = stage_of(event);
    count_attempt(t, event, b);
    if (atomic_flag_test_and_set_explicit(&t->gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&t->dropped[stage], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->event_dropped[event_index(event)], 1, memory_order_relaxed); return;
    }
    if (!t->quiesced) {
        process_cadence_locked(t, x4_trace_now_us());
        append_admitted_locked(t, time, event, flags, a, b);
    }
    atomic_flag_clear_explicit(&t->gate, memory_order_release);
}

X4Trace *x4_trace_create(uint64_t ordinal, unsigned configured_readers)
{
    if (configured_readers != 2 && configured_readers != 4) return NULL;
    X4Trace *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    atomic_flag_clear(&t->gate);
    for (unsigned i = 0; i < STAGES; ++i) {
        atomic_init(&t->attempted[i], 0); atomic_init(&t->admitted[i], 0); atomic_init(&t->dropped[i], 0);
    }
    for (unsigned i = 0; i < EVENTS; ++i) {
        atomic_init(&t->event_attempted[i], 0); atomic_init(&t->event_admitted[i], 0);
        atomic_init(&t->event_dropped[i], 0);
    }
    for (unsigned i = 0; i < RESET_REASONS; ++i) atomic_init(&t->reset_primary[i], 0);
    atomic_init(&t->monitor_skips, 0); atomic_init(&t->unstable, 0); atomic_init(&t->identity_errors, 0);
    atomic_init(&t->version, 0); atomic_init(&t->serial, 0); atomic_init(&t->last_us, 0);
    atomic_init(&t->generation, 0); atomic_init(&t->closed_count, 0);
    atomic_init(&t->gap_id, 0); atomic_init(&t->gap_start, 0); atomic_init(&t->gap_end, 0);
    atomic_init(&t->active, false);
    t->local_session = ordinal; t->configured_readers = configured_readers; t->current_window = -1;
    t->monitor = x4_monitor_create(ordinal, configured_readers);
    x4_trace_record(t, X4_TRACE_SESSION_START, 0, ordinal, configured_readers);
    return t;
}
void x4_trace_record(X4Trace *t, uint16_t event, uint16_t flags, uint64_t a, uint64_t b)
{
    if (t) record_at(t, x4_trace_now_us(), event, flags, a, b);
}
void x4_trace_poll(X4Trace *t)
{
    if (!t) return;
    if (atomic_flag_test_and_set_explicit(&t->gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&t->monitor_skips, 1, memory_order_relaxed); return;
    }
    if (!t->quiesced) process_cadence_locked(t, x4_trace_now_us());
    atomic_flag_clear_explicit(&t->gate, memory_order_release);
}
void x4_trace_set_active(X4Trace *t, bool active)
{
    if (!t || t->main_active == active) return;
    x4_monitor_active(t->monitor, active, x4_trace_now_us());
    t->main_active = active; t->main_last_us = 0; t->main_generation = 0;
    publish_cadence(t);
    x4_trace_record(t, X4_TRACE_ACTIVE_CHANGE, 0, active, t->main_serial);
}
void x4_trace_present_complete(X4Trace *t, uint64_t generation, bool fresh,
                               uint64_t status_num, uint64_t flip_arg)
{
    if (!t) return;
    uint64_t now = x4_trace_now_us();
    x4_monitor_present(t->monitor, generation, fresh, now);
    bool real_new = fresh && generation && t->main_active;
    if (real_new && t->main_last_us && (generation <= t->main_generation || now < t->main_last_us)) {
        atomic_fetch_add_explicit(&t->identity_errors, 1, memory_order_relaxed); real_new = false;
    }
    if (real_new) {
        uint64_t previous = t->main_last_us;
        ++t->main_serial;
        if (previous && now - previous > GAP_US) {
            ++t->main_closed_count; t->main_gap_id = t->main_serial - 1;
            t->main_gap_start = previous; t->main_gap_end = now;
        }
        t->main_last_us = now; t->main_generation = generation;
        t->main_last_ever_us = now; if (!t->main_first_us) t->main_first_us = now;
        publish_cadence(t);
    }
    uint16_t flags = real_new ? X4_TRACE_F_NEW : generation ? X4_TRACE_F_REPEAT : X4_TRACE_F_UI;
    record_at(t, now, X4_TRACE_FLIP_MATCH, flags, flip_arg, status_num);
    record_at(t, now, real_new ? X4_TRACE_PRESENT_NEW : X4_TRACE_PRESENT_REPEAT,
              flags, generation, t->main_serial);
}

void x4_trace_end_session(X4Trace *t)
{
    if (!t || t->quiesced || !x4_monitor_stopped(t->monitor)) return;
    /* Caller proved quiescence. No lock/spin or new native lifetime here. */
    uint64_t now = x4_trace_now_us();
    process_cadence_locked(t, now);
    freeze_locked(t, now, X4_TRACE_R_SESSION_END);
    append_locked(t, now, X4_TRACE_SESSION_END, X4_TRACE_REASON(X4_TRACE_R_SESSION_END),
                  t->main_serial, t->main_closed_count);
    t->main_active = false; publish_cadence(t);
    if (drop_count(t)) t->coverage |= X4_TRACE_C_RECORD_DROP;
    if (atomic_load_explicit(&t->unstable, memory_order_relaxed)) t->coverage |= X4_TRACE_C_SNAPSHOT_UNSTABLE;
    if (atomic_load_explicit(&t->identity_errors, memory_order_relaxed)) t->coverage |= X4_TRACE_C_IDENTITY;
    t->quiesced = true;
    x4_monitor_end(t->monitor);
}
static bool write_items(FILE *file, const void *p, size_t size, size_t count)
{
    return !count || fwrite(p, size, count, file) == count;
}
int x4_trace_dump_file(X4Trace *t, const char *path)
{
    if (!t) return X4_TRACE_DUMP_DISABLED;
    if (!path || !*path) return X4_TRACE_DUMP_ARGUMENT;
    if (!t->quiesced) return X4_TRACE_DUMP_NOT_QUIESCED;
    /* C11 exclusive-create mode: never replace an earlier numeric trace. */
    FILE *file = fopen(path, "wbx");
    if (!file) return X4_TRACE_DUMP_IO;
    uint64_t h[32] = {
        UINT64_C(0x3145434152543458), 1, UINT64_C(0x0102030405060708),
        256, sizeof(X4TraceRecord), 256, t->local_session, t->configured_readers, WINDOWS, t->window_count,
        HISTORY, t->history_count, t->total_records, t->history_overwrites,
        t->main_closed_count, t->ongoing, t->retained_closed, t->late_closed,
        t->missing_details, t->storage_exhausted, drop_count(t),
        atomic_load_explicit(&t->monitor_skips, memory_order_relaxed),
        atomic_load_explicit(&t->unstable, memory_order_relaxed),
        t->event_caps, t->time_caps, t->open_at_stop, t->main_first_us, t->main_last_ever_us,
        t->detection_excess, t->coverage, STAGES, ((uint64_t)EVENTS << 32) | RESET_REASONS
    };
    bool ok = write_items(file, h, sizeof(uint64_t), 32);
    for (unsigned i = 0; ok && i < STAGES; ++i) {
        uint64_t counters[4] = {
            atomic_load_explicit(&t->attempted[i], memory_order_relaxed),
            atomic_load_explicit(&t->admitted[i], memory_order_relaxed),
            atomic_load_explicit(&t->dropped[i], memory_order_relaxed), 0
        };
        ok = write_items(file, counters, sizeof(uint64_t), 4);
    }
    for (unsigned i = 0; ok && i < EVENTS; ++i) {
        uint64_t counters[3] = {
            atomic_load_explicit(&t->event_attempted[i], memory_order_relaxed),
            atomic_load_explicit(&t->event_admitted[i], memory_order_relaxed),
            atomic_load_explicit(&t->event_dropped[i], memory_order_relaxed)
        };
        ok = write_items(file, counters, sizeof(uint64_t), 3);
    }
    for (unsigned i = 0; ok && i < RESET_REASONS; ++i) {
        uint64_t counter = atomic_load_explicit(&t->reset_primary[i], memory_order_relaxed);
        ok = write_items(file, &counter, sizeof(counter), 1);
    }
    for (unsigned i = 0; ok && i < t->window_count; ++i) {
        const TraceWindow *w = &t->windows[i];
        ok = write_items(file, w->h, sizeof(uint64_t), 32) &&
             write_items(file, w->records, sizeof(X4TraceRecord), (size_t)w->h[W_COUNT]);
    }
    unsigned oldest = (t->history_head + HISTORY - t->history_count) % HISTORY;
    unsigned first = t->history_count < HISTORY - oldest ? t->history_count : HISTORY - oldest;
    if (ok) ok = write_items(file, t->history + oldest, sizeof(X4TraceRecord), first) &&
                 write_items(file, t->history, sizeof(X4TraceRecord), t->history_count - first);
    if (fclose(file) != 0) ok = false;
    return ok ? X4_TRACE_DUMP_OK : X4_TRACE_DUMP_IO;
}
bool x4_trace_free(X4Trace *t)
{
    if (!t) return true;
    if (!t->quiesced) return false;
    if (!x4_monitor_free(t->monitor)) return false;
    free(t); return true;
}

bool x4_trace_monitor_bind_queue(X4Trace *t, const X4VideoIngress *q)
{ return t && x4_monitor_bind_queue(t->monitor, q); }
bool x4_trace_monitor_config(X4Trace *t, const X4MonitorConfig *c)
{ return t && x4_monitor_config(t->monitor, c); }
int x4_trace_monitor_start(X4Trace *t) { return x4_monitor_start(t ? t->monitor : NULL); }
int x4_trace_monitor_stop(X4Trace *t) { return x4_monitor_stop(t ? t->monitor : NULL); }
int x4_trace_monitor_dump(X4Trace *t, const char *path)
{ return x4_monitor_dump(t ? t->monitor : NULL, path); }
void x4_trace_monitor_rx(X4Trace *t, unsigned k, bool v, uint32_t s, uint16_t q, size_t b, uint64_t time)
{ x4_monitor_rx(t ? t->monitor : NULL, k, v, s, q, b, time); }
void x4_trace_monitor_transport_packet(X4Trace *t, unsigned stage, bool valid,
    uint32_t ssrc, uint16_t seq, unsigned pt, size_t bytes, uint64_t time,
    uint64_t prior, uint32_t flags)
{ x4_monitor_transport_packet(t ? t->monitor : NULL, stage, valid, ssrc, seq,
    pt, bytes, time, prior, flags); }
void x4_trace_monitor_transport_reject(X4Trace *t, unsigned stage, unsigned reason,
    int rc, bool valid, uint32_t ssrc, uint16_t seq, uint64_t time)
{ x4_monitor_transport_reject(t ? t->monitor : NULL, stage, reason, rc,
    valid, ssrc, seq, time); }
void x4_trace_monitor_critical(X4Trace *t, uint64_t time, unsigned event,
    unsigned stage, uint32_t flags, uint64_t identity, uint64_t source,
    uint64_t a, uint64_t b, uint64_t c)
{ x4_monitor_critical(t ? t->monitor : NULL,time,event,stage,flags,identity,source,a,b,c); }
void x4_trace_monitor_queue(X4Trace *t, uint32_t d, uint64_t o, bool v)
{ x4_monitor_queue(t ? t->monitor : NULL, d, o, v); }
void x4_trace_monitor_state(X4Trace *t, unsigned a, unsigned p, uint64_t b)
{ x4_monitor_state(t ? t->monitor : NULL, a, p, b); }
void x4_trace_monitor_epoch(X4Trace *t, unsigned a, unsigned e)
{ x4_monitor_epoch(t ? t->monitor : NULL, a, e); }
