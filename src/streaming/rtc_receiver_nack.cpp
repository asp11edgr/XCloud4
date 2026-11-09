/* SPDX-License-Identifier: GPL-3.0-only */
/* The pinned native Message already includes its immutable overlay copy of
 * this POD. Load that definition first; #pragma once cannot unify two paths. */
#include <rtc/mediahandler.hpp>
#define X4_NACK_SEQUENCE_FROM_RTC_MESSAGE 1
#include "rtc_receiver_nack.hpp"
#if defined(X4_RECEIVER_NACK_VENDOR)
#include <x4_rtc_receive_diag.h>
#else
#include "rtc_receive_diag.h"
#endif
#include <rtc/rtp.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <limits>

static_assert(sizeof(X4NackSequenceTag) == 48 && alignof(X4NackSequenceTag) == 8 &&
    offsetof(X4NackSequenceTag, transport_epoch) == 24 &&
    offsetof(X4NackSequenceTag, anchor_rtp_timestamp) == 36 &&
    offsetof(X4NackSequenceTag, reason) == 40, "Pinned authenticated sequence POD ABI");

#ifdef X4_OPENORBIS
extern "C" void x4_native_random_or_exit(void *, size_t);
#ifndef X4_RECOVERY43
#define X4_RECOVERY43 1
#endif
#if X4_RECOVERY43
#include "recovery43_bridge_impl.hpp"
#endif

