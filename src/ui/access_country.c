/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "access_country.h"
#include "lang.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define x4_access_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define x4_access_mkdir(p) mkdir(p, 0777)
#endif

#define ACCESS_MAGIC 0x31413458u
#define ACCESS_VERSION 1u
#define ACCESS_PATH "/data/xcloud4/access-v1.bin"
#define ACCESS_ACCEPTED 0x100u
typedef struct {
    uint32_t magic, version;
    char country[4];
    uint32_t accepted, checksum;
} AccessRecord;
static _Atomic unsigned active;
static char custom_path[256];
static X4AccessCountryUi card_ui;
static X4AccessCountryCardPhase card_phase;

static uint32_t record_checksum(const AccessRecord *r)
{
    uint32_t sum = r->magic ^ r->version ^ (r->accepted * 0x9e3779b9u);
    for (unsigned i = 0; i < sizeof(r->country); ++i)
        sum = (sum * 33u) ^ (unsigned char)r->country[i];
    return sum ^ 0xa5a55a5au;
}

static const char *settings_path(void)
{
    return custom_path[0] ? custom_path : ACCESS_PATH;
}

bool x4_access_country_set_path(const char *path)
{
    if (!path) { custom_path[0] = 0; return true; }
    size_t n = strlen(path);
    if (!n || n >= sizeof(custom_path) - 5u) return false;
    memcpy(custom_path, path, n + 1);
    return true;
}

void x4_access_country_init(void)
{
    unsigned loaded = X4_ACCESS_AUTO;
    FILE *f = fopen(settings_path(), "rb");
    if (f) {
        AccessRecord r;
        size_t n = fread(&r, 1, sizeof(r), f);
        int tail = fgetc(f);
        bool good = !ferror(f);
        fclose(f);
        if (good && n == sizeof(r) && tail == EOF && r.magic == ACCESS_MAGIC &&
            r.version == ACCESS_VERSION && r.accepted <= 1u && r.checksum == record_checksum(&r)) {
            for (unsigned i = 0; i < X4_ACCESS_COUNTRY_COUNT; ++i) {
                char code[4] = {0};
                size_t size = strlen(x4_access_country_code((X4AccessCountry)i));
                memcpy(code, x4_access_country_code((X4AccessCountry)i), size);
                if (!memcmp(code, r.country, sizeof(code)) && (!i || r.accepted)) {
                    loaded = i | (r.accepted ? ACCESS_ACCEPTED : 0u);
                    break;
                }
            }
        }
    }
    atomic_store_explicit(&active, loaded, memory_order_release);
    printf("XCloud4: pais de acceso=%s origen=guardado\n", x4_access_country_code((X4AccessCountry)(loaded & 0xffu)));
}

X4AccessCountry x4_access_country_get(void)
{
    return (X4AccessCountry)(atomic_load_explicit(&active, memory_order_acquire) & 0xffu);
}

bool x4_access_country_warning_accepted(void)
{
    return (atomic_load_explicit(&active, memory_order_acquire) & ACCESS_ACCEPTED) != 0;
}

static bool save_record(X4AccessCountry country, bool accepted)
{
    const char *path = settings_path();
    char dir[256], tmp[264];
    size_t n = strlen(path);
    if (n >= sizeof(dir) || n + 5u >= sizeof(tmp)) return false;
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
#if defined(_WIN32)
    char *backslash = strrchr(dir, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    if (slash && slash != dir) {
        *slash = 0;
        if (!strncmp(dir, "/data/xcloud4", 13)) (void)x4_access_mkdir("/data");
        (void)x4_access_mkdir(dir);
    }
    int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(tmp)) return false;
    AccessRecord r = {.magic = ACCESS_MAGIC, .version = ACCESS_VERSION, .accepted = accepted ? 1u : 0u};
    memcpy(r.country, x4_access_country_code(country), strlen(x4_access_country_code(country)));
    r.checksum = record_checksum(&r);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(&r, 1, sizeof(r), f) == sizeof(r);
    if (fflush(f)) ok = false;
#if !defined(_WIN32)
    if (ok && fsync(fileno(f))) ok = false;
#endif
    if (fclose(f)) ok = false;
    if (!ok) { (void)remove(tmp); return false; }
    /* Same-directory rename replaces atomically; never unlink the old record. */
    if (rename(tmp, path)) { (void)remove(tmp); return false; }
    return true;
}

