/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_monitor.h"
#include "live_trace.h"
#ifndef X4_MONITOR_HOST_TEST
#include <orbis/libkernel.h>
#include "../core/build_identity.h"
typedef OrbisPthread X4MonitorThread;
#else
#define X4_BUILD_ID "host-instrumentation-only"
#define X4_PRODUCT_VERSION "host-instrumentation-only"
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PRE_US = 500000, POST_US = 500000, WINDOW_US = 2000000 };
enum { C_SAMPLE = 1, C_CADENCE = 2, C_HISTORY = 4, C_WINDOWS = 8,
    C_DROP = 16, C_UNSTABLE = 32, C_TIME = 64, C_DISABLED = 128,
    C_JOIN = 256, C_ACTOR = 512, C_SEQUENCE = 1024, C_CLOCK = 2048,
    C_EVENTS = 4096, C_OPEN = 8192, C_SHORT = 16384, C_EPOCH = 32768 };
typedef struct {
    atomic_uint_fast64_t version, phase, epoch, begin_us;
} Actor;
typedef struct {
    atomic_uint_fast64_t callbacks, valid, bytes, last_us, last_valid_us;
} RxKind;
typedef struct {
    atomic_uint_fast64_t key, callbacks, bytes, last_us, version, skipped,
        backward, unknown, first_us;
    atomic_uint last_seq;
    atomic_bool dirty;
    atomic_flag sequence_gate;
} RxSource;
typedef struct {
    uint64_t h[X4_MON_WINDOW_HEADER_WORDS];
    X4MonitorRecord records[X4_MON_WINDOW_RECORD_CAP];
    uint64_t last_sampled[512];
    unsigned end_head, end_count;
    X4MonitorRecord end_boundary;
    bool has_end_boundary;
} Context;
typedef struct {
    uint64_t id, time, seen, extended_expected;
    uint16_t expected, count;
} MissingRange;
typedef struct {
    atomic_uint_fast64_t key, count, bytes, last, skipped, gaps, backward,
        duplicates, omissions, late, evicted, ambiguous;
    atomic_flag gate;
    atomic_bool dirty;
    uint64_t first, sequence_time, extended_sequence;
    uint16_t sequence;
    bool initialized;
    unsigned range_next;
    MissingRange ranges[X4_MON_T_RANGE_CAP];
} TransportSource;
struct X4Monitor {
    atomic_flag gate;
    atomic_bool enabled, stopping;
    atomic_uint_fast64_t event_attempt[X4_MON_EVENT_CAP], event_drop[X4_MON_EVENT_CAP];
    atomic_uint_fast64_t reset[X4_MON_RESET_CAP], errors_pli, source_omitted,
        seq_omitted, actor_unstable, queue_unstable, cadence_unstable, queue_accepted,
        queue_rejected[6], rtp_rejected[8], last_progress[6], reorder_state,
        rx_sample_unstable;
    Actor actors[X4_MON_ACTORS];
    RxKind rx[X4_MON_RX_KINDS];
    RxSource sources[X4_MON_RX_SOURCE_CAP];
    atomic_uint_fast64_t primary_video_key;
    atomic_uint_fast64_t supplied_depth, supplied_oldest;
    atomic_bool supplied_valid;
    /* Main owns these atomic cadence publications and the ordinary ledger. */
    atomic_uint_fast64_t cadence_version, serial, last_new, generation, closed;
    atomic_bool active;
    uint64_t main_serial, main_last, main_generation, main_closed, first_new,
        main_last_ever, intervals, identity_errors, cadence_omitted, active_us,
        exposure_begin, manual_marks, clock_errors, interval_epoch_revision,
        epoch_omitted;
    unsigned main_epoch, interval_begin_epoch;
    bool main_active;
    uint64_t epoch_exposure[4], epoch_intervals[4], histogram[X4_MON_HIST_CAP];
    X4MonitorEpoch epochs[X4_MON_EPOCH_CAP];
    unsigned epoch_count;
    X4MonitorCadence cadence[X4_MON_CADENCE_CAP];
    unsigned cadence_count;
    /* Queue pointer/config are immutable from start until successful join. */
    const X4VideoIngress *queue;
    X4MonitorConfig config;
    X4MonitorThread thread;
    bool created, start_called, ended;
    int start_rc, join_rc;
    uint64_t session, created_us, ended_us;
    /* Sample fields: sampler-owned until join. */
    X4MonitorSample samples[X4_MON_SAMPLE_CAP];
    unsigned sample_count;
    uint64_t sample_omitted, sample_total, missed_periods, max_lateness, supplied_samples,
        sampler_clock_errors;
    atomic_uint_fast64_t sampler_gate_omitted;
    /* All history/context fields below are protected by the try-only gate. */
    X4MonitorRecord history[X4_MON_HISTORY_CAP];
    Context windows[X4_MON_WINDOW_CAP];
    unsigned history_head, history_count, window_count;
    int current_window;
    uint64_t history_overwrites, window_omitted, record_ordinal, coverage,
        seen_gap_serial, retained_gaps, open_at_stop, last_window_gap,
        duplicate_window_opens, window_retention_omissions;
    /* Independent sparse critical ledger: high-rate general event admission
     * cannot monopolize this gate. Counters survive gate and storage losses. */
    atomic_flag critical_gate;
    atomic_uint_fast64_t critical_attempt[X4_MON_CR_EVENT_CAP],
        critical_omit[X4_MON_CR_EVENT_CAP], critical_gate_omit, critical_cap_omit,
        transport[X4_MON_T_STAGES][X4_MON_T_COUNTER_WORDS], transport_reject[X4_MON_T_STAGES][32],
        transport_source_omit, transport_sequence_omit, critical_ids;
    atomic_uint_fast64_t delay_max_omit[X4_MON_T_STAGES];
    atomic_uint_fast64_t au_filter[8];
    TransportSource transport_sources[X4_MON_T_STAGES][X4_MON_T_SOURCE_CAP];
    X4MonitorCritical critical[X4_MON_CR_CAP];
    unsigned critical_count;
    uint64_t critical_ordinal;
};
_Static_assert(sizeof(X4Monitor) <= 12 * 1024 * 1024, "bounded independent monitor");
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "monitor requires lock-free counters");

