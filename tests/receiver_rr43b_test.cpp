/* SPDX-License-Identifier: GPL-3.0-only */
#include "receiver_nack_state.h"
#include "rtc_receive_diag.h"
#include <rtc/rtcpreceivingsession.hpp>
#include "impl/logcounter.hpp"
#include <algorithm>
#include "recovery43_bridge_impl.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>

namespace {
std::atomic<uint64_t> clockUs{1};
constexpr uint32_t VideoSsrc = 7, RtxSsrc = 8, FecSsrc = 9;
constexpr uint8_t VideoPt = 102, RtxPt = 103, FecPt = 49;
constexpr uint64_t SrNtp = UINT64_C(0x0123456789abcdef);
constexpr uint32_t Lsr = 0x456789ab;
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "FAIL rr43b:%d %s\n", __LINE__, #expr); std::abort(); } } while (0)

uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void write16(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value >> 8); p[1] = uint8_t(value);
}
void write32(uint8_t *p, uint32_t value) {
    p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
    p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
}
rtc::message_ptr rtp(uint16_t seq, uint32_t ts, uint64_t arrival, uint8_t pt = VideoPt,
                     uint32_t ssrc = VideoSsrc, uint16_t original = 0) {
    auto out = rtc::make_message(pt == RtxPt ? 16 : 14, rtc::Message::Binary);
    auto *p = reinterpret_cast<uint8_t *>(out->data());
    p[0] = 0x80; p[1] = pt;
    write16(p + 2, seq); write32(p + 4, ts); write32(p + 8, ssrc);
    if (pt == RtxPt) {
        write16(p + 12, original); p += 2;
    }
    p[12] = 0x65; p[13] = 0x80;
    out->stream = ssrc;
    out->x4DiagFlags = X4_RD_AUTHENTICATED;
    out->x4DiagTime = arrival;
    return out;
}
rtc::message_ptr senderReport(uint64_t arrival, uint32_t source = VideoSsrc, bool compound = false) {
    auto out = rtc::make_message(compound ? 36 : 28, rtc::Message::Control);
    auto *p = reinterpret_cast<uint8_t *>(out->data());
    if (compound) {
        p[0] = 0x80; p[1] = 201; write16(p + 2, 1); write32(p + 4, source);
        p += 8;
    }
    p[0] = 0x80; p[1] = 200; write16(p + 2, 6); write32(p + 4, source);
    write32(p + 8, uint32_t(SrNtp >> 32)); write32(p + 12, uint32_t(SrNtp));
    out->stream = source;
    out->x4DiagFlags = X4_RD_AUTHENTICATED;
    out->x4DiagTime = arrival;
    return out;
}
rtc::message_vector input(rtc::RtcpReceivingSession &session, const rtc::message_ptr &packet,
                          const rtc::message_callback &send) {
    rtc::message_vector messages{packet};
    session.incomingChain(messages, send);
    return messages;
}

struct Wire {
    rtcX4VideoRrSnapshot fields{};
    uint32_t source = 0;
    unsigned packets = 0;
    bool accept = true;
    void onSend(const rtc::message_ptr &packet) {
        const auto *p = reinterpret_cast<const uint8_t *>(packet->data());
        CHECK(packet->x4VideoRr);
        CHECK(packet->size() == 32 && p[0] == 0x81 && p[1] == 201 && p[2] == 0 && p[3] == 7);
        source = read32(p + 8);
        fields.fractionLost = p[12];
        const uint32_t lost = read32(p + 12) & 0xffffff;
        fields.cumulativeLost = int32_t(lost) - ((lost & 0x800000) ? 0x1000000 : 0);
        fields.extendedSequence = read32(p + 16);
        fields.jitter = read32(p + 20);
        fields.lsr = read32(p + 24);
        fields.dlsr = read32(p + 28);
        ++packets;
        if (accept) {
            ++fields.reports;
            fields.reportsWithLoss += fields.fractionLost != 0;
            fields.maxFractionLost = std::max(fields.maxFractionLost, fields.fractionLost);
        }
        packet->x4VideoRrSent = accept; // fake Track::transportSend return
    }
    rtc::message_callback callback() {
        return [this](rtc::message_ptr packet) { onSend(packet); };
    }
    void checkSnapshot(const rtc::RtcpReceivingSession &session, uint64_t reports) const {
        const auto actual = session.x4GetVideoRrSnapshot();
        CHECK(actual.reports == reports && actual.reports == fields.reports);
        CHECK(actual.reportsWithLoss == fields.reportsWithLoss);
        CHECK(actual.maxFractionLost == fields.maxFractionLost);
        CHECK(actual.fractionLost == fields.fractionLost && actual.cumulativeLost == fields.cumulativeLost);
        CHECK(actual.extendedSequence == fields.extendedSequence && actual.jitter == fields.jitter);
        CHECK(actual.lsr == fields.lsr && actual.dlsr == fields.dlsr);
    }
};

