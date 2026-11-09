/* SPDX-License-Identifier: GPL-3.0-only */
#include "video_pipeline.h"
#include <string.h>
#include <smmintrin.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__SSE2__) && !defined(X4_PIPELINE_SCALAR)
#define X4_PIPELINE_SSE2 1
#else
#define X4_PIPELINE_SSE2 0
#endif

#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
#define OUTPUT_LIMIT(r) ((r)->output_count)
#else
#define OUTPUT_LIMIT(r) X4_PIPELINE_OUTPUTS
#endif

static void clear_lease(X4VideoPipelineLease *lease)
{ if (lease) memset(lease, 0, sizeof(*lease)); }
int x4_video_pipeline_ring_init(X4VideoPipelineRing *r, uint64_t epoch)
{
    if (!r || !epoch) return X4_PIPELINE_INVALID;
    memset(r, 0, sizeof(*r)); r->epoch = epoch;
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
    r->output_count = X4_PIPELINE_OUTPUTS;
#endif
    return 0;
}
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
int x4_video_pipeline_ring_init_outputs(X4VideoPipelineRing *r, uint64_t epoch,
    unsigned count)
{
    if (count != 4 && count != 5) return X4_PIPELINE_INVALID;
    int rc = x4_video_pipeline_ring_init(r, epoch);
    if (!rc) r->output_count = count;
    return rc;
}
#endif
static X4VideoPipelineLease output_lease(const X4VideoPipelineRing *r, unsigned i)
{ return (X4VideoPipelineLease){i, r->epoch, r->slots[i].generation}; }
static int output_check(X4VideoPipelineRing *r, X4VideoPipelineLease lease,
    X4VideoPipelineOutputState state)
{
    if (!r || !r->epoch || lease.slot >= OUTPUT_LIMIT(r)) return X4_PIPELINE_INVALID;
    if (lease.epoch != r->epoch || !lease.generation ||
        lease.generation != r->slots[lease.slot].generation) {
        ++r->rejected; return X4_PIPELINE_STALE;
    }
    if (r->slots[lease.slot].state != state) { ++r->rejected; return X4_PIPELINE_STATE; }
    return 0;
}
static int oldest_ready(const X4VideoPipelineRing *r)
{
    int best = -1;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_READY &&
            (best < 0 || r->slots[i].ready_order < r->slots[best].ready_order)) best = (int)i;
    return best;
}
int x4_video_pipeline_acquire_decode(X4VideoPipelineRing *r,
    X4VideoPipelineLease *out, uint64_t *discarded_output_id)
{
    clear_lease(out);
    if (discarded_output_id) *discarded_output_id = 0;
    if (!r || !r->epoch || !out) return X4_PIPELINE_INVALID;
    if (r->next_generation == UINT64_MAX) return X4_PIPELINE_EXHAUSTED;
    int slot = -1;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_FREE) { slot = (int)i; break; }
    if (slot < 0) {
        slot = oldest_ready(r);
        if (slot < 0) { ++r->waits; return X4_PIPELINE_WAIT; }
        if (discarded_output_id) *discarded_output_id = r->slots[slot].output_id;
        ++r->discarded_ready; --r->ready;
    } else {
        if (++r->occupied > r->peak_occupied) r->peak_occupied = r->occupied;
    }
    r->slots[slot] = (X4VideoPipelineOutput){
        .state = X4_PIPELINE_DECODING, .generation = ++r->next_generation};
    ++r->acquired; *out = output_lease(r, (unsigned)slot);
    return 0;
}
int x4_video_pipeline_decoding_lease(X4VideoPipelineRing *r, unsigned slot,
    X4VideoPipelineLease *out)
{
    clear_lease(out);
    if (!r || !r->epoch || !out || slot >= OUTPUT_LIMIT(r)) return X4_PIPELINE_INVALID;
    if (r->slots[slot].state != X4_PIPELINE_DECODING) { ++r->rejected; return X4_PIPELINE_STATE; }
    *out = output_lease(r, slot); return 0;
}
int x4_video_pipeline_publish_ready(X4VideoPipelineRing *r, X4VideoPipelineLease lease,
    uint64_t output_id, uint64_t decode_end_us)
{
    int rc = output_check(r, lease, X4_PIPELINE_DECODING);
    if (rc) return rc;
    if (!output_id) return X4_PIPELINE_INVALID;
    if (r->next_ready_order == UINT64_MAX) return X4_PIPELINE_EXHAUSTED;
    X4VideoPipelineOutput *s = &r->slots[lease.slot];
    s->state = X4_PIPELINE_READY; s->output_id = output_id;
    s->decode_end_us = decode_end_us; s->ready_order = ++r->next_ready_order;
    ++r->published;
    if (++r->ready > r->peak_ready) r->peak_ready = r->ready;
    return 0;
}
int x4_video_pipeline_take_ready(X4VideoPipelineRing *r, X4VideoPipelineLease *out,
    X4VideoPipelineLatencyDrops *drops)
{
    clear_lease(out);
    if (drops) memset(drops, 0, sizeof(*drops));
    if (!r || !r->epoch || !out) return X4_PIPELINE_INVALID;
    int slot = -1;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_READY &&
            (slot < 0 || r->slots[i].ready_order > r->slots[slot].ready_order)) slot = (int)i;
    if (slot < 0) return X4_PIPELINE_WAIT;
    r->slots[slot].state = X4_PIPELINE_CONVERTING; --r->ready;
    /* The selected slot is no longer READY; only unclaimed older publications
     * can be released. The caller's ring lock covers selection and every drop. */
    int old;
    while ((old = oldest_ready(r)) >= 0) {
        if (drops) drops->output_ids[drops->count++] = r->slots[old].output_id;
        r->slots[old].state = X4_PIPELINE_FREE;
        --r->ready; --r->occupied; ++r->discarded_latency;
    }
    *out = output_lease(r, (unsigned)slot); return 0;
}
int x4_video_pipeline_release_conversion(X4VideoPipelineRing *r, X4VideoPipelineLease lease)
{
    int rc = output_check(r, lease, X4_PIPELINE_CONVERTING);
    if (rc) return rc;
    r->slots[lease.slot].state = X4_PIPELINE_FREE;
    --r->occupied; ++r->converted; return 0;
}
int x4_video_pipeline_abort_decode(X4VideoPipelineRing *r, X4VideoPipelineLease lease)
{
    int rc = output_check(r, lease, X4_PIPELINE_DECODING);
    if (rc) return rc;
    r->slots[lease.slot].state = X4_PIPELINE_FREE;
    --r->occupied; ++r->cancelled; return 0;
}
unsigned x4_video_pipeline_cancel_ready(X4VideoPipelineRing *r)
{
    if (!r || !r->epoch) return 0;
    unsigned count = 0;
    for (unsigned i = 0; i < OUTPUT_LIMIT(r); ++i)
        if (r->slots[i].state == X4_PIPELINE_READY) {
            r->slots[i].state = X4_PIPELINE_FREE; ++count;
        }
    r->ready -= count; r->occupied -= count; r->cancelled += count;
    return count;
}
bool x4_video_pipeline_ring_idle(const X4VideoPipelineRing *r)
{ return r && r->epoch && r->occupied == 0; }