bool x4_access_country_apply(X4AccessCountry country, bool accept_warning, const char *origin)
{
    if (!x4_access_country_valid(country) || !origin ||
        (strcmp(origin, "tarjeta") && strcmp(origin, "ajustes"))) return false;
    bool accepted = x4_access_country_warning_accepted() || accept_warning;
    if (country != X4_ACCESS_AUTO && !accepted) return false;
    if (!save_record(country, accepted)) return false;
    atomic_store_explicit(&active, (unsigned)country | (accepted ? ACCESS_ACCEPTED : 0u), memory_order_release);
    printf("XCloud4: pais de acceso=%s origen=%s\n", x4_access_country_code(country), origin);
    return true;
}

const char *x4_access_country_label(X4AccessCountry country)
{
    static const enum X4TextId labels[] = {X4_TR_ACCESS_AUTO, X4_TR_ACCESS_US,
        X4_TR_ACCESS_BR, X4_TR_ACCESS_JP, X4_TR_ACCESS_KR, X4_TR_ACCESS_PL};
    return x4_tr(labels[x4_access_country_valid(country) ? (unsigned)country : 0u]);
}

void x4_access_country_ui_open(X4AccessCountryUi *ui)
{
    if (!ui) return;
    *ui = (X4AccessCountryUi){.selected = x4_access_country_get()};
}

X4AccessCountryUiResult x4_access_country_ui_handle(X4AccessCountryUi *ui,
    unsigned actions, bool busy, const char *origin)
{
    if (!ui) return X4_ACCESS_UI_NONE;
    if (busy) return (actions & X4_ACCESS_UI_CANCEL) ? X4_ACCESS_UI_CANCEL_QUERY : X4_ACCESS_UI_NONE;
    if (ui->help) {
        if (actions & (X4_ACCESS_UI_CANCEL | X4_ACCESS_UI_HELP)) ui->help = false;
        return X4_ACCESS_UI_NONE;
    }
    if (actions & X4_ACCESS_UI_CANCEL) {
        ui->confirming = false;
        return X4_ACCESS_UI_BACK;
    }
    if (ui->confirming) {
        if (actions & X4_ACCESS_UI_CONFIRM) {
            if (x4_access_country_apply(ui->selected, true, origin)) {
                ui->confirming = false; ui->save_error = false;
                return X4_ACCESS_UI_APPLIED;
            }
            ui->save_error = true;
        }
        return X4_ACCESS_UI_NONE;
    }
    if (actions & X4_ACCESS_UI_HELP) { ui->help = true; return X4_ACCESS_UI_NONE; }
    if (actions & X4_ACCESS_UI_LEFT) {
        ui->selected = (X4AccessCountry)(((unsigned)ui->selected + X4_ACCESS_COUNTRY_COUNT - 1u) % X4_ACCESS_COUNTRY_COUNT);
        ui->save_error = false;
        return X4_ACCESS_UI_NONE;
    }
    if (actions & X4_ACCESS_UI_RIGHT) {
        ui->selected = (X4AccessCountry)(((unsigned)ui->selected + 1u) % X4_ACCESS_COUNTRY_COUNT);
        ui->save_error = false;
        return X4_ACCESS_UI_NONE;
    }
    if (actions & X4_ACCESS_UI_CONFIRM) {
        if (ui->selected != X4_ACCESS_AUTO && !x4_access_country_warning_accepted()) {
            ui->confirming = true;
            return X4_ACCESS_UI_NONE;
        }
        if (x4_access_country_apply(ui->selected, false, origin)) {
            ui->save_error = false;
            return X4_ACCESS_UI_APPLIED;
        }
        ui->save_error = true;
    }
    return X4_ACCESS_UI_NONE;
}

void x4_access_country_card_set(const X4AccessCountryUi *ui, X4AccessCountryCardPhase phase)
{
    if (ui) card_ui = *ui;
    card_phase = phase;
}

void x4_access_country_card_get(X4AccessCountryUi *ui, X4AccessCountryCardPhase *phase)
{
    if (ui) *ui = card_ui;
    if (phase) *phase = card_phase;
}
