/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_transport.h"
#include "rtc_native.h"
#include "../media/live_trace.h"
#include "../auth/json.h"
#include <rtc/rtc.h>
#include <orbis/libkernel.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDP_CAP 32768u
#define CANDIDATE_CAP 1024u
#define MID_CAP 32u
#define CANDIDATE_COUNT 64u
#define CALLBACK_SLOTS 64u
#define INPUT_INTERVAL_USEC 16667u

_Static_assert(sizeof(double) == 8, "Xbox input uses a float64 timestamp");

typedef struct { char candidate[CANDIDATE_CAP], mid[MID_CAP]; } Candidate;
typedef struct { X4Rtc *context; unsigned active; } CallbackSlot;
static CallbackSlot slots[CALLBACK_SLOTS];
static unsigned slots_used;
static atomic_flag callback_gate = ATOMIC_FLAG_INIT;

struct X4Rtc {
    int pc, video, audio, channels[4];
    CallbackSlot *slot;
    atomic_int state, error;
    atomic_bool gathering_done, offer_logged;
    atomic_uint_fast64_t video_packets, audio_packets;
    atomic_flag data_gate;
    char sdp[SDP_CAP];
    Candidate candidates[CANDIDATE_COUNT];
    unsigned candidate_read, candidate_write;
    X4RtcMediaCallback media;
    void *media_user;
    X4Trace *diagnostic_trace;
    uint64_t diagnostic_pli_ordinal; /* Session owner is the sole writer. */
    bool handshake_ack, startup_sent;
    uint64_t started_at;
    X4GamepadSource input_source;
    void *input_user;
    OrbisPthread input_thread;
    bool input_thread_started;
    int input_join_error;
    uint32_t input_sequence;
    atomic_bool input_stop, input_ready, input_metadata_sent, startup_complete;
    atomic_int input_error;
    atomic_uint_fast64_t input_packets, input_dropped;
};

static void lock(atomic_flag *flag) { while (atomic_flag_test_and_set_explicit(flag, memory_order_acquire)) {} }
static void unlock(atomic_flag *flag) { atomic_flag_clear_explicit(flag, memory_order_release); }

static X4Rtc *enter(void *pointer)
{
    CallbackSlot *slot = pointer;
    if (!slot) return NULL;
    lock(&callback_gate);
    X4Rtc *context = slot->context;
    if (context) ++slot->active;
    unlock(&callback_gate);
    return context;
}
static void leave(CallbackSlot *slot)
{
    lock(&callback_gate);
    --slot->active;
    unlock(&callback_gate);
}
static void fail(X4Rtc *rtc, int error)
{
    atomic_store(&rtc->error, error);
    atomic_store(&rtc->state, X4_RTC_FAILED);
}

/* Structural diagnostics only. Never print input strings: the SDP contains
 * addresses, credentials, certificate fingerprints and private identifiers. */
typedef struct { const char *p; size_t n; } SdpSpan;
typedef struct {
    unsigned kind, rtp, pt_count, pt_overflow, pts[16];
    unsigned mid_type, mid, direction, setup, trickle, feedback;
    unsigned fb_nack, fb_pli, fb_fir, fb_remb, profile_found, profile;
    int port, h264_pt, opus_pt, sctp_port;
    SdpSpan mid_span;
} SdpShape;

