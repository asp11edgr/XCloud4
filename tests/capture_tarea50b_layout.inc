/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the host capture driver after the inherited renderer wrappers.
 * Audit the real fit functions rather than a separate font approximation. */
static struct {
    unsigned fit_calls, bounded_calls, cuts, failures, sidebar_rows;
    int sidebar_size, sidebar_weight;
    const char *bounded_cursor;
} capture50b_layout;

static bool layout50b_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xA0 || cp == 0x202F;
}

static const char *layout50b_skip_space(const char *text)
{
    while (*text) {
        const char *next = text;
        if (!layout50b_space(x4_utf8_next(&next))) break;
        text = next;
    }
    return text;
}

static bool layout50b_ellipsis(const char *text, size_t *prefix)
{
    const size_t len = strlen(text);
    const size_t marker_len = sizeof("…") - 1u;
    if (len < marker_len || strcmp(text + len - marker_len, "…")) return false;
    *prefix = len - marker_len;
    return true;
}

static void layout50b_bad(const char *route, const char *source, const char *drawn)
{
    ++capture50b_layout.failures;
    fprintf(stderr, "T50B_LAYOUT_FAIL scene=%s route=%s source=[%.130s] rendered=[%.130s]\n",
        active_scene ? active_scene : "?", route, source ? source : "", drawn ? drawn : "");
}

static void layout50b_check_fit(const char *route, const char *source, const char *drawn)
{
    if (!source || !*source || !drawn || !strcmp(source, drawn)) return;
    size_t prefix = 0;
    if (!layout50b_ellipsis(drawn, &prefix)) {
        if (!*drawn) layout50b_bad(route, source, drawn);
        return;
    }
    ++capture50b_layout.cuts;
    source = layout50b_skip_space(source);
    while (prefix && (drawn[prefix - 1u] == ' ' || drawn[prefix - 1u] == '\t')) --prefix;
    if (!prefix || strncmp(source, drawn, prefix)) {
        layout50b_bad(route, source, drawn);
        return;
    }
    const char *end = source + prefix;
    if (*end && !layout50b_space(x4_utf8_next(&end)))
        layout50b_bad(route, source, drawn);
}

size_t __real_x4_text_fit_utf8(char *, size_t, int, int, const char *, int);
size_t __wrap_x4_text_fit_utf8(char *output, size_t capacity, int size, int weight,
    const char *text, int width)
{
    const size_t result = __real_x4_text_fit_utf8(output, capacity, size, weight, text, width);
    if (audit_text && output && capacity && width > 0) {
        ++capture50b_layout.fit_calls;
        layout50b_check_fit("fit", text, output);
    }
    return result;
}

void __wrap_x4_text_ellipsis_px(uint32_t *p, int x, int y, int size, int weight,
    const char *text, int width, uint32_t color)
{
    if (audit_text && text && *text && width > 0) {
        /* font.c calls its own fit function locally, which --wrap cannot see. */
        char fitted[2048];
        (void)__real_x4_text_fit_utf8(fitted, sizeof(fitted), size, weight, text, width);
        ++capture50b_layout.fit_calls;
        layout50b_check_fit("ellipsis", text, fitted);
    }
    capture50_inherited_ellipsis(p, x, y, size, weight, text, width, color);
}

int __real_x4_ui2_text_bounded(uint32_t *, int, int, int, int,
    const char *, int, int, int, uint32_t);
int __wrap_x4_ui2_text_bounded(uint32_t *p, int x, int y, int size, int weight,
    const char *text, int width, int height, int lines, uint32_t color)
{
    const char *saved = capture50b_layout.bounded_cursor;
    if (audit_text && p && text && *text) {
        ++capture50b_layout.bounded_calls;
        capture50b_layout.bounded_cursor = text;
    }
    const int result = __real_x4_ui2_text_bounded(p, x, y, size, weight, text, width, height, lines, color);
    capture50b_layout.bounded_cursor = saved;
    return result;
}

static void layout50b_sidebar_text(int x, int y, int size, int weight, const char *text)
{
    if (x != 119) return;
    static const int row_y[] = {150, 218, 286, 354, 422, 490, 803, 865};
    for (unsigned row = 0; row < sizeof(row_y) / sizeof(row_y[0]); ++row) {
        if (y != row_y[row] + (48 - size) / 2) continue;
        if (!capture50b_layout.sidebar_rows) {
            capture50b_layout.sidebar_size = size;
            capture50b_layout.sidebar_weight = weight;
        }
        capture50b_layout.sidebar_rows |= 1u << row;
        if (size != capture50b_layout.sidebar_size || weight != capture50b_layout.sidebar_weight ||
            x4_text_width_px(size, weight, text) > 102)
            layout50b_bad("sidebar", text, text);
        break;
    }
}

static void capture50b_layout_text(int x, int y, int size, int weight, const char *text)
{
    if (!audit_text || !text || !*text) return;
    const char *source = capture50b_layout.bounded_cursor;
    size_t prefix = 0;
    if (layout50b_ellipsis(text, &prefix)) {
        if (source) layout50b_check_fit("bounded", layout50b_skip_space(source), text);
        else if (!prefix) layout50b_bad("bare-ellipsis", NULL, text);
    } else if (source) {
        source = layout50b_skip_space(source);
        const size_t used = strlen(text);
        if (!strncmp(source, text, used)) capture50b_layout.bounded_cursor = source + used;
    }
    layout50b_sidebar_text(x, y, size, weight, text);
}

static void capture50b_layout_report(void)
{
    fprintf(stderr, "T50B_LAYOUT fit=%u bounded=%u cuts=%u failures=%u sidebar_rows=%u size=%d weight=%d\n",
        capture50b_layout.fit_calls, capture50b_layout.bounded_calls, capture50b_layout.cuts,
        capture50b_layout.failures, capture50b_layout.sidebar_rows,
        capture50b_layout.sidebar_size, capture50b_layout.sidebar_weight);
}
