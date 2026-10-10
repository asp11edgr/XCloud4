/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual production generator and pinned MediaHandler chain fixtures.
 * The validation tail, entropy and numeric bridge are explicit test doubles.
 * No actual RtcpReceivingSession, CAPI, SRTP, socket or console is measured. */
#include "rtc_receiver_nack.hpp"
#include "rtc_receive_diag.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

static thread_local bool countAllocation = false;
static thread_local uint64_t allocations = 0;
#if defined(__GNUC__) || defined(__clang__)
#define X4_TEST_NOINLINE __attribute__((noinline))
#else
#define X4_TEST_NOINLINE
#endif
// Keep the allocator instrumentation at a real call boundary. GCC otherwise
// inlines the replacement delete and misidentifies its malloc/free pair as a
// mismatched deallocation of the corresponding replacement new expression.
X4_TEST_NOINLINE void *operator new(size_t size) {
    if (countAllocation) ++allocations;
    if (void *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
X4_TEST_NOINLINE void *operator new[](size_t size) { return ::operator new(size); }
X4_TEST_NOINLINE void operator delete(void *value) noexcept { std::free(value); }
X4_TEST_NOINLINE void operator delete[](void *value) noexcept { std::free(value); }
X4_TEST_NOINLINE void operator delete(void *value, size_t) noexcept { std::free(value); }
X4_TEST_NOINLINE void operator delete[](void *value, size_t) noexcept { std::free(value); }

namespace {
struct Event { unsigned event, flags; uint64_t identity, source, a, b, c; };
std::array<Event, 2048> events{};
std::atomic<unsigned> eventCount{0}, eventOmissions{0};
std::atomic<uint64_t> testClock{10000};
std::atomic<uint32_t> entropySender{UINT32_C(0x5a5a5a5a)};
std::atomic<bool> blockRequestRecord{false}, requestRecordEntered{false}, releaseRequestRecord{false};
uint64_t checks = 0;
unsigned cases = 0;
void fail(const char *expression, int line) {
    std::fprintf(stderr, "FAIL receiver_nack_handler_test.cpp:%d: %s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) do { ++checks; if (!(expression)) fail(#expression, __LINE__); } while (0)
#define CASE(test) do { clearEvents(); test(); ++cases; std::printf("PASS %s\n", #test); } while (0)
void clearEvents() { eventCount.store(0); eventOmissions.store(0); testClock.store(10000); }
unsigned count(unsigned event) {
    unsigned result = 0, total = eventCount.load();
    for (unsigned i = 0; i < total && i < events.size(); ++i) if (events[i].event == event) ++result;
    return result;
}
Event last(unsigned event) {
    unsigned total = eventCount.load();
    for (unsigned i = total < events.size() ? total : unsigned(events.size()); i; --i)
        if (events[i - 1].event == event) return events[i - 1];
    fail("requested event exists", __LINE__); return {};
}
uint64_t counter(unsigned id) {
    unsigned total = eventCount.load();
    for (unsigned i = total < events.size() ? total : unsigned(events.size()); i; --i)
        if (events[i - 1].event == 29 && events[i - 1].a == id) return events[i - 1].b;
    fail("requested counter exists", __LINE__); return 0;
}
uint16_t read16(const uint8_t *value) { return uint16_t((uint16_t(value[0]) << 8) | value[1]); }
uint32_t read32(const uint8_t *value) {
    return (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) |
        (uint32_t(value[2]) << 8) | value[3];
}
void write16(uint8_t *value, uint16_t number) { value[0] = uint8_t(number >> 8); value[1] = uint8_t(number); }
void write32(uint8_t *value, uint32_t number) {
    value[0] = uint8_t(number >> 24); value[1] = uint8_t(number >> 16);
    value[2] = uint8_t(number >> 8); value[3] = uint8_t(number);
}
rtc::message_ptr rtp(uint16_t sequence, uint32_t source = 7, uint32_t flags = X4_RD_AUTHENTICATED) {
    auto message = rtc::make_message(13, rtc::Message::Binary);
    auto *p = reinterpret_cast<uint8_t *>(message->data());
    std::memset(p, 0, 13); p[0] = 0x80; p[1] = 102;
    write16(p + 2, sequence); write32(p + 4, 12345); write32(p + 8, source); p[12] = 1;
    message->x4DiagFlags = flags; return message;
}
std::shared_ptr<rtc::X4ReceiverNack> generator(bool enabled = true, uint32_t epoch = 5,
    bool accepted = true) {
    auto result = std::make_shared<rtc::X4ReceiverNack>(enabled, epoch);
    result->acceptedFeedback(accepted); return result;
}
void incoming(const std::shared_ptr<rtc::X4ReceiverNack> &value, const rtc::message_ptr &message) {
    rtc::message_vector packet{message};
    value->incoming(packet, [](rtc::message_ptr) { fail("incoming does not dispatch", __LINE__); });
}
std::vector<uint16_t> positions(const rtc::message_ptr &message) {
    CHECK(message && message->type == rtc::Message::Control);
    CHECK(message->size() >= 72);
    const auto *p = reinterpret_cast<const uint8_t *>(message->data()) + 56;
    CHECK(p[0] == 0x81 && p[1] == 205);
    unsigned length = read16(p + 2);
    CHECK(length >= 3 && (size_t(length) + 1) * 4 == message->size() - 56);
    unsigned parts = length - 2;
    CHECK(parts > 0 && parts <= 4);
    std::vector<uint16_t> result;
    for (unsigned i = 0; i < parts; ++i) {
        uint16_t pid = read16(p + 12 + 4 * i), mask = read16(p + 14 + 4 * i);
        result.push_back(pid);
        for (unsigned bit = 0; bit < 16; ++bit)
            if (mask & (1u << bit)) result.push_back(uint16_t(pid + bit + 1));
    }
    return result;
}
class AuthenticationTail final : public rtc::MediaHandler {
public:
    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
        ++runs;
        for (auto &message : messages) message->x4DiagFlags = X4_RD_AUTHENTICATED;
    }
    unsigned runs = 0;
};

void actual_chain_incoming_runs_last_first() {
    auto value = generator(); auto tail = std::make_shared<AuthenticationTail>(); value->setNext(tail);
    for (uint16_t sequence : {uint16_t(100), uint16_t(102)}) {
        rtc::message_vector packet{rtp(sequence, 7, 0)};
        value->incomingChain(packet, [](rtc::message_ptr) { fail("no receive send", __LINE__); });
    }
    CHECK(tail->runs == 2 && count(19) == 1);
    CHECK(value->next() == tail && !tail->next());
    CHECK(value->sameConfiguration(true, 5) && !value->sameConfiguration(false, 5));
    CHECK(!value->sameConfiguration(true, 6));
    value->duplicateInstall(); CHECK(value->close() == 0); CHECK(counter(105) == 1);
    clearEvents();
    auto backwards = generator(); auto before = std::make_shared<AuthenticationTail>(); before->setNext(backwards);
    for (uint16_t sequence : {uint16_t(100), uint16_t(102)}) {
        rtc::message_vector packet{rtp(sequence, 7, 0)};
        before->incomingChain(packet, [](rtc::message_ptr) { fail("no receive send", __LINE__); });
    }
    CHECK(before->runs == 2 && count(19) == 0); CHECK(backwards->close() == 0); CHECK(counter(102) == 2);
}
void authentication_and_full_rtp_validation_precede_state() {
    auto value = generator(); incoming(value, rtp(100));
    incoming(value, rtp(102, 7, 0));
    incoming(value, rtp(102, 7, X4_RD_AUTHENTICATED | X4_RD_RETRANSMISSION));
    incoming(value, rtp(102, 7, X4_RD_AUTHENTICATED | X4_RD_RTX_NORMALIZED));
    auto invalid = rtp(102); reinterpret_cast<uint8_t *>(invalid->data())[0] |= 0x10;
    incoming(value, invalid); CHECK(count(19) == 0);
    incoming(value, rtp(102)); CHECK(count(19) == 1);
    CHECK(value->close() == 0); CHECK(counter(101) == 1 && counter(102) == 3);
}
void incoming_observation_allocates_nothing_and_never_sends() {
    auto value = generator(); auto first = rtp(100), gap = rtp(102), returned = rtp(101);
    rtc::message_vector packet; packet.reserve(1);
    rtc::message_callback forbidden = [](rtc::message_ptr) { fail("incoming must not send", __LINE__); };
    allocations = 0;
    for (const auto &message : {first, gap, returned}) {
        packet.clear(); packet.push_back(message); countAllocation = true;
        value->incoming(packet, forbidden); countAllocation = false;
    }
    CHECK(allocations == 0 && count(19) == 1 && count(24) == 1);
    unsigned sends = 0; CHECK(value->poll([&](rtc::message_ptr) { ++sends; return true; }) == 0);
    CHECK(sends == 0 && last(28).a == 4 && value->close() == 0);
}
void cancellation_mask_is_a_selection_boundary() {
    auto value = generator(); incoming(value, rtp(100)); incoming(value, rtp(105));
    auto returned = rtp(102); incoming(value, returned); CHECK(returned->x4NackIdentity != 0);
    CHECK(returned->x4NackExtended == 102 && returned->x4NackRtpTimestamp == 12345);
    rtc::message_ptr sent; CHECK(value->poll([&](rtc::message_ptr message) { sent = message; return true; }) == 1);
    CHECK(positions(sent) == std::vector<uint16_t>({101, 103, 104}));
    CHECK(last(30).b == 13 && last(30).c <= sent->x4NackDispatch);
    CHECK(count(20) == 1 && count(23) == 1 && last(23).a == 1);
    CHECK(value->poll([](rtc::message_ptr) { fail("no retry", __LINE__); return true; }) == 0);
    CHECK(value->close() == 0);
}
void advertised_or_disabled_feedback_does_not_send() {
    auto value = generator(true, 5, false); incoming(value, rtp(100)); incoming(value, rtp(102));
    CHECK(count(19) == 0 && last(28).a == 6);
    CHECK(value->poll([](rtc::message_ptr) { fail("feedback unaccepted", __LINE__); return true; }) == 0);
    value->acceptedFeedback(true); incoming(value, rtp(104)); CHECK(count(19) == 1);
    rtc::message_ptr sent; CHECK(value->poll([&](rtc::message_ptr message) { sent = message; return true; }) == 1);
    CHECK(positions(sent) == std::vector<uint16_t>({103})); CHECK(value->close() == 0);
    clearEvents(); auto disabled = generator(false); incoming(disabled, rtp(100)); incoming(disabled, rtp(102));
    CHECK(count(19) == 0 && last(28).a == 1);
    CHECK(disabled->poll([](rtc::message_ptr) { fail("OFF sends nothing", __LINE__); return true; }) == 0);
    CHECK(disabled->close() == 0);
}
void answer_check_provenance_is_explicit() {
    auto value = std::make_shared<rtc::X4ReceiverNack>(true, 5);
    CHECK(count(27) == 0); // Construction alone cannot establish Answer acceptance.
    value->acceptedFeedback(false);
    CHECK(last(27).flags == (1u | 4u | 16u) && last(27).a == 1);
    value->acceptedFeedback(true);
    CHECK(last(27).flags == (1u | 2u | 4u | 16u));
    value->acceptedFeedback(false);
    CHECK((last(27).flags & 16u) != 0 && (last(27).flags & 2u) == 0);
    CHECK(value->close() == 0 && last(27).flags == 8u);
}
void exact_generic_nack_feedback_does_not_accept_pli_alone() {
    CHECK(!rtc::x4ReceiverNackAcceptsNack("H264", {}));
    CHECK(!rtc::x4ReceiverNackAcceptsNack("H264", {"nack pli"}));
    CHECK(!rtc::x4ReceiverNackAcceptsNack("H264", {"x-nack", "ccm fir"}));
    CHECK(!rtc::x4ReceiverNackAcceptsNack("OPUS", {"nack"}));
    CHECK(rtc::x4ReceiverNackAcceptsNack("H264", {"nack"}));
    CHECK(rtc::x4ReceiverNackAcceptsNack("H264", {"nack pli", "nack", "ccm fir"}));
}
void sender_collision_never_produces_zero_feedback_ssrc() {
    entropySender.store(UINT32_C(0x80000000)); auto value = generator();
    entropySender.store(UINT32_C(0x5a5a5a5a));
    incoming(value, rtp(100, UINT32_C(0x80000000)));
    incoming(value, rtp(102, UINT32_C(0x80000000)));
    rtc::message_ptr sent; CHECK(value->poll([&](rtc::message_ptr message) { sent = message; return true; }) == 1);
    CHECK(sent && sent->size() >= 72);
    const auto *p = reinterpret_cast<const uint8_t *>(sent->data());
    CHECK(read32(p + 4) == 1 && read32(p + 12) == 1 && read32(p + 60) == 1);
    CHECK(read32(p + 64) == UINT32_C(0x80000000)); CHECK(value->close() == 0);
}
void sixty_four_attempts_are_the_whole_session_budget() {
    auto value = generator(); incoming(value, rtp(100)); unsigned sends = 0;
    for (unsigned gap = 1; gap <= 65; ++gap) {
        incoming(value, rtp(uint16_t(100 + 2 * gap)));
        int result = value->poll([&](rtc::message_ptr) { ++sends; return true; });
        CHECK(result == (gap <= 64 ? 1 : 0));
    }
    CHECK(sends == 64 && count(19) == 64 && count(20) == 64 && last(28).a == 3);
    CHECK(value->close() == 0 && counter(14) == 64 && counter(19) == 1);
    CHECK(eventOmissions.load() == 0);
}
void failure_is_local_and_does_not_authorize_retry() {
    auto value = generator(); incoming(value, rtp(100)); incoming(value, rtp(102)); unsigned sends = 0;
    CHECK(value->poll([&](rtc::message_ptr) { ++sends; return false; }) == -1);
    CHECK(last(23).a == 0 && last(23).flags == 0);
    CHECK(value->poll([&](rtc::message_ptr) { ++sends; return true; }) == 0);
    CHECK(sends == 1 && count(21) == 0 && count(22) == 0); // Test send double does not protect.
    CHECK(value->close() == 0);
}
void wrap_and_partial_return_keep_extended_identity() {
    auto value = generator(); incoming(value, rtp(65534)); incoming(value, rtp(2));
    auto returned = rtp(0); incoming(value, returned); CHECK(returned->x4NackExtended == 65536);
    rtc::message_ptr sent; CHECK(value->poll([&](rtc::message_ptr message) { sent = message; return true; }) == 1);
    CHECK(positions(sent) == std::vector<uint16_t>({65535, 1}));
    CHECK(sent->x4NackFirst == 65535 && sent->x4NackLast == 65537);
    CHECK(value->close() == 0);
}
void source_change_retires_old_queued_range() {
    auto value = generator(); incoming(value, rtp(100, 7)); incoming(value, rtp(102, 7));
    incoming(value, rtp(500, 8)); incoming(value, rtp(502, 8));
    unsigned sends = 0; CHECK(value->poll([&](rtc::message_ptr) { ++sends; return true; }) == 0);
    CHECK(last(28).a == 5);
    CHECK(value->poll([&](rtc::message_ptr message) {
        ++sends; const auto *p = reinterpret_cast<const uint8_t *>(message->data());
        CHECK(read32(p + 64) == 8); CHECK(positions(message) == std::vector<uint16_t>({501})); return true;
    }) == 1);
    CHECK(sends == 1 && value->close() == 0 && counter(4) == 1);
}
void pending_close_prevents_any_future_send() {
    auto value = generator(); incoming(value, rtp(100)); incoming(value, rtp(102));
    CHECK(value->close() == 0 && counter(106) == 1 && last(27).flags == 8);
    CHECK(!value->sameConfiguration(true, 5)); incoming(value, rtp(104));
    CHECK(value->poll([](rtc::message_ptr) { fail("no send after successful close", __LINE__); return true; }) == 0);
    CHECK(value->close() == 0);
}
void idle_poll_avoids_gate_before_publish_and_after_consume_or_cancel() {
    auto value = generator(); incoming(value, rtp(100)); unsigned sends = 0;
    for (uint16_t sequence : {uint16_t(102), uint16_t(104), uint16_t(108)}) {
        if (sequence == 108) {
            incoming(value, rtp(106)); incoming(value, rtp(105));
            CHECK(value->poll([](rtc::message_ptr) { fail("complete range cannot send", __LINE__); return true; }) == 0);
            CHECK(last(28).a == 4);
        }
        requestRecordEntered.store(false); releaseRequestRecord.store(false); blockRequestRecord.store(true);
        std::thread observer([&] { incoming(value, rtp(sequence)); });
        auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!requestRecordEntered.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() < limit) std::this_thread::yield();
        CHECK(requestRecordEntered.load(std::memory_order_acquire));
        // REQUEST is recorded but its slot is not yet published as queued.
        // An idle dispatcher must not contend with that held receive gate.
        for (unsigned probe = 0; probe < 100; ++probe)
            CHECK(value->poll([](rtc::message_ptr) { fail("unpublished request cannot send", __LINE__); return true; }) == 0);
        releaseRequestRecord.store(true, std::memory_order_release); observer.join(); blockRequestRecord.store(false);
        CHECK(value->poll([&](rtc::message_ptr message) {
            ++sends; CHECK(positions(message) == std::vector<uint16_t>({uint16_t(sequence - 1)})); return true;
        }) == 1);
    }
    CHECK(sends == 3 && value->close() == 0);
    CHECK(counter(104) == 0 && counter(100) == 0 && counter(35) == 0);
    CHECK(counter(14) == 4); // Cancellation consumes a queued slot, not the attempt budget.
}
void try_gate_omission_retires_continuity_without_a_false_gap() {
    auto value = generator(); incoming(value, rtp(100));
    requestRecordEntered.store(false); releaseRequestRecord.store(false); blockRequestRecord.store(true);
    // Test-only numeric bridge latch holds the first observer inside its gate.
    // Production bridge never contains this latch or a waiting operation.
    std::thread observer([&] { incoming(value, rtp(102)); });
    auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!requestRecordEntered.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < limit) std::this_thread::yield();
    CHECK(requestRecordEntered.load(std::memory_order_acquire));
    incoming(value, rtp(104)); // Omitted observation cannot wait for the held gate.
    releaseRequestRecord.store(true, std::memory_order_release); observer.join();
    blockRequestRecord.store(false); incoming(value, rtp(106));
    CHECK(count(19) == 1); // 103..105 are not manufactured as an upstream gap.
    CHECK(value->poll([](rtc::message_ptr) { fail("retired range must not send", __LINE__); return true; }) == 0);
    CHECK(last(28).a == 5);
    incoming(value, rtp(108)); rtc::message_ptr sent;
    CHECK(value->poll([&](rtc::message_ptr message) { sent = message; return true; }) == 1);
    CHECK(positions(sent) == std::vector<uint16_t>({107}));
    CHECK(sent->x4NackIdentity == ((UINT64_C(5) << 32) | 2)); // Attempt budget survives resync.
    CHECK(value->close() == 0 && counter(100) == 1 && counter(35) == 1);
}
void close_retries_while_the_owner_has_an_active_send() {
    auto value = generator(); incoming(value, rtp(100)); incoming(value, rtp(102));
    std::atomic<bool> entered{false}, released{false}; std::atomic<int> result{-10};
    std::thread sender([&] {
        result.store(value->poll([&](rtc::message_ptr) {
            entered.store(true, std::memory_order_release);
            while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
            return true;
        }));
    });
    auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < limit)
        std::this_thread::yield();
    CHECK(entered.load(std::memory_order_acquire)); CHECK(value->close() == -1);
    released.store(true, std::memory_order_release); sender.join();
    CHECK(result.load() == 1 && value->close() == 0);
    CHECK(value->poll([](rtc::message_ptr) { fail("closed owner cannot send", __LINE__); return true; }) == 0);
    CHECK(count(20) == 1 && count(23) == 1);
}
void new_session_has_new_identity_and_budget() {
    auto old = generator(true, 5); incoming(old, rtp(100)); incoming(old, rtp(102)); CHECK(old->close() == 0);
    auto fresh = generator(true, 6); incoming(fresh, rtp(100)); incoming(fresh, rtp(102));
    rtc::message_ptr sent; CHECK(fresh->poll([&](rtc::message_ptr message) { sent = message; return true; }) == 1);
    CHECK(sent->x4NackIdentity == ((UINT64_C(6) << 32) | 1));
    CHECK(fresh->close() == 0 && counter(14) == 1);
}
}

