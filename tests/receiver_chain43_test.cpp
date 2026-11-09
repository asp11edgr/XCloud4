/* SPDX-License-Identifier: GPL-3.0-only */
/* Real pinned MediaHandler and RtcpReceivingSession, own production NACK and
 * recovery bridge. Only entropy/log/clock/control I/O are host doubles. */
#include "rtc_receiver_nack.hpp"
#include "rtc_receive_diag.h"
#include "recovery43_bridge.h"
#include <rtc/rtcpreceivingsession.hpp>
#include "impl/logcounter.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace {
std::atomic<uint64_t> clock_us{100000};
std::atomic<uint64_t> gap_events{0}, return_events{0}, controls{0};
std::atomic<bool> lease_block{false}, lease_entered{false}, lease_release{false}, end_waiting{false};
std::atomic<bool> receive_block{false}, receive_entered{false}, receive_release{false};
uint64_t checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL chain43:%d %s\n", \
    __LINE__, #x); std::abort(); } } while (0)
uint16_t read16(const uint8_t *p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
uint32_t read32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
    uint32_t(p[2]) << 8 | p[3]; }
void write16(uint8_t *p, uint16_t n) { p[0] = uint8_t(n >> 8); p[1] = uint8_t(n); }
void write32(uint8_t *p, uint32_t n) { p[0] = uint8_t(n >> 24); p[1] = uint8_t(n >> 16);
    p[2] = uint8_t(n >> 8); p[3] = uint8_t(n); }