#ifdef X4_MONITOR_TEST_HOOKS
extern void x4_monitor_test_hook(unsigned point, void *context, uint64_t a, uint64_t b);
#define TEST_HOOK(p,c,a,b) x4_monitor_test_hook(p,c,a,b)
#else
#define TEST_HOOK(p,c,a,b) ((void)0)
#endif
uint64_t x4_monitor_now_us(void)
{
#ifdef X4_MONITOR_HOST_TEST
    return x4_monitor_host_now_us();
#else
    return sceKernelGetProcessTime();
#endif
}
static void sleep_us(uint64_t us)
{
#ifdef X4_MONITOR_HOST_TEST
    x4_monitor_host_sleep_us(us);
#else
    sceKernelUsleep((unsigned)(us > X4_MON_PERIOD_US ? X4_MON_PERIOD_US : us));
#endif
}
static unsigned event_index(uint16_t event)
{
    unsigned stage = event >> 8, item = event & 255;
    return stage < 8 && item < 64 ? stage * 64 + item : 63;
}
static uint64_t attempts(const X4Monitor *m, uint16_t event)
{ return atomic_load_explicit(&m->event_attempt[event_index(event)], memory_order_relaxed); }
static uint64_t total(const atomic_uint_fast64_t *a, unsigned n)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; ++i) sum += atomic_load_explicit(a + i, memory_order_relaxed);
    return sum;
}
void x4_monitor_critical(X4Monitor *m, uint64_t time, unsigned event,
    unsigned stage, uint32_t flags, uint64_t identity, uint64_t source,
    uint64_t a, uint64_t b, uint64_t c)
{
    if (!m || !atomic_load_explicit(&m->enabled, memory_order_relaxed)) return;
    if (event >= X4_MON_CR_EVENT_CAP) event = 0;
    atomic_fetch_add_explicit(&m->critical_attempt[event], 1, memory_order_relaxed);
    if (atomic_flag_test_and_set_explicit(&m->critical_gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&m->critical_gate_omit, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&m->critical_omit[event], 1, memory_order_relaxed); return;
    }
    unsigned slot = (unsigned)(m->critical_ordinal % X4_MON_CR_CAP);
    if (m->critical_count == X4_MON_CR_CAP) {
        unsigned old = (unsigned)m->critical[slot].w[2] & 0xffffu;
        if (old >= X4_MON_CR_EVENT_CAP) old = 0;
        atomic_fetch_add_explicit(&m->critical_cap_omit, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&m->critical_omit[old], 1, memory_order_relaxed);
    } else ++m->critical_count;
    X4MonitorCritical r = {{time, identity, (event & 0xffffu) |
        ((uint64_t)(stage & 0xffffu) << 16) | ((uint64_t)flags << 32),
        source, a, b, c, ++m->critical_ordinal}};
    m->critical[slot] = r;
    atomic_flag_clear_explicit(&m->critical_gate, memory_order_release);
}
void x4_monitor_transport_reject(X4Monitor *m, unsigned stage, unsigned reason,
    int rc, bool valid, uint32_t ssrc, uint16_t seq, uint64_t time)
{
    if (!m || stage >= X4_MON_T_STAGES || !atomic_load_explicit(&m->enabled, memory_order_relaxed)) return;
    if (reason >= 32) reason = 31;
    atomic_fetch_add_explicit(&m->transport_reject[stage][reason], 1, memory_order_relaxed);
    /* Expected socket drain/fairness observations are counters, not rejects
     * retained in the sparse ledger. Otherwise every poll can exhaust it. */
    if (reason == 25 || reason == 26) return;
    x4_monitor_critical(m,time,X4_MON_CR_REJECT,stage,valid?1u:0u,0,
        valid?ssrc:0,reason,(uint32_t)rc,valid?seq:UINT64_MAX);
}
void x4_monitor_transport_packet(X4Monitor *m, unsigned stage, bool valid,
    uint32_t ssrc, uint16_t seq, unsigned pt, size_t bytes, uint64_t time,
    uint64_t prior, uint32_t flags)
{
    if (!m || stage >= X4_MON_T_STAGES || !atomic_load_explicit(&m->enabled, memory_order_relaxed)) return;
    atomic_uint_fast64_t *counts = m->transport[stage];
    atomic_fetch_add_explicit(counts+0,1,memory_order_relaxed);
    atomic_fetch_add_explicit(counts+1,bytes,memory_order_relaxed);
    atomic_store_explicit(counts+2,time,memory_order_relaxed);
    atomic_fetch_add_explicit(counts+(valid?3:4),1,memory_order_relaxed);
    if (prior && time >= prior) {
        atomic_fetch_add_explicit(counts+14,1,memory_order_relaxed);
        uint64_t value = atomic_load_explicit(counts+15,memory_order_relaxed);
        if (time-prior > value && !atomic_compare_exchange_strong_explicit(counts+15,
            &value,time-prior,memory_order_relaxed,memory_order_relaxed))
            atomic_fetch_add_explicit(&m->delay_max_omit[stage],1,memory_order_relaxed);
        if(time-prior>10000)x4_monitor_critical(m,time,X4_MON_CR_TRANSPORT_DELAY,
            stage,flags,0,valid?((uint64_t)(pt&127u)<<32)|ssrc:0,
            valid?seq:UINT64_MAX,prior,time-prior);
    }
    if (!valid) return;
    uint64_t key = (UINT64_C(1)<<63) | ((uint64_t)(pt&127u)<<32) | ssrc;
    TransportSource *s = NULL;
    for (unsigned i=0;i<X4_MON_T_SOURCE_CAP;++i) {
        uint64_t observed=atomic_load_explicit(&m->transport_sources[stage][i].key,memory_order_acquire);
        if (!observed) {
            uint64_t empty=0;
            if (atomic_compare_exchange_strong_explicit(&m->transport_sources[stage][i].key,
                &empty,key,memory_order_acq_rel,memory_order_relaxed)) observed=key;
            else observed=empty;
        }
        if (observed==key) { s=&m->transport_sources[stage][i]; break; }
    }
    if (!s) {
        atomic_fetch_add_explicit(counts+10,1,memory_order_relaxed);
        atomic_fetch_add_explicit(&m->transport_source_omit,1,memory_order_relaxed); return;
    }
    atomic_fetch_add_explicit(&s->count,1,memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes,bytes,memory_order_relaxed);
    atomic_store_explicit(&s->last,time,memory_order_relaxed);
    if (atomic_flag_test_and_set_explicit(&s->gate,memory_order_acquire)) {
        atomic_store_explicit(&s->dirty,true,memory_order_release);
        atomic_fetch_add_explicit(&s->omissions,1,memory_order_relaxed);
        atomic_fetch_add_explicit(counts+9,1,memory_order_relaxed);
        atomic_fetch_add_explicit(&m->transport_sequence_omit,1,memory_order_relaxed); return;
    }
    if (!s->first) s->first=time;
    bool dirty=atomic_exchange_explicit(&s->dirty,false,memory_order_acq_rel);
    if (!s->initialized || dirty) {
        if (dirty) { atomic_fetch_add_explicit(&s->ambiguous,1,memory_order_relaxed);
            atomic_fetch_add_explicit(counts+13,1,memory_order_relaxed);
            for(unsigned i=0;i<X4_MON_T_RANGE_CAP;++i)if(s->ranges[i].id){
                s->ranges[i].id=0;atomic_fetch_add_explicit(&s->evicted,1,memory_order_relaxed);
                atomic_fetch_add_explicit(counts+11,1,memory_order_relaxed);
            }
        }
        s->extended_sequence=seq;
        s->sequence=seq; s->sequence_time=time; s->initialized=true;
        atomic_flag_clear_explicit(&s->gate,memory_order_release); return;
    }
    uint16_t diff=(uint16_t)(seq-s->sequence);
    if (diff==0) {
        atomic_fetch_add_explicit(&s->duplicates,1,memory_order_relaxed);
        atomic_fetch_add_explicit(counts+8,1,memory_order_relaxed);
    } else if (diff==0x8000u) {
        atomic_fetch_add_explicit(&s->ambiguous,1,memory_order_relaxed);
        atomic_fetch_add_explicit(counts+13,1,memory_order_relaxed);
        x4_monitor_critical(m,time,X4_MON_CR_SEQUENCE_AMBIGUOUS,stage,flags,0,key,s->sequence,seq,diff);
        for(unsigned i=0;i<X4_MON_T_RANGE_CAP;++i)if(s->ranges[i].id){
            s->ranges[i].id=0;atomic_fetch_add_explicit(&s->evicted,1,memory_order_relaxed);
            atomic_fetch_add_explicit(counts+11,1,memory_order_relaxed);
        }
        s->extended_sequence=seq;
        s->sequence=seq; s->sequence_time=time;
    } else if (diff>0x8000u) {
        atomic_fetch_add_explicit(&s->backward,1,memory_order_relaxed);
        atomic_fetch_add_explicit(counts+7,1,memory_order_relaxed);
        /* A bounded watch of recent missing ranges identifies later positions.
         * For offsets >=64, uniqueness cannot be asserted and flag31 says so. */
        for (unsigned i=0;i<X4_MON_T_RANGE_CAP;++i) {
            MissingRange *r=&s->ranges[i];
            uint16_t backward=(uint16_t)(s->sequence-seq);
            if(s->extended_sequence<backward)continue;
            uint64_t extended=s->extended_sequence-backward;
            if(!r->id || extended<r->extended_expected ||
                extended-r->extended_expected>=r->count)continue;
            unsigned offset=(unsigned)(extended-r->extended_expected);
            bool unknown=offset>=64, repeat=!unknown && (r->seen&(UINT64_C(1)<<offset));
            if (!unknown) r->seen|=UINT64_C(1)<<offset;
            atomic_fetch_add_explicit(&s->late,1,memory_order_relaxed);
            atomic_fetch_add_explicit(counts+12,1,memory_order_relaxed);
            x4_monitor_critical(m,time,X4_MON_CR_LATE_POSITION,stage,
                flags|(repeat?UINT32_C(1)<<30:0)|(unknown?UINT32_C(1)<<31:0),r->id,key,
                extended,r->extended_expected,r->time); break;
        }
    } else {
        if (diff>1) {
            uint16_t expected=(uint16_t)(s->sequence+1), missing=(uint16_t)(diff-1);
            uint64_t id=atomic_fetch_add_explicit(&m->critical_ids,1,memory_order_relaxed)+1;
            MissingRange *r=&s->ranges[s->range_next++%X4_MON_T_RANGE_CAP];
            if (r->id) { atomic_fetch_add_explicit(&s->evicted,1,memory_order_relaxed);
                atomic_fetch_add_explicit(counts+11,1,memory_order_relaxed); }
            *r=(MissingRange){id,time,0,s->extended_sequence+1,expected,missing};
            atomic_fetch_add_explicit(&s->gaps,1,memory_order_relaxed);
            atomic_fetch_add_explicit(&s->skipped,missing,memory_order_relaxed);
            atomic_fetch_add_explicit(counts+5,1,memory_order_relaxed);
            atomic_fetch_add_explicit(counts+6,missing,memory_order_relaxed);
            x4_monitor_critical(m,time,X4_MON_CR_SEQUENCE_GAP,stage,flags,id,key,
                s->extended_sequence+1,s->extended_sequence+diff,missing);
        }
        s->extended_sequence+=diff;s->sequence=seq; s->sequence_time=time;
    }
    atomic_flag_clear_explicit(&s->gate,memory_order_release);
}
static void freeze_locked(X4Monitor *m, uint64_t time, unsigned reason)
{
    if (m->current_window < 0) return;
    Context *w = &m->windows[m->current_window];
    w->h[7] = time; w->h[8] = reason;
    w->h[10] = total(m->event_drop, X4_MON_EVENT_CAP);
    if (w->h[10] != w->h[9]) w->h[2] |= C_DROP;
    if (reason == 2) w->h[2] |= C_TIME;
    if (reason == 3) w->h[2] |= C_EVENTS;
    if (reason == 4 && !w->h[6]) { w->h[2] |= C_OPEN; ++m->open_at_stop; }
    /* Compact the rolling final region only after producers can no longer
     * append to this window. An in-place cycle uses fixed stack storage. */
    if (w->end_count || w->has_end_boundary) {
        X4MonitorRecord end[X4_MON_WINDOW_END_CAP];
        unsigned first=w->end_count==X4_MON_WINDOW_END_CAP-1?w->end_head:0;
        unsigned offset=w->has_end_boundary?1u:0u;
        if(w->has_end_boundary)end[0]=w->end_boundary;
        for(unsigned i=0;i<w->end_count;++i)
            end[offset+i]=w->records[X4_MON_WINDOW_BODY_CAP+(first+i)%(X4_MON_WINDOW_END_CAP-1)];
        for(unsigned i=0;i<w->end_count+offset;++i)w->records[w->h[1]+i]=end[i];
        w->h[1]+=w->end_count+offset; w->end_count=w->end_head=0;w->has_end_boundary=false;
    }
    m->coverage |= w->h[2];
    m->current_window = -1;
}
static void append_locked(X4Monitor *m, uint64_t time, uint16_t event,
                          uint16_t flags, uint64_t a, uint64_t b)
{
    X4MonitorRecord r = { time, a, b, (uint32_t)++m->record_ordinal, event, flags };
    if (!r.ordinal) m->coverage |= C_UNSTABLE;
    m->history[m->history_head] = r;
    m->history_head = (m->history_head + 1) % X4_MON_HISTORY_CAP;
    if (m->history_count < X4_MON_HISTORY_CAP) ++m->history_count;
    else { ++m->history_overwrites; m->coverage |= C_HISTORY; }
    if (m->current_window < 0) return;
    Context *w = &m->windows[m->current_window];
    /* Prehistory, bounded beginning/development, and a separate rolling end
     * region keep a high event rate from filling a window at opening. */
    unsigned index=event_index(event);
    bool important=event==X4_MON_EVENT_GAP_BEGIN || event==X4_MON_EVENT_GAP_END ||
        event==X4_TRACE_AU_RESET || event==0x126 || event==0x127 ||
        event==X4_TRACE_AU_FILTER_REJECT;
    bool high_rate=event==X4_MON_EVENT_RX_OBSERVATION || event==X4_TRACE_RX_VALID ||
        event==X4_TRACE_QUEUE_POP || event==X4_TRACE_REORDER_INSERT ||
        event==X4_TRACE_REORDER_EMIT || event==X4_TRACE_REORDER_STATE ||
        event==X4_MON_EVENT_PHASE_CHANGE;
    if (!important && high_rate && w->last_sampled[index] &&
        time>=w->last_sampled[index] && time-w->last_sampled[index]<10000) {
        ++w->h[14]; ++m->window_retention_omissions; return;
    }
    w->last_sampled[index]=time;
    if (w->h[6]) {
        if(event==X4_MON_EVENT_GAP_END){w->end_boundary=r;w->has_end_boundary=true;return;}
        unsigned slot=X4_MON_WINDOW_BODY_CAP+w->end_head;
        if(w->end_count==X4_MON_WINDOW_END_CAP-1){++w->h[14];++m->window_retention_omissions;w->h[2]|=C_EVENTS;}
        else ++w->end_count;
        w->records[slot]=r; w->end_head=(w->end_head+1)%(X4_MON_WINDOW_END_CAP-1);
    } else if(w->h[1]<X4_MON_WINDOW_BODY_CAP)w->records[w->h[1]++]=r;
    else {++w->h[14];++m->window_retention_omissions;w->h[2]|=C_EVENTS;}
}
static void open_locked(X4Monitor *m, uint64_t id, uint64_t start, uint64_t time)
{
    if (m->current_window >= 0 && m->windows[m->current_window].h[3]==id) return;
    for(unsigned i=0;i<m->window_count;++i)if(m->windows[i].h[3]==id){
        ++m->duplicate_window_opens; return;
    }
    if (m->last_window_gap==id) { ++m->duplicate_window_opens; return; }
    if (m->current_window >= 0) freeze_locked(m,time,1);
    m->last_window_gap=id;
    if (m->window_count == X4_MON_WINDOW_CAP) {
        ++m->window_omitted; m->coverage |= C_WINDOWS; return;
    }
    Context *w = &m->windows[m->window_count];
    m->current_window = (int)m->window_count++;
    w->h[0] = m->window_count; w->h[3] = w->h[12] = id;
    w->h[4] = start; w->h[5] = time;
    w->h[9] = total(m->event_drop, X4_MON_EVENT_CAP);
    uint64_t min = start > PRE_US ? start - PRE_US : 0;
    unsigned begin = (m->history_head + X4_MON_HISTORY_CAP - m->history_count) % X4_MON_HISTORY_CAP;
    uint64_t first = 0;
    unsigned matching=0;
    for(unsigned i=0;i<m->history_count;++i){
        const X4MonitorRecord *r=&m->history[(begin+i)%X4_MON_HISTORY_CAP];
        if(r->t_us>=min && r->t_us<=time)++matching;
    }
    unsigned omit=matching>X4_MON_WINDOW_PRE_CAP?matching-X4_MON_WINDOW_PRE_CAP:0;
    w->h[14]=omit; m->window_retention_omissions+=omit;
    unsigned selected=matching-omit, seen=0, kept=0, next=0;
    for (unsigned i = 0; i < m->history_count; ++i) {
        const X4MonitorRecord *r = &m->history[(begin + i) % X4_MON_HISTORY_CAP];
        if (!first || r->t_us < first) first = r->t_us;
        if (r->t_us < min || r->t_us>time)continue;
        /* Spread a bounded prehistory sample across the available span rather
         * than retaining only its last few milliseconds at high packet rate. */
        unsigned position=seen++;
        if(kept>=selected || position!=next)continue;
        w->records[w->h[1]++] = *r;
        ++kept;
        next=selected>1?(unsigned)(((uint64_t)kept*(matching-1))/(selected-1)):matching;
    }
    w->h[11] = w->h[1];
    if (!first || first > min || w->h[14]) w->h[2] |= C_SHORT;
}
void x4_monitor_record(X4Monitor *m, uint64_t time, uint16_t event,
                       uint16_t flags, uint64_t a, uint64_t b)
{
    if (!m || !atomic_load_explicit(&m->enabled, memory_order_relaxed)) return;
    unsigned index = event_index(event);
    atomic_fetch_add_explicit(&m->event_attempt[index], 1, memory_order_relaxed);
    if(event==X4_TRACE_AU_FILTER_REJECT)
        atomic_fetch_add_explicit(&m->au_filter[flags&7u],1,memory_order_relaxed);
    unsigned critical = 0;
    if (event == X4_TRACE_AU_RESET && (flags & 3u) == 3u) critical=X4_MON_CR_AU_DISCARD;
    else if (event == 0x126) critical=X4_MON_CR_RECOVERY_BEGIN;
    else if (event == 0x127) critical=X4_MON_CR_RECOVERY_END;
    else if (event == X4_TRACE_PLI_REQUEST) critical=X4_MON_CR_PLI_LOCAL;
    else if (event == X4_TRACE_PLI_ATTEMPT) critical=X4_MON_CR_PLI_ATTEMPT;
    else if (event == X4_TRACE_PLI_RESULT) critical=X4_MON_CR_PLI_RESULT;
    else if (event == X4_TRACE_AU_FILTER_REJECT) critical=X4_MON_CR_AU_FILTER;
    else if (event == 0x121) critical=X4_MON_CR_IDR_SEEN;
    else if (event == X4_MON_EVENT_GAP_END) critical=X4_MON_CR_PRESENT_GAP_END;
    if (critical) x4_monitor_critical(m,time,critical,0xffffu,
        flags,a,0,a,b,0);
    if (event == X4_TRACE_AU_RESET) {
        unsigned reason = (unsigned)(b >> 48);
        if (reason >= 31) reason = 31;
        atomic_fetch_add_explicit(&m->reset[reason], 1, memory_order_relaxed);
        if ((flags & 3u) == 3u)
            atomic_fetch_add_explicit(&m->reset[32 + reason], 1, memory_order_relaxed);
    }
    if (event == X4_TRACE_RX_VALID && (flags & X4_TRACE_F_OBSERVED_METADATA))
        atomic_fetch_add_explicit(&m->queue_accepted, 1, memory_order_relaxed);
    if (event == X4_TRACE_QUEUE_DROP) {
        unsigned reason = flags >> 8, i = 5;
        if (reason == X4_TRACE_R_RTP_INVALID) i=0;
        else if (reason == X4_TRACE_R_SIZE) i=1;
        else if (reason == X4_TRACE_R_QUEUE_FULL) i=2;
        else if (reason == X4_TRACE_R_PUSH_CONTENDED) i=3;
        else if (reason == X4_TRACE_R_USER_STOP) i=4;
        atomic_fetch_add_explicit(&m->queue_rejected[i], 1, memory_order_relaxed);
    }
    if (event == X4_TRACE_RX_REJECT) {
        unsigned i = b < 7 ? (unsigned)b : 7;
        atomic_fetch_add_explicit(&m->rtp_rejected[i], 1, memory_order_relaxed);
    }
    if (event == X4_TRACE_REORDER_STATE)
        atomic_store_explicit(&m->reorder_state, a | (b ? UINT64_C(1)<<63 : 0), memory_order_relaxed);
    unsigned progress = 6;
    if (event == X4_TRACE_AU_VALID) progress=0;
    else if (event == X4_TRACE_OUTPUT_VALID) progress=1;
    else if (event == X4_TRACE_RGB_PUBLICATION) progress=2;
    else if (event == X4_TRACE_DECODE_END) progress=3;
    else if (event == X4_TRACE_COPY_END) progress=4;
    else if (event == X4_TRACE_CONVERT_END) progress=5;
    if (progress < 6) atomic_store_explicit(&m->last_progress[progress], time, memory_order_relaxed);
    if (event == 0x606 && (int32_t)(uint32_t)b)
        atomic_fetch_add_explicit(&m->errors_pli, 1, memory_order_relaxed);
    if (atomic_flag_test_and_set_explicit(&m->gate, memory_order_acquire)) {
        atomic_fetch_add_explicit(&m->event_drop[index], 1, memory_order_relaxed); return;
    }
    TEST_HOOK(4, m, event, m->record_ordinal);
    if (event == X4_MON_EVENT_GAP_END) {
        uint64_t start = time >= b ? time - b : 0;
        open_locked(m, a, start, time);
        if (m->current_window >= 0 && m->windows[m->current_window].h[3]==a) {
            Context *w = &m->windows[m->current_window];
            w->h[6] = time; w->h[12] = a; ++w->h[13]; ++m->retained_gaps;
        }
    }
    append_locked(m, time, event, flags, a, b);
    atomic_flag_clear_explicit(&m->gate, memory_order_release);
}

