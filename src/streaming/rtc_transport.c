/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_transport.h"
#include "rtc_native.h"
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

typedef struct { char candidate[CANDIDATE_CAP], mid[MID_CAP]; } Candidate;
typedef struct { X4Rtc *context; unsigned active; } CallbackSlot;
static CallbackSlot slots[CALLBACK_SLOTS];
static unsigned slots_used;
static atomic_flag callback_gate = ATOMIC_FLAG_INIT;

struct X4Rtc {
    int pc, video, audio, channels[4];
    CallbackSlot *slot;
    atomic_int state, error;
    atomic_bool gathering_done;
    atomic_uint_fast64_t video_packets, audio_packets;
    atomic_flag data_gate;
    char sdp[SDP_CAP];
    Candidate candidates[CANDIDATE_COUNT];
    unsigned candidate_read, candidate_write;
    X4RtcMediaCallback media;
    void *media_user;
    bool handshake_ack, startup_sent;
    uint64_t started_at;
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
static void gathering_callback(int pc, rtcGatheringState state, void *pointer)
{
    (void)pc;
    X4Rtc *rtc = enter(pointer);
    if (!rtc) return;
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
    rc |= startup_message(message, "/streaming/characteristics/clientdevicecapabilities", "{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":30,\"maxWidth\":1280,\"maxHeight\":720,\"maxBitrateKbps\":5000,\"video\":{\"width\":1280,\"height\":720,\"maxWidth\":1280,\"maxHeight\":720,\"maxBitrateKbps\":5000}}");
    rc |= startup_message(message, "/streaming/characteristics/dimensionschanged", "{\"horizontal\":1280,\"vertical\":720,\"preferredWidth\":1280,\"preferredHeight\":720,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":1280,\"safeAreaBottom\":720,\"supportsCustomResolution\":true}");
    if (rc < 0) fail(rtc, -42);
    else printf("XCloud4: RTC canales Xbox preparados\n");
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
        /* ClientMetadata report: u16 type8, u32 sequence1, f64 elapsed ms,
         * u8 supported touch points0; all fields are little endian. */
        unsigned char metadata[15] = {8, 0, 1};
        double elapsed_ms = (double)(sceKernelGetProcessTime() - rtc->started_at) / 1000.0;
        memcpy(metadata + 6, &elapsed_ms, sizeof(elapsed_ms));
        if (rtcSendMessage(channel, (const char *)metadata, sizeof(metadata)) < 0) fail(rtc, -43);
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
    atomic_init(&rtc->video_packets, 0);
    atomic_init(&rtc->audio_packets, 0);
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
    CHECK(rtcSetGatheringStateChangeCallback(rtc->pc,gathering_callback));
    rtcTrackInit video={.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_H264,.payloadType=102,.mid="0",
        .profile="level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000"};
    rtcTrackInit audio={.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_OPUS,.payloadType=111,.mid="1",
        .profile="minptime=10;useinbandfec=1;stereo=1"};
    CHECK(rtc->video=rtcAddTrackEx(rtc->pc,&video));
    CHECK(rtc->audio=rtcAddTrackEx(rtc->pc,&audio));
    int tracks[]={rtc->video,rtc->audio};
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
int x4_rtc_local_description(X4Rtc *rtc,char *sdp,size_t capacity)
{
    if(!rtc||!sdp||!capacity)return -1;
    if(atomic_load(&rtc->error))return atomic_load(&rtc->error);
    lock(&rtc->data_gate);
    size_t size=strlen(rtc->sdp);
    int rc=size?1:0;
    if(size+1>capacity)rc=-1;else memcpy(sdp,rtc->sdp,size+1);
    unlock(&rtc->data_gate);return rc;
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
}
int x4_rtc_request_keyframe(X4Rtc *rtc)
{
    if(!rtc)return -1;
    if(rtcIsOpen(rtc->channels[1]))send_text(rtc->channels[1],"{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}");
    return rtcRequestKeyframe(rtc->video);
}
void x4_rtc_close(X4Rtc *rtc)
{
    if(!rtc)return;
    /* Every callback carries a process-lifetime slot. Detach the heap context
     * before deleting asynchronous library objects; stale callbacks see NULL.
     * Slots are deliberately never reused (bounded to64 sessions per process). */
    lock(&callback_gate);rtc->slot->context=NULL;unlock(&callback_gate);
    for(unsigned i=0;i<4;++i)if(rtc->channels[i]>=0) {rtcSetMessageCallback(rtc->channels[i],NULL);rtcSetOpenCallback(rtc->channels[i],NULL);rtcDeleteDataChannel(rtc->channels[i]);}
    if(rtc->video>=0) {rtcSetMessageCallback(rtc->video,NULL);rtcDeleteTrack(rtc->video);}
    if(rtc->audio>=0) {rtcSetMessageCallback(rtc->audio,NULL);rtcDeleteTrack(rtc->audio);}
    if(rtc->pc>=0) {rtcSetLocalDescriptionCallback(rtc->pc,NULL);rtcSetLocalCandidateCallback(rtc->pc,NULL);rtcSetStateChangeCallback(rtc->pc,NULL);rtcSetGatheringStateChangeCallback(rtc->pc,NULL);rtcDeletePeerConnection(rtc->pc);}
    for(;;) {lock(&callback_gate);unsigned active=rtc->slot->active;unlock(&callback_gate);if(!active)break;sceKernelUsleep(1000);}
    volatile unsigned char *wipe=(unsigned char *)rtc;for(size_t i=0;i<sizeof(*rtc);++i)wipe[i]=0;
    free(rtc);
}
