/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "probe_controller.h"
#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "../video/probe_t26_controller.h"
int x4_probe_controller_run(void) { return x4_t26_controller_run(); }
#elif X4_VIDEO_PROBE
#include "lifecycle.h"
#include "build_identity.h"
#include "../video/display.h"
#include "../video/probe_case_catalog.h"
#include "../video/probe_metrics.h"
#include "../video/probe_report_file.h"
#include "../video/probe_files.h"
#include "../video/probe_memory.h"
#include "../video/probe_pipeline.h"
#include "../media/live_media.h"
#include "../input/controller.h"
#include "../ui/font.h"
#include <orbis/libkernel.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { WAIT_CLOSE_US = 10000000 };
static const uint32_t WHITE = 0xffeeeeee, MUTED = 0xffb4bcc0, GREEN = 0xff70da75;
static const char *mode_name(X4ProbeMode mode)
{ return mode == X4_PROBE_PACED ? "paced" : "maximum-speed"; }
static const char *output_memory_name(unsigned choice)
{ return choice == X4_PROBE_MEMORY_CHOICE_CACHEABLE ? "WB_ONION (0), experimental" : "WC_GARLIC (3), actual"; }
static const char *pipeline_name(unsigned choice)
{
    switch (choice) {
    case X4_PROBE_PIPELINE_DECODE_ONLY: return "Solo Decode · sin presentación";
    case X4_PROBE_PIPELINE_TWO: return "Pipeline nuevo · 2 conversores · más reciente";
    case X4_PROBE_PIPELINE_THREE: return "Pipeline nuevo · 3 conversores · más reciente";
    default: return "Pipeline actual (predeterminado)";
    }
}
static const char *layout_name(unsigned layout)
{
    return layout == X4_PROBE_LAYOUT_VERIFIED ? "verificado" :
        layout == X4_PROBE_LAYOUT_INDETERMINATE ? "indeterminado" :
        layout == X4_PROBE_LAYOUT_FAILED ? "rechazado" : "pendiente";
}
static void label(uint32_t *p, int x, int y, int size, const char *text, uint32_t color)
{ x4_text_ellipsis_px(p, x, y, size, 0, text, X4_WIDTH - x - 96, color); }
static const char *rc_text(int rc, char out[32])
{
    if (rc == X4_PROBE_RC_NOT_RUN) return "no ejecutado";
    snprintf(out, 32, "%d", rc); return out;
}
static void menu(uint32_t *p, unsigned selected, X4ProbeMode mode,
    unsigned memory_choice, unsigned pipeline_choice, const X4ProbeStatus *status,
    const char *notice, bool live, bool retained)
{
    for (size_t i = 0; i < (size_t)X4_WIDTH * X4_HEIGHT; ++i) p[i] = 0xff1b1713;
    label(p, 96, 72, 48, "XCloud4 · Sonda local de vídeo", WHITE);
    label(p, 96, 142, 28, "Muestras sintéticas. No hay conexión a Xbox ni prueba de streaming.", MUTED);
    char line[256];
    snprintf(line, sizeof(line), "Modo: %s", mode == X4_PROBE_PACED ? "Ritmo temporal del clip" : "Máxima velocidad sin espera temporal");
    label(p, 96, 199, 32, line, GREEN);
    snprintf(line, sizeof(line), "%s · L2: cambiar con el caso cerrado", pipeline_name(pipeline_choice));
    label(p, 96, 239, 27, line, GREEN);
    const unsigned first = selected / 5 * 5;
    snprintf(line, sizeof(line), "Muestras %u–%u de %u · compatibilidad primero; estrés después",
        first + 1, first + 5 < X4_PROBE_ALL_CASE_COUNT ? first + 5 : (unsigned)X4_PROBE_ALL_CASE_COUNT,
        (unsigned)X4_PROBE_ALL_CASE_COUNT);
    label(p, 96, 280, 24, line, MUTED);
    for (unsigned i = first; i < X4_PROBE_ALL_CASE_COUNT && i < first + 5; ++i) {
        const X4ProbeCase *c = x4_probe_case_at(i);
        snprintf(line, sizeof(line), "%s %s · perfil %u, nivel %u.%u · %u×%u · %u/%u FPS",
            i == selected ? ">" : " ", c->label, c->profile_idc, c->level_idc / 10, c->level_idc % 10,
            c->visible_width, c->visible_height, c->fps_num, c->fps_den);
        label(p, 96, 317 + (int)(i - first) * 54, 30, line, i == selected ? GREEN : WHITE);
    }
    if (status) {
        snprintf(line, sizeof(line), "Último caso: memoria %d · AU %llu · salida %llu · RGB %llu · layout %s",
            status->output_memory_type,
            (unsigned long long)status->input_aus, (unsigned long long)status->native_outputs,
            (unsigned long long)status->rgb_publications, layout_name(status->layout_verdict));
        label(p, 96, 615, 28, line, MUTED);
        char r0[32], r1[32], r2[32], r3[32], r4[32];
        snprintf(line, sizeof(line), "Fuente %s · consulta %s · creación %s · Decode %s · cierre %s",
            rc_text(status->source_result, r0), rc_text(status->query_result, r1),
            rc_text(status->create_result, r2), rc_text(status->decode_result, r3), rc_text(status->close_result, r4));
        label(p, 96, 664, 27, line, MUTED);
    }
    label(p, 96, 740, 30, notice, retained ? 0xff6666ee : GREEN);
    if (pipeline_choice == X4_PROBE_PIPELINE_CURRENT)
        snprintf(line, sizeof(line), "Salida: %s · L1/R1: cambiar solo con el caso cerrado", output_memory_name(memory_choice));
    else snprintf(line, sizeof(line), "Salida WB_ONION (0) · anillo de 4 · pila de 256 KiB por hilo");
    label(p, 96, 803, 27, line, MUTED);
    label(p, 96, 843, 25, "R2: Microprueba de memoria sin decoder · Perfil/nivel no garantiza compatibilidad.", MUTED);
    label(p, 96, 878, 27, "Cruceta: muestra · TRIÁNGULO: modo · X: iniciar · CÍRCULO: cancelar/volver", WHITE);
    label(p, 96, 930, 27, live ? "OPTIONS: cancelar y salir; espera el cierre seguro." : "CUADRADO: Comprobar archivos · OPTIONS: salir", WHITE);
}
static bool safe_id(const char *id)
{
    if (!id || !*id || strlen(id) > 80) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}
