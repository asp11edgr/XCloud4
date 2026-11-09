/* SPDX-License-Identifier: GPL-3.0-only */
/* Production start_channels/channel callbacks run here. Only the native
 * channel and entropy boundaries are simulated; no packets leave the host. */
#define _POSIX_C_SOURCE 200809L
#include "../src/streaming/rtc_transport.c"

static atomic_uint checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"RTC startup check failed at %d: %s\n",__LINE__,#x); abort(); } } while (0)

enum { CONTROL=11, MESSAGE=13, INPUT=12, VIDEO=21, EVENTS=64 };
typedef struct { int channel, result; char text[2048]; } SendEvent;
static SendEvent events[EVENTS];
static unsigned sent, native_keyframes, random_counter;
static int native_keyframe_result;
static bool open_control, open_message, close_before_resolution;
static const char *failed_fragment;
static int failure_return;
static X4StreamStartup bound_startup;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
static X4KeyframeLimiter bound_limiter;
#endif
static const char *expected_alias(unsigned profile);

bool rtcIsOpen(int channel)
{
    return channel==CONTROL ? open_control : channel==MESSAGE ? open_message : channel==INPUT;
}
int rtcSendMessage(int channel, const char *data, int size)
{
    CHECK(sent<EVENTS && data && size==-1);
    CHECK(strlen(data)<sizeof(events[0].text));
    SendEvent *event=&events[sent++];
    event->channel=channel;
    strcpy(event->text,data);
    event->result=rtcIsOpen(channel) ? 0 : -89;
    if (failed_fragment && strstr(data,failed_fragment)) event->result=failure_return;
    if (close_before_resolution && strstr(data,"/streaming/characteristics/dimensionschanged")) open_control=false;
    return event->result;
}
int rtcRequestKeyframe(int track)
{
    CHECK(track==VIDEO);
    ++native_keyframes;
    return native_keyframe_result;
}
void x4_native_random_or_exit(void *data, size_t size)
{
    unsigned char *bytes=data;
    for (size_t i=0;i<size;++i) bytes[i]=(unsigned char)(++random_counter);
}
void x4_trace_record(X4Trace *trace, uint16_t event, uint16_t flags, uint64_t a, uint64_t b)
{ (void)trace;(void)event;(void)flags;(void)a;(void)b; }
static uint64_t fixture_clock_us=1000000;
uint64_t sceKernelGetProcessTime(void) { return fixture_clock_us; }

