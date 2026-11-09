/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual generator -> actual bounded inbox -> application receive/ingress ->
 * actual video_batch/reorder/grace/AU. Authentication flags and kernel/video/
 * diagnostic bridges are host doubles: no crypto, network, native decoding,
 * RGB or console result is implied by any PASS. */
#include "rtc_receiver_nack.hpp"
#include "receiver_nack_pipeline_worker.h"
#include "nack_grace.h"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>

namespace {
std::atomic<uint64_t> clockNow{1000};
std::atomic<unsigned> holdEvent{0};
std::atomic<bool> entered{false}, releaseGate{false};
struct NackEvent { unsigned event,flags; uint64_t identity,source,a,b,c; };
std::array<NackEvent,4096> nackEvents{};
std::atomic<unsigned> nackCount{0}, nackOmitted{0};
uint64_t checks=0; unsigned cases=0;
void fail(const char *text,int line){std::fprintf(stderr,"FAIL pipeline:%d %s\n",line,text);std::exit(1);}
#define CHECK(x) do{++checks;if(!(x))fail(#x,__LINE__);}while(0)
void resetClock(){clockNow.store(1000);}
void now(uint64_t value){CHECK(value>=clockNow.load());clockNow.store(value);}
void write16(uint8_t *p,uint16_t n){p[0]=uint8_t(n>>8);p[1]=uint8_t(n);}
void write32(uint8_t *p,uint32_t n){p[0]=uint8_t(n>>24);p[1]=uint8_t(n>>16);p[2]=uint8_t(n>>8);p[3]=uint8_t(n);}
rtc::message_ptr packet(uint16_t seq,uint32_t timestamp=90000,bool marker=true,
    std::vector<uint8_t> payload={0x61,0x11},uint32_t source=7,uint8_t pt=102)
{
    auto m=rtc::make_message(12+payload.size(),rtc::Message::Binary);
    auto *p=reinterpret_cast<uint8_t *>(m->data());std::memset(p,0,m->size());
    p[0]=0x80;p[1]=uint8_t(pt|(marker?128:0));write16(p+2,seq);write32(p+4,timestamp);write32(p+8,source);
    std::memcpy(p+12,payload.data(),payload.size());m->x4DiagFlags=X4_RD_AUTHENTICATED;return m;
}
void incoming(const std::shared_ptr<rtc::X4ReceiverNack>& g,const rtc::message_ptr& m)
{rtc::message_vector messages{m};g->incoming(messages,[](rtc::message_ptr){fail("receive must not dispatch",__LINE__);});}
void deliver(X4Pipeline *p,const rtc::message_ptr& m,uint64_t enqueueTime)
{
    now(enqueueTime);X4NackPacket ticket{};
    ticket.identity=m->x4NackIdentity;ticket.source=m->x4NackSource;
    ticket.extended_sequence=m->x4NackExtended;ticket.rtp_timestamp=m->x4NackRtpTimestamp;
    ticket.authenticated_us=m->x4NackAuthenticated;
#ifndef X4_PIPELINE_BASELINE
    ticket.sequence=m->x4NackSequence;
#endif
    x4_pipeline_receive(p,m->data(),m->size(),&ticket);
}
void receive(X4Pipeline *p,const std::shared_ptr<rtc::X4ReceiverNack>& g,
    const rtc::message_ptr& m,uint64_t authTime,uint64_t enqueueTime)
{now(authTime);incoming(g,m);deliver(p,m,enqueueTime);}
uint64_t counter(unsigned id)
{
    unsigned count=nackCount.load();for(unsigned i=count;i;--i){auto e=nackEvents[i-1];if(e.event==29&&e.a==id)return e.b;}
    fail("final cumulative counter exists",__LINE__);return 0;
}
std::vector<X4PipelineEvent> events(unsigned event,unsigned stage,uint64_t identity=UINT64_MAX)
{
    std::vector<X4PipelineEvent> result;
    for(size_t i=0;i<x4_pipeline_event_count();++i){X4PipelineEvent e{};CHECK(x4_pipeline_event(i,&e));
        if(e.kind==2&&e.event==event&&e.stage==stage&&(identity==UINT64_MAX||e.identity==identity))result.push_back(e);}
    return result;
}
[[maybe_unused]] void legacy_policy_proof_is_not_an_intentional_test_failure()
{
    X4NackGraceState s{};x4_nack_grace_init(&s,9,true);x4_nack_grace_bind_source(&s,7,1);
    X4NackGraceRequest r{9,(uint64_t(5)<<32)|2,7,1,12000,7,2};X4NackGraceEffects e{};
    CHECK(x4_nack_grace_offer(&s,&r,13000,&e)==X4_GRACE_OFFER_STALE_SOURCE);
    CHECK(s.sequence_epoch==1&&s.counters.stale_source_offers==1&&!s.active_count);
    ++cases;std::puts("PASS explicit 0.7.34 epoch1 policy reproduces stale_source (not an old native build)");
}
[[maybe_unused]] void wrap_gate_to_worker_AU(bool grace,bool dirtyByPoll,bool pollOwner=false)
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;entered=false;releaseGate=false;
    X4Pipeline *p=x4_pipeline_create(grace,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    receive(p,g,packet(65534,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33}),10000,10100);
    now(10200);x4_pipeline_batch(p);
    receive(p,g,packet(65535),10300,10400);receive(p,g,packet(0),10500,10600);
    now(10700);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).expected==1);
    auto two=packet(2);now(11000);
    if(pollOwner){incoming(g,two);deliver(p,two,11010);now(11020);x4_pipeline_batch(p);holdEvent=30;}
    else holdEvent=19;
    std::atomic<unsigned> localSends{0};
    std::atomic<int> pollResult{-99};
    std::thread observer([&]{if(pollOwner)pollResult=g->poll([&](rtc::message_ptr){++localSends;return true;});
                            else incoming(g,two);});
    auto wallLimit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<wallLimit)std::this_thread::yield();
    CHECK(entered.load());auto four=packet(4);now(pollOwner?11021:11001);incoming(g,four);
    CHECK(!four->x4NackSequence.sequence_epoch); // Omitted observation is not fabricated provenance.
    releaseGate.store(true,std::memory_order_release);observer.join();holdEvent=0;
    if(!pollOwner)deliver(p,two,11100);
    CHECK(localSends.load()==unsigned(pollOwner));
    if(pollOwner)CHECK(pollResult.load()==1);
    deliver(p,four,11101);now(11110);x4_pipeline_batch(p);
    if(dirtyByPoll){now(11200);CHECK(g->poll([](rtc::message_ptr){fail("retired watch cannot dispatch",__LINE__);return true;})==0);}
    auto six=packet(6,70000,false,{0x7c,0x81,0xaa});
    receive(p,g,six,12000,12100);
    CHECK(six->x4NackSequence.reason==X4_NACK_SEQUENCE_OMISSION);
    CHECK(six->x4NackSequence.sequence_epoch==2&&six->x4NackSequence.previous_epoch==1);
    CHECK(six->x4NackSequence.previous_high_extended==65538&&six->x4NackSequence.anchor_extended==6);
    CHECK(six->x4NackSequence.transport_epoch==5&&six->x4NackSequence.anchor_rtp_timestamp==70000);
    now(12200);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).epoch==2);