rtc::message_ptr packet(uint16_t sequence, bool rtx = false, uint16_t original = 0) {
    const size_t bytes = rtx ? 16 : 14;
    auto result = rtc::make_message(bytes, rtc::Message::Binary);
    auto *p = reinterpret_cast<uint8_t *>(result->data());
    std::memset(p, 0, bytes); p[0] = 0x80; p[1] = rtx ? 103 : 102;
    write16(p + 2, sequence); write32(p + 4, 1000); write32(p + 8, rtx ? 8 : 7);
    if (rtx) { write16(p + 12, original); p[14] = 0x65; p[15] = 0x80; }
    else { p[12] = 0x65; p[13] = 0x80; }
    result->stream = rtx ? 8 : 7; result->x4DiagFlags = X4_RD_AUTHENTICATED;
    result->x4DiagTime = 777;
    return result;
}
class Receiving final : public rtc::RtcpReceivingSession {
public:
    uint32_t received() const { return mReceived; }
    uint32_t ssrc() const { return mSsrc; }
};
struct Chain {
    uint32_t epoch;
    std::shared_ptr<rtc::X4ReceiverNack> nack;
    std::shared_ptr<Receiving> receiving;
    Chain(uint32_t e, unsigned mode, unsigned loss) : epoch(e) {
        CHECK(x4_recovery43_begin(e, mode, loss, 0x51432) == 0);
        X4VideoRepair43Config config{};
        config.media_ssrc = 7; config.rtx_ssrc = 8; config.fec_ssrc = 9;
        config.media_pt = 102; config.rtx_pt = 103; config.fec_pt = 49;
        config.rtx_accepted = mode >= 1; config.fec_accepted = mode >= 2;
        CHECK(x4_recovery43_answer(e, &config) == 0);
        nack = std::make_shared<rtc::X4ReceiverNack>(true, e);
        nack->acceptedFeedback(true);
        receiving = std::make_shared<Receiving>(); nack->setNext(receiving);
        rtc::Description::Video description("video", rtc::Description::Direction::RecvOnly);
        description.addH264Codec(102);
        nack->mediaChain(description);
        /* Repeated media negotiation must never append a second owner. */
        const auto repair = receiving->next(); nack->mediaChain(description);
        CHECK(receiving->next() == repair);
        CHECK(bool(repair)); // Solo NACK 0% has an observation-only tail.
        CHECK(!repair || !repair->next());
    }
    ~Chain() { x4_recovery43_end(epoch); CHECK(nack->close() == 0); }
    rtc::message_vector input(const rtc::message_ptr &message) {
        rtc::message_vector messages{message};
        nack->incomingChain(messages, [](rtc::message_ptr) {});
        return messages;
    }
};
void default_is_unchanged() {
    Chain chain(401, 0, 0);
    auto first = packet(100);
    const auto copy = *first;
    auto messages = chain.input(first);
    CHECK(messages.size() == 1 && messages[0] == first);
    CHECK(messages[0]->size() == copy.size());
    CHECK(std::memcmp(messages[0]->data(), copy.data(), copy.size()) == 0);
    CHECK(chain.receiving->next());
    CHECK(chain.input(packet(102)).size() == 1);
    auto retransmission = packet(101);
    CHECK(chain.input(retransmission).front() == retransmission);
    CHECK(chain.input(retransmission).front() == retransmission); // Baseline duplicate is not dropped.
    CHECK(chain.receiving->received() == 4);
    auto metrics = std::make_unique<X4Recovery43Snapshot>();
    CHECK(x4_recovery43_snapshot(401, metrics.get()));
    CHECK(metrics->repair.gaps_detected == 1 && metrics->repair.gaps_closed[0] == 1);
    CHECK(metrics->repair.duplicate[0] == 1);
    std::puts("PASS Solo NACK passive metrics preserve original callbacks, duplicates and wire bytes");
}
void failed_mapping_cannot_leak_parity() {
    CHECK(x4_recovery43_begin(406, 2, 0, 77) == 0);
    X4VideoRepair43Config invalid{}; // No media SSRC: engine refuses safely.
    CHECK(x4_recovery43_answer(406, &invalid) == -1);
    auto nack = std::make_shared<rtc::X4ReceiverNack>(true, 406);
    auto receiving = std::make_shared<Receiving>(); nack->setNext(receiving);
    rtc::Description::Video description("video", rtc::Description::Direction::RecvOnly);
    description.addH264Codec(102); nack->mediaChain(description);
    rtc::message_vector messages{packet(1), packet(3, true, 2)};
    auto fec = packet(4); reinterpret_cast<uint8_t *>(fec->data())[1] = 49;
    messages.push_back(fec);
    nack->incomingChain(messages, [](rtc::message_ptr) {});
    CHECK(messages.size() == 1 && receiving->received() == 1 && receiving->ssrc() == 7);
    CHECK((uint8_t((*messages[0])[1]) & 127u) == 102);
    x4_recovery43_end(406); CHECK(nack->close() == 0);
    std::puts("PASS unavailable repair mapping keeps ordinary video and never leaks raw RTX/FEC");
}
void rtx_is_before_rr_and_nack() {
    Chain chain(402, 1, 0);
    const auto gaps_before = gap_events.load(), returns_before = return_events.load();
    CHECK(chain.input(packet(100)).size() == 1);
    CHECK(chain.input(packet(102)).size() == 1);
    CHECK(gap_events.load() == gaps_before + 1);
    auto restored = chain.input(packet(600, true, 101));
    CHECK(restored.size() == 1);
    const auto &m = *restored[0];
    const auto *p = reinterpret_cast<const uint8_t *>(m.data());
    CHECK(m.size() == 14 && read16(p + 2) == 101 && read32(p + 8) == 7 && p[1] == 102);
    CHECK(m.stream == 7 && m.x4DiagTime >= 777);
    CHECK((m.x4DiagFlags & (X4_RD_AUTHENTICATED | X4_RD_RECOVERY43 | X4_RD_RTX_NORMALIZED)) ==
        (X4_RD_AUTHENTICATED | X4_RD_RECOVERY43 | X4_RD_RTX_NORMALIZED));
    CHECK(m.x4NackIdentity != 0 && m.x4NackAuthenticated != 0);
    CHECK(return_events.load() == returns_before + 1);
    CHECK(chain.receiving->ssrc() == 7 && chain.receiving->received() == 3);
    x4_recovery43_delivery(402, 101, 1000, X4_RECOVERY43_DELIVERY_LATE);
    x4_recovery43_delivery(402, 101, 1000, X4_RECOVERY43_DELIVERY_DUPLICATE);
    x4_recovery43_delivery(402, 101, 1001, X4_RECOVERY43_DELIVERY_LATE);
    auto admission = std::make_unique<X4Recovery43Snapshot>();
    CHECK(x4_recovery43_snapshot(402, admission.get()));
    CHECK(admission->delivery_late[1] == 1 && admission->delivery_duplicate[1] == 1);
    CHECK(admission->delivery_unknown == 1 && admission->delivery_late[0] == 0);
    CHECK(chain.nack->poll([](rtc::message_ptr) { std::abort(); return false; }) == 0);
    CHECK(chain.input(packet(601, true, 101)).empty());
    CHECK(chain.receiving->received() == 3);
    auto unauth = packet(103); unauth->x4DiagFlags = 0;
    CHECK(chain.input(unauth).empty() && chain.receiving->received() == 3);
    std::puts("PASS RTX normalized before real RR counters and closes original NACK/grace provenance");
}
void loss_is_before_rr_and_nack() {
    Chain chain(403, 0, 3);
    uint64_t outputs = 0;
    for (unsigned i = 1; i <= 4000; ++i) outputs += chain.input(packet(uint16_t(i))).size();
    auto snapshot = std::make_unique<X4Recovery43Snapshot>();
    CHECK(x4_recovery43_snapshot(403, snapshot.get()));
    CHECK(snapshot->repair.lost_simulated[0] > 0);
    CHECK(outputs + snapshot->repair.lost_simulated[0] == 4000);
    CHECK(chain.receiving->received() == outputs);
    CHECK(snapshot->repair.emitted[0] == outputs);
    CHECK(snapshot->receive_contention == 0);
    std::puts("PASS simulated loss removed before real ReceivingSession received count and NACK");
}
void fec_is_before_rr_and_nack() {
    Chain chain(405, 2, 0);
    CHECK(chain.input(packet(100)).size() == 1);
    CHECK(chain.input(packet(102)).size() == 1);
    /* Independent format fixture: one protected SSRC, short flexible mask,
     * a one-packet repair for missing sequence 101 and its two payload bytes. */
    auto parity = rtc::make_message(34, rtc::Message::Binary);
    auto *p = reinterpret_cast<uint8_t *>(parity->data());
    std::memset(p, 0, parity->size()); p[0] = 0x80; p[1] = 49;
    write16(p + 2, 600); write32(p + 4, 1000); write32(p + 8, 9);
    p[13] = 102; write16(p + 14, 2); write32(p + 16, 1000);
    p[20] = 1; write32(p + 24, 7); write16(p + 28, 101); p[30] = 0xc0;
    p[32] = 0x65; p[33] = 0x80;
    parity->stream = 9; parity->x4DiagFlags = X4_RD_AUTHENTICATED;
    auto output = chain.input(parity);
    CHECK(output.size() == 1 && output[0]->size() == 14);
    CHECK(output[0]->stream == 7 && output[0]->x4NackIdentity != 0);
    CHECK((output[0]->x4DiagFlags & (X4_RD_AUTHENTICATED | X4_RD_RECOVERY43 | X4_RD_FEC_RECOVERED)) ==
        (X4_RD_AUTHENTICATED | X4_RD_RECOVERY43 | X4_RD_FEC_RECOVERED));
    CHECK(chain.receiving->received() == 3 && chain.receiving->ssrc() == 7);
    CHECK(chain.input(parity).empty() && chain.receiving->received() == 3);
    CHECK(chain.nack->poll([](rtc::message_ptr) { std::abort(); return false; }) == 0);
    std::puts("PASS FEC parity never reaches real RR; repair closes original NACK/grace");
}
void incoming_poll_and_end_lifetime() {
    Chain chain(404, 1, 1);
    std::atomic<bool> started{false}, finish{false};
    std::thread receive([&] {
        started.store(true, std::memory_order_release);
        for (unsigned i = 1; i <= 20000; ++i) (void)chain.input(packet(uint16_t(i)));
        finish.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    auto snapshot = std::make_unique<X4Recovery43Snapshot>();
    unsigned polls = 0;
    while (!finish.load(std::memory_order_acquire)) {
        (void)x4_recovery43_snapshot(404, snapshot.get());
        x4_recovery43_poll(404, clock_us.fetch_add(500), false);
        (void)chain.nack->poll([](rtc::message_ptr) { return true; });
        if (++polls == 10) x4_recovery43_end(404);
    }
    receive.join(); x4_recovery43_end(404);
    CHECK(!x4_recovery43_snapshot(404, snapshot.get()));
    CHECK(chain.input(packet(30000)).empty());
    CHECK(chain.nack->close() == 0);
    std::puts("PASS concurrent incoming/poll/end, retained callbacks and safe final close");
}
void metadata_lease_holds_state_during_end() {
    /* No handler/snapshot shared owner: end's local owner is the only thing
     * preventing free while this real worker API is paused after pointer load. */
    CHECK(x4_recovery43_begin(407, 0, 0, 77) == 0);
    lease_entered.store(false); lease_release.store(false); end_waiting.store(false);
    lease_block.store(true, std::memory_order_release);
    std::atomic<bool> ended{false};
    std::thread reader([] { x4_recovery43_note_media(407, 1, 1, 0, 0); });
    while (!lease_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread close([&] { x4_recovery43_end(407); ended.store(true, std::memory_order_release); });
    while (!end_waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(!ended.load(std::memory_order_acquire));
    lease_release.store(true, std::memory_order_release);
    reader.join(); close.join(); lease_block.store(false, std::memory_order_release);
    CHECK(ended.load(std::memory_order_acquire));
    /* New reads after exchange see NULL and may safely do nothing. */
    x4_recovery43_note_media(407, 1, 1, 0, 0);
    x4_recovery43_delivery(407, 1, 1000, X4_RECOVERY43_DELIVERY_LATE);
    std::puts("PASS metadata reader lease delays State release through concurrent end");
}
void preempted_receiver_cannot_write_after_final() {
    Chain chain(408, 1, 0);
    receive_entered.store(false); receive_release.store(false);
    receive_block.store(true, std::memory_order_release);
    rtc::message_vector output;
    std::thread receiver([&] { output = chain.input(packet(1)); });
    while (!receive_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    // First closing check already ran; receive gate has not been acquired.
    x4_recovery43_end(408);
    receive_release.store(true, std::memory_order_release);
    receiver.join(); receive_block.store(false, std::memory_order_release);
    CHECK(output.empty());
    CHECK(chain.receiving->received() == 0);
    std::puts("PASS receiver preempted before gate cannot emit or update RR after end");
}
} // namespace
extern "C" void x4_native_random_or_exit(void *out, size_t bytes) { std::memset(out, 0x5a, bytes); }
extern "C" uint64_t x4_rtc_receive_clock(void) { return clock_us.fetch_add(1); }
extern "C" bool x4_rtc_diag_startup_active(uint64_t) { return false; }
extern "C" uint64_t x4_rtc_diag_startup_release_count(void) { return 0; }
extern "C" void x4_rtc_diag_startup_range(unsigned, uint64_t, uint64_t, uint64_t) {}
extern "C" int x4_rtc_diag_rtt_p50_ms(void) { return -1; }
extern "C" void x4_rtc_diag_first_return_publish(uint32_t, unsigned) {}
/* Diagnostic observations neither authenticate nor rewrite packet flags. The
 * real patched ReceivingSession is allowed to advance the boundary time. */
extern "C" uint64_t x4_rtc_receive_packet(unsigned, const void *, size_t,
    uint64_t, uint32_t) { return clock_us.fetch_add(1); }
extern "C" void x4_rtc_receive_setting(unsigned, uint64_t, uint64_t, uint64_t) {}
extern "C" void x4_rtc_receive_reject(unsigned, unsigned, int, const void *, size_t) {}
extern "C" void x4_rtc_nack_event(unsigned event, unsigned, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t) {
    if (event == 18) gap_events.fetch_add(1);
    if (event == 24) return_events.fetch_add(1);
}
extern "C" bool x4_rtc_nack_grace_control(const X4NackGraceControl *) {
    controls.fetch_add(1); return true;
}
extern "C" int x4_log_printf(const char *, ...) { return 0; }
extern "C" void x4_recovery43_test_reader_acquired(uint32_t epoch) {
    if (epoch != 407 || !lease_block.load(std::memory_order_acquire)) return;
    lease_entered.store(true, std::memory_order_release);
    while (!lease_release.load(std::memory_order_acquire)) std::this_thread::yield();
}
extern "C" void x4_recovery43_test_end_waiting(uint32_t epoch) {
    if (epoch == 407) end_waiting.store(true, std::memory_order_release);
}
extern "C" void x4_recovery43_test_receive_before_gate(uint32_t epoch) {
    if (epoch != 408 || !receive_block.load(std::memory_order_acquire)) return;
    receive_entered.store(true, std::memory_order_release);
    while (!receive_release.load(std::memory_order_acquire)) std::this_thread::yield();
}
/* Logging-only double: do not start the dependency's delayed-log ThreadPool.
 * Actual ReceivingSession validation, sequence/RR counters and chain execute. */
namespace rtc::impl {
LogCounter::LogCounter(plog::Severity, const std::string &, std::chrono::seconds) {}
LogCounter &LogCounter::operator++(int) { return *this; }
}
int main() {
    default_is_unchanged(); failed_mapping_cannot_leak_parity();
    rtx_is_before_rr_and_nack(); loss_is_before_rr_and_nack();
    fec_is_before_rr_and_nack();
    incoming_poll_and_end_lifetime();
    metadata_lease_holds_state_during_end();
    preempted_receiver_cannot_write_after_final();
    std::printf("PASS receiver_chain43 production chain: %llu checks\n", (unsigned long long)checks);
}