static void hash_hex(const uint8_t hash[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        out[i * 2] = digits[hash[i] >> 4]; out[i * 2 + 1] = digits[hash[i] & 15];
    }
    out[64] = 0;
}
static int report_dir(void)
{
    X4ProbeFileStatus result;
    return x4_probe_files_prepare(&result);
}
static const char *const file_steps[X4_PROBE_FILES_STEP_COUNT] = {
    "Comprobar /data", "Crear informe exclusivo", "Escribir informe",
    "Vaciar búfer de escritura", "Sincronizar archivo", "Cerrar informe",
    "Abrir informe de nuevo", "Leer informe", "Cerrar lectura", "Comparar contenido",
    "Abrir índice del clip", "Leer índice del clip", "Cerrar índice", "INFO openat (no obligatorio)"
};
typedef struct {
    X4ProbeStackReport main_report, test_report, default_report;
    bool ran, default_retained;
    int result, report_result, default_result;
    char path[X4_PROBE_FILE_PATH_MAX];
} StackCheck;
static void files_menu(uint32_t *pixels, const X4ProbeFileSelfcheck *check, const StackCheck *stacks)
{
    for (size_t i = 0; i < (size_t)X4_WIDTH * X4_HEIGHT; ++i) pixels[i] = 0xff1b1713;
    label(pixels, 96, 55, 42, "Comprobar archivos · sin decoder", WHITE);
    label(pixels, 96, 114, 26, "Informes directos en /data · creación exclusiva · clips del manifiesto", MUTED);
    char line[256];
    for (unsigned i = 0; i < X4_PROBE_FILES_STEP_COUNT; ++i) {
        const X4ProbeFileStatus *r = &check->step[i];
        bool info = i == X4_PROBE_FILES_OPENAT_INFO;
        if (check->ran[i])
            snprintf(line, sizeof(line), "%02u. %s: %s · retorno %lld · errno %d · estado %d",
                i + 1, file_steps[i], info ? "INFO" : r->result ? "FALLO" : "OK",
                (long long)r->raw_return, r->call_errno, r->result);
        else snprintf(line, sizeof(line), "%02u. %s: sin ejecutar", i + 1, file_steps[i]);
        label(pixels, 96, 158 + (int)i * 32, 24, line,
            check->ran[i] && r->result && !info ? 0xff6666ee : info ? MUTED : WHITE);
    }
    if (stacks && stacks->ran) {
        const X4ProbeStackReport *m = &stacks->main_report, *t = &stacks->test_report;
        snprintf(line, sizeof(line), "15. INFO pila principal: %llu B · getattr %d · getstacksize %d",
            (unsigned long long)m->reported_bytes, m->getattr_rc, m->getstacksize_rc);
        label(pixels, 96, 625, 25, line, MUTED);
        snprintf(line, sizeof(line), "16. INFO hilo de prueba: %llu / %llu B · crear %d · getattr %d · getstacksize %d · unir %d",
            (unsigned long long)t->reported_bytes, (unsigned long long)t->configured_bytes,
            t->create_rc, t->getattr_rc, t->getstacksize_rc, t->join_rc);
        label(pixels, 96, 660, 25, line, MUTED);
        const X4ProbeStackReport *d = &stacks->default_report;
        if (stacks->default_result == X4_PROBE_STACK_NOT_RUN)
            snprintf(line, sizeof(line), "17. INFO atributos NULL: sin ejecutar por el fallo anterior.");
        else snprintf(line, sizeof(line), "17. INFO atributos NULL: pila %llu B · guarda %llu B · crear %d · consultar %d · unir %d",
                (unsigned long long)d->reported_bytes, (unsigned long long)d->guard_bytes,
                d->create_rc, d->measure_rc, d->join_rc);
        label(pixels, 96, 695, 24, line, stacks->default_retained ? 0xff6666ee : MUTED);
        snprintf(line, sizeof(line), "Pilas: resultado %d · informe %d · INFO NULL %d · detalle en -stacks-", stacks->result, stacks->report_result, stacks->default_result);
        label(pixels, 96, 728, 24, line, stacks->result || stacks->report_result ? 0xff6666ee : MUTED);
    } else label(pixels, 96, 645, 25, "15–16. INFO pilas: sin ejecutar; se comprueban después de los archivos.", MUTED);
    label(pixels, 96, 762, 22, check->report_path, MUTED);
    snprintf(line, sizeof(line), "Registro acotado: %u llamadas · %u omitidas · INFO no exige openat exitoso",
        check->call_count, check->calls_omitted);
    label(pixels, 96, 797, 24, line, MUTED);
    const char *note = check->passed ? "13 pasos obligatorios correctos. Conserva ambos informes para verificar por FTP." :
        "No inicies vídeo: fallo obligatorio. Un archivo ya creado se conserva sin sobrescribir.";
    if (check->passed && stacks && stacks->ran && (stacks->result || stacks->report_result))
        note = "Archivos correctos; fallo de pila o de su informe. Vídeo bloqueado.";
    if (stacks && stacks->default_retained) note = "INFO NULL: hilo retenido. Detén la sesión; nuevos casos bloqueados.";
    label(pixels, 96, 838, 27, note, check->passed && !(stacks && stacks->default_retained) ? GREEN : MUTED);
    for (unsigned i = 0; i < X4_PROBE_FILES_OPENAT_INFO; ++i) {
        if (check->ran[i] && check->step[i].result) {
            snprintf(line, sizeof(line), "Detalle: %s · errno diagnóstico %d", check->step[i].operation, check->step[i].error_number);
            label(pixels, 96, 862, 25, line, 0xff6666ee); break;
        }
    }
    if (check->cleanup_failed) label(pixels, 96, 904, 24, "Cierre inseguro del diagnóstico: inicio bloqueado.", 0xff6666ee);
    label(pixels, 96, 954, 26, "CÍRCULO: volver · CUADRADO: comprobar otra vez · OPTIONS: salir", WHITE);
}

