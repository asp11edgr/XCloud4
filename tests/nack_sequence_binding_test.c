/* SPDX-License-Identifier: GPL-3.0-only */
/* Pure actual binding helper tests. No RTP parsing, crypto, worker or decoder. */
#include "nack_sequence_binding.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
static unsigned checks,cases;
static const uint64_t source=(UINT64_C(1)<<63)|(UINT64_C(102)<<32)|7;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL binding:%d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CASE(x) do{x();++cases;puts("PASS " #x);}while(0)
static X4NackSequenceTag initial(void)
{return (X4NackSequenceTag){.anchor_extended=65534,.anchor_us=1000,.transport_epoch=5,
    .sequence_epoch=1,.anchor_rtp_timestamp=90000,.reason=X4_NACK_SEQUENCE_INITIAL};}
static X4NackSequenceTag rebase(void)
{return (X4NackSequenceTag){.anchor_extended=6,.previous_high_extended=65538,.anchor_us=2000,
    .transport_epoch=5,.sequence_epoch=2,.previous_epoch=1,.anchor_rtp_timestamp=70000,.reason=X4_NACK_SEQUENCE_OMISSION};}
static void bound(X4NackSequenceBinding *s)
{
    X4NackSequenceTag t=initial();uint64_t ext=UINT64_MAX;
    x4_nack_binding_init(s,5,source);
    CHECK(x4_nack_binding_announce(s,&t,source,1100)==X4_BINDING_CANDIDATE);
    CHECK(!s->bound&&s->candidate_valid);
    CHECK(x4_nack_binding_stored(s,&t,source,65534,65534,90000,1000,1200,&ext)==X4_BINDING_COMMITTED);
    CHECK(s->bound&&s->canonical_offset==0&&ext==65534);
    CHECK(x4_nack_binding_map(s,&t,source,65536,0,90001,1300,1400,&ext)&&ext==65536);
}
static void announcement_is_not_STORED_and_both_orders_commit(void)
{
    for(unsigned proof_first=0;proof_first<2;++proof_first){
        X4NackSequenceBinding s;X4NackSequenceTag t=initial();uint64_t ext=UINT64_MAX;
        x4_nack_binding_init(&s,5,source);
        CHECK(!x4_nack_binding_offer_map(&s,source,5,1,65535,1,&ext));
        if(proof_first){
            CHECK(x4_nack_binding_stored(&s,&t,source,65534,65534,90000,1000,1100,&ext)==X4_BINDING_PROOF);
            CHECK(!s.bound&&s.proof_valid&&!s.candidate_valid);
            CHECK(x4_nack_binding_announce(&s,&t,source,1200)==X4_BINDING_COMMITTED);
        }else{
            CHECK(x4_nack_binding_announce(&s,&t,source,1100)==X4_BINDING_CANDIDATE);
            CHECK(!s.bound&&!s.proof_valid&&s.candidate_valid);
            CHECK(!x4_nack_binding_offer_map(&s,source,5,1,65535,1,&ext));
            CHECK(x4_nack_binding_stored(&s,&t,source,65534,65534,90000,1000,1200,&ext)==X4_BINDING_COMMITTED);
        }
        CHECK(s.bound&&s.committed.sequence_epoch==1&&s.counters.commits==1);
    }
}
static void wrap_65536_and_old_packet_tickets_never_alias(void)
{
    X4NackSequenceBinding s;bound(&s);X4NackSequenceTag old=initial(),t=rebase();uint64_t ext=0;
    CHECK(x4_nack_binding_announce(&s,&t,source,2100)==X4_BINDING_CANDIDATE);
    CHECK(s.committed.sequence_epoch==1&&x4_nack_binding_waiting(&s,source,5,2));
    CHECK(!x4_nack_binding_map(&s,&old,source,65537,1,90002,2100,2200,&ext));
    CHECK(x4_nack_binding_stored(&s,&t,source,7,7,70000,2100,2200,&ext)==X4_BINDING_UNPROVEN);
    CHECK(x4_nack_binding_stored(&s,&t,source,6,6,70001,2100,2200,&ext)==X4_BINDING_UNPROVEN);
    CHECK(!x4_nack_binding_offer_map(&s,source,5,2,7,1,&ext));
    CHECK(x4_nack_binding_stored(&s,&t,source,6,6,70000,2000,2300,&ext)==X4_BINDING_COMMITTED);
    CHECK(ext==65542&&s.canonical_offset==65536&&s.committed.sequence_epoch==2);
    CHECK(x4_nack_binding_offer_map(&s,source,5,2,7,3,&ext)&&ext==65543);
    CHECK(x4_nack_binding_map(&s,&t,source,7,7,70000,2400,2500,&ext)&&ext==65543);
    CHECK(!x4_nack_binding_map(&s,&old,source,65543,7,70000,2400,2500,&ext));
    CHECK(!x4_nack_binding_offer_map(&s,source,5,1,65543,1,&ext));
    CHECK(x4_nack_binding_announce(&s,&old,source,2600)==X4_BINDING_STALE);
    CHECK(s.counters.commits==2&&s.canonical_offset==65536);
}
static void duplicates_do_not_rebind_or_refresh_anchors(void)
{
    X4NackSequenceBinding s;bound(&s);X4NackSequenceTag t=rebase();uint64_t ext=0;
    CHECK(x4_nack_binding_announce(&s,&t,source,2100)==X4_BINDING_CANDIDATE);
    CHECK(x4_nack_binding_announce(&s,&t,source,2200)==X4_BINDING_DUPLICATE);
    X4NackSequenceTag changed=t;changed.anchor_us=2001;
    CHECK(x4_nack_binding_announce(&s,&changed,source,2200)==X4_BINDING_INVALID);
    CHECK(s.candidate.anchor_us==2000&&s.counters.commits==1);
    CHECK(x4_nack_binding_stored(&s,&t,source,6,6,70000,2000,2300,&ext)==X4_BINDING_COMMITTED);
    CHECK(x4_nack_binding_announce(&s,&t,source,2400)==X4_BINDING_DUPLICATE);
    CHECK(x4_nack_binding_announce(&s,&changed,source,2400)==X4_BINDING_STALE);
    CHECK(s.committed.anchor_us==2000&&s.counters.commits==2);
}
static void owner_source_payload_and_clock_are_not_request_permissions(void)
{
    for(unsigned variant=0;variant<8;++variant){
        X4NackSequenceBinding s;bound(&s);X4NackSequenceTag t=rebase();uint64_t incoming=source,now=2100;
        if(variant==0)t.transport_epoch=6;
        if(variant==1)incoming^=1;
        if(variant==2)incoming=(UINT64_C(1)<<63)|(UINT64_C(103)<<32)|7;
        if(variant==3)t.anchor_extended=65536;
        if(variant==4)t.anchor_us=0;
        if(variant==5)now=1999;
        if(variant==6)t.reason=X4_NACK_SEQUENCE_SOURCE_CHANGE;
        if(variant==7)t.sequence_epoch=3;
        CHECK(x4_nack_binding_announce(&s,&t,incoming,now)==X4_BINDING_INVALID);
        CHECK(s.committed.sequence_epoch==1&&!s.candidate_valid&&s.counters.commits==1);
    }
}
static void ambiguous_backward_and_consumed_high_fail_closed(void)
{
    for(unsigned variant=0;variant<4;++variant){
        X4NackSequenceBinding s;bound(&s);X4NackSequenceTag t=rebase();
        if(variant==0)t.anchor_extended=(uint16_t)(65538+32768);
        if(variant==1)t.anchor_extended=1; /* Backward from wire2. */
        if(variant==2)t.previous_high_extended=65535; /* Consumed65536. */
        if(variant==3)t.previous_high_extended=65536+32768;
        CHECK(x4_nack_binding_announce(&s,&t,source,2100)==X4_BINDING_INVALID);
        CHECK(s.committed.sequence_epoch==1&&!s.candidate_valid);
    }
}
static void mapped_range_bounds_and_actual_packet_proof(void)
{
    X4NackSequenceBinding s;bound(&s);X4NackSequenceTag t=initial();uint64_t ext=123;
    CHECK(!x4_nack_binding_map(&s,&t,source,65535,65535,90000,0,2000,&ext));
    CHECK(!x4_nack_binding_map(&s,&t,source,65535,65535,90000,999,2000,&ext));
    CHECK(!x4_nack_binding_map(&s,&t,source,65535,65535,90000,2001,2000,&ext));
    CHECK(!x4_nack_binding_map(&s,&t,source,65535,0,90000,1900,2000,&ext));
    CHECK(!x4_nack_binding_offer_map(&s,source,5,1,65535,0,&ext));
    CHECK(!x4_nack_binding_offer_map(&s,source,5,1,UINT64_MAX,3,&ext));
    t=rebase();CHECK(x4_nack_binding_announce(&s,&t,source,2100)==X4_BINDING_CANDIDATE);
    CHECK(x4_nack_binding_stored(&s,&t,source,6,6,70000,1999,2300,&ext)==X4_BINDING_UNPROVEN);
    CHECK(x4_nack_binding_stored(&s,&t,source,6,6,70000,2000,2300,&ext)==X4_BINDING_COMMITTED);
    CHECK(!x4_nack_binding_offer_map(&s,source,5,2,UINT64_MAX-65535,1,&ext));
    CHECK(!x4_nack_binding_offer_map(&s,source,6,2,7,1,&ext));
    CHECK(!x4_nack_binding_offer_map(&s,source,5,3,7,1,&ext));
    CHECK(s.counters.commits==2);
}
int main(void)
{
    CASE(announcement_is_not_STORED_and_both_orders_commit);
    CASE(wrap_65536_and_old_packet_tickets_never_alias);
    CASE(duplicates_do_not_rebind_or_refresh_anchors);
    CASE(owner_source_payload_and_clock_are_not_request_permissions);
    CASE(ambiguous_backward_and_consumed_high_fail_closed);
    CASE(mapped_range_bounds_and_actual_packet_proof);
    printf("PASS actual binding helper: %u cases, %u checks\n",cases,checks);return 0;
}
