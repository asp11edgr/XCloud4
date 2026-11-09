/* SPDX-License-Identifier: GPL-3.0-only */
#include "boot_diag51.h"

#if !X4_PUBLIC
#include "diag_file51.h"
#include "../auth/xsts_claims_diag.h"
#include <errno.h>
#include <inttypes.h>

void ui_boot_event(UiBootDiag *diag, enum UiBootEvent event, uint64_t now)
{
    if (!diag->saved && (event == BOOT_HOME_LAST || !diag->event[event]))
        diag->event[event] = now - diag->started_us + 1;
}

static bool write_xsts_claims(FILE *file, const X4XstsDiagSnapshot *snapshot)
{
    if (!x4_diag51_printf(file, "  \"xsts_respuestas_omitidas\": %" PRIu64 ",\n  \"xsts_claims\": [",
            snapshot->revision - snapshot->count)) return false;
    for (unsigned i = 0; i < snapshot->count; ++i) {
        const X4XstsDiagResponse *response = &snapshot->responses[i];
        const char *party = response->party == X4_XSTS_DIAG_MARKET ? "http://mp.microsoft.com/" :
            "http://gssv.xboxlive.com/";
        if (!x4_diag51_printf(file, "%s\n    {\"relying_party\": \"%s\", \"http\": %d, "
                "\"nombres_omitidos\": %u, \"nombres\": [", i ? "," : "", party,
                response->status, response->omitted)) return false;
        for (unsigned j = 0; j < response->count; ++j)
            if (!x4_diag51_printf(file, "%s\"%s\"", j ? ", " : "", response->names[j])) return false;
        if (!x4_diag51_puts(file, "]}")) return false;
    }
    return x4_diag51_puts(file, snapshot->count ? "\n  ]\n}\n" : "]\n}\n");
}

bool ui_boot_diag_save(UiBootDiag *diag, uint64_t now, uint64_t skipped, bool exiting)
{
    if (diag->saved && diag->xsts_revision == x4_xsts_diag_revision()) return true;
    if (!exiting && (now - diag->started_us < UINT64_C(60000000) || now < diag->next_write_us))
        return false;
    static const char *const names[BOOT_EVENT_COUNT] = {
        [BOOT_RESTORED] = "sesion_restaurada_ms", [BOOT_GAMERTAG] = "gamertag_conocido_ms",
        [BOOT_CATALOG_REQUESTED] = "catalogo_peticion_ms", [BOOT_CATALOG_RECEIVED] = "catalogo_llegada_ms",
        [BOOT_RECENTS_REQUESTED] = "recientes_peticion_ms", [BOOT_RECENTS_RECEIVED] = "recientes_llegada_ms",
        [BOOT_SIGLS_REQUESTED] = "sigl_peticion_ms", [BOOT_SIGLS_RECEIVED] = "sigl_llegada_ms",
        [BOOT_HOME_FIRST] = "inicio_primer_dibujo_ms", [BOOT_HOME_LAST] = "inicio_ultimo_dibujo_ms",
        [BOOT_LANGUAGE] = "idioma_carga_ms",
        [BOOT_SPLASH_HIDDEN] = "splash_oculto_ms",
    };
    static const char path[] = "/data/xcloud4/ui-boot-diag.json";
    /* Only the UI thread writes the file. Keep the bounded copy off its
     * stack and release the auth lock before starting any file I/O. */
    static X4XstsDiagSnapshot xsts;
    x4_xsts_diag_snapshot(&xsts);
    char temporary[X4_DIAG51_PATH_CAPACITY];
    FILE *file = x4_diag51_open(path, temporary, now);
    if (file) {
        bool ok = x4_diag51_puts(file, "{\n  \"schema\": 1,\n");
        for (unsigned i = 0; ok && i < BOOT_EVENT_COUNT; ++i) {
            if (diag->event[i])
                ok = x4_diag51_printf(file, "  \"%s\": %" PRIu64 ",\n", names[i], (diag->event[i] - 1) / 1000);
            else
                ok = x4_diag51_printf(file, "  \"%s\": null,\n", names[i]);
        }
        if (ok) {
            if (diag->splash_called)
                ok = x4_diag51_printf(file, "  \"splash_codigo\": %" PRId32 ",\n", diag->splash_code);
            else
                ok = x4_diag51_puts(file, "  \"splash_codigo\": null,\n");
        }
        if (ok) ok = x4_diag51_printf(file, "  \"dibujos_inicio\": %" PRIu64 ",\n"
            "  \"fotogramas_saltados\": %" PRIu64 ",\n", diag->home_draws, diag->saved ? diag->saved_skipped : skipped) &&
            write_xsts_claims(file, &xsts);
        diag->write_errno = x4_diag51_commit(file, temporary, path, ok);
    } else {
        diag->write_errno = errno ? errno : EIO;
    }
    if (diag->write_errno) {
        diag->next_write_us = now + UINT64_C(1000000);
        return false;
    }
    if (!diag->saved) diag->saved_skipped = skipped;
    diag->saved = true;
    diag->xsts_revision = xsts.revision;
    diag->next_write_us = 0;
    return true;
}
#endif
