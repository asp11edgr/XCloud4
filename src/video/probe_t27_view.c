/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t27_view.h"
#if defined(X4_VIDEO_PROBE) && X4_VIDEO_PROBE && \
    defined(X4_PROBE_T26) && X4_PROBE_T26 && \
    defined(X4_PROBE_T27) && X4_PROBE_T27

#include "../ui/font.h"
#include <stdio.h>
#include <string.h>

enum { CARD_X = 72, CARD_Y = 48, CARD_W = 1776, CARD_H = 984,
    TEXT_X = 112, TEXT_RIGHT = 1808, TEXT_BOTTOM = 1032,
    TEXT_WIDTH = TEXT_RIGHT - TEXT_X };
static const uint32_t background = UINT32_C(0xff16110d);
static const uint32_t card = UINT32_C(0xff292119);
static const uint32_t foreground = UINT32_C(0xfff6f4ee);
static const uint32_t secondary = UINT32_C(0xffc9bcb0);
static const uint32_t accent = UINT32_C(0xff8ae34d);
static const uint32_t warning = UINT32_C(0xff80c8ff);
static const uint32_t danger = UINT32_C(0xffa5a5ff);

static void t27_rectangle(uint32_t *pixels, int x, int y, int width, int height,
    uint32_t color)
{
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            pixels[(size_t)(y + row) * X4_T27_VIEW_WIDTH + (size_t)(x + col)] = color;
}

/* Snapshot strings have fixed capacity and may not be NUL-terminated. The
 * font fitter also sanitizes malformed UTF-8 and stops at a newline. */
static void t27_snapshot_text(char out[X4_T27_VIEW_TEXT], const char *text)
{
    size_t count = 0;
    while (count + 1u < X4_T27_VIEW_TEXT && text[count]) ++count;
    memcpy(out, text, count);
    out[count] = 0;
}

static void t27_line(uint32_t *pixels, X4T27ViewMetrics *m, int y, int size,
    int weight, const char *text, uint32_t color)
{
    char fitted[512];
    int original_width = x4_text_width_px(size, weight, text);
    (void)x4_text_fit_utf8(fitted, sizeof(fitted), size, weight, text, TEXT_WIDTH);
    int width = x4_text_width_px(size, weight, fitted);
    /* Font pixels are top-of-line coordinates; glyph ink can extend below
     * the nominal point size. Reserve a conservative 1.5-size line box. */
    int line_bottom = y + (size * 3 + 1) / 2;
    ++m->text_lines;
    if (original_width > TEXT_WIDTH) ++m->ellipsized_lines;
    if (!m->minimum_font_px || (unsigned)size < m->minimum_font_px)
        m->minimum_font_px = (unsigned)size;
    if (TEXT_X + width > m->maximum_text_right)
        m->maximum_text_right = TEXT_X + width;
    if (line_bottom > m->maximum_text_bottom) m->maximum_text_bottom = line_bottom;
    if (TEXT_X < CARD_X || TEXT_X + width > TEXT_RIGHT ||
        y < CARD_Y || line_bottom > TEXT_BOTTOM) ++m->bounds_failures;
    x4_text_px(pixels, TEXT_X, y, size, weight, fitted, color);
}

static const char *mode_name(const X4T27ViewSnapshot *s)
{
    if (s->done) return "Medición finalizada";
    if (s->mode == X4_T26_MODE_MAXIMUM) return "Solo Decode · máxima velocidad";
    if (s->mode == X4_T26_MODE_PACED) return "GPU · presentación de producción";
    return "Preparando el banco";
}