static X4Rtc *fixture(unsigned profile)
{
    memset(events,0,sizeof(events));
    sent=native_keyframes=random_counter=0;
    native_keyframe_result=0;
    open_control=open_message=true;
    close_before_resolution=false;
    failed_fragment=NULL;failure_return=-81;
    X4Rtc *rtc=calloc(1,sizeof(*rtc));
    CHECK(rtc);
    rtc->channels[0]=10;rtc->channels[1]=CONTROL;rtc->channels[2]=INPUT;rtc->channels[3]=MESSAGE;
    rtc->video=VIDEO;
    rtc->slot=calloc(1,sizeof(*rtc->slot));
    CHECK(rtc->slot);
    rtc->slot->context=rtc;
    atomic_init(&rtc->state,X4_RTC_CONNECTED);
    atomic_init(&rtc->error,0);
    atomic_init(&rtc->startup_complete,false);
    atomic_init(&rtc->input_metadata_sent,false);
#if !X4_PUBLIC
    lab_peer_diagnostics_init(rtc);
#endif
    atomic_flag_clear(&rtc->data_gate);
#if X4_STREAM_PROFILE
    rtc->request_config=x4_stream_request_config((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709);
#else
    (void)profile;
#endif
    x4_stream_startup_init(&bound_startup,expected_alias(profile)!=NULL);
    CHECK(x4_rtc_set_stream_startup(rtc,&bound_startup)==0);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    x4_keyframe_limiter_init(&bound_limiter);
    CHECK(x4_rtc_set_keyframe_limiter(rtc,&bound_limiter)==0);
#endif
    return rtc;
}
static void reserve_keyframe(X4Rtc *rtc,uint64_t now)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    CHECK(rtc->keyframe_limiter==&bound_limiter);
    x4_keyframe_limiter_request(&bound_limiter,X4_KEYFRAME_STARTUP);
    CHECK(x4_keyframe_limiter_take(&bound_limiter,now,NULL));
#else
    (void)rtc;(void)now;
#endif
}
static void destroy(X4Rtc *rtc)
{
    CHECK(rtc->slot->active==0);
    free(rtc->slot);free(rtc);
}
static void ack(X4Rtc *rtc)
{
    static const char text[]="{\"type\":\"HandshakeAck\"}";
    channel_message(MESSAGE,text,-(int)sizeof(text),rtc->slot);
}
static unsigned count_fragment(const char *fragment)
{
    unsigned count=0;
    for (unsigned i=0;i<sent;++i) count+=strstr(events[i].text,fragment)!=NULL;
    return count;
}
static void expect_message(unsigned i, const char *target, const char *expected_content)
{
    CHECK(i<sent && events[i].channel==MESSAGE && events[i].result==0);
    char type[32],content[512],actual_target[128],id[40],cv[4];
    X4JsonField fields[]={
        {.name="type",.kind=X4_JSON_STRING,.text=type,.capacity=sizeof(type)},
        {.name="content",.kind=X4_JSON_STRING,.text=content,.capacity=sizeof(content)},
        {.name="target",.kind=X4_JSON_STRING,.text=actual_target,.capacity=sizeof(actual_target)},
        {.name="id",.kind=X4_JSON_STRING,.text=id,.capacity=sizeof(id)},
        {.name="cv",.kind=X4_JSON_STRING,.text=cv,.capacity=sizeof(cv)}
    };
    CHECK(x4_json_fields(events[i].text,strlen(events[i].text),fields,5)==0);
    for(unsigned j=0;j<5;++j) CHECK(fields[j].found);
    CHECK(!strcmp(type,"Message") && !strcmp(actual_target,target) && !strcmp(content,expected_content));
    CHECK(strlen(id)==36 && id[8]=='-' && id[13]=='-' && id[18]=='-' && id[23]=='-' && !*cv);
}
static const char *expected_alias(unsigned profile)
{
#if X4_STREAM_PROFILE
    return profile==0 ? NULL : profile==4 ? "720HQ" : profile==3 ? "1080HQ" : "1080";
#else
    (void)profile;return NULL;
#endif
}
static void verify_success(X4Rtc *rtc, unsigned profile, unsigned offset)
{
    const char *alias=expected_alias(profile);
    CHECK(atomic_load(&rtc->startup_complete) && rtc->startup_sent && !atomic_load(&rtc->error));
    CHECK(sent==offset+8+(alias!=NULL));
    CHECK(events[offset].channel==CONTROL && events[offset].result==0);
    CHECK(!strcmp(events[offset].text,"{\"message\":\"authorizationRequest\",\"accessKey\":\"4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E\"}"));
    CHECK(events[offset+1].channel==CONTROL && !strcmp(events[offset+1].text,"{\"message\":\"gamepadChanged\",\"gamepadIndex\":0,\"wasAdded\":true}"));
    expect_message(offset+2,"/streaming/systemUi/configuration","{\"version\":[0,2,0],\"systemUis\":[]}");
    expect_message(offset+3,"/streaming/properties/clientappinstallidchanged","{\"clientAppInstallId\":\"f9ac8684-1032-4131-bb47-d2f58da9bb93\"}");
    expect_message(offset+4,"/streaming/characteristics/orientationchanged","{\"orientation\":0}");
    expect_message(offset+5,"/streaming/characteristics/touchinputenabledchanged","{\"touchInputEnabled\":false}");
    char capabilities[512],dimensions[512];
#if X4_STREAM_PROFILE
    CHECK(x4_stream_request_capabilities(&rtc->request_config,capabilities,sizeof(capabilities)));
    CHECK(x4_stream_request_dimensions(&rtc->request_config,dimensions,sizeof(dimensions)));
    if(profile) {
        CHECK(!strcmp(capabilities,"{}"));
        CHECK(!strcmp(dimensions,profile==4 ?
            "{\"horizontal\":1280,\"vertical\":720,\"preferredWidth\":1280,\"preferredHeight\":720,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":1280,\"safeAreaBottom\":720,\"supportsCustomResolution\":true}" :
            "{\"horizontal\":1920,\"vertical\":1080,\"preferredWidth\":1920,\"preferredHeight\":1080,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":1920,\"safeAreaBottom\":1080,\"supportsCustomResolution\":true}"));
    } else {
        CHECK(!strcmp(capabilities,"{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":60,\"maxWidth\":1280,\"maxHeight\":720,\"maxBitrateKbps\":10000,\"video\":{\"width\":1280,\"height\":720,\"maxWidth\":1280,\"maxHeight\":720,\"maxBitrateKbps\":10000}}"));
        CHECK(!strcmp(dimensions,"{\"horizontal\":1280,\"vertical\":720,\"preferredWidth\":1280,\"preferredHeight\":720,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":1280,\"safeAreaBottom\":720,\"supportsCustomResolution\":true}"));
    }
#else
    strcpy(capabilities,"{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":30,\"maxWidth\":960,\"maxHeight\":540,\"maxBitrateKbps\":5000,\"video\":{\"width\":960,\"height\":540,\"maxWidth\":960,\"maxHeight\":540,\"maxBitrateKbps\":5000}}");
    strcpy(dimensions,"{\"horizontal\":960,\"vertical\":540,\"preferredWidth\":960,\"preferredHeight\":540,\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":960,\"safeAreaBottom\":540,\"supportsCustomResolution\":true}");
#endif
    expect_message(offset+6,"/streaming/characteristics/clientdevicecapabilities",capabilities);
    expect_message(offset+7,"/streaming/characteristics/dimensionschanged",dimensions);
    if(alias) {
        char expected[96];
        snprintf(expected,sizeof(expected),"{\"message\":\"userRequestedResolutionUpdate\",\"resolutionAlias\":\"%s\"}",alias);
        CHECK(events[offset+8].channel==CONTROL && events[offset+8].result==0 && !strcmp(events[offset+8].text,expected));
    }
    CHECK(count_fragment("userRequestedResolutionUpdate")==!!alias);
    CHECK(x4_rtc_keyframe_startup_ready(rtc));
    CHECK(x4_stream_startup_alias_sent_us(&bound_startup)==(alias?1000000u:0u));
#if X4_STREAM_PROFILE
    CHECK(x4_rtc_set_stream_startup(rtc,&bound_startup)==-1);
#endif
    unsigned before=sent;
    start_channels(rtc);ack(rtc);start_channels(rtc);
    CHECK(sent==before); /* Repeated startup/ack must not repeat public requests. */
    reserve_keyframe(rtc,1000000);
    CHECK(x4_rtc_request_keyframe(rtc)==0 && native_keyframes==1);
    CHECK(sent==before+1 && events[before].channel==CONTROL);
    CHECK(!strcmp(events[before].text,"{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}"));
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    CHECK(x4_rtc_set_keyframe_limiter(rtc,&bound_limiter)==-1);
    CHECK(x4_rtc_request_keyframe(rtc)==RTC_ERR_NOT_AVAIL&&native_keyframes==1&&sent==before+1);
#endif
}
static void ordinary_and_deferred(unsigned profile)
{
    X4Rtc *rtc=fixture(profile);
    start_channels(rtc);CHECK(sent==0);
    channel_open(MESSAGE,rtc->slot);CHECK(sent==1 && strstr(events[0].text,"\"type\":\"Handshake\""));
    start_channels(rtc);CHECK(sent==1);
    ack(rtc);verify_success(rtc,profile,1);destroy(rtc);

    rtc=fixture(profile);open_control=false;
    channel_open(MESSAGE,rtc->slot);ack(rtc);
    CHECK(sent==1 && rtc->handshake_ack && !rtc->startup_sent && !atomic_load(&rtc->startup_complete));
    open_control=true;channel_open(CONTROL,rtc->slot);
    verify_success(rtc,profile,1);destroy(rtc);

    rtc=fixture(profile);open_message=false;
    channel_open(CONTROL,rtc->slot);ack(rtc);
    CHECK(!sent && !rtc->startup_sent);
    open_message=true;channel_open(MESSAGE,rtc->slot);
    verify_success(rtc,profile,1);destroy(rtc);
}
static void failures(unsigned profile)
{
    X4Rtc *rtc=fixture(profile);rtc->handshake_ack=true;
    failed_fragment="/streaming/characteristics/dimensionschanged";
    start_channels(rtc);
    CHECK(atomic_load(&rtc->error)==-42 && atomic_load(&rtc->state)==X4_RTC_FAILED);
    CHECK(!atomic_load(&rtc->startup_complete) && rtc->startup_sent);
    CHECK(!count_fragment("userRequestedResolutionUpdate"));
    unsigned before=sent;start_channels(rtc);ack(rtc);CHECK(sent==before);destroy(rtc);
    if(!expected_alias(profile)) return;

    rtc=fixture(profile);rtc->handshake_ack=true;
    failed_fragment="userRequestedResolutionUpdate";
    start_channels(rtc);
    CHECK(events[sent-1].channel==CONTROL && events[sent-1].result==-81);
    CHECK(count_fragment("userRequestedResolutionUpdate")==1 && atomic_load(&rtc->error)==-42);
    bool attempted=false;CHECK(x4_stream_startup_alias_result(&bound_startup,&attempted)==-81&&attempted);
    CHECK(!x4_stream_startup_alias_sent_us(&bound_startup));
    CHECK(!atomic_load(&rtc->startup_complete));destroy(rtc);

    rtc=fixture(profile);rtc->handshake_ack=true;close_before_resolution=true;
    start_channels(rtc);
    CHECK(!open_control && events[sent-1].channel==CONTROL && events[sent-1].result==-89);
    CHECK(count_fragment("userRequestedResolutionUpdate")==1 && atomic_load(&rtc->error)==-42);
    CHECK(!atomic_load(&rtc->startup_complete));destroy(rtc);
}
static void keyframe_startup_order(unsigned profile)
{
    X4Rtc *rtc=fixture(profile);
    const char *alias=expected_alias(profile);
    CHECK(x4_rtc_keyframe_startup_ready(rtc)==!alias);
    if(!alias)reserve_keyframe(rtc,1000000);
    int rc=x4_rtc_request_keyframe(rtc);
    if(alias) {
        CHECK(rc==RTC_ERR_NOT_AVAIL && !sent && !native_keyframes && !rtc->diagnostic_pli_ordinal);
        open_control=false;channel_open(MESSAGE,rtc->slot);ack(rtc);
        CHECK(sent==1 && !x4_rtc_keyframe_startup_ready(rtc));
        CHECK(x4_rtc_request_keyframe(rtc)==RTC_ERR_NOT_AVAIL && sent==1 && !native_keyframes);
        open_control=true;channel_open(CONTROL,rtc->slot);
        verify_success(rtc,profile,1);
    } else {
        CHECK(rc==0 && sent==1 && native_keyframes==1); /* 720/legacy retain their previous timing. */
    }
    destroy(rtc);
}
static void *parallel_start(void *pointer)
{
    for(unsigned i=0;i<100;++i) start_channels(pointer);
    return NULL;
}
static void concurrent_start(unsigned profile)
{
    X4Rtc *rtc=fixture(profile);rtc->handshake_ack=true;
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i) CHECK(pthread_create(&threads[i],NULL,parallel_start,rtc)==0);
    for(unsigned i=0;i<4;++i) CHECK(pthread_join(threads[i],NULL)==0);
    verify_success(rtc,profile,0);destroy(rtc);
}
int main(void)
{
    CHECK(x4_rtc_set_stream_startup(NULL,NULL)==-1);
    CHECK(x4_rtc_set_keyframe_limiter(NULL,NULL)==-1);
    CHECK(!x4_rtc_keyframe_startup_ready(NULL));
    CHECK(x4_rtc_request_keyframe(NULL)==-1);
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile) {
        ordinary_and_deferred(profile);failures(profile);keyframe_startup_order(profile);concurrent_start(profile);
        printf("RTC real startup flag=%d profile=%u alias=%s passed\n",X4_STREAM_PROFILE,profile,expected_alias(profile)?expected_alias(profile):"none");
    }
    printf("RTC real startup checks=%u passed; channel sends are host simulations, not server delivery\n",checks);
    return 0;
}
