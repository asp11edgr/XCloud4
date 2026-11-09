/* SPDX-License-Identifier: MPL-2.0 */
#ifndef X4_VIDEO_RR_HPP
#define X4_VIDEO_RR_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace rtc {

struct X4RrVideoStats {
	static constexpr unsigned Window = 2048;
	static constexpr unsigned MaxForward = 3000;

	uint32_t source = 0;
	uint64_t base = 0, highest = 0, received = 0;
	uint64_t expectedPrior = 0, receivedPrior = 0, epoch = 0;
	std::array<uint64_t, Window> seen{};
	uint32_t transit = 0;
	uint64_t jitterQ4 = 0;
	bool transitValid = false;
	uint32_t candidateSource = 0, candidateTransit = 0;
	uint16_t candidateSequence = 0;

	void observe(uint32_t ssrc, uint16_t seq, uint32_t timestamp, uint64_t arrivalUs, bool repair) {
		if (!ssrc)
			return;
		// Keep the same 32-bit time domain as the 90 kHz RTP timestamps.
		const uint32_t arrival = uint32_t((arrivalUs / 100) * 9 + (arrivalUs % 100) * 9 / 100);
		const uint32_t packetTransit = arrival - timestamp;
		if (!source) {
			reset(ssrc, seq, packetTransit, !repair);
			return;
		}

		const unsigned delta = uint16_t(seq - uint16_t(highest));
		const uint64_t behind = delta ? 65536u - delta : 0;
		const bool forward = delta < 32768u;
		if (ssrc != source || (forward ? delta > MaxForward : behind >= Window)) {
			// A synthetic repair cannot move the reception statistics to a new epoch.
			// Require consecutive primary packets before accepting a discontinuity.
			if (repair)
				return;
			if (candidateSource != ssrc || seq != uint16_t(candidateSequence + 1u)) {
				candidateSource = ssrc;
				candidateSequence = seq;
				candidateTransit = packetTransit;
				return;
			}
			reset(ssrc, candidateSequence, candidateTransit, true);
			add(highest + 1, packetTransit, false);
			return;
		}
		candidateSource = 0;
		if (!forward && behind > highest - base)
			return;
		add(forward ? highest + delta : highest - behind, packetTransit, repair);
	}

	uint64_t expected() const { return highest - base + 1; }

	void fill(rtcX4VideoRrSnapshot &out) const {
		const auto total = expected();
		const auto lost = total - received;
		const auto expectedInterval = total - expectedPrior;
		const auto receivedInterval = received - receivedPrior;
		out.extendedSequence = uint32_t(highest);
		out.cumulativeLost = int32_t(std::min<uint64_t>(0x7fffff, lost));
		if (expectedInterval > receivedInterval) {
			out.fractionLost = uint8_t(std::min<uint64_t>(255,
			    (expectedInterval - receivedInterval) * 256 / expectedInterval));
		}
		out.jitter = uint32_t(jitterQ4 >> 4);
	}

private:
	void reset(uint32_t ssrc, uint16_t seq, uint32_t packetTransit, bool native) {
		source = ssrc;
		base = highest = seq;
		received = 1;
		expectedPrior = receivedPrior = 0;
		++epoch;
		seen.fill(0);
		seen[seq % Window] = uint64_t(seq) + 1;
		transit = packetTransit;
		transitValid = native;
		jitterQ4 = 0;
		candidateSource = 0;
	}

	void add(uint64_t extended, uint32_t packetTransit, bool repair) {
		auto &tag = seen[extended % Window];
		if (tag == extended + 1)
			return;
		tag = extended + 1;
		highest = std::max(highest, extended);
		++received;
		// Normalized RTX and synthetic FEC repair loss, but their arrival did
		// not take place on the original media stream's clock.
		if (repair)
			return;
		if (transitValid) {
			const uint32_t delta = packetTransit - transit;
			const uint64_t magnitude = delta <= UINT32_C(0x7fffffff) ?
			    delta : UINT64_C(0x100000000) - delta;
			jitterQ4 = jitterQ4 - ((jitterQ4 + 8) >> 4) + magnitude;
		}
		transit = packetTransit;
		transitValid = true;
	}
};