int x4_t27_view_draw(uint32_t *pixels, const X4T27ViewSnapshot *s,
    X4T27ViewMetrics *optional_metrics)
{
    X4T27ViewMetrics local = {0};
    X4T27ViewMetrics *m = optional_metrics ? optional_metrics : &local;
    if (!pixels || !s) return -1;
    *m = (X4T27ViewMetrics){0};
    t27_rectangle(pixels, 0, 0, X4_T27_VIEW_WIDTH, X4_T27_VIEW_HEIGHT, background);
    t27_rectangle(pixels, CARD_X, CARD_Y, CARD_W, CARD_H, card);
    t27_rectangle(pixels, CARD_X, CARD_Y, 6, CARD_H, s->retained ? danger : accent);
    t27_line(pixels, m, 80, 42, X4_FONT_SEMIBOLD, X4_T27_VIEW_TITLE, foreground);
    t27_line(pixels, m, 142, 28, X4_FONT_REGULAR,
        "Pruebas locales de vídeo · XCloud4 Sonda", secondary);
    if (s->host_example)
        t27_line(pixels, m, 184, 28, X4_FONT_SEMIBOLD,
            "EJEMPLO HOST · sin mediciones de consola", warning);

    unsigned total = s->total ? s->total : X4_T27_CELL_COUNT;
    unsigned current = s->cell < total ? s->cell + 1u : total;
    char progress[128];
    (void)snprintf(progress, sizeof(progress), "Celda %u de %u", current, total);
    t27_line(pixels, m, 242, 36, X4_FONT_SEMIBOLD, progress, foreground);
    t27_line(pixels, m, 292, 28, X4_FONT_REGULAR, mode_name(s), secondary);

    char remaining[128];
    if (!s->remaining_us && !s->done) {
        (void)snprintf(remaining, sizeof(remaining), "Tiempo restante estimado: calculando…");
    } else {
        uint64_t seconds = s->remaining_us / UINT64_C(1000000);
        (void)snprintf(remaining, sizeof(remaining),
            "Tiempo restante estimado: %llu:%02u",
            (unsigned long long)(seconds / 60u), (unsigned)(seconds % 60u));
    }
    t27_line(pixels, m, 334, 28, X4_FONT_REGULAR, remaining, secondary);

    char status[X4_T27_VIEW_TEXT];
    t27_snapshot_text(status, s->status);
    if (!status[0])
        (void)snprintf(status, sizeof(status), "%s", s->retained ?
            "Recursos retenidos: banco detenido" : s->done ?
            "Banco finalizado" : "Preparando la siguiente medición…");
    t27_line(pixels, m, 382, 32, X4_FONT_SEMIBOLD, status,
        s->retained ? danger : foreground);

    t27_rectangle(pixels, TEXT_X, 440, TEXT_WIDTH, 12, UINT32_C(0xff4b4035));
    /* Progress describes completed cells, not elapsed time or measured speed. */
    unsigned completed = s->done ? total : s->cell < total ? s->cell : total;
    int filled = (int)((uint64_t)TEXT_WIDTH * completed / total);
    if (filled) t27_rectangle(pixels, TEXT_X, 440, filled, 12, s->retained ? danger : accent);

    t27_line(pixels, m, 476, 30, X4_FONT_SEMIBOLD,
        s->done ? "Resumen · dos bases / nueve celdas de presentación" :
        "Estado · base sin vídeo / medidas realistas con vídeo", foreground);
    unsigned rows = s->row_count < X4_T27_VIEW_ROWS ? s->row_count : X4_T27_VIEW_ROWS;
    if (rows) {
        for (unsigned i = 0; i < rows; ++i) {
            char row[X4_T27_VIEW_TEXT];
            t27_snapshot_text(row, s->rows[i]);
            t27_line(pixels, m, 522 + (int)i * 34, 28, X4_FONT_REGULAR, row, secondary);
            ++m->rows_drawn;
        }
    } else {
        static const char *const empty[] = {
            "Base: No ejecutado — sin mediciones",
            "Cola: No ejecutado — sin mediciones",
            "Mailbox: No ejecutado — sin mediciones"
        };
        for (unsigned i = 0; i < sizeof(empty) / sizeof(empty[0]); ++i) {
            t27_line(pixels, m, 522 + (int)i * 34, 28, X4_FONT_REGULAR, empty[i], secondary);
            ++m->rows_drawn;
        }
    }
    t27_line(pixels, m, 946, 28, X4_FONT_REGULAR,
        "Resultados: /data/xcloud4-probe/t27/", accent);
    t27_line(pixels, m, 988, 28, X4_FONT_REGULAR,
        s->retained ? "Recursos retenidos: no ejecutes otra celda." : s->done ?
        "Banco finalizado. Sal con OPTIONS." :
        "Si la app se cierra, vuelve a abrir la sonda.", secondary);
    return m->bounds_failures ? -1 : 0;
}
#endif
