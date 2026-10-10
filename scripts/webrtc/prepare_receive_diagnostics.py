#!/usr/bin/env python3
"""Apply fail-closed, idempotent receive-only observations to pinned sources.

No branch condition, RTP/RTCP feedback, queue capacity, recovery, negotiation,
or crypto policy is changed. Dependency rebuild is required: Message gains two
private numeric diagnostic fields under X4_OPENORBIS. No installed SDK changes.
"""
import pathlib
import sys


def replace(text, old, new, label):
    if new in text:
        return text
    if text.count(old) != 1:
        raise SystemExit(f"Unexpected pinned receive hook: {label} ({text.count(old)})")
    return text.replace(old, new, 1)


def apply(base):
    base = pathlib.Path(base).resolve()
    vendor = base / 'libdatachannel'
    header = pathlib.Path(__file__).resolve().parents[2] / 'src/streaming/rtc_receive_diag.h'
    out = base / 'overlay/x4_rtc_receive_diag.h'
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_bytes() != header.read_bytes():
        out.write_bytes(header.read_bytes())
    edits = {}

    def patch(path, old, new):
        p = vendor / path
        text = edits.get(p, p.read_text())
        edits[p] = replace(text, old, new, path)

    def include(path, first):
        patch(path, first, first + '\n#ifdef X4_OPENORBIS\n#include <x4_rtc_receive_diag.h>\n#endif\n')

    include('deps/libjuice/src/conn_poll.c', '#include "conn_poll.h"')
    include('deps/libjuice/src/agent.c', '#include "agent.h"')
    for path, first in [('src/impl/icetransport.cpp', '#include "icetransport.hpp"'),
                        ('src/impl/dtlstransport.cpp', '#include "dtlstransport.hpp"'),
                        ('src/impl/dtlssrtptransport.cpp', '#include "dtlssrtptransport.hpp"'),
                        ('src/impl/peerconnection.cpp', '#include "processor.hpp"'),
                        ('src/impl/track.cpp', '#include "track.hpp"'),
                        ('src/rtcpreceivingsession.cpp', '#include "rtcpreceivingsession.hpp"'),
                        ('src/capi.cpp', '#include "rtc.hpp"')]:
        include(path, first)

    patch('include/rtc/message.hpp', '\tType type;\n',
        '\tType type;\n#ifdef X4_OPENORBIS\n'
        '\tuint64_t x4DiagTime = 0; // preceding observed stage, never decoder PTS\n'
        '\tuint32_t x4DiagFlags = 0; // provenance only\n#endif\n')
    patch('src/message.cpp', '\tmessage->frameInfo = orig->frameInfo;\n',
        '\tmessage->frameInfo = orig->frameInfo;\n#ifdef X4_OPENORBIS\n'
        '\tmessage->x4DiagTime = orig->x4DiagTime;\n'
        '\tmessage->x4DiagFlags = orig->x4DiagFlags;\n#endif\n')

    # Observe successful recvfrom results before STUN/TURN/ICE filtering.
    patch('deps/libjuice/src/conn_poll.c', '\t\t// Empty datagram, ignore\n',
        '\t\t// Empty datagram, ignore\n#ifdef X4_OPENORBIS\n'
        '\t\tx4_rtc_receive_reject(X4_RD_UDP, X4_RD_EMPTY_DATAGRAM, 0, NULL, 0);\n#endif\n')
    patch('deps/libjuice/src/conn_poll.c', '\tif (len < 0) {\n\t\tif (sockerrno',
        '\tif (len < 0) {\n#ifdef X4_OPENORBIS\n'
        '\t\tconst int saved_socket_errno = sockerrno;\n'
        '\t\tx4_rtc_receive_reject(X4_RD_UDP,\n'
        '\t\t\t(saved_socket_errno == SEAGAIN || saved_socket_errno == SEWOULDBLOCK)\n'
        '\t\t\t? X4_RD_SOCKET_WOULD_BLOCK : X4_RD_SOCKET_ERROR, saved_socket_errno, NULL, 0);\n'
        '#endif\n\t\tif (sockerrno')
    patch('deps/libjuice/src/conn_poll.c', '\taddr_unmap_inet6_v4mapped((struct sockaddr *)&src->addr, &src->len);\n',
        '#ifdef X4_OPENORBIS\n'
        '\tx4_rtc_receive_packet(X4_RD_UDP, buffer, (size_t)len, 0, X4_RD_UNAUTHENTICATED);\n'
        '#endif\n\taddr_unmap_inet6_v4mapped((struct sockaddr *)&src->addr, &src->len);\n')
    patch('deps/libjuice/src/conn_poll.c', '\t\tJLOG_WARN("UDP socket error");\n',
        '\t\tJLOG_WARN("UDP socket error");\n#ifdef X4_OPENORBIS\n'
        '\t\tx4_rtc_receive_reject(X4_RD_UDP, X4_RD_SOCKET_POLL_ERROR, (int)pfd->revents, NULL, 0);\n#endif\n')
    patch('deps/libjuice/src/conn_poll.c', '\t\t\tif (agent_conn_recv(agent, context->buffer, (size_t)ret, &conn_impl->tcp_dst) != 0) {',
        '#ifdef X4_OPENORBIS\n'
        '\t\t\tx4_rtc_receive_packet(X4_RD_TCP, context->buffer, (size_t)ret, 0, X4_RD_UNAUTHENTICATED | X4_RD_TCP_FLAG);\n'
        '#endif\n\t\t\tif (agent_conn_recv(agent, context->buffer, (size_t)ret, &conn_impl->tcp_dst) != 0) {')

    # Preserve agent_input return conditions; TURN decapsulation recursively
    # reaches this same application boundary with the original outer timestamp.
    patch('deps/libjuice/src/agent.c', '\tJLOG_VERBOSE("Received datagram, size=%d", len);\n\n\tif (agent->state == JUICE_STATE_DISCONNECTED || agent->state == JUICE_STATE_GATHERING)\n\t\treturn 0;',
        '\tJLOG_VERBOSE("Received datagram, size=%d", len);\n\n\tif (agent->state == JUICE_STATE_DISCONNECTED || agent->state == JUICE_STATE_GATHERING) {\n'
        '#ifdef X4_OPENORBIS\n\t\tx4_rtc_receive_reject(X4_RD_ICE, X4_RD_ICE_INACTIVE, agent->state, buf, len);\n#endif\n'
        '\t\treturn 0;\n\t}')
    for old, reason in [('\t\t\tJLOG_ERROR("STUN message reading failed");', 'X4_RD_STUN_PARSE'),
                        ('\t\tJLOG_WARN("Received a datagram from unknown address, ignoring");', 'X4_RD_ICE_UNKNOWN_SOURCE'),
                        ('\tJLOG_WARN("Received unexpected non-STUN datagram, ignoring");', 'X4_RD_ICE_UNEXPECTED')]:
        patch('deps/libjuice/src/agent.c', old,
            old + '\n#ifdef X4_OPENORBIS\n\t\tx4_rtc_receive_reject(X4_RD_ICE, ' + reason + ', -1, buf, len);\n#endif')
    patch('deps/libjuice/src/agent.c', '\tconst addr_record_t *peer = msg->peers;\n\treturn agent_input(agent, (char *)msg->data, msg->data_size, peer, &entry->relayed);',
        '\tconst addr_record_t *peer = msg->peers;\n#ifdef X4_OPENORBIS\n'
        '\tx4_rtc_receive_packet(X4_RD_TURN, msg->data, msg->data_size, x4_rtc_receive_socket_time(), X4_RD_UNAUTHENTICATED);\n#endif\n'
        '\treturn agent_input(agent, (char *)msg->data, msg->data_size, peer, &entry->relayed);')
    patch('deps/libjuice/src/agent.c', '\treturn agent_input(agent, buf, length, &src, &entry->relayed);',
        '#ifdef X4_OPENORBIS\n\tx4_rtc_receive_packet(X4_RD_TURN, buf, length, x4_rtc_receive_socket_time(), X4_RD_UNAUTHENTICATED);\n#endif\n'
        '\treturn agent_input(agent, buf, length, &src, &entry->relayed);')
    for reason_number, old in enumerate([
        '\t\tJLOG_WARN("Received TURN Data message for a non-relay entry, ignoring");',
        '\t\tJLOG_WARN("Received non-indication TURN Data message, ignoring");',
        '\t\tJLOG_WARN("Missing data in TURN Data indication");',
        '\t\tJLOG_WARN("Missing peer address in TURN Data indication");',
        '\t\tJLOG_WARN("ChannelData is too short");',
        '\t\tJLOG_WARN("ChannelData has invalid length");',
        '\t\tJLOG_WARN("Channel not found");'], 1):
        # No media tuple is trustworthy before a valid TURN wrapper is decoded.
        patch('deps/libjuice/src/agent.c', old, old + '\n#ifdef X4_OPENORBIS\n'
            f'\t\tx4_rtc_receive_reject(X4_RD_TURN, X4_RD_TURN_INVALID, {reason_number}, NULL, 0);\n#endif')
    # Clock travels with each message through the asynchronous MbedTLS queue.
    old = '\t\tauto b = reinterpret_cast<const byte *>(data);\n\t\ticeTransport->incoming(make_message(b, b + size));'
    patch('src/impl/icetransport.cpp', old,
        '\t\tauto b = reinterpret_cast<const byte *>(data);\n'
        '\t\tauto received = make_message(b, b + size);\n#ifdef X4_OPENORBIS\n'
        '\t\treceived->x4DiagFlags = X4_RD_UNAUTHENTICATED;\n'
        '\t\treceived->x4DiagTime = x4_rtc_receive_packet(X4_RD_ICE, data, size, x4_rtc_receive_socket_time(), received->x4DiagFlags);\n'
        '#endif\n\t\ticeTransport->incoming(std::move(received));')

    # Patch only the configured MbedTLS implementation, not unavailable APIs.
    p = vendor / 'src/impl/dtlstransport.cpp'
    text = edits.get(p, p.read_text())
    start = text.index('#elif USE_MBEDTLS')
    end = text.index('#else // OPENSSL', start)
    segment = text[start:end]
    old = '\tif (mIncomingQueue.tryPush(std::move(message))) {\n\t\tenqueueRecv();\n\t} else {\n\t\tPLOG_VERBOSE << "DTLS incoming queue is full, dropping";\n\t}'
    new = '#ifdef X4_OPENORBIS\n\tunsigned char diagnosticHeader[12] = {}; // fixed clear header only\n'
    new += '\tconst auto diagnosticBytes = message->size();\n'
    new += '\tstd::memcpy(diagnosticHeader, message->data(), std::min(diagnosticBytes, sizeof(diagnosticHeader)));\n'
    new += '\tconst auto diagnosticFlags = message->x4DiagFlags;\n'
    new += '\tconst auto preceding = message->x4DiagTime;\n\tmessage->x4DiagTime = x4_rtc_receive_clock();\n#endif\n'
    new += '\tif (mIncomingQueue.tryPush(std::move(message))) {\n#ifdef X4_OPENORBIS\n'
    new += '\t\tx4_rtc_receive_packet(X4_RD_DTLS_ACCEPT, diagnosticHeader, diagnosticBytes, preceding, diagnosticFlags);\n#endif\n'
    new += '\t\tenqueueRecv();\n\t} else {\n#ifdef X4_OPENORBIS\n'
    new += '\t\tx4_rtc_receive_reject(X4_RD_DTLS_ACCEPT, X4_RD_DTLS_QUEUE_FULL, 0, diagnosticHeader, diagnosticBytes);\n#endif\n'
    new += '\t\tPLOG_VERBOSE << "DTLS incoming queue is full, dropping";\n\t}'
    segment = replace(segment, old, new, 'MbedTLS queue admit')
    old = '\t\t\tmessage_ptr message = std::move(*next);\n\t\t\tif (t->demuxMessage(message))'
    new = '\t\t\tmessage_ptr message = std::move(*next);\n#ifdef X4_OPENORBIS\n'
    new += '\t\t\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_DTLS_POP, message->data(), message->size(), message->x4DiagTime, message->x4DiagFlags);\n#endif\n'
    new += '\t\t\tif (t->demuxMessage(message))'
    segment = replace(segment, old, new, 'MbedTLS queue pop')
    edits[p] = text[:start] + segment + text[end:]

    patch('src/impl/dtlssrtptransport.cpp', '\tint size = int(message->size());\n\tif (size < 8) {',
        '\tint size = int(message->size());\n#ifdef X4_OPENORBIS\n'
        '\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_SRTP_INPUT, message->data(), message->size(), message->x4DiagTime, X4_RD_UNAUTHENTICATED);\n'
        '#endif\n\tif (size < 8) {\n#ifdef X4_OPENORBIS\n'
        '\t\tx4_rtc_receive_reject(X4_RD_SRTP_INPUT, X4_RD_MEDIA_SHORT, size, message->data(), message->size());\n#endif')
    for fn, reason in [('srtp_unprotect', 'X4_RD_SRTP_ERROR'), ('srtp_unprotect_rtcp', 'X4_RD_SRTCP_ERROR')]:
        old = f'\t\tif (srtp_err_status_t err = {fn}(mSrtpIn, message->data(), &size)) {{'
        patch('src/impl/dtlssrtptransport.cpp', old,
            old + '\n#ifdef X4_OPENORBIS\n'
            '\t\t\tunsigned reason = err == srtp_err_status_auth_fail ? X4_RD_SRTP_AUTH_FAIL :\n'
            '\t\t\t    err == srtp_err_status_replay_fail ? X4_RD_SRTP_REPLAY_FAIL :\n'
            '\t\t\t    err == srtp_err_status_replay_old ? X4_RD_SRTP_REPLAY_OLD : ' + reason + ';\n'
            '\t\t\tx4_rtc_receive_reject(' + ('X4_RD_RTCP' if fn == 'srtp_unprotect_rtcp' else 'X4_RD_SRTP_INPUT') +
            ', reason, int(err), message->data(), message->size());\n#endif')
    patch('src/impl/dtlssrtptransport.cpp', '\tmessage->resize(size);\n\tmSrtpRecvCallback(message);',
        '\tmessage->resize(size);\n#ifdef X4_OPENORBIS\n'
        '\tmessage->x4DiagFlags = X4_RD_AUTHENTICATED;\n'
        '\tmessage->x4DiagTime = x4_rtc_receive_packet(message->type == Message::Control ? X4_RD_RTCP : X4_RD_SRTP_VALID, message->data(), message->size(), message->x4DiagTime, message->x4DiagFlags);\n'
        '#endif\n\tmSrtpRecvCallback(message);')
    patch('src/impl/dtlssrtptransport.cpp', '\t\tCOUNTER_UNKNOWN_PACKET_TYPE++;\n\t\tPLOG_DEBUG << "Unknown packet type, value="',
        '\t\tCOUNTER_UNKNOWN_PACKET_TYPE++;\n#ifdef X4_OPENORBIS\n'
        '\t\tx4_rtc_receive_reject(X4_RD_SRTP_INPUT, X4_RD_DEMUX_UNKNOWN, int(value1), message->data(), message->size());\n#endif\n'
        '\t\tPLOG_DEBUG << "Unknown packet type, value="')

    patch('src/impl/peerconnection.cpp', 'void PeerConnection::forwardMedia([[maybe_unused]] message_ptr message) {\n#if RTC_ENABLE_MEDIA\n\tif (!message)\n\t\treturn;',
        'void PeerConnection::forwardMedia([[maybe_unused]] message_ptr message) {\n#if RTC_ENABLE_MEDIA\n\tif (!message)\n\t\treturn;\n'
        '#ifdef X4_OPENORBIS\n\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_PEER, message->data(), message->size(), message->x4DiagTime, message->x4DiagFlags);\n#endif')
    patch('src/impl/peerconnection.cpp', '\t\t\tPLOG_WARNING << "Exception in global incoming media handler: " << e.what();',
        '\t\t\tPLOG_WARNING << "Exception in global incoming media handler: " << e.what();\n#ifdef X4_OPENORBIS\n'
        '\t\t\tx4_rtc_receive_reject(X4_RD_PEER, X4_RD_PEER_HANDLER_EXCEPTION, 0, nullptr, 0);\n#endif')
    patch('src/impl/peerconnection.cpp', '\t\t// PLOG_WARNING << "Track not found for SSRC " << ssrc << ", dropping";',
        '\t\t// PLOG_WARNING << "Track not found for SSRC " << ssrc << ", dropping";\n#ifdef X4_OPENORBIS\n'
        '\t\tx4_rtc_receive_reject(X4_RD_PEER, X4_RD_NO_TRACK, 0, message->data(), message->size());\n#endif')
    patch('src/impl/track.cpp', 'void Track::incoming(message_ptr message) {\n\tif (!message)\n\t\treturn;',
        'void Track::incoming(message_ptr message) {\n\tif (!message)\n\t\treturn;\n#ifdef X4_OPENORBIS\n'
        '\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_TRACK, message->data(), message->size(), message->x4DiagTime, message->x4DiagFlags);\n#endif')
    patch('src/impl/track.cpp', '\t\tCOUNTER_MEDIA_BAD_DIRECTION++;\n\t\treturn;',
        '\t\tCOUNTER_MEDIA_BAD_DIRECTION++;\n#ifdef X4_OPENORBIS\n'
        '\t\tx4_rtc_receive_reject(X4_RD_TRACK, X4_RD_TRACK_DIRECTION, int(dir), message->data(), message->size());\n#endif\n\t\treturn;')
    patch('src/impl/track.cpp', '\t\t\tPLOG_WARNING << "Exception in incoming media handler: " << e.what();',
        '\t\t\tPLOG_WARNING << "Exception in incoming media handler: " << e.what();\n#ifdef X4_OPENORBIS\n'
        '\t\t\tx4_rtc_receive_reject(X4_RD_TRACK, X4_RD_TRACK_HANDLER_EXCEPTION, 0, nullptr, 0);\n#endif')
    patch('src/impl/track.cpp', '\t\t\tCOUNTER_QUEUE_FULL++;\n\t\t\treturn;',
        '\t\t\tCOUNTER_QUEUE_FULL++;\n#ifdef X4_OPENORBIS\n'
        '\t\t\tx4_rtc_receive_reject(X4_RD_TRACK_QUEUE, X4_RD_TRACK_QUEUE_FULL, 0, m->data(), m->size());\n#endif\n\t\t\treturn;')
    patch('src/impl/track.cpp', '\t\tmRecvQueue.push(m);',
        '#ifdef X4_OPENORBIS\n\t\tm->x4DiagTime = x4_rtc_receive_packet(X4_RD_TRACK_QUEUE, m->data(), m->size(), m->x4DiagTime, m->x4DiagFlags);\n#endif\n'
        '\t\tmRecvQueue.push(m);')
    patch('src/impl/track.cpp', '\tif (auto next = mRecvQueue.pop()) {\n\t\treturn trackMessageToVariant(*next);',
        '\tif (auto next = mRecvQueue.pop()) {\n#ifdef X4_OPENORBIS\n'
        '\t\t(*next)->x4DiagTime = x4_rtc_receive_packet(X4_RD_TRACK_POP, (*next)->data(), (*next)->size(), (*next)->x4DiagTime, (*next)->x4DiagFlags);\n#endif\n'
        '\t\treturn trackMessageToVariant(*next);')
    patch('src/impl/track.cpp', '\t\tauto message = next.value();\n\t\ttry {',
        '\t\tauto message = next.value();\n#ifdef X4_OPENORBIS\n'
        '\t\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_TRACK_POP, message->data(), message->size(), message->x4DiagTime, message->x4DiagFlags);\n#endif\n'
        '\t\ttry {')

    # RTX behavior/configuration is observed without enabling retransmission.
    patch('include/rtc/rtcpreceivingsession.hpp', '\tSyncTimestamps getSyncTimestamps();',
        '\tSyncTimestamps getSyncTimestamps();\n#ifdef X4_OPENORBIS\n'
        '\tvoid x4DiagnosticSnapshot(unsigned track); // owner-only numeric extension\n#endif')
    patch('src/rtcpreceivingsession.cpp', 'message_ptr RtcpReceivingSession::unwrapRtx(const message_ptr &rtxPacket) {',
        '#ifdef X4_OPENORBIS\nvoid RtcpReceivingSession::x4DiagnosticSnapshot(unsigned track) {\n'
        '\tstd::lock_guard lock(mMutex);\n'
        '\tx4_rtc_receive_setting(4, track, (mRtxEnabled ? 1u : 0u) | (mSupportsRfc5104Fir ? 2u : 0u), mRtxPrimarySsrc);\n'
        '\tfor (const auto &entry : mRtxToPrimaryPtMap) x4_rtc_receive_setting(5, track, entry.first, entry.second);\n'
        '}\n#endif\n\nmessage_ptr RtcpReceivingSession::unwrapRtx(const message_ptr &rtxPacket) {')
    patch('src/capi.cpp', 'int rtcChainRtcpReceivingSession(int tr) {',
        '#ifdef X4_OPENORBIS\nextern "C" int x4_rtc_receive_track_config(int tr) {\n'
        '\treturn wrap([&] { getRtcpReceivingSession(tr)->x4DiagnosticSnapshot(unsigned(tr)); return RTC_ERR_SUCCESS; });\n'
        '}\n#endif\n\nint rtcChainRtcpReceivingSession(int tr) {')
    patch('src/rtcpreceivingsession.cpp', '\tmRtxPrimarySsrc = newRtxPrimarySsrc;\n',
        '\tmRtxPrimarySsrc = newRtxPrimarySsrc;\n#ifdef X4_OPENORBIS\n'
        '\tx4_rtc_receive_setting(1, mRtxEnabled ? 1 : 0, mRtxPrimarySsrc, mRtxToPrimaryPtMap.size());\n'
        '\tfor (const auto &entry : mRtxToPrimaryPtMap) x4_rtc_receive_setting(2, entry.first, entry.second, 0);\n#endif\n')
    # Record wire RTX identity before normalization, original identity after.
    patch('src/rtcpreceivingsession.cpp', '\t\t\t\t// RTX packet\n\t\t\t\tauto unwrapped = unwrapRtx(message);\n\t\t\t\tif (unwrapped)\n\t\t\t\t\tmessage = unwrapped;',
        '\t\t\t\t// RTX packet\n#ifdef X4_OPENORBIS\n'
        '\t\t\t\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_PEER_OUT, message->data(), message->size(), message->x4DiagTime, X4_RD_AUTHENTICATED | X4_RD_RETRANSMISSION);\n#endif\n'
        '\t\t\t\tauto unwrapped = unwrapRtx(message);\n\t\t\t\tif (unwrapped) {\n#ifdef X4_OPENORBIS\n'
        '\t\t\t\t\tunwrapped->x4DiagFlags = X4_RD_AUTHENTICATED | X4_RD_RTX_NORMALIZED;\n'
        '\t\t\t\t\tauto primary = reinterpret_cast<const RtpHeader *>(unwrapped->data());\n'
        '\t\t\t\t\tx4_rtc_receive_setting(3, (uint64_t(rtp->ssrc()) << 32) | (uint64_t(rtp->payloadType()) << 16) | rtp->seqNumber(), (uint64_t(primary->ssrc()) << 32) | (uint64_t(primary->payloadType()) << 16) | primary->seqNumber(), 0);\n'
        '\t\t\t\t\tunwrapped->x4DiagTime = x4_rtc_receive_packet(X4_RD_RTX, unwrapped->data(), unwrapped->size(), message->x4DiagTime, unwrapped->x4DiagFlags);\n#endif\n'
        '\t\t\t\t\tmessage = unwrapped;\n\t\t\t\t} else {\n#ifdef X4_OPENORBIS\n'
        '\t\t\t\t\tx4_rtc_receive_reject(X4_RD_PEER_OUT, X4_RD_RTX_UNWRAP_UNAVAILABLE, 0, message->data(), message->size());\n#endif\n\t\t\t\t}')
    for old, reason in [('\t\t\t\tPLOG_VERBOSE << "RTP packet is too small, size=" << message->size();', 'X4_RD_RTP_SHORT'),
                        ('\t\t\t\tPLOG_VERBOSE << "RTP packet is not version 2";', 'X4_RD_RTP_VERSION'),
                        ('\t\t\t\tPLOG_VERBOSE << "RTP packet has a payload type indicating RR/SR";', 'X4_RD_RTP_PAYLOAD')]:
        patch('src/rtcpreceivingsession.cpp', old,
            old + '\n#ifdef X4_OPENORBIS\n\t\t\t\tx4_rtc_receive_reject(X4_RD_PEER_OUT, ' + reason + ', 0, message->data(), message->size());\n#endif')
    patch('src/rtcpreceivingsession.cpp', '\t\t\tresult.push_back(std::move(message));\n\t\t\tbreak;',
        '#ifdef X4_OPENORBIS\n\t\t\tmessage->x4DiagTime = x4_rtc_receive_packet(X4_RD_PEER_OUT, message->data(), message->size(), message->x4DiagTime, message->x4DiagFlags);\n#endif\n'
        '\t\t\tresult.push_back(std::move(message));\n\t\t\tbreak;')

    for path, text in edits.items():
        if path.read_text() != text:
            path.write_text(text)
    return len(edits)


if __name__ == '__main__':
    print(f'Applied/verified receive observations in {apply(sys.argv[1])} pinned files')