int x4_video_pipeline_display_init_mode(X4VideoPipelineDisplay *d,uint64_t epoch,
    unsigned count,bool mailbox)
{
    if(!d||!epoch||(count!=3&&count!=4))return X4_PIPELINE_INVALID;
    memset(d,0,sizeof(*d));d->epoch=epoch;d->pending=d->screen=-1;
    d->scanout_count=count;d->mailbox_capable=mailbox;return 0;
}
int x4_video_pipeline_display_init(X4VideoPipelineDisplay *d,uint64_t epoch)
{return x4_video_pipeline_display_init_mode(d,epoch,3,false);}
int x4_video_pipeline_display_init_synced(X4VideoPipelineDisplay *d,uint64_t epoch)
{
    int rc=x4_video_pipeline_display_init_mode(d,epoch,4,false);
    if(!rc)d->synced=true;
    return rc;
}
int x4_video_pipeline_display_occupancy(const X4VideoPipelineDisplay *d,
    X4VideoPipelineDisplayOccupancy *out)
{
    if(out)memset(out,0,sizeof(*out));
    if(!d||!d->epoch||!out||(d->scanout_count!=3&&d->scanout_count!=4))return X4_PIPELINE_INVALID;
    for(unsigned i=0;i<d->scanout_count;++i){
        switch(d->slots[i].state){
        case X4_SCANOUT_FREE:++out->free;break;
        case X4_SCANOUT_WRITING:++out->writing;break;
        case X4_SCANOUT_READY:++out->ready;break;
        case X4_SCANOUT_PENDING:++out->pending;break;
        case X4_SCANOUT_SCREEN:++out->screen;break;
        case X4_SCANOUT_QUARANTINED:++out->quarantined;break;
        default:return X4_PIPELINE_STATE;
        }
    }
    out->in_flight=out->writing+out->ready+out->pending;
    out->occupied=out->in_flight+out->screen+out->quarantined;return 0;
}
static int display_policy(X4VideoPipelineDisplay *d)
{
    X4VideoPipelineDisplayOccupancy o;
    if(x4_video_pipeline_display_occupancy(d,&o)||o.screen>1||o.writing>1||o.pending>1||
        (d->synced?(d->mailbox_capable||o.ready>2):
            (o.ready>1||(!d->mailbox_capable&&o.in_flight>1)))||o.in_flight>3||
        d->pending < -1||d->pending >= (int)d->scanout_count||
        d->screen < -1||d->screen >= (int)d->scanout_count||
        (d->pending>=0?d->slots[d->pending].state!=X4_SCANOUT_PENDING:o.pending!=0)||
        (d->screen>=0?d->slots[d->screen].state!=X4_SCANOUT_SCREEN:o.screen!=0)){
        ++d->rejected;return X4_PIPELINE_STATE;
    }
    if(o.occupied>d->peak_occupied)d->peak_occupied=o.occupied;
    if(o.in_flight>d->peak_in_flight)d->peak_in_flight=o.in_flight;return 0;
}
int x4_video_pipeline_display_mailbox_admission(X4VideoPipelineDisplay *d,bool enabled)
{
    if(!d||!d->epoch||(enabled&&!d->mailbox_capable))return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    /* Disabling admission never revokes an already-issued writer or flip. */
    d->mailbox_admission=enabled;return 0;
}
static X4VideoPipelineLease scanout_lease(const X4VideoPipelineDisplay *d,unsigned slot)
{return (X4VideoPipelineLease){slot,d->epoch,d->slots[slot].lease_generation};}
static int scanout_check(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease,
    X4VideoPipelineScanoutState state)
{
    if(!d||!d->epoch||lease.slot>=d->scanout_count)return X4_PIPELINE_INVALID;
    if(lease.epoch!=d->epoch||!lease.generation||lease.generation!=d->slots[lease.slot].lease_generation){
        ++d->rejected;return X4_PIPELINE_STALE;}
    if(d->slots[lease.slot].state!=state){++d->rejected;return X4_PIPELINE_STATE;}return 0;
}
int x4_video_pipeline_acquire_scanout(X4VideoPipelineDisplay *d,X4VideoPipelineLease *out)
{
    clear_lease(out);if(!d||!d->epoch||!out)return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    if(d->synced){
        X4VideoPipelineDisplayOccupancy o;
        rc=x4_video_pipeline_display_occupancy(d,&o);if(rc)return rc;
        if(o.writing||o.ready>=2)return X4_PIPELINE_WAIT;
    }
    for(unsigned i=0;i<d->scanout_count;++i)
        if(d->slots[i].state==X4_SCANOUT_WRITING||(!d->synced&&!d->mailbox_admission&&
            (d->slots[i].state==X4_SCANOUT_READY||d->slots[i].state==X4_SCANOUT_PENDING)))return X4_PIPELINE_WAIT;
    if(d->next_generation==UINT64_MAX)return X4_PIPELINE_EXHAUSTED;
    for(unsigned i=0;i<d->scanout_count;++i)if(d->slots[i].state==X4_SCANOUT_FREE){
        d->slots[i]=(X4VideoPipelineScanout){.state=X4_SCANOUT_WRITING,
            .lease_generation=++d->next_generation};
        *out=scanout_lease(d,i);return display_policy(d);
    }
    return X4_PIPELINE_WAIT;
}
int x4_video_pipeline_finish_scanout(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease,
    uint64_t output_id,uint64_t decode_end_us,uint64_t converted_us)
{
    int rc=scanout_check(d,lease,X4_SCANOUT_WRITING);if(rc)return rc;
    rc=display_policy(d);if(rc)return rc;
    if(!output_id||converted_us<decode_end_us)return X4_PIPELINE_INVALID;
    if(d->next_ready_order==UINT64_MAX)return X4_PIPELINE_EXHAUSTED;
    X4VideoPipelineScanout *s=&d->slots[lease.slot];
    if(d->mailbox_capable){
        if(output_id<=d->last_submitted_output_id){s->state=X4_SCANOUT_FREE;++d->mailbox_discarded_old;return display_policy(d);}
        for(unsigned i=0;i<d->scanout_count;++i)if(d->slots[i].state==X4_SCANOUT_READY){
            if(output_id<=d->slots[i].output_id){s->state=X4_SCANOUT_FREE;++d->mailbox_discarded_old;return display_policy(d);}
            d->slots[i].state=X4_SCANOUT_FREE;++d->mailbox_replaced;
        }
    }
    s->state=X4_SCANOUT_READY;s->output_id=output_id;s->decode_end_us=decode_end_us;
    s->converted_us=converted_us;s->ready_order=++d->next_ready_order;
    if(!d->ready_since_us)d->ready_since_us=converted_us;
    return display_policy(d);
}
int x4_video_pipeline_abort_scanout(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease)
{
    int rc=scanout_check(d,lease,X4_SCANOUT_WRITING);if(rc)return rc;
    d->slots[lease.slot].state=X4_SCANOUT_FREE;return display_policy(d);
}
int x4_video_pipeline_quarantine_scanout(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease)
{
    int rc=scanout_check(d,lease,X4_SCANOUT_WRITING);if(rc)return rc;
    d->slots[lease.slot].state=X4_SCANOUT_QUARANTINED;return display_policy(d);
}
int x4_video_pipeline_retire_quarantine(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease)
{
    int rc=scanout_check(d,lease,X4_SCANOUT_QUARANTINED);if(rc)return rc;
    d->slots[lease.slot].state=X4_SCANOUT_FREE;return display_policy(d);
}
unsigned x4_video_pipeline_cancel_ready_scanout(X4VideoPipelineDisplay *d)
{
    if(!d||!d->epoch)return 0;unsigned count=0;
    for(unsigned i=0;i<d->scanout_count;++i)if(d->slots[i].state==X4_SCANOUT_READY){d->slots[i].state=X4_SCANOUT_FREE;++count;}
    d->ready_since_us=0;(void)display_policy(d);return count;
}
static void synced_ready_since(X4VideoPipelineDisplay *d)
{
    if(!d->synced)return;
    int oldest=-1;
    for(unsigned i=0;i<d->scanout_count;++i)
        if(d->slots[i].state==X4_SCANOUT_READY&&(oldest<0||
            d->slots[i].ready_order<d->slots[oldest].ready_order))oldest=(int)i;
    d->ready_since_us=oldest<0?0:d->slots[oldest].converted_us;
}
int x4_video_pipeline_cancel_ready_lease(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease)
{
    int rc=scanout_check(d,lease,X4_SCANOUT_READY);if(rc)return rc;
    d->slots[lease.slot].state=X4_SCANOUT_FREE;
    if(d->synced)synced_ready_since(d);else d->ready_since_us=0;
    return display_policy(d);
}
int x4_video_pipeline_display_closed(X4VideoPipelineDisplay *d)
{
    if(!d||!d->epoch)return X4_PIPELINE_INVALID;
    for(unsigned i=0;i<d->scanout_count;++i)if(d->slots[i].state==X4_SCANOUT_WRITING||d->slots[i].state==X4_SCANOUT_QUARANTINED)return X4_PIPELINE_STATE;
    for(unsigned i=0;i<d->scanout_count;++i)d->slots[i].state=X4_SCANOUT_FREE;
    d->pending=d->screen=-1;d->ready_since_us=0;return display_policy(d);
}
int x4_video_pipeline_ready_scanout(X4VideoPipelineDisplay *d,X4VideoPipelineLease *out)
{
    clear_lease(out);if(!d||!d->epoch||!out)return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;if(d->pending>=0)return X4_PIPELINE_WAIT;
    int slot=-1;
    for(unsigned i=0;i<d->scanout_count;++i)if(d->slots[i].state==X4_SCANOUT_READY&&
        (slot<0||(d->mailbox_capable?d->slots[i].ready_order>d->slots[slot].ready_order:
            d->slots[i].ready_order<d->slots[slot].ready_order)))slot=(int)i;
    if(slot<0)return X4_PIPELINE_WAIT;*out=scanout_lease(d,(unsigned)slot);return 0;
}
int x4_video_pipeline_mark_submitted(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease,
    uint64_t flip_arg,uint64_t submitted_us)
{
    int rc=scanout_check(d,lease,X4_SCANOUT_READY);if(rc)return rc;
    rc=display_policy(d);if(rc)return rc;
    X4VideoPipelineScanout *s=&d->slots[lease.slot];
    if(d->pending>=0||!flip_arg||flip_arg<=d->last_submitted_arg||submitted_us<s->converted_us||
        (d->mailbox_capable&&s->output_id<=d->last_submitted_output_id))return X4_PIPELINE_INVALID;
    s->state=X4_SCANOUT_PENDING;s->flip_arg=flip_arg;s->submitted_us=submitted_us;
    d->pending=(int)lease.slot;d->last_submitted_arg=flip_arg;d->last_submitted_output_id=s->output_id;
    d->ready_since_us=0;
    if(d->synced)synced_ready_since(d);
    ++d->submitted;return display_policy(d);
}
void x4_video_pipeline_sync_history_init(X4VideoPipelineSyncHistory *h)
{if(h)memset(h,0,sizeof(*h));}
int x4_video_pipeline_synced_observed(X4VideoPipelineDisplay *d,X4VideoPipelineSyncHistory *h,uint64_t now)
{
    if(!d||!h||!d->synced||!now)return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    /* A long display gap is not a continuous backlog window. A missing READY
     * sample similarly requires a whole new window before recovering. */
    if(!h->synced_last_observed_us||now<h->synced_last_observed_us||
        now-h->synced_last_observed_us>50000u)
        h->synced_history_count=h->synced_history_next=0;
    h->synced_last_observed_us=now;
    unsigned ready=0;int oldest=-1;
    for(unsigned i=0;i<d->scanout_count;++i)if(d->slots[i].state==X4_SCANOUT_READY){
        ++ready;
        if(oldest<0||d->slots[i].ready_order<d->slots[oldest].ready_order)oldest=(int)i;
    }
    h->synced_history[h->synced_history_next]=(uint8_t)ready;
    h->synced_history_next=(h->synced_history_next+1u)%30u;
    if(h->synced_history_count<30u)++h->synced_history_count;
    if(h->synced_history_count<30u)return 0;
    for(unsigned i=0;i<30u;++i)if(!h->synced_history[i])return 0;
    if(oldest>=0){
        h->synced_last_dropped_output=d->slots[oldest].output_id;
        d->slots[oldest].state=X4_SCANOUT_FREE;
        ++h->synced_recovery_drops;
        synced_ready_since(d);
    }
    h->synced_history_count=h->synced_history_next=0;
    return display_policy(d);
}
int x4_video_pipeline_observe_flip(X4VideoPipelineDisplay *d,uint64_t flip_arg,
    uint64_t observed_us,X4VideoPipelineLease *presented)
{
    clear_lease(presented);if(!d||!d->epoch||!presented)return X4_PIPELINE_INVALID;
    int rc=display_policy(d);if(rc)return rc;
    if(d->pending<0||d->slots[d->pending].flip_arg!=flip_arg)return X4_PIPELINE_WAIT;
    X4VideoPipelineScanout *s=&d->slots[d->pending];
    if(observed_us<s->submitted_us||(d->mailbox_capable&&d->screen>=0&&
        s->output_id<=d->slots[d->screen].output_id))return X4_PIPELINE_INVALID;
    if(d->screen>=0)d->slots[d->screen].state=X4_SCANOUT_FREE;
    d->screen=d->pending;d->pending=-1;s->state=X4_SCANOUT_SCREEN;s->observed_us=observed_us;
    ++d->observed;*presented=scanout_lease(d,(unsigned)d->screen);return display_policy(d);
}
void x4_video_pipeline_mailbox_clock_init(X4VideoPipelineMailboxClock *c)
{if(c){memset(c,0,sizeof(*c));c->period_us=16667;c->margin_us=2000;}}
bool x4_video_pipeline_mailbox_anchor(X4VideoPipelineMailboxClock *c,uint64_t ptime,
    uint64_t submitted,uint64_t observed,bool vsync)
{
    if(!c||!vsync)return false;
    if(!c->period_us||!ptime||ptime<submitted||ptime>observed||ptime<=c->anchor_us){
        ++c->invalid_anchors;c->valid=false;return false;}
    bool valid=false;
    if(c->anchor_us){
        uint64_t delta=ptime-c->anchor_us;
        uint64_t cycles=delta/c->period_us;
        if(delta%c->period_us>=c->period_us/2)++cycles;
        if(cycles&&cycles<=UINT64_MAX/c->period_us){
            uint64_t sample=delta/cycles,expected=cycles*c->period_us;
            uint64_t error=delta>expected?delta-expected:expected-delta;
            ++c->residual_samples;c->residual_sum_us+=error;
            if(error>c->residual_max_us)c->residual_max_us=error;
            /* Nominal 60 Hz only seeds cycle matching. Measured ptime deltas
             * must remain plausible; the undocumented Vblank ABI is unused. */
            if(sample>=15000&&sample<=18000&&error<=1000){
                c->period_us=(7*c->period_us+sample+4)/8;valid=true;
            }
        }
        if(!valid)++c->invalid_anchors;
    }
    c->anchor_us=ptime;c->target_us=0;c->valid=valid;++c->anchors;return valid;
}
int x4_video_pipeline_mailbox_due(X4VideoPipelineMailboxClock *c,uint64_t now,
    uint64_t ready_since,uint64_t *target)
{
    if(target)*target=0;
    if(!c||!target||!c->period_us||!ready_since||now<ready_since)return X4_PIPELINE_INVALID;
    if(!c->valid||now<c->anchor_us||now-c->anchor_us>1000000){
        c->valid=false;c->target_us=0;++c->warmup_submissions;return 0;}
    if(!c->target_us){
        if(c->period_us>UINT64_MAX-c->anchor_us)return X4_PIPELINE_EXHAUSTED;
        c->target_us=c->anchor_us+c->period_us;
    }
    if(now>=c->target_us){
        uint64_t missed=(now-c->target_us)/c->period_us+1;
        if(c->margin_us>UINT64_MAX-ready_since)return X4_PIPELINE_EXHAUSTED;
        uint64_t first_safe=ready_since+c->margin_us,first=c->target_us;
        if(first<first_safe){
            uint64_t gap=first_safe-first,skip=gap/c->period_us+(gap%c->period_us!=0);
            if(skip>(UINT64_MAX-first)/c->period_us)return X4_PIPELINE_EXHAUSTED;
            first+=skip*c->period_us;
        }
        if(first<=now)c->missed_opportunities+=(now-first)/c->period_us+1;
        if(missed>(UINT64_MAX-c->target_us)/c->period_us)return X4_PIPELINE_EXHAUSTED;
        c->target_us+=missed*c->period_us;
    }
    *target=c->target_us;
    return c->target_us-now>c->margin_us?X4_PIPELINE_WAIT:0;
}

