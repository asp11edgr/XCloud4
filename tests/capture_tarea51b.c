/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "ui2_home.h"
#include "ui2_info.h"
#include "ui2_modal.h"
#include "ui2_shell.h"
#include "../src/core/app_identity.h"
#include "../src/update/update_manifest.h"

enum { WIDTH = 1920, HEIGHT = 1080 };
static uint32_t frame[WIDTH * HEIGHT];
static unsigned char rgb[3 * WIDTH * HEIGHT];
static unsigned calls, checks, clipped;
static const char *case_name;
static const char *stress;
static int stress_width;
static const char *expected_body, *expected_code;
static size_t body_bytes;
static unsigned code_hits;
static const char *expected_pkg;
static size_t pkg_bytes;
static const char *expected_action;
static unsigned action_hits;
static const char *expected_greeting;
static unsigned greeting_hits;

void __real_x4_text_px(uint32_t *, int, int, int, int, const char *, uint32_t);
void __wrap_x4_text_px(uint32_t *p, int x, int y, int size, int weight,
    const char *text, uint32_t color)
{
    if (p && text && *text) {
        int width = x4_text_width_px(size, weight, text);
        if (!(x >= 0 && y >= 0 && x + width <= WIDTH && y + size <= HEIGHT)) {
            fprintf(stderr, "OUTSIDE_FRAME %s %d,%d %dx%d %s\n", case_name, x, y, width, size, text);
            abort();
        }
        assert(strcmp(text, "…") != 0);
        if (expected_body) {
            const char *wanted = expected_body + body_bytes;
            const size_t bytes = strlen(text);
            if (strncmp(wanted, text, bytes) == 0 &&
                (wanted[bytes] == '\0' || wanted[bytes] == ' ' || wanted[bytes] == '\n')) {
                body_bytes += bytes;
                if (expected_body[body_bytes] == ' ' || expected_body[body_bytes] == '\n') ++body_bytes;
            }
        }
        if (expected_code && strcmp(text, expected_code) == 0) ++code_hits;
        if (expected_pkg && strncmp(expected_pkg + pkg_bytes, text, strlen(text)) == 0)
            pkg_bytes += strlen(text);
        if (expected_action && strcmp(text, expected_action) == 0) ++action_hits;
        if (stress) {
            size_t bytes = strlen(text);
            assert(bytes > 3 && memcmp(text + bytes - 3, "…", 3) == 0);
            assert(strncmp(text, stress, bytes - 3) == 0 && stress[bytes - 3] == ' ');
            assert(width <= stress_width);
            ++clipped;
        }
        ++calls;
    }
    __real_x4_text_px(p, x, y, size, weight, text, color);
}

void __real_x4_text_ellipsis_px(uint32_t *, int, int, int, int, const char *, int, uint32_t);
void __wrap_x4_text_ellipsis_px(uint32_t *p, int x, int y, int size, int weight,
    const char *text, int width, uint32_t color)
{
    if (p && text && *text) {
        char shown[1024];
        size_t bytes = x4_text_fit_utf8(shown, sizeof(shown), size, weight, text, width);
        assert(bytes && x >= 0 && y >= 0 && x + width <= WIDTH && y + size <= HEIGHT);
        assert(x4_text_width_px(size, weight, shown) <= width);
        if (expected_greeting) {
            if (strcmp(text, expected_greeting) == 0) {
                assert(strcmp(shown, expected_greeting) == 0);
                ++greeting_hits;
            }
        } else if (bytes >= 3 && memcmp(shown + bytes - 3, "…", 3) == 0
            && strcmp(text, shown) != 0) {
            assert(bytes > 3 && strncmp(text, shown, bytes - 3) == 0);
            assert(text[bytes - 3] == ' ');
        }
        ++calls;
    }
    __real_x4_text_ellipsis_px(p, x, y, size, weight, text, width, color);
}

static X4CoverRasters load_icon(void)
{
#if X4_PUBLIC
    const char *path = "assets/icon0.png";
#else
    const char *path = "assets/icon0-lab.png";
#endif
    FILE *fp = fopen(path, "rb");
    assert(fp && fseek(fp, 0, SEEK_END) == 0);
    long length = ftell(fp);
    assert(length > 0 && fseek(fp, 0, SEEK_SET) == 0);
    unsigned char *data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, fp) == (size_t)length);
    assert(fclose(fp) == 0);
    X4CoverRasters icon = {0};
    assert(x4_cover_decode(data, (size_t)length, &icon) == 0);
    free(data);
    return icon;
}

