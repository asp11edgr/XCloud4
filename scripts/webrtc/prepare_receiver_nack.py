#!/usr/bin/env python3
"""Explicit, fail-closed receiver NACK overlay for pinned libdatachannel.

The handler queues bounded original-video requests after ReceivingSession.
Only an owner dispatcher sends through existing Track/RTCP/SRTCP transport.
No SDP, RTX, crypto policy, application queue or reorder setting is changed.
"""
import pathlib
import sys


FIELDS = (
    ('uint64_t', 'Identity'), ('uint64_t', 'Source'), ('uint64_t', 'First'),
    ('uint64_t', 'Last'), ('uint64_t', 'Detected'), ('uint64_t', 'Requested'),
    ('uint64_t', 'Dispatch'), ('uint64_t', 'Extended'),
    ('uint32_t', 'RtpTimestamp'), ('uint32_t', 'ReturnFlags'),
)


def replace(text, old, new, label):
    if new in text:
        return text
    if text.count(old) != 1:
        raise SystemExit(f'Unexpected pinned NACK anchor: {label} ({text.count(old)})')
    return text.replace(old, new, 1)


def apply(base):
    base = pathlib.Path(base).resolve()
    vendor = base / 'libdatachannel'
    project = pathlib.Path(__file__).resolve().parents[2]
    source = project / 'src/streaming'
    edits = {}

    def patch(relative, old, new):
        path = vendor / relative
        text = edits.get(path, path.read_text(encoding='utf-8'))
        edits[path] = replace(text, old, new, relative)

    # Keep the complete preceding diagnostic replacements intact so that
    # prepare_receive_diagnostics remains idempotent on the combined overlay.
    old = '\tuint32_t x4DiagFlags = 0; // provenance only\n#endif\n'
    declarations = ''.join(f'\t{kind} x4Nack{name} = 0;\n' for kind, name in FIELDS)
    patch('include/rtc/message.hpp', old, old + '#ifdef X4_OPENORBIS\n' + declarations + '#endif\n')
    old = '\tmessage->x4DiagFlags = orig->x4DiagFlags;\n#endif\n'
    copies = ''.join(f'\tmessage->x4Nack{name} = orig->x4Nack{name};\n' for _, name in FIELDS)
    patch('src/message.cpp', old, old + '#ifdef X4_OPENORBIS\n' + copies + '#endif\n')
    patch('src/impl/track.cpp',
        'message_variant Track::trackMessageToVariant(message_ptr message) {',
        'message_variant Track::trackMessageToVariant(message_ptr message) {\n#ifdef X4_OPENORBIS\n'
        '\tx4_rtc_nack_delivery(message->x4NackIdentity, message->x4NackSource,\n'
        '\t    message->x4NackExtended, message->x4NackRtpTimestamp);\n#endif')
    old = '\t\t\t\tif (auto locked = weak_this.lock()) {\n\t\t\t\t\tlocked->transportSend(m);\n\t\t\t\t}\n'
    patch('src/impl/track.cpp', old,
        '\t\t\t\tif (auto locked = weak_this.lock()) {\n'
        '#ifdef X4_OPENORBIS\n'
        '\t\t\t\t\tbool localResult = locked->transportSend(m);\n'
        '\t\t\t\t\tif (m->x4NackIdentity) x4_rtc_nack_event(23, 0, m->x4NackIdentity, m->x4NackSource, localResult ? 1 : 0, m->size(), m->x4NackDispatch);\n'
        '#else\n\t\t\t\t\tlocked->transportSend(m);\n#endif\n'
        '\t\t\t\t}\n')
    patch('include/rtc/track.hpp', 'private:\n\tusing CheshireCat<impl::Track>::impl;',
        '#ifdef X4_OPENORBIS\n'
        '\tbool x4SendReceiverNack(message_ptr message); // explicit protected vendor route\n'
        '#endif\nprivate:\n\tusing CheshireCat<impl::Track>::impl;')
    patch('src/track.cpp', '} // namespace rtc',
        '#ifdef X4_OPENORBIS\n'
        'bool Track::x4SendReceiverNack(message_ptr message) {\n'
        '\tif (impl()->isClosed()) return false;\n'
        '\treturn impl()->transportSend(std::move(message));\n'
        '}\n#endif\n\n} // namespace rtc')

    old = '\t\tif (srtp_err_status_t err = srtp_protect_rtcp(mSrtpOut, message->data(), &size)) {'
    patch('src/impl/dtlssrtptransport.cpp', old,
        '#ifdef X4_OPENORBIS\n'
        '\t\tif (message->x4NackIdentity) x4_rtc_nack_event(21, 0, message->x4NackIdentity, message->x4NackSource, 0, size, message->x4NackDispatch);\n'
        '#endif\n'
        '\t\tsrtp_err_status_t protectStatus = srtp_protect_rtcp(mSrtpOut, message->data(), &size);\n'
        '#ifdef X4_OPENORBIS\n'
        '\t\tif (message->x4NackIdentity) x4_rtc_nack_event(22, 0, message->x4NackIdentity, message->x4NackSource, uint64_t(int64_t(protectStatus)), size, message->x4NackDispatch);\n'
        '#endif\n'
        '\t\tif (srtp_err_status_t err = protectStatus) {')
    patch('src/capi.cpp', '#include <type_traits>',
        '#include <type_traits>\n#ifdef X4_OPENORBIS\n#include <rtc_receiver_nack.hpp>\n#endif')
    capi = vendor / 'src/capi.cpp'
    text = edits[capi]
    marker = '// XCloud4 bounded receiver NACK owner API.\n'
    owner_api = '\n#ifdef X4_OPENORBIS\n#if RTC_ENABLE_MEDIA\n' + marker + r'''
extern "C" int x4_rtc_configure_receiver_nack(int tr, int enabled, uint32_t epoch) {
    return wrap([&] {
        auto track = getTrack(tr);
        auto existing = track->getMediaHandler();
        if (auto generator = std::dynamic_pointer_cast<X4ReceiverNack>(existing)) {
            generator->duplicateInstall();
            if (!generator->sameConfiguration(enabled != 0, epoch))
                throw std::invalid_argument("Receiver NACK already configured");
            return RTC_ERR_SUCCESS;
        }
        // incomingChain executes last first: root generator -> ReceivingSession
        // therefore means ReceivingSession validation -> generator observation.
        if (!std::dynamic_pointer_cast<RtcpReceivingSession>(existing) || existing->next() ||
            track->description().type() != "video")
            throw std::invalid_argument("Unexpected receiver NACK handler chain");
        auto generator = std::make_shared<X4ReceiverNack>(enabled != 0, epoch);
        generator->setNext(existing);
        track->setMediaHandler(generator);
        return RTC_ERR_SUCCESS;
    });
}
extern "C" int x4_rtc_receiver_nack_accept(int tr, int pc) {
    return wrap([&] {
        auto track = getTrack(tr);
        auto generator = std::dynamic_pointer_cast<X4ReceiverNack>(track->getMediaHandler());
        if (!generator) throw std::invalid_argument("Receiver NACK is absent");
        auto answer = getPeerConnection(pc)->remoteDescription();
        if (!answer || answer->type() != Description::Type::Answer)
            throw std::invalid_argument("Receiver NACK answer is absent");
        bool found = false, accepted = false;
        for (int i = 0; i < answer->mediaCount(); ++i) {
            auto value = answer->media(i);
            if (!std::holds_alternative<Description::Media *>(value)) continue;
            auto media = std::get<Description::Media *>(value);
            if (media->mid() != track->mid()) continue;
            if (found) throw std::invalid_argument("Duplicate receiver NACK MID");
            found = true;
            auto rtp = media->rtpMap(X4_RECEIVER_NACK_VIDEO_PT);
            accepted = media->type() == "video" && !media->isRemoved() && rtp &&
                x4ReceiverNackAcceptsNack(rtp->format, rtp->rtcpFbs);
        }
        if (!found) throw std::invalid_argument("Receiver NACK answer track is absent");
        generator->acceptedFeedback(accepted);
        return RTC_ERR_SUCCESS;
    });
}
extern "C" int x4_rtc_poll_receiver_nack(int tr) {
    return wrap([&] {
        auto track = getTrack(tr);
        auto generator = std::dynamic_pointer_cast<X4ReceiverNack>(track->getMediaHandler());
        if (!generator) return RTC_ERR_NOT_AVAIL;
        return generator->poll([&](message_ptr message) {
            return track->x4SendReceiverNack(std::move(message));
        });
    });
}
extern "C" int x4_rtc_close_receiver_nack(int tr) {
    return wrap([&] {
        auto generator = std::dynamic_pointer_cast<X4ReceiverNack>(getTrack(tr)->getMediaHandler());
        return generator ? generator->close() : RTC_ERR_SUCCESS;
    });
}
''' + '#endif\n#endif\n'
    # Permit only the complete prior experimental body for this explicit
    # migration. A marker alone does not prove all four owner APIs exist.
    legacy_api = owner_api.replace(
        'x4ReceiverNackAcceptsNack(rtp->format, rtp->rtcpFbs);',
        'rtp->format == "H264" && rtp->hasFeedback("nack");')
    if marker not in text:
        text += owner_api
    elif text.count(marker) != 1:
        raise SystemExit('Duplicate receiver NACK owner API marker')
    elif text.endswith(owner_api):
        pass
    elif text.endswith(legacy_api):
        text = text[:-len(legacy_api)] + owner_api
    else:
        raise SystemExit('Incomplete or unknown receiver NACK owner API body')
    edits[capi] = text
    old = '\t${CMAKE_CURRENT_SOURCE_DIR}/src/rtcpreceivingsession.cpp\n'
    patch('CMakeLists.txt', old, old + '\t${CMAKE_CURRENT_SOURCE_DIR}/src/rtc_receiver_nack.cpp\n')

    # Complete anchor validation precedes any vendor source mutation.
    for path, text in edits.items():
        if path.read_text(encoding='utf-8') != text:
            path.write_text(text, encoding='utf-8', newline='\n')
    for name in ('receiver_nack_state.h', 'rtc_receiver_nack.hpp'):
        target = base / 'overlay' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        data = (source / name).read_bytes()
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    target = vendor / 'src/rtc_receiver_nack.cpp'
    data = (source / 'rtc_receiver_nack.cpp').read_bytes()
    if not target.exists() or target.read_bytes() != data:
        target.write_bytes(data)
    return len(edits)


if __name__ == '__main__':
    print(f'Applied/verified receiver NACK overlay in {apply(sys.argv[1])} pinned files')