int x4_video_pipeline_stripe(unsigned height, unsigned workers, unsigned index,
    X4VideoPipelineStripe *out)
{
    if (!out || !height || height > X4_PIPELINE_HEIGHT ||
        (workers != 2 && workers != 3) || index >= workers || height < workers)
        return X4_PIPELINE_INVALID;
    *out = (X4VideoPipelineStripe){height * index / workers, height * (index + 1) / workers};
    return 0;
}
static int validated_layout(const X4VideoNv12Layout *l, size_t source_bytes)
{
    if (!l || !((l->visible_width == 1280 && l->visible_height == 720 &&
            l->coded_width == 1280 && l->coded_height == 720 && l->storage_height == 720) ||
        (l->visible_width == 1920 && l->visible_height == 1080 &&
            l->coded_width == 1920 && l->coded_height == 1088 && l->storage_height == 1088)) ||
        l->pitch < l->coded_width || l->pitch > X4_VIDEO_MAX_PITCH || (l->pitch & 1))
        return X4_PIPELINE_INVALID;
    size_t y = (size_t)l->pitch * l->storage_height, uv = y / 2;
    size_t pixels = (size_t)l->visible_width * l->visible_height;
    if (l->y_bytes != y || l->uv_bytes != uv || l->uv_plane_offset != y ||
        l->total_nv12_bytes != y + uv || l->visible_y_offset != 0 ||
        l->visible_uv_offset != y || l->rgb_pixels != pixels ||
        l->rgb_bytes != pixels * sizeof(uint32_t) || source_bytes < y + uv)
        return X4_PIPELINE_INVALID;
    return 0;
}
static int validated_destination(const uint8_t *source, size_t source_bytes,
    uint32_t *dst, size_t bytes, unsigned width, unsigned height, unsigned pitch)
{
    if (!source || !dst || ((uintptr_t)dst & (sizeof(uint32_t) - 1)) ||
        pitch < width || pitch > X4_VIDEO_MAX_PITCH ||
        bytes < (size_t)pitch * height * sizeof(uint32_t)) return X4_PIPELINE_INVALID;
    uintptr_t a = (uintptr_t)source, b = (uintptr_t)dst;
    size_t written_span = (size_t)pitch * height * sizeof(uint32_t);
    if (source_bytes > UINTPTR_MAX - a || written_span > UINTPTR_MAX - b ||
        (a < b + written_span && b < a + source_bytes)) return X4_PIPELINE_INVALID;
    return 0;
}
static unsigned clamp_channel(int v)
{ return v < 0 ? 0u : v > 255 ? 255u : (unsigned)v; }
static uint32_t scalar_pixel(unsigned y, unsigned u, unsigned v)
{
    int c = (int)y - 16, d = (int)u - 128, e = (int)v - 128;
    if (c < 0) c = 0;
    unsigned r = clamp_channel((298*c + 409*e + 128) >> 8);
    unsigned g = clamp_channel((298*c - 100*d - 208*e + 128) >> 8);
    unsigned b = clamp_channel((298*c + 516*d + 128) >> 8);
    return 0xff000000u | b << 16 | g << 8 | r;
}