static void background(const X4CoverRaster *icon)
{
    X4Ui2Shell shell = { X4_UI2_ABOUT, X4_UI2_ABOUT, false };
    X4Ui2Status status = {0};
    x4_ui2_status_update(&status, X4_UI2_NETWORK_CABLE, 1080, 60, 21 * 60 + 30, "Jugador");
    x4_ui2_about_draw(frame, icon, X4_UPDATE_LATEST, NULL);
    x4_ui2_shell_draw(frame, &shell, &status);
}

static void emit(const char *name, bool audit_only)
{
    assert(calls > checks);
    checks = calls;
    if (audit_only)
        return;
    printf("FRAME %s\n", name);
    for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
        rgb[3 * i] = (unsigned char)frame[i];
        rgb[3 * i + 1] = (unsigned char)(frame[i] >> 8);
        rgb[3 * i + 2] = (unsigned char)(frame[i] >> 16);
    }
    assert(fwrite(rgb, 1, sizeof(rgb), stdout) == sizeof(rgb));
}

static void stress_word_cuts(void)
{
    const enum X4TextId strings[] = {
        X4_TR_51_UPDATE_ACCEPTED, X4_TR_51_UPDATE_ERR_SIGNATURE,
        X4_TR_51_UPDATE_ERR_HASH, X4_TR_51_UPDATE_ERR_SPACE,
    };
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); ++i) {
        stress = x4_tr(strings[i]);
        const char *split = strchr(stress, ' ');
        char first[128];
        assert(split && (size_t)(split - stress) + 4 <= sizeof(first));
        memcpy(first, stress, (size_t)(split - stress));
        memcpy(first + (split - stress), "…", 4);
        stress_width = x4_text_width_px(26, X4_FONT_REGULAR, first);
        assert(x4_ui2_text_bounded(frame, 200, 200, 26, X4_FONT_REGULAR,
            stress, stress_width, 38, 1, X4_UI2_TEXT) == 1);
    }
    stress = NULL;
    assert(clipped == sizeof(strings) / sizeof(*strings));
}

static void capture_errors(const X4CoverRaster *icon, enum X4Lang language, bool audit_only)
{
    static const struct {
        const char *name;
        X4Update51Error error;
        int32_t code;
        enum X4TextId id;
        const char *es, *en;
    } scenes[] = {
        { "error-version", X4_UPDATE51_OLD_VERSION, X4_UPDATE_VERIFY_VERSION,
          X4_TR_51_UPDATE_ERR_VERSION_FMT, "Ya tienes la versión %s.", "You already have version %s." },
        { "error-lectura", X4_UPDATE51_READ, -EIO, X4_TR_51C_UPDATE_ERR_READ_FMT,
          "No se pudo leer el paquete (código %s).", "Could not read the package (code %s)." },
        { "error-firma", X4_UPDATE51_BAD_SIGNATURE, X4_UPDATE_VERIFY_SIGNATURE, X4_TR_51_UPDATE_ERR_SIGNATURE,
          "La firma de la actualización no es válida.", "The update signature is invalid." },
        { "error-sha", X4_UPDATE51_BAD_HASH, X4_UPDATE_VERIFY_HASH, X4_TR_51_UPDATE_ERR_HASH,
          "El SHA-256 del archivo no coincide.", "The file's SHA-256 does not match." },
        { "error-espacio", X4_UPDATE51_NO_SPACE, -ENOSPC, X4_TR_51_UPDATE_ERR_SPACE,
          "La actualización requiere más espacio libre o no se pudo comprobar el espacio.",
          "The update needs more free space, or free space could not be checked." },
        { "error-manifiesto", X4_UPDATE51_BAD_MANIFEST, X4_UPDATE_VERIFY_MANIFEST, X4_TR_51C_UPDATE_ERR_MANIFEST,
          "El manifiesto de actualización no es válido.", "The update manifest is invalid." },
        { "error-paquete", X4_UPDATE51_BAD_PACKAGE, X4_UPDATE_VERIFY_PACKAGE, X4_TR_51C_UPDATE_ERR_PACKAGE,
          "El paquete no coincide con la actualización firmada.", "The package does not match the signed update." },
        { "error-ocupado", X4_UPDATE51_BUSY, -EBUSY, X4_TR_51C_UPDATE_ERR_BUSY,
          "Ya hay una actualización en curso o pendiente.", "An update is already running or pending." },
        { "error-red", X4_UPDATE51_NETWORK, -ENETUNREACH, X4_TR_51C_UPDATE_ERR_NETWORK,
          "No se pudo obtener la actualización de GitHub.", "Could not get the update from GitHub." },
        { "error-instalacion", X4_UPDATE51_INSTALL_ERROR, -EIO, X4_TR_51C_UPDATE_ERR_INSTALL,
          "La PS4 no pudo instalar el paquete.", "The PS4 could not install the package." },
        { "error-arranque", X4_UPDATE51_START_ERROR, -EAGAIN, X4_TR_51C_UPDATE_ERR_START,
          "No se pudo iniciar el actualizador.", "Could not start the updater." },
    };
    for (size_t i = 0; i < sizeof(scenes) / sizeof(*scenes); ++i) {
        case_name = scenes[i].name;
        background(icon);
        x4_ui2_modal_scrim(frame);
        const X4Update51View job = { .stage = X4_UPDATE51_FAILED,
            .error = scenes[i].error, .code = scenes[i].code };
        const char *translated = x4_tr(scenes[i].id);
        const char *format = language == X4_LANG_ES ? scenes[i].es :
            language == X4_LANG_EN ? scenes[i].en : translated;
        assert(*format && strcmp(translated, format) == 0);
        if (language != X4_LANG_ES && language != X4_LANG_EN)
            assert(strcmp(format, scenes[i].es) != 0 && strcmp(format, scenes[i].en) != 0);
        char code[16], expected[160];
        assert(snprintf(code, sizeof(code), "0x%08x", (unsigned)job.code) == 10);
        if (job.error == X4_UPDATE51_OLD_VERSION || job.error == X4_UPDATE51_READ) {
            int size = snprintf(expected, sizeof(expected), format,
                job.error == X4_UPDATE51_OLD_VERSION ? X4_PRODUCT_VERSION : code);
            assert(size > 0 && (size_t)size < sizeof(expected));
            expected_body = expected;
        } else {
            expected_body = format;
        }
        expected_code = code;
        body_bytes = code_hits = 0;
        x4_ui2_update_job_draw(&job, frame);
        assert(body_bytes == strlen(expected_body) && code_hits == (unsigned)(job.error != X4_UPDATE51_READ));
        expected_body = expected_code = NULL;
        emit(case_name, audit_only);
    }
    fprintf(stderr, "PASS T51C_ERRORS product=%s lang=%d scenes=%zu lines=%u\n",
        X4_PUBLIC ? "publica" : "lab", language, sizeof(scenes) / sizeof(*scenes), calls);
}

