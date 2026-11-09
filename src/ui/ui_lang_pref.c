/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui_lang_pref.h"
#include "lang.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#include <sys/stat.h>
#else
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#endif

static const unsigned char magic[] = {'X', '4', 'L', 'A', 'N', 'G', 1, 0};
enum { CHOICE_OFFSET = sizeof(magic), CHECKSUM_OFFSET = CHOICE_OFFSET + 1,
    RECORD_BYTES = CHECKSUM_OFFSET + 4 };
static X4UiLangPreference current;
static char preference_path[512] = X4_UI_LANG_PREF_PATH;

/* One request owned by the UI thread. The status publishes requested to the
 * worker and publishes the write result back before the UI applies it. */
static _Atomic X4UiLangSave save_status;
static atomic_bool stopping;
static X4UiLangPreference requested;
static bool worker_started;
#ifdef X4_OPENORBIS
static OrbisPthread worker;
static OrbisKernelSema wake_event;
#else
static pthread_t worker;
static sem_t wake_event;
#endif

static bool valid(X4UiLangPreference preference)
{
    return (unsigned)preference < X4_UI_LANG_COUNT;
}

static void encode(unsigned char record[RECORD_BYTES], X4UiLangPreference preference)
{
    memcpy(record, magic, sizeof(magic));
    record[CHOICE_OFFSET] = (unsigned char)preference;
    uint32_t sum = UINT32_C(2166136261);
    for (unsigned i = 0; i < CHECKSUM_OFFSET; ++i)
        sum = (sum ^ record[i]) * UINT32_C(16777619);
    for (unsigned i = 0; i < 4; ++i)
        record[CHECKSUM_OFFSET + i] = (unsigned char)(sum >> (i * 8));
}

static X4UiLangPreference load(void)
{
    FILE *file = fopen(preference_path, "rb");
    if (!file) return X4_UI_LANG_AUTO;
    unsigned char record[RECORD_BYTES];
    bool ok = fread(record, 1, sizeof(record), file) == sizeof(record) &&
        fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) ok = false;
    if (!ok || !valid((X4UiLangPreference)record[CHOICE_OFFSET])) return X4_UI_LANG_AUTO;
    X4UiLangPreference preference = (X4UiLangPreference)record[CHOICE_OFFSET];
    unsigned char expected[RECORD_BYTES];
    encode(expected, preference);
    return memcmp(record, expected, sizeof(record)) ? X4_UI_LANG_AUTO : preference;
}

static bool save(X4UiLangPreference preference)
{
    char temporary[sizeof(preference_path) + sizeof(".tmp")];
    if (!preference_path[0]) return false;
    const int n = snprintf(temporary, sizeof(temporary), "%s.tmp", preference_path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) return false;
#ifdef X4_OPENORBIS
    if (!strcmp(preference_path, X4_UI_LANG_PREF_PATH)) (void)mkdir("/data/xcloud4", 0777);
#endif
    FILE *file = fopen(temporary, "wb");
    if (!file) return false;
    unsigned char record[RECORD_BYTES];
    encode(record, preference);
    bool ok = fwrite(record, 1, sizeof(record), file) == sizeof(record);
    if (fclose(file)) ok = false;
    if (ok && !rename(temporary, preference_path)) return true;
    (void)remove(temporary);
    return false;
}

static void apply(X4UiLangPreference preference)
{
    static const enum X4Lang manual[] = {
        [X4_UI_LANG_ES] = X4_LANG_ES, [X4_UI_LANG_EN] = X4_LANG_EN,
        [X4_UI_LANG_PT_BR] = X4_LANG_PT_BR, [X4_UI_LANG_FR] = X4_LANG_FR,
        [X4_UI_LANG_IT] = X4_LANG_IT, [X4_UI_LANG_DE] = X4_LANG_DE,
    };
    current = preference;
    x4_lang_set_ui(preference == X4_UI_LANG_AUTO ? x4_lang_system_ui() : manual[preference]);
}

static int wake_worker(void)
{
#ifdef X4_OPENORBIS
    return sceKernelSignalSema(wake_event, 1);
#else
    return sem_post(&wake_event);
#endif
}

