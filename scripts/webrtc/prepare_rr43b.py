#!/usr/bin/env python3
"""Apply the video-only RR overlay to the pinned OpenOrbis libdatachannel."""
import pathlib
import subprocess
import sys

PIN = 'bdc5ff28e9d3b863144c94a677ecf5bf043aaf15'


def apply(base):
    vendor = pathlib.Path(base).resolve() / 'libdatachannel'
    if (vendor / '.git').exists():
        head = subprocess.check_output(
            ['git', '-C', str(vendor), 'rev-parse', 'HEAD'], text=True).strip()
        if head != PIN:
            raise SystemExit(f'Unexpected libdatachannel revision: {head}')

    edits = {}

    def patch(path, old, new):
        path = vendor / path
        text = edits[path] if path in edits else path.read_text(encoding='utf-8')
        if text.count(new) == 1:
            return
        if text.count(old) != 1 or new in text:
            raise SystemExit(f'Unexpected pinned RR anchor in {path.name}')
        edits[path] = text.replace(old, new, 1)

    old = 'RTC_C_EXPORT int rtcChainRtcpReceivingSession(int tr);\n'
    patch('include/rtc/rtc.h', old, old + '''
#ifdef X4_OPENORBIS
typedef enum rtcX4VideoRrMode {
	RTC_X4_VIDEO_RR_ZERO = 0,
	RTC_X4_VIDEO_RR_REPAIRED = 1,
	RTC_X4_VIDEO_RR_EDGE = 2,
} rtcX4VideoRrMode;

typedef struct rtcX4VideoRrSnapshot {
	uint64_t reports; // reports accepted by the local transport
	uint32_t extendedSequence;
	int32_t cumulativeLost;
	uint32_t jitter; // 90 kHz RTP units
	uint32_t lsr;
	uint32_t dlsr;
	uint8_t fractionLost;
	uint64_t reportsWithLoss;
	uint8_t maxFractionLost;
} rtcX4VideoRrSnapshot;

RTC_C_EXPORT int rtcX4ConfigureVideoRr(int tr, rtcX4VideoRrMode mode, uint8_t mediaPayloadType);
RTC_C_EXPORT int rtcX4GetVideoRrSnapshot(int tr, rtcX4VideoRrSnapshot *out);
#endif
''')
    old = '\tunsigned int stream = 0; // Stream id (SCTP stream or SSRC)\n'
    patch('include/rtc/message.hpp', old, old + '''
#ifdef X4_OPENORBIS
	bool x4VideoRr = false;
	bool x4VideoRrObserved = false; // native packet observed before recovery admission
	bool x4VideoRrSent = false; // synchronous result on this original message
#endif
''')
    old = '#include "rtp.hpp"\n'
    patch('include/rtc/rtcpreceivingsession.hpp', old, old + '''
#ifdef X4_OPENORBIS
#include "rtc.h"
#endif
''')
    old = 'namespace rtc {\n'
    patch('include/rtc/rtcpreceivingsession.hpp', old, old + '''
#ifdef X4_OPENORBIS
struct X4VideoRrState;
#endif
''')
    old = 'protected:\n\tvoid pushREMB'
    patch('include/rtc/rtcpreceivingsession.hpp', old, '''#ifdef X4_OPENORBIS
	void x4ConfigureVideoRr(rtcX4VideoRrMode mode, uint8_t mediaPayloadType);
	rtcX4VideoRrSnapshot x4GetVideoRrSnapshot() const;
	void x4ObserveEdgeVideoRr(Message &message);

private:
	enum X4VideoRrMode : uint8_t {
		X4VideoRrZero = RTC_X4_VIDEO_RR_ZERO,
		X4VideoRrRepaired = RTC_X4_VIDEO_RR_REPAIRED,
		X4VideoRrEdge = RTC_X4_VIDEO_RR_EDGE,
		X4VideoRrUpstream,
	};
	void x4ObserveVideoRr(const RtpHeader &rtp, Message &message, bool beforeRepair = false);
	void x4PushVideoRr(const message_callback &send, const Message &message, X4VideoRrMode mode);
	std::atomic<X4VideoRrMode> mX4VideoRrMode{X4VideoRrUpstream};
	mutable std::mutex mX4VideoRrMutex;
	std::shared_ptr<X4VideoRrState> mX4VideoRr;
#endif
''' + old)

    old = 'namespace rtc {\n'
    patch('src/rtcpreceivingsession.cpp', old, '''
#ifdef X4_OPENORBIS
#include "x4_video_rr.hpp"
#endif

''' + old)
    old = '\t\t\tupdateSeq(rtp->seqNumber());\n'
    patch('src/rtcpreceivingsession.cpp', old, old + '''
#ifdef X4_OPENORBIS
			x4ObserveVideoRr(*rtp, *message);
#endif
''')
    old = '\t\t\tauto header = reinterpret_cast<const RtcpHeader *>(message->data());\n'
    patch('src/rtcpreceivingsession.cpp', old, old + '''
#ifdef X4_OPENORBIS
			const auto rrMode = mX4VideoRrMode.load(std::memory_order_acquire);
			if (rrMode != X4VideoRrUpstream && rrMode != X4VideoRrZero && header->payloadType() != 200)
				x4PushVideoRr(send, *message, rrMode);
#endif
''')
    old = '\t\t\t\tpushRR(send, 0);\n'
    patch('src/rtcpreceivingsession.cpp', old, '''
#ifdef X4_OPENORBIS
				if (rrMode != X4VideoRrUpstream)
					x4PushVideoRr(send, *message, rrMode);
				else
#endif
					pushRR(send, 0);
''')

    old = '\treturn transport->sendMedia(message);\n'
    patch('src/impl/track.cpp', old, '''
#ifdef X4_OPENORBIS
	if (message->x4VideoRr) {
		message->x4VideoRrSent = transport->sendMedia(message);
		return message->x4VideoRrSent;
	}
#endif
''' + old)
    old = 'int rtcChainRtcpSrReporter(int tr) {\n'
    patch('src/capi.cpp', old, '''
#ifdef X4_OPENORBIS
int rtcX4ConfigureVideoRr(int tr, rtcX4VideoRrMode mode, uint8_t mediaPayloadType) {
	return wrap([&] {
		if (getTrack(tr)->description().type() != "video")
			return RTC_ERR_INVALID;
		getRtcpReceivingSession(tr)->x4ConfigureVideoRr(mode, mediaPayloadType);
		return RTC_ERR_SUCCESS;
	});
}
int rtcX4GetVideoRrSnapshot(int tr, rtcX4VideoRrSnapshot *out) {
	return wrap([&] {
		if (!out)
			return RTC_ERR_INVALID;
		*out = getRtcpReceivingSession(tr)->x4GetVideoRrSnapshot();
		return RTC_ERR_SUCCESS;
	});
}
#endif

''' + old)

    implementation = pathlib.Path(__file__).with_name('rr43b_impl.hpp').read_bytes()
    generated = vendor / 'src/x4_video_rr.hpp'
    if generated.exists() and generated.read_bytes() != implementation:
        raise SystemExit('Unexpected generated video RR overlay')

    for path, text in edits.items():
        path.write_text(text, encoding='utf-8')
    if not generated.exists():
        generated.write_bytes(implementation)


if __name__ == '__main__':
    apply(sys.argv[1])
