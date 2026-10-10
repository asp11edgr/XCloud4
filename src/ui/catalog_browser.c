/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_browser.h"
#include <stddef.h>
#include <string.h>

_Static_assert(X4_CATALOG_MAX <= UINT16_MAX + 1u, "catalog index must fit uint16_t");
static const char keyboard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static unsigned folded(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static bool query_length(const X4CatalogBrowser *b, size_t *length)
{
    for (size_t i = 0; i <= X4_CATALOG_QUERY_MAX; ++i) {
        unsigned c = (unsigned char)b->query[i];
        if (!c) { *length = i; return true; }
        if (i == X4_CATALOG_QUERY_MAX ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ')) return false;
    }
    return false;
}

static bool contains(const char *text, size_t capacity, const char *query, size_t length)
{
    if (!length) return true;
    for (size_t i = 0; i < capacity && text[i]; ++i) {
        size_t matched = 0;
        while (matched < length && matched < capacity - i && text[i + matched] &&
            folded((unsigned char)text[i + matched]) == folded((unsigned char)query[matched]))
            ++matched;
        if (matched == length) return true;
    }
    return false;
}

static bool matches(const X4CatalogBrowser *b, const X4CatalogTitle *title, size_t length)
{
    if (!title->id[0] || strnlen(title->id, sizeof(title->id)) == sizeof(title->id) ||
        strnlen(title->name, sizeof(title->name)) == sizeof(title->name)) return false;
    if (b->filter == X4_CATALOG_FILTER_CONFIRMED_ACCESS && !title->entitled) return false;
    if (b->filter == X4_CATALOG_FILTER_UNCONFIRMED_ACCESS && title->entitled) return false;
    return contains(title->name, sizeof(title->name), b->query, length) ||
        contains(title->id, sizeof(title->id), b->query, length);
}

static void remember_selected(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    b->selected_id[0] = 0;
    if (b->selected >= b->filtered_count || b->filtered_count > X4_CATALOG_MAX) return;
    unsigned index = b->indices[b->selected];
    if (index >= c->count || index >= X4_CATALOG_MAX) return;
    size_t length = strnlen(c->titles[index].id, sizeof(b->selected_id));
    if (length && length < sizeof(b->selected_id))
        memcpy(b->selected_id, c->titles[index].id, length + 1);
}

static void criteria_changed(X4CatalogBrowser *b)
{
    b->selected = 0;
    b->selected_id[0] = 0;
    b->criteria_dirty = true;
}

void x4_catalog_browser_init(X4CatalogBrowser *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->criteria_dirty = true;
}

void x4_catalog_browser_rebuild(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (!b) return;
    bool preserve = !b->criteria_dirty && b->selected_id[0];
    size_t length = 0;
    b->filtered_count = b->selected = 0;
    memset(b->group_counts, 0, sizeof(b->group_counts));
    if (!c || c->count > X4_CATALOG_MAX || c->state != X4_CATALOG_READY ||
        (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT || !query_length(b, &length)) {
        if (!c || c->state != X4_CATALOG_LOADING || !preserve || c->count > X4_CATALOG_MAX)
            b->selected_id[0] = 0;
        b->criteria_dirty = false;
        return;
    }
    b->group_counts[X4_CATALOG_FILTER_ALL] = c->count;
    bool found = false;
    for (unsigned index = 0; index < c->count; ++index) {
        const X4CatalogTitle *title = &c->titles[index];
        ++b->group_counts[title->entitled ? X4_CATALOG_FILTER_CONFIRMED_ACCESS :
            X4_CATALOG_FILTER_UNCONFIRMED_ACCESS];
        if (!matches(b, title, length)) continue;
        unsigned visible = b->filtered_count++;
        b->indices[visible] = (uint16_t)index;
        if (preserve && !strncmp(b->selected_id, title->id, sizeof(b->selected_id))) {
            b->selected = visible;
            found = true;
        }
    }
    if (!found) b->selected = 0;
    b->criteria_dirty = false;
    remember_selected(b, c);
}

bool x4_catalog_browser_selected_index(const X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, unsigned *source_index)
{
    size_t length;
    if (!b || !c || !source_index || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX ||
        b->criteria_dirty || !b->filtered_count || b->filtered_count > X4_CATALOG_MAX ||
        b->selected >= b->filtered_count || (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT ||
        !query_length(b, &length)) return false;
    unsigned index = b->indices[b->selected];
    if (index >= c->count || index >= X4_CATALOG_MAX || !matches(b, &c->titles[index], length) ||
        strncmp(b->selected_id, c->titles[index].id, sizeof(b->selected_id))) return false;
    *source_index = index;
    return true;
}

void x4_catalog_browser_move(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    int delta, bool page_clamp)
{
    unsigned original;
    if (!x4_catalog_browser_selected_index(b, c, &original)) return;
    int64_t next = (int64_t)b->selected + delta;
    if (page_clamp) {
        if (next < 0) next = 0;
        if (next >= b->filtered_count) next = b->filtered_count - 1;
    } else {
        next %= b->filtered_count;
        if (next < 0) next += b->filtered_count;
    }
    b->selected = (unsigned)next;
    remember_selected(b, c);
}

bool x4_catalog_browser_cycle_filter(X4CatalogBrowser *b, int delta)
{
    if (!b || (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT) return false;
    int64_t next = ((int64_t)b->filter + delta) % X4_CATALOG_FILTER_COUNT;
    if (next < 0) next += X4_CATALOG_FILTER_COUNT;
    if (next == b->filter) return false;
    b->filter = (enum X4CatalogFilter)next;
    criteria_changed(b);
    return true;
}

bool x4_catalog_browser_query_append(X4CatalogBrowser *b, char character)
{
    size_t length;
    unsigned c = folded((unsigned char)character);
    if (!b || !query_length(b, &length) || length >= X4_CATALOG_QUERY_MAX ||
        !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')) return false;
    b->query[length] = (char)c;
    b->query[length + 1] = 0;
    criteria_changed(b);
    return true;
}

bool x4_catalog_browser_query_delete(X4CatalogBrowser *b)
{
    size_t length;
    if (!b || !query_length(b, &length) || !length) return false;
    b->query[length - 1] = 0;
    criteria_changed(b);
    return true;
}

bool x4_catalog_browser_query_clear(X4CatalogBrowser *b)
{
    size_t length;
    if (!b || !query_length(b, &length) || !length) return false;
    b->query[0] = 0;
    criteria_changed(b);
    return true;
}

void x4_catalog_browser_keyboard_move(X4CatalogBrowser *b, int dx, int dy)
{
    if (!b) return;
    unsigned cursor = b->keyboard_cursor % X4_CATALOG_KEYBOARD_KEYS;
    int64_t x = ((int64_t)(cursor % 6) + dx) % 6;
    int64_t y = ((int64_t)(cursor / 6) + dy) % 6;
    if (x < 0) x += 6;
    if (y < 0) y += 6;
    b->keyboard_cursor = (unsigned)(y * 6 + x);
}

char x4_catalog_browser_keyboard_char(const X4CatalogBrowser *b)
{
    return b && b->keyboard_cursor < X4_CATALOG_KEYBOARD_KEYS ? keyboard[b->keyboard_cursor] : 0;
}