// Independent reference: callers supply known extended numbers and unwrapped
// RTP timestamps. A set counts unique packets, and the estimator uses double.
struct Reference {
    std::set<uint64_t> sequence;
    uint64_t expectedPrior = 0, receivedPrior = 0;
    double jitter = 0, previousTransit = 0;
    bool hasTransit = false;
    void receive(uint64_t ext, uint64_t ts, uint64_t us, bool repair = false) {
        if (!sequence.insert(ext).second || repair) return;
        const double transit = double(us) * 0.09 - double(ts);
        if (hasTransit) jitter += (std::abs(transit - previousTransit) - jitter) / 16.;
        previousTransit = transit;
        hasTransit = true;
    }
    void expect(const Wire &wire) {
        const uint64_t high = *sequence.rbegin(), count = sequence.size();
        const uint64_t expected = high - *sequence.begin() + 1;
        const uint64_t inc = expected - expectedPrior, unique = count - receivedPrior;
        CHECK(wire.fields.extendedSequence == high);
        CHECK(wire.fields.cumulativeLost == int32_t(expected - count));
        CHECK(wire.fields.fractionLost == (inc > unique ? (inc - unique) * 256 / inc : 0));
        CHECK(wire.fields.jitter == uint32_t(jitter));
        expectedPrior = expected;
        receivedPrior = count;
    }
};

void realVectors() {
    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    session->x4ConfigureVideoRr(RTC_X4_VIDEO_RR_REPAIRED, VideoPt);
    Wire wire;
    auto send = wire.callback();
    Reference ref;
    constexpr uint64_t TsWrap = UINT64_C(1) << 32;
    const auto add = [&](uint64_t seq, uint64_t ts, uint64_t at, bool repair = false) {
        auto p = rtp(uint16_t(seq), uint32_t(ts), at);
        if (repair) p->x4DiagFlags |= X4_RD_RTX_NORMALIZED;
        CHECK(input(*session, p, send).size() == 1);
        ref.receive(seq, ts, at, repair);
    };
    CHECK(session->x4GetVideoRrSnapshot().reports == 0);
    add(65534, TsWrap - 2000, 1000000);
    add(65536, TsWrap + 1000, 1052000);
    add(65536, TsWrap + 1000, 1053000); // duplicate
    clockUs.store(1065000);
    input(*session, senderReport(1055000), send);
    CHECK(wire.packets == 1 && wire.source == VideoSsrc);
    CHECK(wire.fields.lsr == Lsr && wire.fields.dlsr == 655);
    CHECK(wire.fields.fractionLost == 85 && wire.fields.cumulativeLost == 1);
    ref.expect(wire); wire.checkSnapshot(*session, 1);

    add(65535, TsWrap - 500, 1066000, true); // fill a previous interval
    add(65537, TsWrap + 2500, 1069000);
    clockUs.store(1085000);
    input(*session, senderReport(1075000, VideoSsrc, true), send);
    CHECK(wire.packets == 2);
    CHECK(wire.fields.fractionLost == 0 && wire.fields.cumulativeLost == 0);
    ref.expect(wire); wire.checkSnapshot(*session, 2);

    input(*session, senderReport(1075000, RtxSsrc), send);
    auto malformed = senderReport(1075000);
    (*malformed)[3] = std::byte{0xff};
    input(*session, malformed, send);
    CHECK(wire.packets == 2); // wrong source and truncated RTCP are ignored

    add(65539, TsWrap + 5500, 1110000);
    wire.accept = false;
    input(*session, senderReport(1111000), send);
    const auto afterRejection = session->x4GetVideoRrSnapshot();
    CHECK(wire.packets == 3 && afterRejection.reports == 2);
    CHECK(afterRejection.reportsWithLoss == 1 && afterRejection.maxFractionLost == 85);
    CHECK(wire.fields.fractionLost == 128);
    wire.accept = true;
    input(*session, senderReport(1112000), send);
    CHECK(wire.packets == 4 && wire.fields.fractionLost == 128);
    ref.expect(wire); wire.checkSnapshot(*session, 3);
    std::puts("PASS RR wire wrap, duplicates, interval fraction, jitter, SR delay and local send failure");
}

