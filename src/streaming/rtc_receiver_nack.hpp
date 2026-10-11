/* SPDX-License-Identifier: GPL-3.0-only */
/* XCloud4 vendor extension; not an upstream receiver-NACK API. */
#pragma once
#include "receiver_nack_state.h"
#include <rtc/mediahandler.hpp>
#include <rtc/message.hpp>
#include <array>
#include <atomic>
#include <functional>

namespace rtc {
/* Exact Generic NACK acceptance; upstream hasFeedback uses substring search. */
bool x4ReceiverNackAcceptsNack(const std::string &format,
    const std::vector<std::string> &feedback);
/* Production encoder exposed for bounded host fixtures; no network call. */
message_ptr x4ReceiverNackPacket(uint32_t senderSsrc, uint32_t epoch,
    const X4ReceiverNackGap &gap, uint64_t pending, uint64_t dispatchTime,
    const char *cname, size_t cnameBytes);
class X4ReceiverNack final : public MediaHandler {
public:
    X4ReceiverNack(bool enabled, uint32_t epoch);
    ~X4ReceiverNack() override;
    void media(const Description::Media &description) override;
    void acceptedFeedback(bool accepted);
    void incoming(message_vector &messages, const message_callback &send) override;
    int poll(const std::function<bool(message_ptr)> &send);
    int close();
    bool sameConfiguration(bool enabled, uint32_t epoch) const;
    void duplicateInstall();
private:
    struct Request { X4ReceiverNackGap gap{}; bool queued = false; };
    static unsigned population(uint64_t bits);
    static bool validateRtp(const Message &message, uint32_t &ssrc,
        uint16_t &sequence, uint32_t &timestamp, uint8_t &payloadType);
    static uint64_t sourceKey(uint32_t ssrc);
    uint64_t identity(uint64_t request) const;
    message_ptr packet(const X4ReceiverNackGap &gap, uint64_t pending,
        uint64_t dispatchTime);
    void counters();
    void suppress(const X4ReceiverNackGap &gap, unsigned reason, uint64_t positions);
    void graceControl(const X4ReceiverNackGap &gap, unsigned kind,
        uint64_t selectedMask, uint64_t attemptStart, uint64_t localReturn,
        int localResult, unsigned reason);
    const bool mEnabled;
    const uint32_t mEpoch;
    uint32_t mSenderSsrc;
    std::array<char, 35> mCname{};
    std::atomic<bool> mFeedback{false};
    std::atomic<bool> mClosing{false};
    std::atomic<bool> mPollActive{false};
    std::atomic<bool> mDirty{false};
    std::atomic<unsigned> mQueued{0};
    std::atomic_flag mGate = ATOMIC_FLAG_INIT;
    std::array<std::atomic<uint64_t>, 7> mExtra{};
    X4ReceiverNackState mState{};
    std::array<Request, X4_RECEIVER_NACK_SESSION_REQUEST_MAX> mRequests{};
    unsigned mNextDispatch = 0;
    bool mFinalCounters = false;
};
}