#ifndef X4_PIPELINE_BASELINE
    CHECK(x4_pipeline_snapshot(p).canonical_offset==65536); // Same-source rebase keeps the proven extended origin.
#endif
    if(!dirtyByPoll&&!pollOwner){
        now(12250);CHECK(g->poll([](rtc::message_ptr){fail("retired queued attempt must only cancel",__LINE__);return true;})==0);
        /* poll handles at most one queued attempt. Do not mistake the old
         * retained cancellation for a failure of the next new request. */
    }
    for(uint16_t seq:{uint16_t(1),uint16_t(3),uint16_t(5)})receive(p,g,packet(seq),12300+2*seq,12301+2*seq);
    now(12500);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).expected==7);
    auto eight=packet(8,70000,true,{0x7c,0x41,0xcc});receive(p,g,eight,13000,13100);
    for(uint16_t seq=9;seq<=39;++seq)receive(p,g,packet(seq,90000+seq),13200+2*seq,13201+2*seq);
    now(14000);x4_pipeline_batch(p);auto held=x4_pipeline_snapshot(p);
    if(grace){CHECK(held.expected==7&&held.buffered==32&&held.pending==1&&held.held>0);}
    else{CHECK(held.expected==40&&!held.pending);}
    CHECK(g->poll([&](rtc::message_ptr){++localSends;return true;})==1); // Local success only.
    auto seven=packet(7,70000,false,{0x7c,0x01,0xbb});now(20000);incoming(g,seven);
    CHECK(seven->x4NackIdentity==((uint64_t(5)<<32)|2));
    CHECK(seven->x4NackExtended==7&&seven->x4NackAuthenticated==20000);
    CHECK(seven->x4NackSource==((UINT64_C(1)<<63)|(UINT64_C(102)<<32)|7)&&
        seven->x4NackSequence.transport_epoch==5&&seven->x4NackSequence.sequence_epoch==2&&
        seven->x4NackSequence.previous_epoch==1);
    auto before=x4_pipeline_snapshot(p);if(grace)CHECK(before.pending==1&&!before.accepted);
    deliver(p,seven,20100);auto enqueued=x4_pipeline_snapshot(p);CHECK(enqueued.queue_depth==1);
    if(grace)CHECK(enqueued.pending==1&&!enqueued.accepted); // AUTH+ENQUEUE never complete worker grace.
    now(25000);x4_pipeline_batch(p);auto end=x4_pipeline_snapshot(p);
    CHECK(end.expected==40&&!end.pending&&end.queue_depth==0&&!end.event_omitted);
    const uint64_t request=(uint64_t(5)<<32)|2;
    auto callbacks=events(31,5,request),pops=events(31,6,request),stored=events(31,7,request);
    CHECK(callbacks.size()==1&&pops.size()==1&&stored.size()==1);
    CHECK(callbacks[0].time==20100&&callbacks[0].b==20000);
    CHECK(pops[0].time==25000&&pops[0].b==20100&&pops[0].c==20000);
    CHECK(stored[0].time==25000);
    if(grace){CHECK(stored[0].flags==x4_pipeline_reorder_code(false)&&end.accepted==1);}
    else{CHECK(stored[0].flags==x4_pipeline_reorder_code(true)&&!end.accepted);}
    auto auEnter=events(26,65535,request); // Numeric NACK_AU_CONTRIBUTION, not output picture.
    bool contribution=false,structural=false,accepted=false;
    for(auto e:auEnter){contribution|=e.b==0;structural|=e.b==1;accepted|=e.b==2;}
    if(grace){CHECK(contribution&&structural&&accepted);uint8_t bytes[128]{};size_t n=x4_pipeline_feed_bytes(bytes,sizeof(bytes));
        const uint8_t expected[]={0,0,0,1,0x61,0xaa,0xbb,0xcc};CHECK(n==sizeof(expected)&&!std::memcmp(bytes,expected,n));}
    else CHECK(!contribution&&!structural&&!accepted);
    CHECK(!end.inbox_omitted&&!end.inbox_stale&&g->close()==0);
    CHECK(counter(100)==1&&counter(35)==1&&counter(14)==2&&counter(4)==0);
    CHECK(localSends.load()==unsigned(pollOwner)+1); // No feedback burst after the rebase.
    CHECK(!nackOmitted.load());x4_pipeline_close(p);
    ++cases;std::printf("PASS actual pipeline G%u gate-owner=%s dirty-consumer=%s wrap65536 AUTH/ENQUEUE/POP/STORED/AU separate\n",
        grace?45:0,pollOwner?"poll":"incoming",dirtyByPoll?"poll":"incoming");
}
[[maybe_unused]] void timely_AUTH_queued_before_deadline_is_not_late_worker_utility(bool grace,bool capacity,bool authAfterDeadline=false)
{
    /* The wide-span calendar is the timely case; delayed AUTH/storage is
     * covered separately with one missing position and no capacity pressure. */
    CHECK(!capacity||!authAfterDeadline);
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(grace,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    receive(p,g,packet(0,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33}),10000,10100);
    now(10200);x4_pipeline_batch(p);
    receive(p,g,packet(capacity?15:2),11000,11100);now(11200);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).pending==(grace?(capacity?14u:1u):0u));
    if(capacity)for(uint16_t seq=16;seq<=129;++seq)receive(p,g,packet(seq),13000+2*seq,13001+2*seq);
    unsigned returns=capacity?14:1;
    for(unsigned seq=1;seq<=returns;++seq){auto m=packet(uint16_t(seq));
        receive(p,g,m,(authAfterDeadline?56000:20000)+2*seq,(authAfterDeadline?56001:20001)+2*seq);
        CHECK(m->x4NackIdentity==((uint64_t(5)<<32)|1));}
    auto queued=x4_pipeline_snapshot(p);CHECK(queued.pending==(grace?returns:0u)&&!queued.accepted);
    const uint64_t workerTime=authAfterDeadline?57000:(capacity?25000:56000);
    now(workerTime);x4_pipeline_batch(p);auto end=x4_pipeline_snapshot(p);
    if(grace&&capacity&&!authAfterDeadline){
        /* The unchanged trace calendar has115 resident future packets, not
         * 128 occupied positions. Sequence129 is128 ahead of expected1; sparse
         * video must retain it and then accept all14 actual worker returns. */
        CHECK(!end.pending&&end.accepted==14&&end.queue_depth==0&&!end.event_omitted);
        CHECK(end.expected==130&&!end.capacity_ranges&&!end.distance_ranges);
        const uint64_t request=(uint64_t(5)<<32)|1;
        auto stored=events(31,7,request),abandon=events(31,12,request),releases=events(31,4,request);
        CHECK(stored.size()==14&&abandon.empty());
        for(auto e:stored)CHECK(e.flags==x4_pipeline_reorder_code(false)&&e.time==25000);
        bool completed=false;for(auto e:releases)completed|=(e.flags&255)==X4_GRACE_RELEASE_ACCEPTED;
        CHECK(completed&&!end.reorder_ticket_omissions);
        auto contributions=events(26,65535,request);
        unsigned enteredAU=0,structural=0,acceptedAU=0;
        for(auto e:contributions){enteredAU+=e.b==0;structural+=e.b==1;acceptedAU+=e.b==2;}
        CHECK(enteredAU==14&&structural==14&&acceptedAU==14);
        CHECK(g->close()==0);x4_pipeline_close(p);++cases;
        std::puts("PASS same-calendar G45 worker25000 accepts14/14 with115 resident futures and span128; synthetic AU stages, native output untested");
        return;
    }
    CHECK(!end.pending&&!end.accepted&&end.queue_depth==0&&!end.event_omitted);
    CHECK(end.expected==(capacity?130u:3u));
    const uint64_t request=(uint64_t(5)<<32)|1;
    auto releases=events(31,4,request),abandon=events(31,12,request),stored=events(31,7,request);
    CHECK(stored.size()==returns);
    if(grace){bool reason=false;for(auto e:releases)reason|=(e.flags&255)==X4_GRACE_RELEASE_DEADLINE;CHECK(reason);
        CHECK(abandon.size()==1&&abandon[0].time==workerTime);
        for(auto e:stored)CHECK(e.flags==x4_pipeline_reorder_code(true)); // Protected gap already abandoned at its absolute deadline.
    }else if(capacity){CHECK(releases.empty()&&abandon.empty());
        for(auto e:stored)CHECK(e.flags==x4_pipeline_reorder_code(true)); // Existing depth32 drained before the returns.
    }else{CHECK(releases.empty()&&abandon.empty());
        for(auto e:stored)CHECK(e.flags==x4_pipeline_reorder_code(false)); // Ordinary expiry is suppressed while ingress still holds data.
    }
    auto contributions=events(26,65535,request);
    if(grace||capacity)CHECK(contributions.empty());
    else{bool enteredAU=false,structural=false,acceptedAU=false;
        for(auto e:contributions){enteredAU|=e.b==0;structural|=e.b==1;acceptedAU|=e.b==2;}
        CHECK(enteredAU&&structural&&acceptedAU); // Ordinary processing, not grace acceptance or a native frame.
    }
    CHECK(g->close()==0);x4_pipeline_close(p);
    ++cases;std::printf("PASS same-calendar G%u AUTH=%s worker=%llu %s; grace acceptance0, native output untested\n",
        grace?45:0,authAfterDeadline?"after deadline":"before deadline",static_cast<unsigned long long>(workerTime),
        capacity?"wide span128 with available physical slots":"absolute45ms boundary");
}
#ifndef X4_PIPELINE_BASELINE
bool sameSequenceTag(const X4NackSequenceTag& a,const X4NackSequenceTag& b)
{
    return a.anchor_extended==b.anchor_extended&&a.previous_high_extended==b.previous_high_extended&&
        a.anchor_us==b.anchor_us&&a.transport_epoch==b.transport_epoch&&
        a.sequence_epoch==b.sequence_epoch&&a.previous_epoch==b.previous_epoch&&
        a.anchor_rtp_timestamp==b.anchor_rtp_timestamp&&a.reason==b.reason;
}
void checkStoredTuple(X4Pipeline *p,const rtc::message_ptr& packet,uint64_t workerTime)
{
    const auto *bytes=reinterpret_cast<const uint8_t *>(packet->data());
    uint16_t sequence=uint16_t((uint16_t(bytes[2])<<8)|bytes[3]);
    std::array<uint8_t,2048> stored{};size_t size=0;X4NackPacket ticket{};uint64_t storedTime=0;
    CHECK(x4_pipeline_stored(p,sequence,stored.data(),stored.size(),&size,&ticket,&storedTime));
    CHECK(size==packet->size()&&!std::memcmp(stored.data(),packet->data(),size));
    CHECK(storedTime==workerTime&&ticket.identity==packet->x4NackIdentity&&
        ticket.source==packet->x4NackSource&&ticket.extended_sequence==packet->x4NackExtended&&
        ticket.rtp_timestamp==packet->x4NackRtpTimestamp&&
        ticket.authenticated_us==packet->x4NackAuthenticated);
    CHECK(sameSequenceTag(ticket.sequence,packet->x4NackSequence));
}
void initializeSparsePipeline(X4Pipeline *p,const std::shared_ptr<rtc::X4ReceiverNack>& g)
{
    receive(p,g,packet(0,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33}),10000,10100);
    now(10200);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).expected==1&&x4_pipeline_snapshot(p).epoch==1);
    CHECK(x4_pipeline_snapshot(p).max_forward==4096);
}
rtc::message_ptr taggedPayload(uint16_t sequence,bool marker=false)
{return packet(sequence,70000,marker,{0x61,uint8_t(sequence),uint8_t(sequence>>8)});}
void checkSyntheticOrderedAU(unsigned first,unsigned last)
{
    std::vector<uint8_t> wanted;
    for(unsigned sequence=first;sequence<=last;++sequence){
        const uint8_t nal[]={0,0,0,1,0x61,uint8_t(sequence),uint8_t(sequence>>8)};
        wanted.insert(wanted.end(),std::begin(nal),std::end(nal));
    }
    std::vector<uint8_t> observed(wanted.size()+16);
    size_t size=x4_pipeline_feed_bytes(observed.data(),observed.size());
    CHECK(size==wanted.size()&&!std::memcmp(observed.data(),wanted.data(),size));
}
void sparse_low_seven_bit_alias_keeps_exact_physical_ticket()
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    initializeSparsePipeline(p,g);
    auto three=taggedPayload(3),far=taggedPayload(130),two=taggedPayload(2);
    receive(p,g,three,11000,11100);receive(p,g,far,11200,11300);
    now(11400);x4_pipeline_batch(p);
    receive(p,g,two,12000,12100);CHECK(two->x4NackIdentity==((uint64_t(5)<<32)|1));
    now(12200);x4_pipeline_batch(p);
    auto held=x4_pipeline_snapshot(p);
    CHECK(held.expected==1&&held.buffered==3&&held.pending==1&&held.accepted==1);
    CHECK(held.resident_bytes==three->size()+far->size()+two->size());
    CHECK((uint16_t(two->x4NackExtended)&127)==(uint16_t(far->x4NackExtended)&127));
    checkStoredTuple(p,three,11400);checkStoredTuple(p,far,11400);checkStoredTuple(p,two,12200);
    CHECK(!far->x4NackIdentity&&far->x4NackExtended==130&&two->x4NackExtended==2);
    auto duplicate=taggedPayload(2);receive(p,g,duplicate,12300,12400);
    now(12500);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).buffered==3&&x4_pipeline_snapshot(p).accepted==1);
    checkStoredTuple(p,two,12200);checkStoredTuple(p,far,11400);
    auto one=taggedPayload(1);receive(p,g,one,13000,13100);now(13200);x4_pipeline_batch(p);
    auto end=x4_pipeline_snapshot(p);
    CHECK(end.expected==4&&end.buffered==1&&!end.pending&&end.accepted==2);
    CHECK(!end.reorder_ticket_omissions&&!end.capacity_ranges&&!end.distance_ranges);
    checkStoredTuple(p,far,11400);
    CHECK(g->close()==0);x4_pipeline_close(p);++cases;
    std::puts("PASS actual worker slots keep sequence2 watched ticket distinct from130 ordinary ticket, exact bytes/source/epoch/clocks; duplicate preserves first owner");
}
void full_sparse_expected_direct_is_timely_only_before_deadline(bool deadline)
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    initializeSparsePipeline(p,g);
    std::vector<rtc::message_ptr> futures;
    for(unsigned sequence=2;sequence<=129;++sequence){
        auto m=taggedPayload(uint16_t(sequence),sequence==129);futures.push_back(m);
        receive(p,g,m,11000+2*sequence,11001+2*sequence);
    }
    now(13000);x4_pipeline_batch(p);auto held=x4_pipeline_snapshot(p);
    CHECK(held.expected==1&&held.buffered==128&&held.pending==1&&!held.accepted);
    CHECK(held.resident_bytes==128*futures[0]->size()&&!held.direct_packets&&!held.capacity_ranges);
    for(const auto& m:futures)checkStoredTuple(p,m,13000);
    auto one=taggedPayload(1);receive(p,g,one,20000,20100);
    CHECK(x4_pipeline_snapshot(p).pending==1&&x4_pipeline_snapshot(p).queue_depth==1);
    const uint64_t workerTime=deadline?56004:25000; // Detect happened at11004; exact deadline=56004.
    now(workerTime);x4_pipeline_batch(p);auto end=x4_pipeline_snapshot(p);
    CHECK(end.expected==130&&!end.buffered&&!end.resident_bytes&&!end.pending&&!end.queue_depth);
    CHECK(!end.capacity_ranges&&!end.distance_ranges&&!end.reorder_ticket_omissions&&!end.event_omitted);
    auto outcomes=events(31,7,(uint64_t(5)<<32)|1);CHECK(outcomes.size()==1&&outcomes[0].time==workerTime);
    auto contributions=events(26,65535,(uint64_t(5)<<32)|1);
    if(deadline){
        CHECK(outcomes[0].flags==x4_pipeline_reorder_code(true)&&!end.accepted&&!end.direct_packets);
        CHECK(contributions.empty());
        auto releases=events(31,4,(uint64_t(5)<<32)|1);bool expired=false;
        for(auto e:releases){expired|=e.flags==X4_GRACE_RELEASE_DEADLINE;}
        CHECK(expired);
    }else{
        CHECK(outcomes[0].flags==x4_pipeline_reorder_direct_code()&&end.accepted==1&&end.direct_packets==1);
        CHECK(outcomes[0].flags!=x4_pipeline_reorder_code(false)); // Actual C DIRECT, not STORED.
        bool enteredAU=false,structural=false,acceptedAU=false;
        for(auto e:contributions){enteredAU|=e.b==0;structural|=e.b==1;acceptedAU|=e.b==2;}
        CHECK(enteredAU&&structural&&acceptedAU);checkSyntheticOrderedAU(1,129);
    }
    CHECK(g->close()==0);x4_pipeline_close(p);++cases;
    std::printf("PASS actual full128 worker %s expected arrival; DIRECT distinct fromSTORED, no129th slot; AUTH earlier is not deadline acceptance\n",deadline?"exact-deadline rejects":"timely delivers");
}
void direct_fresh_budget_rejection_keeps_resident_ownership(unsigned kind)
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    initializeSparsePipeline(p,g);
    std::vector<rtc::message_ptr> futures;
    for(unsigned sequence=2;sequence<=129;++sequence){
        auto m=taggedPayload(uint16_t(sequence));futures.push_back(m);
        receive(p,g,m,11000+2*sequence,11001+2*sequence);
    }
    now(13000);x4_pipeline_batch(p);auto before=x4_pipeline_snapshot(p);
    CHECK(before.expected==1&&before.buffered==128&&before.pending==1&&!before.accepted);
    CHECK(!before.waiting_keyframe&&!before.direct_packets&&!before.direct_budget_drops);
    for(const auto& m:futures)checkStoredTuple(p,m,13000);
    /* Marker would make this a complete synthetic AU and invoke the feed
     * double if DIRECT bypassed its fresh emission budget check. */
    auto one=taggedPayload(1,true);receive(p,g,one,20000,20100);
    const uint64_t value=kind==X4_PIPELINE_DIRECT_HOOK_TIME?41000:
        kind==X4_PIPELINE_DIRECT_HOOK_DECODE?4:0;
    x4_pipeline_arm_direct_hook(p,kind,value);
    now(25000);x4_pipeline_batch(p);auto after=x4_pipeline_snapshot(p);
    std::printf("BUDGET_OBS hook=%u expected=%u resident=%u bytes=%u accepted=%llu drops=%llu feed_before=%llu feed_after=%llu feed_delta=%llu decode_counter_delta=%llu\n",
        kind,after.expected,after.buffered,after.resident_bytes,
        static_cast<unsigned long long>(after.accepted),
        static_cast<unsigned long long>(after.direct_budget_drops),
        static_cast<unsigned long long>(before.feed_calls),
        static_cast<unsigned long long>(after.feed_calls),
        static_cast<unsigned long long>(after.feed_calls-before.feed_calls),
        static_cast<unsigned long long>(after.worker_decode_calls-before.worker_decode_calls));
    CHECK(x4_pipeline_direct_hook_count()==1);
    CHECK(after.expected==2&&after.buffered==128&&after.resident_bytes==before.resident_bytes);
    /* Prove the unsafe delivery itself before inspecting the new counter,
     * which is absent from the private pre-guard reproduction snapshot. */
    CHECK(after.feed_calls==before.feed_calls&&after.au_submitted==before.au_submitted);
    CHECK(!after.accepted&&events(26,65535,(uint64_t(5)<<32)|1).empty());
    for(size_t i=0;i<x4_pipeline_event_count();++i){X4PipelineEvent e{};CHECK(x4_pipeline_event(i,&e));
        CHECK(!(e.kind==1&&e.event==x4_pipeline_reorder_emit_event()&&e.a==1));}
    CHECK(after.direct_packets==1&&after.direct_budget_drops==1&&!after.pending);
    CHECK(after.worker_decode_calls-before.worker_decode_calls==(kind==X4_PIPELINE_DIRECT_HOOK_DECODE?4u:0u));
    CHECK(after.lost_packets==before.lost_packets+1&&after.dropped_packets==before.dropped_packets+1);
    CHECK(after.recovery_episodes==before.recovery_episodes+1&&after.waiting_keyframe);
    CHECK(after.budget_reason==x4_pipeline_budget_reason(kind));
    CHECK(after.worker_stopped==(kind==X4_PIPELINE_DIRECT_HOOK_STOP));
    CHECK(after.epoch==before.epoch&&after.canonical_offset==before.canonical_offset);
    CHECK(!after.capacity_ranges&&!after.distance_ranges&&!after.reorder_ticket_omissions&&!after.event_omitted);
    for(const auto& m:futures)checkStoredTuple(p,m,13000);
    const uint64_t request=(uint64_t(5)<<32)|1;
    auto admissions=events(31,7,request),drops=events(31,x4_pipeline_direct_budget_event(),request);
    CHECK(admissions.size()==1&&admissions[0].flags==x4_pipeline_reorder_direct_code());
    CHECK(drops.size()==1&&drops[0].flags==x4_pipeline_budget_reason(kind));
    CHECK(drops[0].a==1&&drops[0].b==((UINT64_C(1)<<32)|2)&&drops[0].c==128);
    CHECK(drops[0].time==(kind==X4_PIPELINE_DIRECT_HOOK_TIME?41000u:25000u));
    auto releases=events(31,4,request);bool passed=false;
    for(auto e:releases){passed|=e.flags==X4_GRACE_RELEASE_PASSED;}
    CHECK(passed);
    CHECK(g->close()==0);x4_pipeline_close(p);++cases;
    std::printf("PASS actual DIRECT fresh %s guard after admission: one advance,128 exact resident tuples, noAU/feed/emit, explicit recovery/PASSED\n",
        kind==X4_PIPELINE_DIRECT_HOOK_TIME?"16ms":kind==X4_PIPELINE_DIRECT_HOOK_DECODE?"four synthetic Decode-counter":"stop");
}
void actual_failed_full_admission_is_capacity_and_preserves_recovery()
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    initializeSparsePipeline(p,g);
    for(unsigned sequence=2;sequence<=129;++sequence)
        receive(p,g,taggedPayload(uint16_t(sequence)),11000+2*sequence,11001+2*sequence);
    now(13000);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).buffered==128);
    auto future=taggedPayload(130);receive(p,g,future,14000,14100);
    now(14200);x4_pipeline_batch(p);auto after=x4_pipeline_snapshot(p);
    CHECK(after.capacity_ranges==1&&!after.distance_ranges&&!after.direct_packets&&!after.pending);
    CHECK(after.expected==130&&!after.buffered&&!after.resident_bytes&&!after.accepted);
    CHECK(after.recovery_episodes==1&&after.waiting_keyframe&&!after.reorder_ticket_omissions&&!after.event_omitted);
    std::array<uint8_t,2048> bytes{};size_t size=0;X4NackPacket ticket{};uint64_t storedTime=0;
    CHECK(!x4_pipeline_stored(p,130,bytes.data(),bytes.size(),&size,&ticket,&storedTime));
    bool full=false,stored=false;
    for(size_t i=0;i<x4_pipeline_event_count();++i){X4PipelineEvent e{};CHECK(x4_pipeline_event(i,&e));
        if(e.kind==1&&e.event==x4_pipeline_reorder_insert_event()&&(e.a&UINT32_MAX)==130){
            full|=e.flags==x4_pipeline_reorder_full_code();stored|=e.flags==x4_pipeline_reorder_code(false);}}
    CHECK(full&&!stored); // Actual rejected attempted insert; no invented packet residence.
    auto releases=events(31,4,(uint64_t(5)<<32)|1);bool capacity=false;
    for(auto e:releases){capacity|=e.flags==X4_GRACE_RELEASE_CAPACITY;}
    CHECK(capacity);
    CHECK(g->close()==0);x4_pipeline_close(p);++cases;
    std::puts("PASS actual future admission at128 returnsFULL, recordsCAPACITY, preserves existing recovery and never fabricatesSTORED forsequence130");
}
void normal_and_inverse_returns_preserve_worker_order(bool inverse)
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    initializeSparsePipeline(p,g);
    for(unsigned sequence=15;sequence<=129;++sequence)
        receive(p,g,taggedPayload(uint16_t(sequence),sequence==129),11000+2*sequence,11001+2*sequence);
    now(13000);x4_pipeline_batch(p);auto held=x4_pipeline_snapshot(p);
    CHECK(held.expected==1&&held.buffered==115&&held.pending==14&&!held.accepted);
    for(unsigned index=0;index<14;++index){unsigned sequence=inverse?14-index:index+1;
        auto returned=taggedPayload(uint16_t(sequence));receive(p,g,returned,20000+2*index,20001+2*index);
        CHECK(returned->x4NackIdentity==((uint64_t(5)<<32)|1)&&returned->x4NackExtended==sequence);}
    now(25000);x4_pipeline_batch(p);auto end=x4_pipeline_snapshot(p);
    CHECK(end.accepted==14&&!end.pending&&end.expected==130&&!end.buffered&&!end.resident_bytes);
    CHECK(end.direct_packets==unsigned(inverse)&&!end.capacity_ranges&&!end.distance_ranges&&
        !end.reorder_ticket_omissions&&!end.event_omitted);
    checkSyntheticOrderedAU(1,129);
    CHECK(g->close()==0);x4_pipeline_close(p);++cases;
    std::printf("PASS actual worker14 %s returns before45ms, exact synthetic AU payload order1..129; native output untested\n",inverse?"inverted":"normal");
}
void fresh_session_rejects_old_controls_and_provenance()
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto old=std::make_shared<rtc::X4ReceiverNack>(true,5);old->acceptedFeedback(true);
    initializeSparsePipeline(p,old);
    receive(p,old,taggedPayload(2),11000,11100);now(11200);x4_pipeline_batch(p);
    auto oldReturn=taggedPayload(1);now(12000);incoming(old,oldReturn);
    CHECK(oldReturn->x4NackIdentity==((uint64_t(5)<<32)|1));
    CHECK(old->close()==0);x4_pipeline_close(p);
    resetClock();nackCount=0;nackOmitted=0;
    p=x4_pipeline_create(true,6);
    auto current=std::make_shared<rtc::X4ReceiverNack>(true,6);current->acceptedFeedback(true);
    initializeSparsePipeline(p,current);
    receive(p,current,taggedPayload(2),11000,11100);now(11200);x4_pipeline_batch(p);
    X4NackGraceControl stale{};stale.kind=X4_NACK_GRACE_REQUEST;
    stale.identity=(uint64_t(5)<<32)|1;stale.source=(UINT64_C(1)<<63)|(UINT64_C(102)<<32)|7;
    stale.sequence_epoch=1;stale.first_extended=1;stale.original_mask=1;
    stale.detected_us=11000;stale.deadline_us=56000;
    now(12000);CHECK(!x4_pipeline_control(&stale));
    deliver(p,oldReturn,12100);now(12200);x4_pipeline_batch(p);auto after=x4_pipeline_snapshot(p);
    CHECK(after.epoch==1&&after.expected==3&&!after.accepted&&!after.pending&&after.inbox_stale==1);
    CHECK(!after.capacity_ranges&&!after.distance_ranges&&!after.reorder_ticket_omissions&&!after.event_omitted);
    CHECK(current->close()==0);x4_pipeline_close(p);++cases;
    std::puts("PASS close/new session retains no slots or holds; old transport control rejected and old payload provenance cannot grant grace");
}
void mapped_stale_offer_consumes_ID_before_a_later_valid_binding()
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    receive(p,g,packet(0,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33}),10000,10100);
    now(10200);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).epoch==1);
    const uint64_t source=(UINT64_C(1)<<63)|(UINT64_C(102)<<32)|7;
    X4NackGraceControl sequence{};sequence.kind=X4_NACK_GRACE_SEQUENCE;
    sequence.identity=(uint64_t(5)<<32)|2;sequence.source=source;sequence.sequence_epoch=2;
    sequence.first_extended=6;sequence.original_mask=0;sequence.selected_mask=1;
    sequence.detected_us=11000;sequence.attempt_start_us=90000;sequence.reason=X4_NACK_SEQUENCE_OMISSION;
    sequence.deadline_us=1; // Contradictory announcement retires grace but leaves its proven helper mapping unchanged.
    now(11100);CHECK(x4_pipeline_control(&sequence));x4_pipeline_batch(p);
    X4NackGraceControl request{};request.kind=X4_NACK_GRACE_REQUEST;
    request.identity=(uint64_t(5)<<32)|1;request.source=source;request.sequence_epoch=1;
    request.first_extended=1;request.original_mask=1;request.detected_us=12000;request.deadline_us=57000;
    now(12100);CHECK(x4_pipeline_control(&request));x4_pipeline_batch(p);
    auto first=events(31,3,request.identity);CHECK(first.size()==1&&(first[0].c&UINT32_MAX)==X4_GRACE_OFFER_STALE_SOURCE);
    CHECK(x4_pipeline_snapshot(p).stale_source==1&&!x4_pipeline_snapshot(p).admitted);
    /* Explicit contract fixture for SEQUENCE + matching metadata; production
     * generation of this same transition is covered by the real gate suite.
     * This proof still uses actual ingress, parsing, reorder STORED and helper. */
    sequence.deadline_us=0;sequence.detected_us=13000;
    now(13000);CHECK(x4_pipeline_control(&sequence));
    auto anchor=packet(6);anchor->x4NackSource=source;anchor->x4NackExtended=6;
    anchor->x4NackRtpTimestamp=90000;anchor->x4NackAuthenticated=13000;
    anchor->x4NackSequence={};anchor->x4NackSequence.transport_epoch=5;
    anchor->x4NackSequence.sequence_epoch=2;anchor->x4NackSequence.previous_epoch=1;
    anchor->x4NackSequence.anchor_extended=6;anchor->x4NackSequence.anchor_us=13000;
    anchor->x4NackSequence.anchor_rtp_timestamp=90000;anchor->x4NackSequence.reason=X4_NACK_SEQUENCE_OMISSION;
    deliver(p,anchor,13010);now(13100);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).epoch==2);
    request.sequence_epoch=2;request.first_extended=7;request.detected_us=14000;request.deadline_us=59000;
    now(14100);CHECK(x4_pipeline_control(&request));x4_pipeline_batch(p);
    auto duplicate=events(31,3,request.identity);CHECK(duplicate.size()==2);
    CHECK((duplicate[1].c&UINT32_MAX)==X4_GRACE_OFFER_DUPLICATE);
    CHECK(x4_pipeline_snapshot(p).duplicate==1&&!x4_pipeline_snapshot(p).pending&&!x4_pipeline_snapshot(p).admitted);
    request.identity=(uint64_t(5)<<32)|2;request.detected_us=15000;request.deadline_us=60000;
    now(15100);CHECK(x4_pipeline_control(&request));x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).admitted==1&&x4_pipeline_snapshot(p).pending==1);
    CHECK(!x4_pipeline_snapshot(p).event_omitted&&g->close()==0);x4_pipeline_close(p);++cases;
    std::puts("PASS F3 real worker mapped STALE_SOURCE burns consumed ID; later valid STORED binding cannot renew it, fresh ID still works");
}
void foreign_track_and_stale_session_ticket_never_grant_worker_acceptance(bool coherentOldOwner)
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    receive(p,g,packet(0,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33}),10000,10100);
    now(10200);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).epoch==1);
    /* These receive-path fixtures are metadata-less and use actual RTP parsing
     * and Track admission. They must not move the accepted source or expected. */
    deliver(p,packet(1,90001,true,{0x61,0x22},8),10300);
    deliver(p,packet(1,90001,true,{0x61,0x22},7,103),10400);
    now(10500);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).expected==1&&x4_pipeline_snapshot(p).epoch==1);
    receive(p,g,packet(2),11000,11100);now(11200);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).pending==1&&!x4_pipeline_snapshot(p).accepted);
    const uint64_t request=(uint64_t(5)<<32)|1;
    X4NackGraceControl foreign{};foreign.identity=request;foreign.sequence_epoch=1;
    foreign.first_extended=1;foreign.original_mask=1;foreign.detected_us=11000;
    foreign.deadline_us=56000;foreign.kind=X4_NACK_GRACE_REQUEST;
    for(uint64_t source:{(UINT64_C(1)<<63)|(UINT64_C(102)<<32)|8,
                         (UINT64_C(1)<<63)|(UINT64_C(103)<<32)|7}){
        foreign.source=source;now(11300);CHECK(x4_pipeline_control(&foreign));x4_pipeline_batch(p);
        CHECK(x4_pipeline_snapshot(p).pending==1&&!x4_pipeline_snapshot(p).accepted);
    }
    foreign.source=(UINT64_C(1)<<63)|(UINT64_C(102)<<32)|7;
    foreign.identity=(uint64_t(6)<<32)|1;now(11400);
    CHECK(!x4_pipeline_control(&foreign)); // Actual inbox rejects the stale transport owner.
    CHECK(x4_pipeline_snapshot(p).inbox_stale==1&&x4_pipeline_snapshot(p).pending==1);
    /* The observed RTP can be STORED even when its injected provenance belongs
     * to a stale owner. STORED must not authorize grace from that metadata. */
    auto one=packet(1);std::shared_ptr<rtc::X4ReceiverNack> oldOwner;
    if(coherentOldOwner){
        oldOwner=std::make_shared<rtc::X4ReceiverNack>(true,6);oldOwner->acceptedFeedback(true);
        now(11900);incoming(oldOwner,packet(65535));now(11910);incoming(oldOwner,packet(2));
        now(12000);incoming(oldOwner,one);
        CHECK(one->x4NackIdentity==((uint64_t(6)<<32)|1)&&one->x4NackSequence.transport_epoch==6);
    }else{now(12000);incoming(g,one);CHECK(one->x4NackIdentity==request);
        one->x4NackSequence.transport_epoch=6; // Explicit contradictory metadata negative fixture.
    }
    deliver(p,one,12100);now(12200);x4_pipeline_batch(p);
    auto end=x4_pipeline_snapshot(p);CHECK(end.expected==3&&end.epoch==1&&!end.accepted&&!end.pending);
    auto stored=events(31,7,(uint64_t(coherentOldOwner?6:5)<<32)|1);
    CHECK(stored.size()==1&&stored[0].flags==x4_pipeline_reorder_code(false));
    if(oldOwner)CHECK(oldOwner->close()==0);
    auto duplicate=packet(1);receive(p,g,duplicate,13000,13100);
    CHECK(bool(duplicate->x4NackIdentity)==coherentOldOwner); // Current owner has not seen this return in the stale-owner branch.
    CHECK(duplicate->x4NackSequence.sequence_epoch==1); // Repeat provenance, not a new request or binding.
    now(13200);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).expected==3&&!x4_pipeline_snapshot(p).accepted);
    CHECK(!x4_pipeline_snapshot(p).event_omitted&&g->close()==0);x4_pipeline_close(p);++cases;
    std::printf("PASS actual Track/inbox/worker: foreign SSRC/PT rejected; STORED with %s owner metadata never counts as grace acceptance; duplicate cannot rearm\n",
        coherentOldOwner?"coherent stale":"contradictory");
}
void consumed_unmapped_request_cannot_rearm_after_late_real_anchor()
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;
    X4Pipeline *p=x4_pipeline_create(true,5);
    /* Packet0 is explicitly metadata-less. It still takes the real callback,
     * ingress and Track path; no REQUEST is allowed to create the binding. */
    auto zero=packet(0,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33});
    deliver(p,zero,10100);now(10200);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).locked&&x4_pipeline_snapshot(p).epoch==0);
    const uint64_t source=(UINT64_C(1)<<63)|(UINT64_C(102)<<32)|7;
    X4NackGraceControl control{};
    control.identity=(uint64_t(5)<<32)|1;control.source=source;control.sequence_epoch=1;
    control.first_extended=2;control.original_mask=1;control.detected_us=10500;
    control.deadline_us=55500;control.kind=X4_NACK_GRACE_REQUEST;
    now(10600);CHECK(x4_pipeline_control(&control));x4_pipeline_batch(p);
    auto consumed=x4_pipeline_snapshot(p);CHECK(consumed.stale_source==1&&!consumed.pending&&!consumed.admitted);
    /* Only this actual handler anchor + actual STORED packet can bind epoch1. */
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    receive(p,g,packet(1),12000,12100);now(12200);x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).epoch==1);
    /* Replaying a burned ID with a fresh-looking deadline is still duplicate. */
    control.detected_us=12900;control.deadline_us=57900;
    now(13000);CHECK(x4_pipeline_control(&control));x4_pipeline_batch(p);
    auto duplicate=x4_pipeline_snapshot(p);CHECK(duplicate.duplicate==1&&!duplicate.pending&&!duplicate.admitted);
    control.identity=(uint64_t(5)<<32)|2;control.detected_us=14000;control.deadline_us=59000;
    now(14100);CHECK(x4_pipeline_control(&control));x4_pipeline_batch(p);
    auto fresh=x4_pipeline_snapshot(p);CHECK(fresh.admitted==1&&fresh.pending==1);
    /* An old/mismatched OUTCOME must not cancel that separately proven range. */
    X4NackGraceControl outcome=control;outcome.kind=X4_NACK_GRACE_OUTCOME;
    outcome.sequence_epoch=2;outcome.reason=X4_NACK_GRACE_ATTEMPT;outcome.local_result=0;
    now(14200);CHECK(x4_pipeline_control(&outcome));x4_pipeline_batch(p);
    CHECK(x4_pipeline_snapshot(p).pending==1);
    for(unsigned variant=0;variant<2;++variant){
        X4NackGraceControl invalid=control;invalid.identity=(uint64_t(5)<<32)|(3+variant);
        if(variant==0)invalid.deadline_us++;else{invalid.detected_us=20000;invalid.deadline_us=65000;}
        now(14300+variant);CHECK(x4_pipeline_control(&invalid));x4_pipeline_batch(p);
        CHECK(x4_pipeline_snapshot(p).pending==1);
        auto offers=events(31,3,invalid.identity);CHECK(offers.size()==1&&(offers[0].c&UINT32_MAX)==X4_GRACE_OFFER_INVALID);
    }
    auto offers=events(31,3,(uint64_t(5)<<32)|1);CHECK(offers.size()==2);
    CHECK((offers[0].c&UINT32_MAX)==X4_GRACE_OFFER_STALE_SOURCE);
    CHECK((offers[1].c&UINT32_MAX)==X4_GRACE_OFFER_DUPLICATE);
    CHECK(!x4_pipeline_snapshot(p).event_omitted&&g->close()==0);x4_pipeline_close(p);++cases;
    std::puts("PASS actual inbox/worker: consumed unmapped ID burned; late real STORED anchor cannot renew duplicate; foreign outcome/invalid clocks cannot cancel current range");
}
#endif
#ifdef X4_PIPELINE_BASELINE
void exact_baseline_wrap_gate_reproduces_five_stale_offers(bool dirtyByPoll)
{
    resetClock();nackCount=0;nackOmitted=0;holdEvent=0;entered=false;releaseGate=false;
    X4Pipeline *p=x4_pipeline_create(true,5);
    auto g=std::make_shared<rtc::X4ReceiverNack>(true,5);g->acceptedFeedback(true);
    receive(p,g,packet(65534,90000,true,{24,0,2,0x67,0x11,0,2,0x68,0x22,0,2,0x65,0x33}),10000,10100);
    receive(p,g,packet(65535),10300,10400);receive(p,g,packet(0),10500,10600);
    now(10700);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).expected==1);
    auto two=packet(2);now(11000);holdEvent=19;std::thread observer([&]{incoming(g,two);});
    auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<limit)std::this_thread::yield();
    CHECK(entered.load());auto four=packet(4);now(11001);incoming(g,four);
    releaseGate.store(true,std::memory_order_release);observer.join();holdEvent=0;
    deliver(p,two,11100);deliver(p,four,11101);now(11110);x4_pipeline_batch(p);
    if(dirtyByPoll){now(11200);CHECK(g->poll([](rtc::message_ptr){fail("retired baseline watch",__LINE__);return true;})==0);}
    receive(p,g,packet(6),12000,12100);now(12200);x4_pipeline_batch(p);
    for(uint16_t seq:{uint16_t(1),uint16_t(3),uint16_t(5)})receive(p,g,packet(seq),12300+2*seq,12301+2*seq);
    now(12500);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).expected==7);
    const uint64_t priorAccepted=x4_pipeline_snapshot(p).accepted;
    CHECK(priorAccepted==(dirtyByPoll?0u:1u)); // Old policy accepts packet1 unless poll canceled its old range.
    for(uint16_t seq:{uint16_t(8),uint16_t(10),uint16_t(12),uint16_t(14),uint16_t(16)})
        receive(p,g,packet(seq),13000+2*seq,13001+2*seq);
    now(13200);x4_pipeline_batch(p);auto rejected=x4_pipeline_snapshot(p);
    CHECK(rejected.epoch==1&&rejected.stale_source==5&&!rejected.pending);
    for(uint16_t seq=17;seq<=50;++seq)receive(p,g,packet(seq),14000+2*seq,14001+2*seq);
    now(16000);x4_pipeline_batch(p);CHECK(x4_pipeline_snapshot(p).expected==51);
    auto seven=packet(7);receive(p,g,seven,20000,20100);
    CHECK(seven->x4NackIdentity==((uint64_t(5)<<32)|2)&&seven->x4NackExtended==7);
    now(25000);x4_pipeline_batch(p);auto after=x4_pipeline_snapshot(p);
    CHECK(after.epoch==1&&after.stale_source==5&&after.accepted==priorAccepted&&!after.pending&&!after.event_omitted);
    auto stored=events(31,7,(uint64_t(5)<<32)|2);CHECK(stored.size()==1&&stored[0].flags==x4_pipeline_reorder_code(true));
    CHECK(g->close()==0&&counter(100)==1&&counter(35)==1&&counter(14)==6&&counter(4)==0);
    CHECK(!nackOmitted.load());x4_pipeline_close(p);++cases;
    std::printf("PASS exact 0.7.34 sources: wrap+gate dirty=%s observer rebase; worker epoch1 rejects five epoch2 offers\n",dirtyByPoll?"poll":"incoming");
}
#endif
}
extern "C" uint64_t x4_pipeline_clock(void){return clockNow.load();}
extern "C" void x4_pipeline_set_clock(uint64_t value){now(value);}
extern "C" bool x4_rtc_diag_startup_active(uint64_t) { return false; }
extern "C" uint64_t x4_rtc_diag_startup_release_count(void) { return 0; }
extern "C" void x4_rtc_diag_startup_range(unsigned, uint64_t, uint64_t, uint64_t) {}
extern "C" int x4_rtc_diag_rtt_p50_ms(void) { return 26; }
extern "C" void x4_rtc_diag_first_return_publish(uint32_t, unsigned) {}
extern "C" uint64_t x4_rtc_receive_clock(void){return clockNow.load();}
extern "C" void x4_native_random_or_exit(void *p,size_t n){std::memset(p,0x5a,n);}
extern "C" void x4_rtc_nack_event(unsigned event,unsigned flags,uint64_t id,uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{
    unsigned at=nackCount.fetch_add(1);if(at<nackEvents.size())nackEvents[at]={event,flags,id,source,a,b,c};else ++nackOmitted;
    if(event==holdEvent.load()){entered.store(true,std::memory_order_release);while(!releaseGate.load(std::memory_order_acquire))std::this_thread::yield();}
}
int main(int argc,char **argv)
{
    bool budgetOnly=argc==2&&!std::strcmp(argv[1],"--budget-only");
    if(argc!=1&&!budgetOnly){std::fputs("Usage: pipeline [--budget-only]\n",stderr);return 2;}
#ifdef X4_PIPELINE_BASELINE
    if(budgetOnly){std::fputs("Budget fixture unavailable for historical legacy sources without DIRECT\n",stderr);return 2;}
    exact_baseline_wrap_gate_reproduces_five_stale_offers(false);
    exact_baseline_wrap_gate_reproduces_five_stale_offers(true);
#else
    if(budgetOnly){
        for(unsigned kind:{X4_PIPELINE_DIRECT_HOOK_TIME,X4_PIPELINE_DIRECT_HOOK_DECODE,X4_PIPELINE_DIRECT_HOOK_STOP})
            direct_fresh_budget_rejection_keeps_resident_ownership(kind);
        std::printf("PASS production DIRECT budget fixture: %u cases, %llu checks; host scheduling doubles only\n",cases,static_cast<unsigned long long>(checks));
        return 0;
    }
    legacy_policy_proof_is_not_an_intentional_test_failure();
    for(bool poll:{false,true})for(bool grace:{false,true})wrap_gate_to_worker_AU(grace,poll);
    for(bool grace:{false,true})wrap_gate_to_worker_AU(grace,false,true);
    for(bool grace:{false,true}){
        timely_AUTH_queued_before_deadline_is_not_late_worker_utility(grace,false);
        timely_AUTH_queued_before_deadline_is_not_late_worker_utility(grace,true);
        timely_AUTH_queued_before_deadline_is_not_late_worker_utility(grace,false,true);
    }
    consumed_unmapped_request_cannot_rearm_after_late_real_anchor();
    foreign_track_and_stale_session_ticket_never_grant_worker_acceptance(false);
    foreign_track_and_stale_session_ticket_never_grant_worker_acceptance(true);
    mapped_stale_offer_consumes_ID_before_a_later_valid_binding();
    sparse_low_seven_bit_alias_keeps_exact_physical_ticket();
    full_sparse_expected_direct_is_timely_only_before_deadline(false);
    full_sparse_expected_direct_is_timely_only_before_deadline(true);
    actual_failed_full_admission_is_capacity_and_preserves_recovery();
    normal_and_inverse_returns_preserve_worker_order(false);
    normal_and_inverse_returns_preserve_worker_order(true);
    fresh_session_rejects_old_controls_and_provenance();
    for(unsigned kind:{X4_PIPELINE_DIRECT_HOOK_TIME,X4_PIPELINE_DIRECT_HOOK_DECODE,X4_PIPELINE_DIRECT_HOOK_STOP})
        direct_fresh_budget_rejection_keeps_resident_ownership(kind);
#endif
    std::printf("PASS production NACK pipeline: %u cases, %llu checks; local post-AUTH host scope\n",cases,static_cast<unsigned long long>(checks));
}