static void capture_manual(const X4CoverRaster *icon, enum X4Lang language, bool audit_only)
{
#if X4_PUBLIC
    const char *version = "1.3.1";
    const char *name = "XCloud4-1.3.1-publico.pkg";
#else
    const char *version = "0.7.110";
    const char *name = "XCloud4-0.7.110-lab-prueba-51g.pkg";
#endif
    char verified[192], body[512];
    (void)snprintf(verified, sizeof(verified), x4_tr(X4_TR_51F_MANUAL_VERIFIED_FMT), version);
    (void)snprintf(body, sizeof(body), "%s\n%s", verified, x4_tr(X4_TR_51F_MANUAL_STEPS));

    case_name = "instalacion-manual";
    background(icon);
    x4_ui2_modal_scrim(frame);
    expected_body = body;
    expected_pkg = name;
    body_bytes = pkg_bytes = 0;
    x4_ui2_update_manual_draw(frame, version, name, 0);
    assert(body_bytes == strlen(body) && pkg_bytes == strlen(name));
    expected_body = expected_pkg = NULL;
    emit(case_name, audit_only);

    case_name = "instalacion-manual-conflicto";
    background(icon);
    x4_ui2_modal_scrim(frame);
    expected_body = x4_tr(X4_TR_51F_MANUAL_CONFLICT);
    expected_pkg = name;
    body_bytes = pkg_bytes = 0;
    x4_ui2_update_manual_draw(frame, version, name, -EEXIST);
    assert(body_bytes == strlen(expected_body) && pkg_bytes == strlen(name));
    expected_body = expected_pkg = NULL;

    case_name = "instalacion-manual-nombre-largo";
    background(icon);
    x4_ui2_modal_scrim(frame);
    char long_name[129];
    memset(long_name, 'W', sizeof(long_name) - 5);
    memcpy(long_name + sizeof(long_name) - 5, ".pkg", 5);
    expected_pkg = long_name;
    pkg_bytes = 0;
    x4_ui2_update_manual_draw(frame, version, long_name, 0);
    assert(pkg_bytes == strlen(long_name));
    expected_pkg = NULL;
    fprintf(stderr, "PASS T51F_MANUAL product=%s lang=%d complete_names=3 lines=%u\n",
        X4_PUBLIC ? "publica" : "lab", language, calls);
}