rtc::message_ptr parityFor(uint16_t seq, uint32_t timestamp) {
    auto out = rtc::make_message(34, rtc::Message::Binary);
    auto *p = reinterpret_cast<uint8_t *>(out->data());
    p[0] = 0x80; p[1] = FecPt; write16(p + 2, 600);
    write32(p + 4, timestamp); write32(p + 8, FecSsrc);
    p[13] = VideoPt; write16(p + 14, 2); write32(p + 16, timestamp);
    p[20] = 1; write32(p + 24, VideoSsrc); write16(p + 28, seq); p[30] = 0xc0;
    p[32] = 0x65; p[33] = 0x80;
    out->stream = FecSsrc; out->x4DiagFlags = X4_RD_AUTHENTICATED;
    out->x4DiagTime = clockUs.load();
    return out;
}

void edgeProvenance() {
    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    session->x4ConfigureVideoRr(RTC_X4_VIDEO_RR_EDGE, VideoPt);
    Wire wire;
    auto send = wire.callback();
    auto add = [&](uint16_t seq, uint64_t arrival, unsigned flags) {
        auto packet = rtp(seq, 1000, arrival);
        packet->x4DiagFlags = flags;
        CHECK(input(*session, packet, send).size() == 1);
    };
    add(99, 1900000, X4_RD_AUTHENTICATED | X4_RD_RTX_NORMALIZED);
    clockUs.store(1900100);
    input(*session, senderReport(1900050), send);
    CHECK(wire.packets == 0); // a synthetic packet cannot establish the media source
    add(100, 1901000, X4_RD_AUTHENTICATED);
    add(104, 1902000, X4_RD_AUTHENTICATED);
    add(101, 1903000, X4_RD_AUTHENTICATED | X4_RD_RECOVERY43);
    add(102, 1904000, X4_RD_AUTHENTICATED | X4_RD_FEC_RECOVERED);
    add(103, 1905000, X4_RD_RETRANSMISSION); // unauthenticated
    add(103, 1906000, X4_RD_AUTHENTICATED | X4_RD_RETRANSMISSION);
    add(103, 1907000, X4_RD_AUTHENTICATED | X4_RD_RETRANSMISSION);
    clockUs.store(1910000);
    input(*session, senderReport(1908000, VideoSsrc, true), send);
    Reference ref;
    ref.receive(100, 1000, 1901000);
    ref.receive(104, 1000, 1902000);
    ref.receive(103, 1000, 1906000);
    ref.expect(wire);
    CHECK(wire.packets == 1 && wire.fields.cumulativeLost == 2 && wire.fields.fractionLost == 102);
    CHECK(wire.fields.lsr == Lsr && wire.fields.dlsr == 131);
    wire.checkSnapshot(*session, 1);
    std::puts("PASS RR Edge provenance rejects synthetic and unauthenticated packets, not same-SSRC retransmissions");
}