static bool sdp_equal(SdpSpan s, const char *literal)
{
    size_t n = strlen(literal);
    return s.n == n && !memcmp(s.p, literal, n);
}
static bool sdp_prefix(SdpSpan s, const char *literal)
{
    size_t n = strlen(literal);
    return s.n >= n && !memcmp(s.p, literal, n);
}
static SdpSpan sdp_after(SdpSpan s, size_t n)
{
    return n <= s.n ? (SdpSpan){s.p + n, s.n - n} : (SdpSpan){s.p, 0};
}
static SdpSpan sdp_token(SdpSpan *rest)
{
    size_t first = 0, end;
    while (first < rest->n && (rest->p[first] == ' ' || rest->p[first] == '\t')) ++first;
    end = first;
    while (end < rest->n && rest->p[end] != ' ' && rest->p[end] != '\t') ++end;
    SdpSpan token = {rest->p + first, end - first};
    *rest = sdp_after(*rest, end);
    return token;
}
static bool sdp_number(SdpSpan token, unsigned maximum, unsigned *number)
{
    if (!token.n) return false;
    unsigned value = 0;
    for (size_t i = 0; i < token.n; ++i) {
        unsigned digit = (unsigned char)token.p[i] - (unsigned)'0';
        if (digit > 9 || digit > maximum || value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *number = value;
    return true;
}
static unsigned sdp_setup(SdpSpan value)
{
    return sdp_equal(value, "actpass") ? 1 : sdp_equal(value, "active") ? 2 :
        sdp_equal(value, "passive") ? 3 : sdp_equal(value, "holdconn") ? 4 : 5;
}
static bool sdp_trickle(SdpSpan rest)
{
    while (rest.n) if (sdp_equal(sdp_token(&rest), "trickle")) return true;
    return false;
}
static void sdp_profile(SdpShape *m, SdpSpan line)
{
    static const char key[] = "profile-level-id=";
    for (size_t i = 0; i + sizeof(key) - 1 + 6 <= line.n; ++i) {
        if (i && line.p[i - 1] != ';' && line.p[i - 1] != ' ' && line.p[i - 1] != '\t') continue;
        if (memcmp(line.p + i, key, sizeof(key) - 1)) continue;
        size_t start = i + sizeof(key) - 1;
        unsigned value = 0;
        bool valid = true;
        for (size_t j = 0; j < 6; ++j) {
            unsigned char c = (unsigned char)line.p[start + j];
            unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit > 15) { valid = false; break; }
            value = value * 16 + digit;
        }
        size_t end = start + 6;
        if (end < line.n && line.p[end] != ';' && line.p[end] != ' ' && line.p[end] != '\t') valid = false;
        if (valid) { m->profile_found = 1; m->profile = value; }
    }
}
static void sdp_shape_log(const char *sdp, size_t length)
{
    SdpShape media[8] = {0};
    SdpSpan bundle[8] = {{0}};
    unsigned count = 0, stored = 0, candidates = 0, bundle_count = 0;
    unsigned ssrc_lines = 0, bare_ssrc = 0;
    unsigned session_setup = 0, session_trickle = 0, bundle_unique = 1, bundle_matched = 0;
    SdpShape *m = NULL;
    for (size_t offset = 0; offset < length;) {
        size_t end = offset;
        while (end < length && sdp[end] != '\n') ++end;
        size_t line_end = end;
        if (line_end > offset && sdp[line_end - 1] == '\r') --line_end;
        SdpSpan line = {sdp + offset, line_end - offset};
        offset = end < length ? end + 1 : end;
        if (sdp_prefix(line, "m=")) {
            ++count;
            m = stored < 8 ? &media[stored++] : NULL;
            if (!m) continue;
            *m = (SdpShape){.port = -1, .h264_pt = -1, .opus_pt = -1, .sctp_port = -1,
                .setup = session_setup, .trickle = session_trickle};
            SdpSpan rest = sdp_after(line, 2), type = sdp_token(&rest);
            m->kind = sdp_equal(type, "video") ? 1 : sdp_equal(type, "audio") ? 2 :
                sdp_equal(type, "application") ? 3 : 4;
            unsigned number;
            if (sdp_number(sdp_token(&rest), 65535, &number)) m->port = (int)number;
            SdpSpan protocol = sdp_token(&rest);
            m->rtp = sdp_equal(protocol, "UDP/TLS/RTP/SAVPF") || sdp_equal(protocol, "RTP/SAVPF");
            if (m->rtp) while (rest.n) {
                SdpSpan pt = sdp_token(&rest);
                if (!pt.n) break;
                if (!sdp_number(pt, 127, &number)) { m->pt_overflow = 1; continue; }
                if (m->pt_count < 16) m->pts[m->pt_count++] = number;
                else m->pt_overflow = 1;
            }
            continue;
        }
        if (sdp_prefix(line, "a=candidate:")) { ++candidates; continue; }
        if (sdp_prefix(line, "a=ssrc:")) {
            ++ssrc_lines;
            SdpSpan rest = sdp_after(line, 7);
            (void)sdp_token(&rest); /* The source ID stays private. */
            if (!sdp_token(&rest).n) ++bare_ssrc;
            continue;
        }
        if (sdp_prefix(line, "a=group:BUNDLE")) {
            SdpSpan rest = sdp_after(line, 14);
            while (rest.n) {
                SdpSpan token = sdp_token(&rest);
                if (!token.n) break;
                for (unsigned i = 0; i < bundle_count && i < 8; ++i)
                    if (bundle[i].n == token.n && !memcmp(bundle[i].p, token.p, token.n)) bundle_unique = 0;
                if (bundle_count < 8) bundle[bundle_count] = token;
                ++bundle_count;
            }
            continue;
        }
        if (sdp_prefix(line, "a=setup:")) {
            unsigned setup = sdp_setup(sdp_after(line, 8));
            if (m) m->setup = setup; else if (!count) session_setup = setup;
        } else if (sdp_prefix(line, "a=ice-options:")) {
            unsigned trickle = sdp_trickle(sdp_after(line, 14));
            if (m) m->trickle = trickle; else if (!count) session_trickle = trickle;
        } else if (m) {
            unsigned number;
            if (sdp_prefix(line, "a=mid:")) {
                m->mid_span = sdp_after(line, 6);
                m->mid_type = sdp_number(m->mid_span, 65535, &m->mid) ? 1 : 2;
            } else if (sdp_equal(line, "a=recvonly")) m->direction = 1;
            else if (sdp_equal(line, "a=sendonly")) m->direction = 2;
            else if (sdp_equal(line, "a=sendrecv")) m->direction = 3;
            else if (sdp_equal(line, "a=inactive")) m->direction = 4;
            else if (sdp_prefix(line, "a=rtpmap:")) {
                SdpSpan rest = sdp_after(line, 9), pt = sdp_token(&rest), codec = sdp_token(&rest);
                if (sdp_number(pt, 127, &number)) {
                    if (sdp_equal(codec, "H264/90000")) m->h264_pt = (int)number;
                    if (sdp_equal(codec, "opus/48000/2")) m->opus_pt = (int)number;
                }
            } else if (sdp_prefix(line, "a=rtcp-fb:")) {
                ++m->feedback;
                SdpSpan rest = sdp_after(line, 10);
                (void)sdp_token(&rest);
                while (rest.n && (rest.p[0] == ' ' || rest.p[0] == '\t')) rest = sdp_after(rest, 1);
                m->fb_nack |= sdp_equal(rest, "nack"); m->fb_pli |= sdp_equal(rest, "nack pli");
                m->fb_fir |= sdp_equal(rest, "ccm fir"); m->fb_remb |= sdp_equal(rest, "goog-remb");
            } else if (sdp_prefix(line, "a=fmtp:")) sdp_profile(m, line);
            else if (sdp_prefix(line, "a=sctp-port:") && sdp_number(sdp_after(line, 12), 65535, &number))
                m->sctp_port = (int)number;
        }
    }
    if (bundle_count > 8) bundle_unique = 2; /* Unknown when the diagnostic bound is exceeded. */
    for (unsigned i = 0; i < bundle_count && i < 8; ++i)
        for (unsigned j = 0; j < stored; ++j)
            if (media[j].mid_span.n == bundle[i].n && !memcmp(media[j].mid_span.p, bundle[i].p, bundle[i].n)) {
                ++bundle_matched; break;
            }
    printf("XCloud4: SDP shape bytes=%zu media=%u stored=%u candidates=%u bundle=%u unique=%u matched=%u setup=%u trickle=%u ssrc=%u bare_ssrc=%u first_kind=%u second_kind=%u\n",
        length, count, stored, candidates, bundle_count, bundle_unique, bundle_matched, session_setup, session_trickle,
        ssrc_lines, bare_ssrc, stored ? media[0].kind : 0, stored > 1 ? media[1].kind : 0);
    for (unsigned i = 0; i < stored; ++i) {
        m = &media[i];
        printf("XCloud4: SDP media index=%u kind=%u port=%d midtype=%u midnum=%u direction=%u rtp=%u pts=%u overflow=%u setup=%u trickle=%u sctp=%d\n",
            i, m->kind, m->port, m->mid_type, m->mid, m->direction, m->rtp, m->pt_count, m->pt_overflow,
            m->setup, m->trickle, m->sctp_port);
        for (unsigned j = 0; j < m->pt_count; ++j)
            printf("XCloud4: SDP payload media=%u index=%u pt=%u\n", i, j, m->pts[j]);
        printf("XCloud4: SDP codec media=%u h264=%d opus=%d feedback=%u nack=%u pli=%u fir=%u remb=%u profile_present=%u profile=%06X known31=%u known32=%u\n",
            i, m->h264_pt, m->opus_pt, m->feedback, m->fb_nack, m->fb_pli, m->fb_fir, m->fb_remb,
            m->profile_found, m->profile, m->profile_found && m->profile == 0x42e01f,
            m->profile_found && m->profile == 0x42e020);
    }
}

static void description_callback(int pc, const char *sdp, const char *type, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    size_t length = sdp ? strnlen(sdp, SDP_CAP) : SDP_CAP;
    if (length == SDP_CAP || !type || strcmp(type, "offer")) fail(rtc, -40);
    else {
        lock(&rtc->data_gate);
        memcpy(rtc->sdp, sdp, length + 1);
        unlock(&rtc->data_gate);
    }
    leave(pointer);
}
static void candidate_callback(int pc, const char *candidate, const char *mid, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    size_t length = candidate ? strnlen(candidate, CANDIDATE_CAP) : CANDIDATE_CAP;
    size_t mid_length = mid ? strnlen(mid, MID_CAP) : MID_CAP;
    lock(&rtc->data_gate);
    if (length == CANDIDATE_CAP || mid_length == MID_CAP ||
        rtc->candidate_write - rtc->candidate_read == CANDIDATE_COUNT) fail(rtc, -41);
    else {
        Candidate *item = &rtc->candidates[rtc->candidate_write++ % CANDIDATE_COUNT];
        memcpy(item->candidate, candidate, length + 1);
        memcpy(item->mid, mid, mid_length + 1);
    }
    unlock(&rtc->data_gate);
    leave(pointer);
}
static void state_callback(int pc, rtcState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    if (!atomic_load(&rtc->error)) atomic_store(&rtc->state, state);
    printf("XCloud4: RTC estado=%d\n", (int)state);
    leave(pointer);
}
static void ice_state_callback(int pc, rtcIceState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    printf("XCloud4: RTC ICE estado=%d\n", (int)state);
    leave(pointer);
}
static void gathering_callback(int pc, rtcGatheringState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    printf("XCloud4: RTC gathering state=%d\n", (int)state);
    if (state == RTC_GATHERING_COMPLETE) atomic_store(&rtc->gathering_done, true);
    leave(pointer);
}

static void rtp_callback(int track, const char *packet, int size, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    /* RtcpReceivingSession consumes control packets and leaves RTP intact. */
    if (size >= 12 && packet && (((const uint8_t *)packet)[0] >> 6) == 2) {
        int kind = track == rtc->video ? X4_RTC_VIDEO : X4_RTC_AUDIO;
        if (kind == X4_RTC_VIDEO) atomic_fetch_add(&rtc->video_packets, 1);
        else atomic_fetch_add(&rtc->audio_packets, 1);
        lock(&rtc->data_gate);
        X4RtcMediaCallback callback = rtc->media;
        void *user = rtc->media_user;
        unlock(&rtc->data_gate);
        if (callback) callback(user, kind, (const uint8_t *)packet, (size_t)size);
    }
    leave(pointer);
}

static void uuid(char output[37])
{
    unsigned char bytes[16];
    x4_native_random_or_exit(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    snprintf(output, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
             bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
}
static int send_text(int channel, const char *text) { return rtcSendMessage(channel, text, -1); }

static void input_u16(uint8_t *p, uint16_t value)
{ p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void input_u32(uint8_t *p, uint32_t value)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i)); }

static void input_header(X4Rtc *rtc, uint8_t *packet, uint16_t type)
{
    lock(&rtc->data_gate);
    uint32_t sequence = ++rtc->input_sequence; /* Defined uint32 wrap. */
    unlock(&rtc->data_gate);
    uint64_t t=sceKernelGetProcessTime();
    double milliseconds=t>=rtc->started_at ? (double)(t-rtc->started_at)/1000.0 : 0.0;
    uint64_t bits;
    memcpy(&bits,&milliseconds,sizeof(bits));
    input_u16(packet,type);input_u32(packet+2,sequence);
    for(unsigned i=0;i<8;++i)packet[6+i]=(uint8_t)(bits>>(8*i));
}

/* Single-controller Xbox input 1.0 report. Layout and physicality trailers
 * match pinned GreenVita input.rs and PSBox input_packet.cpp. They are wire
 * constants, not an arbitrary serialized controller struct. */
static void input_gamepad(X4Rtc *rtc, const X4GamepadFrame *frame, uint8_t packet[38])
{
    input_header(rtc,packet,2);
    packet[14]=1;packet[15]=0;
    input_u16(packet+16,frame->buttons & 0xfffeu);
    int16_t axes[]={frame->left_x,frame->left_y,frame->right_x,frame->right_y};
    for(unsigned i=0;i<4;++i) {
        if(axes[i]<-32767)axes[i]=-32767;
        input_u16(packet+18+2*i,(uint16_t)axes[i]);
    }
    input_u16(packet+26,frame->left_trigger);input_u16(packet+28,frame->right_trigger);
    input_u32(packet+30,1);
    packet[34]=packet[35]=packet[36]=0;packet[37]=1; /* uint32 big endian. */
}

static void input_error(X4Rtc *rtc,int error)
{
    int previous=atomic_exchange(&rtc->input_error,error);
    if(error && error!=previous)printf("XCloud4: RTC input error=0x%08x\n",(unsigned)error);
}

static void *input_sender(void *pointer)
{
    X4Rtc *rtc=pointer;
    while(!atomic_load(&rtc->input_stop)) {
        uint64_t until=sceKernelGetProcessTime()+INPUT_INTERVAL_USEC;
        lock(&rtc->data_gate);
        X4GamepadSource source=rtc->input_source;
        void *user=rtc->input_user;
        unlock(&rtc->data_gate);
        bool ready=source && atomic_load(&rtc->input_metadata_sent) &&
            atomic_load(&rtc->startup_complete) && !atomic_load(&rtc->error) &&
            atomic_load(&rtc->state)==X4_RTC_CONNECTED && rtcIsOpen(rtc->channels[2]);
        atomic_store(&rtc->input_ready,ready);
        if(ready) {
            X4GamepadFrame frame={0};
            /* Auth copies its mailbox under its own gate. No RTC gate is
             * held across this call; close joins before freeing either. */
            if(!source(user,&frame) || !frame.connected)frame=(X4GamepadFrame){0};
            if(!atomic_load(&rtc->input_stop)) {
                int buffered=rtcGetBufferedAmount(rtc->channels[2]);
                if(buffered) {
                    atomic_fetch_add(&rtc->input_dropped,1);
                    if(buffered<0)input_error(rtc,buffered);
                } else {
                    uint8_t packet[38];
                    input_gamepad(rtc,&frame,packet);
                    int rc=rtcSendMessage(rtc->channels[2],(const char *)packet,sizeof(packet));
                    if(rc<0) {atomic_fetch_add(&rtc->input_dropped,1);input_error(rtc,rc);}
                    else {atomic_fetch_add(&rtc->input_packets,1);input_error(rtc,0);}
                }
            }
        }
        /* No catch-up bursts: each iteration starts a fresh monotonic
         * deadline; interrupted sleeps cannot exceed the 60 Hz ceiling. */
        while(!atomic_load(&rtc->input_stop)) {
            uint64_t t=sceKernelGetProcessTime();
            if(t>=until)break;
            sceKernelUsleep((unsigned)(until-t));
        }
    }
    atomic_store(&rtc->input_ready,false);
    return NULL;
}

static int startup_message(int channel, const char *target, const char *content)
{
    char escaped[1536], message[2048], id[37];
    size_t used = 0;
    for (const char *p = content; *p; ++p) {
        if (used + 3 >= sizeof(escaped)) return -1;
        if (*p == '"' || *p == '\\') escaped[used++] = '\\';
        escaped[used++] = *p;
    }
    escaped[used] = 0;
    uuid(id);
    int count = snprintf(message, sizeof(message), "{\"type\":\"Message\",\"content\":\"%s\",\"id\":\"%s\",\"target\":\"%s\",\"cv\":\"\"}", escaped, id, target);
    return count > 0 && (unsigned)count < sizeof(message) ? send_text(channel, message) : -1;
}

static void start_channels(X4Rtc *rtc)
{
    lock(&rtc->data_gate);
    bool ready = rtc->handshake_ack && !rtc->startup_sent &&
                 rtcIsOpen(rtc->channels[1]) && rtcIsOpen(rtc->channels[3]);
    if (ready) rtc->startup_sent = true;
    unlock(&rtc->data_gate);
    if (!ready) return;
    int control = rtc->channels[1], message = rtc->channels[3];
    /* Provider protocol constants, matching pinned GreenVita/Greenlight. */
    int rc = send_text(control, "{\"message\":\"authorizationRequest\",\"accessKey\":\"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E\"}");
    rc |= send_text(control, "{\"message\":\"gamepadChanged\",\"gamepadIndex\":0,\"wasAdded\":true}");
    rc |= startup_message(message, "/streaming/systemUi/configuration", "{\"version\":[0,2,0],\"systemUis\":[]}");
    rc |= startup_message(message, "/streaming/properties/clientappinstallidchanged", "{\"clientAppInstallId\":\"f9ac8684-1032-4131-bb47-d2f58da9bb93\"}");
    rc |= startup_message(message, "/streaming/characteristics/orientationchanged", "{\"orientation\":0}");
    rc |= startup_message(message, "/streaming/characteristics/touchinputenabledchanged", "{\"touchInputEnabled\":false}");
    printf("XCloud4: RTC video request width=960 height=540 max_fps=30 bitrate_kbps=5000\n");
    rc |= startup_message(message, "/streaming/characteristics/clientdevicecapabilities", "{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":30,\"maxWidth\":960,\"maxHeight\":540,\"maxBitrateKbps\":5000,\"video\":{\"width\":960,\"height\":540,\"maxWidth\":960,\"maxHeight\":540,\"maxBitrateKbps\":5000}}");
    rc |= startup_message(message, "/streaming/characteristics/dimensionschanged", "{\"horizontal\":960,\"vertical\":540,\"preferredWidth\":960,\"preferredHeight\":540,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":960,\"safeAreaBottom\":540,\"supportsCustomResolution\":true}");
    if (rc < 0) fail(rtc, -42);
    else {atomic_store(&rtc->startup_complete,true);printf("XCloud4: RTC canales Xbox preparados\n");}
}
static void channel_open(int channel, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    if (channel == rtc->channels[3]) {
        char id[37], message[192];
        uuid(id);
        snprintf(message, sizeof(message), "{\"type\":\"Handshake\",\"version\":\"messageV1\",\"id\":\"%s\",\"cv\":\"0\"}", id);
        if (send_text(channel, message) < 0) fail(rtc, -43);
    }
    if (channel == rtc->channels[2]) {
        /* ClientMetadata report: u16 type8, shared u32 sequence, f64 elapsed ms,
         * u8 supported touch points0; all fields are little endian. */
        unsigned char metadata[15] = {0};
        atomic_store(&rtc->input_metadata_sent,false);
        input_header(rtc,metadata,8);
        if (rtcSendMessage(channel, (const char *)metadata, sizeof(metadata)) < 0) fail(rtc, -43);
        else atomic_store(&rtc->input_metadata_sent,true);
    }
    start_channels(rtc);
    leave(pointer);
}
static void channel_message(int channel, const char *data, int size, void *pointer)
{
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
    size_t length = size < 0 ? (size_t)(-(int64_t)size - 1) : (size_t)size;
    if (channel == rtc->channels[3] && data && length && length <= 4096) {
        char type[32];
        X4JsonField field = {.name="type", .kind=X4_JSON_STRING, .text=type, .capacity=sizeof(type)};
        if (x4_json_fields(data, length, &field, 1) == 0 && field.found && !strcmp(type,"HandshakeAck")) {
            lock(&rtc->data_gate);
            rtc->handshake_ack = true;
            unlock(&rtc->data_gate);
            start_channels(rtc);
        }
    }
    leave(pointer);
}

X4Rtc *x4_rtc_open(int *error)
{
    int rc = -44;
    if (x4_native_entropy_prepare() < 0 || x4_native_net_prepare() < 0) goto fail_no_context;
    X4Rtc *rtc = calloc(1, sizeof(*rtc));
    if (!rtc) goto fail_no_context;
    rtc->pc = rtc->video = rtc->audio = -1;
    rtc->started_at = sceKernelGetProcessTime();
    for (unsigned i=0;i<4;++i) rtc->channels[i]=-1;
    atomic_init(&rtc->state, X4_RTC_NEW);
    atomic_init(&rtc->error, 0);
    atomic_init(&rtc->gathering_done, false);
    atomic_init(&rtc->offer_logged, false);
    atomic_init(&rtc->video_packets, 0);
    atomic_init(&rtc->audio_packets, 0);
    atomic_init(&rtc->input_stop,false);atomic_init(&rtc->input_ready,false);
    atomic_init(&rtc->input_metadata_sent,false);atomic_init(&rtc->startup_complete,false);
    atomic_init(&rtc->input_error,0);
    atomic_init(&rtc->input_packets,0);atomic_init(&rtc->input_dropped,0);
    atomic_flag_clear(&rtc->data_gate);
    lock(&callback_gate);
    if (slots_used < CALLBACK_SLOTS) { rtc->slot=&slots[slots_used++]; rtc->slot->context=rtc; }
    unlock(&callback_gate);
    if (!rtc->slot) { free(rtc); rc=-45; goto fail_no_context; }
    rtcInitLogger(RTC_LOG_NONE, NULL); /* Raw dependency logs can contain ICE credentials. */
    /* Explicit application policy: four RTC workers. The SDK's precompiled
     * hardware_concurrency probe reported200112 workers in the actual0.7.1
     * PS4 kernel capture, so it cannot define this application's pool. */
    printf("XCloud4: RTC begin rtcSetThreadPoolSize(4)\n");
    rc=rtcSetThreadPoolSize(4);
    printf("XCloud4: RTC rtcSetThreadPoolSize(4) rc=%d\n",rc);
    if(rc<0)goto fail_context;
    const char *servers[] = {"stun:stun.l.google.com:19302"};
    rtcConfiguration configuration = {.iceServers=servers,.iceServersCount=1,
        .certificateType=RTC_CERTIFICATE_ECDSA,.disableAutoNegotiation=true,
        .forceMediaTransport=true,.mtu=1280,.maxMessageSize=65536,
        .disableFingerprintVerification=false};
    printf("XCloud4: RTC begin rtcCreatePeerConnection\n");
    rc=rtc->pc=rtcCreatePeerConnection(&configuration);
    printf("XCloud4: RTC rtcCreatePeerConnection rc=%d\n",rc);
    if (rc<0) goto fail_context;
    rtcSetUserPointer(rtc->pc,rtc->slot);
#define CHECK(call) do { \
    printf("XCloud4: RTC begin %s\n",#call); \
    rc=(call); \
    printf("XCloud4: RTC %s rc=%d\n",#call,rc); \
    if(rc<0)goto fail_context; \
}while(0)
    CHECK(rtcSetLocalDescriptionCallback(rtc->pc,description_callback));
    CHECK(rtcSetLocalCandidateCallback(rtc->pc,candidate_callback));
    CHECK(rtcSetStateChangeCallback(rtc->pc,state_callback));
    CHECK(rtcSetIceStateChangeCallback(rtc->pc,ice_state_callback));
    CHECK(rtcSetGatheringStateChangeCallback(rtc->pc,gathering_callback));
    /* Keep media routing keyed by track handle when the offer order changes. */
    rtcTrackInit audio={.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_OPUS,.payloadType=111,.mid="0",
        .profile="minptime=10;useinbandfec=1;stereo=1"};
    rtcTrackInit video={.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_H264,.payloadType=102,.mid="1",
        .profile="level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000"};
    CHECK(rtc->audio=rtcAddTrackEx(rtc->pc,&audio));
    CHECK(rtc->video=rtcAddTrackEx(rtc->pc,&video));
    int tracks[]={rtc->audio,rtc->video};
    for(unsigned i=0;i<2;++i) {
        printf("XCloud4: RTC configure track index=%u\n",i);
        rtcSetUserPointer(tracks[i],rtc->slot);
        CHECK(rtcChainRtcpReceivingSession(tracks[i]));
        CHECK(rtcSetMessageCallback(tracks[i],rtp_callback));
    }
    const char *labels[]={"chat","control","input","message"};
    const char *protocols[]={"chatV1","controlV1","1.0","messageV1"};
    for(unsigned i=0;i<4;++i) {
        printf("XCloud4: RTC configure channel index=%u\n",i);
        rtcDataChannelInit config={.protocol=protocols[i]};
        if(i==2) {config.reliability.unordered=true;config.reliability.unreliable=true;config.reliability.maxRetransmits=0;}
        CHECK(rtc->channels[i]=rtcCreateDataChannelEx(rtc->pc,labels[i],&config));
        rtcSetUserPointer(rtc->channels[i],rtc->slot);
        CHECK(rtcSetOpenCallback(rtc->channels[i],channel_open));
        CHECK(rtcSetMessageCallback(rtc->channels[i],channel_message));
    }
    CHECK(rtcSetLocalDescription(rtc->pc,"offer"));
#undef CHECK
    if(error)*error=0;
    return rtc;
fail_context:
    printf("XCloud4: RTC open failed rc=%d\n",rc);
    x4_rtc_close(rtc);
fail_no_context:
    if(error)*error=rc;
    return NULL;
}

void x4_rtc_set_media_callback(X4Rtc *rtc,X4RtcMediaCallback callback,void *user)
{
    if(!rtc)return;
    lock(&rtc->data_gate);rtc->media=callback;rtc->media_user=user;unlock(&rtc->data_gate);
}
void x4_rtc_set_diagnostic_trace(X4Rtc *rtc, X4Trace *trace)
{
    if (rtc) rtc->diagnostic_trace = trace;
}
int x4_rtc_set_gamepad_source(X4Rtc *rtc,X4GamepadSource source,void *user)
{
    if(!rtc || !source || rtc->input_thread_started || atomic_load(&rtc->input_stop))return -46;
    lock(&rtc->data_gate);rtc->input_source=source;rtc->input_user=user;unlock(&rtc->data_gate);
    int rc=scePthreadCreate(&rtc->input_thread,NULL,input_sender,rtc,"XCloud4Input");
    if(rc) {
        lock(&rtc->data_gate);rtc->input_source=NULL;rtc->input_user=NULL;unlock(&rtc->data_gate);
        input_error(rtc,rc);
        return rc;
    }
    rtc->input_thread_started=true;
    printf("XCloud4: RTC input sender iniciado\n");
    return 0;
}
int x4_rtc_local_description(X4Rtc *rtc,char *sdp,size_t capacity)
{
    if(!rtc||!sdp||!capacity)return -1;
    sdp[0]=0;
    if(atomic_load(&rtc->error))return atomic_load(&rtc->error);
    /* The initial description callback precedes gathering and is only a
     * snapshot. GreenVita gathers before creating its offer; use the current
     * library description after gathering instead of that stale snapshot.
     * The session worker owns this call, and its existing deadline bounds
     * the pending return. No credentials or candidates are constructed here. */
    if(!atomic_load(&rtc->gathering_done))return 0;
    if(capacity>SDP_CAP)capacity=SDP_CAP;
    int required=rtcGetLocalDescription(rtc->pc,NULL,0);
    if(required==RTC_ERR_NOT_AVAIL)return 0;
    if(required<1)return required<0?required:-40;
    /* CAPI lengths include the NUL terminator. The library copies its
     * description under its own mutex; no application callback lock is held. */
    if((size_t)required>capacity)return RTC_ERR_TOO_SMALL;
    char type[16]={0};
    int rc=rtcGetLocalDescriptionType(rtc->pc,type,sizeof(type));
    if(rc!=6||memcmp(type,"offer",6))return rc<0?rc:-40;
    rc=rtcGetLocalDescription(rtc->pc,sdp,(int)capacity);
    if(rc!=required||rc<1||(size_t)rc>capacity||sdp[rc-1]||strnlen(sdp,(size_t)rc)!=(size_t)rc-1) {
        memset(sdp,0,capacity);
        return rc<0?rc:-40;
    }
    if(!atomic_exchange(&rtc->offer_logged,true))sdp_shape_log(sdp,(size_t)rc-1);
    return 1;
}
int x4_rtc_next_local_candidate(X4Rtc *rtc,char *candidate,size_t capacity,char *mid,size_t mid_capacity)
{
    if(!rtc||!candidate||!mid)return -1;
    if(atomic_load(&rtc->error))return atomic_load(&rtc->error);
    lock(&rtc->data_gate);
    int rc=0;
    if(rtc->candidate_read!=rtc->candidate_write) {
        Candidate *item=&rtc->candidates[rtc->candidate_read%CANDIDATE_COUNT];
        if(strlen(item->candidate)+1>capacity||strlen(item->mid)+1>mid_capacity)rc=-1;
        else {strcpy(candidate,item->candidate);strcpy(mid,item->mid);++rtc->candidate_read;rc=1;}
    }
    unlock(&rtc->data_gate);return rc;
}
int x4_rtc_set_remote_description(X4Rtc *rtc,const char *sdp)
{
    if(!rtc||!sdp||strnlen(sdp,SDP_CAP)>=SDP_CAP)return -1;
    int rc=rtcSetRemoteDescription(rtc->pc,sdp,"answer");if(rc<0)fail(rtc,rc);return rc;
}
int x4_rtc_add_remote_candidate(X4Rtc *rtc,const char *candidate,const char *mid)
{
    if(!rtc||!candidate||!mid||strnlen(candidate,CANDIDATE_CAP)>=CANDIDATE_CAP||strnlen(mid,MID_CAP)>=MID_CAP)return -1;
    int rc=rtcAddRemoteCandidate(rtc->pc,candidate,mid);if(rc<0)fail(rtc,rc);return rc;
}
void x4_rtc_snapshot(X4Rtc *rtc,X4RtcSnapshot *snapshot)
{
    if(!snapshot)return;
    *snapshot=(X4RtcSnapshot){.state=X4_RTC_CLOSED};if(!rtc)return;
    snapshot->state=(X4RtcState)atomic_load(&rtc->state);snapshot->error=atomic_load(&rtc->error);
    snapshot->gathering_done=atomic_load(&rtc->gathering_done);
    snapshot->video_packets=atomic_load(&rtc->video_packets);snapshot->audio_packets=atomic_load(&rtc->audio_packets);
    snapshot->input_ready=atomic_load(&rtc->input_ready);snapshot->input_error=atomic_load(&rtc->input_error);
    snapshot->input_packets=atomic_load(&rtc->input_packets);snapshot->input_dropped=atomic_load(&rtc->input_dropped);
}
int x4_rtc_request_keyframe(X4Rtc *rtc)
{
    if(!rtc)return -1;
    /* Native dispatch attempts/results only, never a network delivery ACK.
     * Keep the existing control message, RTCP call, cadence and return value. */
    X4Trace *trace = rtc->diagnostic_trace;
    if(rtcIsOpen(rtc->channels[1])) {
        uint64_t ordinal = ++rtc->diagnostic_pli_ordinal;
        x4_trace_record(trace, X4_TRACE_PLI_ATTEMPT, 1, ordinal, 0);
        int control_rc = send_text(rtc->channels[1],"{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}");
        x4_trace_record(trace, X4_TRACE_PLI_RESULT, 1, ordinal, (uint32_t)control_rc);
    }
    uint64_t ordinal = ++rtc->diagnostic_pli_ordinal;
    x4_trace_record(trace, X4_TRACE_PLI_ATTEMPT, 2, ordinal, 0);
    int rc = rtcRequestKeyframe(rtc->video);
    x4_trace_record(trace, X4_TRACE_PLI_RESULT, 2, ordinal, (uint32_t)rc);
    return rc;
}
int x4_rtc_close(X4Rtc *rtc)
{
    if(!rtc)return 0;
    atomic_store(&rtc->input_stop,true);
    if(rtc->input_thread_started) {
        int rc=scePthreadJoin(rtc->input_thread,NULL);
        if(rc) {
            if(rtc->input_join_error!=rc)printf("XCloud4: RTC input join error=0x%08x; contexto retenido\n",(unsigned)rc);
            rtc->input_join_error=rc;
            input_error(rtc,rc);
            return rc; /* No handle, callback or provider context is freed. */
        }
        rtc->input_thread_started=false;
    }
    printf("XCloud4: RTC input resumen submitted=%llu dropped=%llu error=0x%08x\n",
        (unsigned long long)atomic_load(&rtc->input_packets),
        (unsigned long long)atomic_load(&rtc->input_dropped),(unsigned)atomic_load(&rtc->input_error));
    /* Every callback carries a process-lifetime slot. Detach the heap context
     * before deleting asynchronous library objects; stale callbacks see NULL.
     * Slots are deliberately never reused (bounded to64 sessions per process). */
    lock(&callback_gate);rtc->slot->context=NULL;unlock(&callback_gate);
    for(unsigned i=0;i<4;++i)if(rtc->channels[i]>=0) {rtcSetMessageCallback(rtc->channels[i],NULL);rtcSetOpenCallback(rtc->channels[i],NULL);rtcDeleteDataChannel(rtc->channels[i]);}
    if(rtc->video>=0) {rtcSetMessageCallback(rtc->video,NULL);rtcDeleteTrack(rtc->video);}
    if(rtc->audio>=0) {rtcSetMessageCallback(rtc->audio,NULL);rtcDeleteTrack(rtc->audio);}
    if(rtc->pc>=0) {rtcSetLocalDescriptionCallback(rtc->pc,NULL);rtcSetLocalCandidateCallback(rtc->pc,NULL);rtcSetStateChangeCallback(rtc->pc,NULL);rtcSetIceStateChangeCallback(rtc->pc,NULL);rtcSetGatheringStateChangeCallback(rtc->pc,NULL);rtcDeletePeerConnection(rtc->pc);}
    for(;;) {lock(&callback_gate);unsigned active=rtc->slot->active;unlock(&callback_gate);if(!active)break;sceKernelUsleep(1000);}
    volatile unsigned char *wipe=(unsigned char *)rtc;for(size_t i=0;i<sizeof(*rtc);++i)wipe[i]=0;
    free(rtc);
    return 0;
}
