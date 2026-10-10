/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_receiver_nack.hpp"
#include <x4_rtc_receive_diag.h>
#include <rtc/rtp.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <limits>

#ifdef X4_OPENORBIS
extern "C" void x4_native_random_or_exit(void *, size_t);

namespace rtc {
namespace {
enum { Detect = 18, RequestCreated, Dispatch, SrtcpBegin, SrtcpResult,
    LocalResult, AuthReturn, ReorderResult, AuContribution, State, Suppress, Counter };
uint16_t read16(const uint8_t *p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t read32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) | p[3];
}
}
X4ReceiverNack::~X4ReceiverNack() { (void)close(); }
bool x4ReceiverNackAcceptsNack(const std::string &format,
    const std::vector<std::string> &feedback) {
    return format == "H264" &&
        std::find(feedback.begin(), feedback.end(), "nack") != feedback.end();
}

X4ReceiverNack::X4ReceiverNack(bool enabled, uint32_t epoch)
    : mEnabled(enabled), mEpoch(epoch), mSenderSsrc(0) {
    if (!epoch) throw std::invalid_argument("Receiver NACK epoch is zero");
    x4_native_random_or_exit(&mSenderSsrc, sizeof(mSenderSsrc));
    if (!mSenderSsrc) mSenderSsrc = 1;
    std::array<uint8_t, 16> cnameRandom{};
    x4_native_random_or_exit(cnameRandom.data(), cnameRandom.size());
    constexpr char hex[] = "0123456789abcdef";
    mCname[0] = 'x'; mCname[1] = '4'; mCname[2] = '-';
    for (unsigned i = 0; i < cnameRandom.size(); ++i) {
        mCname[3 + 2 * i] = hex[cnameRandom[i] >> 4];
        mCname[4 + 2 * i] = hex[cnameRandom[i] & 15];
    }
    x4_receiver_nack_init(&mState, false);
    for (auto &counter : mExtra) counter.store(0, std::memory_order_relaxed);
}

bool X4ReceiverNack::sameConfiguration(bool enabled, uint32_t epoch) const {
    return enabled == mEnabled && epoch == mEpoch && !mClosing.load();
}
void X4ReceiverNack::duplicateInstall() {
    mExtra[5].fetch_add(1, std::memory_order_relaxed);
}
uint64_t X4ReceiverNack::sourceKey(uint32_t ssrc) {
    return (UINT64_C(1) << 63) | (uint64_t(X4_RECEIVER_NACK_VIDEO_PT) << 32) | ssrc;
}
uint64_t X4ReceiverNack::identity(uint64_t request) const {
    return (uint64_t(mEpoch) << 32) | request;
}
unsigned X4ReceiverNack::population(uint64_t bits) {
    unsigned result = 0;
    while (bits) { bits &= bits - 1; ++result; }
    return result;
}
void X4ReceiverNack::media(const Description::Media &description) {
    (void)description;
    // The local Track description advertises feedback; it is not proof that
    // the answer accepted it. The owner API supplies the actual answer result.
    bool accepted = mFeedback.load(std::memory_order_acquire);
    x4_rtc_nack_event(State, (mEnabled ? 1u : 0u) | (accepted ? 2u : 0u) | 4u,
        identity(0), 0, mEnabled ? 1 : 0, X4_RECEIVER_NACK_SESSION_REQUEST_MAX,
        (uint64_t(X4_RECEIVER_NACK_WATCH_SLOTS) << 32) | X4_RECEIVER_NACK_RANGE_MAX);
}
void X4ReceiverNack::acceptedFeedback(bool accepted) {
    mFeedback.store(accepted, std::memory_order_release);
    x4_rtc_nack_event(State, (mEnabled ? 1u : 0u) | (accepted ? 2u : 0u) | 4u | 16u,
        identity(0), 0, mEnabled ? 1 : 0, X4_RECEIVER_NACK_SESSION_REQUEST_MAX,
        (uint64_t(X4_RECEIVER_NACK_WATCH_SLOTS) << 32) | X4_RECEIVER_NACK_RANGE_MAX);
}