void repairsInRealChain(rtcX4VideoRrMode mode) {
    const uint32_t epoch = 43005 + unsigned(mode);
    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    session->x4ConfigureVideoRr(mode, VideoPt);
    CHECK(x4_recovery43_begin(epoch, 2, 0, 0x4513) == 0);
    X4VideoRepair43Config config{};
    config.media_ssrc = VideoSsrc; config.rtx_ssrc = RtxSsrc; config.fec_ssrc = FecSsrc;
    config.media_pt = VideoPt; config.rtx_pt = RtxPt; config.fec_pt = FecPt;
    config.rtx_accepted = config.fec_accepted = true;
    CHECK(x4_recovery43_answer(epoch, &config) == 0);
    x4_recovery43::install(epoch, session);
    CHECK(bool(session->next()));
    Wire wire;
    auto send = wire.callback();
    Reference ref;
    const auto native = [&](uint16_t seq, uint64_t at, bool retransmission = false) {
        clockUs.store(at);
        auto p = rtp(seq, 1000, at);
        if (retransmission) p->x4DiagFlags |= X4_RD_RETRANSMISSION;
        ref.receive(seq, 1000, at);
        return input(*session, p, send);
    };
    CHECK(native(100, 2000000).size() == 1);
    CHECK(native(103, 2001000).size() == 1);
    CHECK(native(105, 2002000).size() == 1);
    clockUs.store(2003000);
    auto normalized = input(*session, rtp(500, 1000, 2003000, RtxPt, RtxSsrc, 101), send);
    CHECK(normalized.size() == 1 && (normalized[0]->x4DiagFlags & X4_RD_RTX_NORMALIZED));
    clockUs.store(2004000);
    auto repaired = input(*session, parityFor(102, 1000), send);
    CHECK(repaired.size() == 1 && (repaired[0]->x4DiagFlags & X4_RD_FEC_RECOVERED));
    CHECK(input(*session, parityFor(102, 1000), send).empty());
    if (mode == RTC_X4_VIDEO_RR_REPAIRED) {
        ref.receive(101, 1000, 2003000, true);
        ref.receive(102, 1000, 2004000, true);
    }
    CHECK(native(104, 2005000, true).size() == 1);
    CHECK(native(104, 2006000, true).empty());
    clockUs.store(2008000);
    input(*session, senderReport(2007000), send);
    CHECK(wire.fields.lsr == Lsr);
    if (mode != RTC_X4_VIDEO_RR_ZERO) ref.expect(wire);
    CHECK(wire.fields.cumulativeLost == (mode == RTC_X4_VIDEO_RR_EDGE ? 2 : 0));
    CHECK(wire.fields.fractionLost == (mode == RTC_X4_VIDEO_RR_EDGE ? 85 : 0));
    wire.checkSnapshot(*session, 1);

    // RTX already supplied 101 for decoding. A later original on the media
    // SSRC still counts in Edge even though the recovery engine discards it.
    CHECK(native(101, 2009000, true).empty());
    CHECK(native(107, 2010000).size() == 1);
    clockUs.store(2012000);
    input(*session, senderReport(2011000), send);
    if (mode != RTC_X4_VIDEO_RR_ZERO) ref.expect(wire);
    CHECK(wire.fields.cumulativeLost == (mode == RTC_X4_VIDEO_RR_EDGE ? 2 : mode == RTC_X4_VIDEO_RR_REPAIRED ? 1 : 0));
    CHECK(wire.fields.fractionLost == (mode == RTC_X4_VIDEO_RR_REPAIRED ? 128 : 0));
    wire.checkSnapshot(*session, 2);
    x4_recovery43_end(epoch);
    std::printf("PASS actual recovery chain on the RR wire, mode=%u\n", unsigned(mode));
}

