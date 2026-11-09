/* SPDX-License-Identifier: GPL-3.0-only */
/* The pinned receiver chain and production REMB bridge are real. The CAPI
 * Track lookup and its protected transport callback are doubles. No socket,
 * SRTCP delivery or remote receipt is inferred from these tests. */
#include "rtc_receiver_nack.hpp"
#include "rtc_receive_diag.h"
#include <rtc/rtcpreceivingsession.hpp>
#include "impl/logcounter.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <thread>

extern "C" int x4_rtc_send_video_remb(int, uint32_t, uint32_t, uint32_t) noexcept;

namespace {
constexpr int VideoTrack = 42, AudioTrack = 43;
constexpr uint32_t Sender = 0x31323334, Video = 0x51525354, Audio = 0x61626364;
constexpr uint8_t VideoPt = 102;

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL REMB:%d %s\n", __LINE__, #expression); std::abort(); \
} } while (0)

enum class Result { Accept, Refuse, Throw, Hold };
struct Wire {
    std::shared_ptr<rtc::RtcpReceivingSession> receiver =
        std::make_shared<rtc::RtcpReceivingSession>();
    rtc::X4ReceiverNack handler{false, 1, false, false, true};
    std::atomic<Result> result{Result::Accept};
    std::atomic<bool> sendEntered{false}, releaseSend{false}, throwLookup{false}, skipDispatch{false};
    std::atomic<unsigned> attempts{0}, accepted{0};
    rtc::message_ptr packet;

    Wire() {
        handler.setNext(receiver);
        receiver->x4ConfigureVideoRr(RTC_X4_VIDEO_RR_EDGE, VideoPt);
    }