struct X4VideoRrState {
	explicit X4VideoRrState(bool real, uint8_t payloadType) : payloadType(payloadType) {
		if (real)
			stats = std::make_unique<X4RrVideoStats>();
	}

	const uint8_t payloadType;
	std::unique_ptr<X4RrVideoStats> stats;
	rtcX4VideoRrSnapshot last{};
	bool reporting = false;
};

// RTCP can be compound, and a control datagram can reach more than one
// track. Return the last complete SR for the media source in this datagram.
static const RtcpSr *x4FindVideoSr(const Message &message, uint32_t mediaSsrc) {
	const RtcpSr *found = nullptr;
	size_t offset = 0;
	while (message.size() - offset >= sizeof(RtcpHeader)) {
		const auto *header = reinterpret_cast<const RtcpHeader *>(message.data() + offset);
		const size_t remaining = message.size() - offset;
		const size_t bytes = header->lengthInBytes();
		if (header->version() != 2 || bytes < sizeof(RtcpHeader) || bytes > remaining)
			return nullptr;
		size_t content = bytes;
		if (header->padding()) {
			const auto padding = std::to_integer<uint8_t>(message[offset + bytes - 1]);
			if (bytes != remaining || !padding || padding > bytes - sizeof(RtcpHeader))
				return nullptr;
			content -= padding;
		}
		if (header->payloadType() == 200) {
			if (content < RtcpSr::Size(header->reportCount()))
				return nullptr;
			const auto *sr = reinterpret_cast<const RtcpSr *>(header);
			if (!mediaSsrc || sr->senderSSRC() == mediaSsrc)
				found = sr;
		}
		offset += bytes;
	}
	return offset == message.size() ? found : nullptr;
}

static uint32_t x4RrDlsr(uint64_t arrivalUs, uint64_t sendUs) {
	if (!arrivalUs || sendUs <= arrivalUs)
		return 0;
	const auto elapsedUs = sendUs - arrivalUs;
	if (elapsedUs >= UINT64_C(65536000000))
		return UINT32_MAX;
	return uint32_t(elapsedUs * 65536u / 1000000u);
}

void RtcpReceivingSession::x4ConfigureVideoRr(rtcX4VideoRrMode mode, uint8_t mediaPayloadType) {
	if (mediaPayloadType > 127 ||
	    (mode != RTC_X4_VIDEO_RR_ZERO && mode != RTC_X4_VIDEO_RR_REPAIRED &&
	     mode != RTC_X4_VIDEO_RR_EDGE))
		throw std::invalid_argument("Invalid video RR mode or payload type");
	std::lock_guard lock(mX4VideoRrMutex);
	mX4VideoRr = std::make_shared<X4VideoRrState>(mode != RTC_X4_VIDEO_RR_ZERO, mediaPayloadType);
	mX4VideoRrMode.store(static_cast<X4VideoRrMode>(mode), std::memory_order_release);
}

rtcX4VideoRrSnapshot RtcpReceivingSession::x4GetVideoRrSnapshot() const {
	std::lock_guard lock(mX4VideoRrMutex);
	return mX4VideoRr ? mX4VideoRr->last : rtcX4VideoRrSnapshot{};
}

void RtcpReceivingSession::x4ObserveEdgeVideoRr(Message &message) {
	if (mX4VideoRrMode.load(std::memory_order_acquire) != X4VideoRrEdge ||
	    message.type != Message::Binary || message.size() < sizeof(RtpHeader))
		return;
	// Recovery calls this for valid media after loss simulation and before
	// admission can discard a same-SSRC copy of an already repaired packet.
	x4ObserveVideoRr(*reinterpret_cast<const RtpHeader *>(message.data()), message, true);
}