namespace rtc {
namespace {
enum { Detect = 18, RequestCreated, Dispatch, SrtcpBegin, SrtcpResult,
    LocalResult, AuthReturn, ReorderResult, AuContribution, State, Suppress, Counter };
uint16_t read16(const uint8_t *p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
void write16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value >> 8); p[1] = uint8_t(value); }
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

X4ReceiverNack::X4ReceiverNack(bool enabled, uint32_t epoch, bool legacy, bool grace43c,
    bool policy46b)
    : mEnabled(enabled), mLegacy(legacy), mGrace43c(!legacy && (grace43c || policy46b)),
      mPolicy46b(!legacy && policy46b), mEpoch(epoch), mSenderSsrc(0) {
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
    if (mPolicy46b) x4_receiver_nack_init_46b(&mState, false);
    else x4_receiver_nack_init_policy(&mState, false, legacy, mGrace43c);
    for (auto &counter : mExtra) counter.store(0, std::memory_order_relaxed);
}

bool X4ReceiverNack::sameConfiguration(bool enabled, uint32_t epoch, bool legacy, bool grace43c,
    bool policy46b) const {
    return enabled == mEnabled && legacy == mLegacy &&
        (!legacy && (grace43c || policy46b)) == mGrace43c && (!legacy && policy46b) == mPolicy46b &&
        epoch == mEpoch && !mClosing.load();
}

void X4ReceiverNack::takeFirstDispatch() {
    uint64_t request = mFirstDispatchRequest.exchange(0, std::memory_order_acquire);
    if (request) x4_receiver_nack_sent_first(&mState, request,
        mFirstDispatchTime.load(std::memory_order_relaxed));
}
bool X4ReceiverNack::metrics(X4ReceiverNackMetrics &out) {
    if (mGate.test_and_set(std::memory_order_acquire)) return false;
    x4_receiver_nack_snapshot(&mState, &out);
    if (mLegacy) out.budget_suppressed_positions = mState.counters.budget_suppressed_positions;
    mGate.clear(std::memory_order_release);
    out.fci_total = mFciTotal.load(std::memory_order_relaxed);
    out.rtcp_packets = mRtcpPackets.load(std::memory_order_relaxed);
#if !X4_PUBLIC
    out.startup_releases = x4_rtc_diag_startup_release_count();
#endif
    return true;
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
#if X4_RECOVERY43
    x4_recovery43::install(mEpoch, next());
#endif
    // The local Track description advertises feedback; it is not proof that
    // the answer accepted it. The owner API supplies the actual answer result.
    bool accepted = mFeedback.load(std::memory_order_acquire);
    x4_rtc_nack_event(State, (mEnabled ? 1u : 0u) | (accepted ? 2u : 0u) | 4u,
        identity(0), 0, mEnabled ? 1 : 0, mLegacy ? X4_RECEIVER_NACK_SESSION_REQUEST_MAX : 0,
        mLegacy ? (uint64_t(X4_RECEIVER_NACK_WATCH_SLOTS) << 32) | X4_RECEIVER_NACK_RANGE_MAX :
            (uint64_t(X4_RECEIVER_NACK_ADAPTIVE_SLOTS) << 32) | X4_RECEIVER_NACK_ADAPTIVE_MAX);
}
void X4ReceiverNack::acceptedFeedback(bool accepted) {
    mFeedback.store(accepted, std::memory_order_release);
    x4_rtc_nack_event(State, (mEnabled ? 1u : 0u) | (accepted ? 2u : 0u) | 4u | 16u,
        identity(0), 0, mEnabled ? 1 : 0, mLegacy ? X4_RECEIVER_NACK_SESSION_REQUEST_MAX : 0,
        mLegacy ? (uint64_t(X4_RECEIVER_NACK_WATCH_SLOTS) << 32) | X4_RECEIVER_NACK_RANGE_MAX :
            (uint64_t(X4_RECEIVER_NACK_ADAPTIVE_SLOTS) << 32) | X4_RECEIVER_NACK_ADAPTIVE_MAX);
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
void X4ReceiverNack::graceControl(const X4ReceiverNackGap &gap, unsigned kind,
    uint64_t selectedMask, uint64_t attemptStart, uint64_t localReturn,
    int localResult, unsigned reason) {
    // This control path is independent of optional diagnostic recording. The
    // bridge owns a bounded, try-only inbox and accounts omitted controls.
    // Keep the original request range: AUTH observation does not establish
    // that the application worker has stored the returning packet.
    X4NackGraceControl control{};
    control.identity = identity(gap.request_id);
    control.source = sourceKey(gap.ssrc);
    control.sequence_epoch = gap.sequence_epoch;
    control.first_extended = gap.first_extended;
    if (gap.missing_count && gap.missing_count <= X4_RECEIVER_NACK_RANGE_MAX)
        control.original_mask = gap.missing_count == 64 ? UINT64_MAX :
            (UINT64_C(1) << gap.missing_count) - 1;
    control.selected_mask = selectedMask;
    control.detected_us = gap.detected_us;
    if (gap.detected_us && gap.detected_us <= UINT64_MAX - X4_NACK_GRACE_MAX_US)
        control.deadline_us = gap.detected_us + X4_NACK_GRACE_MAX_US;
    control.attempt_start_us = attemptStart;
    control.local_return_us = localReturn;
    control.kind = kind;
    control.local_result = localResult;
    control.reason = reason;
    (void)x4_rtc_nack_grace_control(&control);
}

void X4ReceiverNack::resetObserver(unsigned locus) {
    // Serialized, including a dirty consumed by poll. The old state remains
    // exported separately from a later authenticated anchor; no REQUEST may
    // authorize the worker to learn a new epoch on its own.
    x4_rtc_nack_event(31, 16u | (locus << 8), identity(mState.sequence_epoch),
        sourceKey(mState.ssrc), mState.high_extended, mState.sequence_epoch, 0);
    x4_receiver_nack_discontinuity(&mState);
    mExtra[8].fetch_add(1, std::memory_order_relaxed);
}
void X4ReceiverNack::sequenceControl(uint32_t ssrc, const X4NackSequenceTag &sequence) {
    X4NackGraceControl control{};
    control.identity = identity(sequence.sequence_epoch);
    control.source = sourceKey(ssrc);
    control.sequence_epoch = sequence.sequence_epoch;
    control.first_extended = sequence.anchor_extended;
    control.original_mask = sequence.previous_high_extended;
    control.selected_mask = sequence.previous_epoch;
    control.detected_us = sequence.anchor_us;
    control.attempt_start_us = sequence.anchor_rtp_timestamp;
    control.kind = X4_NACK_GRACE_SEQUENCE;
    control.reason = sequence.reason;
    if (!x4_rtc_nack_grace_control(&control))
        mExtra[7].fetch_add(1, std::memory_order_relaxed);
    x4_rtc_nack_event(31, 13u | (sequence.reason << 8), control.identity, control.source,
        sequence.anchor_extended, sequence.previous_high_extended, sequence.previous_epoch);
    x4_rtc_nack_event(31, 14u, control.identity, control.source,
        sequence.anchor_us, sequence.anchor_rtp_timestamp, 0);
}

void X4ReceiverNack::incoming(message_vector &messages, const message_callback &send) {
    (void)send; // Observation only: no transport send or allocation on this path.
    for (auto &message : messages) {
        if (message->type != Message::Binary) continue;
        // Provenance must be produced by THIS owner, never retained from a
        // message reused by a host fixture or a previous handler invocation.
        message->x4NackSequence = {};
        message->x4NackIdentity = message->x4NackSource = message->x4NackExtended = 0;
        message->x4NackRtpTimestamp = message->x4NackReturnFlags = 0;
        message->x4NackAuthenticated = 0;
        bool foreignRetransmission = message->x4DiagFlags &
            (X4_RD_RETRANSMISSION | X4_RD_RTX_NORMALIZED);
#if X4_RECOVERY43
        // Only our authenticated deep handler may normalize a repair before
        // ReceivingSession. Keep rejecting unrelated vendor RTX provenance.
        if (message->x4DiagFlags & X4_RD_RECOVERY43) foreignRetransmission = false;
#endif
        if (!(message->x4DiagFlags & X4_RD_AUTHENTICATED) || foreignRetransmission) {
            mExtra[2].fetch_add(1, std::memory_order_relaxed); continue;
        }
        uint32_t ssrc = 0, timestamp = 0; uint16_t sequence = 0; uint8_t pt = 0;
        if (!validateRtp(*message, ssrc, sequence, timestamp, pt)) {
            mExtra[1].fetch_add(1, std::memory_order_relaxed); continue;
        }
        if (mClosing.load(std::memory_order_acquire)) continue;
        if (mGate.test_and_set(std::memory_order_acquire)) {
            mExtra[0].fetch_add(1, std::memory_order_relaxed);
            mDirty.store(true, std::memory_order_release);
            // Do not read worker/state fields without the gate. This is an
            // observed local omission, independent of trace-store omissions.
            x4_rtc_nack_event(31, 15u, identity(0), sourceKey(ssrc), sequence, timestamp, 1);
            continue;
        }
        if (mDirty.exchange(false, std::memory_order_acq_rel))
            resetObserver(1);
        if (mPolicy46b) takeFirstDispatch();
        mState.enabled = mEnabled && mFeedback.load(std::memory_order_acquire);
        X4ReceiverNackObservation observation{};
        const uint64_t now = x4_rtc_receive_clock();
        const uint16_t distance = uint16_t(sequence - uint16_t(mState.high_extended));
        int rtt = -1;
        bool startup = false;
        if (mGrace43c) {
            rtt = x4_rtc_diag_rtt_p50_ms();
            startup = x4_rtc_diag_startup_active(now);
            x4_receiver_nack_refresh(&mState, rtt, now, startup);
            mLastRtt.store(rtt, std::memory_order_release);
        } else if (!mLegacy && pt == X4_RECEIVER_NACK_VIDEO_PT && mState.bound &&
            ssrc == mState.ssrc && distance > 1u && distance < 32768u) {
            x4_receiver_nack_set_rtt(&mState, x4_rtc_diag_rtt_p50_ms());
        }
        if (mPolicy46b) {
            unsigned samples = mState.metrics.first_return_samples;
            uint32_t p95 = mState.metrics.first_return_p95_us;
            x4_receiver_nack_observe_return(&mState, ssrc, pt, sequence, now,
                !(message->x4DiagFlags & X4_RD_FEC_RECOVERED), &observation);
            if (samples != mState.metrics.first_return_samples ||
                p95 != mState.metrics.first_return_p95_us) {
                x4_receiver_nack_refresh(&mState, rtt, now, startup);
                x4_rtc_diag_first_return_publish(mState.metrics.first_return_p95_us,
                    mState.metrics.first_return_samples);
            }
        } else {
            x4_receiver_nack_observe(&mState, ssrc, pt, sequence, now, &observation);
        }
        if (pt == X4_RECEIVER_NACK_VIDEO_PT && observation.sequence_tag.sequence_epoch &&
            !(observation.flags & X4_NACK_OBS_BEFORE_EPOCH)) {
            bool newAnchor = observation.flags & (X4_NACK_OBS_INITIAL | X4_NACK_OBS_AMBIGUOUS);
            if (newAnchor) mAnchorTimestamp = timestamp;
            auto tag = observation.sequence_tag;
            tag.transport_epoch = mEpoch;
            tag.anchor_rtp_timestamp = mAnchorTimestamp;
            message->x4NackSequence = tag;
            message->x4NackSource = sourceKey(ssrc);
            message->x4NackExtended = observation.extended_sequence;
            message->x4NackRtpTimestamp = timestamp;
            message->x4NackAuthenticated = x4_rtc_receive_clock();
            if (newAnchor && mLegacy) sequenceControl(ssrc, tag);
        }
        if (observation.gap.present) {
            const auto &gap = observation.gap;
#if !X4_PUBLIC
            x4_rtc_diag_startup_range(X4_STARTUP_DETECT, identity(gap.gap_id),
                gap.detected_us, now);
#endif
            x4_rtc_nack_event(Detect, mState.enabled ? 1u : 0u, identity(gap.gap_id),
                sourceKey(ssrc), gap.first_extended, gap.last_extended, gap.received_extended);
            if (gap.request_id && mLegacy) {
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
                    graceControl(slot.gap, X4_NACK_GRACE_REQUEST, 0, 0, 0,
                        X4_NACK_GRACE_NOT_ATTEMPTED, X4_NACK_GRACE_ATTEMPT);
                    // Publish only the fully initialized queued request. The
                    // dispatcher can avoid the receive gate when no work is
                    // queued, including OFF and the ordinary idle interval.
                    mQueued.fetch_add(1, std::memory_order_release);
                }
            } else if (gap.request_id) {
                x4_rtc_nack_event(RequestCreated, 0, identity(gap.request_id), sourceKey(ssrc),
                    gap.first_extended, gap.last_extended, gap.detected_us);
            } else {
                suppress(gap, mEnabled && !mFeedback.load() ? 6u : gap.suppression,
                    gap.missing_count);
            }
        }
        if (observation.arrival.present) {
            const auto &arrival = observation.arrival;
            unsigned flags = (arrival.first_observation ? 1u : 0u) |
                (arrival.duplicate_observation ? 2u : 0u) | (arrival.range_complete ? 4u : 0u);
            x4_rtc_nack_event(AuthReturn, flags, identity(arrival.request_id), sourceKey(ssrc),
                arrival.extended_sequence, timestamp, arrival.requested_us);
            if (arrival.first_observation) {
#if !X4_PUBLIC
                x4_rtc_diag_startup_range(X4_STARTUP_RETURN, identity(arrival.gap_id),
                    arrival.detected_us, arrival.observed_us);
#endif
                message->x4NackIdentity = identity(arrival.request_id);
                message->x4NackSource = sourceKey(ssrc);
                message->x4NackExtended = arrival.extended_sequence;
                message->x4NackRtpTimestamp = timestamp;
                message->x4NackReturnFlags = flags;
                message->x4NackAuthenticated = arrival.observed_us;
            }
        }
        if (!mLegacy) mNextDue.store(mState.next_send_us, std::memory_order_release);
        mGate.clear(std::memory_order_release);
    }
}

message_ptr x4ReceiverNackPacket(uint32_t senderSsrc, uint32_t epoch,
    const X4ReceiverNackGap &gap, uint64_t pending, uint64_t dispatchTime,
    const char *cname, size_t cnameBytes) {
    if (gap.request_id > X4_RECEIVER_NACK_SESSION_REQUEST_MAX ||
        gap.missing_count > X4_RECEIVER_NACK_RANGE_MAX)
        throw std::invalid_argument("Invalid legacy receiver NACK descriptor");
    const uint64_t words[X4_RECEIVER_NACK_MASK_WORDS]{pending};
    return x4ReceiverNackPacketWide(senderSsrc, epoch, gap, words, dispatchTime,
        cname, cnameBytes, nullptr);
}

message_ptr x4ReceiverNackPacketWide(uint32_t senderSsrc, uint32_t epoch,
    const X4ReceiverNackGap &gap, const uint64_t pending[X4_RECEIVER_NACK_MASK_WORDS],
    uint64_t dispatchTime, const char *cname, size_t cnameBytes, unsigned *fciCount) {
    // RFC4585 minimal compound RR + SDES(CNAME) + Generic NACK. No SDP change.
    constexpr size_t rrBytes = 8;
    if (!senderSsrc || !epoch || !gap.request_id || gap.request_id > UINT32_MAX ||
        !gap.missing_count || gap.missing_count > X4_RECEIVER_NACK_ADAPTIVE_MAX ||
        gap.last_extended < gap.first_extended ||
        gap.last_extended - gap.first_extended != gap.missing_count - 1u ||
        !pending || !cname || cnameBytes != 35)
        throw std::invalid_argument("Invalid bounded receiver NACK descriptor");
    auto selected = [pending](unsigned offset) {
        return (pending[offset / 64u] & (UINT64_C(1) << (offset % 64u))) != 0;
    };
    for (unsigned offset = gap.missing_count; offset < X4_RECEIVER_NACK_ADAPTIVE_MAX; ++offset)
        if (selected(offset)) throw std::invalid_argument("Receiver NACK mask exceeds range");
    size_t sdesBytes = (4 + 4 + 2 + cnameBytes + 1 + 3) & ~size_t(3);
    constexpr unsigned maxParts = (X4_RECEIVER_NACK_ADAPTIVE_MAX + 16u) / 17u;
    std::array<uint16_t, maxParts> pids{}, masks{};
    unsigned parts = 0;
    for (unsigned bit = 0; bit < gap.missing_count;) {
        if (!selected(bit)) { ++bit; continue; }
        if (parts >= pids.size()) throw std::logic_error("Invalid receiver NACK FCI count");
        unsigned base = bit;
        pids[parts] = uint16_t(gap.first_extended + bit);
        uint16_t mask = 0;
        for (unsigned following = 1; following <= 16 && base + following < gap.missing_count;
            ++following) {
            if (selected(base + following)) mask |= uint16_t(1u << (following - 1));
        }
        masks[parts++] = mask; bit = base + 17;
    }
    if (!parts) throw std::invalid_argument("Empty receiver NACK FCI");
    if (fciCount) *fciCount = parts;
    auto result = make_message(rrBytes + sdesBytes + RtcpNack::Size(parts), Message::Control);
    std::memset(result->data(), 0, result->size());
    auto *rr = reinterpret_cast<RtcpRr *>(result->data()); rr->preparePacket(senderSsrc, 0);
    auto *sdes = reinterpret_cast<RtcpHeader *>(result->data() + rrBytes);
    sdes->prepareHeader(202, 1, uint16_t(sdesBytes / 4 - 1));
    auto *chunk = reinterpret_cast<RtcpSdesChunk *>(result->data() + rrBytes + 4);
    chunk->setSSRC(senderSsrc); chunk->_items.type = 1;
    chunk->_items._length = uint8_t(cnameBytes);
    std::memcpy(chunk->_items._text, cname, cnameBytes);
    auto *feedback = reinterpret_cast<uint8_t *>(result->data() + rrBytes + sdesBytes);
    auto *nack = reinterpret_cast<RtcpNack *>(feedback);
    nack->preparePacket(gap.ssrc, parts); nack->header.setPacketSenderSSRC(senderSsrc);
    static_assert(offsetof(RtcpNack, parts) == 12 && sizeof(RtcpNackPart) == 4,
        "RFC4585 feedback header and FCI sizes");
    // The vendor declares parts[1]; encode the variable-length tail as bytes.
    for (unsigned i = 0; i < parts; ++i) {
        uint8_t *fci = feedback + 12u + 4u * i;
        write16(fci, pids[i]);
        write16(fci + 2u, masks[i]);
    }
    result->x4NackIdentity = (uint64_t(epoch) << 32) | gap.request_id;
    result->x4NackSource = (UINT64_C(1) << 63) | (uint64_t(102) << 32) | gap.ssrc;
    result->x4NackFirst = gap.first_extended; result->x4NackLast = gap.last_extended;
    result->x4NackDetected = gap.detected_us; result->x4NackRequested = gap.requested_us;
    result->x4NackDispatch = dispatchTime;
    return result;
}
#if defined(X4_OPENORBIS) && !defined(X4_RECEIVER_NACK_VENDOR)
namespace {
bool tryRembPoll(const std::function<bool(message_ptr)> &send,
    const std::atomic<bool> &closing, int &result) noexcept;
}
#endif
/* Keep the debug line table for the unchanged NACK poll body. */
#line 426
int X4ReceiverNack::poll(const std::function<bool(message_ptr)> &send) {
#if defined(X4_OPENORBIS) && !defined(X4_RECEIVER_NACK_VENDOR)
    int rembResult;
    if (tryRembPoll(send, mClosing, rembResult)) return rembResult;
#endif
#line 427
    if (mClosing.load(std::memory_order_acquire)) return 0;
    if (mLegacy) {
        if (!mQueued.load(std::memory_order_acquire)) return 0;
    } else {
        uint64_t due = mNextDue.load(std::memory_order_acquire);
        if (!due) return 0;
        if (x4_rtc_receive_clock() < due &&
            (!mPolicy46b || !mFirstDispatchRequest.load(std::memory_order_acquire)) &&
            (!mGrace43c || mLastRtt.load(std::memory_order_acquire) == x4_rtc_diag_rtt_p50_ms()))
            return 0;
    }
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
        resetObserver(2);
    if (mPolicy46b) takeFirstDispatch();
    X4ReceiverNackGap gap{};
    uint64_t pending = 0;
    uint64_t pendingWide[X4_RECEIVER_NACK_MASK_WORDS]{};
    bool selected = false, retry = false;
    unsigned positions = 0;
    if (!mLegacy) {
        const uint64_t now = x4_rtc_receive_clock();
        if (mGrace43c) {
            int rtt = x4_rtc_diag_rtt_p50_ms();
            x4_receiver_nack_refresh(&mState, rtt, now, x4_rtc_diag_startup_active(now));
            mLastRtt.store(rtt, std::memory_order_release);
        }
        selected = x4_receiver_nack_next(&mState, now, &gap, pendingWide, &retry);
        mNextDue.store(mState.next_send_us, std::memory_order_release);
        if (selected) for (uint64_t word : pendingWide) positions += population(word);
    }
    for (unsigned checked = 0; mLegacy && checked < mRequests.size(); ++checked) {
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
            graceControl(gap, X4_NACK_GRACE_OUTCOME, 0, 0,
                x4_rtc_receive_clock(), X4_NACK_GRACE_NOT_ATTEMPTED,
                retained ? X4_NACK_GRACE_CANCEL_AUTH_COMPLETE : X4_NACK_GRACE_CANCEL_RETIRED);
        } else {
            positions = population(pending);
            ++mState.metrics.initial_requests;
            mState.metrics.requested_positions += positions;
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
    if (!selected || !positions) return 0;
    uint64_t dispatchTime = x4_rtc_receive_clock();
    if (mClosing.load(std::memory_order_acquire)) {
        suppress(gap, 7, positions);
        if (mLegacy) graceControl(gap, X4_NACK_GRACE_OUTCOME, pending, 0,
            x4_rtc_receive_clock(), X4_NACK_GRACE_NOT_ATTEMPTED, X4_NACK_GRACE_CANCEL_CLOSING);
        return 0;
    }
    x4_rtc_nack_event(Dispatch, positions, identity(gap.request_id), sourceKey(gap.ssrc),
        gap.first_extended, gap.last_extended, gap.requested_us);
    try {
        if (mSenderSsrc == gap.ssrc) {
            mSenderSsrc ^= UINT32_C(0x80000000);
            if (!mSenderSsrc) mSenderSsrc = 1;
        }
        if (mLegacy) pendingWide[0] = pending;
        unsigned fciCount = 0;
        auto message = x4ReceiverNackPacketWide(mSenderSsrc, mEpoch, gap, pendingWide,
            dispatchTime, mCname.data(), mCname.size(), &fciCount);
        mFciTotal.fetch_add(fciCount, std::memory_order_relaxed);
        mRtcpPackets.fetch_add(1, std::memory_order_relaxed);
        if (mPolicy46b) {
            dispatchTime = x4_rtc_receive_clock();
            message->x4NackDispatch = dispatchTime;
        }
#if !X4_PUBLIC
        x4_rtc_diag_startup_range(retry ? X4_STARTUP_RETRY : X4_STARTUP_NACK,
            identity(gap.gap_id), gap.detected_us, dispatchTime);
#endif
        if (mPolicy46b && !retry) {
            mFirstDispatchTime.store(dispatchTime, std::memory_order_relaxed);
            mFirstDispatchRequest.store(gap.request_id, std::memory_order_release);
        }
        bool result = send(message);
        // The send clock is a local attempt, separate from socket delivery
        // or server receipt. T46b takes it after packet construction.
        uint64_t returnObserved = x4_rtc_receive_clock();
        if (mLegacy) graceControl(gap, X4_NACK_GRACE_OUTCOME, pending, dispatchTime,
            returnObserved, result ? 1 : 0, X4_NACK_GRACE_ATTEMPT);
        x4_rtc_nack_event(LocalResult, 0, identity(gap.request_id), sourceKey(gap.ssrc),
            result ? 1 : 0, message->size(), dispatchTime);
        return result ? 1 : -1;
    } catch (const std::exception &) {
        uint64_t returnObserved = x4_rtc_receive_clock();
        if (mLegacy) graceControl(gap, X4_NACK_GRACE_OUTCOME, pending, dispatchTime,
            returnObserved, X4_NACK_GRACE_EXCEPTION, X4_NACK_GRACE_ATTEMPT);
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
            i < 7 ? 100 + i : 110 + i - 7,
            mExtra[i].load(std::memory_order_relaxed), mEpoch);
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
            graceControl(slot.gap, X4_NACK_GRACE_OUTCOME, 0, 0,
                x4_rtc_receive_clock(), X4_NACK_GRACE_NOT_ATTEMPTED, X4_NACK_GRACE_CANCEL_CLOSING);
        }
        x4_receiver_nack_close(&mState); counters(); mFinalCounters = true;
        x4_rtc_nack_event(State, 8, identity(0), sourceKey(mState.ssrc), 0, 0, 0);
    }
    mGate.clear(std::memory_order_release); return 0;
}
}
#endif