    bool dispatch(rtc::message_ptr message) {
        ++attempts;
        packet = std::move(message);
        const auto state = result.load();
        if (state == Result::Throw) throw std::runtime_error("simulated send");
        if (state == Result::Hold) {
            sendEntered.store(true, std::memory_order_release);
            while (!releaseSend.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        if (state == Result::Refuse) return false;
        ++accepted;
        return true;
    }
};
Wire *wire;

rtc::message_ptr videoRtp() {
    const std::byte bytes[] = {std::byte{0x80}, std::byte{VideoPt},
        std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
        std::byte{0x51}, std::byte{0x52}, std::byte{0x53}, std::byte{0x54}, std::byte{0x61}};
    auto packet = rtc::make_message(rtc::binary(std::begin(bytes), std::end(bytes)),
        rtc::Message::Binary, Video);
    packet->x4DiagFlags = X4_RD_AUTHENTICATED;
    return packet;
}

rtc::message_ptr senderReport(bool compound) {
    constexpr unsigned Bytes = 28;
    auto packet = rtc::make_message(compound ? 2 * Bytes : Bytes, rtc::Message::Control);
    auto *sr = reinterpret_cast<rtc::RtcpSr *>(packet->data());
    sr->preparePacket(compound ? Audio : Video, 0);
    if (compound) {
        sr = reinterpret_cast<rtc::RtcpSr *>(packet->data() + Bytes);
        sr->preparePacket(Video, 0);
    }
    packet->x4DiagFlags = X4_RD_AUTHENTICATED;
    return packet;
}

void receiveNoAutomaticRemb(Wire &test) {
    unsigned reports = 0;
    auto send = [&](rtc::message_ptr packet) {
        CHECK(packet && packet->type == rtc::Message::Control && rtc::IsRtcp(*packet));
        const auto *header = reinterpret_cast<const rtc::RtcpHeader *>(packet->data());
        CHECK(header->payloadType() == 201 && packet->x4VideoRr);
        const auto *rr = reinterpret_cast<const rtc::RtcpRr *>(packet->data());
        CHECK(rr->senderSSRC() == Video && rr->getReportBlock(0)->getSSRC() == Video);
        packet->x4VideoRrSent = true;
        ++reports;
    };
    rtc::message_vector media{videoRtp()};
    test.handler.incomingChain(media, send);
    CHECK(media.size() == 1);
    for (const bool compound : {false, true}) {
        rtc::message_vector control{senderReport(compound)};
        test.handler.incomingChain(control, send);
        CHECK(control.empty());
    }
    CHECK(reports == 2);
}

void checkWire(const rtc::message_ptr &packet, uint32_t bitrate, uint8_t packedHigh,
    uint8_t packedMid, uint8_t packedLow) {
    CHECK(packet && packet->type == rtc::Message::Control && rtc::IsRtcp(*packet));
    CHECK(!packet->x4NackIdentity && !packet->x4VideoRr);
    const uint8_t expected[] = {0x8f, 206, 0, 5, 0x31, 0x32, 0x33, 0x34,
        0, 0, 0, 0, 'R', 'E', 'M', 'B', 1, packedHigh, packedMid, packedLow,
        0x51, 0x52, 0x53, 0x54};
    CHECK(packet->size() == sizeof(expected));
    CHECK(std::memcmp(packet->data(), expected, sizeof(expected)) == 0);
    const auto *remb = reinterpret_cast<const rtc::RtcpRemb *>(packet->data());
    CHECK(remb->hasValidId() && remb->getSSRCCount() == 1);
    CHECK(remb->getSSRC(0) == Video && remb->getBitrate() == bitrate);
}

void singleCommandOwnership(Wire &test) {
    const unsigned prior = test.attempts.load();
    test.result.store(Result::Hold);
    int result = -99;
    std::thread sender([&] { result = x4_rtc_send_video_remb(VideoTrack, Sender, Video, 20000000); });
    while (!test.sendEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 10000000) == RTC_ERR_NOT_AVAIL);
    CHECK(test.handler.poll([](rtc::message_ptr) { std::abort(); return false; }) == 0);
    test.releaseSend.store(true, std::memory_order_release);
    sender.join();
    CHECK(result == 1 && test.attempts.load() == prior + 1);
    test.result.store(Result::Accept);
}
} // namespace

extern "C" int x4_rtc_poll_receiver_nack(int track) {
    if (wire->throwLookup.load()) throw std::runtime_error("simulated lookup");
    if (track != VideoTrack) return RTC_ERR_NOT_AVAIL;
    if (wire->skipDispatch.load()) return 1;
    return wire->handler.poll([](rtc::message_ptr packet) {
        return wire->dispatch(std::move(packet));
    });
}

extern "C" void x4_native_random_or_exit(void *data, size_t size) { std::memset(data, 1, size); }
extern "C" uint64_t x4_rtc_receive_clock() { return 20000; }
extern "C" uint64_t x4_rtc_receive_packet(unsigned, const void *, size_t, uint64_t at, uint32_t) {
    return at ? at : 10000;
}
extern "C" void x4_rtc_receive_setting(unsigned, uint64_t, uint64_t, uint64_t) {}
extern "C" void x4_rtc_receive_reject(unsigned, unsigned, int, const void *, size_t) {}
extern "C" void x4_rtc_nack_event(unsigned, unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
extern "C" bool x4_rtc_nack_grace_control(const X4NackGraceControl *) { return true; }
extern "C" bool x4_rtc_diag_startup_active(uint64_t) { return false; }
extern "C" uint64_t x4_rtc_diag_startup_release_count() { return 0; }
extern "C" void x4_rtc_diag_startup_range(unsigned, uint64_t, uint64_t, uint64_t) {}
extern "C" int x4_rtc_diag_rtt_p50_ms() { return 20; }
extern "C" void x4_rtc_diag_first_return_publish(uint32_t, unsigned) {}

namespace rtc::impl {
LogCounter::LogCounter(plog::Severity, const std::string &, std::chrono::seconds) {}
LogCounter &LogCounter::operator++(int) { return *this; }
}

int main() {
    Wire test;
    wire = &test;
    receiveNoAutomaticRemb(test);
    CHECK(test.attempts.load() == 0);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 0) == RTC_ERR_INVALID);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, 0, 20000000) == RTC_ERR_INVALID);
    CHECK(x4_rtc_send_video_remb(VideoTrack, 0, Video, 20000000) == RTC_ERR_INVALID);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Video, Video, 20000000) == RTC_ERR_INVALID);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 30000001) == RTC_ERR_INVALID);
    CHECK(x4_rtc_send_video_remb(AudioTrack, Sender, Video, 20000000) == RTC_ERR_NOT_AVAIL);
    CHECK(test.attempts.load() == 0);
    std::puts("PASS No: video and compound SR produce RR only; invalid/other-track requests do not send");

    struct Vector { uint32_t bitrate; uint8_t high, mid, low; };
    constexpr Vector values[] = {{3000000, 0x12, 0xdc, 0x6c}, {10000000, 0x1a, 0x62, 0x5a},
        {12000000, 0x1a, 0xdc, 0x6c}, {15000000, 0x1b, 0x93, 0x87},
        {20000000, 0x1e, 0x62, 0x5a}, {30000000, 0x1f, 0x93, 0x87}};
    for (const auto &value : values) {
        CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, value.bitrate) == 1);
        checkWire(test.packet, value.bitrate, value.high, value.mid, value.low);
    }
    CHECK(test.attempts.load() == 6 && test.accepted.load() == 6);
    receiveNoAutomaticRemb(test);
    CHECK(test.attempts.load() == 6);
    std::puts("PASS six exact packet vectors through the protected-send callback; no SR repeats");

    test.result.store(Result::Refuse);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 20000000) == 0);
    test.result.store(Result::Throw);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 20000000) == RTC_ERR_FAILURE);
    CHECK(test.attempts.load() == 8 && test.accepted.load() == 6);
    test.throwLookup.store(true);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 20000000) == RTC_ERR_FAILURE);
    test.throwLookup.store(false);
    test.skipDispatch.store(true);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 20000000) == RTC_ERR_FAILURE);
    test.skipDispatch.store(false);
    CHECK(test.attempts.load() == 8);
    singleCommandOwnership(test);
    CHECK(test.accepted.load() == 7);
    std::puts("PASS local accept/refuse/exception, scoped ownership and concurrent regular poll");

    X4ReceiverNackMetrics nack{};
    CHECK(test.handler.metrics(nack));
    CHECK(!nack.rtcp_packets && !nack.initial_requests);
    CHECK(test.handler.close() == 0);
    CHECK(x4_rtc_send_video_remb(VideoTrack, Sender, Video, 20000000) == 0);
    CHECK(test.attempts.load() == 9 && test.accepted.load() == 7);
    std::puts("PASS REMB leaves NACK metrics unchanged and sends nothing after closure");
}