typedef struct { X4Display *display; int presentation_result; const StackCheck *stacks; } FilesView;
static void files_progress(const X4ProbeFileSelfcheck *check, void *context)
{
    FilesView *view = context;
    /* Display progress only. Bounded diagnostic logs publish after selfcheck. */
    files_menu(x4_display_pixels(view->display), check, view->stacks);
    int rc = x4_display_present(view->display);
    if (rc < 0 && !view->presentation_result) view->presentation_result = rc;
}
static int stack_check_report(StackCheck *s, uint64_t now)
{
    if (x4_probe_files_report_path(s->path, sizeof(s->path), "stacks", now)) return -1;
    FILE *f = x4_probe_report_create(s->path); if (!f) return -1;
    errno = 0;
    fprintf(f, "{\"schema\":\"X4PROBE_STACK_CHECK1\",\"build_id\":\"%s\",\"result\":%d,\"main\":", X4_BUILD_ID, s->result);
    x4_probe_stack_report_json(f, &s->main_report);
    fputs(",\"test_thread\":", f); x4_probe_stack_report_json(f, &s->test_report);
    fprintf(f, ",\"default_thread_result\":%d,\"default_thread_retained\":%s,\"default_thread\":",
        s->default_result, s->default_retained ? "true" : "false");
    if (s->default_result == X4_PROBE_STACK_NOT_RUN) fputs("null", f);
    else x4_probe_stack_report_json(f, &s->default_report);
    fputs(",\"informational\":true,\"main_stack_was_not_changed\":true}\n", f);
    int failed = ferror(f), saved = failed ? (errno ? errno : EIO) : 0;
    if (x4_probe_report_finish(f)) { failed = 1; if (!saved) saved = errno; }
    if (failed) errno = saved;
    return failed ? -1 : 0;
}
typedef struct {
    X4ProbeMemoryResult result;
    char path[X4_PROBE_FILE_PATH_MAX];
    int export_result;
    bool ran;
} MemoryCheck;
static double memory_median_rate(const X4ProbeMemoryRow *row, unsigned operation)
{
    uint64_t values[X4_PROBE_MEMORY_REPETITIONS];
    unsigned n = row->completed[operation];
    if (!n || n > X4_PROBE_MEMORY_REPETITIONS) return 0;
    for (unsigned i = 0; i < n; ++i) {
        values[i] = row->elapsed_us[operation][i];
        for (unsigned j = i; j && values[j] < values[j - 1]; --j) {
            uint64_t t = values[j]; values[j] = values[j - 1]; values[j - 1] = t;
        }
    }
    return x4_probe_memory_mb_s(values[n / 2]);
}
static void memory_menu(uint32_t *pixels, const MemoryCheck *check, bool running)
{
    for (size_t i = 0; i < (size_t)X4_WIDTH * X4_HEIGHT; ++i) pixels[i] = 0xff1b1713;
    label(pixels, 96, 64, 42, "Microprueba de memoria · sin decoder", WHITE);
    label(pixels, 96, 130, 28, "Bloques de 4 MiB · 5 repeticiones · MB/s decimales (mediana de duración)", MUTED);
    label(pixels, 96, 177, 26, "Lectura CPU, escritura CPU y memcpy a memoria normal; no mide salida de vídeo.", MUTED);
    char line[256];
    const X4ProbeMemoryResult *r = &check->result;
    for (unsigned i = 0; i < X4_PROBE_MEMORY_TYPES; ++i) {
        const X4ProbeMemoryRow *row = &r->rows[i];
        snprintf(line, sizeof(line), "%s (%d) · resultado %d", x4_probe_memory_type_name(i), row->native_type, row->result);
        label(pixels, 96, 246 + (int)i * 140, 30, line, row->result ? MUTED : GREEN);
        snprintf(line, sizeof(line), "Leer %.1f MB/s (%u/5) · escribir %.1f (%u/5) · copiar %.1f (%u/5)",
            memory_median_rate(row, X4_PROBE_MEMORY_READ), row->completed[X4_PROBE_MEMORY_READ],
            memory_median_rate(row, X4_PROBE_MEMORY_WRITE), row->completed[X4_PROBE_MEMORY_WRITE],
            memory_median_rate(row, X4_PROBE_MEMORY_COPY), row->completed[X4_PROBE_MEMORY_COPY]);
        label(pixels, 96, 290 + (int)i * 140, 27, line, WHITE);
        snprintf(line, sizeof(line), "Reservar %d · mapear %d · desmapear %d · liberar %d · contenido %s",
            row->allocate_rc, row->map_rc, row->unmap_rc, row->release_rc, row->content_verified ? "verificado" : "pendiente");
        label(pixels, 96, 328 + (int)i * 140, 23, line, MUTED);
    }
    snprintf(line, sizeof(line), "%s · resultado %d · liberación %s · informe %d",
        running ? "Midiendo" : r->cancelled ? "Cancelada" : "Terminada", r->result,
        r->cleanup_safe ? "correcta" : running ? "pendiente" : "INSEGURA", check->export_result);
    label(pixels, 96, 720, 28, line, !running && !r->cleanup_safe ? 0xff6666ee : GREEN);
    label(pixels, 96, 775, 22, check->path, MUTED);
    label(pixels, 96, 826, 26, "No extrapoles estos MB/s a la coherencia ni al rendimiento del decoder.", MUTED);
    label(pixels, 96, 915, 26, running ? "CÍRCULO: cancelar entre repeticiones · OPTIONS: cancelar y salir" :
        "CÍRCULO: volver · OPTIONS: salir", WHITE);
}
typedef struct {
    X4Display *display;
    X4Controller *pad;
    MemoryCheck *check;
    unsigned poll_frame;
    int presentation_result;
    bool exit_requested;
} MemoryView;
static int memory_progress(const X4ProbeMemoryResult *result, void *context)
{
    MemoryView *v = context;
    /* Backend calls outside measured intervals. One UI owner; no video worker. */
    (void)result;
    memory_menu(x4_display_pixels(v->display), v->check, true);
    int rc = x4_display_present(v->display);
    if (rc < 0 && !v->presentation_result) v->presentation_result = rc;
    x4_controller_read(v->pad, v->poll_frame++);
    if (v->pad->pressed & ORBIS_PAD_BUTTON_OPTIONS) v->exit_requested = true;
    return v->presentation_result || v->exit_requested || (v->pad->pressed & ORBIS_PAD_BUTTON_CIRCLE);
}
static bool variant_failed(const X4ProbeStatus *s)
{
    return x4_probe_memory_variant_failed(s);
}
static bool stack_failure_notice(char *notice, size_t size, const X4ProbeMetrics *metrics)
{
    static const char *const calls[] = {"iniciar atributos", "fijar tamaño", "crear hilo", "cerrar atributos",
        "iniciar consulta", "consultar hilo", "consultar tamaño", "consultar límites",
        "consultar guarda", "cerrar consulta", "medir pila", "unir hilo", "preparar hilo"};
    for (unsigned i = 0; i < 4; ++i) {
        const X4ProbeStackReport *r = x4_probe_metrics_stack(metrics, i);
        if (!r) continue;
        const int values[] = {r->attr_init_rc, r->set_size_rc, r->create_rc, r->attr_destroy_rc,
            r->getattr_init_rc, r->getattr_rc, r->getstacksize_rc, r->getstack_rc,
            r->getguardsize_rc, r->query_destroy_rc, r->measure_rc, r->join_rc, r->wrapper_result};
        for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            if (values[j] != X4_PROBE_STACK_NOT_RUN && values[j]) {
                snprintf(notice, size, "Pila de %s: %s falló (%d). Caso detenido.", r->name, calls[j], values[j]);
                return true;
            }
        }
    }
    return false;
}
static int case_report(const X4ProbeCase *c, X4ProbeMode mode, uint64_t started,
    const X4ProbeStatus *s, int metrics_result, const X4ProbeMetrics *metrics)
{
    if (!safe_id(c->id) || report_dir() < 0) return -1;
    char path[256];
    char report_case[128];
    int count = snprintf(report_case, sizeof(report_case), "%s-%s-case", c->id, mode_name(mode));
    if (count < 0 || (size_t)count >= sizeof(report_case) ||
        x4_probe_files_report_path(path, sizeof(path), report_case, started)) return -1;
    FILE *f = x4_probe_report_create(path);
    if (!f) {
        int saved = errno;
        printf("XCloud4 probe: case report create=-1 errno=%d path=%s\n", saved, path);
        errno = saved; return -1;
    }
    char clip_hash[65], index_hash[65], reference_hash[65];
    hash_hex(c->clip_sha256, clip_hash); hash_hex(c->index_sha256, index_hash);
    hash_hex(c->reference_sha256, reference_hash);
    errno = 0;
    fprintf(f, "{\"output_memory_type\":%d,\"output_memory_protection\":%d,\"output_memory_name\":\"%s\",",
        s->output_memory_type, s->output_memory_protection,
        s->output_memory_type == 0 ? "WB_ONION" : s->output_memory_type == 3 ? "WC_GARLIC" : "unknown");
    fprintf(f, "\"sample\":{\"clip_sha256\":\"%s\",\"index_sha256\":\"%s\",\"reference_sha256\":\"%s\","
        "\"visible_width\":%u,\"visible_height\":%u,\"coded_width\":%u,\"coded_height\":%u,"
        "\"crop_left\":%u,\"crop_top\":%u,\"crop_right\":%u,\"crop_bottom\":%u,"
        "\"profile_idc\":%u,\"level_idc\":%u,\"fps_num\":%u,\"fps_den\":%u,",
        clip_hash, index_hash, reference_hash, c->visible_width, c->visible_height,
        c->coded_width, c->coded_height, c->crop_left, c->crop_top, c->crop_right, c->crop_bottom,
        c->profile_idc, c->level_idc, c->fps_num, c->fps_den);
    fprintf(f, "\"AU_count\":%llu,\"duration_us\":%llu,\"chosen_DPB_frames\":%u,\"SPS_references\":%u,"
        "\"SPS_max_dec_frame_buffering\":%u,\"SPS_num_reorder_frames\":%u,\"matrix_coefficients\":%u,"
        "\"full_range\":%u,\"chroma_format_idc\":%u,\"bit_depth_luma\":%u,\"bit_depth_chroma\":%u},",
        (unsigned long long)c->au_count, (unsigned long long)c->duration_us, c->chosen_dpb_frames,
        c->max_num_ref_frames, c->max_dec_frame_buffering, c->num_reorder_frames,
        c->matrix_coefficients, c->full_range, c->chroma_format_idc, c->bit_depth_luma, c->bit_depth_chroma);
    fprintf(f, "\"compute_query_result\":%d,\"allocate_queue_result\":%d,\"flush_result\":%d,"
        "\"delete_decoder_result\":%d,\"release_queue_result\":%d,\"reference_close_result\":%d,"
        "\"output_reject_reason\":%u,",
        s->compute_query_result, s->allocate_queue_result, s->flush_result,
        s->delete_decoder_result, s->release_queue_result, s->reference_close_result,
        s->output_reject_reason);
    fprintf(f, "\"schema\":\"X4PROBE_CASE1\",\"source_id\":\"%s\",\"case_id\":\"%s\",\"mode\":\"%s\","
        "\"build_id\":\"%s\",\"native_result_is_not_streaming_support\":true,"
        "\"input_output_PTS_association\":false,\"warmup_rule_RGB_publications\":60,"
        "\"measurement_start_us\":%llu,\"observed_warmup_us\":%llu,"
        "\"source_result\":%d,\"reference_result\":%d,\"query_result\":%d,\"create_result\":%d,\"decode_result\":%d,\"close_result\":%d,\"pipeline_result\":%d,"
        "\"input_AU\":%llu,\"native_outputs\":%llu,\"RGB_conversions\":%llu,\"RGB_publications\":%llu,\"late_inputs\":%llu,"
        "\"maximum_input_late_us\":%llu,\"layout_verdict\":%u,\"storage_height\":%u,\"crop_y\":%u,"
        "\"layout_checks\":%llu,\"layout_ambiguous\":%llu,\"layout_reference_matches\":%llu,"
        "\"source_eof\":%s,\"finished\":%s,\"cancel_requested\":%s,\"metrics_dump_result\":%d,"
        "\"query_observed\":%s,\"create_observed\":%s,\"decode_observed\":%s,"
        "\"queried_compute_bytes\":%llu,\"queried_CPU_bytes\":%llu,\"queried_GPU_bytes\":%llu,"
        "\"queried_shared_bytes\":%llu,\"queried_frame_bytes\":%llu,\"rounded_native_bytes\":%llu,"
        "\"allocated_native_bytes\":%llu,\"peak_native_allocated_bytes\":%llu,\"native_policy_budget\":%llu,"
        "\"planned_total_exceeds_policy_budget\":%s,\"staging_bytes\":%llu,\"RGB_bytes\":%llu,\"thread_stacks\":",
        X4_SOURCE_ID, c->id, mode_name(mode), X4_BUILD_ID,
        (unsigned long long)s->measured_start_us,
        (unsigned long long)(s->measured_start_us >= started ? s->measured_start_us - started : 0),
        s->source_result, s->reference_result, s->query_result, s->create_result,
        s->decode_result, s->close_result, s->pipeline_result, (unsigned long long)s->input_aus,
        (unsigned long long)s->native_outputs, (unsigned long long)s->rgb_conversions,
        (unsigned long long)s->rgb_publications,
        (unsigned long long)s->late_inputs, (unsigned long long)s->max_input_late_us,
        s->layout_verdict, s->selected_storage_height, s->selected_crop_y,
        (unsigned long long)s->layout_checks, (unsigned long long)s->layout_ambiguous,
        (unsigned long long)s->layout_reference_matches, s->source_eof ? "true" : "false",
        s->finished ? "true" : "false", s->cancel_requested ? "true" : "false", metrics_result,
        s->query_result != X4_PROBE_RC_NOT_RUN ? "true" : "false",
        s->create_result != X4_PROBE_RC_NOT_RUN ? "true" : "false",
        s->decode_result != X4_PROBE_RC_NOT_RUN ? "true" : "false",
        (unsigned long long)s->queried_compute_bytes, (unsigned long long)s->queried_cpu_bytes,
        (unsigned long long)s->queried_gpu_bytes, (unsigned long long)s->queried_shared_bytes,
        (unsigned long long)s->queried_frame_bytes, (unsigned long long)s->rounded_native_bytes,
        (unsigned long long)s->allocated_native_bytes, (unsigned long long)s->peak_native_allocated_bytes,
        (unsigned long long)s->native_policy_budget,
        s->query_result == 0 && s->native_policy_budget && s->rounded_native_bytes > s->native_policy_budget ? "true" : "false",
        (unsigned long long)s->staging_bytes,
        (unsigned long long)s->rgb_bytes);
    if (metrics) x4_probe_metrics_stacks_json(f, metrics);
    else fputs("null", f); /* Active/retained writers: no unsafe snapshot. */
    fputs("}\n", f);
    int failed = ferror(f), write_error = failed ? (errno ? errno : EIO) : 0;
    if (x4_probe_report_finish(f)) { failed = 1; if (!write_error) write_error = errno; }
    if (failed) errno = write_error;
    printf("XCloud4 probe: case report result=%d errno=%d path=%s\n", failed ? -1 : 0, write_error, path);
    return failed ? -1 : 0;
}
static int export_metrics(const X4ProbeCase *c, X4ProbeMode mode, uint64_t started, X4ProbeMetrics *metrics)
{
    if (!safe_id(c->id) || report_dir() < 0) return -1;
    char path[256], report_case[128];
    int count = snprintf(report_case, sizeof(report_case), "%s-%s-metrics", c->id, mode_name(mode));
    if (count < 0 || (size_t)count >= sizeof(report_case) ||
        x4_probe_files_report_path(path, sizeof(path), report_case, started)) return -1;
    int rc = x4_probe_metrics_dump_file(metrics, path);
    printf("XCloud4 probe: metrics report result=%d path=%s\n", rc, path); return rc;
}
static void open_display_or_wait(X4Display *d)
{
    int rc = x4_display_open(d);
    if (rc < 0) {
        printf("XCloud4 probe: VideoOut failure=0x%08x\n", (unsigned)rc);
        if (x4_exit_prepare() >= 0) (void)x4_exit_request();
        for (;;) sceKernelUsleep(100000);
    }
}
static void open_ui_or_wait(X4Display *d, X4Controller *pad)
{
    (void)x4_font_init("/app0/assets/fonts/noto_sans.x4font");
    open_display_or_wait(d); x4_controller_init(pad);
}
int x4_probe_controller_run(void)
{
    X4Display display; X4Controller pad; X4LiveMedia *live = NULL;
    X4ProbeMetrics *metrics = NULL; X4ProbeStatus status = x4_probe_status_pending();
    unsigned selected = 0; X4ProbeMode mode = X4_PROBE_PACED;
    X4ProbeMemoryChoice memory_choice = {0};
    X4ProbePipelineChoice pipeline_choice = {0};
    bool pipeline_compatibility_passed[X4_PROBE_PIPELINE_COUNT * X4_PROBE_CASE_COUNT] = {false};
    unsigned active_memory_choice = X4_PROBE_MEMORY_CHOICE_CURRENT;
    const X4ProbeCase *active = NULL; X4ProbeMode active_mode = X4_PROBE_PACED;
    uint64_t started = 0, close_started = 0, last_presented_us = 0, stable_started_us = 0;
    bool closing = false, exit_requested = false, retained = false, measured = false;
    bool compatibility_passed[X4_PROBE_MEMORY_CHOICE_COUNT * X4_PROBE_CASE_COUNT] = {false};
    unsigned active_index = 0;
    bool files_checked = false, files_view = false, memory_view = false, session_stopped = false;
    X4ProbeFileSelfcheck file_check = {0};
    /* Single controller owner; keep the diagnostic results off its stack. */
    static StackCheck stack_check;
    static MemoryCheck memory_check;
    char notice[192] = "Primero pulsa CUADRADO: Comprobar archivos. Todavía no hay casos de vídeo.";
    setvbuf(stdout, NULL, _IONBF, 0); open_ui_or_wait(&display, &pad);
    printf("XCloud4 probe: local-only build=%s compatibility_cases=%u stress_cases=%u\n", X4_BUILD_ID,
        (unsigned)X4_PROBE_CASE_COUNT, (unsigned)X4_PROBE_STRESS_CASE_COUNT);
    for (unsigned frame = 0;; ++frame) {
        uint64_t now = sceKernelGetProcessTime(); x4_controller_read(&pad, frame);
        if (pad.pressed & ORBIS_PAD_BUTTON_OPTIONS) {
            exit_requested = true;
            if (live && !retained && !closing) {
                x4_live_media_probe_request_cancel(live); closing = true; close_started = now;
            }
        }
        if (live && !retained) {
            (void)x4_live_media_probe_snapshot(live, &status);
            if (!closing && (pad.pressed & ORBIS_PAD_BUTTON_CIRCLE)) {
                x4_live_media_probe_request_cancel(live); closing = true; close_started = now;
            }
            if (!measured && status.rgb_publications >= 60 && status.layout_verdict == X4_PROBE_LAYOUT_VERIFIED) {
                uint64_t boundary = sceKernelGetProcessTime();
                measured = x4_probe_metrics_begin_measurement(metrics, boundary);
                if (measured) {
                    stable_started_us = boundary;
                    printf("XCloud4 probe: stable begin=%llu elapsed=%llu RGB=%llu\n",
                        (unsigned long long)boundary, (unsigned long long)(boundary - started),
                        (unsigned long long)status.rgb_publications);
                }
            }
            x4_live_media_tick(live);
        }
        bool drew = false;
        /* Match production's unchanged scanout between pictures, periodic
         * 500 ms status redraw, and two-millisecond idle yield. A repeated
         * draw is tagged by the real mailbox generation, not counted NEW. */
        if (live && !retained && (x4_live_media_has_new_picture(live) ||
            (status.rgb_publications && now >= last_presented_us && now - last_presented_us >= 500000)))
            drew = x4_live_media_draw(live, x4_display_pixels(&display)) > 0;
        if (drew) {
            uint64_t generation = 0, output = 0; bool fresh = false;
            (void)x4_live_media_drawn(live, &generation, &output, &fresh);
            uint64_t begin = sceKernelGetProcessTime();
            int rc = x4_display_present_trace(&display, x4_live_media_trace(live), generation, fresh);
            if (rc >= 0) {
                x4_live_media_note_present(live, sceKernelGetProcessTime() - begin);
                last_presented_us = now;
            }
            else if (!closing) {
                printf("XCloud4 probe: presentation failed=0x%08x\n", (unsigned)rc);
                x4_probe_metrics_note(metrics, X4_PROBE_COUNT_ERRORS, 1);
                x4_live_media_probe_request_cancel(live); closing = true; close_started = now;
            }
            (void)output;
        }
        if (live && !retained && !closing && status.finished && !x4_live_media_has_new_picture(live)) {
            /* EOF/error already made the owner leave its loop. Do not turn
             * natural completion into a user-cancel result during teardown. */
            closing = true; close_started = now;
        }
        if (live && closing && !retained) {
            if (x4_live_media_probe_owner_done(live)) {
                (void)x4_live_media_probe_snapshot(live, &status);
                status.measured_start_us = stable_started_us;
                int rc = x4_live_media_close(live); status.close_result = rc;
                if (rc == 0) {
                    live = NULL; closing = false;
                    x4_probe_metrics_end(metrics, sceKernelGetProcessTime(), x4_probe_end_reason(&status));
                    X4ProbeMetricsSummary *summary = x4_probe_metrics_summary_storage(metrics);
                    bool compatible = active_index < X4_PROBE_CASE_COUNT &&
                        x4_probe_metrics_summary(metrics, summary) == 0 &&
                        x4_probe_compatibility_pass(active, &status, summary->total[X4_PROBE_COUNT_NEW_PRESENT],
                            summary->total[X4_PROBE_COUNT_ERRORS]);
                    int report = export_metrics(active, active_mode, started, metrics);
                    int case_rc = case_report(active, active_mode, started, &status, report, metrics);
                    if (report < 0 || case_rc < 0) files_checked = false;
                    /* A prerequisite without saved reports must be repeated;
                     * a later file selfcheck does not validate an older case. */
                    if (compatible && report == 0 && case_rc == 0) {
                        compatibility_passed[active_memory_choice * X4_PROBE_CASE_COUNT + active_index] = true;
                        printf("XCloud4 probe: native compatibility prerequisite accepted case=%s output_memory_type=%d\n",
                            active->id, x4_probe_memory_choice_type(active_memory_choice));
                    }
                    snprintf(notice, sizeof(notice), "Caso cerrado. Informes: métricas %d, resultado %d. Layout %s.",
                        report, case_rc, layout_name(status.layout_verdict));
                    bool stack_failed = stack_failure_notice(notice, sizeof(notice), metrics);
                    if (stack_failed) session_stopped = true;
                    if (!stack_failed && active_memory_choice == X4_PROBE_MEMORY_CHOICE_CACHEABLE && variant_failed(&status)) {
                        memory_choice.blocked[X4_PROBE_MEMORY_CHOICE_CACHEABLE] = true;
                        memory_choice.selected = X4_PROBE_MEMORY_CHOICE_CURRENT;
                        snprintf(notice, sizeof(notice), "Variante WB_ONION rechazada: no repetir. Memoria actual seleccionada; consulta el informe.");
                    } else if (!stack_failed && x4_probe_case_stops_session(&status)) {
                        session_stopped = true;
                        snprintf(notice, sizeof(notice), "%s", x4_probe_case_policy_rejected(&status) ?
                            "Rechazo por presupuesto, no incompatibilidad del decoder. Detén la sesión y conserva el informe." :
                            "Caso fallido. Sesión detenida; no inicies otra muestra. Conserva los informes.");
                    }
                    x4_probe_metrics_free(metrics); metrics = NULL;
                } else {
                    retained = true;
                    snprintf(notice, sizeof(notice), "Cierre falló (%d). Recursos retenidos; caso indeterminado.", rc);
                    (void)case_report(active, active_mode, started, &status, -5, NULL);
                }
            } else if (now >= close_started && now - close_started >= WAIT_CLOSE_US) {
                retained = true;
                status.measured_start_us = stable_started_us;
                snprintf(notice, sizeof(notice), "Cierre excedió 10 s. Recursos retenidos; no se fuerza su liberación.");
                (void)case_report(active, active_mode, started, &status, -6, NULL);
            }
        }
        if (!live) {
            bool moved = files_view || memory_view;
            if (!exit_requested && !retained && (pad.pressed & ORBIS_PAD_BUTTON_SQUARE)) {
                char path[X4_PROBE_FILE_PATH_MAX];
                if (x4_probe_files_report_path(path, sizeof(path), "files", now)) {
                    snprintf(notice, sizeof(notice), "No se pudo construir el nombre validado del informe.");
                    files_checked = false; continue;
                }
                files_checked = false; files_view = true; memory_view = false; moved = true;
                memset(&stack_check, 0, sizeof(stack_check));
                /* Idle only; these are the same helpers used by video cases.
                 * No producer or decoder exists during this action. */
                FilesView view = { .display = &display, .stacks = &stack_check };
                int file_rc = x4_probe_files_selfcheck(path, x4_probe_cases[0].index_path,
                    &file_check, files_progress, &view);
                files_checked = file_rc == 0 && view.presentation_result == 0;
                if (files_checked) {
                    stack_check.result = x4_probe_stack_check(&stack_check.main_report, &stack_check.test_report);
                    stack_check.default_result = X4_PROBE_STACK_NOT_RUN;
                    if (!stack_check.result)
                        stack_check.default_result = x4_probe_default_stack_check(&stack_check.default_report,
                            &stack_check.default_retained);
                    stack_check.ran = true;
                    stack_check.report_result = stack_check_report(&stack_check, now);
                    files_checked = stack_check.result == 0 && stack_check.report_result == 0;
                    /* A query/create failure in this INFO is nonblocking. A
                     * failed join is retained state and forbids new work. */
                    if (stack_check.default_retained) { retained = true; files_checked = false; }
                    printf("XCloud4 probe: stack selfcheck=%d export=%d main_bytes=%llu test_bytes=%llu configured=%llu path=%s\n",
                        stack_check.result, stack_check.report_result,
                        (unsigned long long)stack_check.main_report.reported_bytes,
                        (unsigned long long)stack_check.test_report.reported_bytes,
                        (unsigned long long)stack_check.test_report.configured_bytes, stack_check.path);
                    printf("XCloud4 probe: default stack INFO=%d bytes=%llu guard=%llu create=%d query=%d join=%d retained=%d\n",
                        stack_check.default_result, (unsigned long long)stack_check.default_report.reported_bytes,
                        (unsigned long long)stack_check.default_report.guard_bytes, stack_check.default_report.create_rc,
                        stack_check.default_report.measure_rc, stack_check.default_report.join_rc, stack_check.default_retained);
                }
                /* Publish the bounded idle diagnostics only after the I/O action.
                 * Never write one log per file call inside the backend. */
                for (unsigned i = 0; i < X4_PROBE_FILES_STEP_COUNT; ++i) {
                    if (!file_check.ran[i]) continue;
                    const X4ProbeFileStatus *r = &file_check.step[i];
                    printf("XCloud4 probe: files step=%u result=%d errno=%d raw=%lld call_errno=%d native=0x%08x informational=%d operation=%s\n",
                        i + 1, r->result, r->error_number, (long long)r->raw_return, r->call_errno,
                        (unsigned)r->native_result, i == X4_PROBE_FILES_OPENAT_INFO, r->operation);
                }
                for (unsigned i = 0; i < file_check.call_count; ++i) {
                    const X4ProbeFileCall *call = &file_check.calls[i];
                    printf("XCloud4 probe: filecall=%u call=%s raw=%lld errno=%d native=0x%08x\n",
                        i + 1, call->call, (long long)call->raw_return, call->error_number, (unsigned)call->native_result);
                }
                printf("XCloud4 probe: files selfcheck=%d screen=0x%08x calls=%u omitted=%u path=%s\n",
                    file_rc, (unsigned)view.presentation_result, file_check.call_count, file_check.calls_omitted, path);
            }
            if (files_view && (pad.pressed & ORBIS_PAD_BUTTON_CIRCLE)) {
                files_view = false; moved = true;
                snprintf(notice, sizeof(notice), "%s", files_checked ? "Archivos comprobados. Elige una muestra." :
                    "Comprobación incompleta: pulsa CUADRADO antes de iniciar vídeo.");
            }
            if (memory_view && (pad.pressed & ORBIS_PAD_BUTTON_CIRCLE)) { memory_view = false; moved = true; }
            if (!files_view && !memory_view && !retained && !exit_requested && (pad.pressed & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1))) {
                moved = true;
                if (pipeline_choice.selected != X4_PROBE_PIPELINE_CURRENT)
                    snprintf(notice, sizeof(notice), "Esta ruta usa WB_ONION. L2 cambia la ruta; L1/R1 solo se aplican al pipeline actual.");
                else if (!x4_probe_memory_choice_change(&memory_choice, true))
                    snprintf(notice, sizeof(notice), "La variante experimental ya falló: no repetir en esta sesión.");
                else snprintf(notice, sizeof(notice), "Salida seleccionada: %s. Compatibilidad independiente para esta memoria.", output_memory_name(memory_choice.selected));
            }
            if (!files_view && !memory_view && !retained && !exit_requested && (pad.pressed & ORBIS_PAD_BUTTON_L2)) {
                moved = true;
                if (x4_probe_pipeline_choice_change(&pipeline_choice, true))
                    snprintf(notice, sizeof(notice), "%s. El estrés exige su compatibilidad en esta ruta.", pipeline_name(pipeline_choice.selected));
                else snprintf(notice, sizeof(notice), "Las otras rutas están bloqueadas por un fallo de esta sesión.");
            }
            if (!files_view && !memory_view && ((pad.pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_LEFT)) && selected)) { --selected; moved = true; }
            if (!files_view && !memory_view && ((pad.pressed & (ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_RIGHT)) && selected + 1 < X4_PROBE_ALL_CASE_COUNT)) { ++selected; moved = true; }
            if (!files_view && !memory_view && (pad.pressed & ORBIS_PAD_BUTTON_TRIANGLE)) { mode = mode == X4_PROBE_PACED ? X4_PROBE_MAX_SPEED : X4_PROBE_PACED; moved = true; }
            if (!exit_requested && !retained && !session_stopped && !files_view && !memory_view &&
                (pad.pressed & ORBIS_PAD_BUTTON_R2)) {
                moved = true;
                if (!files_checked) {
                    snprintf(notice, sizeof(notice), "Primero completa CUADRADO: Comprobar archivos y pila.");
                } else if (x4_probe_files_report_path(memory_check.path, sizeof(memory_check.path), "memory", now)) {
                    snprintf(notice, sizeof(notice), "No se pudo preparar el nombre del informe de memoria.");
                } else {
                    memory_check.ran = true; memory_check.export_result = X4_PROBE_MEMORY_NOT_RUN;
                    MemoryView view = { .display = &display, .pad = &pad, .check = &memory_check, .poll_frame = frame };
                    int memory_rc = x4_probe_memory_run(&memory_check.result, NULL, memory_progress, &view);
                    memory_check.export_result = x4_probe_memory_export_json(memory_check.path, &memory_check.result);
                    memory_view = true; exit_requested = view.exit_requested;
                    if (!memory_check.result.cleanup_safe) retained = true;
                    /* A later user cancellation cannot erase an earlier native
                     * failure retained as the first result by the backend. */
                    if ((memory_rc && memory_rc != X4_PROBE_MEMORY_CANCELLED) || view.presentation_result) session_stopped = true;
                    if (memory_check.export_result) files_checked = false;
                    printf("XCloud4 probe: memory result=%d export=%d cleanup_safe=%d screen=%d path=%s\n",
                        memory_rc, memory_check.export_result, memory_check.result.cleanup_safe, view.presentation_result, memory_check.path);
                    snprintf(notice, sizeof(notice), "%s", retained ? "Microprueba: recursos retenidos. Detén la sesión." :
                        session_stopped ? "Microprueba fallida: detén la sesión y conserva el informe." : "Microprueba terminada. Revisa el informe antes de elegir WB_ONION.");
                }
            }
            if (!exit_requested && !moved && (pad.pressed & ORBIS_PAD_BUTTON_CROSS)) {
                /* Check the absolute /data report root before any producer.
                 * The exclusive probe prefix cannot name normal app data.
                 * Local-probe close never exports normal streaming traces. */
                if (!files_checked) {
                    snprintf(notice, sizeof(notice), "Primero pulsa CUADRADO y completa Comprobar archivos; inicio bloqueado.");
                    continue;
                }
                if (retained || session_stopped) {
                    snprintf(notice, sizeof(notice), "Sesión detenida por fallo o recursos retenidos. No inicies más casos.");
                    continue;
                }
                if (report_dir() < 0) {
                    files_checked = false;
                    int saved = errno;
                    snprintf(notice, sizeof(notice),
                        "Archivos de informes: fallo, errno %d. Repite Comprobar archivos.", saved);
                    continue;
                }
                int prerequisite = x4_probe_required_compatibility(selected);
                if (pipeline_choice.selected == X4_PROBE_PIPELINE_CURRENT && x4_probe_case_pipeline_only(selected)) {
                    snprintf(notice, sizeof(notice), "Esta muestra pertenece a Solo Decode y Pipeline nuevo. Elige una ruta con L2.");
                    continue;
                }
                if (pipeline_choice.selected != X4_PROBE_PIPELINE_CURRENT) {
                    if (!x4_probe_pipeline_choice_allowed(&pipeline_choice, prerequisite,
                        pipeline_compatibility_passed, X4_PROBE_CASE_COUNT)) {
                        if (prerequisite >= 0 && (unsigned)prerequisite < X4_PROBE_CASE_COUNT)
                            snprintf(notice, sizeof(notice), "Completa primero %s en la misma ruta elegida.", x4_probe_cases[prerequisite].label);
                        else snprintf(notice, sizeof(notice), "Ruta bloqueada o muestra sin compatibilidad equivalente verificada.");
                        continue;
                    }
                    unsigned run_choice = pipeline_choice.selected;
                    bool *passed = pipeline_compatibility_passed + run_choice * X4_PROBE_CASE_COUNT;
                    (void)x4_probe_compatibility_begin(selected, passed, X4_PROBE_CASE_COUNT);
                    active = x4_probe_case_at(selected); active_index = selected;
                    active_mode = mode; active_memory_choice = X4_PROBE_MEMORY_CHOICE_CACHEABLE;
                    /* The independent runtime owns its three VideoOut buffers.
                     * The idle UI is closed first; no streaming worker exists. */
                    x4_display_close(&display);
                    X4ProbePipelineRunResult result = x4_probe_pipeline_run(active, active_mode,
                        x4_probe_pipeline_converters(run_choice), &pad);
                    status = result.status; started = result.started_us;
                    exit_requested = result.exit_requested;
                    if (result.retained) {
                        /* A writer may still own a native/scanout buffer. Do not
                         * reopen VideoOut, free storage, or request unsafe exit. */
                        printf("XCloud4 probe: new pipeline retained resources; no further action is safe.\n");
                        for (;;) sceKernelUsleep(100000);
                    }
                    open_display_or_wait(&display);
                    if (result.report_result) files_checked = false;
                    if (result.compatibility_passed && !result.report_result &&
                        active_index < X4_PROBE_CASE_COUNT) passed[active_index] = true;
                    if (result.stopped) {
                        x4_probe_pipeline_choice_failed(&pipeline_choice, run_choice);
                        snprintf(notice, sizeof(notice), "Ruta detenida por fallo; revisa el informe. Las otras rutas siguen disponibles.");
                    } else snprintf(notice, sizeof(notice), "%s cerrado. Informe %d · layout %s.",
                        result.cancelled ? "Caso cancelado" : "Caso", result.report_result, layout_name(status.layout_verdict));
                    continue; /* Never launch a second route on the same X edge. */
                }
                if (!x4_probe_memory_choice_allowed(&memory_choice, prerequisite,
                    compatibility_passed, X4_PROBE_CASE_COUNT)) {
                    if (prerequisite >= 0 && (unsigned)prerequisite < X4_PROBE_CASE_COUNT)
                        snprintf(notice, sizeof(notice), "Antes del estrés, completa %s con layout y cierre válidos.",
                            x4_probe_cases[prerequisite].label);
                    else snprintf(notice, sizeof(notice), "Muestra de estrés sin compatibilidad equivalente verificada; inicio bloqueado.");
                    continue;
                }
                (void)x4_probe_compatibility_begin(selected,
                    compatibility_passed + memory_choice.selected * X4_PROBE_CASE_COUNT, X4_PROBE_CASE_COUNT);
                active = x4_probe_case_at(selected); active_index = selected;
                active_memory_choice = memory_choice.selected;
                active_mode = mode; started = sceKernelGetProcessTime();
                metrics = x4_probe_metrics_create(started);
                status = x4_probe_status_pending();
                status.output_memory_type = x4_probe_memory_choice_type(active_memory_choice);
                status.output_memory_protection = 0x33;
                measured = false; last_presented_us = stable_started_us = 0;
                int rc = -1; live = metrics ? x4_live_media_probe_create_memory(active, active_mode, metrics,
                    status.output_memory_type, &rc) : NULL;
                if (!live) {
                    session_stopped = true;
                    snprintf(notice, sizeof(notice), "No se pudo preparar el caso: %d.", rc);
                    if (metrics) {
                        status.pipeline_result = rc;
                        x4_probe_metrics_end(metrics, started, 3);
                        int report = export_metrics(active, active_mode, started, metrics);
                        (void)case_report(active, active_mode, started, &status, report, metrics);
                        (void)stack_failure_notice(notice, sizeof(notice), metrics);
                        x4_probe_metrics_free(metrics); metrics = NULL;
                    }
                } else {
                    rc = x4_live_media_start(live);
                    if (rc < 0) { x4_live_media_probe_request_cancel(live); closing = true; close_started = started; }
                    snprintf(notice, sizeof(notice), "Preparando muestra; OPTIONS cancela y sale.");
                }
            }
        }
        if (exit_requested && (!live || retained)) {
            if (!live) {
                X4ProbeFileStatus file_close;
                (void)x4_probe_files_shutdown(&file_close); files_checked = false;
                printf("XCloud4 probe: directory close=%d errno=%d\n", file_close.result, file_close.error_number);
            }
            x4_controller_close(&pad); x4_display_close(&display); x4_font_close();
            int rc = x4_exit_prepare(); if (rc >= 0) rc = x4_exit_request();
            if (rc >= 0) { for (unsigned i = 0; i < 100; ++i) sceKernelUsleep(100000); rc = -10; }
            open_ui_or_wait(&display, &pad); exit_requested = false;
            snprintf(notice, sizeof(notice), "La salida no terminó (%d). OPTIONS permite volver a solicitarla.", rc);
        }
        if (!drew && (!live || retained || !status.rgb_publications)) {
            if (files_view && !live) files_menu(x4_display_pixels(&display), &file_check, &stack_check);
            else if (memory_view && !live) memory_menu(x4_display_pixels(&display), &memory_check, false);
            else menu(x4_display_pixels(&display), selected, live ? active_mode : mode,
                live ? active_memory_choice : memory_choice.selected, pipeline_choice.selected,
                active ? &status : NULL, notice, live != NULL, retained);
            int rc = x4_display_present(&display);
            if (rc < 0 && live && !retained && !closing) {
                x4_live_media_probe_request_cancel(live); closing = true; close_started = now;
            }
        } else if (!drew) sceKernelUsleep(2000);
    }
}
#else
int x4_probe_controller_run(void) { return -1; }
#endif