bool X4ReceiverNack::validateRtp(const Message &message, uint32_t &ssrc,
    uint16_t &sequence, uint32_t &timestamp, uint8_t &payloadType) {
    if (message.size() < 12) return false;
    const auto *p = reinterpret_cast<const uint8_t *>(message.data());
    if ((p[0] >> 6) != 2) return false;
    size_t header = 12u + 4u * (p[0] & 15u);
    if (header > message.size()) return false;
    if (p[0] & 16u) {
        if (message.size() - header < 4) return false;
        size_t extension = size_t(read16(p + header + 2)) * 4u;
        if (extension > message.size() - header - 4u) return false;
        header += 4u + extension;
    }
    size_t payload = message.size() - header;
    if (p[0] & 32u) {
        unsigned padding = p[message.size() - 1];
        if (!padding || padding > payload) return false;
        payload -= padding;
    }
    if (!payload) return false;
    payloadType = p[1] & 127u;
    sequence = read16(p + 2); timestamp = read32(p + 4); ssrc = read32(p + 8);
    return true;
}
void X4ReceiverNack::suppress(const X4ReceiverNackGap &gap, unsigned reason,
    uint64_t positions) {
    x4_rtc_nack_event(Suppress, 0, identity(gap.request_id ? gap.request_id : gap.gap_id),
        sourceKey(gap.ssrc), reason, positions, gap.gap_id);
}

void X4ReceiverNack::incoming(message_vector &messages, const message_callback &send) {
    (void)send; // Observation only: no transport send or allocation on this path.
    for (auto &message : messages) {
        if (message->type != Message::Binary) continue;
        if (!(message->x4DiagFlags & X4_RD_AUTHENTICATED) ||
            (message->x4DiagFlags & (X4_RD_RETRANSMISSION | X4_RD_RTX_NORMALIZED))) {
            mExtra[2].fetch_add(1, std::memory_order_relaxed); continue;
        }
        uint32_t ssrc = 0, timestamp = 0; uint16_t sequence = 0; uint8_t pt = 0;
        if (!validateRtp(*message, ssrc, sequence, timestamp, pt)) {
            mExtra[1].fetch_add(1, std::memory_order_relaxed); continue;
        }
        if (mClosing.load(std::memory_order_acquire)) continue;
        if (mGate.test_and_set(std::memory_order_acquire)) {
            mExtra[0].fetch_add(1, std::memory_order_relaxed);
            mDirty.store(true, std::memory_order_release); continue;
        }
        if (mDirty.exchange(false, std::memory_order_acq_rel))
            x4_receiver_nack_discontinuity(&mState);
        mState.enabled = mEnabled && mFeedback.load(std::memory_order_acquire);
        X4ReceiverNackObservation observation{};
        x4_receiver_nack_observe(&mState, ssrc, pt, sequence,
            x4_rtc_receive_clock(), &observation);
        if (observation.gap.present) {
            const auto &gap = observation.gap;
            x4_rtc_nack_event(Detect, mState.enabled ? 1u : 0u, identity(gap.gap_id),
                sourceKey(ssrc), gap.first_extended, gap.last_extended, gap.received_extended);
            if (gap.request_id) {
                auto &slot = mRequests[size_t(gap.request_id - 1)];
                if (slot.queued) {
                    mExtra[3].fetch_add(1, std::memory_order_relaxed);
                    suppress(gap, 8, gap.missing_count);
                } else {
                    slot.gap = gap; slot.queued = true;
                    slot.gap.requested_us = x4_rtc_receive_clock();
                    for (auto &range : mState.ranges)
                        if (range.used && range.request_id == gap.request_id)
                            range.requested_us = slot.gap.requested_us;
                    x4_rtc_nack_event(RequestCreated, 0, identity(gap.request_id), sourceKey(ssrc),
                        gap.first_extended, gap.last_extended, gap.detected_us);
                    // Publish only the fully initialized queued request. The
                    // dispatcher can avoid the receive gate when no work is
                    // queued, including OFF and the ordinary idle interval.
                    mQueued.fetch_add(1, std::memory_order_release);
                }
            } else suppress(gap, mEnabled && !mFeedback.load() ? 6u : gap.suppression,
                gap.missing_count);
        }
        if (observation.arrival.present) {
            const auto &arrival = observation.arrival;
            unsigned flags = (arrival.first_observation ? 1u : 0u) |
                (arrival.duplicate_observation ? 2u : 0u) | (arrival.range_complete ? 4u : 0u);
            x4_rtc_nack_event(AuthReturn, flags, identity(arrival.request_id), sourceKey(ssrc),
                arrival.extended_sequence, timestamp, arrival.requested_us);
            if (arrival.first_observation) {
                message->x4NackIdentity = identity(arrival.request_id);
                message->x4NackSource = sourceKey(ssrc);
                message->x4NackExtended = arrival.extended_sequence;
                message->x4NackRtpTimestamp = timestamp;
                message->x4NackReturnFlags = flags;
            }
        }
        mGate.clear(std::memory_order_release);
    }
}