void vendorRtxUnwrap() {
    rtc::Description::Video media;
    media.addH264Codec(VideoPt);
    media.addSSRC(VideoSsrc, "rr-test");
    media.addRtxSSRC(VideoSsrc, RtxSsrc);
    media.addRtxCodec(RtxPt, VideoPt, 90000);
    for (auto mode : {RTC_X4_VIDEO_RR_REPAIRED, RTC_X4_VIDEO_RR_EDGE}) {
        auto session = std::make_shared<rtc::RtcpReceivingSession>();
        session->mediaChain(media);
        session->x4ConfigureVideoRr(mode, VideoPt);
        Wire wire;
        auto send = wire.callback();
        Reference ref;
        CHECK(input(*session, rtp(110, 1000, 2100000), send).size() == 1);
        ref.receive(110, 1000, 2100000);
        CHECK(input(*session, rtp(112, 1000, 2101000), send).size() == 1);
        ref.receive(112, 1000, 2101000);
        const auto out = input(*session, rtp(510, 1000, 2102000, RtxPt, RtxSsrc, 111), send);
        CHECK(out.size() == 1 && (out[0]->x4DiagFlags & X4_RD_RTX_NORMALIZED));
        const auto *packet = reinterpret_cast<const rtc::RtpHeader *>(out[0]->data());
        CHECK(packet->seqNumber() == 111 && packet->ssrc() == VideoSsrc && packet->payloadType() == VideoPt);
        if (mode == RTC_X4_VIDEO_RR_REPAIRED) ref.receive(111, 1000, 2102000, true);
        clockUs.store(2104000);
        input(*session, senderReport(2103000), send);
        ref.expect(wire);
        CHECK(wire.fields.fractionLost == (mode == RTC_X4_VIDEO_RR_EDGE ? 85 : 0));
        wire.checkSnapshot(*session, 1);
    }
    std::puts("PASS RR distinguishes RTX normalized by libdatachannel itself");
}

void edgeAfterSimulatedLoss() {
    constexpr uint32_t epoch = 43014;
    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    session->x4ConfigureVideoRr(RTC_X4_VIDEO_RR_EDGE, VideoPt);
    CHECK(x4_recovery43_begin(epoch, 0, 3, 0x4513) == 0);
    X4VideoRepair43Config config{};
    config.media_ssrc = VideoSsrc;
    config.media_pt = VideoPt;
    CHECK(x4_recovery43_answer(epoch, &config) == 0);
    x4_recovery43::install(epoch, session);
    Wire wire;
    auto send = wire.callback();
    Reference ref;
    unsigned dropped = 0;
    for (uint16_t seq = 1; seq <= 256; ++seq) {
        const uint64_t arrival = 2100000 + seq * 1000u;
        clockUs.store(arrival);
        const auto out = input(*session, rtp(seq, 90u * seq, arrival), send);
        if (out.empty()) ++dropped;
        else ref.receive(seq, 90u * seq, arrival);
    }
    auto snapshot = std::make_unique<X4Recovery43Snapshot>();
    CHECK(x4_recovery43_snapshot(epoch, snapshot.get()));
    CHECK(dropped > 0 && snapshot->repair.lost_simulated[0] == dropped);
    clockUs.store(2360000);
    input(*session, senderReport(2359000), send);
    ref.expect(wire);
    CHECK(wire.fields.cumulativeLost > 0 && wire.fields.fractionLost > 0);
    wire.checkSnapshot(*session, 1);
    x4_recovery43_end(epoch);
    std::puts("PASS RR Edge observes native packets after simulated loss");
}

