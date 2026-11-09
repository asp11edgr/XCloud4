/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef NDEBUG
#error "This test requires active assertions"
#endif
#include "video/probe_t27_view.h"
#include "ui/font.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GUARD = 64, CARD_X = 72, CARD_Y = 48, CARD_RIGHT = 1848,
    CARD_BOTTOM = 1032 };
static const uint32_t canary = UINT32_C(0x6fa195e3);

static void check(uint32_t *allocation, const X4T27ViewSnapshot *snapshot)
{
    const size_t count = (size_t)X4_T27_VIEW_WIDTH * X4_T27_VIEW_HEIGHT;
    uint32_t *pixels = allocation + GUARD;
    X4T27ViewMetrics metrics;
    for (unsigned i = 0; i < GUARD; ++i) {
        allocation[i] = canary;
        pixels[count + i] = canary;
    }
    assert(x4_t27_view_draw(pixels, snapshot, &metrics) == 0);
    assert(metrics.minimum_font_px >= 28);
    assert(metrics.bounds_failures == 0);
    assert(metrics.maximum_text_right <= 1808);
    assert(metrics.maximum_text_bottom <= 1030);
    assert(metrics.rows_drawn <= X4_T27_VIEW_ROWS);
    for (unsigned i = 0; i < GUARD; ++i) {
        assert(allocation[i] == canary);
        assert(pixels[count + i] == canary);
    }
    /* Inspect real rendered pixels too, rather than trusting only the
     * renderer's coordinate accounting. Nothing may leak outside its card. */
    uint32_t background = pixels[0];
    for (int y = 0; y < X4_T27_VIEW_HEIGHT; ++y)
        for (int x = 0; x < X4_T27_VIEW_WIDTH; ++x)
            if (x < CARD_X || x >= CARD_RIGHT || y < CARD_Y || y >= CARD_BOTTOM)
                assert(pixels[(size_t)y * X4_T27_VIEW_WIDTH + (size_t)x] == background);
}

static void capture(const char *directory, const char *name, const uint32_t *pixels)
{
    if (!directory) return;
    char path[4096];
    int written = snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    assert(written > 0 && (size_t)written < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fprintf(file, "P6\n%d %d\n255\n", X4_T27_VIEW_WIDTH, X4_T27_VIEW_HEIGHT) > 0);
    unsigned char row[X4_T27_VIEW_WIDTH * 3];
    for (int y = 0; y < X4_T27_VIEW_HEIGHT; ++y) {
        for (int x = 0; x < X4_T27_VIEW_WIDTH; ++x) {
            uint32_t pixel = pixels[(size_t)y * X4_T27_VIEW_WIDTH + (size_t)x];
            row[x * 3] = (unsigned char)pixel;
            row[x * 3 + 1] = (unsigned char)(pixel >> 8);
            row[x * 3 + 2] = (unsigned char)(pixel >> 16);
        }
        assert(fwrite(row, 1, sizeof(row), file) == sizeof(row));
    }
    assert(fclose(file) == 0);
}

static void snapshot_row(X4T27ViewSnapshot *s, unsigned row, const char *text)
{
    assert(row < X4_T27_VIEW_ROWS);
    (void)snprintf(s->rows[row], sizeof(s->rows[row]), "%s", text);
    if (s->row_count <= row) s->row_count = row + 1u;
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    assert(x4_font_init(argv[1]) == 0);
    assert(!x4_font_is_fallback());
    const char *directory = argc == 3 ? argv[2] : NULL;
    size_t count = (size_t)X4_T27_VIEW_WIDTH * X4_T27_VIEW_HEIGHT;
    uint32_t *allocation = malloc((count + 2u * GUARD) * sizeof(*allocation));
    assert(allocation);
    uint32_t *pixels = allocation + GUARD;
    X4T27ViewSnapshot s = {.mode = X4_T27_VIEW_PREPARING, .host_example = true};
    (void)snprintf(s.status, sizeof(s.status), "No ejecutado · preparando el banco");
    check(allocation, &s);
    capture(directory, "t27-inicial", pixels);

    /* Cell/time below are illustrative UI inputs, never console evidence. */
    s.cell = 8;
    s.mode = X4_T26_MODE_PACED;
    s.remaining_us = UINT64_C(900000000);
    (void)snprintf(s.status, sizeof(s.status), "EJEMPLO HOST · medición en curso");
    snapshot_row(&s, 0, "Base: No ejecutado — sin mediciones");
    snapshot_row(&s, 1, "Cola: No ejecutado — sin mediciones");
    snapshot_row(&s, 2, "Mailbox: No ejecutado — sin mediciones");
    check(allocation, &s);
    capture(directory, "t27-progreso", pixels);

    s.cell = 13;
    s.mode = X4_T26_MODE_MAXIMUM;
    (void)snprintf(s.status, sizeof(s.status), "EJEMPLO HOST · variante rechazada; se omite");
    snapshot_row(&s, 3, "Query: rechazo de ejemplo — no ejecutado en consola");
    check(allocation, &s);
    capture(directory, "t27-variante-rechazada", pixels);

    s = (X4T27ViewSnapshot){.cell = X4_T27_CELL_COUNT,
        .done = true, .host_example = true};
    (void)snprintf(s.status, sizeof(s.status), "EJEMPLO HOST · final sin mediciones");
    snapshot_row(&s, 0, "Base: No ejecutado — sin mediciones");
    snapshot_row(&s, 1, "Cola: No ejecutado — sin mediciones");
    snapshot_row(&s, 2, "Mailbox: No ejecutado — sin mediciones");
    check(allocation, &s);
    capture(directory, "t27-final-sin-mediciones", pixels);

    s.retained = true;
    (void)snprintf(s.status, sizeof(s.status), "EJEMPLO HOST · cierre no confirmado");
    check(allocation, &s);
    capture(directory, "t27-recursos-retenidos", pixels);

    /* Fixed-size untrusted-length strings, malformed UTF-8, oversized counts
     * and estimates must stay inside the same card at a readable size. */
    memset(s.status, 'W', sizeof(s.status));
    memset(s.rows, 'W', sizeof(s.rows));
    s.rows[1][0] = (char)0xff;
    s.rows[2][0] = '\n';
    s.row_count = UINT_MAX;
    s.cell = UINT_MAX;
    s.total = UINT_MAX;
    s.remaining_us = UINT64_MAX;
    check(allocation, &s);
    X4T27ViewMetrics metrics;
    assert(x4_t27_view_draw(pixels, &s, &metrics) == 0);
    assert(metrics.rows_drawn == X4_T27_VIEW_ROWS);
    assert(metrics.ellipsized_lines >= X4_T27_VIEW_ROWS - 1u);
    assert(x4_t27_view_draw(pixels, &s, NULL) == 0);
    assert(x4_t27_view_draw(NULL, &s, NULL) == -1);
    assert(x4_t27_view_draw(pixels, NULL, NULL) == -1);
    free(allocation);
    x4_font_close();
    puts("PASS T27 view: real font, readable bounds, pixel guards; captures are EJEMPLO HOST");
    return 0;
}
