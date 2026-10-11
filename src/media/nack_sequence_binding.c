/* SPDX-License-Identifier: GPL-3.0-only */
#include "nack_sequence_binding.h"
#include <string.h>

static bool same_tag(const X4NackSequenceTag *a, const X4NackSequenceTag *b)
{
    return a->anchor_extended==b->anchor_extended &&
        a->previous_high_extended==b->previous_high_extended && a->anchor_us==b->anchor_us &&
        a->transport_epoch==b->transport_epoch && a->sequence_epoch==b->sequence_epoch &&
        a->previous_epoch==b->previous_epoch && a->anchor_rtp_timestamp==b->anchor_rtp_timestamp &&
        a->reason==b->reason;
}
static bool owner_ok(const X4NackSequenceBinding *s,const X4NackSequenceTag *t,uint64_t source,uint64_t now)
{
    return t && s->owner_epoch && t->transport_epoch==s->owner_epoch && source==s->source &&
        (source>>32)==(UINT64_C(0x80000000)|102u) && t->sequence_epoch &&
        t->anchor_extended<=UINT16_MAX && t->anchor_us && t->anchor_us<=now;
}
/* The observer's previous high may be ahead of the worker's consumed high,
 * but cannot precede it or cross an ambiguous modular halfspan. */
static bool mapping(const X4NackSequenceBinding *s,const X4NackSequenceTag *t,uint64_t *offset)
{
    if(!s->bound) {
        if(t->reason!=X4_NACK_SEQUENCE_INITIAL || t->sequence_epoch!=1 ||
            t->previous_epoch || t->previous_high_extended) return false;
        *offset=0; return true;
    }
    if(t->reason!=X4_NACK_SEQUENCE_OMISSION || s->committed.sequence_epoch==UINT32_MAX ||
        t->anchor_us<s->committed.anchor_us ||
        t->previous_epoch!=s->committed.sequence_epoch ||
        t->sequence_epoch!=s->committed.sequence_epoch+1u ||
        t->previous_high_extended<s->committed.anchor_extended ||
        t->previous_high_extended<s->consumed_raw_high ||
        t->previous_high_extended-s->consumed_raw_high>=32768u ||
        t->previous_high_extended>UINT64_MAX-s->canonical_offset) return false;
    uint32_t delta=(uint16_t)((uint16_t)t->anchor_extended-(uint16_t)t->previous_high_extended);
    if(delta>=32768u) return false;
    uint64_t previous=t->previous_high_extended+s->canonical_offset;
    if(previous>UINT64_MAX-delta) return false;
    uint64_t anchor=previous+delta;
    if(anchor<t->anchor_extended) return false;
    *offset=anchor-t->anchor_extended;
    return (*offset&UINT16_MAX)==0;
}
static int commit(X4NackSequenceBinding *s)
{
    if(!s->candidate_valid || !s->proof_valid || !same_tag(&s->candidate,&s->proof))
        return X4_BINDING_CANDIDATE;
    uint64_t offset;
    if(!mapping(s,&s->candidate,&offset)) {
        s->candidate_valid=s->proof_valid=false; ++s->counters.invalid;
        return X4_BINDING_INVALID;
    }
    s->committed=s->candidate; s->canonical_offset=offset;
    s->consumed_raw_high=s->committed.anchor_extended; s->bound=true;
    s->candidate_valid=s->proof_valid=false; ++s->counters.commits;
    return X4_BINDING_COMMITTED;
}
void x4_nack_binding_init(X4NackSequenceBinding *s,uint32_t owner,uint64_t source)
{
    memset(s,0,sizeof(*s)); s->owner_epoch=owner; s->source=source;
}
int x4_nack_binding_announce(X4NackSequenceBinding *s,const X4NackSequenceTag *t,
    uint64_t source,uint64_t now)
{
    ++s->counters.announcements;
    if(!owner_ok(s,t,source,now)) { ++s->counters.invalid; return X4_BINDING_INVALID; }
    if(s->bound && t->sequence_epoch<=s->committed.sequence_epoch) {
        if(same_tag(t,&s->committed)) { ++s->counters.duplicates; return X4_BINDING_DUPLICATE; }
        ++s->counters.stale; return X4_BINDING_STALE;
    }
    if(s->candidate_valid) {
        if(same_tag(t,&s->candidate)) { ++s->counters.duplicates; return X4_BINDING_DUPLICATE; }
        ++s->counters.invalid; return X4_BINDING_INVALID;
    }
    uint64_t offset;
    if(!mapping(s,t,&offset)) { ++s->counters.invalid; return X4_BINDING_INVALID; }
    s->candidate=*t; s->candidate_valid=true; ++s->counters.candidates;
    return commit(s);
}
bool x4_nack_binding_map(X4NackSequenceBinding *s,const X4NackSequenceTag *t,
    uint64_t source,uint64_t raw,uint16_t wire,uint32_t timestamp,uint64_t auth,uint64_t now,
    uint64_t *canonical)
{
    if(!owner_ok(s,t,source,now) || !s->bound || s->candidate_valid ||
        !same_tag(t,&s->committed) || !auth || auth<t->anchor_us || auth>now || (uint16_t)raw!=wire ||
        raw>UINT64_MAX-s->canonical_offset ||
        (raw==t->anchor_extended && timestamp!=t->anchor_rtp_timestamp)) {
        ++s->counters.unmapped_provenance; return false;
    }
    if(canonical) *canonical=raw+s->canonical_offset;
    if(raw>s->consumed_raw_high) s->consumed_raw_high=raw;
    ++s->counters.mapping_checks; return true;
}
int x4_nack_binding_stored(X4NackSequenceBinding *s,const X4NackSequenceTag *t,
    uint64_t source,uint64_t raw,uint16_t wire,uint32_t timestamp,uint64_t auth,uint64_t now,
    uint64_t *canonical)
{
    if(x4_nack_binding_map(s,t,source,raw,wire,timestamp,auth,now,canonical)) return X4_BINDING_NONE;
    /* Older STORED media may finish while a newer announcement is pending.
     * Keep the old-domain high for validating the candidate, but do not let
     * those tickets accept any newly bound request. */
    if(s->candidate_valid && owner_ok(s,t,source,now) && same_tag(t,&s->committed) &&
        auth>=t->anchor_us && auth<=now && (uint16_t)raw==wire && raw>s->consumed_raw_high)
        s->consumed_raw_high=raw;
    if(!owner_ok(s,t,source,now) || !auth || auth<t->anchor_us || auth>now || raw!=t->anchor_extended ||
        wire!=(uint16_t)raw || timestamp!=t->anchor_rtp_timestamp) {
        ++s->counters.unproven; return X4_BINDING_UNPROVEN;
    }
    if(s->bound && t->sequence_epoch<=s->committed.sequence_epoch) {
        ++s->counters.stale; return X4_BINDING_STALE;
    }
    uint64_t offset;
    if(!mapping(s,t,&offset) || (s->candidate_valid && !same_tag(t,&s->candidate))) {
        ++s->counters.invalid; return X4_BINDING_INVALID;
    }
    if(s->proof_valid && !same_tag(t,&s->proof)) {
        ++s->counters.invalid; return X4_BINDING_INVALID;
    }
    s->proof=*t; s->proof_authenticated_us=auth; s->proof_valid=true; ++s->counters.proofs;
    int result=commit(s);
    if(result==X4_BINDING_COMMITTED && canonical) *canonical=raw+s->canonical_offset;
    return result==X4_BINDING_CANDIDATE ? X4_BINDING_PROOF : result;
}
bool x4_nack_binding_offer_map(X4NackSequenceBinding *s,uint64_t source,
    uint32_t transport,uint32_t epoch,uint64_t first,uint64_t mask,uint64_t *canonical)
{
    unsigned highest=0;
    for(uint64_t bits=mask;bits>1;bits>>=1) ++highest;
    if(!s->bound || s->candidate_valid || source!=s->source || transport!=s->owner_epoch ||
        epoch!=s->committed.sequence_epoch || !mask || first<s->committed.anchor_extended ||
        first>UINT64_MAX-highest ||
        first+highest>UINT64_MAX-s->canonical_offset) return false;
    if(canonical) *canonical=first+s->canonical_offset;
    ++s->counters.mapped_offers; return true;
}
bool x4_nack_binding_waiting(const X4NackSequenceBinding *s,uint64_t source,uint32_t transport,uint32_t epoch)
{
    return s->candidate_valid && source==s->source && transport==s->owner_epoch &&
        epoch==s->candidate.sequence_epoch;
}