void x4_monitor_observe_queue(const X4VideoIngress *q, uint64_t time, X4MonitorQueue *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (!q || !q->slots || !q->capacity) { s->flags = X4_MON_Q_UNBOUND; return; }
    uint64_t head = atomic_load_explicit(&q->dequeue, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&q->enqueue, memory_order_acquire);
    const X4IngressSlot *slot = &q->slots[head % q->capacity];
    bool ready = atomic_load_explicit(&slot->ready, memory_order_acquire);
    uint64_t arrival = atomic_load_explicit(&slot->arrival_us, memory_order_relaxed);
    TEST_HOOK(3, (void *)q, head, tail);
    atomic_thread_fence(memory_order_acquire);
    bool ready2 = atomic_load_explicit(&slot->ready, memory_order_acquire);
    uint64_t arrival2 = atomic_load_explicit(&slot->arrival_us, memory_order_relaxed);
    uint64_t head2 = atomic_load_explicit(&q->dequeue, memory_order_acquire);
    uint64_t tail2 = atomic_load_explicit(&q->enqueue, memory_order_acquire);
    s->head = head; s->tail = tail;
    if (head != head2 || tail != tail2 || ready != ready2 || arrival != arrival2) {
        s->flags = X4_MON_Q_UNSTABLE; return;
    }
    uint64_t depth = tail - head; /* full 64-bit modulo counters, not slot-index witnesses */
    if (depth > q->capacity) { s->flags = X4_MON_Q_RANGE; return; }
    s->depth = depth; s->flags = X4_MON_Q_VALID;
    if (!depth) { s->flags |= X4_MON_Q_EMPTY; return; }
    if (!ready) { s->flags |= X4_MON_Q_UNPUBLISHED; return; }
    if (!arrival || arrival > time) { s->flags |= X4_MON_Q_CLOCK; return; }
    s->oldest_us = arrival;
}