static void *write_preferences(void *unused)
{
    (void)unused;
    for (;;) {
#ifdef X4_OPENORBIS
        if (sceKernelWaitSema(wake_event, 1, NULL)) sceKernelUsleep(10000);
#else
        while (sem_wait(&wake_event) && errno == EINTR) {}
#endif
        if (atomic_load_explicit(&save_status, memory_order_acquire) == X4_UI_LANG_SAVE_PENDING) {
            const bool ok = save(requested);
            atomic_store_explicit(&save_status, ok ? X4_UI_LANG_SAVE_DONE : X4_UI_LANG_SAVE_FAILED,
                memory_order_release);
        }
        if (atomic_load_explicit(&stopping, memory_order_acquire)) return NULL;
    }
}

void x4_ui_lang_pref_start(void)
{
    if (worker_started || !preference_path[0]) return;
    atomic_store_explicit(&stopping, false, memory_order_relaxed);
#ifdef X4_OPENORBIS
    if (sceKernelCreateSema(&wake_event, "x4-ui-lang-wake", ORBIS_KERNEL_SEMA_ATTR_TH_FIFO,
            0, 1, NULL)) return;
    OrbisPthreadAttr attr;
    int rc = scePthreadAttrInit(&attr);
    if (!rc) {
        rc = scePthreadAttrSetstacksize(&attr, 256u * 1024u);
        if (!rc) rc = scePthreadCreate(&worker, &attr, write_preferences, NULL, "x4-ui-lang");
        (void)scePthreadAttrDestroy(&attr);
    }
    if (rc) (void)sceKernelDeleteSema(wake_event);
#else
    if (sem_init(&wake_event, 0, 0)) return;
    const int rc = pthread_create(&worker, NULL, write_preferences, NULL);
    if (rc) (void)sem_destroy(&wake_event);
#endif
    worker_started = rc == 0;
}

void x4_ui_lang_pref_stop(void)
{
    if (!worker_started) return;
    atomic_store_explicit(&stopping, true, memory_order_release);
    (void)wake_worker();
#ifdef X4_OPENORBIS
    (void)scePthreadJoin(worker, NULL);
    (void)sceKernelDeleteSema(wake_event);
#else
    (void)pthread_join(worker, NULL);
    (void)sem_destroy(&wake_event);
#endif
    worker_started = false;
    (void)x4_ui_lang_pref_poll();
}

void x4_ui_lang_pref_init(const char *path)
{
    x4_ui_lang_pref_stop();
    const int n = snprintf(preference_path, sizeof(preference_path), "%s",
        path ? path : X4_UI_LANG_PREF_PATH);
    if (n < 0 || (size_t)n >= sizeof(preference_path)) preference_path[0] = 0;
    apply(preference_path[0] ? load() : X4_UI_LANG_AUTO);
    x4_ui_lang_pref_start();
}

X4UiLangPreference x4_ui_lang_pref_get(void)
{
    return current;
}

X4UiLangSave x4_ui_lang_pref_request(X4UiLangPreference preference)
{
    if (!valid(preference)) return X4_UI_LANG_SAVE_FAILED;
    if (preference == current) return X4_UI_LANG_SAVE_DONE;
    if (!worker_started ||
        atomic_load_explicit(&save_status, memory_order_acquire) != X4_UI_LANG_SAVE_IDLE)
        return X4_UI_LANG_SAVE_FAILED;
    requested = preference;
    atomic_store_explicit(&save_status, X4_UI_LANG_SAVE_PENDING, memory_order_release);
    if (!wake_worker()) return X4_UI_LANG_SAVE_PENDING;
    atomic_store_explicit(&save_status, X4_UI_LANG_SAVE_IDLE, memory_order_release);
    return X4_UI_LANG_SAVE_FAILED;
}

X4UiLangSave x4_ui_lang_pref_poll(void)
{
    const X4UiLangSave status = atomic_load_explicit(&save_status, memory_order_acquire);
    if (status == X4_UI_LANG_SAVE_DONE) apply(requested);
    if (status == X4_UI_LANG_SAVE_DONE || status == X4_UI_LANG_SAVE_FAILED)
        atomic_store_explicit(&save_status, X4_UI_LANG_SAVE_IDLE, memory_order_release);
    return status;
}

const char *x4_ui_lang_pref_label(X4UiLangPreference preference)
{
    static const enum X4TextId labels[X4_UI_LANG_COUNT] = {
        X4_TR_50_LANG_AUTO, X4_TR_50_LANG_ES, X4_TR_50_LANG_EN, X4_TR_50_LANG_PT_BR,
        X4_TR_50_LANG_FR, X4_TR_50_LANG_IT, X4_TR_50_LANG_DE,
    };
    return x4_tr(labels[valid(preference) ? preference : X4_UI_LANG_AUTO]);
}