#if X4_PUBLIC
int x4_catalog_assets_acquire(X4CatalogAssets *assets, const char *id, X4CatalogAssetView *view)
{
    (void)assets;
    (void)id;
    (void)view;
    assert(!"the fixture renders Home without network assets");
    return 0;
}

void x4_catalog_assets_release(X4CatalogAssets *assets, X4CatalogAssetView *view)
{
    (void)assets;
    (void)view;
    assert(!"the fixture has no network assets to release");
}

static void check_public_greeting(enum X4Lang language)
{
    static X4CatalogSnapshot catalog;
    const X4Ui2HomeModel model = { .mode = X4_UI2_HOME_EMPTY, .continuation = X4_CATALOG_MAX };
    X4Ui2Status status = {0};
    X4UiRect damage[2];
    case_name = "saludo-gamertag";
    expected_greeting = x4_tr(X4_TR_U49_HOME_WELCOME);
    if (language == X4_LANG_ES) assert(strcmp(expected_greeting, "Bienvenido a XCloud4") == 0);
    greeting_hits = 0;
    assert(x4_ui2_home_draw(&model, &catalog, NULL, 0, -1, false, frame, damage) == SIZE_MAX);
    assert(greeting_hits == 1);
    assert(x4_ui2_status_set(&status, X4_UI2_NETWORK_CABLE, "1080p", 0, catalog.gamertag));
    assert(status.initial[0] == '\0');

    memcpy(catalog.gamertag, "ñube51g", sizeof("ñube51g"));
    char greeting[76];
    (void)snprintf(greeting, sizeof(greeting), x4_tr(X4_TR_U49_HOME_WELCOME_FMT), catalog.gamertag);
    if (language == X4_LANG_ES) assert(strcmp(greeting, "Bienvenido, ñube51g") == 0);
    expected_greeting = greeting;
    greeting_hits = 0;
    assert(x4_ui2_home_draw(&model, &catalog, NULL, 0, -1, true, frame, damage) == SIZE_MAX);
    assert(greeting_hits == 1);
    assert(x4_ui2_status_set(&status, X4_UI2_NETWORK_CABLE, "1080p", 0, catalog.gamertag));
    assert(strcmp(status.initial, "Ñ") == 0);
    expected_greeting = NULL;
    x4_ui2_home_close();
    fprintf(stderr, "PASS T51G_GREETING lang=%d named=1 generic=1 avatar=1\n", language);
}

static void capture_applied(const X4CoverRaster *icon, enum X4Lang language, bool audit_only)
{
    const char *version = "1.3.1";
    const char *name = "XCloud4-1.3.1-publico.pkg";
    char updated[128], body[384];
    (void)snprintf(updated, sizeof(updated), x4_tr(X4_TR_51F_UPDATED_FMT), version);
    (void)snprintf(body, sizeof(body), "%s\n%s", updated, x4_tr(X4_TR_51G_REMOVE_PKG));

    case_name = "actualizacion-instalada";
    background(icon);
    x4_ui2_modal_scrim(frame);
    expected_body = body;
    expected_pkg = name;
    body_bytes = pkg_bytes = 0;
    x4_ui2_update_applied_draw(frame, version, name);
    assert(body_bytes == strlen(body) && pkg_bytes == strlen(name));
    expected_body = expected_pkg = NULL;
    emit(case_name, audit_only);

    case_name = "accion-publica";
    background(icon);
    x4_ui2_modal_scrim(frame);
    X4UpdateNotice notice = {0};
    assert(x4_apple_update_notice_offer(&notice, version, "v1.3.1", true, false));
    notice.can_install = true;
    notice.install_focused = true;
    expected_action = x4_tr(X4_TR_51_UPDATE_NOW);
    action_hits = 0;
    x4_ui2_update_notice_draw(&notice, frame);
    assert(action_hits == 1);
    expected_action = NULL;

    const unsigned before = calls;
    const X4Update51View choose = {.stage = X4_UPDATE51_CHOOSE};
    const X4Update51View accepted = {.stage = X4_UPDATE51_ACCEPTED};
    x4_ui2_update_job_draw(&choose, frame);
    x4_ui2_update_job_draw(&accepted, frame);
    assert(calls == before);
    fprintf(stderr, "PASS T51G_PUBLIC_UI lang=%d exact_name=1 update_now=1 bgft_actions=0\n", language);
    check_public_greeting(language);
}
#endif

