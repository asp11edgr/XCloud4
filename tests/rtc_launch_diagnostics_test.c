/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the real callback and strict log projection without network or credentials. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sched.h>
#include "../src/core/x4_log.h"
#include "../src/ui/picture_settings.h"
static int capture_printf(const char *format, ...);
#define printf capture_printf
#define x4_log_printf capture_printf
#define main rtc_resolution_previous_main
#include "rtc_resolution_startup_test.c"
#undef main
#undef x4_log_printf
#undef printf

static pthread_t log_owner;
static char captured[32768];
static size_t captured_length;

static int capture_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    CHECK(pthread_equal(pthread_self(), log_owner));
    size_t available = sizeof(captured) - captured_length;
    int n = vsnprintf(captured + captured_length, available, format, args);
    CHECK(n >= 0 && (size_t)n < available);
    captured_length += (size_t)n;
    va_end(args);
    return n;
}

void x4_rtc_receive_diag_reset(void) {}

#if X4_PUBLIC
X4PictureSettingsConfig x4_picture_settings_config(void)
{
    return (X4PictureSettingsConfig){.quality=X4_PIC_QUALITY_1080P};
}

X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption quality)
{
    CHECK(quality==X4_PIC_QUALITY_1080P);
    return X4_REQUEST_1080P_G8;
}
#endif

static const char unknown[] =
    "{\"type\":\"FAKE_PRIVATE_GAMERTAG\",\"id\":\"FAKE_PRIVATE_ID\","
    "\"target\":\"/streaming/systemUi/FAKE_PRIVATE_TARGET\","
    "\"content\":{\"type\":\"ShowMessage\",\"token\":\"FAKE_PRIVATE_TOKEN\"}}";
static const char lifetime[] =
    "{\"message\":\"authorizationResponse\",\"id\":\"FAKE_PRIVATE_ID\","
    "\"target\":\"/streaming/sessionLifetimeManagement/FAKE_PRIVATE_TARGET\","
    "\"content\":\"{\\\"reason\\\":\\\"KickForClosedGame\\\","
    "\\\"secondsUntilKick\\\":7,\\\"gamertag\\\":\\\"FAKE_PRIVATE_GAMERTAG\\\"}\"}";
static const char lifetime_numeric[] =
    "{\"type\":\"TransactionStart\",\"id\":98765432101,"
    "\"target\":\"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect\","
    "\"content\":\"{\\\"reason\\\":\\\"KickForOutOfStreamingTime\\\"}\"}";
static const char other[] =
    "{\"type\":\"Message\",\"target\":\"/streaming/FAKE_PRIVATE_TARGET\","
    "\"content\":{\"type\":\"ShowMessage\",\"password\":\"FAKE_PRIVATE_TOKEN\"}}";

static void feed(X4Rtc *rtc, int channel, const char *message)
{
    channel_message(channel, message, -(int)strlen(message)-1, rtc->slot);
}

#if !X4_PUBLIC
static unsigned lines_with(const char *part)
{
    unsigned n = 0;
    size_t length = strlen(part);
    for (const char *at = captured; (at = strstr(at, part)); at += length) ++n;
    return n;
}

static atomic_uint active_flood;

static void *flood(void *pointer)
{
    X4Rtc *rtc = pointer;
    for (unsigned i = 0; i < 100; ++i) feed(rtc, CONTROL, other);
    atomic_fetch_sub_explicit(&active_flood, 1, memory_order_release);
    return NULL;
}

static void *lifetime_producer(void *pointer)
{
    X4Rtc *rtc=pointer;
    static const char rejected[]="{\"type\":\"TransactionStart\",\"id\":false,"
        "\"target\":\"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect\","
        "\"content\":{\"reason\":\"KickForClosedGame\"}}";
    static const char accepted[]="{\"type\":\"TransactionStart\",\"id\":\"FAKE_PRIVATE_TRANSACTION+/==\","
        "\"target\":\"/streaming/sessionLifetimeManagement/serverInitiatedDisconnect\","
        "\"content\":{\"reason\":\"KickForClosedGame\"}}";
    feed(rtc,MESSAGE,rejected);
    feed(rtc,MESSAGE,accepted);
    return NULL;
}