#if defined(X4_OPENORBIS) && !defined(X4_RECEIVER_NACK_VENDOR)
#include <rtc/rtc.h>
#if defined(__FreeBSD__)
extern "C" int32_t scePthreadGetthreadid(void);
#endif

namespace rtc {
namespace {
struct RembRequest {
    uint32_t senderSsrc, videoSsrc, bitrate;
};

/* The pinned C API owns the Track handle. Its poll entry invokes the app's
 * handler synchronously with the real protected RTCP sender. This handoff
 * allows only the calling thread to read its stack-backed command. A normal
 * poll never consumes a command owned by another thread. */
std::atomic<uintptr_t> rembOwner{0};
static_assert(std::atomic<uintptr_t>::is_always_lock_free, "REMB ownership requires a lock-free word");
const RembRequest *rembPending = nullptr;

uintptr_t rembThreadKey() noexcept {
#if defined(__FreeBSD__)
    const int32_t id = scePthreadGetthreadid();
    return id > 0 ? static_cast<uintptr_t>(id) : 0;
#else
    static thread_local char identity;
    return reinterpret_cast<uintptr_t>(&identity);
#endif
}

void writeRembU32(uint8_t *output, uint32_t value) {
    write16(output, uint16_t(value >> 16));
    write16(output + 2, uint16_t(value));
}

message_ptr rembPacket(const RembRequest &request) {
    constexpr size_t Bytes = 24;
    auto message = make_message(Bytes, Message::Control);
    auto *bytes = reinterpret_cast<uint8_t *>(message->data());
    bytes[0] = 0x8f; // RTCP v2, FMT=15, no padding.
    bytes[1] = 206;  // PSFB.
    write16(bytes + 2, Bytes / 4 - 1);
    writeRembU32(bytes + 4, request.senderSsrc);
    writeRembU32(bytes + 8, 0);
    std::memcpy(bytes + 12, "REMB", 4);
    bytes[16] = 1;
    uint32_t mantissa = request.bitrate;
    unsigned exponent = 0;
    while (mantissa > 0x3ffffu) {
        mantissa >>= 1;
        ++exponent;
    }
    bytes[17] = uint8_t((exponent << 2) | (mantissa >> 16));
    write16(bytes + 18, uint16_t(mantissa));
    writeRembU32(bytes + 20, request.videoSsrc);
    // No NACK identity: the vendor takes its generic SRTCP route.
    return message;
}

bool tryRembPoll(const std::function<bool(message_ptr)> &send,
    const std::atomic<bool> &closing, int &result) noexcept {
    const auto owner = rembOwner.load(std::memory_order_acquire);
    if (!owner || owner != rembThreadKey()) return false;
    const auto *request = rembPending;
    rembPending = nullptr;
    if (!request) result = RTC_ERR_NOT_AVAIL;
    else if (closing.load(std::memory_order_acquire)) result = 0;
    else {
        try {
            result = send(rembPacket(*request)) ? 1 : 0;
        } catch (...) {
            result = RTC_ERR_FAILURE;
        }
    }
    return true;
}

int sendRembThroughPoll(int videoTrack, const RembRequest &request) noexcept {
    if (videoTrack < 0 || !request.senderSsrc || !request.videoSsrc ||
        request.senderSsrc == request.videoSsrc ||
        request.bitrate < 3000000u || request.bitrate > 30000000u)
        return RTC_ERR_INVALID;
    const uintptr_t owner = rembThreadKey();
    uintptr_t idle = 0;
    if (!owner || !rembOwner.compare_exchange_strong(idle, owner, std::memory_order_acq_rel))
        return RTC_ERR_NOT_AVAIL;
    struct ReleaseRembOwner {
        ~ReleaseRembOwner() {
            rembPending = nullptr;
            rembOwner.store(0, std::memory_order_release);
        }
    } release;
    rembPending = &request;
    try {
        const int result = x4_rtc_poll_receiver_nack(videoTrack);
        // A nonnegative result must be from this command, never a normal poll.
        return result >= 0 && rembPending ? RTC_ERR_FAILURE : result;
    } catch (...) {
        return RTC_ERR_FAILURE;
    }
}
} // namespace
} // namespace rtc

extern "C" int x4_rtc_send_video_remb(int videoTrack, uint32_t senderSsrc,
    uint32_t videoSsrc, uint32_t bitrate) noexcept {
    return rtc::sendRembThroughPoll(videoTrack, {senderSsrc, videoSsrc, bitrate});
}
#endif