void zeroAndUpstreamTracks() {
    auto zero = std::make_shared<rtc::RtcpReceivingSession>();
    zero->x4ConfigureVideoRr(RTC_X4_VIDEO_RR_ZERO, VideoPt);
    Wire wire;
    auto send = wire.callback();
    input(*zero, rtp(100, 1000, 2000000), send);
    input(*zero, rtp(200, 2000, 2000001), send);
    clockUs.store(2100000);
    input(*zero, senderReport(2050000), send);
    CHECK(wire.fields.fractionLost == 0 && wire.fields.cumulativeLost == 0 && wire.fields.jitter == 0);
    CHECK(wire.fields.lsr == Lsr && wire.fields.dlsr == 0);
    wire.checkSnapshot(*zero, 1);

    auto audio = std::make_shared<rtc::RtcpReceivingSession>(); // never configured
    const uint32_t audioSsrc = 55;
    unsigned audioReports = 0;
    auto old = [&](rtc::message_ptr p) {
        CHECK(!p->x4VideoRr && p->size() == 32);
        const auto *data = reinterpret_cast<const uint8_t *>(p->data());
        CHECK(read32(data + 8) == audioSsrc && data[12] == 128);
        ++audioReports;
    };
    input(*audio, rtp(0, 1000, 2000000, 111, audioSsrc), old);
    input(*audio, rtp(3, 1000, 2001000, 111, audioSsrc), old);
    input(*audio, senderReport(2050000, audioSsrc), old);
    CHECK(audioReports == 1 && audio->x4GetVideoRrSnapshot().reports == 0);
    std::puts("PASS configured Cero emits zero loss and jitter; unconfigured audio uses upstream RR");
}

void snapshotsConcurrent() {
    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    session->x4ConfigureVideoRr(RTC_X4_VIDEO_RR_REPAIRED, VideoPt);
    std::atomic<bool> running{true}, invalid{false};
    auto reader = std::thread([&, session] {
        uint64_t prev = 0;
        while (running.load()) {
            const auto out = session->x4GetVideoRrSnapshot();
            if (out.reports < prev || (out.reports &&
                (out.lsr != Lsr || out.cumulativeLost || out.fractionLost)))
                invalid.store(true);
            prev = out.reports;
        }
    });
    auto accept = [](rtc::message_ptr packet) { packet->x4VideoRrSent = true; };
    for (uint16_t i = 1; i <= 1024; ++i) {
        input(*session, rtp(i, 1000, 2200000 + i), accept);
        if (i % 16 == 0)
            input(*session, senderReport(2202000 + i), accept);
    }
    CHECK(session->x4GetVideoRrSnapshot().reports == 64);
    session.reset();
    running.store(false);
    reader.join();
    CHECK(!invalid.load());
    std::puts("PASS RR snapshot concurrent with receive and release of the owning reference");
}
} // namespace

extern "C" uint64_t x4_rtc_receive_clock() { return clockUs.load(); }
extern "C" uint64_t x4_rtc_receive_packet(unsigned, const void *, size_t,
                                          uint64_t time, uint32_t) { return time; }
extern "C" void x4_rtc_receive_setting(unsigned, uint64_t, uint64_t, uint64_t) {}
extern "C" void x4_rtc_receive_reject(unsigned, unsigned, int, const void *, size_t) {}
extern "C" int x4_log_printf(const char *, ...) { return 0; }
extern "C" void x4_recovery43_test_reader_acquired(uint32_t) {}
extern "C" void x4_recovery43_test_end_waiting(uint32_t) {}
extern "C" void x4_recovery43_test_receive_before_gate(uint32_t) {}
namespace rtc::impl {
LogCounter::LogCounter(plog::Severity, const std::string &, std::chrono::seconds) {}
LogCounter &LogCounter::operator++(int) { return *this; }
}

int main() {
    realVectors();
    edgeProvenance();
    for (auto mode : {RTC_X4_VIDEO_RR_ZERO, RTC_X4_VIDEO_RR_REPAIRED, RTC_X4_VIDEO_RR_EDGE})
        repairsInRealChain(mode);
    vendorRtxUnwrap();
    edgeAfterSimulatedLoss();
    zeroAndUpstreamTracks();
    snapshotsConcurrent();
    std::printf("PASS %u checks\n", checks);
}