#if !X4_PIPELINE_SSE2
static inline uint32_t blend_argb(uint32_t c0, uint32_t c1, unsigned w0, unsigned w1)
{
    unsigned b0 = c0 & 0xff, g0 = (c0 >> 8) & 0xff, r0 = (c0 >> 16) & 0xff, a0 = (c0 >> 24) & 0xff;
    unsigned b1 = c1 & 0xff, g1 = (c1 >> 8) & 0xff, r1 = (c1 >> 16) & 0xff, a1 = (c1 >> 24) & 0xff;
    unsigned b = (b0 * w0 + b1 * w1 + 128) >> 8;
    unsigned g = (g0 * w0 + g1 * w1 + 128) >> 8;
    unsigned r = (r0 * w0 + r1 * w1 + 128) >> 8;
    unsigned a = (a0 * w0 + a1 * w1 + 128) >> 8;
    return (a << 24) | (r << 16) | (g << 8) | b;
}
static inline void scale_row_1280_to_1920_scalar(uint32_t *dst, const uint32_t *src)
{
    for (unsigned k = 0; k < 640; ++k) {
        uint32_t s0 = src[2 * k];
        uint32_t s1 = src[2 * k + 1];
        uint32_t s2 = (2 * k + 2 < 1280) ? src[2 * k + 2] : s1;
        dst[3 * k + 0] = s0;
        dst[3 * k + 1] = blend_argb(s0, s1, 85, 171);
        dst[3 * k + 2] = blend_argb(s1, s2, 171, 85);
    }
}
static inline void blend_rows_1280_scalar(uint32_t *dst, const uint32_t *r0, const uint32_t *r1,
    unsigned w0, unsigned w1)
{
    for (unsigned x = 0; x < 1280; ++x) {
        dst[x] = blend_argb(r0[x], r1[x], w0, w1);
    }
}
#endif
#if X4_PIPELINE_SSE2
static inline __m128i blend_4px_sse2(__m128i c0, __m128i c1, __m128i w0, __m128i w1,
    __m128i zero, __m128i round)
{
    __m128i c0_lo = _mm_unpacklo_epi8(c0, zero);
    __m128i c1_lo = _mm_unpacklo_epi8(c1, zero);
    __m128i s_lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(c0_lo, w0),
        _mm_mullo_epi16(c1_lo, w1)), round), 8);
    __m128i c0_hi = _mm_unpackhi_epi8(c0, zero);
    __m128i c1_hi = _mm_unpackhi_epi8(c1, zero);
    __m128i s_hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(c0_hi, w0),
        _mm_mullo_epi16(c1_hi, w1)), round), 8);
    return _mm_packus_epi16(s_lo, s_hi);
}
static void scale_row_1280_to_1920_sse2(uint32_t *dst, const uint32_t *src)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi16(128);
    const __m128i w85 = _mm_set1_epi16(85);
    const __m128i w171 = _mm_set1_epi16(171);

    for (unsigned x = 0; x < 1280; x += 8) {
        __m128i s_lo = _mm_loadu_si128((const __m128i *)(src + x));
        __m128i s_hi = _mm_loadu_si128((const __m128i *)(src + x + 4));

        __m128i E = (__m128i)_mm_shuffle_ps((__m128)s_lo, (__m128)s_hi, _MM_SHUFFLE(2, 0, 2, 0));
        __m128i O = (__m128i)_mm_shuffle_ps((__m128)s_lo, (__m128)s_hi, _MM_SHUFFLE(3, 1, 3, 1));

        uint32_t s8 = (x + 8 < 1280) ? src[x + 8] : src[1279];
        __m128i s8_v = _mm_cvtsi32_si128(s8);
        __m128i E_shift = _mm_srli_si128(E, 4);
        __m128i high_pair = _mm_unpacklo_epi32(_mm_srli_si128(E, 12), s8_v);
        __m128i Enext = _mm_unpacklo_epi64(E_shift, high_pair);

        __m128i B1 = blend_4px_sse2(E, O, w85, w171, zero, round);
        __m128i B2 = blend_4px_sse2(O, Enext, w171, w85, zero, round);

        __m128i EB1_lo = _mm_unpacklo_epi32(E, B1);
        __m128i EB1_hi = _mm_unpackhi_epi32(E, B1);

        __m128i out0 = (__m128i)_mm_shuffle_ps((__m128)EB1_lo,
            (__m128)_mm_unpacklo_epi32(B2, _mm_srli_si128(EB1_lo, 8)),
            _MM_SHUFFLE(1, 0, 1, 0));

        __m128i b11_b21 = _mm_unpacklo_epi32(_mm_srli_si128(EB1_lo, 12), _mm_srli_si128(B2, 4));
        __m128i out1 = (__m128i)_mm_shuffle_ps((__m128)b11_b21, (__m128)EB1_hi, _MM_SHUFFLE(1, 0, 1, 0));

        __m128i b22_e3 = _mm_unpacklo_epi32(_mm_srli_si128(B2, 8), _mm_srli_si128(EB1_hi, 8));
        __m128i b13_b23 = _mm_unpacklo_epi32(_mm_srli_si128(EB1_hi, 12), _mm_srli_si128(B2, 12));
        __m128i out2 = (__m128i)_mm_shuffle_ps((__m128)b22_e3, (__m128)b13_b23, _MM_SHUFFLE(1, 0, 1, 0));

        uint32_t *out = dst + x * 3 / 2;
        _mm_storeu_si128((__m128i *)out, out0);
        _mm_storeu_si128((__m128i *)(out + 4), out1);
        _mm_storeu_si128((__m128i *)(out + 8), out2);
    }
}
static void blend_rows_1280_sse2(uint32_t *dst, const uint32_t *r0, const uint32_t *r1,
    unsigned w0, unsigned w1)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i round = _mm_set1_epi16(128);
    const __m128i w0_vec = _mm_set1_epi16((short)w0);
    const __m128i w1_vec = _mm_set1_epi16((short)w1);

    for (unsigned x = 0; x < 1280; x += 4) {
        __m128i c0 = _mm_loadu_si128((const __m128i *)(r0 + x));
        __m128i c1 = _mm_loadu_si128((const __m128i *)(r1 + x));
        __m128i res = blend_4px_sse2(c0, c1, w0_vec, w1_vec, zero, round);
        _mm_storeu_si128((__m128i *)(dst + x), res);
    }
}
#endif
static inline void scale_row_1280_to_1920(uint32_t *dst, const uint32_t *src)
{
#if X4_PIPELINE_SSE2
    scale_row_1280_to_1920_sse2(dst, src);
#else
    scale_row_1280_to_1920_scalar(dst, src);
#endif
}
static inline void blend_rows_1280(uint32_t *dst, const uint32_t *r0, const uint32_t *r1,
    unsigned w0, unsigned w1)
{
#if X4_PIPELINE_SSE2
    blend_rows_1280_sse2(dst, r0, r1, w0, w1);
#else
    blend_rows_1280_scalar(dst, r0, r1, w0, w1);
#endif
}
static inline unsigned scalar_sharpen_luma(const uint8_t *y_plane, unsigned pitch,
    unsigned crop_x, unsigned crop_y, unsigned visible_w, unsigned visible_h,
    unsigned sx, unsigned sy, unsigned level)
{
    unsigned y_center = y_plane[(size_t)(crop_y + sy) * pitch + crop_x + sx];
    if (!level) return y_center;
    unsigned sy_up = (sy > 0) ? sy - 1 : 0;
    unsigned sy_down = (sy < visible_h - 1) ? sy + 1 : sy;
    unsigned sx_left = (sx > 0) ? sx - 1 : 0;
    unsigned sx_right = (sx < visible_w - 1) ? sx + 1 : sx;
    unsigned y_up = y_plane[(size_t)(crop_y + sy_up) * pitch + crop_x + sx];
    unsigned y_down = y_plane[(size_t)(crop_y + sy_down) * pitch + crop_x + sx];
    unsigned y_left = y_plane[(size_t)(crop_y + sy) * pitch + crop_x + sx_left];
    unsigned y_right = y_plane[(size_t)(crop_y + sy) * pitch + crop_x + sx_right];
    int delta = (int)(4 * y_center) - (int)y_up - (int)y_down - (int)y_left - (int)y_right;
    int adj = (level == 1) ? ((delta + 8) >> 4) : ((delta + 4) >> 3);
    int val = (int)y_center + adj;
    return (val < 0) ? 0 : ((val > 255) ? 255 : (unsigned)val);
}
#if X4_PIPELINE_SSE2
/* Same BT.601 integer math, saturation and byte order as live_h264.c. Only
 * register-sized blocks exist; there is no intermediate NV12/RGB image. */