static void closure_timeline(X4Rtc *rtc)
{
    atomic_store(&lab_first_video, 900000);
    rtc->nack_epoch = 3;
    lock(&callback_gate);
    lab_transport_context = rtc;
    unlock(&callback_gate);
    x4_rtc_lab_native_transport_state(80, 2); /* Connected is not a closure. */
    x4_rtc_lab_native_transport_state(80, 0);
    x4_rtc_lab_native_transport_state(87, 4);
    x4_rtc_lab_native_transport_state(80, 0); /* Duplicate closure. */
    x4_rtc_lab_native_transport_state(90, 0); /* Other native event. */
    for (unsigned i = 0; i < 4; ++i) lab_channel_closed(rtc->channels[i], rtc->slot);
    lab_observe_close(rtc, LAB_CLOSE_LOCAL_CONTROL, 0);
    lock(&callback_gate);
    lab_transport_context = NULL;
    unlock(&callback_gate);
    CHECK(!lines_with("XCloud4: RTC cierre orden="));
    lab_emit_closures(rtc);
    CHECK(lines_with("XCloud4: RTC cierre orden=") == 7);
    CHECK(strstr(captured, "orden=1 peer=3 desde_video_ms=100 parte=DTLS origen=observado"));
    CHECK(strstr(captured, "orden=2 peer=3 desde_video_ms=100 parte=SCTP origen=observado estado_rc=4"));
    CHECK(strstr(captured, "orden=3 peer=3 desde_video_ms=100 parte=chat origen=observado"));
    CHECK(strstr(captured, "orden=4 peer=3 desde_video_ms=100 parte=control origen=observado"));
    CHECK(strstr(captured, "orden=5 peer=3 desde_video_ms=100 parte=input origen=observado"));
    CHECK(strstr(captured, "orden=6 peer=3 desde_video_ms=100 parte=message origen=observado"));
    CHECK(strstr(captured, "orden=7 peer=3 desde_video_ms=100 parte=control origen=liberacion_local"));
}
#endif

int main(void)
{
    log_owner = pthread_self();
    open_diagnostics(false);
    X4Rtc *first = fixture(1);
    feed(first, MESSAGE, unknown);
    feed(first, CONTROL, lifetime);
    feed(first, CONTROL, lifetime_numeric);
    feed(first, CONTROL, other);
    CHECK(sent == 0);
#if !X4_PUBLIC
    CHECK(!captured_length);
    lab_emit_messages(first);
    CHECK(strstr(captured, "type=oculto target=/streaming/systemUi/* id=1 content_wire_bytes="));
    CHECK(strstr(captured, "content=type=ShowMessage"));
    CHECK(strstr(captured, "type=authorizationResponse target=/streaming/sessionLifetimeManagement/* id_tipo=string"));
    CHECK(strstr(captured, "content=reason=KickForClosedGame secondsUntilKick=7"));
    CHECK(strstr(captured, "type=TransactionStart target=/streaming/sessionLifetimeManagement/serverInitiatedDisconnect id_tipo=numero"));
    CHECK(strstr(captured, "content=reason=KickForOutOfStreamingTime"));
    CHECK(strstr(captured, "type=Message target=oculto id=0") &&
        lines_with("content=type=ShowMessage") == 1);
    lab_close_summary(first);
    CHECK(strstr(captured, "target=/streaming/systemUi/* total=1"));
    closure_timeline(first);
    destroy(first);

    open_diagnostics(true);
    X4Rtc *next = fixture(1);
    pthread_t threads[4];
    atomic_init(&active_flood, 4);
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_create(&threads[i], NULL, flood, next));
    while (atomic_load_explicit(&active_flood, memory_order_acquire)) {
        lab_emit_messages(next);
        sched_yield();
    }
    for (unsigned i = 0; i < 4; ++i) CHECK(!pthread_join(threads[i], NULL));
    lab_emit_messages(next);
    lab_emit_messages(next); /* Owner drain cannot repeat a record. */
    lab_close_summary(next);
    CHECK(lines_with("XCloud4: RTC mensaje orden=") == 64);
    const char *entry = captured;
    for (unsigned i = 1; i <= 64; ++i) {
        entry = strstr(entry, "XCloud4: RTC mensaje orden=");
        unsigned actual;
        CHECK(entry && sscanf(entry, "XCloud4: RTC mensaje orden=%u", &actual) == 1 && actual == i);
        ++entry;
    }
    CHECK(strstr(captured, "orden=64 canal=control") && !strstr(captured, "orden=65 canal=control"));
    CHECK(strstr(captured, "target=oculto total=400"));
    CHECK(sent == 0);
    size_t previous_length=captured_length;
    pthread_t producer;
    CHECK(!pthread_create(&producer,NULL,lifetime_producer,next));
    CHECK(!pthread_join(producer,NULL));
    CHECK(captured_length==previous_length&&sent==1);
    lab_emit_messages(next);
    lab_emit_messages(next);
    CHECK(lines_with("resultado=rechazado paso=id motivo=ausente_o_invalido")==1);
    CHECK(lines_with("resultado=aceptado paso=aviso motivo=terminal")==1);
    CHECK(lines_with("RTC lifetime paso=ack rc=0")==1);
    CHECK(lines_with("XCloud4: RTC mensaje orden=")==64);
    destroy(next);
#else
    CHECK(!captured_length);
    destroy(first);
#endif
    CHECK(!strstr(captured, "FAKE_PRIVATE") && !strstr(captured, "98765432101"));
    puts("PASS RTC launch logs: fixed safe fields, 64 lines across reconnect, ordered closure events; public silent");
    return 0;
}