message_ptr x4ReceiverNackPacket(uint32_t senderSsrc, uint32_t epoch,
    const X4ReceiverNackGap &gap, uint64_t pending, uint64_t dispatchTime,
    const char *cname, size_t cnameBytes) {
    // RFC4585 minimal compound RR + SDES(CNAME) + Generic NACK. No SDP change.
    constexpr size_t rrBytes = 8;
    if (!senderSsrc || !epoch || !gap.request_id || gap.request_id > 64 ||
        !gap.missing_count || gap.missing_count > 64 || !pending ||
        gap.last_extended < gap.first_extended ||
        gap.last_extended - gap.first_extended != gap.missing_count - 1u ||
        (gap.missing_count < 64 && (pending >> gap.missing_count)) ||
        !cname || cnameBytes != 35)
        throw std::invalid_argument("Invalid bounded receiver NACK descriptor");
    size_t sdesBytes = (4 + 4 + 2 + cnameBytes + 1 + 3) & ~size_t(3);
    std::array<uint16_t, 4> pids{}, masks{};
    unsigned parts = 0;
    for (unsigned bit = 0; bit < gap.missing_count;) {
        if (!(pending & (UINT64_C(1) << bit))) { ++bit; continue; }
        unsigned base = bit;
        pids[parts] = uint16_t(gap.first_extended + bit);
        uint16_t mask = 0;
        for (unsigned following = 1; following <= 16 && base + following < gap.missing_count;
            ++following) {
            if (pending & (UINT64_C(1) << (base + following)))
                mask |= uint16_t(1u << (following - 1));
        }
        masks[parts++] = mask; bit = base + 17;
    }
    if (!parts || parts > pids.size()) throw std::logic_error("Invalid receiver NACK FCI count");
    auto result = make_message(rrBytes + sdesBytes + RtcpNack::Size(parts), Message::Control);
    std::memset(result->data(), 0, result->size());
    auto *rr = reinterpret_cast<RtcpRr *>(result->data()); rr->preparePacket(senderSsrc, 0);
    auto *sdes = reinterpret_cast<RtcpHeader *>(result->data() + rrBytes);
    sdes->prepareHeader(202, 1, uint16_t(sdesBytes / 4 - 1));
    auto *chunk = reinterpret_cast<RtcpSdesChunk *>(result->data() + rrBytes + 4);
    chunk->setSSRC(senderSsrc); chunk->_items.type = 1;
    chunk->_items._length = uint8_t(cnameBytes);
    std::memcpy(chunk->_items._text, cname, cnameBytes);
    auto *nack = reinterpret_cast<RtcpNack *>(result->data() + rrBytes + sdesBytes);
    nack->preparePacket(gap.ssrc, parts); nack->header.setPacketSenderSSRC(senderSsrc);
    for (unsigned i = 0; i < parts; ++i) { nack->parts[i].setPid(pids[i]); nack->parts[i].setBlp(masks[i]); }
    result->x4NackIdentity = (uint64_t(epoch) << 32) | gap.request_id;
    result->x4NackSource = (UINT64_C(1) << 63) | (uint64_t(102) << 32) | gap.ssrc;
    result->x4NackFirst = gap.first_extended; result->x4NackLast = gap.last_extended;
    result->x4NackDetected = gap.detected_us; result->x4NackRequested = gap.requested_us;
    result->x4NackDispatch = dispatchTime;
    return result;
}
message_ptr X4ReceiverNack::packet(const X4ReceiverNackGap &gap, uint64_t pending,
    uint64_t dispatchTime) {
    if (mSenderSsrc == gap.ssrc) {
        mSenderSsrc ^= UINT32_C(0x80000000);
        if (!mSenderSsrc) mSenderSsrc = 1;
    }
    return x4ReceiverNackPacket(mSenderSsrc, mEpoch, gap, pending, dispatchTime,
        mCname.data(), mCname.size());
}