static void rgb_eight(__m128i y, __m128i u, __m128i v, __m128i *first, __m128i *second)
{
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi32(128);
    const __m128i rc = _mm_setr_epi16(298,409,298,409,298,409,298,409);
    const __m128i bc = _mm_setr_epi16(298,516,298,516,298,516,298,516);
    const __m128i gc = _mm_setr_epi16(-100,-208,-100,-208,-100,-208,-100,-208);
    const __m128i yc = _mm_set1_epi32(298), alpha = _mm_set1_epi8((char)-1);
    __m128i rl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,v),rc)),8);
    __m128i rh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,v),rc)),8);
    __m128i bl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,u),bc)),8);
    __m128i bh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,u),bc)),8);
    __m128i gl = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpacklo_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpacklo_epi16(u,v),gc))),8);
    __m128i gh = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpackhi_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpackhi_epi16(u,v),gc))),8);
    __m128i r = _mm_packus_epi16(_mm_packs_epi32(rl,rh),zero);
    __m128i g = _mm_packus_epi16(_mm_packs_epi32(gl,gh),zero);
    __m128i b = _mm_packus_epi16(_mm_packs_epi32(bl,bh),zero);
    __m128i rg = _mm_unpacklo_epi8(r,g), ba = _mm_unpacklo_epi8(b,alpha);
    *first = _mm_unpacklo_epi16(rg,ba); *second = _mm_unpackhi_epi16(rg,ba);
}
static void convert_row_sse2(uint32_t *dst, const uint8_t *y, const uint8_t *uv,
    unsigned source_width, bool scale_three_halves)
{
    const __m128i zero = _mm_setzero_si128(), ybias = _mm_set1_epi16(16), uvbias = _mm_set1_epi16(128);
    for (unsigned x = 0; x < source_width; x += 8) {
        __m128i luma = _mm_subs_epu16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(y+x)),zero),ybias);
        __m128i chroma = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(uv+x)),zero),uvbias);
        __m128i u = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(2,2,0,0)),_MM_SHUFFLE(2,2,0,0));
        __m128i v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(3,3,1,1)),_MM_SHUFFLE(3,3,1,1));
        __m128i first, second;
        rgb_eight(luma,u,v,&first,&second);
        if (scale_three_halves) {
            uint32_t *out = dst + x * 3 / 2;
            /* Eight source pixels become [0,0,1,2,2,3,4,4,5,6,6,7].
             * This is exactly floor(destination_x*2/3), using registers only. */
            _mm_storeu_si128((__m128i *)out,
                _mm_shuffle_epi32(first,_MM_SHUFFLE(2,1,0,0)));
            _mm_storeu_si128((__m128i *)(out+4),_mm_unpackhi_epi64(first,
                _mm_shuffle_epi32(second,_MM_SHUFFLE(0,0,0,0))));
            _mm_storeu_si128((__m128i *)(out+8),
                _mm_shuffle_epi32(second,_MM_SHUFFLE(3,2,2,1)));
        } else {
            _mm_storeu_si128((__m128i *)(dst+x),first);
            _mm_storeu_si128((__m128i *)(dst+x+4),second);
        }
    }
}
static void chroma_vertical_row_sse2(uint8_t *c_vert, const uint8_t *uv_j,
    const uint8_t *uv_other, unsigned width)
{
    const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16(2);
    for (unsigned x = 0; x < width; x += 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)(uv_j + x));
        __m128i b = _mm_loadu_si128((const __m128i *)(uv_other + x));
        __m128i a_lo = _mm_unpacklo_epi8(a, zero);
        __m128i b_lo = _mm_unpacklo_epi8(b, zero);
        __m128i sum_lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(a_lo, 1), a_lo),
            _mm_add_epi16(b_lo, two)), 2);
        __m128i a_hi = _mm_unpackhi_epi8(a, zero);
        __m128i b_hi = _mm_unpackhi_epi8(b, zero);
        __m128i sum_hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(a_hi, 1), a_hi),
            _mm_add_epi16(b_hi, two)), 2);
        __m128i res = _mm_packus_epi16(sum_lo, sum_hi);
        _mm_storeu_si128((__m128i *)(c_vert + x), res);
    }
}
static inline __m128i sharpen_luma_sse2(const uint8_t *y_plane, unsigned pitch,
    unsigned crop_x, unsigned crop_y, unsigned visible_w, unsigned visible_h,
    unsigned sx, unsigned sy, unsigned level, __m128i ybias)
{
    const __m128i zero = _mm_setzero_si128();
    const uint8_t *y_row = y_plane + (size_t)(crop_y + sy) * pitch + crop_x;
    __m128i center_raw = _mm_loadl_epi64((const __m128i *)(y_row + sx));
    if (!level) {
        return _mm_sub_epi16(_mm_unpacklo_epi8(center_raw, zero), ybias);
    }
    unsigned sy_up = (sy > 0) ? sy - 1 : 0;
    unsigned sy_down = (sy < visible_h - 1) ? sy + 1 : sy;
    const uint8_t *y_up_row = y_plane + (size_t)(crop_y + sy_up) * pitch + crop_x;
    const uint8_t *y_down_row = y_plane + (size_t)(crop_y + sy_down) * pitch + crop_x;
    __m128i up_raw = _mm_loadl_epi64((const __m128i *)(y_up_row + sx));
    __m128i down_raw = _mm_loadl_epi64((const __m128i *)(y_down_row + sx));

    __m128i left_raw, right_raw;
    if (sx > 0 && sx + 8 < visible_w) {
        left_raw = _mm_loadl_epi64((const __m128i *)(y_row + sx - 1));
        right_raw = _mm_loadl_epi64((const __m128i *)(y_row + sx + 1));
    } else {
        uint8_t left_bytes[8], right_bytes[8];
        for (int k = 0; k < 8; ++k) {
            unsigned px = sx + (unsigned)k;
            left_bytes[k] = (px > 0) ? y_row[px - 1] : y_row[0];
            right_bytes[k] = (px < visible_w - 1) ? y_row[px + 1] : y_row[visible_w - 1];
        }
        left_raw = _mm_loadl_epi64((const __m128i *)left_bytes);
        right_raw = _mm_loadl_epi64((const __m128i *)right_bytes);
    }

    __m128i c16 = _mm_unpacklo_epi8(center_raw, zero);
    __m128i u16 = _mm_unpacklo_epi8(up_raw, zero);
    __m128i d16 = _mm_unpacklo_epi8(down_raw, zero);
    __m128i l16 = _mm_unpacklo_epi8(left_raw, zero);
    __m128i r16 = _mm_unpacklo_epi8(right_raw, zero);

    __m128i delta = _mm_sub_epi16(_mm_sub_epi16(_mm_slli_epi16(c16, 2), _mm_add_epi16(u16, d16)),
        _mm_add_epi16(l16, r16));
    __m128i round = (level == 1) ? _mm_set1_epi16(8) : _mm_set1_epi16(4);
    int shift = (level == 1) ? 4 : 3;
    __m128i adj = _mm_srai_epi16(_mm_add_epi16(delta, round), shift);
    __m128i sharp = _mm_add_epi16(c16, adj);
    __m128i clamped = _mm_packus_epi16(sharp, zero);
    return _mm_sub_epi16(_mm_unpacklo_epi8(clamped, zero), ybias);
}
#if defined(X4_CONVERT_LEGACY_SMOOTH) && X4_CONVERT_LEGACY_SMOOTH
static inline void convert_row_smooth_sse2(uint32_t *dst, const uint8_t *source,
    size_t uv_plane_offset, unsigned pitch, unsigned visible_w, unsigned visible_h,
    unsigned sy)
{
    unsigned j_max = (visible_h / 2) - 1;
    unsigned j = sy / 2;
    unsigned j_other = ((sy & 1) == 0) ? ((j > 0) ? j - 1 : 0) : ((j < j_max) ? j + 1 : j);

    const uint8_t *y_plane = source;
    const uint8_t *uv_j = source + uv_plane_offset + (size_t)j * pitch;
    const uint8_t *uv_other = source + uv_plane_offset + (size_t)j_other * pitch;

    uint8_t c_vert[1928];
    chroma_vertical_row_sse2(c_vert, uv_j, uv_other, visible_w);
    c_vert[visible_w] = c_vert[visible_w - 2];
    c_vert[visible_w + 1] = c_vert[visible_w - 1];
    c_vert[visible_w + 2] = c_vert[visible_w - 2];
    c_vert[visible_w + 3] = c_vert[visible_w - 1];

    const __m128i zero = _mm_setzero_si128(), ybias = _mm_set1_epi16(16), uvbias = _mm_set1_epi16(128);

    for (unsigned x = 0; x < visible_w; x += 8) {
        __m128i center_raw = _mm_loadl_epi64((const __m128i *)(y_plane + (size_t)sy * pitch + x));
        __m128i luma = _mm_subs_epu16(_mm_unpacklo_epi8(center_raw, zero), ybias);

        __m128i cur = _mm_loadu_si128((const __m128i *)(c_vert + x));
        __m128i nxt = _mm_srli_si128(cur, 2);
        __m128i avg = _mm_avg_epu8(cur, nxt);
        __m128i pairs = _mm_unpacklo_epi16(cur, avg);
        __m128i u = _mm_sub_epi16(_mm_and_si128(pairs, _mm_set1_epi16(0x00ff)), uvbias);
        __m128i v = _mm_sub_epi16(_mm_srli_epi16(pairs, 8), uvbias);

        __m128i first, second;
        rgb_eight(luma, u, v, &first, &second);
        _mm_storeu_si128((__m128i *)(dst + x), first);
        _mm_storeu_si128((__m128i *)(dst + x + 4), second);
    }
}
#endif
#endif