extern "C" void x4_native_random_or_exit(void *target, size_t size) {
    std::memset(target, 0x5a, size); // Test-only deterministic entropy, never linked into runtime.
    if (size == sizeof(uint32_t)) {
        uint32_t sender = entropySender.load(); std::memcpy(target, &sender, sizeof(sender));
    }
}
extern "C" uint64_t x4_rtc_receive_clock(void) { return testClock.fetch_add(1); }
extern "C" void x4_rtc_nack_event(unsigned event, unsigned flags, uint64_t identity,
    uint64_t source, uint64_t a, uint64_t b, uint64_t c) {
    unsigned slot = eventCount.fetch_add(1);
    if (slot < events.size()) events[slot] = {event, flags, identity, source, a, b, c};
    else eventOmissions.fetch_add(1);
    if (event == 19 && blockRequestRecord.load(std::memory_order_acquire)) {
        requestRecordEntered.store(true, std::memory_order_release);
        while (!releaseRequestRecord.load(std::memory_order_acquire)) std::this_thread::yield();
    }
}

int main() {
    CASE(actual_chain_incoming_runs_last_first);
    CASE(authentication_and_full_rtp_validation_precede_state);
    CASE(incoming_observation_allocates_nothing_and_never_sends);
    CASE(cancellation_mask_is_a_selection_boundary);
    CASE(advertised_or_disabled_feedback_does_not_send);
    CASE(answer_check_provenance_is_explicit);
    CASE(exact_generic_nack_feedback_does_not_accept_pli_alone);
    CASE(sender_collision_never_produces_zero_feedback_ssrc);
    CASE(sixty_four_attempts_are_the_whole_session_budget);
    CASE(failure_is_local_and_does_not_authorize_retry);
    CASE(wrap_and_partial_return_keep_extended_identity);
    CASE(source_change_retires_old_queued_range);
    CASE(pending_close_prevents_any_future_send);
    CASE(idle_poll_avoids_gate_before_publish_and_after_consume_or_cancel);
    CASE(try_gate_omission_retires_continuity_without_a_false_gap);
    CASE(close_retries_while_the_owner_has_an_active_send);
    CASE(new_session_has_new_identity_and_budget);
    std::printf("PASS receiver NACK production handler: %u cases, %llu checks\n", cases,
        static_cast<unsigned long long>(checks));
    return 0;
}
