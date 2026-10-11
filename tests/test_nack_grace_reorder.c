/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual portable reorder plus pure grace policy. Synthetic worker ownership
 * below supplies clocks/admission; no video worker, AUTH, decoder or network
 * execution is claimed. These tests validate the shared production helpers. */
#include "live_media.h"
#include "nack_grace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned cases;
static unsigned long long checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
#define CASE(x) do { x(); ++cases; printf("PASS %s\n",#x); } while(0)
typedef struct { X4LiveReorder reorder; X4NackGraceState grace; } Fixture;
static const uint8_t payload[4]={1,2,3,4};
static void init(Fixture *f,bool enabled,uint16_t first)
{
    memset(f,0,sizeof(*f));
    CHECK(x4_live_reorder_init(&f->reorder,128,32,25000)==0);
    x4_nack_grace_init(&f->grace,9,enabled);
    (void)x4_nack_grace_bind_source(&f->grace,7,1);
    CHECK(x4_live_reorder_insert(&f->reorder,payload,4,first,1000)==X4_LIVE_REORDER_STORED);
    size_t size; uint32_t lost;
    CHECK(x4_live_reorder_next(&f->reorder,1000,&size,&lost)!=NULL && size==4 && lost==0);
}
static uint32_t span(const X4LiveReorder *q)
{
    uint32_t result=0;
    for(unsigned i=0;i<q->window;++i) if(q->slots[i].used) {
        uint32_t d=(uint16_t)(q->slots[i].sequence-q->expected);
        if(d>result) result=d;
    }
    return result;
}
static void offer(Fixture *f,unsigned ordinal,uint64_t first,uint64_t mask,uint64_t detection,uint64_t now)
{
    X4NackGraceRequest request={9,(UINT64_C(5)<<32)|ordinal,first,mask,detection,7,1};
    X4NackGraceEffects effects;
    CHECK(x4_nack_grace_offer(&f->grace,&request,now,&effects)==X4_GRACE_OFFER_ADMITTED);
}
static const uint8_t *next(Fixture *f,uint64_t real_now,uint64_t ordinary_now,
    size_t *size,uint32_t *lost,unsigned *reason)
{
    X4NackGraceDecision decision=x4_nack_grace_query(&f->grace,f->reorder.expected,
        f->reorder.buffered,span(&f->reorder),real_now);
    unsigned at=f->reorder.expected&(f->reorder.window-1);
    bool hole=!f->reorder.slots[at].used || f->reorder.slots[at].sequence!=f->reorder.expected;
    if(hole&&decision.reason==X4_GRACE_HOLD) { *size=0; *lost=0; *reason=0; return NULL; }
    enum X4NackGraceRelease release=decision.reason==X4_GRACE_DEADLINE?X4_GRACE_RELEASE_DEADLINE:
        decision.reason==X4_GRACE_CAPACITY?X4_GRACE_RELEASE_CAPACITY:X4_GRACE_RELEASE_NONE;
    bool force=hole&&x4_nack_grace_release_forces_gap(release);
    uint32_t limit=x4_nack_grace_advance_limit(&f->grace,f->reorder.expected,f->reorder.window);
    return x4_live_reorder_next_limited(&f->reorder,ordinary_now,size,lost,reason,force,limit);
}
static int stored(Fixture *f,uint16_t sequence,uint64_t now)
{
    int rc=x4_live_reorder_insert(&f->reorder,payload,4,sequence,now);
    if(rc==X4_LIVE_REORDER_STORED) (void)x4_nack_grace_accepted_wire(&f->grace,7,1,sequence,now);
    return rc;
}
static void depth32_is_bypassed_only_for_requested_expected(void)
{
    Fixture off,on; init(&off,false,100); init(&on,true,100); offer(&on,1,101,1,2000,2000);
    for(uint16_t seq=102;seq<134;++seq) {
        CHECK(stored(&off,seq,3000)==0 && stored(&on,seq,3000)==0);
    }
    size_t size;uint32_t lost;unsigned reason;
    CHECK(x4_live_reorder_next_reason(&off.reorder,0,&size,&lost,&reason,false)!=NULL);
    CHECK(lost==1 && reason==1 && off.reorder.expected==103);
    CHECK(next(&on,4000,0,&size,&lost,&reason)==NULL);
    CHECK(on.reorder.expected==101 && on.reorder.buffered==32);
    CHECK(next(&on,46999,0,&size,&lost,&reason)==NULL);
    CHECK(next(&on,47000,0,&size,&lost,&reason)!=NULL && lost==1 && (reason&8));
    CHECK(on.grace.counters.deadline_ranges==1);
    x4_live_reorder_free(&off.reorder);x4_live_reorder_free(&on.reorder);
}
static void twenty_five_ms_time_is_replaced_by_absolute_deadline(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);CHECK(stored(&f,102,2000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,27000,27000,&size,&lost,&reason)==NULL);
    CHECK(f.reorder.expected==101 && f.reorder.buffered==1);
    CHECK(next(&f,47000,47000,&size,&lost,&reason)!=NULL && lost==1 && reason==(2u|8u));
    CHECK(f.grace.ranges[0].deadline_us==47000);
    x4_live_reorder_free(&f.reorder);
}
static void expiry_without_new_packet_forces_with_zero_ordinary_clock(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);CHECK(stored(&f,102,46000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,46999,0,&size,&lost,&reason)==NULL);
    CHECK(next(&f,47000,0,&size,&lost,&reason)!=NULL && lost==1 && reason==8);
    CHECK(f.grace.active_count==0 && f.reorder.expected==103);
    x4_live_reorder_free(&f.reorder);
}
static void timely_auth_without_worker_store_never_completes_grace(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);CHECK(stored(&f,102,3000)==0);
    // Synthetic AUTH observation at33000 deliberately does not invoke stored().
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,33000,33000,&size,&lost,&reason)==NULL);
    CHECK(f.grace.ranges[0].pending_mask==1 && f.grace.ranges[0].accepted_mask==0);
    CHECK(stored(&f,101,34000)==0);
    CHECK(f.grace.counters.completed_ranges==1 && f.grace.counters.accepted_positions==1);
    CHECK(next(&f,34000,34000,&size,&lost,&reason)!=NULL && lost==0 && reason==0);
    CHECK(f.reorder.expected==102);
    x4_live_reorder_free(&f.reorder);
}
static void stored_after_deadline_is_not_timely_grace_acceptance(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);CHECK(stored(&f,102,3000)==0);
    CHECK(stored(&f,101,47000)==0);
    CHECK(f.grace.counters.accepted_positions==0 && f.grace.counters.deadline_ranges==1);
    CHECK(f.grace.ranges[0].accepted_mask==0 && f.grace.active_count==0);
    size_t size;uint32_t lost;
    CHECK(x4_live_reorder_next(&f.reorder,47000,&size,&lost)!=NULL && lost==0);
    x4_live_reorder_free(&f.reorder);
}
static void expired_expected_abandons_before_late_worker_insert(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);CHECK(stored(&f,102,3000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,47000,0,&size,&lost,&reason)!=NULL && lost==1);
    CHECK(stored(&f,101,48000)==X4_LIVE_REORDER_LATE);
    CHECK(f.grace.counters.accepted_positions==0);
    x4_live_reorder_free(&f.reorder);
}
static void expiry_fallback_advances_without_emit_before_delayed_return(void)
{
    /* The owner resumes after an uninterruptible native call with its work
     * budget exhausted. Use actual production gap advancement rather than
     * next(), so no packet is emitted/consumed to enforce the deadline. This
     * fixture executes portable helpers, not the native worker or Decode. */
    const uint16_t initial[]={100,65534};
    const uint64_t resumed[]={47000,120000};
    for(unsigned seed=0;seed<2;++seed) for(unsigned clock=0;clock<2;++clock) {
        Fixture f;init(&f,true,initial[seed]);
        uint16_t missing=(uint16_t)(initial[seed]+1),future=(uint16_t)(initial[seed]+2);
        offer(&f,1,(uint64_t)initial[seed]+1,1,2000,2000);
        CHECK(stored(&f,future,3000)==X4_LIVE_REORDER_STORED);
        unsigned at=future&127;
        X4LiveReorderSlot before=f.reorder.slots[at];
        uint8_t bytes[sizeof(payload)];
        memcpy(bytes,f.reorder.data+(size_t)at*X4_LIVE_RTP_MAX,sizeof(bytes));
        X4NackGraceDecision d=x4_nack_grace_query(&f.grace,f.reorder.expected,
            f.reorder.buffered,span(&f.reorder),resumed[clock]);
        CHECK(d.reason==X4_GRACE_DEADLINE && d.effects.deadline==1);
        CHECK(f.grace.ranges[0].deadline_us==47000 && f.grace.active_count==0);
        CHECK(x4_live_reorder_abandon_gap(&f.reorder)==1);
        CHECK(f.reorder.expected==future && f.reorder.buffered==1);
        CHECK(f.reorder.slots[at].used && f.reorder.slots[at].sequence==before.sequence);
        CHECK(f.reorder.slots[at].size==before.size && f.reorder.slots[at].arrival==before.arrival);
        CHECK(memcmp(f.reorder.data+(size_t)at*X4_LIVE_RTP_MAX,bytes,sizeof(bytes))==0);
        CHECK(stored(&f,missing,resumed[clock])==X4_LIVE_REORDER_LATE);
        CHECK(f.grace.counters.accepted_positions==0 && f.grace.counters.completed_ranges==0);
        CHECK(f.grace.counters.deadline_ranges==1 && f.grace.ranges[0].accepted_mask==0);
        /* Expected is already available: a repeated force operation cannot
         * consume it, advance again or count the same hole twice. */
        CHECK(x4_live_reorder_abandon_gap(&f.reorder)==0 && f.reorder.buffered==1);
        size_t size;uint32_t lost;unsigned reason;
        const uint8_t *p=x4_live_reorder_next_reason(&f.reorder,0,&size,&lost,&reason,false);
        CHECK(p!=NULL && size==sizeof(payload) && lost==0 && reason==0);
        CHECK(memcmp(p,bytes,sizeof(bytes))==0);
        CHECK(f.reorder.expected==(uint16_t)(future+1) && f.reorder.buffered==0);
        CHECK(f.reorder.window==128 && f.reorder.depth==32 && f.reorder.max_wait==25000);
        x4_live_reorder_free(&f.reorder);
    }
}
static void capacity128_releases_before_alias_and_never_overwrites(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);
    const uint8_t old[]={9,8},other[]={7,6};
    CHECK(x4_live_reorder_insert(&f.reorder,old,2,102,3000)==0);
    unsigned at=102&127;
    CHECK(x4_live_reorder_insert(&f.reorder,other,2,102,3500)==X4_LIVE_REORDER_LATE);
    CHECK(memcmp(f.reorder.data+(size_t)at*X4_LIVE_RTP_MAX,old,2)==0);
    X4NackGraceDecision d=x4_nack_grace_query(&f.grace,101,f.reorder.buffered,128,4000);
    CHECK(d.reason==X4_GRACE_CAPACITY && f.grace.active_count==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(x4_live_reorder_next_reason(&f.reorder,0,&size,&lost,&reason,true)!=NULL && lost==1 && reason==8);
    CHECK(f.reorder.buffered==0);
    // Subsequent in-window sequence stores in a cleared slot. Beyond-window
    // insert resets the reorder and returns JUMP, never overwrites a live alias.
    CHECK(x4_live_reorder_insert(&f.reorder,other,2,229,4000)==0);
    CHECK(x4_live_reorder_insert(&f.reorder,payload,4,357,4001)==X4_LIVE_REORDER_JUMP);
    CHECK(f.reorder.buffered==1 && f.reorder.expected==357 && f.reorder.window==128);
    x4_live_reorder_free(&f.reorder);
}
static void wrap_return_reconstructs_in_order_before_deadline(void)
{
    Fixture f;init(&f,true,65534);offer(&f,1,65535,7,2000,2000);
    CHECK(stored(&f,2,3000)==0 && stored(&f,0,34000)==0 && stored(&f,1,34001)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,34001,0,&size,&lost,&reason)==NULL);
    CHECK(stored(&f,65535,34002)==0 && f.grace.counters.accepted_positions==3);
    for(uint16_t expected=65535,n=0;n<4;++n,++expected) {
        CHECK(f.reorder.expected==expected);
        CHECK(next(&f,34002,0,&size,&lost,&reason)!=NULL && lost==0 && reason==0);
    }
    CHECK(f.reorder.expected==3 && f.reorder.buffered==0);
    x4_live_reorder_free(&f.reorder);
}
static void multiple_ranges_keep_original_deadlines_after_expected_advances(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);offer(&f,2,103,1,5000,5000);
    CHECK(stored(&f,102,6000)==0 && stored(&f,104,6001)==0);
    CHECK(stored(&f,101,34000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,34000,0,&size,&lost,&reason)!=NULL && lost==0);
    CHECK(next(&f,34000,0,&size,&lost,&reason)!=NULL && lost==0);
    CHECK(f.reorder.expected==103);
    CHECK(next(&f,49999,0,&size,&lost,&reason)==NULL);
    CHECK(f.grace.ranges[1].deadline_us==50000);
    CHECK(next(&f,50000,0,&size,&lost,&reason)!=NULL && lost==1 && reason==8);
    CHECK(f.grace.counters.completed_ranges==1 && f.grace.counters.deadline_ranges==1);
    x4_live_reorder_free(&f.reorder);
}
static void ordinary_unrequested_holes_keep_baseline_policy(void)
{
    Fixture f;init(&f,true,100);CHECK(stored(&f,102,2000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,26999,26999,&size,&lost,&reason)==NULL);
    CHECK(next(&f,27000,27000,&size,&lost,&reason)!=NULL && lost==1 && reason==2);
    x4_live_reorder_free(&f.reorder);
}
static void deadline_abandon_before_insert_preserves_buffered_payload(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,7,2000,2000);CHECK(stored(&f,104,3000)==0);
    X4NackGraceDecision d=x4_nack_grace_query(&f.grace,101,1,3,47000);
    CHECK(d.reason==X4_GRACE_DEADLINE);
    // Production helper advances only expected, without invoking the video
    // decoder or consuming the buffered packet. The worker records loss/reset.
    CHECK(x4_live_reorder_abandon_gap(&f.reorder)==3);
    CHECK(f.reorder.expected==104 && f.reorder.buffered==1);
    CHECK(f.reorder.slots[104&127].used);
    CHECK(memcmp(f.reorder.data+(size_t)(104&127)*X4_LIVE_RTP_MAX,payload,4)==0);
    CHECK(stored(&f,101,48000)==X4_LIVE_REORDER_LATE);
    size_t size;uint32_t lost;
    CHECK(x4_live_reorder_next(&f.reorder,48000,&size,&lost)!=NULL && lost==0 && size==4);
    CHECK(f.reorder.expected==105 && f.reorder.buffered==0);
    CHECK(x4_live_reorder_abandon_gap(&f.reorder)==0);
    x4_live_reorder_free(&f.reorder);
}
static void explicit_abandon_never_skips_a_present_expected_packet(void)
{
    Fixture f;init(&f,true,100);
    CHECK(stored(&f,101,3000)==0 && stored(&f,103,3001)==0);
    CHECK(x4_live_reorder_abandon_gap(&f.reorder)==0);
    CHECK(f.reorder.expected==101 && f.reorder.buffered==2);
    size_t size;uint32_t lost;
    CHECK(x4_live_reorder_next(&f.reorder,0,&size,&lost)!=NULL && lost==0);
    CHECK(f.reorder.expected==102 && f.reorder.buffered==1);
    x4_live_reorder_free(&f.reorder);
}
static void only_deadline_and_capacity_force_abandonment(void)
{
    for(unsigned reason=0;reason<=X4_GRACE_RELEASE_EXPLICIT;++reason)
        CHECK(x4_nack_grace_release_forces_gap((enum X4NackGraceRelease)reason)==
            (reason==X4_GRACE_RELEASE_DEADLINE||reason==X4_GRACE_RELEASE_CAPACITY));
    const enum X4NackGraceRelease cancel[]={X4_GRACE_RELEASE_TRANSPORT,
        X4_GRACE_RELEASE_SOURCE,X4_GRACE_RELEASE_CLOSE,X4_GRACE_RELEASE_EXPLICIT};
    for(unsigned i=0;i<sizeof(cancel)/sizeof(cancel[0]);++i) {
        Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);
        CHECK(stored(&f,102,3000)==0);
        X4NackGraceEffects effects=x4_nack_grace_cancel(&f.grace,
            (UINT64_C(5)<<32)|1,cancel[i]);
        CHECK(effects.invalidated==1 && f.grace.active_count==0);
        CHECK(!x4_nack_grace_release_forces_gap(f.grace.ranges[0].release_reason));
        CHECK(x4_nack_grace_advance_limit(&f.grace,101,128)==128);
        size_t size;uint32_t lost;unsigned reason;
        CHECK(next(&f,4000,4000,&size,&lost,&reason)==NULL && lost==0);
        CHECK(f.reorder.expected==101); /* Baseline32/25 is restored, not forced. */
        CHECK(stored(&f,101,5000)==0);
        CHECK(next(&f,5000,5000,&size,&lost,&reason)!=NULL && lost==0 && reason==0);
        x4_live_reorder_free(&f.reorder);
    }
}
static void first_deadline_preserves_other_pending_range_with_wrap(void)
{
    const uint16_t initial[]={100,65534};
    for(unsigned seed=0;seed<2;++seed) {
        Fixture f;init(&f,true,initial[seed]);
        uint64_t first=(uint64_t)initial[seed]+1,second=(uint64_t)initial[seed]+3;
        offer(&f,1,first,1,2000,2000);offer(&f,2,second,1,5000,5000);
        CHECK(stored(&f,(uint16_t)(second+1),6000)==0); /* Only104, or wrapped2. */
        size_t size;uint32_t lost;unsigned reason;
        CHECK(next(&f,47000,0,&size,&lost,&reason)==NULL);
        CHECK(lost==2 && size==0 && reason==(8u|16u));
        CHECK(f.reorder.expected==(uint16_t)second && f.reorder.buffered==1);
        CHECK(f.grace.ranges[1].active && f.grace.ranges[1].pending_mask==1);
        CHECK(f.grace.ranges[1].deadline_us==50000 && f.grace.counters.passed_positions==0);
        CHECK(next(&f,49999,0,&size,&lost,&reason)==NULL && lost==0);
        CHECK(stored(&f,(uint16_t)second,49999)==0);
        CHECK(f.grace.counters.completed_ranges==1 && f.grace.counters.deadline_ranges==1);
        CHECK(next(&f,49999,0,&size,&lost,&reason)!=NULL && lost==0);
        CHECK(next(&f,49999,0,&size,&lost,&reason)!=NULL && lost==0);
        x4_live_reorder_free(&f.reorder);
    }
}
static void preinsert_abandon_stops_at_other_request_and_preserves_payload(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,1,2000,2000);offer(&f,2,103,1,5000,5000);
    CHECK(stored(&f,104,6000)==0);
    X4NackGraceDecision d=x4_nack_grace_query(&f.grace,101,1,3,47000);
    CHECK(d.reason==X4_GRACE_DEADLINE && d.effects.deadline==1);
    CHECK(x4_nack_grace_advance_limit(&f.grace,101,128)==2);
    CHECK(x4_live_reorder_abandon_gap_limited(&f.reorder,2)==2);
    CHECK(f.reorder.expected==103 && f.reorder.buffered==1);
    CHECK(f.reorder.slots[104&127].used);
    CHECK(memcmp(f.reorder.data+(104&127)*X4_LIVE_RTP_MAX,payload,sizeof(payload))==0);
    CHECK(stored(&f,101,48000)==X4_LIVE_REORDER_LATE);
    CHECK(x4_nack_grace_advance_limit(&f.grace,103,128)==0);
    CHECK(x4_live_reorder_abandon_gap_limited(&f.reorder,0)==0);
    CHECK(stored(&f,103,49000)==0 && f.grace.ranges[1].accepted_mask==1);
    x4_live_reorder_free(&f.reorder);
}
static void ordinary_ready_gap_stops_at_protected_position(void)
{
    for(unsigned depth=0;depth<2;++depth) {
        Fixture f;init(&f,true,100);offer(&f,1,103,1,5000,5000);
        unsigned count=depth?32:1;
        for(unsigned i=0;i<count;++i) CHECK(stored(&f,(uint16_t)(104+i),6000)==0);
        size_t size;uint32_t lost;unsigned reason;
        CHECK(next(&f,depth?7000:31000,depth?0:31000,&size,&lost,&reason)==NULL);
        CHECK(lost==2 && reason==((depth?1u:2u)|16u));
        CHECK(f.reorder.expected==103 && f.reorder.buffered==count);
        CHECK(f.grace.ranges[0].active && f.grace.ranges[0].deadline_us==50000);
        CHECK(next(&f,49999,49999,&size,&lost,&reason)==NULL && lost==0);
        CHECK(next(&f,50000,50000,&size,&lost,&reason)!=NULL && lost==1 && (reason&8u));
        x4_live_reorder_free(&f.reorder);
    }
}
static void overlapping_request_does_not_renew_but_keeps_new_position(void)
{
    Fixture f;init(&f,true,100);offer(&f,1,101,5,2000,2000);offer(&f,2,103,3,5000,5000);
    CHECK(f.grace.ranges[1].granted_mask==2 && f.grace.counters.overlap_positions==1);
    CHECK(stored(&f,105,6000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,47000,0,&size,&lost,&reason)==NULL && lost==3 && reason==(8u|16u));
    CHECK(f.reorder.expected==104 && f.grace.ranges[1].pending_mask==2);
    CHECK(f.grace.ranges[0].deadline_us==47000 && f.grace.ranges[1].deadline_us==50000);
    CHECK(stored(&f,104,49000)==0 && f.grace.counters.completed_ranges==1);
    CHECK(next(&f,49000,0,&size,&lost,&reason)!=NULL && lost==0);
    x4_live_reorder_free(&f.reorder);
}
static void limited_helpers_are_inert_without_grace(void)
{
    Fixture f;init(&f,false,100);
    CHECK(x4_nack_grace_advance_limit(&f.grace,101,128)==128);
    CHECK(stored(&f,104,6000)==0);
    size_t size;uint32_t lost;unsigned reason;
    CHECK(next(&f,31000,31000,&size,&lost,&reason)!=NULL && lost==3 && reason==2);
    CHECK(f.reorder.expected==105 && f.reorder.buffered==0);
    x4_live_reorder_free(&f.reorder);
}
int main(void)
{
    CASE(depth32_is_bypassed_only_for_requested_expected);
    CASE(twenty_five_ms_time_is_replaced_by_absolute_deadline);
    CASE(expiry_without_new_packet_forces_with_zero_ordinary_clock);
    CASE(timely_auth_without_worker_store_never_completes_grace);
    CASE(stored_after_deadline_is_not_timely_grace_acceptance);
    CASE(expired_expected_abandons_before_late_worker_insert);
    CASE(expiry_fallback_advances_without_emit_before_delayed_return);
    CASE(capacity128_releases_before_alias_and_never_overwrites);
    CASE(wrap_return_reconstructs_in_order_before_deadline);
    CASE(multiple_ranges_keep_original_deadlines_after_expected_advances);
    CASE(ordinary_unrequested_holes_keep_baseline_policy);
    CASE(deadline_abandon_before_insert_preserves_buffered_payload);
    CASE(explicit_abandon_never_skips_a_present_expected_packet);
    CASE(only_deadline_and_capacity_force_abandonment);
    CASE(first_deadline_preserves_other_pending_range_with_wrap);
    CASE(preinsert_abandon_stops_at_other_request_and_preserves_payload);
    CASE(ordinary_ready_gap_stops_at_protected_position);
    CASE(overlapping_request_does_not_renew_but_keeps_new_position);
    CASE(limited_helpers_are_inert_without_grace);
    printf("PASS actual reorder + grace: %u cases, %llu checks\n",cases,checks);
    return 0;
}