#if defined(X4_CONVERT_LEGACY_SMOOTH) && X4_CONVERT_LEGACY_SMOOTH
static inline void convert_row_smooth_scalar(uint32_t *dst, const uint8_t *source,
    size_t uv_plane_offset, unsigned pitch, unsigned visible_w, unsigned visible_h,
    unsigned sy)
{
    unsigned j_max = (visible_h / 2) - 1;
    unsigned j = sy / 2;
    unsigned j_other = ((sy & 1) == 0) ? ((j > 0) ? j - 1 : 0) : ((j < j_max) ? j + 1 : j);

    const uint8_t *y_plane = source;
    const uint8_t *uv_j = source + uv_plane_offset + (size_t)j * pitch;
    const uint8_t *uv_other = source + uv_plane_offset + (size_t)j_other * pitch;

    for (unsigned sx = 0; sx < visible_w; ++sx) {
        unsigned y_val = y_plane[(size_t)sy * pitch + sx];
        unsigned i = sx / 2;
        unsigned i_next = (i < (visible_w / 2) - 1) ? i + 1 : i;

        unsigned u_vi = (3u * uv_j[2 * i] + uv_other[2 * i] + 2u) >> 2;
        unsigned v_vi = (3u * uv_j[2 * i + 1] + uv_other[2 * i + 1] + 2u) >> 2;

        unsigned u_final, v_final;
        if ((sx & 1) == 0) {
            u_final = u_vi;
            v_final = v_vi;
        } else {
            unsigned u_vnext = (3u * uv_j[2 * i_next] + uv_other[2 * i_next] + 2u) >> 2;
            unsigned v_vnext = (3u * uv_j[2 * i_next + 1] + uv_other[2 * i_next + 1] + 2u) >> 2;
            u_final = (u_vi + u_vnext + 1u) >> 1;
            v_final = (v_vi + v_vnext + 1u) >> 1;
        }
        dst[sx] = scalar_pixel(y_val, u_final, v_final);
    }
}
#endif
static void convert_rows(const X4VideoNv12Layout *l, const uint8_t *source,
    uint32_t *dst, unsigned dst_width, unsigned dst_height, unsigned dst_pitch,
    unsigned first_row, unsigned end_row)
{
#if defined(X4_CONVERT_LEGACY_SMOOTH) && X4_CONVERT_LEGACY_SMOOTH
    if (l->visible_width == 1920 && dst_width == 1920 && dst_height == 1080) {
        for (unsigned dy = first_row; dy < end_row; ++dy) {
            uint32_t *row = dst + (size_t)dy * dst_pitch;
#if X4_PIPELINE_SSE2
            convert_row_smooth_sse2(row, source, l->uv_plane_offset, l->pitch, 1920, 1080, dy);
#else
            convert_row_smooth_scalar(row, source, l->uv_plane_offset, l->pitch, 1920, 1080, dy);
#endif
        }
        return;
    } else if (l->visible_width == 1280 && l->visible_height == 720 && dst_width == 1920 && dst_height == 1080) {
        uint32_t rgb_rows[3][1280];
        uint32_t rgb_blend[1280];
        int cached_sy[3] = {-1, -1, -1};
        unsigned m_start = first_row / 3;
        unsigned m_end = (end_row + 2) / 3;
        for (unsigned m = m_start; m < m_end; ++m) {
            unsigned sy0 = 2 * m;
            unsigned sy1 = 2 * m + 1;
            unsigned sy2 = (2 * m + 2 < 720) ? (2 * m + 2) : 719;

            unsigned slot0 = sy0 % 3;
            if (cached_sy[slot0] != (int)sy0) {
#if X4_PIPELINE_SSE2
                convert_row_smooth_sse2(rgb_rows[slot0], source, l->uv_plane_offset, l->pitch, 1280, 720, sy0);
#else
                convert_row_smooth_scalar(rgb_rows[slot0], source, l->uv_plane_offset, l->pitch, 1280, 720, sy0);
#endif
                cached_sy[slot0] = (int)sy0;
            }
            unsigned slot1 = sy1 % 3;
            if (cached_sy[slot1] != (int)sy1) {
#if X4_PIPELINE_SSE2
                convert_row_smooth_sse2(rgb_rows[slot1], source, l->uv_plane_offset, l->pitch, 1280, 720, sy1);
#else
                convert_row_smooth_scalar(rgb_rows[slot1], source, l->uv_plane_offset, l->pitch, 1280, 720, sy1);
#endif
                cached_sy[slot1] = (int)sy1;
            }
            unsigned slot2 = sy2 % 3;
            if (cached_sy[slot2] != (int)sy2) {
#if X4_PIPELINE_SSE2
                convert_row_smooth_sse2(rgb_rows[slot2], source, l->uv_plane_offset, l->pitch, 1280, 720, sy2);
#else
                convert_row_smooth_scalar(rgb_rows[slot2], source, l->uv_plane_offset, l->pitch, 1280, 720, sy2);
#endif
                cached_sy[slot2] = (int)sy2;
            }

            unsigned dy0 = 3 * m;
            if (dy0 >= first_row && dy0 < end_row) {
                scale_row_1280_to_1920(dst + (size_t)dy0 * dst_pitch, rgb_rows[slot0]);
            }
            unsigned dy1 = 3 * m + 1;
            if (dy1 >= first_row && dy1 < end_row) {
                blend_rows_1280(rgb_blend, rgb_rows[slot0], rgb_rows[slot1], 85, 171);
                scale_row_1280_to_1920(dst + (size_t)dy1 * dst_pitch, rgb_blend);
            }
            unsigned dy2 = 3 * m + 2;
            if (dy2 >= first_row && dy2 < end_row) {
                blend_rows_1280(rgb_blend, rgb_rows[slot1], rgb_rows[slot2], 171, 85);
                scale_row_1280_to_1920(dst + (size_t)dy2 * dst_pitch, rgb_blend);
            }
        }
        return;
    }
#endif
    for (unsigned dy = first_row; dy < end_row; ++dy) {
        unsigned sy = dy * l->visible_height / dst_height;
        const uint8_t *y = source + (size_t)sy * l->pitch;
        const uint8_t *uv = source + l->uv_plane_offset + (size_t)(sy / 2) * l->pitch;
        uint32_t *row = dst + (size_t)dy * dst_pitch;
#if X4_PIPELINE_SSE2
        if (dst_width == l->visible_width ||
            (l->visible_width == 1280 && dst_width == 1920)) {
            convert_row_sse2(row,y,uv,l->visible_width,dst_width != l->visible_width);
            continue;
        }
#endif
        /* floor(x*source_width/destination_width), identical to the retained
         * live_media_draw nearest-neighbour map. No conversion of extra rows. */
        for (unsigned dx = 0; dx < dst_width; ++dx) {
            unsigned sx = dx * l->visible_width / dst_width, cx = sx & ~1u;
            row[dx] = scalar_pixel(y[sx], uv[cx], uv[cx+1]);
        }
    }
}
int x4_video_pipeline_convert_stripe(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes,
    unsigned dst_pitch, unsigned workers, unsigned index)
{
    X4VideoPipelineStripe stripe;
    int rc = x4_video_pipeline_stripe(X4_PIPELINE_HEIGHT,workers,index,&stripe);
    if (rc || validated_layout(l,source_bytes) || validated_destination(source,source_bytes,
            dst,dst_bytes,X4_PIPELINE_WIDTH,X4_PIPELINE_HEIGHT,dst_pitch)) return X4_PIPELINE_INVALID;
    convert_rows(l,source,dst,X4_PIPELINE_WIDTH,X4_PIPELINE_HEIGHT,dst_pitch,stripe.first_row,stripe.end_row);
#if defined(__SSE2__)
    /* Each converter drains its own WC writes before the caller publishes
     * completion. A later thread's fence cannot order this worker's stores. */
    _mm_sfence();
#endif
    return 0;
}
int x4_video_pipeline_convert_reference(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes)
{
    if (validated_layout(l,source_bytes) || validated_destination(source,source_bytes,
            dst,dst_bytes,l->visible_width,l->visible_height,l->visible_width)) return X4_PIPELINE_INVALID;
    convert_rows(l,source,dst,l->visible_width,l->visible_height,l->visible_width,0,l->visible_height);
    return 0;
}

/* New streaming conversion deliberately leaves the legacy/probe Q8 converter
 * above unchanged. Coefficients below are Q13, rounded from the inverse Y'CbCr
 * equations using Kr/Kb=0.299/0.114 (BT.601) or 0.2126/0.0722 (BT.709).
 * Limited range scales Y by 255/219 and chroma by 255/224; full range uses
 * unity Y and unscaled chroma. No source-range clamp precedes the matrix:
 * out-of-nominal Y/C values are clipped only after RGB reconstruction. */
