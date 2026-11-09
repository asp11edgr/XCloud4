/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_browser.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(X4_CATALOG_MAX <= UINT16_MAX + 1u, "catalog index must fit uint16_t");
_Static_assert(sizeof(X4CatalogBrowser)<=48u*1024,"browser RAM budget is 48 KiB");
static const char keyboard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static unsigned folded(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static bool ascii_ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (folded((unsigned char)*a) != folded((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

/* Validated bounded UTF-8 presentation search. The existing on-screen query
 * is ASCII; Latin accents/case are folded only for matching, never rewritten
 * in the displayed name, provider ID, launch ID or credential. */
static bool next_codepoint(const char *text, size_t n, size_t *offset, unsigned *value)
{
    if (*offset >= n) return false;
    unsigned c=(unsigned char)text[(*offset)++];
    if (c<0x80) { *value=c; return true; }
    unsigned extra,minimum;
    if (c>=0xc2&&c<=0xdf) { extra=1; *value=c&31; minimum=0x80; }
    else if (c>=0xe0&&c<=0xef) { extra=2; *value=c&15; minimum=0x800; }
    else if (c>=0xf0&&c<=0xf4) { extra=3; *value=c&7; minimum=0x10000; }
    else return false;
    if (extra>n-*offset) return false;
    while (extra--) { c=(unsigned char)text[(*offset)++]; if ((c&0xc0)!=0x80) return false; *value=(*value<<6)|(c&63); }
    return *value>=minimum && *value<=0x10ffff && !(*value>=0xd800&&*value<=0xdfff);
}

static unsigned latin_fold(unsigned c)
{
    if (c<0x80) return folded((unsigned char)c);
    if ((c>=0xc0&&c<=0xc5)||(c>=0xe0&&c<=0xe5)||c==0x100||c==0x101) return 'A';
    if (c==0xc7||c==0xe7||c==0x106||c==0x107) return 'C';
    if ((c>=0xc8&&c<=0xcb)||(c>=0xe8&&c<=0xeb)||c==0x112||c==0x113) return 'E';
    if ((c>=0xcc&&c<=0xcf)||(c>=0xec&&c<=0xef)||c==0x130||c==0x131) return 'I';
    if (c==0xd1||c==0xf1) return 'N';
    if ((c>=0xd2&&c<=0xd6)||(c>=0xf2&&c<=0xf6)||c==0xd8||c==0xf8) return 'O';
    if ((c>=0xd9&&c<=0xdc)||(c>=0xf9&&c<=0xfc)) return 'U';
    if (c==0xdd||c==0xfd||c==0xff) return 'Y';
    if (c==0x160||c==0x161) return 'S';
    if (c==0x17d||c==0x17e) return 'Z';
    return c;
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
    size_t n=strnlen(text,capacity);
    if (n==capacity) return false;
    unsigned normalized[128];
    size_t offset=0,count=0;
    while (offset<n) {
        unsigned code;
        if (!next_codepoint(text,n,&offset,&code)) return false;
        /* Combining accents do not change the ASCII keyboard search. */
        if (code>=0x300&&code<=0x36f) continue;
        if (code==0xc6||code==0xe6||code==0x152||code==0x153||code==0xdf||code==0x1e9e) {
            if (count+2>sizeof(normalized)/sizeof(normalized[0])) return false;
            normalized[count++]=(code==0xc6||code==0xe6)?'A':(code==0x152||code==0x153)?'O':'S';
            normalized[count++]=(code==0xdf||code==0x1e9e)?'S':'E';
            continue;
        }
        if (count==sizeof(normalized)/sizeof(normalized[0])) return false;
        normalized[count++]=latin_fold(code);
    }
    for (size_t i = 0; i < count; ++i) {
        size_t matched = 0;
        while (matched < length && matched < count - i &&
            normalized[i + matched] == folded((unsigned char)query[matched]))
            ++matched;
        if (matched == length) return true;
    }
    return false;
}

static bool matches_query(const X4CatalogBrowser *b, const X4CatalogTitle *title, size_t length)
{
    if (!x4_recent_title_id_valid(title->id,sizeof(title->id)) ||
        strnlen(title->name, sizeof(title->name)) == sizeof(title->name)) return false;
    return !length || contains(title->name, sizeof(title->name), b->query, length) ||
        contains(title->id, sizeof(title->id), b->query, length);
}

static bool matches_access(const X4CatalogBrowser *b, const X4CatalogTitle *title)
{
    if (b->filter == X4_CATALOG_FILTER_CONFIRMED_ACCESS) return title->entitled;
    if (b->filter == X4_CATALOG_FILTER_UNCONFIRMED_ACCESS) return !title->entitled;
    if (b->filter == X4_CATALOG_FILTER_FAVORITES)
        return x4_favorites_contains(b->favorites, title->product_id);
    return true;
}

static X4Ui2CollectionKind category_collection(const char *category)
{
    if (ascii_ieq(category, "Multi-player Online Battle Arena") ||
        ascii_ieq(category, "Multiplayer") || ascii_ieq(category, "Multijugador"))
        return X4_UI2_COLLECTION_MULTIPLAYER;
    if (ascii_ieq(category, "Open world") || ascii_ieq(category, "Mundo abierto"))
        return X4_UI2_COLLECTION_OPEN_WORLD;
    if (ascii_ieq(category, "Sports") || ascii_ieq(category, "Deportes"))
        return X4_UI2_COLLECTION_SPORTS;
    if (ascii_ieq(category, "Racing & flying") || ascii_ieq(category, "Racing") ||
        ascii_ieq(category, "Carreras y vuelo") || ascii_ieq(category, "Carreras"))
        return X4_UI2_COLLECTION_RACING;
    if (ascii_ieq(category, "Shooter") || ascii_ieq(category, "Disparos"))
        return X4_UI2_COLLECTION_SHOOTER;
    if (ascii_ieq(category, "Indie")) return X4_UI2_COLLECTION_INDIE;
    return X4_UI2_COLLECTION_NONE;
}

static bool in_collection(const X4CatalogBrowser *b, unsigned index, X4Ui2CollectionKind kind)
{
    if ((unsigned)kind >= X4_UI2_COLLECTION_COUNT) return false;
    if (b->ui2_genre_masks[index] & b->ui2_collection_genres[kind]) return true;
    unsigned attributes = b->ui2_attributes[index];
    switch (kind) {
    case X4_UI2_COLLECTION_MULTIPLAYER:
        return (attributes & (X4_GAME_ATTR_ONLINE_MULTIPLAYER |
            X4_GAME_ATTR_LOCAL_MULTIPLAYER | X4_GAME_ATTR_COOP)) != 0;
    case X4_UI2_COLLECTION_OPEN_WORLD: return (attributes & X4_GAME_ATTR_OPEN_WORLD) != 0;
    case X4_UI2_COLLECTION_INDIE: return (attributes & X4_GAME_ATTR_INDIE) != 0;
    default: return false;
    }
}

static bool matches_genre(const X4CatalogBrowser *b, unsigned index)
{
    if (!b->ui2_enabled) return true;
    if (b->ui2_collection_filter != X4_UI2_COLLECTION_NONE)
        return in_collection(b, index, b->ui2_collection_filter);
    return !b->ui2_genre_slot || (b->ui2_genre_slot < X4_UI2_GENRE_MAX &&
        (b->ui2_genre_masks[index] & (UINT32_C(1) << b->ui2_genre_slot)));
}

static bool matches_filters(const X4CatalogBrowser *b, const X4CatalogTitle *title, unsigned index)
{
    if (b->ui2_enabled && b->active_tab == X4_CAT_TAB_SEARCH) return true;
    return matches_access(b, title) && matches_genre(b, index);
}

static void ui2_prepare_counts(X4CatalogBrowser *b, uint8_t chip_by_slot[X4_UI2_GENRE_MAX])
{
    if (b->ui2_genre_slot >= X4_UI2_GENRE_MAX ||
        !(b->ui2_genre_present & (UINT32_C(1) << b->ui2_genre_slot))) b->ui2_genre_slot = 0;
    memset(b->ui2_collection_genres, 0, sizeof(b->ui2_collection_genres));
    memset(b->ui2_access_counts, 0, sizeof(b->ui2_access_counts));
    b->ui2_multiplayer_available = false;
    b->ui2_multiplayer_count = 0;
    b->ui2_genre = 0;
    b->ui2_genre_count = 1;
    b->ui2_genres[0] = (X4Ui2GenreChip){0};
    for (unsigned slot = 1; slot < X4_UI2_GENRE_MAX; ++slot) {
        uint32_t bit = UINT32_C(1) << slot;
        if (!(b->ui2_genre_present & bit)) continue;
        X4Ui2CollectionKind kind = category_collection(b->ui2_genre_names[slot]);
        if (kind != X4_UI2_COLLECTION_NONE) b->ui2_collection_genres[kind] |= bit;
        unsigned at = b->ui2_genre_count++;
        chip_by_slot[slot] = (uint8_t)at;
        X4Ui2GenreChip *chip = &b->ui2_genres[at];
        *chip = (X4Ui2GenreChip){.slot = slot};
        memcpy(chip->label, b->ui2_genre_names[slot], sizeof(chip->label));
        if (slot == b->ui2_genre_slot) b->ui2_genre = at;
    }
}

static void ui2_count_title(X4CatalogBrowser *b, const X4CatalogTitle *title, unsigned index,
    const uint8_t chip_by_slot[X4_UI2_GENRE_MAX])
{
    const bool multiplayer = in_collection(b, index, X4_UI2_COLLECTION_MULTIPLAYER);
    if (multiplayer) b->ui2_multiplayer_available = true;
    if (matches_access(b, title)) {
        ++b->ui2_genres[0].count;
        for (uint32_t mask = b->ui2_genre_masks[index] & b->ui2_genre_present; mask; mask &= mask - 1) {
            const unsigned slot = (unsigned)__builtin_ctz(mask);
            ++b->ui2_genres[chip_by_slot[slot]].count;
        }
        if (b->ui2_collection_filter != X4_UI2_COLLECTION_NONE &&
            in_collection(b, index, b->ui2_collection_filter))
            ++b->ui2_collection_filter_count;
        if (multiplayer) ++b->ui2_multiplayer_count;
    }
    if (matches_genre(b, index)) {
        ++b->ui2_access_counts[X4_CATALOG_FILTER_ALL];
        ++b->ui2_access_counts[title->entitled ? X4_CATALOG_FILTER_CONFIRMED_ACCESS :
            X4_CATALOG_FILTER_UNCONFIRMED_ACCESS];
        if (x4_favorites_contains(b->favorites, title->product_id))
            ++b->ui2_access_counts[X4_CATALOG_FILTER_FAVORITES];
    }
}

static void ui2_publish_collections(X4CatalogBrowser *b, const unsigned counts[X4_UI2_COLLECTION_COUNT],
    const uint16_t sources[X4_UI2_COLLECTION_COUNT])
{
    X4Ui2CollectionKind selected = X4_UI2_COLLECTION_NONE;
    if (b->ui2_collection_focus && b->ui2_collection_selected < b->ui2_collection_count)
        selected = b->ui2_collections[b->ui2_collection_selected].kind;
    b->ui2_collection_count = b->ui2_collection_selected = 0;
    for (unsigned kind = 1; kind < X4_UI2_COLLECTION_COUNT; ++kind) {
        if (counts[kind] < 4) continue;
        unsigned at = b->ui2_collection_count++;
        b->ui2_collections[at] = (X4Ui2Collection){(X4Ui2CollectionKind)kind, counts[kind], sources[kind]};
        if (kind == selected) b->ui2_collection_selected = at;
    }
    if (!b->ui2_collection_count && b->ui2_collection_focus) {
        b->ui2_collection_focus = false;
        b->hero_focused = true;
    }
}

static void remember_selected(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    b->selected_id[0] = 0;
    b->detail_scroll_offset = 0;
    if (!c || c->state!=X4_CATALOG_READY || c->count>X4_CATALOG_MAX) return;
    if (b->selected >= b->filtered_count || b->filtered_count > X4_CATALOG_MAX) return;
    unsigned index = b->indices[b->selected];
    if (index >= c->count || index >= X4_CATALOG_MAX) return;
    size_t length = strnlen(c->titles[index].id, sizeof(b->selected_id));
    if (length && length < sizeof(b->selected_id))
        memcpy(b->selected_id, c->titles[index].id, length + 1);
}

static void criteria_changed(X4CatalogBrowser *b)
{
    if (!b->apple_mode || b->active_tab != X4_CAT_TAB_RECENTS)
        b->section = X4_CATALOG_SECTION_CATALOG;
    b->selected = 0;
    b->catalog_preferred_column = 0;
    b->selected_id[0] = 0;
    b->detail_scroll_offset = 0;
    b->criteria_dirty = true;
}

void x4_catalog_browser_init(X4CatalogBrowser *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->criteria_dirty = true;
    b->hero_focused = true;
    b->ui2_selector_preference = X4_UI2_CATALOG_GENRES;
}

static bool title_after(const X4CatalogSnapshot *c, uint16_t first, uint16_t second)
{
    const char *a = c->titles[first].name, *b = c->titles[second].name;
    size_t an = strnlen(a, sizeof(c->titles[first].name));
    size_t bn = strnlen(b, sizeof(c->titles[second].name));
    size_t i = 0, j = 0;
    while (i < an && j < bn) {
        unsigned ac, bc;
        const size_t next_a = i, next_b = j;
        if (!next_codepoint(a, an, &i, &ac)) {
            ac = 0x110000u + (unsigned char)a[next_a];
            i = next_a + 1;
        }
        if (!next_codepoint(b, bn, &j, &bc)) {
            bc = 0x110000u + (unsigned char)b[next_b];
            j = next_b + 1;
        }
        ac = latin_fold(ac);
        bc = latin_fold(bc);
        if (ac != bc) return ac > bc;
    }
    if (i < an || j < bn) return i < an;
    return first > second;
}

static void alphabetic_sift(uint16_t *indices, const X4CatalogSnapshot *c,
    unsigned root, unsigned count)
{
    while (root * 2 + 1 < count) {
        unsigned child = root * 2 + 1;
        if (child + 1 < count && title_after(c, indices[child + 1], indices[child])) ++child;
        if (!title_after(c, indices[child], indices[root])) break;
        uint16_t title = indices[root];
        indices[root] = indices[child];
        indices[child] = title;
        root = child;
    }
}

static void sort_catalog(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (b->ui2_sort == X4_UI2_SORT_NEW) {
        /* Rank the already loaded New row first, preserving catalog order
         * for all other titles. The published row has at most 30 entries. */
        unsigned front = 0;
        for (unsigned k = 0; k < b->ui2_new.count && front < b->filtered_count; ++k)
            for (unsigned i = front; i < b->filtered_count; ++i) {
                if (b->indices[i] != b->ui2_new.indices[k]) continue;
                uint16_t title = b->indices[i];
                memmove(&b->indices[front + 1], &b->indices[front],
                    (i - front) * sizeof(b->indices[0]));
                b->indices[front++] = title;
                break;
            }
        return;
    }
    const unsigned count = b->filtered_count;
    for (unsigned i = count / 2; i > 0; --i)
        alphabetic_sift(b->indices, c, i - 1, count);
    for (unsigned i = count; i > 1; --i) {
        uint16_t title = b->indices[i - 1];
        b->indices[i - 1] = b->indices[0];
        b->indices[0] = title;
        alphabetic_sift(b->indices, c, 0, i - 1);
    }
}

void x4_catalog_browser_rebuild(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (!b) return;
    b->library.dirty = true;
    bool preserve = !b->criteria_dirty && b->selected_id[0];
    unsigned collection_counts[X4_UI2_COLLECTION_COUNT] = {0};
    uint16_t collection_sources[X4_UI2_COLLECTION_COUNT] = {0};
    uint8_t chip_by_slot[X4_UI2_GENRE_MAX] = {0};
    if (b->ui2_enabled) ++b->ui2_revision;
    b->ui2_collection_filter_count = 0;
    size_t length = 0;
    unsigned previous = b->selected;
    b->filtered_count = b->selected = 0;
    memset(b->group_counts, 0, sizeof(b->group_counts));
    if (!c || c->count > X4_CATALOG_MAX || c->state != X4_CATALOG_READY ||
        (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT || !query_length(b, &length)) {
        if (!c || c->state != X4_CATALOG_LOADING || !preserve || c->count > X4_CATALOG_MAX)
            b->selected_id[0] = 0;
        if (b->ui2_enabled) {
            b->ui2_genre_count = 1;
            b->ui2_multiplayer_available = false;
            b->ui2_multiplayer_count = 0;
            b->ui2_genre = 0;
            b->ui2_genres[0] = (X4Ui2GenreChip){0};
            memset(b->ui2_access_counts, 0, sizeof(b->ui2_access_counts));
            ui2_publish_collections(b, collection_counts, collection_sources);
        }
        b->criteria_dirty = false;
        return;
    }
    if (b->ui2_enabled) ui2_prepare_counts(b, chip_by_slot);
    b->group_counts[X4_CATALOG_FILTER_ALL] = c->count;
    bool found = false;
    for (unsigned index = 0; index < c->count; ++index) {
        const X4CatalogTitle *title = &c->titles[index];
        ++b->group_counts[title->entitled ? X4_CATALOG_FILTER_CONFIRMED_ACCESS :
            X4_CATALOG_FILTER_UNCONFIRMED_ACCESS];
        bool query_match = matches_query(b, title, length);
        if (b->ui2_enabled) {
            if (title->product_id[0] &&
                x4_recent_title_id_valid(title->id, sizeof(title->id))) {
                for (unsigned kind = 1; kind < X4_UI2_COLLECTION_COUNT; ++kind) {
                    if (!in_collection(b, index, (X4Ui2CollectionKind)kind)) continue;
                    if (!collection_counts[kind]++) collection_sources[kind] = (uint16_t)index;
                }
            }
            if (query_match) ui2_count_title(b, title, index, chip_by_slot);
        }
        if (!query_match || !matches_filters(b, title, index)) continue;
        unsigned visible = b->filtered_count++;
        b->indices[visible] = (uint16_t)index;
        if (preserve && !strncmp(b->selected_id, title->id, sizeof(b->selected_id))) {
            b->selected = visible;
            found = true;
        }
    }
    if (b->ui2_enabled) {
        b->ui2_multiplayer_available |= collection_counts[X4_UI2_COLLECTION_MULTIPLAYER] != 0;
        ui2_publish_collections(b, collection_counts, collection_sources);
        if (b->active_tab == X4_CAT_TAB_CATALOG && b->ui2_sort != X4_UI2_SORT_FEATURED) {
            sort_catalog(b, c);
            if (found)
                for (unsigned i = 0; i < b->filtered_count; ++i)
                    if (!strcmp(c->titles[b->indices[i]].id, b->selected_id)) {
                        b->selected = i;
                        break;
                    }
        }
    }
    if (!found) b->selected = 0;
    if (!found || previous != b->selected)
        b->catalog_preferred_column = b->selected %
            (b->ui2_enabled ? X4_CATALOG_UI2_COLUMNS : X4_CATALOG_TAB_COLUMNS);
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
    if (index >= c->count || index >= X4_CATALOG_MAX ||
        !matches_query(b, &c->titles[index], length) || !matches_filters(b, &c->titles[index], index) ||
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

unsigned x4_catalog_browser_page_start(const X4CatalogBrowser *b)
{
    return b ? b->selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE : 0;
}

unsigned x4_catalog_browser_page_count(const X4CatalogBrowser *b)
{
    if (!b || b->filtered_count>X4_CATALOG_MAX) return 0;
    unsigned start=x4_catalog_browser_page_start(b);
    if (start>=b->filtered_count) return 0;
    unsigned count=b->filtered_count-start;
    return count<X4_CATALOG_PAGE_SIZE?count:X4_CATALOG_PAGE_SIZE;
}

bool x4_catalog_browser_grid_move(X4CatalogBrowser *b,const X4CatalogSnapshot *c,int dx,int dy)
{
    unsigned original;
    if (!x4_catalog_browser_selected_index(b,c,&original) || (dx && dy)) return false;
    unsigned old=b->selected,start=x4_catalog_browser_page_start(b),local=old-start;
    unsigned count=x4_catalog_browser_page_count(b),row=local/X4_CATALOG_COLUMNS,col=local%X4_CATALOG_COLUMNS;
    int next=(int)local;
    if (dx) {
        int next_col=(int)col+(dx<0?-1:1);
        if (next_col<0 || next_col>=X4_CATALOG_COLUMNS) return false;
        next=(int)(row*X4_CATALOG_COLUMNS)+next_col;
        if ((unsigned)next>=count) return false;
    } else if (dy) {
        int next_row=(int)row+(dy<0?-1:1);
        if (next_row<0 || next_row>=X4_CATALOG_ROWS || (unsigned)(next_row*X4_CATALOG_COLUMNS)>=count) return false;
        next=next_row*X4_CATALOG_COLUMNS+(int)col;
        if ((unsigned)next>=count) next=(int)count-1;
    }
    b->selected=start+(unsigned)next;
    remember_selected(b,c);
    return old!=b->selected;
}

static void remember_recent(X4CatalogBrowser *b,const X4RecentGames *h)
{
    const X4RecentGame *game=x4_recent_get(h,b->recent_selected);
    b->recent_selected_id[0]=0;
    b->detail_scroll_offset = 0;
    if (game) memcpy(b->recent_selected_id,game->id,sizeof(b->recent_selected_id));
}

void x4_catalog_browser_sync_recents(X4CatalogBrowser *b,const X4RecentGames *h)
{
    if (!b) return;
    unsigned count=x4_recent_count(h);
    uint64_t account=h?h->account_generation:0;
    bool changed=b->recent_account_generation!=account;
    unsigned previous=b->recent_selected;
    if (changed) {
        b->recent_account_generation=account;
        b->recent_initialized=false;
        b->recent_selected=0; b->recent_selected_id[0]=0;
        b->recent_preferred_column=0;
        b->section=count?X4_CATALOG_SECTION_RECENT:X4_CATALOG_SECTION_CATALOG;
    } else if (h&&h->state==X4_RECENT_LOADING) return;
    else if (b->recent_selected_id[0]) {
        bool found=false;
        for (unsigned i=0;i<count;++i) if (!strcmp(b->recent_selected_id,h->entries[i].id)) {
            b->recent_selected=i; found=true; break;
        }
        if (!found) b->recent_selected=0;
    }
    if (h&&h->state==X4_RECENT_READY&&!b->recent_initialized) {
        b->section=count?X4_CATALOG_SECTION_RECENT:X4_CATALOG_SECTION_CATALOG;
        b->recent_initialized=true;
    }
    if (b->recent_selected>=count) b->recent_selected=0;
    if (previous!=b->recent_selected)
        b->recent_preferred_column = b->recent_selected %
            (b->ui2_enabled ? X4_CATALOG_UI2_COLUMNS : X4_CATALOG_TAB_COLUMNS);
    if (b->apple_mode && b->active_tab == X4_CAT_TAB_RECENTS)
        b->section = X4_CATALOG_SECTION_RECENT;
    else if (b->apple_mode && b->active_tab == X4_CAT_TAB_CATALOG)
        b->section = X4_CATALOG_SECTION_CATALOG;
    const X4RecentGame *game=x4_recent_get(h,b->recent_selected);
    if (changed || previous!=b->recent_selected ||
        (game ? strcmp(game->id,b->recent_selected_id)!=0 : b->recent_selected_id[0]!=0))
        remember_recent(b,h);
}

bool x4_catalog_browser_nav(X4CatalogBrowser *b,const X4CatalogSnapshot *c,const X4RecentGames *h,int dx,int dy)
{
    if (!b || (dx&&dy)) return false;
    x4_catalog_browser_sync_recents(b,h);
    if (b->section==X4_CATALOG_SECTION_RECENT) {
        if (dy>0) {
            unsigned col=(b->recent_selected%X4_CATALOG_PAGE_SIZE)/2;
            b->section=X4_CATALOG_SECTION_CATALOG;
            if (b->filtered_count) {
                unsigned start=x4_catalog_browser_page_start(b),count=x4_catalog_browser_page_count(b);
                if (count) b->selected=start+(col<count?col:count-1);
                remember_selected(b,c);
            }
            return true;
        }
        unsigned count=x4_recent_count(h);
        if (!dx || !count) return false;
        unsigned start=b->recent_selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
        int next=(int)b->recent_selected+(dx<0?-1:1);
        if (next<(int)start || next>=(int)(start+X4_CATALOG_PAGE_SIZE) || next>=(int)count) return false;
        b->recent_selected=(unsigned)next; remember_recent(b,h); return true;
    }
    if (dy<0 && (!b->filtered_count || b->selected%X4_CATALOG_PAGE_SIZE<X4_CATALOG_COLUMNS)) {
        b->section=X4_CATALOG_SECTION_RECENT;
        unsigned count=x4_recent_count(h),col=b->selected%X4_CATALOG_COLUMNS;
        unsigned start=b->recent_selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
        if (count) {
            unsigned end=start+X4_CATALOG_PAGE_SIZE;
            if (end>count) end=count;
            unsigned local=col*2;
            b->recent_selected=start+local<end?start+local:end-1;
        }
        remember_recent(b,h); return true;
    }
    return x4_catalog_browser_grid_move(b,c,dx,dy);
}

bool x4_catalog_browser_page_move(X4CatalogBrowser *b,const X4CatalogSnapshot *c,const X4RecentGames *h,int direction)
{
    if (!b || !direction) return false;
    if (b->section==X4_CATALOG_SECTION_RECENT) {
        unsigned count=x4_recent_count(h);
        if (!count) return false;
        unsigned old=b->recent_selected,start=old/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
        int next=(int)start+(direction<0?-X4_CATALOG_PAGE_SIZE:X4_CATALOG_PAGE_SIZE);
        if (next<0 || next>=(int)count) return false;
        unsigned offset=old%X4_CATALOG_PAGE_SIZE;
        b->recent_selected=(unsigned)next+offset;
        if (b->recent_selected>=count) b->recent_selected=count-1;
        remember_recent(b,h); return old!=b->recent_selected;
    }
    unsigned source;
    if (!x4_catalog_browser_selected_index(b,c,&source)) return false;
    unsigned old=b->selected,start=x4_catalog_browser_page_start(b);
    int next=(int)start+(direction<0?-X4_CATALOG_PAGE_SIZE:X4_CATALOG_PAGE_SIZE);
    if (next<0 || next>=(int)b->filtered_count) return false;
    b->selected=(unsigned)next+old%X4_CATALOG_PAGE_SIZE;
    if (b->selected>=b->filtered_count) b->selected=b->filtered_count-1;
    remember_selected(b,c); return old!=b->selected;
}

static bool add_unique_featured(X4CatalogBrowser *b, unsigned src)
{
    if (b->featured_count >= X4_CATALOG_FEATURED_MAX) return false;
    for (unsigned i = 0; i < b->featured_count; ++i) {
        if (b->featured_sources[i] == (uint16_t)src) return false;
    }
    b->featured_sources[b->featured_count++] = (uint16_t)src;
    return true;
}

void x4_catalog_browser_rebuild_featured(X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, const X4RecentGames *h)
{
    if (!b) return;
    b->apple_mode = true;
    b->featured_count = 0;
    if (!c || c->state != X4_CATALOG_READY || c->count == 0 || c->count > X4_CATALOG_MAX) {
        b->featured_index = 0;
        b->ui2_featured_id[0] = 0;
        b->ui2_carousel_since = 0;
        return;
    }

    /* 1. Recent games (MRU) that resolve in the current playable catalog */
    unsigned rcount = x4_recent_count(h);
    for (unsigned i = 0; i < rcount && b->featured_count < X4_CATALOG_FEATURED_MAX; ++i) {
        unsigned src = 0;
        if (x4_recent_catalog_index(x4_recent_get(h, i), c, &src) && src < c->count) {
            add_unique_featured(b, src);
        }
    }

    /* 2. Fortnite (productId BT5P2X999VH2 or titleId FORTNITE) if present in catalog */
    for (unsigned i = 0; i < c->count && b->featured_count < X4_CATALOG_FEATURED_MAX; ++i) {
        const X4CatalogTitle *t = &c->titles[i];
        if (!x4_recent_title_id_valid(t->id, sizeof(t->id))) continue;
        if (ascii_ieq(t->product_id, "BT5P2X999VH2") || ascii_ieq(t->id, "FORTNITE")) {
            add_unique_featured(b, i);
            break;
        }
    }

    /* 3. First titles with confirmed access (entitled) */
    for (unsigned i = 0; i < c->count && b->featured_count < X4_CATALOG_FEATURED_MAX; ++i) {
        const X4CatalogTitle *t = &c->titles[i];
        if (!x4_recent_title_id_valid(t->id, sizeof(t->id))) continue;
        if (t->entitled) {
            add_unique_featured(b, i);
        }
    }

    /* 4. Fallback to first valid catalog titles if fewer than 4 were found */
    for (unsigned i = 0; i < c->count && b->featured_count < X4_CATALOG_FEATURED_MAX; ++i) {
        const X4CatalogTitle *t = &c->titles[i];
        if (!x4_recent_title_id_valid(t->id, sizeof(t->id))) continue;
        add_unique_featured(b, i);
    }

    if (b->featured_index >= b->featured_count) b->featured_index = 0;
    if (b->ui2_enabled) {
        for (unsigned i = 0; i < b->featured_count; ++i) {
            if (!strcmp(b->ui2_featured_id, c->titles[b->featured_sources[i]].product_id)) {
                b->featured_index = i;
                break;
            }
        }
        const X4CatalogTitle *hero = x4_catalog_browser_ui2_hero(b, c);
        if (hero) memcpy(b->ui2_featured_id, hero->product_id, sizeof(b->ui2_featured_id));
    }
}

const X4CatalogTitle *x4_catalog_browser_ui2_hero(const X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (!b || !c || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX ||
        b->featured_count > X4_CATALOG_FEATURED_MAX || b->featured_index >= b->featured_count)
        return NULL;
    unsigned source = b->featured_sources[b->featured_index];
    if (source >= c->count || !x4_recent_title_id_valid(c->titles[source].id, sizeof(c->titles[source].id)))
        return NULL;
    return &c->titles[source];
}

static bool hero_can_favorite(const X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    const X4CatalogTitle *hero = x4_catalog_browser_ui2_hero(b, c);
    return hero && hero->product_id[0];
}

static bool ui2_move_hero(X4CatalogBrowser *b, const X4CatalogSnapshot *c, int dx)
{
    if (b->featured_count < 2 || b->featured_count > X4_CATALOG_FEATURED_MAX) return false;
    unsigned n = b->featured_count;
    b->featured_index = (b->featured_index + (dx > 0 ? 1u : n - 1u)) % n;
    const X4CatalogTitle *hero = x4_catalog_browser_ui2_hero(b, c);
    if (hero) memcpy(b->ui2_featured_id, hero->product_id, sizeof(b->ui2_featured_id));
    b->ui2_carousel_since = 0;
    return true;
}

bool x4_catalog_browser_ui2_carousel_tick(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    uint64_t now, bool interacting)
{
    if (!b || !b->ui2_enabled || b->active_tab != X4_CAT_TAB_DISCOVER) return false;
    if (interacting || !b->ui2_carousel_since || now < b->ui2_carousel_since) {
        b->ui2_carousel_since = now;
        return false;
    }
    if (!x4_catalog_browser_ui2_hero(b, c) || now - b->ui2_carousel_since < UINT64_C(10000000) ||
        !ui2_move_hero(b, c, 1)) return false;
    b->ui2_carousel_since = now;
    ++b->ui2_revision;
    return true;
}

unsigned x4_catalog_browser_discover_row_count(const X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, const X4RecentGames *h)
{
    (void)b;
    unsigned rcount = x4_recent_count(h);
    if (rcount > 0) return rcount;
    if (!c || c->state != X4_CATALOG_READY || c->count == 0 || c->count > X4_CATALOG_MAX) return 0;
    unsigned pcount = 0;
    for (unsigned i = 0; i < c->count && pcount < X4_CATALOG_POPULAR_FALLBACK_MAX; ++i) {
        if (c->titles[i].entitled && x4_recent_title_id_valid(c->titles[i].id, sizeof(c->titles[i].id)))
            ++pcount;
    }
    if (pcount == 0) {
        for (unsigned i = 0; i < c->count && pcount < X4_CATALOG_POPULAR_FALLBACK_MAX; ++i) {
            if (x4_recent_title_id_valid(c->titles[i].id, sizeof(c->titles[i].id)))
                ++pcount;
        }
    }
    return pcount;
}

bool x4_catalog_browser_discover_row_source(const X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, const X4RecentGames *h,
    unsigned row_idx, unsigned *source_index)
{
    (void)b;
    if (!c || !source_index || c->state != X4_CATALOG_READY || c->count == 0 || c->count > X4_CATALOG_MAX)
        return false;
    unsigned rcount = x4_recent_count(h);
    if (rcount > 0) {
        if (row_idx >= rcount) return false;
        return x4_recent_catalog_index(x4_recent_get(h, row_idx), c, source_index);
    }
    unsigned pcount = 0;
    for (unsigned i = 0; i < c->count && pcount < X4_CATALOG_POPULAR_FALLBACK_MAX; ++i) {
        if (c->titles[i].entitled && x4_recent_title_id_valid(c->titles[i].id, sizeof(c->titles[i].id))) {
            if (pcount == row_idx) {
                *source_index = i;
                return true;
            }
            ++pcount;
        }
    }
    if (pcount == 0) {
        for (unsigned i = 0; i < c->count && pcount < X4_CATALOG_POPULAR_FALLBACK_MAX; ++i) {
            if (x4_recent_title_id_valid(c->titles[i].id, sizeof(c->titles[i].id))) {
                if (pcount == row_idx) {
                    *source_index = i;
                    return true;
                }
                ++pcount;
            }
        }
    }
    return false;
}

const X4CatalogTitle *x4_catalog_browser_home_product(const X4CatalogSnapshot *c,
    const char *id)
{
    if (!c || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX ||
        !id || !id[0]) return NULL;
    for (unsigned i = 0; i < c->count; ++i) {
        if (!strcmp(c->titles[i].product_id, id)) return &c->titles[i];
    }
    return NULL;
}

static void home_select(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (!b->home_rows.count) {
        b->home_row = b->home_first_row = b->discover_row_selected = 0;
        b->ui2_home_row_id[0] = 0;
        b->hero_focused = !b->ui2_collection_focus;
        return;
    }
    if (b->home_row >= b->home_rows.count) b->home_row = b->home_rows.count - 1;
    const X4HomeRow *row = &b->home_rows.rows[b->home_row];
    unsigned *column = &b->home_columns[row->kind];
    if (*column >= row->count) *column = row->count - 1;
    b->discover_row_selected = *column;
    if (b->ui2_enabled) {
        b->ui2_home_row_id[0] = 0;
        if (c && c->state == X4_CATALOG_READY && c->count <= X4_CATALOG_MAX &&
            *column < row->count && row->indices[*column] < c->count)
            memcpy(b->ui2_home_row_id, c->titles[row->indices[*column]].product_id,
                sizeof(b->ui2_home_row_id));
    }
    if (b->home_first_row > b->home_row) b->home_first_row = b->home_row;
    if (b->home_row > b->home_first_row + 1) b->home_first_row = b->home_row - 1;
    if (b->home_rows.count > 1 && b->home_first_row == b->home_rows.count - 1 &&
        !b->ui2_collection_focus) --b->home_first_row;
}

void x4_catalog_browser_home_apply(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    const X4HomeRows *rows)
{
    if (!b || !rows || rows->count > X4_HOME_ROW_COUNT) return;
    X4HomeRows discover = {0};
    if (b->ui2_enabled) {
        static const X4HomeRowKind order[] = {X4_HOME_CONTINUE, X4_HOME_POPULAR, X4_HOME_MINE};
        X4HomeRow news = {0};
        for (unsigned i = 0; i < rows->count; ++i)
            if (rows->rows[i].kind == X4_HOME_NEW && rows->rows[i].count <= X4_HOME_ROW_MAX)
                news = rows->rows[i];
        if (memcmp(&b->ui2_new, &news, sizeof(news))) {
            b->ui2_new = news;
            if (!news.count && b->ui2_sort == X4_UI2_SORT_NEW) b->ui2_sort = X4_UI2_SORT_FEATURED;
            if (b->active_tab == X4_CAT_TAB_CATALOG) x4_catalog_browser_rebuild(b, c);
        }
        for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); ++k)
            for (unsigned i = 0; i < rows->count; ++i)
                if (rows->rows[i].kind == order[k] && rows->rows[i].count &&
                    rows->rows[i].count <= X4_HOME_ROW_MAX) {
                    discover.rows[discover.count++] = rows->rows[i];
                    break;
                }
        rows = &discover;
    }
    unsigned previous_kind = X4_HOME_ROW_COUNT;
    if (b->home_row < b->home_rows.count) previous_kind = b->home_rows.rows[b->home_row].kind;
    b->home_configured = true;
    if (memcmp(rows, &b->home_rows, sizeof(*rows))) {
        b->home_rows = *rows;
        ++b->home_revision;
        for (unsigned i = 0; i < rows->count; ++i) {
            const X4HomeRow *row = &rows->rows[i];
            if (row->kind != previous_kind) continue;
            b->home_row = i;
            const char *selected = b->ui2_enabled ? b->ui2_home_row_id : b->home_candidate_id;
            for (unsigned j = 0; c && selected[0] && j < row->count; ++j) {
                if (row->indices[j] < c->count && !strcmp(c->titles[row->indices[j]].product_id, selected)) {
                    b->home_columns[row->kind] = j;
                    break;
                }
            }
            break;
        }
    }
    home_select(b, c);
}

static bool home_source(const X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    unsigned *source)
{
    if (b->home_row < b->home_rows.count) {
        const X4HomeRow *row = &b->home_rows.rows[b->home_row];
        if (b->discover_row_selected >= row->count) return false;
        *source = row->indices[b->discover_row_selected];
    } else {
        *source = 0;
    }
    return *source < c->count &&
        x4_recent_title_id_valid(c->titles[*source].id, sizeof(c->titles[*source].id));
}

bool x4_catalog_browser_home_tick(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    uint64_t now)
{
    if (!b || !b->home_configured || b->active_tab != X4_CAT_TAB_DISCOVER) return false;
    unsigned source;
    const X4CatalogTitle *hero = b->ui2_enabled ? x4_catalog_browser_ui2_hero(b, c) : NULL;
    const char *id = hero ? hero->product_id : c && c->state == X4_CATALOG_READY && c->count <= X4_CATALOG_MAX &&
        home_source(b, c, &source) ? c->titles[source].product_id : "";
    bool cleared = b->home_background_id[0] && !x4_catalog_browser_home_product(c, b->home_background_id);
    if (cleared) b->home_background_id[0] = 0;
    if (!b->ui2_enabled && b->ui2_collection_focus && b->home_background_id[0]) return false;
    if (strcmp(id, b->home_candidate_id)) {
        memcpy(b->home_candidate_id, id, strlen(id) + 1);
        b->home_focus_at = now;
    }
    if (!id[0] || !strcmp(id, b->home_background_id)) return cleared;
    if (!b->home_background_id[0] || (now >= b->home_focus_at && now - b->home_focus_at >= 300000)) {
        memcpy(b->home_background_id, id, strlen(id) + 1);
        return true;
    }
    return cleared;
}

const char *x4_catalog_browser_tab_background(X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, const X4RecentGames *h, uint64_t now)
{
    if (!b) return NULL;
    const char *id = "";
    if (b->active_tab == X4_CAT_TAB_CATALOG) {
        unsigned source;
        if (x4_catalog_browser_selected_index(b, c, &source)) id = c->titles[source].product_id;
    } else if (b->active_tab == X4_CAT_TAB_RECENTS &&
        h && h->account_generation && h->account_generation == b->recent_account_generation) {
        const X4RecentGame *game = x4_recent_get(h, b->recent_selected);
        if (game && !strcmp(game->id, b->recent_selected_id)) {
            unsigned source;
            if (x4_recent_catalog_index(game, c, &source) && c->titles[source].product_id[0])
                id = c->titles[source].product_id;
            else
                id = game->product_id;
        }
    }
    size_t length = strnlen(id, sizeof(b->tab_candidate_id));
    if (length == sizeof(b->tab_candidate_id)) { id = ""; length = 0; }
    if (strcmp(id, b->tab_candidate_id)) {
        memcpy(b->tab_candidate_id, id, length + 1);
        b->tab_focus_at = now;
        b->tab_background_id[0] = 0;
    }
    if (!length || now < b->tab_focus_at || now - b->tab_focus_at < 300000) {
        b->tab_background_id[0] = 0;
        return NULL;
    }
    memcpy(b->tab_background_id, id, length + 1);
    return b->tab_background_id;
}

unsigned x4_catalog_browser_home_row_start(const X4CatalogBrowser *b, unsigned index)
{
    if (!b || index >= b->home_rows.count) return 0;
    const X4HomeRow *row = &b->home_rows.rows[index];
    unsigned column = b->home_columns[row->kind];
    const unsigned last_visible = (b->ui2_enabled ? X4_CATALOG_UI2_DISCOVER_VISIBLE :
        X4_CATALOG_DISCOVER_ROW_VISIBLE) - 1u;
    return column > last_visible ? column - last_visible : 0;
}

static int query_slot(enum X4CatalogTab tab)
{
    if (tab == X4_CAT_TAB_CATALOG) return 0;
    if (tab == X4_CAT_TAB_SEARCH) return 1;
    return -1;
}

void x4_catalog_browser_ui2_select(X4CatalogBrowser *b, enum X4CatalogTab tab)
{
    if (!b || (unsigned)tab >= X4_CAT_TAB_COUNT) return;
    bool first = !b->ui2_enabled;
    b->ui2_enabled = b->apple_mode = true;
    b->top_bar_focused = false;
    if (!first && b->active_tab == tab) return;
    ++b->ui2_revision;
    int previous = query_slot(b->active_tab);
    if (previous >= 0) memcpy(b->ui2_queries[previous], b->query, sizeof(b->query));
    b->active_tab = tab;
    b->section = tab == X4_CAT_TAB_RECENTS ? X4_CATALOG_SECTION_RECENT : X4_CATALOG_SECTION_CATALOG;
    b->detail_scroll_offset = 0;
    b->hero_focused = tab == X4_CAT_TAB_DISCOVER;
    b->ui2_collection_focus = false;
    b->ui2_hero_favorite = b->ui2_library_tabs = b->ui2_selector_open = false;
    b->ui2_carousel_since = 0;
    b->editing = false;
    b->ui2_content_focus = tab == X4_CAT_TAB_SEARCH ? X4_UI2_CATALOG_SEARCH : X4_UI2_CATALOG_GRID;
    int next = query_slot(tab);
    if (next >= 0) {
        memcpy(b->query, b->ui2_queries[next], sizeof(b->query));
        criteria_changed(b);
    }
}

unsigned x4_catalog_browser_ui2_selector_count(const X4CatalogBrowser *b, X4Ui2CatalogFocus selector)
{
    if (!b) return 0;
    if (selector == X4_UI2_CATALOG_ACCESS) return X4_CATALOG_FILTER_COUNT;
    if (selector == X4_UI2_CATALOG_SORT) return b->ui2_new.count ? 3u : 2u;
    if (selector != X4_UI2_CATALOG_GENRES) return 0;
    unsigned genres = b->ui2_genre_count;
    if (!genres || genres > X4_UI2_GENRE_MAX) genres = 1;
    return genres + (b->ui2_collection_filter != X4_UI2_COLLECTION_NONE) +
        (b->ui2_multiplayer_available && b->ui2_collection_filter != X4_UI2_COLLECTION_MULTIPLAYER);
}

X4Ui2CollectionKind x4_catalog_browser_ui2_genre_option(const X4CatalogBrowser *b,
    unsigned value, unsigned *genre_index)
{
    if (value && b->ui2_collection_filter != X4_UI2_COLLECTION_NONE) {
        if (value == 1) return b->ui2_collection_filter;
        --value;
    }
    if (value && b->ui2_multiplayer_available &&
        b->ui2_collection_filter != X4_UI2_COLLECTION_MULTIPLAYER) {
        if (value == 1) return X4_UI2_COLLECTION_MULTIPLAYER;
        --value;
    }
    *genre_index = value;
    return X4_UI2_COLLECTION_NONE;
}

unsigned x4_catalog_browser_ui2_selector_value(const X4CatalogBrowser *b, X4Ui2CatalogFocus selector)
{
    if (!b) return 0;
    if (selector == X4_UI2_CATALOG_ACCESS) return b->filter;
    if (selector == X4_UI2_CATALOG_SORT) return b->ui2_sort;
    if (selector == X4_UI2_CATALOG_GENRES)
        return b->ui2_collection_filter != X4_UI2_COLLECTION_NONE ? 1u :
            b->ui2_genre + (b->ui2_genre && b->ui2_multiplayer_available);
    return 0;
}

static bool apply_selector(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    X4Ui2CatalogFocus selector, unsigned value)
{
    if (value >= x4_catalog_browser_ui2_selector_count(b, selector) ||
        value == x4_catalog_browser_ui2_selector_value(b, selector)) return false;
    switch (selector) {
    case X4_UI2_CATALOG_GENRES: {
        unsigned genre = 0;
        b->ui2_collection_filter = x4_catalog_browser_ui2_genre_option(b, value, &genre);
        b->ui2_genre = genre;
        b->ui2_genre_slot = b->ui2_genres[genre].slot;
        break;
    }
    case X4_UI2_CATALOG_ACCESS: b->filter = (enum X4CatalogFilter)value; break;
    case X4_UI2_CATALOG_SORT: b->ui2_sort = (X4Ui2CatalogSort)value; break;
    default: return false;
    }
    criteria_changed(b);
    x4_catalog_browser_rebuild(b, c);
    return true;
}

bool x4_catalog_browser_ui2_close_selector(X4CatalogBrowser *b)
{
    if (!b) return false;
    if (b->active_tab == X4_CAT_TAB_RECENTS && (b->library.popup || b->library.manage)) {
        b->library.popup = b->library.manage = false;
        ++b->ui2_revision;
        return true;
    }
    if (!b->ui2_selector_open) return false;
    b->ui2_selector_open = false;
    ++b->ui2_revision;
    return true;
}

bool x4_catalog_browser_ui2_cycle_chip(X4CatalogBrowser *b, const X4CatalogSnapshot *c, int direction)
{
    if (!b || !direction || b->active_tab != X4_CAT_TAB_CATALOG || b->editing ||
        b->ui2_selector_open) return false;
    X4Ui2CatalogFocus selector = b->ui2_selector_preference;
    const unsigned count = x4_catalog_browser_ui2_selector_count(b, selector);
    if (!count) return false;
    const unsigned current = x4_catalog_browser_ui2_selector_value(b, selector);
    const unsigned next = (current + (direction > 0 ? 1u : count - 1u)) % count;
    return apply_selector(b, c, selector, next);
}

enum { LIBRARY_BUCKETS = 1024 };
typedef struct {
    uint16_t source, recent, favorite, next;
    const X4HistoryEntry *history;
} LibraryRecord;

struct X4Ui2LibraryStorage {
    uint16_t buckets[LIBRARY_BUCKETS];
    uint16_t continuing[X4_UI2_LIBRARY_SHELF];
    uint16_t grid[X4_UI2_LIBRARY_MAX];
    LibraryRecord records[X4_UI2_LIBRARY_MAX];
};
_Static_assert(X4_UI2_LIBRARY_MAX < UINT16_MAX, "library indices fit in 16 bits");

static X4Ui2LibraryCard library_card(const X4CatalogBrowser *b, const LibraryRecord *record)
{
    const X4Ui2Library *l = &b->library;
    const X4CatalogTitle *title = record->source < l->catalog_count ?
        &l->catalog->titles[record->source] : NULL;
    const X4RecentGame *recent = record->recent < X4_RECENT_MAX ?
        x4_recent_get(l->recents, record->recent) : NULL;
    const X4Favorite *favorite = b->favorites && record->favorite < b->favorites->count ?
        &b->favorites->items[record->favorite] : NULL;
    return (X4Ui2LibraryCard){
        .product_id = title ? title->product_id : recent && recent->product_id[0] ?
            recent->product_id : favorite ? favorite->product_id : "",
        .title = title ? title->name : recent && recent->name[0] ? recent->name :
            favorite ? favorite->name : recent ? recent->id : "",
        .title_id = title ? title->id : recent ? recent->id : "",
        .source = title ? record->source : X4_CATALOG_MAX,
        .favorite = favorite != NULL,
        .history = record->history,
    };
}

X4Ui2LibraryCard x4_catalog_browser_ui2_library_card(const X4CatalogBrowser *b,
    bool continuing, unsigned ordinal)
{
    const X4Ui2Library *l = &b->library;
    if (l->storage && ordinal < (continuing ?
        (l->recent_count < X4_UI2_LIBRARY_SHELF ? l->recent_count : X4_UI2_LIBRARY_SHELF) :
        l->grid_count)) {
        unsigned index = continuing ? l->storage->continuing[ordinal] : l->storage->grid[ordinal];
        return library_card(b, &l->storage->records[index]);
    }
    return (X4Ui2LibraryCard){.source = X4_CATALOG_MAX};
}

X4Ui2LibraryCard x4_catalog_browser_ui2_library_selection(const X4CatalogBrowser *b)
{
    if (b && b->active_tab == X4_CAT_TAB_RECENTS) {
        const X4Ui2Library *l = &b->library;
        if (l->focus == X4_UI2_LIBRARY_GRID)
            return x4_catalog_browser_ui2_library_card(b, false, l->grid_selected);
        if (l->focus == X4_UI2_LIBRARY_CONTINUE)
            return x4_catalog_browser_ui2_library_card(b, true, l->continue_selected);
    }
    return (X4Ui2LibraryCard){.source = X4_CATALOG_MAX};
}

static const char *library_key(X4Ui2LibraryCard card)
{
    return card.product_id[0] ? card.product_id : card.title_id;
}

static unsigned library_bucket(const char *id)
{
    uint32_t hash = UINT32_C(2166136261);
    for (; *id; ++id) hash = (hash ^ folded((unsigned char)*id)) * UINT32_C(16777619);
    return hash & (LIBRARY_BUCKETS - 1u);
}

static unsigned library_add(X4CatalogBrowser *b, LibraryRecord item)
{
    X4Ui2Library *l = &b->library;
    X4Ui2LibraryStorage *s = l->storage;
    X4Ui2LibraryCard card = library_card(b, &item);
    const char *key = library_key(card);
    if (!*key) return X4_UI2_LIBRARY_MAX;
    unsigned bucket = library_bucket(key);
    for (unsigned at = s->buckets[bucket]; at; at = s->records[at - 1].next) {
        LibraryRecord *existing = &s->records[at - 1];
        if (!ascii_ieq(key, library_key(library_card(b, existing)))) continue;
        if (existing->source >= X4_CATALOG_MAX) existing->source = item.source;
        if (existing->recent >= X4_RECENT_MAX) existing->recent = item.recent;
        if (existing->favorite >= X4_FAVORITES_MAX) existing->favorite = item.favorite;
        return at - 1u;
    }
    if (l->members == X4_UI2_LIBRARY_MAX) return X4_UI2_LIBRARY_MAX;
    unsigned at = l->members++;
    item.next = s->buckets[bucket];
    s->records[at] = item;
    s->buckets[bucket] = (uint16_t)(at + 1u);
    return at;
}

static int library_name_compare(const char *a, const char *b)
{
    size_t ai = 0, bi = 0, an = strlen(a), bn = strlen(b);
    while (ai < an && bi < bn) {
        unsigned ac, bc;
        if (!next_codepoint(a, an, &ai, &ac) || !next_codepoint(b, bn, &bi, &bc)) break;
        ac = latin_fold(ac);
        bc = latin_fold(bc);
        if (ac != bc) return ac < bc ? -1 : 1;
    }
    return (ai < an) - (bi < bn);
}

static bool library_after(const X4CatalogBrowser *b, unsigned ai, unsigned bi)
{
    const LibraryRecord *a = &b->library.storage->records[ai], *z = &b->library.storage->records[bi];
    switch (b->library.sort) {
    case X4_UI2_LIBRARY_ALPHABETIC: {
        int compare = library_name_compare(library_card(b, a).title, library_card(b, z).title);
        if (compare) return compare > 0;
        break;
    }
    case X4_UI2_LIBRARY_PLAYED: {
        uint64_t aa = a->history ? a->history->played_s : 0, zz = z->history ? z->history->played_s : 0;
        if (aa != zz) return aa < zz;
        break;
    }
    case X4_UI2_LIBRARY_FAVORITE_FIRST:
        if ((a->favorite < X4_FAVORITES_MAX) != (z->favorite < X4_FAVORITES_MAX))
            return a->favorite >= X4_FAVORITES_MAX;
        break;
    default:
        if (a->recent != z->recent) return a->recent > z->recent;
        if (a->recent >= X4_RECENT_MAX) {
            uint64_t aa = a->history ? a->history->last_session_s : 0;
            uint64_t zz = z->history ? z->history->last_session_s : 0;
            if (aa != zz) return aa < zz;
        }
        break;
    }
    return ai > bi;
}

static void library_sift(const X4CatalogBrowser *b, unsigned root, unsigned count)
{
    uint16_t *grid = b->library.storage->grid;
    for (;;) {
        unsigned child = root * 2u + 1u;
        if (child >= count) return;
        if (child + 1u < count && library_after(b, grid[child + 1u], grid[child])) ++child;
        if (!library_after(b, grid[child], grid[root])) return;
        uint16_t saved = grid[root];
        grid[root] = grid[child];
        grid[child] = saved;
        root = child;
    }
}

static void library_filter(X4CatalogBrowser *b)
{
    X4Ui2Library *l = &b->library;
    uint16_t *grid = l->storage->grid;
    l->grid_count = 0;
    for (unsigned i = 0; i < l->members; ++i) {
        const LibraryRecord *record = &l->storage->records[i];
        if (l->tab == X4_UI2_LIBRARY_FAVORITES && record->favorite >= X4_FAVORITES_MAX) continue;
        if (l->tab == X4_UI2_LIBRARY_RECENT && record->recent >= X4_RECENT_MAX) continue;
        if (l->genre && (record->source >= l->catalog_count ||
            !(b->ui2_genre_masks[record->source] & (UINT32_C(1) << l->genre)))) continue;
        grid[l->grid_count++] = (uint16_t)i;
    }
    for (unsigned i = l->grid_count / 2; i > 0; --i) library_sift(b, i - 1, l->grid_count);
    for (unsigned i = l->grid_count; i > 1; --i) {
        uint16_t saved = grid[i - 1];
        grid[i - 1] = grid[0];
        grid[0] = saved;
        library_sift(b, 0, i - 1);
    }
    if (l->grid_selected >= l->grid_count)
        l->grid_selected = l->grid_count ? l->grid_count - 1u : 0;
}

void x4_catalog_browser_ui2_library_close(X4CatalogBrowser *b)
{
    if (!b) return;
    free(b->library.storage);
    b->library = (X4Ui2Library){0};
    b->history = NULL;
}

void x4_catalog_browser_ui2_library_sync(X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, const X4RecentGames *recent, const X4History *history)
{
    if (!b) return;
    if (b->history != history) b->library.dirty = true;
    b->history = history;
    if (b->active_tab != X4_CAT_TAB_RECENTS || !b->ui2_enabled) return;
    X4Ui2Library *l = &b->library;
    uint64_t revision = history ? history->revision : 0;
    uint64_t epoch = recent ? recent->account_generation : 0;
    uint64_t generation = recent ? recent->generation : 0;
    enum X4CatalogState state = c ? c->state : X4_CATALOG_IDLE;
    enum X4RecentState recent_state = recent ? recent->state : X4_RECENT_IDLE;
    unsigned catalog_count = state == X4_CATALOG_READY && c->count <= X4_CATALOG_MAX ? c->count : 0;
    if (l->bound && !l->dirty && l->catalog == c && l->recents == recent &&
        l->catalog_state == state && l->catalog_count == catalog_count && l->recent_epoch == epoch &&
        l->recent_generation == generation && l->recent_state == recent_state &&
        l->history_revision == revision) return;
    if (!l->storage) l->storage = calloc(1, sizeof(*l->storage));
    if (!l->storage) return;
    bool new_epoch = !l->bound || l->recent_epoch != epoch;
    char selected[X4_PRODUCT_ID_CAP] = "";
    if (!new_epoch) {
        X4Ui2LibraryCard old = x4_catalog_browser_ui2_library_card(b, false, l->grid_selected);
        if (old.product_id) snprintf(selected, sizeof(selected), "%s", old.product_id);
    }
    l->catalog = c;
    l->recents = recent;
    l->catalog_count = catalog_count;
    l->catalog_state = state;
    l->recent_state = recent_state;
    l->recent_epoch = epoch;
    l->recent_generation = generation;
    l->history_revision = revision;
    l->bound = true;
    l->dirty = false;
    l->members = l->recent_count = l->favorite_count = 0;
    X4Ui2LibraryStorage *s = l->storage;
    memset(s->buckets, 0, sizeof(s->buckets));
    for (unsigned i = 0; i < x4_recent_count(recent); ++i) {
        unsigned source = X4_CATALOG_MAX;
        const X4RecentGame *game = x4_recent_get(recent, i);
        unsigned resolved;
        if (x4_recent_catalog_index(game, c, &resolved)) source = resolved;
        unsigned before = l->members;
        unsigned at = library_add(b, (LibraryRecord){source, i, X4_FAVORITES_MAX, 0, NULL});
        if (l->members == before) continue;
        if (l->recent_count < X4_UI2_LIBRARY_SHELF) s->continuing[l->recent_count] = (uint16_t)at;
        ++l->recent_count;
    }
    for (unsigned i = 0; i < catalog_count; ++i)
        if (c->titles[i].entitled && c->titles[i].product_id[0] &&
            x4_recent_title_id_valid(c->titles[i].id, sizeof(c->titles[i].id)))
            (void)library_add(b, (LibraryRecord){i, X4_RECENT_MAX, X4_FAVORITES_MAX, 0, NULL});
    for (unsigned i = 0; b->favorites && i < b->favorites->count; ++i) {
        const char *id = b->favorites->items[i].product_id;
        if (!*id) continue;
        unsigned source = X4_CATALOG_MAX;
        for (unsigned j = 0; j < catalog_count; ++j)
            if (ascii_ieq(id, c->titles[j].product_id) &&
                x4_recent_title_id_valid(c->titles[j].id, sizeof(c->titles[j].id))) {
                source = j;
                break;
            }
        unsigned at = library_add(b, (LibraryRecord){source, X4_RECENT_MAX, i, 0, NULL});
        if (at < l->members) ++l->favorite_count;
    }
    for (unsigned i = 0; history && i < l->members; ++i) {
        LibraryRecord *item = &s->records[i];
        item->history = x4_history_find(history, library_card(b, item).product_id);
    }
    if (new_epoch) {
        l->continue_selected = l->grid_selected = 0;
        l->focus = l->recent_count ? X4_UI2_LIBRARY_CONTINUE : X4_UI2_LIBRARY_GRID;
        l->manage = l->popup = false;
    }
    if (l->continue_selected >= l->recent_count)
        l->continue_selected = l->recent_count ? l->recent_count - 1u : 0;
    if (l->focus == X4_UI2_LIBRARY_CONTINUE && !l->recent_count) l->focus = X4_UI2_LIBRARY_GRID;
    if (!(b->ui2_genre_present & (UINT32_C(1) << l->genre))) l->genre = 0;
    library_filter(b);
    for (unsigned i = 0; selected[0] && i < l->grid_count; ++i)
        if (ascii_ieq(selected, x4_catalog_browser_ui2_library_card(b, false, i).product_id)) {
            l->grid_selected = i;
            break;
        }
    ++b->ui2_revision;
}

bool x4_catalog_browser_ui2_library_manage(X4CatalogBrowser *b)
{
    if (!b || b->library.manage || b->library.popup) return false;
    X4Ui2LibraryCard selected = x4_catalog_browser_ui2_library_selection(b);
    if (!selected.product_id || !selected.product_id[0]) return false;
    b->library.manage = true;
    b->library.cursor = 0;
    ++b->ui2_revision;
    return true;
}

static X4Ui2CatalogAction library_activate(X4CatalogBrowser *b)
{
    X4Ui2Library *l = &b->library;
    if (l->manage) {
        l->manage = false;
        ++b->ui2_revision;
        return l->cursor == 1 ? X4_UI2_CATALOG_ACT_REMOVE_FAVORITE : X4_UI2_CATALOG_ACT_NONE;
    }
    if (l->popup) {
        l->popup = false;
        if (l->focus == X4_UI2_LIBRARY_SORT) l->sort = (X4Ui2LibrarySort)l->cursor;
        else l->genre = l->cursor < b->ui2_genre_count ? b->ui2_genres[l->cursor].slot : 0;
        l->grid_selected = 0;
        library_filter(b);
        ++b->ui2_revision;
        return X4_UI2_CATALOG_ACT_CONSUMED;
    }
    switch (l->focus) {
    case X4_UI2_LIBRARY_TABS:
        l->focus = l->tab == X4_UI2_LIBRARY_ALL && l->recent_count ?
            X4_UI2_LIBRARY_CONTINUE : X4_UI2_LIBRARY_SORT;
        break;
    case X4_UI2_LIBRARY_SEE_ALL:
        l->tab = X4_UI2_LIBRARY_RECENT;
        l->focus = X4_UI2_LIBRARY_GRID;
        l->grid_selected = l->genre = 0;
        l->sort = X4_UI2_LIBRARY_RECENCY;
        library_filter(b);
        break;
    case X4_UI2_LIBRARY_SORT:
        l->cursor = l->sort;
        l->popup = true;
        break;
    case X4_UI2_LIBRARY_GENRE:
        l->cursor = 0;
        for (unsigned i = 1; i < b->ui2_genre_count; ++i)
            if (b->ui2_genres[i].slot == l->genre) l->cursor = i;
        l->popup = true;
        break;
    default:
        return X4_UI2_CATALOG_ACT_NONE;
    }
    ++b->ui2_revision;
    return X4_UI2_CATALOG_ACT_CONSUMED;
}

X4Ui2CatalogAction x4_catalog_browser_ui2_activate(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (!b || !b->ui2_enabled || b->editing) return X4_UI2_CATALOG_ACT_NONE;
    if (b->active_tab == X4_CAT_TAB_DISCOVER && b->ui2_collection_focus &&
        b->ui2_collection_selected < b->ui2_collection_count) {
        X4Ui2CollectionKind collection = b->ui2_collections[b->ui2_collection_selected].kind;
        x4_catalog_browser_ui2_select(b, X4_CAT_TAB_CATALOG);
        b->ui2_collection_filter = collection;
        b->ui2_genre = b->ui2_genre_slot = 0;
        b->filter = X4_CATALOG_FILTER_ALL;
        b->query[0] = 0;
        b->ui2_selector_preference = X4_UI2_CATALOG_GENRES;
        x4_catalog_browser_rebuild(b, c);
        return X4_UI2_CATALOG_ACT_CONSUMED;
    }
    if (b->active_tab == X4_CAT_TAB_DISCOVER && b->hero_focused && b->ui2_hero_favorite &&
        hero_can_favorite(b, c)) return X4_UI2_CATALOG_ACT_FAVORITE;
    if (b->active_tab == X4_CAT_TAB_RECENTS && b->library.bound) return library_activate(b);
    if (b->active_tab == X4_CAT_TAB_RECENTS && b->ui2_library_tabs) {
        b->ui2_library_tabs = false;
        ++b->ui2_revision;
        return X4_UI2_CATALOG_ACT_CONSUMED;
    }
    if (b->active_tab != X4_CAT_TAB_CATALOG && b->active_tab != X4_CAT_TAB_SEARCH)
        return X4_UI2_CATALOG_ACT_NONE;
    if (b->ui2_content_focus == X4_UI2_CATALOG_SEARCH) {
        ++b->ui2_revision;
        b->editing = true;
        return X4_UI2_CATALOG_ACT_KEYBOARD;
    }
    if (x4_catalog_browser_ui2_selector_count(b, b->ui2_content_focus)) {
        if (b->ui2_selector_open) {
            x4_catalog_browser_ui2_close_selector(b);
            apply_selector(b, c, b->ui2_content_focus, b->ui2_selector_cursor);
        } else {
            ++b->ui2_revision;
            b->ui2_selector_open = true;
            b->ui2_selector_cursor = x4_catalog_browser_ui2_selector_value(b, b->ui2_content_focus);
        }
        return X4_UI2_CATALOG_ACT_CONSUMED;
    }
    return X4_UI2_CATALOG_ACT_NONE;
}

static void remember_favorite(X4CatalogBrowser *b)
{
    b->ui2_favorite_id[0] = 0;
    if (b->favorites && b->ui2_favorite_selected < b->favorites->count)
        memcpy(b->ui2_favorite_id, b->favorites->items[b->ui2_favorite_selected].product_id,
            sizeof(b->ui2_favorite_id));
}

void x4_catalog_browser_ui2_favorites(X4CatalogBrowser *b, const X4Favorites *favorites)
{
    if (!b) return;
    b->library.dirty = true;
    if (b->filter == X4_CATALOG_FILTER_FAVORITES) b->criteria_dirty = true;
    ++b->ui2_revision;
    b->favorites = favorites && favorites->count <= X4_FAVORITES_MAX ? favorites : NULL;
    unsigned count = b->favorites ? b->favorites->count : 0;
    if (b->ui2_favorite_id[0]) {
        for (unsigned i = 0; i < count; ++i) {
            if (ascii_ieq(b->ui2_favorite_id, b->favorites->items[i].product_id)) {
                b->ui2_favorite_selected = i;
                break;
            }
        }
    }
    if (b->ui2_favorite_selected >= count) b->ui2_favorite_selected = count ? count - 1 : 0;
    remember_favorite(b);
}

const X4Favorite *x4_catalog_browser_ui2_favorite(const X4CatalogBrowser *b)
{
    if (!b || b->active_tab != X4_CAT_TAB_RECENTS || !b->ui2_favorites || b->ui2_library_tabs ||
        !b->favorites || b->ui2_favorite_selected >= b->favorites->count || b->favorites->count > X4_FAVORITES_MAX)
        return NULL;
    return &b->favorites->items[b->ui2_favorite_selected];
}

bool x4_catalog_browser_ui2_left_edge(const X4CatalogBrowser *b)
{
    if (!b) return true;
    if (b->active_tab == X4_CAT_TAB_DISCOVER)
        return b->ui2_collection_focus ? !b->ui2_collection_selected :
            b->hero_focused ? b->featured_count < 2 : !b->discover_row_selected;
    if (b->active_tab == X4_CAT_TAB_RECENTS) {
        if (b->library.bound) {
            const X4Ui2Library *l = &b->library;
            if (l->popup || l->manage) return false;
            switch (l->focus) {
            case X4_UI2_LIBRARY_TABS: return l->tab == X4_UI2_LIBRARY_ALL;
            case X4_UI2_LIBRARY_CONTINUE: return !l->continue_selected;
            case X4_UI2_LIBRARY_GRID: return !(l->grid_selected % X4_CATALOG_UI2_COLUMNS);
            default: return l->focus == X4_UI2_LIBRARY_SORT;
            }
        }
        if (b->ui2_library_tabs) return !b->ui2_favorites;
        unsigned selected = b->ui2_favorites ? b->ui2_favorite_selected : b->recent_selected;
        return !(selected % X4_CATALOG_UI2_COLUMNS);
    }
    if (b->ui2_selector_open) return false;
    if (x4_catalog_browser_ui2_selector_count(b, b->ui2_content_focus))
        return b->ui2_content_focus == X4_UI2_CATALOG_GENRES;
    if (b->ui2_content_focus == X4_UI2_CATALOG_SEARCH) return true;
    return !b->filtered_count || !(b->selected % X4_CATALOG_UI2_COLUMNS);
}

unsigned x4_catalog_browser_ui2_similar(const X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    unsigned source, unsigned out[X4_UI2_SIMILAR_MAX])
{
    if (!b || !c || !out || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX ||
        c->count != b->ui2_genre_sources || source >= c->count || !b->ui2_primary_genre[source]) return 0;
    const unsigned primary = b->ui2_primary_genre[source];
    unsigned count = 0;
    for (unsigned i = 0; i < c->count && count < X4_UI2_SIMILAR_MAX; ++i) {
        if (i == source || primary != b->ui2_primary_genre[i] ||
            !c->titles[i].product_id[0] ||
            ascii_ieq(c->titles[i].product_id, c->titles[source].product_id) ||
            !x4_recent_title_id_valid(c->titles[i].id, sizeof(c->titles[i].id))) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < count; ++j) {
            if (!ascii_ieq(c->titles[i].product_id, c->titles[out[j]].product_id)) continue;
            duplicate = true;
            break;
        }
        if (!duplicate) out[count++] = i;
    }
    return count;
}

static bool ui2_home_nav(X4CatalogBrowser *b, const X4CatalogSnapshot *c, int dx, int dy)
{
    if (b->hero_focused) {
        if (dx) return ui2_move_hero(b, c, dx);
        if (dy < 0 && b->ui2_hero_favorite) {
            b->ui2_hero_favorite = false;
            return true;
        }
        if (dy <= 0) return false;
        if (!b->ui2_hero_favorite && hero_can_favorite(b, c)) {
            b->ui2_hero_favorite = true;
            return true;
        }
        if (!b->home_rows.count && !b->ui2_collection_count) return false;
        b->hero_focused = false;
        b->home_row = b->home_first_row = 0;
        b->ui2_collection_focus = !b->home_rows.count;
        if (b->home_rows.count) home_select(b, c);
        return true;
    }
    if (b->ui2_collection_focus) {
        if (dx) {
            int next = (int)b->ui2_collection_selected + (dx > 0 ? 1 : -1);
            if (next < 0 || (unsigned)next >= b->ui2_collection_count) return false;
            b->ui2_collection_selected = (unsigned)next;
            return true;
        }
        if (dy >= 0) return false;
        b->ui2_collection_focus = false;
        b->hero_focused = !b->home_rows.count;
        if (b->home_rows.count) {
            b->home_row = b->home_rows.count - 1;
            home_select(b, c);
        }
        return true;
    }
    if (dy > 0 && b->home_row + 1 >= b->home_rows.count && b->ui2_collection_count) {
        b->ui2_collection_focus = true;
        b->home_first_row = b->home_rows.count ? b->home_rows.count - 1 : 0;
        return true;
    }
    if (dy < 0 && !b->home_row) {
        b->hero_focused = true;
        b->ui2_hero_favorite = hero_can_favorite(b, c);
        return true;
    }
    return false;
}

static bool home_nav(X4CatalogBrowser *b, const X4CatalogSnapshot *c, int dx, int dy)
{
    if (dy < 0 && b->hero_focused) {
        if (b->ui2_enabled) return false;
        b->top_bar_focused = true;
        b->top_bar_index = X4_CAT_TAB_DISCOVER;
        return true;
    }
    if (!b->home_rows.count) return false;
    const X4HomeRow *row = &b->home_rows.rows[b->home_row];
    if (dx) {
        int next = (int)b->discover_row_selected + (dx > 0 ? 1 : -1);
        if (next < 0 || (unsigned)next >= row->count) return false;
        b->home_columns[row->kind] = (unsigned)next;
    } else if (dy > 0 && b->hero_focused) {
        b->hero_focused = false;
    } else if (dy < 0 && !b->home_row) {
        b->hero_focused = true;
    } else {
        int next = (int)b->home_row + (dy > 0 ? 1 : -1);
        if (next < 0 || (unsigned)next >= b->home_rows.count) return false;
        b->home_row = (unsigned)next;
    }
    home_select(b, c);
    return true;
}

static bool tab_grid_move(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    const X4RecentGames *h, bool recent, int dx, int dy)
{
    b->section = recent ? X4_CATALOG_SECTION_RECENT : X4_CATALOG_SECTION_CATALOG;
    unsigned count = recent ? x4_recent_count(h) : b->filtered_count;
    unsigned *selected = recent ? &b->recent_selected : &b->selected;
    unsigned *preferred = recent ? &b->recent_preferred_column : &b->catalog_preferred_column;
    unsigned original;
    if (!recent && !x4_catalog_browser_selected_index(b, c, &original)) count = 0;
    const unsigned columns = b->ui2_enabled ? X4_CATALOG_UI2_COLUMNS : X4_CATALOG_TAB_COLUMNS;
    unsigned row = *selected / columns;
    if (dy < 0 && (!count || !row)) {
        b->top_bar_focused = true;
        b->top_bar_index = b->active_tab;
        b->detail_scroll_offset = 0;
        return true;
    }
    if (*selected >= count) return false;
    unsigned col = *selected % columns;
    unsigned next;
    if (dx) {
        int next_col = (int)col + (dx > 0 ? 1 : -1);
        if (next_col < 0 || (unsigned)next_col >= columns) return false;
        next = row * columns + (unsigned)next_col;
        if (next >= count) return false;
        *preferred = (unsigned)next_col;
    } else {
        unsigned first = (dy > 0 ? row + 1 : row - 1) * columns;
        if (first >= count) return false;
        if (*preferred < col) *preferred = col;
        next = first + *preferred;
        if (next >= count) next = count - 1;
    }
    *selected = next;
    if (recent) remember_recent(b, h);
    else remember_selected(b, c);
    return true;
}

static unsigned library_continue_count(const X4Ui2Library *l)
{
    return l->recent_count < X4_UI2_LIBRARY_SHELF ? l->recent_count : X4_UI2_LIBRARY_SHELF;
}

static bool library_move(X4CatalogBrowser *b, int dx, int dy)
{
    X4Ui2Library *l = &b->library;
    if (l->manage || l->popup) {
        if (!dy) return false;
        unsigned count = l->manage ? 1u + x4_catalog_browser_ui2_library_selection(b).favorite :
            l->focus == X4_UI2_LIBRARY_SORT ? X4_UI2_LIBRARY_SORT_COUNT : b->ui2_genre_count;
        if (!count) return false;
        unsigned next = (l->cursor + (dy > 0 ? 1u : count - 1u)) % count;
        if (next == l->cursor) return false;
        l->cursor = next;
        return true;
    }
    bool shelf = l->tab == X4_UI2_LIBRARY_ALL && l->recent_count;
    switch (l->focus) {
    case X4_UI2_LIBRARY_TABS:
        if (dx) {
            int next = (int)l->tab + (dx > 0 ? 1 : -1);
            if (next < 0 || next >= X4_UI2_LIBRARY_TAB_COUNT) return false;
            l->tab = (X4Ui2LibraryTab)next;
            l->grid_selected = 0;
            library_filter(b);
        } else if (dy > 0) l->focus = shelf ? X4_UI2_LIBRARY_CONTINUE : X4_UI2_LIBRARY_SORT;
        else return false;
        break;
    case X4_UI2_LIBRARY_CONTINUE:
        if (dx < 0 && l->continue_selected) --l->continue_selected;
        else if (dx > 0) {
            if (l->continue_selected + 1u < library_continue_count(l)) ++l->continue_selected;
            else l->focus = X4_UI2_LIBRARY_SEE_ALL;
        } else if (dy < 0) l->focus = X4_UI2_LIBRARY_TABS;
        else if (dy > 0) l->focus = X4_UI2_LIBRARY_SORT;
        else return false;
        break;
    case X4_UI2_LIBRARY_SEE_ALL:
        if (dx < 0) l->focus = X4_UI2_LIBRARY_CONTINUE;
        else if (dy < 0) l->focus = X4_UI2_LIBRARY_TABS;
        else if (dy > 0) l->focus = X4_UI2_LIBRARY_GENRE;
        else return false;
        break;
    case X4_UI2_LIBRARY_SORT: case X4_UI2_LIBRARY_GENRE:
        if (dx > 0 && l->focus == X4_UI2_LIBRARY_SORT) l->focus = X4_UI2_LIBRARY_GENRE;
        else if (dx < 0 && l->focus == X4_UI2_LIBRARY_GENRE) l->focus = X4_UI2_LIBRARY_SORT;
        else if (dy < 0) l->focus = shelf ? X4_UI2_LIBRARY_CONTINUE : X4_UI2_LIBRARY_TABS;
        else if (dy > 0 && l->grid_count) l->focus = X4_UI2_LIBRARY_GRID;
        else return false;
        break;
    case X4_UI2_LIBRARY_GRID: {
        const unsigned cols = X4_CATALOG_UI2_COLUMNS;
        unsigned selected = l->grid_selected, col = selected % cols;
        if (dx < 0 && col) --l->grid_selected;
        else if (dx > 0 && col + 1u < cols && selected + 1u < l->grid_count)
            ++l->grid_selected;
        else if (dy < 0) {
            if (selected >= cols) l->grid_selected -= cols;
            else l->focus = X4_UI2_LIBRARY_SORT;
        } else if (dy > 0 && l->grid_count && selected / cols + 1u <= (l->grid_count - 1u) / cols) {
            l->grid_selected += cols;
            if (l->grid_selected >= l->grid_count) l->grid_selected = l->grid_count - 1u;
        } else return false;
        break;
    }
    }
    return true;
}

static bool ui2_library_move(X4CatalogBrowser *b, const X4RecentGames *h, int dx, int dy)
{
    if (b->library.bound) return library_move(b, dx, dy);
    b->section = X4_CATALOG_SECTION_RECENT;
    if (b->ui2_library_tabs) {
        if (dy > 0) b->ui2_library_tabs = false;
        else if (dx && (dx > 0) != b->ui2_favorites) b->ui2_favorites = dx > 0;
        else return false;
        return true;
    }
    const bool favorites = b->ui2_favorites;
    const unsigned count = favorites ? (b->favorites ? b->favorites->count : 0) : x4_recent_count(h);
    unsigned *selected = favorites ? &b->ui2_favorite_selected : &b->recent_selected;
    unsigned *preferred = favorites ? &b->ui2_favorite_preferred_column : &b->recent_preferred_column;
    unsigned row = *selected / X4_CATALOG_UI2_COLUMNS, col = *selected % X4_CATALOG_UI2_COLUMNS;
    if (dy < 0 && (!row || !count)) {
        b->ui2_library_tabs = true;
        return true;
    }
    if (*selected >= count) return false;
    unsigned next;
    if (dx) {
        int column = (int)col + (dx > 0 ? 1 : -1);
        if (column < 0 || column >= X4_CATALOG_UI2_COLUMNS) return false;
        next = row * X4_CATALOG_UI2_COLUMNS + (unsigned)column;
        if (next >= count) return false;
        *preferred = (unsigned)column;
    } else {
        unsigned first = (dy > 0 ? row + 1 : row - 1) * X4_CATALOG_UI2_COLUMNS;
        if (first >= count) return false;
        if (*preferred < col) *preferred = col;
        next = first + *preferred;
        if (next >= count) next = count - 1;
    }
    *selected = next;
    if (favorites) remember_favorite(b);
    else remember_recent(b, h);
    return true;
}

static bool ui2_grid_nav(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    const X4RecentGames *h, int dx, int dy)
{
    const bool search = b->active_tab == X4_CAT_TAB_SEARCH;
    if (b->ui2_selector_open) {
        const unsigned count = x4_catalog_browser_ui2_selector_count(b, b->ui2_content_focus);
        if (!count || !dy) return false;
        unsigned current = b->ui2_selector_cursor < count ? b->ui2_selector_cursor : count - 1;
        unsigned next = (current + (dy > 0 ? 1u : count - 1)) % count;
        b->ui2_selector_cursor = next;
        return current != next;
    }
    if (b->ui2_content_focus == X4_UI2_CATALOG_GRID) {
        if (dy >= 0 || (b->filtered_count && b->selected >= X4_CATALOG_UI2_COLUMNS))
            return tab_grid_move(b, c, h, false, dx, dy);
        b->ui2_content_focus = search ? X4_UI2_CATALOG_SEARCH : b->ui2_selector_preference;
        return true;
    }
    if (b->ui2_content_focus == X4_UI2_CATALOG_SEARCH) {
        if (dy <= 0) return false;
        b->ui2_content_focus = search ? X4_UI2_CATALOG_GRID : b->ui2_selector_preference;
        return true;
    }
    if (dx) {
        int next = (int)b->ui2_content_focus + (dx > 0 ? 1 : -1);
        if (next < X4_UI2_CATALOG_GENRES || next > X4_UI2_CATALOG_SORT) return false;
        b->ui2_content_focus = b->ui2_selector_preference = (X4Ui2CatalogFocus)next;
    } else {
        b->ui2_content_focus = dy > 0 ? X4_UI2_CATALOG_GRID : X4_UI2_CATALOG_SEARCH;
    }
    return true;
}

bool x4_catalog_browser_apple_nav(X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, const X4RecentGames *h,
    int dx, int dy, int tab_delta)
{
    if (!b) return false;
    b->apple_mode = true;
    x4_catalog_browser_sync_recents(b, h);
    if (!b->home_configured) x4_catalog_browser_rebuild_featured(b, c, h);

    if (tab_delta != 0) {
        int tabs = b->ui2_enabled ? X4_CAT_TAB_COUNT : X4_CAT_TAB_RECENTS + 1;
        int next_tab = ((int)b->active_tab + (tab_delta > 0 ? 1 : -1) + tabs) % tabs;
        if (b->ui2_enabled) {
            x4_catalog_browser_ui2_select(b, (enum X4CatalogTab)next_tab);
            if (b->criteria_dirty) x4_catalog_browser_rebuild(b, c);
            return true;
        }
        b->active_tab = (enum X4CatalogTab)next_tab;
        b->detail_scroll_offset = 0;
        if (b->top_bar_focused && b->top_bar_index < (unsigned)tabs) {
            b->top_bar_index = (unsigned)b->active_tab;
        }
        if (b->active_tab == X4_CAT_TAB_CATALOG) {
            b->section = X4_CATALOG_SECTION_CATALOG;
        } else if (b->active_tab == X4_CAT_TAB_RECENTS) {
            b->section = X4_CATALOG_SECTION_RECENT;
        } else if (b->active_tab == X4_CAT_TAB_DISCOVER) {
            b->hero_focused = true;
        }
        return true;
    }

    if (dx && dy) return false;
    if (!dx && !dy) return false;

    if (b->top_bar_focused) {
        if (dx != 0) {
            unsigned old = b->top_bar_index;
            if (dx < 0 && b->top_bar_index > 0) b->top_bar_index--;
            else if (dx > 0 && b->top_bar_index < 5) b->top_bar_index++;
            if (b->top_bar_index <= 2) {
                b->active_tab = (enum X4CatalogTab)b->top_bar_index;
                b->detail_scroll_offset = 0;
                if (b->active_tab == X4_CAT_TAB_CATALOG) b->section = X4_CATALOG_SECTION_CATALOG;
                else if (b->active_tab == X4_CAT_TAB_RECENTS) b->section = X4_CATALOG_SECTION_RECENT;
            }
            return b->top_bar_index != old;
        }
        if (dy > 0) {
            b->top_bar_focused = false;
            if (b->active_tab == X4_CAT_TAB_DISCOVER) b->hero_focused = true;
            return true;
        }
        return false;
    }

    if (b->active_tab == X4_CAT_TAB_DISCOVER) {
        if (b->home_configured) {
            bool changed = b->ui2_enabled && ui2_home_nav(b, c, dx, dy);
            if (!changed && !(b->ui2_enabled && (b->hero_focused || b->ui2_collection_focus)))
                changed = home_nav(b, c, dx, dy);
            if (changed && b->ui2_enabled) ++b->ui2_revision;
            return changed;
        }
        unsigned row_count = x4_catalog_browser_discover_row_count(b, c, h);
        if (b->discover_row_selected >= row_count && row_count > 0)
            b->discover_row_selected = row_count - 1;
        if (b->hero_focused) {
            if (dx != 0) {
                if (b->featured_count <= 1) return false;
                b->featured_index = (b->featured_index + (dx > 0 ? 1u : b->featured_count - 1u)) % b->featured_count;
                return true;
            }
            if (dy > 0) {
                if (row_count == 0) return false;
                b->hero_focused = false;
                return true;
            }
            if (dy < 0) {
                b->top_bar_focused = true;
                b->top_bar_index = X4_CAT_TAB_DISCOVER;
                return true;
            }
        } else {
            if (dx != 0) {
                if (row_count == 0) return false;
                int next = (int)b->discover_row_selected + (dx > 0 ? 1 : -1);
                if (next < 0 || next >= (int)row_count) return false;
                b->discover_row_selected = (unsigned)next;
                if (x4_recent_count(h) > 0 && b->discover_row_selected < x4_recent_count(h)) {
                    b->recent_selected = b->discover_row_selected;
                    remember_recent(b, h);
                }
                return true;
            }
            if (dy < 0) {
                b->hero_focused = true;
                return true;
            }
        }
        return false;
    }

    if (b->ui2_enabled) {
        bool changed = b->active_tab == X4_CAT_TAB_RECENTS ?
            ui2_library_move(b, h, dx, dy) : ui2_grid_nav(b, c, h, dx, dy);
        if (changed) ++b->ui2_revision;
        return changed;
    }
    if (b->active_tab == X4_CAT_TAB_CATALOG || b->active_tab == X4_CAT_TAB_RECENTS)
        return tab_grid_move(b, c, h, b->active_tab == X4_CAT_TAB_RECENTS, dx, dy);

    return false;
}

bool x4_catalog_browser_activate_top_bar(X4CatalogBrowser *b)
{
    if (!b || !b->top_bar_focused) return false;
    if (b->top_bar_index <= 2) {
        b->active_tab = (enum X4CatalogTab)b->top_bar_index;
        b->top_bar_focused = false;
        b->detail_scroll_offset = 0;
        if (b->active_tab == X4_CAT_TAB_CATALOG) b->section = X4_CATALOG_SECTION_CATALOG;
        else if (b->active_tab == X4_CAT_TAB_RECENTS) b->section = X4_CATALOG_SECTION_RECENT;
        else if (b->active_tab == X4_CAT_TAB_DISCOVER) b->hero_focused = true;
    } else if (b->top_bar_index == 3) {
        b->editing = true;
        b->top_bar_focused = false;
    } else if (b->top_bar_index == 4) {
        b->open_settings_requested = true;
    } else if (b->top_bar_index == 5) {
        b->open_account_requested = true;
    }
    return true;
}

bool x4_catalog_browser_scroll_detail(X4CatalogBrowser *b, int delta)
{
    if (!b || delta == 0) return false;
    if (delta < 0) {
        if (b->detail_scroll_offset > 0) {
            b->detail_scroll_offset--;
            return true;
        }
    } else {
        if (b->detail_scroll_offset < 100) {
            b->detail_scroll_offset++;
            return true;
        }
    }
    return false;
}

bool x4_catalog_browser_resolved_index(const X4CatalogBrowser *b,const X4CatalogSnapshot *c,
    const X4RecentGames *h,unsigned *source_index)
{
    if (!b) return false;
    if (b->apple_mode) {
        if (!c || !source_index || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX)
            return false;
        if (b->active_tab == X4_CAT_TAB_DISCOVER) {
            if (b->ui2_collection_focus) return false;
            if (b->ui2_enabled && b->hero_focused) {
                const X4CatalogTitle *hero = x4_catalog_browser_ui2_hero(b, c);
                if (!hero) return false;
                *source_index = (unsigned)(hero - c->titles);
                return true;
            }
            if (b->home_configured) {
                /* Jugar uses the debounced title drawn beside it, while a
                 * row card always uses that exact focused session entry. */
                const X4CatalogTitle *hero = b->hero_focused ?
                    x4_catalog_browser_home_product(c, b->home_background_id) : NULL;
                if (!hero) return home_source(b, c, source_index);
                *source_index = (unsigned)(hero - c->titles);
                return x4_recent_title_id_valid(hero->id, sizeof(hero->id));
            }
            if (b->hero_focused) {
                if (!b->featured_count || b->featured_index >= b->featured_count) return false;
                unsigned src = b->featured_sources[b->featured_index];
                if (src >= c->count || !x4_recent_title_id_valid(c->titles[src].id, sizeof(c->titles[src].id)))
                    return false;
                *source_index = src;
                return true;
            }
            return x4_catalog_browser_discover_row_source(b, c, h, b->discover_row_selected, source_index);
        }
        if (b->active_tab == X4_CAT_TAB_RECENTS) {
            if (b->library.bound) {
                const X4Ui2LibraryCard card = x4_catalog_browser_ui2_library_selection(b);
                if (card.source >= c->count || b->library.catalog != c) return false;
                *source_index = card.source;
                return true;
            }
            if (b->ui2_enabled && b->ui2_library_tabs) return false;
            if (b->ui2_enabled && b->ui2_favorites) {
                const X4Favorite *favorite = x4_catalog_browser_ui2_favorite(b);
                if (!favorite) return false;
                for (unsigned i = 0; i < c->count; ++i) {
                    const X4CatalogTitle *title = &c->titles[i];
                    if (!ascii_ieq(title->product_id, favorite->product_id) ||
                        !x4_recent_title_id_valid(title->id, sizeof(title->id))) continue;
                    *source_index = i;
                    return true;
                }
                return false;
            }
            if (!h || !h->account_generation || b->recent_account_generation != h->account_generation)
                return false;
            const X4RecentGame *game = x4_recent_get(h, b->recent_selected);
            if (!game || strcmp(b->recent_selected_id, game->id)) return false;
            return x4_recent_catalog_index(game, c, source_index);
        }
        if (b->active_tab == X4_CAT_TAB_CATALOG || b->active_tab == X4_CAT_TAB_SEARCH) {
            if (b->ui2_enabled && b->ui2_content_focus != X4_UI2_CATALOG_GRID) return false;
            return x4_catalog_browser_selected_index(b, c, source_index);
        }
        return false;
    }
    if (b->section==X4_CATALOG_SECTION_CATALOG) return x4_catalog_browser_selected_index(b,c,source_index);
    if (b->section!=X4_CATALOG_SECTION_RECENT || !h || !h->account_generation ||
        b->recent_account_generation!=h->account_generation) return false;
    const X4RecentGame *game=x4_recent_get(h,b->recent_selected);
    if (!game || strcmp(b->recent_selected_id,game->id)) return false;
    return x4_recent_catalog_index(game,c,source_index);
}

const X4CatalogTitle *x4_catalog_browser_selected_title(const X4CatalogBrowser *b,const X4CatalogSnapshot *c,
    const X4RecentGames *h)
{
    unsigned index;
    return x4_catalog_browser_resolved_index(b,c,h,&index)?&c->titles[index]:NULL;
}

bool x4_catalog_browser_can_launch_frame(uint32_t action_flags) { return action_flags==0; }

bool x4_catalog_browser_cycle_filter(X4CatalogBrowser *b, int delta)
{
    if (!b) return false;
    const int count = b->ui2_enabled ? X4_CATALOG_FILTER_COUNT : X4_CATALOG_FILTER_FAVORITES;
    if ((unsigned)b->filter >= (unsigned)count) return false;
    int64_t next = ((int64_t)b->filter + delta) % count;
    if (next < 0) next += count;
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