void x4_monitor_rx(X4Monitor *m, unsigned kind, bool valid, uint32_t ssrc,
                    uint16_t seq, size_t bytes, uint64_t time)
{
    if (!m || !atomic_load_explicit(&m->enabled, memory_order_relaxed)) return;
    if (kind >= X4_MON_RX_KINDS) kind = X4_MON_RX_OTHER;
    RxKind *k = &m->rx[kind];
    atomic_fetch_add_explicit(&k->callbacks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&k->bytes, bytes, memory_order_relaxed);
    atomic_store_explicit(&k->last_us, time, memory_order_relaxed);
    /* Private numeric observation only. Ring admission can fail; raw counters
     * above survive that failure. Invalid packets have no trusted SSRC/seq. */
    x4_monitor_record(m, time, X4_MON_EVENT_RX_OBSERVATION,
        (uint16_t)(kind | (valid ? 0x100u : 0u)),
        valid ? ((uint64_t)ssrc << 32) | seq : 0, bytes);
    if (!valid) return;
    atomic_fetch_add_explicit(&k->valid, 1, memory_order_relaxed);
    atomic_store_explicit(&k->last_valid_us, time, memory_order_relaxed);
    uint64_t key = ((uint64_t)(kind + 1) << 32) | ssrc;
    RxSource *s = NULL;
    for (unsigned i = 0; i < X4_MON_RX_SOURCE_CAP; ++i) {
        uint64_t observed = atomic_load_explicit(&m->sources[i].key, memory_order_acquire);
        if (!observed) {
            uint64_t empty = 0;
            if (atomic_compare_exchange_strong_explicit(&m->sources[i].key, &empty,
                    key, memory_order_acq_rel, memory_order_relaxed)) observed = key;
            else observed = empty;
        }
        if (observed == key) { s = &m->sources[i]; break; }
    }
    if (!s) { atomic_fetch_add_explicit(&m->source_omitted, 1, memory_order_relaxed); return; }
    if (kind == X4_MON_RX_VIDEO)
        atomic_store_explicit(&m->primary_video_key, key, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->callbacks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes, bytes, memory_order_relaxed);
    atomic_store_explicit(&s->last_us, time, memory_order_relaxed);
    uint64_t empty_time = 0;
    atomic_compare_exchange_strong_explicit(&s->first_us, &empty_time, time,
        memory_order_relaxed, memory_order_relaxed);
    if (atomic_flag_test_and_set_explicit(&s->sequence_gate, memory_order_acquire)) {
        atomic_store_explicit(&s->dirty, true, memory_order_release);
        atomic_fetch_add_explicit(&s->unknown, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&m->seq_omitted, 1, memory_order_relaxed); return;
    }
    atomic_fetch_add_explicit(&s->version, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
    unsigned last = atomic_load_explicit(&s->last_seq, memory_order_relaxed);
    bool dirty = atomic_exchange_explicit(&s->dirty, false, memory_order_acq_rel);
    if ((last & 0x10000u) && !dirty) {
        uint16_t diff = (uint16_t)(seq - (uint16_t)last);
        if (!diff || diff >= 0x8000u)
            atomic_fetch_add_explicit(&s->backward, 1, memory_order_relaxed);
        else if (diff > 1)
            atomic_fetch_add_explicit(&s->skipped, diff - 1, memory_order_relaxed);
    } else if (dirty) atomic_fetch_add_explicit(&s->unknown, 1, memory_order_relaxed);
    atomic_store_explicit(&s->last_seq, seq | 0x10000u, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->version, 1, memory_order_release);
    atomic_flag_clear_explicit(&s->sequence_gate, memory_order_release);
}
void x4_monitor_queue(X4Monitor *m, uint32_t depth, uint64_t oldest, bool valid)
{
    if (!m) return;
    atomic_store_explicit(&m->supplied_depth, depth, memory_order_relaxed);
    atomic_store_explicit(&m->supplied_oldest, oldest, memory_order_relaxed);
    atomic_store_explicit(&m->supplied_valid, valid, memory_order_relaxed);
}
void x4_monitor_state(X4Monitor *m, unsigned actor, unsigned phase, uint64_t begin)
{
    if (!m || actor >= X4_MON_ACTORS) return;
    Actor *a = &m->actors[actor];
    if (atomic_load_explicit(&a->phase, memory_order_relaxed) == phase && !begin) return;
    atomic_fetch_add_explicit(&a->version, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&a->phase, phase, memory_order_relaxed);
    uint64_t time = begin ? begin : x4_monitor_now_us();
    atomic_store_explicit(&a->begin_us, time, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->version, 1, memory_order_release);
    x4_monitor_record(m, time, X4_MON_EVENT_PHASE_CHANGE, (uint16_t)actor, phase, time);
}
static void expose_until(X4Monitor *m, uint64_t time)
{
    if (!m->main_active) return;
    if (time >= m->exposure_begin) {
        uint64_t delta = time - m->exposure_begin;
        m->active_us += delta; m->epoch_exposure[m->main_epoch] += delta;
    } else ++m->clock_errors;
    m->exposure_begin = time;
}
void x4_monitor_epoch(X4Monitor *m, unsigned actor, unsigned epoch)
{
    if (!m || actor != X4_MON_ACTOR_SESSION || epoch > X4_MON_EPOCH_TRANSITION) return;
    if (m->main_epoch == epoch) return;
    uint64_t time = x4_monitor_now_us();
    expose_until(m, time); m->main_epoch = epoch; ++m->manual_marks;
    if (m->epoch_count < X4_MON_EPOCH_CAP)
        m->epochs[m->epoch_count++] = (X4MonitorEpoch){time,m->manual_marks,epoch,m->main_active};
    else ++m->epoch_omitted;
    Actor *a = &m->actors[actor];
    atomic_fetch_add_explicit(&a->version, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&a->epoch, epoch, memory_order_relaxed);
    atomic_store_explicit(&a->begin_us, time, memory_order_relaxed);
    atomic_fetch_add_explicit(&a->version, 1, memory_order_release);
    x4_monitor_record(m, time, X4_MON_EVENT_EPOCH, 0, epoch, m->manual_marks);
}
static void publish_cadence(X4Monitor *m)
{
    atomic_fetch_add_explicit(&m->cadence_version, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&m->active, m->main_active, memory_order_relaxed);
    atomic_store_explicit(&m->serial, m->main_serial, memory_order_relaxed);
    atomic_store_explicit(&m->last_new, m->main_last, memory_order_relaxed);
    atomic_store_explicit(&m->generation, m->main_generation, memory_order_relaxed);
    atomic_store_explicit(&m->closed, m->main_closed, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->cadence_version, 1, memory_order_release);
}
void x4_monitor_active(X4Monitor *m, bool active, uint64_t time)
{
    if (!m || m->main_active == active) return;
    expose_until(m, time); m->main_active = active;
    m->exposure_begin = active ? time : 0;
    m->main_last = m->main_generation = 0;
    publish_cadence(m);
    x4_monitor_record(m, time, X4_MON_EVENT_ACTIVE, 0, active, m->main_epoch);
}
void x4_monitor_present(X4Monitor *m, uint64_t generation, bool fresh, uint64_t time)
{
    if (!m || !fresh || !generation || !m->main_active) return;
    if (m->main_last && (time < m->main_last || generation <= m->main_generation)) {
        ++m->identity_errors; return;
    }
    uint64_t interval = m->main_last ? time - m->main_last : 0;
    ++m->main_serial;
    if (m->main_last) {
        ++m->intervals;
        unsigned bin = 0;
        for (uint64_t n = interval; n > 1 && bin < X4_MON_HIST_CAP - 1; n >>= 1) ++bin;
        ++m->histogram[bin];
        if (m->cadence_count < X4_MON_CADENCE_CAP) {
            m->cadence[m->cadence_count++] = (X4MonitorCadence){time, generation, interval,
                ((uint64_t)m->interval_begin_epoch << 32) | m->main_epoch |
                (m->interval_epoch_revision != m->manual_marks ? UINT64_C(1)<<63 : 0)};
        } else ++m->cadence_omitted;
        if (m->interval_begin_epoch == m->main_epoch && m->interval_epoch_revision == m->manual_marks)
            ++m->epoch_intervals[m->main_epoch];
        if (interval > X4_MON_GAP_US) {
            ++m->main_closed;
            x4_monitor_record(m, time, X4_MON_EVENT_GAP_END, 0, m->main_serial, interval);
        }
    }
    if (!m->first_new) m->first_new = time;
    m->main_last_ever = m->main_last = time; m->main_generation = generation;
    m->interval_begin_epoch = m->main_epoch;
    m->interval_epoch_revision = m->manual_marks;
    publish_cadence(m);
}

static bool cadence_read(X4Monitor *m, uint64_t *serial, uint64_t *last,
                         uint64_t *generation, uint64_t *closed, bool *active)
{
    uint64_t v = atomic_load_explicit(&m->cadence_version, memory_order_acquire);
    if (v & 1) goto unknown;
    *serial = atomic_load_explicit(&m->serial, memory_order_relaxed);
    *last = atomic_load_explicit(&m->last_new, memory_order_relaxed);
    *generation = atomic_load_explicit(&m->generation, memory_order_relaxed);
    *closed = atomic_load_explicit(&m->closed, memory_order_relaxed);
    *active = atomic_load_explicit(&m->active, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (v == atomic_load_explicit(&m->cadence_version, memory_order_acquire)) return true;
unknown:
    atomic_fetch_add_explicit(&m->cadence_unstable, 1, memory_order_relaxed); return false;
}
static void sample(X4Monitor *m, uint64_t target, uint64_t time, uint64_t missed)
{
    X4MonitorSample s = {0};
    s.w[0] = target; s.w[1] = time; s.w[2] = time >= target ? time - target : 0;
    s.w[3] = missed; s.w[4] = total(m->event_drop, X4_MON_EVENT_CAP);
    s.w[5] = UINT64_MAX; /* History tuple is unknown if its trygate is busy. */
    bool active = false; uint64_t serial = 0, last = 0, generation = 0, closed = 0;
    bool stable = cadence_read(m, &serial, &last, &generation, &closed, &active);
    if (stable) { s.w[6] |= X4_MON_S_CADENCE_STABLE;
        s.w[23] = serial; s.w[24] = last; s.w[25] = generation; s.w[26] = closed; }
    X4MonitorQueue q = {0};
    if (m->queue) x4_monitor_observe_queue(m->queue, time, &q);
    else {
        ++m->supplied_samples;
        q.depth = atomic_load_explicit(&m->supplied_depth, memory_order_relaxed);
        q.oldest_us = atomic_load_explicit(&m->supplied_oldest, memory_order_relaxed);
        q.flags = X4_MON_Q_UNBOUND; /* Independent fallback tuple never claims coherence. */
    }
    s.w[7] = q.depth; s.w[8] = q.oldest_us; s.w[9] = q.flags;
    if (q.flags & X4_MON_Q_VALID) s.w[6] |= X4_MON_S_QUEUE_STABLE;
    else atomic_fetch_add_explicit(&m->queue_unstable, 1, memory_order_relaxed);
    s.w[10] = atomic_load_explicit(&m->rx[0].callbacks, memory_order_relaxed);
    s.w[11] = atomic_load_explicit(&m->rx[0].valid, memory_order_relaxed);
    s.w[12] = atomic_load_explicit(&m->rx[0].bytes, memory_order_relaxed);
    s.w[13] = atomic_load_explicit(&m->rx[0].last_us, memory_order_relaxed);
    s.w[14] = atomic_load_explicit(&m->rx[0].last_valid_us, memory_order_relaxed);
    s.w[16] = atomic_load_explicit(&m->queue_accepted, memory_order_relaxed);
    s.w[17] = attempts(m, X4_TRACE_QUEUE_DROP);
    s.w[18] = attempts(m, 0x600); s.w[19] = attempts(m, X4_TRACE_DECODE_END);
    s.w[20] = attempts(m, X4_TRACE_OUTPUT_VALID); s.w[21] = attempts(m, X4_TRACE_RGB_PUBLICATION);
    s.w[22] = attempts(m, X4_TRACE_FLIP_SUBMIT); s.w[27] = attempts(m, X4_TRACE_AU_RESET);
    s.w[28] = attempts(m, 0x605);
    s.w[29] = atomic_load_explicit(&m->errors_pli, memory_order_relaxed);
    s.w[15]=UINT64_MAX;
    s.w[31] = atomic_load_explicit(&m->primary_video_key, memory_order_acquire);
    if(s.w[31]){
        bool rx_stable=false;
        for(unsigned i=0;i<X4_MON_RX_SOURCE_CAP;++i){
            RxSource *r=&m->sources[i];
            uint64_t key=atomic_load_explicit(&r->key,memory_order_acquire);
            if(key!=s.w[31])continue;
            uint64_t version=atomic_load_explicit(&r->version,memory_order_acquire);
            unsigned seq=atomic_load_explicit(&r->last_seq,memory_order_relaxed);
            bool dirty=atomic_load_explicit(&r->dirty,memory_order_acquire);
            atomic_thread_fence(memory_order_acquire);
            if(!(version&1)&&!dirty&&(seq&0x10000u)&&
                version==atomic_load_explicit(&r->version,memory_order_acquire)&&
                !atomic_load_explicit(&r->dirty,memory_order_acquire)&&
                key==atomic_load_explicit(&r->key,memory_order_acquire)&&
                key==atomic_load_explicit(&m->primary_video_key,memory_order_acquire)){
                s.w[15]=seq; s.w[6]|=X4_MON_S_RX_STABLE; rx_stable=true;
            }
            break;
        }
        if(!rx_stable)atomic_fetch_add_explicit(&m->rx_sample_unstable,1,memory_order_relaxed);
    }
    for (unsigned i = 0; i < X4_MON_ACTORS; ++i) {
        Actor *a = &m->actors[i]; uint64_t v = atomic_load_explicit(&a->version, memory_order_acquire);
        unsigned off = 32 + i * 4;
        s.w[off] = atomic_load_explicit(&a->phase, memory_order_relaxed);
        s.w[off+1] = atomic_load_explicit(&a->epoch, memory_order_relaxed);
        s.w[off+2] = atomic_load_explicit(&a->begin_us, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (!(v & 1) && v == atomic_load_explicit(&a->version, memory_order_acquire)) {
            s.w[off+3] = v; s.w[6] |= UINT64_C(1) << (X4_MON_S_ACTOR_SHIFT + i);
        } else {
            s.w[off] = s.w[off+1] = s.w[off+2] = 0; s.w[off+3] = UINT64_MAX;
            atomic_fetch_add_explicit(&m->actor_unstable, 1, memory_order_relaxed);
        }
    }
    s.w[64]=attempts(m,X4_TRACE_QUEUE_POP);
    s.w[65]=atomic_load_explicit(&m->reorder_state,memory_order_relaxed);
    s.w[66]=attempts(m,X4_TRACE_REORDER_EMIT); s.w[67]=attempts(m,X4_TRACE_REORDER_HOLE);
    s.w[68]=attempts(m,X4_TRACE_DECODE_BEGIN); s.w[69]=attempts(m,X4_TRACE_COPY_BEGIN);
    s.w[70]=attempts(m,X4_TRACE_COPY_END); s.w[71]=attempts(m,X4_TRACE_CONVERT_BEGIN);
    s.w[72]=attempts(m,X4_TRACE_CONVERT_END);
    for(unsigned i=0;i<6;++i)s.w[73+i]=atomic_load_explicit(&m->last_progress[i],memory_order_relaxed);
    s.w[79]=total(m->reset+32,32);
    if (!atomic_flag_test_and_set_explicit(&m->gate, memory_order_acquire)) {
        s.w[5] = m->history_overwrites;
        s.w[6] |= X4_MON_S_HISTORY_STABLE;
        if (m->current_window >= 0) {
            Context *w = &m->windows[m->current_window];
            if (time >= w->h[5] && time - w->h[5] >= WINDOW_US) freeze_locked(m, time, 2);
            else if (w->h[6] && time >= w->h[6] && time - w->h[6] >= POST_US)
                freeze_locked(m, time, 1);
        }
        if (stable && active && last && time > last && time - last > X4_MON_GAP_US &&
                serial != m->seen_gap_serial) {
            m->seen_gap_serial = serial;
            open_locked(m, serial + 1, last, time);
            if (m->current_window >= 0 && m->windows[m->current_window].h[3]==serial+1) {
                /* A second ongoing gap cancels the previous gap's post
                 * deadline in a shared context. It is not closed yet. */
                Context *w = &m->windows[m->current_window];
                w->h[6] = 0; w->h[12] = serial + 1;
            }
            append_locked(m, time, X4_MON_EVENT_GAP_BEGIN, 0, serial + 1, time - last);
            x4_monitor_critical(m,time,X4_MON_CR_PRESENT_GAP_BEGIN,
                0xffffu,0,serial+1,0,last,time-last,0);
        }
        atomic_flag_clear_explicit(&m->gate, memory_order_release);
    } else atomic_fetch_add_explicit(&m->sampler_gate_omitted,1,memory_order_relaxed);
    s.w[30]=x4_monitor_now_us(); /* All sample observations span [w1,w30]. */
    m->samples[m->sample_total % X4_MON_SAMPLE_CAP] = s;
    ++m->sample_total;
    if (m->sample_count < X4_MON_SAMPLE_CAP) ++m->sample_count;
    else ++m->sample_omitted;
    m->missed_periods += missed;
    if (s.w[2] > m->max_lateness) m->max_lateness = s.w[2];
}
static void *sampler(void *arg)
{
    X4Monitor *m = arg;
    uint64_t previous = x4_monitor_now_us();
    uint64_t target = previous + X4_MON_PERIOD_US;
    while (!atomic_load_explicit(&m->stopping, memory_order_acquire)) {
        uint64_t now = x4_monitor_now_us();
        if(now<previous){++m->sampler_clock_errors;target=now+X4_MON_PERIOD_US;}
        previous=now;
        if (now < target) { sleep_us(target - now); continue; }
        uint64_t missed = (now - target) / X4_MON_PERIOD_US;
        TEST_HOOK(1, m, target, now);
        sample(m, target, now, missed);
        TEST_HOOK(2, m, target, now);
        uint64_t advance = (missed + 1) * X4_MON_PERIOD_US;
        if (target > UINT64_MAX - advance) break;
        target += advance; /* One actual observation; never manufacture missed snapshots. */
    }
    return NULL;
}
X4Monitor *x4_monitor_create(uint64_t session, unsigned readers)
{
    X4Monitor *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    atomic_flag_clear(&m->gate); atomic_init(&m->enabled, true); atomic_init(&m->stopping, false);
    atomic_flag_clear(&m->critical_gate);
    atomic_init(&m->critical_gate_omit,0);atomic_init(&m->critical_cap_omit,0);
    atomic_init(&m->transport_source_omit,0);atomic_init(&m->transport_sequence_omit,0);
    atomic_init(&m->critical_ids,0);
    for(unsigned i=0;i<8;++i)atomic_init(&m->au_filter[i],0);
    for(unsigned i=0;i<X4_MON_CR_EVENT_CAP;++i){
        atomic_init(&m->critical_attempt[i],0);atomic_init(&m->critical_omit[i],0);
    }
    for(unsigned stage=0;stage<X4_MON_T_STAGES;++stage){
        atomic_init(&m->delay_max_omit[stage],0);
        for(unsigned i=0;i<X4_MON_T_COUNTER_WORDS;++i)atomic_init(&m->transport[stage][i],0);
        for(unsigned i=0;i<32;++i)atomic_init(&m->transport_reject[stage][i],0);
        for(unsigned i=0;i<X4_MON_T_SOURCE_CAP;++i){TransportSource*s=&m->transport_sources[stage][i];
            atomic_flag_clear(&s->gate);atomic_init(&s->dirty,false);
            atomic_init(&s->key,0);atomic_init(&s->count,0);atomic_init(&s->bytes,0);atomic_init(&s->last,0);
            atomic_init(&s->skipped,0);atomic_init(&s->gaps,0);atomic_init(&s->backward,0);atomic_init(&s->duplicates,0);
            atomic_init(&s->omissions,0);atomic_init(&s->late,0);atomic_init(&s->evicted,0);atomic_init(&s->ambiguous,0);
        }
    }
    for (unsigned i = 0; i < X4_MON_EVENT_CAP; ++i) {
        atomic_init(&m->event_attempt[i], 0); atomic_init(&m->event_drop[i], 0);
    }
    for (unsigned i = 0; i < X4_MON_RESET_CAP; ++i) atomic_init(&m->reset[i], 0);
#define INIT_ZERO(name) atomic_init(&m->name, 0)
    INIT_ZERO(errors_pli); INIT_ZERO(source_omitted); INIT_ZERO(seq_omitted);
    INIT_ZERO(actor_unstable); INIT_ZERO(queue_unstable); INIT_ZERO(cadence_unstable);
    INIT_ZERO(primary_video_key); INIT_ZERO(supplied_depth); INIT_ZERO(supplied_oldest);
    INIT_ZERO(queue_accepted); INIT_ZERO(reorder_state); INIT_ZERO(sampler_gate_omitted);
    INIT_ZERO(rx_sample_unstable);
    INIT_ZERO(cadence_version); INIT_ZERO(serial); INIT_ZERO(last_new); INIT_ZERO(generation); INIT_ZERO(closed);
    atomic_init(&m->supplied_valid, false); atomic_init(&m->active, false);
#undef INIT_ZERO
    for(unsigned i=0;i<6;++i){atomic_init(&m->queue_rejected[i],0);atomic_init(&m->last_progress[i],0);}
    for(unsigned i=0;i<8;++i)atomic_init(&m->rtp_rejected[i],0);
    for (unsigned i = 0; i < X4_MON_ACTORS; ++i) {
        atomic_init(&m->actors[i].version, 0); atomic_init(&m->actors[i].phase, 0);
        atomic_init(&m->actors[i].epoch, 0); atomic_init(&m->actors[i].begin_us, 0);
    }
    for (unsigned i = 0; i < X4_MON_RX_KINDS; ++i) {
        atomic_init(&m->rx[i].callbacks, 0); atomic_init(&m->rx[i].valid, 0);
        atomic_init(&m->rx[i].bytes, 0); atomic_init(&m->rx[i].last_us, 0); atomic_init(&m->rx[i].last_valid_us, 0);
    }
    for (unsigned i = 0; i < X4_MON_RX_SOURCE_CAP; ++i) {
        RxSource *s = &m->sources[i];
        atomic_init(&s->key, 0); atomic_init(&s->callbacks, 0); atomic_init(&s->bytes, 0);
        atomic_init(&s->last_us, 0); atomic_init(&s->version, 0); atomic_init(&s->skipped, 0);
        atomic_init(&s->backward, 0); atomic_init(&s->unknown, 0); atomic_init(&s->first_us, 0);
        atomic_init(&s->last_seq, 0); atomic_init(&s->dirty, false); atomic_flag_clear(&s->sequence_gate);
    }
    m->session = session; m->created_us = x4_monitor_now_us(); m->current_window = -1;
    m->config = (X4MonitorConfig){960,540,30,5000,readers,16000,4,256,128,32,25000,0x42e01f,3600,108000,0,0};
    return m;
}
bool x4_monitor_bind_queue(X4Monitor *m, const X4VideoIngress *queue)
{
    if (!m || m->start_called || m->queue || !queue || !queue->slots || !queue->capacity) return false;
    m->queue = queue; return true;
}
bool x4_monitor_config(X4Monitor *m, const X4MonitorConfig *c)
{
    if (!m || !c || m->start_called) return false;
    m->config = *c; return true;
}
int x4_monitor_start(X4Monitor *m)
{
    if (!m) return 1;
    if (m->start_called) return m->created ? 0 : 1;
    m->start_called = true;
#ifdef X4_MONITOR_HOST_TEST
    int rc = x4_monitor_host_create(&m->thread, sampler, m);
#else
    int rc = scePthreadCreate(&m->thread, NULL, sampler, m, "x4progress");
#endif
    m->start_rc = rc;
    if (rc) {
        x4_monitor_record(m, x4_monitor_now_us(), X4_MON_EVENT_DISABLED, 0, (uint32_t)rc, 0);
        atomic_store_explicit(&m->enabled, false, memory_order_release); return 1;
    }
    m->created = true;
    x4_monitor_record(m, x4_monitor_now_us(), X4_MON_EVENT_SAMPLER_START, 0, X4_MON_PERIOD_US, 0);
    return 0;
}
int x4_monitor_stop(X4Monitor *m)
{
    if (!m) return 0;
    if (m->join_rc) return m->join_rc;
    atomic_store_explicit(&m->stopping, true, memory_order_release);
    if (!m->created) return 0;
#ifdef X4_MONITOR_HOST_TEST
    int rc = x4_monitor_host_join(m->thread);
#else
    int rc = scePthreadJoin(m->thread, NULL);
#endif
    if (rc) { m->join_rc = rc > 0 ? -rc : rc; return m->join_rc; }
    m->created = false;
    x4_monitor_record(m, x4_monitor_now_us(), X4_MON_EVENT_SAMPLER_STOP, 0, m->sample_count, 0);
    return 0;
}
bool x4_monitor_stopped(const X4Monitor *m) { return !m || (!m->created && !m->join_rc); }
void x4_monitor_end(X4Monitor *m)
{
    if (!m || !x4_monitor_stopped(m) || m->ended) return;
    m->ended_us = x4_monitor_now_us(); expose_until(m, m->ended_us);
    m->main_active = false; publish_cadence(m);
    /* External producers have joined: no gate spin or concurrent dump needed. */
    freeze_locked(m, m->ended_us, 4);
    atomic_store_explicit(&m->enabled, false, memory_order_release); m->ended = true;
}
static bool put(FILE *f, const void *p, size_t bytes) { return fwrite(p, 1, bytes, f) == bytes; }
int x4_monitor_dump(X4Monitor *m, const char *base_path)
{
    if (!m) return 1;
    if (!base_path || !m->ended || !x4_monitor_stopped(m)) return -2;
    char path[384];
    size_t length = strlen(base_path);
    static const char suffix[] = ".progress.bin";
    if (length >= sizeof(path) - sizeof(suffix)) return -1;
    memcpy(path, base_path, length); memcpy(path + length, suffix, sizeof(suffix));
    FILE *f = fopen(path, "wbx");
    if (!f) return -3;
    uint64_t h[X4_MON_HEADER_WORDS] = {0};
    memcpy(h, "X4PROG2", 8);
    h[1]=2; h[2]=sizeof(h); h[3]=m->session; h[4]=m->created_us; h[5]=m->ended_us;
    h[6]=(uint64_t)(int64_t)m->start_rc; h[7]=(uint64_t)(int64_t)m->join_rc;
    h[8]=m->config.readers; h[9]=X4_MON_PERIOD_US; h[10]=m->sample_count; h[11]=m->sample_omitted;
    h[12]=m->cadence_count; h[13]=m->cadence_omitted; h[14]=m->history_count; h[15]=m->history_overwrites;
    h[16]=m->window_count; h[17]=m->window_omitted;
    h[18]=total(m->event_attempt,X4_MON_EVENT_CAP); h[19]=total(m->event_drop,X4_MON_EVENT_CAP);
    h[20]=m->missed_periods; h[21]=m->max_lateness; h[22]=m->main_serial; h[23]=m->first_new;
    h[24]=m->main_last_ever; h[25]=m->main_closed; h[26]=m->identity_errors;
    h[27]=m->intervals; h[28]=m->active_us; h[29]=m->clock_errors+m->sampler_clock_errors;
    h[30]=atomic_load_explicit(&m->source_omitted,memory_order_relaxed);
    h[31]=atomic_load_explicit(&m->seq_omitted,memory_order_relaxed);
    h[32]=(m->start_called&&!m->start_rc?1u:0u)|2u|4u|(m->queue?8u:0u);
    h[33]=m->coverage|(m->sample_omitted?C_SAMPLE:0)|(m->cadence_omitted?C_CADENCE:0)|
        (h[19]?C_DROP:0)|(m->start_rc?C_DISABLED:0)|(m->join_rc?C_JOIN:0)|(h[31]?C_SEQUENCE:0)|
        (h[29]?C_CLOCK:0);
    h[34]=X4_MON_SAMPLE_CAP; h[35]=X4_MON_CADENCE_CAP; h[36]=X4_MON_HISTORY_CAP;
    h[37]=X4_MON_WINDOW_CAP; h[38]=X4_MON_WINDOW_RECORD_CAP; h[39]=X4_MON_EVENT_CAP;
    h[40]=X4_MON_RESET_CAP; h[41]=X4_MON_RX_SOURCE_CAP; h[42]=X4_MON_HIST_CAP;
    h[43]=sizeof(X4MonitorSample); h[44]=sizeof(X4MonitorCadence); h[45]=sizeof(X4MonitorRecord);
    h[46]=X4_MON_ACTORS; h[47]=X4_MON_RX_KINDS;
    for(unsigned i=0;i<X4_MON_RX_KINDS;++i){
        RxKind *k=&m->rx[i]; unsigned o=48+i*5;
        h[o]=atomic_load_explicit(&k->callbacks,memory_order_relaxed);
        h[o+1]=atomic_load_explicit(&k->valid,memory_order_relaxed);
        h[o+2]=atomic_load_explicit(&k->bytes,memory_order_relaxed);
        h[o+3]=atomic_load_explicit(&k->last_us,memory_order_relaxed);
        h[o+4]=atomic_load_explicit(&k->last_valid_us,memory_order_relaxed);
    }
    h[64]=atomic_load_explicit(&m->actor_unstable,memory_order_relaxed);
    h[65]=atomic_load_explicit(&m->queue_unstable,memory_order_relaxed);
    h[66]=atomic_load_explicit(&m->cadence_unstable,memory_order_relaxed);
    h[67]=m->supplied_samples; h[68]=m->manual_marks; h[69]=m->open_at_stop; h[70]=m->retained_gaps;
    h[71]=atomic_load_explicit(&m->queue_accepted,memory_order_relaxed);
    for(unsigned i=0;i<6;++i)h[72+i]=atomic_load_explicit(&m->queue_rejected[i],memory_order_relaxed);
    for(unsigned i=0;i<8;++i)h[78+i]=atomic_load_explicit(&m->rtp_rejected[i],memory_order_relaxed);
    h[86]=atomic_load_explicit(&m->sampler_gate_omitted,memory_order_relaxed);
    h[87]=m->sample_total;h[88]=m->sampler_clock_errors;h[89]=m->epoch_count;
    h[90]=m->epoch_omitted;h[91]=sizeof(X4MonitorEpoch);
    h[92]=atomic_load_explicit(&m->rx_sample_unstable,memory_order_relaxed);
    if(h[64])h[33]|=C_ACTOR;
    if(h[65]||h[66]||h[86]||h[92])h[33]|=C_UNSTABLE;
    if(m->epoch_omitted)h[33]|=C_EPOCH;
    snprintf((char*)&h[96],64,"%s",X4_BUILD_ID); snprintf((char*)&h[104],64,"%s",X4_PRODUCT_VERSION);
    bool ok=put(f,h,sizeof(h))&&put(f,&m->config,sizeof(m->config));
    uint64_t values[X4_MON_EVENT_CAP];
    for(unsigned i=0;i<X4_MON_EVENT_CAP;++i)values[i]=atomic_load_explicit(&m->event_attempt[i],memory_order_relaxed);
    ok=ok&&put(f,values,sizeof(values));
    for(unsigned i=0;i<X4_MON_EVENT_CAP;++i)values[i]=atomic_load_explicit(&m->event_drop[i],memory_order_relaxed);
    ok=ok&&put(f,values,sizeof(values));
    for(unsigned i=0;i<X4_MON_RESET_CAP;++i)values[i]=atomic_load_explicit(&m->reset[i],memory_order_relaxed);
    ok=ok&&put(f,values,X4_MON_RESET_CAP*sizeof(uint64_t))&&put(f,m->histogram,sizeof(m->histogram));
    uint64_t exposures[8];for(unsigned i=0;i<4;++i){exposures[i*2]=m->epoch_exposure[i];exposures[i*2+1]=m->epoch_intervals[i];}
    ok=ok&&put(f,exposures,sizeof(exposures));
    ok=ok&&put(f,m->epochs,sizeof(m->epochs));
    for(unsigned i=0;i<X4_MON_RX_SOURCE_CAP;++i){
        RxSource *s=&m->sources[i];uint64_t r[X4_MON_RX_WORDS]={0};
        r[0]=atomic_load_explicit(&s->key,memory_order_relaxed);r[1]=atomic_load_explicit(&s->callbacks,memory_order_relaxed);
        r[2]=atomic_load_explicit(&s->bytes,memory_order_relaxed);r[3]=atomic_load_explicit(&s->last_us,memory_order_relaxed);
        r[4]=atomic_load_explicit(&s->version,memory_order_relaxed);r[5]=atomic_load_explicit(&s->last_seq,memory_order_relaxed);
        r[6]=atomic_load_explicit(&s->skipped,memory_order_relaxed);r[7]=atomic_load_explicit(&s->backward,memory_order_relaxed);
        r[8]=atomic_load_explicit(&s->unknown,memory_order_relaxed);r[9]=atomic_load_explicit(&s->first_us,memory_order_relaxed);
        ok=ok&&put(f,r,sizeof(r));
    }
    unsigned sample_begin=(unsigned)((m->sample_total-m->sample_count)%X4_MON_SAMPLE_CAP);
    for(unsigned i=0;i<m->sample_count;++i)
        ok=ok&&put(f,&m->samples[(sample_begin+i)%X4_MON_SAMPLE_CAP],sizeof(X4MonitorSample));
    ok=ok&&put(f,m->cadence,m->cadence_count*sizeof(*m->cadence));
    unsigned begin=(m->history_head+X4_MON_HISTORY_CAP-m->history_count)%X4_MON_HISTORY_CAP;
    for(unsigned i=0;i<m->history_count;++i)ok=ok&&put(f,&m->history[(begin+i)%X4_MON_HISTORY_CAP],sizeof(X4MonitorRecord));
    for(unsigned i=0;i<m->window_count;++i){Context*w=&m->windows[i];ok=ok&&put(f,w->h,sizeof(w->h))&&put(f,w->records,w->h[1]*sizeof(X4MonitorRecord));}
    uint64_t ch[X4_MON_CR_HEADER_WORDS]={0};memcpy(ch,"X4CRIT2",8);
    ch[1]=2;ch[2]=sizeof(ch);ch[3]=sizeof(X4MonitorCritical);ch[4]=X4_MON_CR_CAP;
    ch[5]=m->critical_count;ch[6]=total(m->critical_attempt,X4_MON_CR_EVENT_CAP);
    ch[7]=atomic_load_explicit(&m->critical_gate_omit,memory_order_relaxed);
    ch[8]=atomic_load_explicit(&m->critical_cap_omit,memory_order_relaxed);
    ch[9]=X4_MON_T_STAGES;ch[10]=X4_MON_T_SOURCE_CAP;ch[11]=X4_MON_T_SOURCE_WORDS;ch[12]=32;
    ch[13]=atomic_load_explicit(&m->transport_source_omit,memory_order_relaxed);
    ch[14]=atomic_load_explicit(&m->transport_sequence_omit,memory_order_relaxed);
    ch[15]=atomic_load_explicit(&m->critical_ids,memory_order_relaxed);
    ch[16]=X4_MON_CR_EVENT_CAP;ch[17]=X4_MON_T_COUNTER_WORDS;
    ch[18]=X4_MON_WINDOW_PRE_CAP;ch[19]=X4_MON_WINDOW_BODY_CAP;ch[20]=X4_MON_WINDOW_END_CAP;
    ch[21]=m->duplicate_window_opens;ch[22]=m->window_retention_omissions;
    ch[23]=m->critical_ordinal;ch[24]=X4_MON_T_STAGES;ch[25]=10000;
    ch[26]=8;
    ok=ok&&put(f,ch,sizeof(ch));
    for(unsigned i=0;i<X4_MON_CR_EVENT_CAP;++i)values[i]=atomic_load_explicit(&m->critical_attempt[i],memory_order_relaxed);
    ok=ok&&put(f,values,X4_MON_CR_EVENT_CAP*sizeof(uint64_t));
    for(unsigned i=0;i<X4_MON_CR_EVENT_CAP;++i)values[i]=atomic_load_explicit(&m->critical_omit[i],memory_order_relaxed);
    ok=ok&&put(f,values,X4_MON_CR_EVENT_CAP*sizeof(uint64_t));
    for(unsigned stage=0;stage<X4_MON_T_STAGES;++stage){
        for(unsigned i=0;i<X4_MON_T_COUNTER_WORDS;++i)values[i]=atomic_load_explicit(&m->transport[stage][i],memory_order_relaxed);
        ok=ok&&put(f,values,X4_MON_T_COUNTER_WORDS*sizeof(uint64_t));
    }
    for(unsigned stage=0;stage<X4_MON_T_STAGES;++stage){
        for(unsigned i=0;i<32;++i)values[i]=atomic_load_explicit(&m->transport_reject[stage][i],memory_order_relaxed);
        ok=ok&&put(f,values,32*sizeof(uint64_t));
    }
    for(unsigned stage=0;stage<X4_MON_T_STAGES;++stage)values[stage]=atomic_load_explicit(&m->delay_max_omit[stage],memory_order_relaxed);
    ok=ok&&put(f,values,X4_MON_T_STAGES*sizeof(uint64_t));
    for(unsigned i=0;i<8;++i)values[i]=atomic_load_explicit(&m->au_filter[i],memory_order_relaxed);
    ok=ok&&put(f,values,8*sizeof(uint64_t));
    for(unsigned stage=0;stage<X4_MON_T_STAGES;++stage)for(unsigned i=0;i<X4_MON_T_SOURCE_CAP;++i){
        TransportSource*s=&m->transport_sources[stage][i];uint64_t r[X4_MON_T_SOURCE_WORDS]={0};
        r[0]=atomic_load_explicit(&s->key,memory_order_relaxed);
        r[1]=atomic_load_explicit(&s->count,memory_order_relaxed);r[2]=atomic_load_explicit(&s->bytes,memory_order_relaxed);
        r[3]=atomic_load_explicit(&s->last,memory_order_relaxed);r[4]=s->first;
        r[5]=s->initialized?(uint64_t)s->sequence|0x10000u:0;r[6]=s->sequence_time;
        r[7]=atomic_load_explicit(&s->skipped,memory_order_relaxed);r[8]=atomic_load_explicit(&s->gaps,memory_order_relaxed);
        r[9]=atomic_load_explicit(&s->backward,memory_order_relaxed);r[10]=atomic_load_explicit(&s->duplicates,memory_order_relaxed);
        r[11]=atomic_load_explicit(&s->omissions,memory_order_relaxed);r[12]=atomic_load_explicit(&s->late,memory_order_relaxed);
        r[13]=atomic_load_explicit(&s->evicted,memory_order_relaxed);r[14]=atomic_load_explicit(&s->ambiguous,memory_order_relaxed);
        r[15]=atomic_load_explicit(&s->dirty,memory_order_relaxed);r[16]=s->range_next;
        for(unsigned j=0;j<X4_MON_T_RANGE_CAP;++j)if(s->ranges[j].id)++r[17];
        r[18]=s->extended_sequence;
        ok=ok&&put(f,r,sizeof(r));
    }
    unsigned critical_begin=(unsigned)((m->critical_ordinal-m->critical_count)%X4_MON_CR_CAP);
    for(unsigned i=0;i<m->critical_count;++i)
        ok=ok&&put(f,&m->critical[(critical_begin+i)%X4_MON_CR_CAP],sizeof(X4MonitorCritical));
    if(fclose(f))ok=false;
    return ok?0:-3;
}
bool x4_monitor_free(X4Monitor *m)
{
    if(!m)return true;
    if(!m->ended||!x4_monitor_stopped(m))return false;
    free(m);return true;
}