typedef struct { int y, rv, gu, gv, bu, y_bias; } ColorCoefficients;
static ColorCoefficients color_coefficients(const X4VideoPipelineColor *color)
{
    bool hd = color->matrix_coefficients == 1;
    if (color->full_range)
        return hd ? (ColorCoefficients){8192,12901,-1535,-3835,15201,0} :
            (ColorCoefficients){8192,11485,-2819,-5850,14516,0};
    return hd ? (ColorCoefficients){9539,14686,-1747,-4366,17305,16} :
        (ColorCoefficients){9539,13075,-3209,-6660,16525,16};
}
static int validated_color_layout(const X4VideoNv12Layout *l, size_t bytes,
    unsigned *crop_x, unsigned *crop_y)
{
    if (!l || !crop_x || !crop_y || !l->visible_width || !l->visible_height ||
        !l->coded_width || !l->coded_height || l->coded_width > 1920 ||
        l->coded_height > 1088 || (l->coded_width & 1) || (l->coded_height & 1) ||
        l->storage_height < l->coded_height || l->storage_height > 1088 ||
        (l->storage_height & 1) || l->pitch < l->coded_width ||
        l->pitch > X4_VIDEO_MAX_PITCH || (l->pitch & 1)) return X4_PIPELINE_INVALID;
    size_t y = (size_t)l->pitch * l->storage_height, uv = y / 2;
    if (l->visible_y_offset >= y) return X4_PIPELINE_INVALID;
    unsigned cx = (unsigned)(l->visible_y_offset % l->pitch);
    unsigned cy = (unsigned)(l->visible_y_offset / l->pitch);
    if (cx >= l->coded_width || cy >= l->coded_height ||
        l->visible_width > l->coded_width - cx ||
        l->visible_height > l->coded_height - cy) return X4_PIPELINE_INVALID;
    size_t pixels = (size_t)l->visible_width * l->visible_height;
    if (l->y_bytes != y || l->uv_bytes != uv || l->uv_plane_offset != y ||
        l->total_nv12_bytes != y + uv || bytes < y + uv ||
        l->visible_uv_offset != y + (size_t)(cy / 2) * l->pitch + (cx & ~1u) ||
        l->rgb_pixels != pixels || l->rgb_bytes != pixels * sizeof(uint32_t))
        return X4_PIPELINE_INVALID;
    *crop_x = cx; *crop_y = cy;
    return 0;
}
static bool color_fast_geometry(const X4VideoNv12Layout *l,
    unsigned crop_x, unsigned crop_y)
{
    return !(crop_x & 1) && !(crop_y & 1) &&
        ((l->visible_width == 1280 && l->visible_height == 720 &&
          l->coded_width == 1280 && l->coded_height == 720 && l->storage_height == 720) ||
         (l->visible_width == 1920 && l->visible_height == 1080 &&
          l->coded_width == 1920 && l->coded_height == 1088 && l->storage_height == 1088));
}
int x4_video_pipeline_color_path(const X4VideoNv12Layout *l, size_t bytes,
    unsigned *path)
{
    unsigned cx, cy;
    if (!path || validated_color_layout(l,bytes,&cx,&cy)) return X4_PIPELINE_INVALID;
    *path = X4_PIPELINE_CONVERT_SCALAR;
#if X4_PIPELINE_SSE2
    if (color_fast_geometry(l,cx,cy)) *path = X4_PIPELINE_CONVERT_SSE2;
#else
    (void)color_fast_geometry;
#endif
    return 0;
}
static uint32_t color_scalar_pixel(unsigned y, unsigned u, unsigned v,
    const ColorCoefficients *m)
{
    int c = (int)y - m->y_bias, d = (int)u - 128, e = (int)v - 128;
    unsigned r = clamp_channel((m->y*c + m->rv*e + 4096) >> 13);
    unsigned g = clamp_channel((m->y*c + m->gu*d + m->gv*e + 4096) >> 13);
    unsigned b = clamp_channel((m->y*c + m->bu*d + 4096) >> 13);
    return 0xff000000u | b << 16 | g << 8 | r;
}
#if X4_PIPELINE_SSE2
static void color_rgb_eight(__m128i y, __m128i u, __m128i v,
    const ColorCoefficients *m, __m128i *first, __m128i *second)
{
    const __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi32(4096);
    const __m128i rc = _mm_setr_epi16(m->y,m->rv,m->y,m->rv,m->y,m->rv,m->y,m->rv);
    const __m128i bc = _mm_setr_epi16(m->y,m->bu,m->y,m->bu,m->y,m->bu,m->y,m->bu);
    const __m128i gc = _mm_setr_epi16(m->gu,m->gv,m->gu,m->gv,m->gu,m->gv,m->gu,m->gv);
    const __m128i yc = _mm_set1_epi32(m->y), alpha = _mm_set1_epi8((char)-1);
    __m128i rl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,v),rc)),13);
    __m128i rh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,v),rc)),13);
    __m128i bl = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpacklo_epi16(y,u),bc)),13);
    __m128i bh = _mm_srai_epi32(_mm_add_epi32(round,_mm_madd_epi16(_mm_unpackhi_epi16(y,u),bc)),13);
    __m128i gl = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpacklo_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpacklo_epi16(u,v),gc))),13);
    __m128i gh = _mm_srai_epi32(_mm_add_epi32(round,_mm_add_epi32(
        _mm_madd_epi16(_mm_unpackhi_epi16(y,zero),yc),_mm_madd_epi16(_mm_unpackhi_epi16(u,v),gc))),13);
    __m128i r = _mm_packus_epi16(_mm_packs_epi32(rl,rh),zero);
    __m128i g = _mm_packus_epi16(_mm_packs_epi32(gl,gh),zero);
    __m128i b = _mm_packus_epi16(_mm_packs_epi32(bl,bh),zero);
    __m128i rg = _mm_unpacklo_epi8(r,g), ba = _mm_unpacklo_epi8(b,alpha);
    *first = _mm_unpacklo_epi16(rg,ba); *second = _mm_unpackhi_epi16(rg,ba);
}
static void color_row_sse2(uint32_t *dst, const uint8_t *y, const uint8_t *uv,
    unsigned width, bool scale_three_halves, const ColorCoefficients *m)
{
    const __m128i zero = _mm_setzero_si128(), ybias = _mm_set1_epi16(m->y_bias);
    const __m128i uvbias = _mm_set1_epi16(128);
    for (unsigned x = 0; x < width; x += 8) {
        __m128i luma = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(y+x)),zero),ybias);
        __m128i chroma = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(uv+x)),zero),uvbias);
        __m128i u = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(2,2,0,0)),_MM_SHUFFLE(2,2,0,0));
        __m128i v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(chroma,_MM_SHUFFLE(3,3,1,1)),_MM_SHUFFLE(3,3,1,1));
        __m128i first, second; color_rgb_eight(luma,u,v,m,&first,&second);
        if (scale_three_halves) {
            uint32_t *out = dst + x * 3 / 2;
            _mm_storeu_si128((__m128i *)out,_mm_shuffle_epi32(first,_MM_SHUFFLE(2,1,0,0)));
            _mm_storeu_si128((__m128i *)(out+4),_mm_unpackhi_epi64(first,
                _mm_shuffle_epi32(second,_MM_SHUFFLE(0,0,0,0))));
            _mm_storeu_si128((__m128i *)(out+8),_mm_shuffle_epi32(second,_MM_SHUFFLE(3,2,2,1)));
        } else {
            _mm_storeu_si128((__m128i *)(dst+x),first);
            _mm_storeu_si128((__m128i *)(dst+x+4),second);
        }
    }
}
static void color_row_smooth_sse2(uint32_t *dst, const uint8_t *source,
    size_t uv_plane_offset, unsigned pitch, unsigned crop_x, unsigned crop_y,
    unsigned visible_w, unsigned visible_h, unsigned sy,
    const ColorCoefficients *m, unsigned sharpness)
{
    unsigned j_min = crop_y / 2;
    unsigned j_max = crop_y / 2 + (visible_h / 2) - 1;
    unsigned j = (crop_y + sy) / 2;
    unsigned j_other = ((sy & 1) == 0) ? ((j > j_min) ? j - 1 : j) : ((j < j_max) ? j + 1 : j);

    const uint8_t *y_plane = source;
    const uint8_t *uv_j = source + uv_plane_offset + (size_t)j * pitch + (crop_x & ~1u);
    const uint8_t *uv_other = source + uv_plane_offset + (size_t)j_other * pitch + (crop_x & ~1u);

    uint8_t c_vert[1928];
    chroma_vertical_row_sse2(c_vert, uv_j, uv_other, visible_w);
    c_vert[visible_w] = c_vert[visible_w - 2];
    c_vert[visible_w + 1] = c_vert[visible_w - 1];
    c_vert[visible_w + 2] = c_vert[visible_w - 2];
    c_vert[visible_w + 3] = c_vert[visible_w - 1];

    const __m128i ybias = _mm_set1_epi16(m->y_bias), uvbias = _mm_set1_epi16(128);

    for (unsigned x = 0; x < visible_w; x += 8) {
        __m128i luma = sharpen_luma_sse2(y_plane, pitch, crop_x, crop_y, visible_w, visible_h,
            x, sy, sharpness, ybias);

        __m128i cur = _mm_loadu_si128((const __m128i *)(c_vert + x));
        __m128i nxt = _mm_srli_si128(cur, 2);
        __m128i avg = _mm_avg_epu8(cur, nxt);
        __m128i pairs = _mm_unpacklo_epi16(cur, avg);
        __m128i u = _mm_sub_epi16(_mm_and_si128(pairs, _mm_set1_epi16(0x00ff)), uvbias);
        __m128i v = _mm_sub_epi16(_mm_srli_epi16(pairs, 8), uvbias);

        __m128i first, second;
        color_rgb_eight(luma, u, v, m, &first, &second);
        _mm_storeu_si128((__m128i *)(dst + x), first);
        _mm_storeu_si128((__m128i *)(dst + x + 4), second);
    }
}
#endif
static void color_row_smooth_scalar(uint32_t *dst, const uint8_t *source,
    size_t uv_plane_offset, unsigned pitch, unsigned crop_x, unsigned crop_y,
    unsigned visible_w, unsigned visible_h, unsigned sy,
    const ColorCoefficients *m, unsigned sharpness)
{
    unsigned j_min = crop_y / 2;
    unsigned j_max = crop_y / 2 + (visible_h / 2) - 1;
    unsigned j = (crop_y + sy) / 2;
    unsigned j_other = ((sy & 1) == 0) ? ((j > j_min) ? j - 1 : j) : ((j < j_max) ? j + 1 : j);

    const uint8_t *y_plane = source;
    const uint8_t *uv_j = source + uv_plane_offset + (size_t)j * pitch + (crop_x & ~1u);
    const uint8_t *uv_other = source + uv_plane_offset + (size_t)j_other * pitch + (crop_x & ~1u);

    for (unsigned sx = 0; sx < visible_w; ++sx) {
        unsigned y_val = scalar_sharpen_luma(y_plane, pitch, crop_x, crop_y, visible_w, visible_h,
            sx, sy, sharpness);
        unsigned i = sx / 2;
        unsigned i_next = (i < (visible_w / 2) - 1) ? i + 1 : i;

        unsigned u_vi = (3u * uv_j[2 * i] + uv_other[2 * i] + 2u) >> 2;
        unsigned v_vi = (3u * uv_j[2 * i + 1] + uv_other[2 * i + 1] + 2u) >> 2;

        unsigned u_final, v_final;
        if ((sx & 1) == 0) {
            u_final = u_vi;
            v_final = v_vi;
        } else {
            unsigned u_vnext = (3u * uv_j[2 * i_next] + uv_other[2 * i_next] + 2u) >> 2;
            unsigned v_vnext = (3u * uv_j[2 * i_next + 1] + uv_other[2 * i_next + 1] + 2u) >> 2;
            u_final = (u_vi + u_vnext + 1u) >> 1;
            v_final = (v_vi + v_vnext + 1u) >> 1;
        }
        dst[sx] = color_scalar_pixel(y_val, u_final, v_final, m);
    }
}
typedef struct {
    uint32_t rows[3][1280];
    uint32_t blend[1280];
} ConverterScalingBuffer;

static ConverterScalingBuffer s_scale_buf[3];