void RtcpReceivingSession::x4ObserveVideoRr(const RtpHeader &rtp, Message &message, bool beforeRepair) {
	const auto mode = mX4VideoRrMode.load(std::memory_order_acquire);
	if (!(message.x4DiagFlags & X4_RD_AUTHENTICATED) ||
	    (mode != X4VideoRrRepaired && mode != X4VideoRrEdge) ||
	    (beforeRepair && mode != X4VideoRrEdge) ||
	    (mode == X4VideoRrEdge && (message.x4VideoRrObserved ||
	        (message.x4DiagFlags & (X4_RD_RTX_NORMALIZED | X4_RD_FEC_RECOVERED | X4_RD_RECOVERY43)))))
		return;
	std::lock_guard lock(mX4VideoRrMutex);
	if (mX4VideoRrMode.load(std::memory_order_relaxed) != mode)
		return;
	auto &state = *mX4VideoRr;
	if (rtp.payloadType() == state.payloadType) {
		const bool repair = message.x4DiagFlags & (X4_RD_RTX_NORMALIZED | X4_RD_FEC_RECOVERED);
		const auto arrival = message.x4DiagTime ? message.x4DiagTime : x4_rtc_receive_clock();
		state.stats->observe(rtp.ssrc(), rtp.seqNumber(), rtp.timestamp(), arrival, repair);
		if (beforeRepair)
			message.x4VideoRrObserved = true;
	}
}

void RtcpReceivingSession::x4PushVideoRr(const message_callback &send, const Message &message,
                                       X4VideoRrMode mode) {
	if (!(message.x4DiagFlags & X4_RD_AUTHENTICATED))
		return;
	std::unique_lock lock(mX4VideoRrMutex);
	const auto state = mX4VideoRr;
	if (!state || state->reporting || mX4VideoRrMode.load(std::memory_order_relaxed) != mode)
		return;

	const auto *stats = state->stats.get();
	if (stats && !stats->source)
		return;
	const auto *sr = x4FindVideoSr(message, stats ? stats->source : 0);
	if (!sr)
		return;

	const auto ntp = sr->ntpTimestamp();
	const uint32_t source = sr->senderSSRC();
	const uint64_t received = stats ? stats->received : 0;
	const uint64_t expected = stats ? stats->expected() : 0;
	const uint64_t epoch = stats ? stats->epoch : 0;
	rtcX4VideoRrSnapshot report{};
	report.lsr = uint32_t(ntp >> 16);
	if (stats) {
		stats->fill(report);
		report.dlsr = x4RrDlsr(message.x4DiagTime, x4_rtc_receive_clock());
	} else {
		// Keep the previous sequence-field representation in Cero. The loss
		// and jitter fields are deliberately zero in this mode.
		report.extendedSequence = uint32_t(mMaxSeq) << 16 | uint16_t(mGreatestSeqNo);
	}

	auto outgoing = make_message(RtcpRr::SizeWithReportBlocks(1), Message::Control);
	auto *rr = reinterpret_cast<RtcpRr *>(outgoing->data());
	rr->preparePacket(source, 1);
	rr->getReportBlock(0)->preparePacket(source, report.fractionLost, uint32_t(report.cumulativeLost),
	    uint16_t(report.extendedSequence), uint16_t(report.extendedSequence >> 16),
	    report.jitter, ntp, report.dlsr);
	outgoing->x4VideoRr = true;
	state->reporting = true;
	lock.unlock();
	try {
		send(outgoing);
	} catch (...) {
		lock.lock();
		state->reporting = false;
		throw;
	}
	lock.lock();
	state->reporting = false;
	if (outgoing->x4VideoRrSent) {
		report.reports = state->last.reports + 1;
		report.reportsWithLoss = state->last.reportsWithLoss + (report.fractionLost != 0);
		report.maxFractionLost = std::max(state->last.maxFractionLost, report.fractionLost);
		state->last = report;
		if (stats && stats->epoch == epoch) {
			state->stats->expectedPrior = expected;
			state->stats->receivedPrior = received;
		}
	}
}

} // namespace rtc

#endif