int X4ReceiverNack::poll(const std::function<bool(message_ptr)> &send) {
    if (mClosing.load(std::memory_order_acquire)) return 0;
    if (!mQueued.load(std::memory_order_acquire)) return 0;
    bool inactive = false;
    if (!mPollActive.compare_exchange_strong(inactive, true, std::memory_order_acq_rel)) {
        mExtra[4].fetch_add(1, std::memory_order_relaxed); return 0;
    }
    struct PollGuard {
        std::atomic<bool> &active;
        ~PollGuard() { active.store(false, std::memory_order_release); }
    } pollGuard{mPollActive};
    if (mClosing.load(std::memory_order_acquire)) return 0;
    if (mGate.test_and_set(std::memory_order_acquire)) {
        mExtra[4].fetch_add(1, std::memory_order_relaxed); return 0;
    }
    if (mDirty.exchange(false, std::memory_order_acq_rel))
        x4_receiver_nack_discontinuity(&mState);
    X4ReceiverNackGap gap{}; uint64_t pending = 0; bool selected = false;
    for (unsigned checked = 0; checked < mRequests.size(); ++checked) {
        auto &slot = mRequests[mNextDispatch++ % mRequests.size()];
        if (!slot.queued) continue;
        slot.queued = false;
        mQueued.fetch_sub(1, std::memory_order_acq_rel);
        gap = slot.gap; selected = true;
        uint64_t first = 0;
        pending = x4_receiver_nack_pending(&mState, gap.request_id, &first);
        if (!pending) {
            bool retained = false;
            for (const auto &range : mState.ranges)
                if (range.used && range.request_id == gap.request_id) retained = true;
            suppress(gap, retained ? 4 : 5, gap.missing_count);
        } else {
            // Cancellation is exact at this serialized selection boundary.
            // A later arrival can still race packet construction/protection;
            // retain the selection clock instead of calling it atomic send.
            uint64_t snapshotTime = x4_rtc_receive_clock();
            x4_rtc_nack_event(30, 0, identity(gap.request_id), sourceKey(gap.ssrc),
                gap.first_extended, pending, snapshotTime);
        }
        break;
    }
    mGate.clear(std::memory_order_release);
    if (!selected || !pending) return 0;
    uint64_t dispatchTime = x4_rtc_receive_clock();
    if (mClosing.load(std::memory_order_acquire)) { suppress(gap, 7, population(pending)); return 0; }
    x4_rtc_nack_event(Dispatch, population(pending), identity(gap.request_id), sourceKey(gap.ssrc),
        gap.first_extended, gap.last_extended, gap.requested_us);
    try {
        auto message = packet(gap, pending, dispatchTime);
        bool result = send(message);
        x4_rtc_nack_event(LocalResult, 0, identity(gap.request_id), sourceKey(gap.ssrc),
            result ? 1 : 0, message->size(), dispatchTime);
        return result ? 1 : -1;
    } catch (const std::exception &) {
        // Numeric class only; no secret-bearing exception text is exported.
        x4_rtc_nack_event(LocalResult, 1, identity(gap.request_id), sourceKey(gap.ssrc),
            uint64_t(int64_t(-1)), 0, dispatchTime);
        return -1;
    }
}
void X4ReceiverNack::counters() {
    static_assert(sizeof(X4ReceiverNackCounters) % sizeof(uint64_t) == 0, "Counter ABI");
    std::array<uint64_t, sizeof(X4ReceiverNackCounters) / sizeof(uint64_t)> values{};
    std::memcpy(values.data(), &mState.counters, sizeof(mState.counters));
    for (unsigned i = 0; i < values.size(); ++i)
        x4_rtc_nack_event(Counter, 0, identity(0), sourceKey(mState.ssrc), i, values[i], mEpoch);
    for (unsigned i = 0; i < mExtra.size(); ++i)
        x4_rtc_nack_event(Counter, 0, identity(0), sourceKey(mState.ssrc),
            100 + i, mExtra[i].load(std::memory_order_relaxed), mEpoch);
}
int X4ReceiverNack::close() {
    mClosing.store(true, std::memory_order_release);
    if (mPollActive.load(std::memory_order_acquire)) return -1;
    if (mGate.test_and_set(std::memory_order_acquire)) return -1;
    if (!mFinalCounters) {
        for (auto &slot : mRequests) if (slot.queued) {
            slot.queued = false; mQueued.fetch_sub(1, std::memory_order_acq_rel);
            mExtra[6].fetch_add(1, std::memory_order_relaxed);
            suppress(slot.gap, 7, slot.gap.missing_count);
        }
        x4_receiver_nack_close(&mState); counters(); mFinalCounters = true;
        x4_rtc_nack_event(State, 8, identity(0), sourceKey(mState.ssrc), 0, 0, 0);
    }
    mGate.clear(std::memory_order_release); return 0;
}
}
#endif