static void color_convert_rows(const X4VideoNv12Layout *l, const uint8_t *source,
    uint32_t *dst, unsigned width, unsigned height, unsigned pitch,
    unsigned first_row, unsigned end_row, unsigned crop_x, unsigned crop_y,
    const ColorCoefficients *m, const X4VideoPipelineColor *color, unsigned worker_index)
{
    unsigned convert_mode = color ? color->convert_mode : 0;
    unsigned sharpness = color ? color->sharpness : 0;

    if (convert_mode == X4_CONVERT_SMOOTH) {
#if X4_PIPELINE_SSE2
        bool sse2_fast = color_fast_geometry(l, crop_x, crop_y);
#endif
        if (width == l->visible_width && height == l->visible_height) {
            for (unsigned dy = first_row; dy < end_row; ++dy) {
                uint32_t *row = dst + (size_t)dy * pitch;
#if X4_PIPELINE_SSE2
                if (sse2_fast) {
                    color_row_smooth_sse2(row, source, l->uv_plane_offset, l->pitch,
                        crop_x, crop_y, l->visible_width, l->visible_height, dy, m, sharpness);
                    continue;
                }
#endif
                color_row_smooth_scalar(row, source, l->uv_plane_offset, l->pitch,
                    crop_x, crop_y, l->visible_width, l->visible_height, dy, m, sharpness);
            }
            return;
        } else if (l->visible_width == 1280 && l->visible_height == 720 && width == 1920 && height == 1080) {
            ConverterScalingBuffer *sb = &s_scale_buf[worker_index < 3 ? worker_index : 0];
            uint32_t (*rgb_rows)[1280] = sb->rows;
            uint32_t *rgb_blend = sb->blend;
            int cached_sy[3] = {-1, -1, -1};
            unsigned m_start = first_row / 3;
            unsigned m_end = (end_row + 2) / 3;
            for (unsigned group = m_start; group < m_end; ++group) {
                unsigned sy0 = 2 * group;
                unsigned sy1 = 2 * group + 1;
                unsigned sy2 = (2 * group + 2 < 720) ? (2 * group + 2) : 719;

                unsigned slot0 = sy0 % 3;
                if (cached_sy[slot0] != (int)sy0) {
#if X4_PIPELINE_SSE2
                    if (sse2_fast) {
                        color_row_smooth_sse2(rgb_rows[slot0], source, l->uv_plane_offset, l->pitch,
                            crop_x, crop_y, 1280, 720, sy0, m, sharpness);
                    } else
#endif
                    {
                        color_row_smooth_scalar(rgb_rows[slot0], source, l->uv_plane_offset, l->pitch,
                            crop_x, crop_y, 1280, 720, sy0, m, sharpness);
                    }
                    cached_sy[slot0] = (int)sy0;
                }

                unsigned slot1 = sy1 % 3;
                if (cached_sy[slot1] != (int)sy1) {
#if X4_PIPELINE_SSE2
                    if (sse2_fast) {
                        color_row_smooth_sse2(rgb_rows[slot1], source, l->uv_plane_offset, l->pitch,
                            crop_x, crop_y, 1280, 720, sy1, m, sharpness);
                    } else
#endif
                    {
                        color_row_smooth_scalar(rgb_rows[slot1], source, l->uv_plane_offset, l->pitch,
                            crop_x, crop_y, 1280, 720, sy1, m, sharpness);
                    }
                    cached_sy[slot1] = (int)sy1;
                }

                unsigned slot2 = sy2 % 3;
                if (cached_sy[slot2] != (int)sy2) {
#if X4_PIPELINE_SSE2
                    if (sse2_fast) {
                        color_row_smooth_sse2(rgb_rows[slot2], source, l->uv_plane_offset, l->pitch,
                            crop_x, crop_y, 1280, 720, sy2, m, sharpness);
                    } else
#endif
                    {
                        color_row_smooth_scalar(rgb_rows[slot2], source, l->uv_plane_offset, l->pitch,
                            crop_x, crop_y, 1280, 720, sy2, m, sharpness);
                    }
                    cached_sy[slot2] = (int)sy2;
                }

                unsigned dy0 = 3 * group;
                if (dy0 >= first_row && dy0 < end_row) {
                    scale_row_1280_to_1920(dst + (size_t)dy0 * pitch, rgb_rows[slot0]);
                }
                unsigned dy1 = 3 * group + 1;
                if (dy1 >= first_row && dy1 < end_row) {
                    blend_rows_1280(rgb_blend, rgb_rows[slot0], rgb_rows[slot1], 85, 171);
                    scale_row_1280_to_1920(dst + (size_t)dy1 * pitch, rgb_blend);
                }
                unsigned dy2 = 3 * group + 2;
                if (dy2 >= first_row && dy2 < end_row) {
                    blend_rows_1280(rgb_blend, rgb_rows[slot1], rgb_rows[slot2], 171, 85);
                    scale_row_1280_to_1920(dst + (size_t)dy2 * pitch, rgb_blend);
                }
            }
            return;
        }
    }

    for (unsigned dy = first_row; dy < end_row; ++dy) {
        unsigned sy = dy * l->visible_height / height;
        const uint8_t *y = source + l->visible_y_offset + (size_t)sy * l->pitch;
        const uint8_t *uv = source + l->uv_plane_offset +
            (size_t)((crop_y+sy)/2) * l->pitch + (crop_x & ~1u);
        uint32_t *row = dst + (size_t)dy * pitch;
#if X4_PIPELINE_SSE2
        if (color_fast_geometry(l,crop_x,crop_y) &&
            (width == l->visible_width || (l->visible_width == 1280 && width == 1920))) {
            color_row_sse2(row,y,uv,l->visible_width,width != l->visible_width,m);
            continue;
        }
#endif
        for (unsigned dx = 0; dx < width; ++dx) {
            unsigned sx = dx * l->visible_width / width;
            unsigned cx = ((crop_x + sx) & ~1u) - (crop_x & ~1u);
            row[dx] = color_scalar_pixel(y[sx],uv[cx],uv[cx+1],m);
        }
    }
}
int x4_video_pipeline_convert_stripe_color(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes,
    unsigned pitch, unsigned workers, unsigned index, const X4VideoPipelineColor *color)
{
    unsigned cx, cy; X4VideoPipelineStripe stripe;
    if (!color || x4_video_pipeline_stripe(1080,workers,index,&stripe) ||
        validated_color_layout(l,source_bytes,&cx,&cy) ||
        validated_destination(source,source_bytes,dst,dst_bytes,1920,1080,pitch))
        return X4_PIPELINE_INVALID;
    ColorCoefficients m = color_coefficients(color);
    color_convert_rows(l,source,dst,1920,1080,pitch,stripe.first_row,stripe.end_row,cx,cy,&m,color,index);
#if defined(__SSE2__)
    _mm_sfence();
#endif
    return 0;
}
int x4_video_pipeline_convert_reference_color(const X4VideoNv12Layout *l,
    const uint8_t *source, size_t source_bytes, uint32_t *dst, size_t dst_bytes,
    const X4VideoPipelineColor *color)
{
    unsigned cx, cy;
    if (!color || validated_color_layout(l,source_bytes,&cx,&cy) ||
        validated_destination(source,source_bytes,dst,dst_bytes,
            l->visible_width,l->visible_height,l->visible_width)) return X4_PIPELINE_INVALID;
    ColorCoefficients m = color_coefficients(color);
    color_convert_rows(l,source,dst,l->visible_width,l->visible_height,
        l->visible_width,0,l->visible_height,cx,cy,&m,color,0);
    return 0;
}
#if !X4_PUBLIC
__attribute__((target("sse4.1")))
int x4_video_pipeline_gpu_copy_rows(const uint32_t *source,size_t source_bytes,
    unsigned pitch,uint32_t *destination,size_t destination_bytes,
    unsigned first,unsigned rows)
{
    if(!source||((uintptr_t)source&15u)||pitch<1920||pitch>X4_VIDEO_MAX_PITCH||
        (pitch&3u)||!rows||rows>16||first>=1080||rows>1080-first||
        source_bytes<((size_t)(first+rows-1)*pitch+1920)*4||
        validated_destination((const uint8_t *)source,source_bytes,destination,
            destination_bytes,1920,rows,1920))return X4_PIPELINE_INVALID;
    /* MOVNTDQA reads WC_GARLIC in 16-byte units into a cached scratch;
     * all comparison loads then use the scratch, never scalar WC loads. */
    _mm_mfence();
    for(unsigned y=0;y<rows;++y){
        const __m128i *src=(const __m128i *)(source+(size_t)(first+y)*pitch);
        uint32_t *dst=destination+(size_t)y*1920;
        for(unsigned x=0;x<1920;x+=4)
            _mm_storeu_si128((__m128i *)(dst+x),_mm_stream_load_si128(src+x/4));
    }
    _mm_mfence();
    return 0;
}
#endif
int x4_video_pipeline_gpu_reference_rows(const X4VideoNv12Layout *l,
    const uint8_t *source,size_t source_bytes,uint32_t *dst,size_t dst_bytes,
    unsigned first,unsigned rows,const X4VideoPipelineColor *color)
{
    unsigned cx=0,cy=0;
    if(!color||!rows||rows>64||first>1080||rows>1080-first||
        validated_color_layout(l,source_bytes,&cx,&cy)||cx||cy||
        l->visible_width!=1920||l->visible_height!=1080||
        l->coded_width!=1920||l->coded_height!=1088||l->storage_height!=1088||
        validated_destination(source,source_bytes,dst,dst_bytes,1920,rows,1920))return X4_PIPELINE_INVALID;
    ColorCoefficients m=color_coefficients(color);
    for(unsigned y=first;y<first+rows;++y){
        uint32_t *row=dst+(size_t)(y-first)*1920;
        if(color->convert_mode==X4_CONVERT_SMOOTH){
#if X4_PIPELINE_SSE2
            color_row_smooth_sse2(row,source,l->uv_plane_offset,l->pitch,0,0,1920,1080,y,&m,color->sharpness);
#else
            color_row_smooth_scalar(row,source,l->uv_plane_offset,l->pitch,0,0,1920,1080,y,&m,color->sharpness);
#endif
        }else{
            const uint8_t *yp=source+(size_t)y*l->pitch;
            const uint8_t *uv=source+l->uv_plane_offset+(size_t)(y/2)*l->pitch;
#if X4_PIPELINE_SSE2
            color_row_sse2(row,yp,uv,1920,false,&m);
#else
            for(unsigned x=0;x<1920;++x)row[x]=color_scalar_pixel(yp[x],uv[x&~1u],uv[(x&~1u)+1],&m);
#endif
        }
    }
#if defined(__SSE2__)
    _mm_sfence();
#endif
    return 0;
}
int x4_video_pipeline_gpu_reference_720_rows(const X4VideoNv12Layout *l,
    const uint8_t *source,size_t source_bytes,uint32_t *dst,size_t dst_bytes,
    unsigned first,unsigned rows,const X4VideoPipelineColor *color)
{
    unsigned cx=0,cy=0;
    if(!color||!rows||rows>16||first>=1080||rows>1080-first||
        validated_color_layout(l,source_bytes,&cx,&cy)||cx||cy||
        l->visible_width!=1280||l->visible_height!=720||
        l->coded_width!=1280||l->coded_height!=720||l->storage_height!=720||
        validated_destination(source,source_bytes,dst,dst_bytes,1920,1080,1920))return X4_PIPELINE_INVALID;
    ColorCoefficients m=color_coefficients(color);
    /* The production row function writes absolute destination row indices.
     * Its own smooth scratch slot 2 is exclusive here: streaming converters
     * use 0/1, and the three-worker probe is a different compiled program.
     * Only these <=16 rows are converted; no frame-sized copy or staging. */
    color_convert_rows(l,source,dst,1920,1080,1920,first,first+rows,0,0,&m,color,2);
#if defined(__SSE2__)
    _mm_sfence();
#endif
    return 0;
}