int main(int argc, char **argv)
{
    static const char *const names[X4_LANG_COUNT] = { "es", "en", "pt-br", "fr", "it", "de" };
    static const char *const locales[X4_LANG_COUNT] = { "es-MX", "en-US", "pt-BR", "fr-FR", "it-IT", "de-DE" };
    static const char *const markets[X4_LANG_COUNT] = { "MX", "US", "BR", "FR", "IT", "DE" };
    assert(argc >= 2 && argc <= 4);
    int language = 0;
    while (language < X4_LANG_COUNT && strcmp(argv[1], names[language]) != 0)
        ++language;
    assert(language < X4_LANG_COUNT);
    bool audit_only = false, errors_only = false, manual = false, applied = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--audit-only") == 0) audit_only = true;
        else if (strcmp(argv[i], "--errors-only") == 0) errors_only = true;
        else if (strcmp(argv[i], "--manual") == 0) manual = true;
        else if (strcmp(argv[i], "--manual-and-updated") == 0) manual = applied = true;
        else assert(!"Unknown capture option");
    }
    x4_lang_set((enum X4Lang)language, locales[language], markets[language]);
    x4_lang_set_ui((enum X4Lang)language);
    assert(x4_font_init("assets/fonts/inter.x4font") == 0);
    X4CoverRasters icons = load_icon();
    const X4CoverRaster *icon = &icons.surface[X4_COVER_GRID];
    assert(icon->rgba && icon->width && icon->height);
    if (errors_only || manual) {
        assert(!errors_only || !manual);
        if (manual) {
            capture_manual(icon, (enum X4Lang)language, audit_only);
#if X4_PUBLIC
            if (applied) capture_applied(icon, (enum X4Lang)language, audit_only);
#else
            assert(!applied);
#endif
        }
        else capture_errors(icon, (enum X4Lang)language, audit_only);
        x4_cover_rasters_free(&icons);
        x4_font_close();
        return 0;
    }
    case_name = "acerca-de";
    background(icon);
    emit(case_name, audit_only);

    case_name = "aviso-actualizar";
    background(icon);
    x4_ui2_modal_scrim(frame);
    X4UpdateNotice notice = {0};
#if X4_PUBLIC
    assert(x4_apple_update_notice_offer(&notice, "1.3.1", "v1.3.1", true, false));
#else
    assert(x4_apple_update_notice_offer(&notice, "99.99.99", "lab-99.99.99", true, false));
    notice.local = true;
#endif
    notice.can_install = true;
    notice.install_focused = true;
    x4_ui2_update_notice_draw(&notice, frame);
    emit(case_name, audit_only);

    static const struct {
        const char *name;
        X4Update51View job;
    } scenes[] = {
        { "descarga-mitad", { .stage = X4_UPDATE51_DOWNLOADING, .received = 9050000,
            .total = 18100000, .busy = true } },
        { "verificando", { .stage = X4_UPDATE51_VERIFYING, .busy = true } },
        { "instalando", { .stage = X4_UPDATE51_INSTALLING, .busy = true } },
#if !X4_PUBLIC
        { "instalacion-aceptada", { .stage = X4_UPDATE51_ACCEPTED } },
#endif
        { "error-firma", { .stage = X4_UPDATE51_FAILED, .error = X4_UPDATE51_BAD_SIGNATURE, .code = -1 } },
        { "error-sha", { .stage = X4_UPDATE51_FAILED, .error = X4_UPDATE51_BAD_HASH, .code = -2 } },
        { "error-espacio", { .stage = X4_UPDATE51_FAILED, .error = X4_UPDATE51_NO_SPACE, .code = -3 } },
    };
    for (size_t i = 0; i < sizeof(scenes) / sizeof(*scenes); ++i) {
        case_name = scenes[i].name;
        background(icon);
        x4_ui2_modal_scrim(frame);
        x4_ui2_update_job_draw(&scenes[i].job, frame);
        emit(case_name, audit_only);
    }

    case_name = "palabras-enteras";
    stress_word_cuts();
    fprintf(stderr, "PASS T51B_LAYOUT product=%s lang=%s scenes=%zu lines=%u whole_word_cases=%u\n",
        X4_PUBLIC ? "publica" : "lab", names[language], sizeof(scenes) / sizeof(*scenes) + 2, calls, clipped);
    x4_cover_rasters_free(&icons);
    x4_font_close();
    return 0;
}
