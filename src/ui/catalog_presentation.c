/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_presentation.h"
#include <string.h>

_Static_assert(sizeof(X4CatalogPresentation)<=736u*1024,"public name/genre overlay budget is 736 KiB");
enum { TOMBSTONE=UINT16_MAX };
static unsigned fold(unsigned c) { return c>='a'&&c<='z'?c-32:c; }
static bool product_valid(const char *id)
{
    if(!id||!id[0])return false;
    for(unsigned i=0;i<X4_PRODUCT_ID_CAP;++i){unsigned c=(unsigned char)id[i];if(!c)return true;
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')))return false;}
    return false;
}
static bool same(const char *a,const char *b)
{
    for(;*a&&*b;++a,++b)if(fold((unsigned char)*a)!=fold((unsigned char)*b))return false;
    return *a==*b;
}
static unsigned hash(const char *id)
{
    uint32_t h=2166136261u;
    while(*id){h^=fold((unsigned char)*id++);h*=16777619u;}
    return h&(X4_PRESENTATION_HASH_SLOTS-1);
}
static unsigned find(const X4CatalogPresentation *p,const char *id,unsigned *empty)
{
    unsigned at=hash(id),first=TOMBSTONE;
    for(unsigned step=0;step<X4_PRESENTATION_HASH_SLOTS;++step,at=(at+1)&(X4_PRESENTATION_HASH_SLOTS-1)){
        unsigned slot=p->lookup[at];
        if(!slot){if(empty)*empty=first==TOMBSTONE?at:first;return X4_CATALOG_MAX;}
        if(slot==TOMBSTONE){if(first==TOMBSTONE)first=at;continue;}
        if(slot<=p->count&&same(p->names[slot-1].product_id,id)){if(empty)*empty=at;return slot-1;}
    }
    if(empty)*empty=first;
    return X4_CATALOG_MAX;
}
static unsigned intern_genre(X4CatalogPresentation *p, const char *name)
{
    size_t length = strnlen(name, sizeof(p->genre_names[0]));
    if (!length || length == sizeof(p->genre_names[0]) || !x4_recent_display_name_valid(name)) return 0;
    for (unsigned i = 1; i <= p->genre_count; ++i)
        if (same(p->genre_names[i], name)) return i;
    if (p->genre_count + 1u == X4_UI2_GENRE_MAX) return 0;
    unsigned slot = ++p->genre_count;
    memcpy(p->genre_names[slot], name, length + 1);
    return slot;
}

static unsigned get_or_create(X4CatalogPresentation *p, const char *id, uint64_t generation)
{
    unsigned empty, index = find(p, id, &empty);
    if (index != X4_CATALOG_MAX) return index;
    if (p->count < X4_CATALOG_MAX) {
        index = p->count++;
    } else {
        for (index = 0; index < p->count; ++index)
            if (p->names[index].seen_generation != generation) break;
        if (index == p->count) {
            ++p->capacity_rejected;
            return X4_CATALOG_MAX;
        }
        unsigned old_slot;
        find(p, p->names[index].product_id, &old_slot);
        p->lookup[old_slot] = TOMBSTONE;
        find(p, id, &empty);
    }
    if (empty == TOMBSTONE) {
        ++p->capacity_rejected;
        return X4_CATALOG_MAX;
    }
    memset(&p->names[index], 0, sizeof(p->names[index]));
    p->primary_genres[index] = p->detail_genres[index] = 0;
    p->attributes[index] = 0;
    memcpy(p->names[index].product_id, id, strlen(id) + 1);
    p->lookup[empty] = (uint16_t)(index + 1);
    return index;
}

void x4_catalog_presentation_init(X4CatalogPresentation *p){if(p)memset(p,0,sizeof(*p));}
void x4_catalog_presentation_set_generation(X4CatalogPresentation *p,uint64_t generation)
{
    if(!p||p->generation==generation)return;
    p->generation=generation;p->last_revision=0;
}
size_t x4_catalog_presentation_ingest(X4CatalogPresentation *p,uint64_t generation,uint64_t revision,
    const X4ProductMetadata *metadata,size_t count)
{
    if(!p||(!metadata&&count)||count>X4_PRESENTATION_DEMAND_MAX)return 0;
    if(!generation||generation!=p->generation){++p->stale_results;return 0;}
    if(!revision||revision<=p->last_revision)return 0;
    p->last_revision=revision;size_t changed=0;
    for(size_t i=0;i<count;++i){
        const X4ProductMetadata *m=&metadata[i];
        if(!product_valid(m->product_id)||!m->name[0]||!x4_recent_display_name_valid(m->name)){++p->rejected_metadata;continue;}
        unsigned index=get_or_create(p,m->product_id,generation);
        if(index==X4_CATALOG_MAX)continue;
        unsigned genre=intern_genre(p,m->category);
        if(genre)p->primary_genres[index]=(uint8_t)genre;
        p->names[index].seen_generation=generation;
        if(strcmp(p->names[index].name,m->name)){
            memset(p->names[index].name,0,sizeof(p->names[index].name));
            memcpy(p->names[index].name,m->name,strlen(m->name)+1);++changed;
        }
    }
    return changed;
}
bool x4_catalog_presentation_apply_names(X4CatalogPresentation *p,X4CatalogSnapshot *c)
{
    if(!p||!c||c->state!=X4_CATALOG_READY||c->count>X4_CATALOG_MAX)return false;
    bool changed=false;
    for(unsigned i=0;i<c->count;++i){
        X4CatalogTitle *t=&c->titles[i];
        if(!x4_recent_title_id_valid(t->id,sizeof(t->id))||!product_valid(t->product_id))continue;
        unsigned index=find(p,t->product_id,NULL);if(index==X4_CATALOG_MAX)continue;
        p->names[index].seen_generation=p->generation;
        if(p->names[index].name[0]&&strncmp(t->name,p->names[index].name,sizeof(t->name))){
            memcpy(t->name,p->names[index].name,sizeof(t->name));changed=true;
        }
    }
    return changed;
}
bool x4_catalog_presentation_ingest_detail(X4CatalogPresentation *p,
    const char *id, const X4GameDetails *details)
{
    if (!p || !p->generation || !product_valid(id) || !details) return false;
    unsigned primary = intern_genre(p, details->category);
    unsigned secondary = intern_genre(p, details->subcategory);
    const uint16_t attributes = (uint16_t)details->attributes;
    if (!primary && !secondary && !attributes) return false;
    unsigned index = get_or_create(p, id, p->generation);
    if (index == X4_CATALOG_MAX) return false;
    p->names[index].seen_generation = p->generation;
    if (!primary) primary = p->primary_genres[index] ? p->primary_genres[index] : secondary;
    bool changed = p->primary_genres[index] != primary ||
        p->detail_genres[index] != secondary || p->attributes[index] != attributes;
    p->primary_genres[index] = (uint8_t)primary;
    p->detail_genres[index] = (uint8_t)secondary;
    p->attributes[index] = attributes;
    return changed;
}

bool x4_catalog_presentation_apply_genres(X4CatalogPresentation *p, X4CatalogBrowser *b,
    const X4CatalogSnapshot *c)
{
    if (!p || !b || !c || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX) return false;
    bool changed = b->ui2_genre_sources != c->count;
    uint32_t present = 0;
    for (unsigned i = 0; i < c->count; ++i) {
        const X4CatalogTitle *title = &c->titles[i];
        unsigned index = product_valid(title->product_id) &&
            x4_recent_title_id_valid(title->id, sizeof(title->id)) ?
            find(p, title->product_id, NULL) : X4_CATALOG_MAX;
        uint8_t primary = 0, secondary = 0;
        uint16_t attributes = 0;
        if (index != X4_CATALOG_MAX) {
            p->names[index].seen_generation = p->generation;
            primary = p->primary_genres[index];
            secondary = p->detail_genres[index];
            attributes = p->attributes[index];
        }
        uint32_t genres = ((UINT32_C(1) << primary) | (UINT32_C(1) << secondary)) & ~UINT32_C(1);
        present |= genres;
        if (b->ui2_genre_masks[i] != genres || b->ui2_primary_genre[i] != primary ||
            b->ui2_attributes[i] != attributes) changed = true;
        b->ui2_genre_masks[i] = genres;
        b->ui2_primary_genre[i] = primary;
        b->ui2_attributes[i] = attributes;
    }
    if (memcmp(b->ui2_genre_names, p->genre_names, sizeof(b->ui2_genre_names))) {
        memcpy(b->ui2_genre_names, p->genre_names, sizeof(b->ui2_genre_names));
        changed = true;
    }
    b->ui2_genre_present = present;
    b->ui2_genre_sources = c->count;
    return changed;
}

static void append(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],size_t *count,const char *id)
{
    if(*count>=X4_PRESENTATION_DEMAND_MAX||!product_valid(id))return;
    for(size_t i=0;i<*count;++i)if(same(ids[i],id))return;
    memcpy(ids[(*count)++],id,strlen(id)+1);
}
static void catalog_page(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],size_t *count,
    const X4CatalogBrowser *b,const X4CatalogSnapshot *c,unsigned start)
{
    for(unsigned i=start;i<b->filtered_count&&i<start+X4_CATALOG_PAGE_SIZE;++i){
        unsigned source=b->indices[i];
        if(source<c->count&&source<X4_CATALOG_MAX&&x4_recent_title_id_valid(c->titles[source].id,sizeof(c->titles[source].id)))
            append(ids,count,c->titles[source].product_id);
    }
}
static void recent_page(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],size_t *count,
    const X4RecentGames *h,const X4CatalogSnapshot *c,unsigned start)
{
    unsigned recent_count=x4_recent_count(h);
    for(unsigned i=start;i<recent_count&&i<start+X4_CATALOG_PAGE_SIZE;++i){
        unsigned source;
        if(x4_recent_catalog_index(x4_recent_get(h,i),c,&source))append(ids,count,c->titles[source].product_id);
    }
}
static unsigned adjacent_page(unsigned start,unsigned count)
{
    if(start>=count)return count;
    return start+X4_CATALOG_PAGE_SIZE<count?start+X4_CATALOG_PAGE_SIZE:
        start>=X4_CATALOG_PAGE_SIZE?start-X4_CATALOG_PAGE_SIZE:start;
}
size_t x4_catalog_presentation_demand(const X4CatalogPresentation *p,const X4CatalogBrowser *b,
    const X4CatalogSnapshot *c,const X4RecentGames *h,char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP])
{
    if(!ids)return 0;
    memset(ids,0,X4_PRESENTATION_DEMAND_MAX*X4_PRODUCT_ID_CAP);
    if(!p||!p->generation||!b||!c||c->state!=X4_CATALOG_READY||c->count>X4_CATALOG_MAX||
        b->filtered_count>X4_CATALOG_MAX||b->criteria_dirty)return 0;
    size_t count=0;
    const X4CatalogTitle *selected=x4_catalog_browser_selected_title(b,c,h);
    if(selected)append(ids,&count,selected->product_id);
    unsigned catalog_start=x4_catalog_browser_page_start(b);
    bool current_recent=h&&h->account_generation&&b->recent_account_generation==h->account_generation;
    unsigned recent_count=current_recent?x4_recent_count(h):0;
    unsigned recent_start=b->recent_selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
    /* Match the renderer's visible page, not MRU index zero. A changed
     * generation is never used to populate this account's demand. */
    if(b->section==X4_CATALOG_SECTION_RECENT&&current_recent)
        recent_page(ids,&count,h,c,recent_start);
    catalog_page(ids,&count,b,c,catalog_start);
    if(b->section!=X4_CATALOG_SECTION_RECENT&&current_recent)
        recent_page(ids,&count,h,c,recent_start);
    /* No speculative item precedes an actually visible item. */
    if(b->section==X4_CATALOG_SECTION_RECENT&&recent_count>X4_CATALOG_PAGE_SIZE)
        recent_page(ids,&count,h,c,adjacent_page(recent_start,recent_count));
    if(b->filtered_count>X4_CATALOG_PAGE_SIZE)
        catalog_page(ids,&count,b,c,adjacent_page(catalog_start,b->filtered_count));
    if(b->section!=X4_CATALOG_SECTION_RECENT&&recent_count>X4_CATALOG_PAGE_SIZE)
        recent_page(ids,&count,h,c,adjacent_page(recent_start,recent_count));
    return count;
}

static void append_surface(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX], size_t *count, const char *id, uint32_t mask)
{
    if (!product_valid(id)) return;
    for (size_t i = 0; i < *count; ++i) {
        if (same(ids[i], id)) {
            masks[i] |= mask;
            return;
        }
    }
    if (*count == X4_PRESENTATION_DEMAND_MAX) return;
    memcpy(ids[*count], id, strlen(id) + 1);
    masks[(*count)++] = mask;
}

static void demand_row(const X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    unsigned row_index, unsigned first, unsigned length,
    char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX], size_t *count)
{
    if (row_index >= b->home_rows.count) return;
    const X4HomeRow *row = &b->home_rows.rows[row_index];
    for (unsigned i = first; i < row->count && i < first + length; ++i) {
        unsigned source = row->indices[i];
        if (source < c->count)
            append_surface(ids, masks, count, c->titles[source].product_id, 1u << X4_COVER_ROW);
    }
}

/* The tab page and demand use the same ordinal grid. Historical product
 * IDs are copied by the MRU owner within an account; they can supply art,
 * but only catalog_browser may resolve a launch. */
static const char *secondary_product(const X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    const X4RecentGames *h, unsigned ordinal)
{
    if (b->active_tab == X4_CAT_TAB_RECENTS) {
        if (b->ui2_enabled && b->ui2_favorites)
            return b->favorites && ordinal < b->favorites->count ?
                b->favorites->items[ordinal].product_id : NULL;
        const X4RecentGame *game = x4_recent_get(h, ordinal);
        return game ? game->product_id : NULL;
    }
    unsigned source = b->indices[ordinal];
    return source < c->count ? c->titles[source].product_id : NULL;
}

static size_t demand_secondary(const X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    const X4RecentGames *h, char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX])
{
    if (b->active_tab == X4_CAT_TAB_RECENTS && b->library.bound) {
        const X4Ui2Library *library = &b->library;
        const bool shelf = library->tab == X4_UI2_LIBRARY_ALL && library->recent_count;
        const unsigned page = shelf ? X4_CATALOG_UI2_COLUMNS : X4_CATALOG_UI2_PAGE_SIZE;
        const unsigned first = library->grid_selected / page * page;
        const uint32_t mask = 1u << X4_COVER_ROW;
        size_t count = 0;
        append_surface(ids, masks, &count,
            x4_catalog_browser_ui2_library_selection(b).product_id, mask);
        if (shelf)
            for (unsigned i = 0; i < X4_UI2_LIBRARY_SHELF && i < library->recent_count; ++i)
                append_surface(ids, masks, &count,
                    x4_catalog_browser_ui2_library_card(b, true, i).product_id, mask);
        for (unsigned i = first; i < first + page && i < library->grid_count; ++i)
            append_surface(ids, masks, &count,
                x4_catalog_browser_ui2_library_card(b, false, i).product_id, mask);
        unsigned adjacent = first + page;
        if (adjacent >= library->grid_count) adjacent = first ? first - page : first;
        if (adjacent != first)
            for (unsigned i = adjacent; i < adjacent + page && i < library->grid_count; ++i)
                append_surface(ids, masks, &count,
                    x4_catalog_browser_ui2_library_card(b, false, i).product_id, mask);
        return count;
    }
    unsigned total, selected;
    if (b->active_tab == X4_CAT_TAB_RECENTS && b->ui2_enabled && b->ui2_favorites) {
        total = b->favorites && b->favorites->count <= X4_FAVORITES_MAX ? b->favorites->count : 0;
        selected = b->ui2_favorite_selected;
    } else if (b->active_tab == X4_CAT_TAB_RECENTS) {
        if (!h || !h->account_generation || b->recent_account_generation != h->account_generation)
            return 0;
        total = x4_recent_count(h);
        selected = b->recent_selected;
    } else {
        if (!c || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX ||
            b->filtered_count > c->count || b->criteria_dirty)
            return 0;
        total = b->filtered_count;
        selected = b->selected;
    }
    if (!total) return 0;
    if (selected >= total) selected = 0;
    const unsigned page = b->ui2_enabled ? X4_CATALOG_UI2_PAGE_SIZE : X4_CATALOG_TAB_PAGE_SIZE;
    const unsigned first = selected / page * page;
    unsigned adjacent = first + page;
    if (adjacent >= total) adjacent = first ? first - page : first;
    size_t count = 0;
    const uint32_t mask = 1u << X4_COVER_ROW;
    append_surface(ids, masks, &count, secondary_product(b, c, h, selected), mask);
    for (unsigned i = first; i < first + page && i < total; ++i)
        append_surface(ids, masks, &count, secondary_product(b, c, h, i), mask);
    if (adjacent != first)
        for (unsigned i = adjacent; i < adjacent + page && i < total; ++i)
            append_surface(ids, masks, &count, secondary_product(b, c, h, i), mask);
    return count;
}

size_t x4_catalog_presentation_demand_surfaces(const X4CatalogPresentation *p,
    const X4CatalogBrowser *b, const X4CatalogSnapshot *c, const X4RecentGames *h,
    char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX])
{
    if (!ids || !masks) return 0;
    memset(masks, 0, X4_PRESENTATION_DEMAND_MAX * sizeof(*masks));
    if (b && b->apple_mode &&
        (b->active_tab == X4_CAT_TAB_CATALOG || b->active_tab == X4_CAT_TAB_RECENTS ||
         b->active_tab == X4_CAT_TAB_SEARCH)) {
        memset(ids, 0, X4_PRESENTATION_DEMAND_MAX * X4_PRODUCT_ID_CAP);
        return p && p->generation ? demand_secondary(b, c, h, ids, masks) : 0;
    }
    if (!b || !b->home_configured || b->active_tab != X4_CAT_TAB_DISCOVER) {
        size_t count = x4_catalog_presentation_demand(p, b, c, h, ids);
        const X4CatalogTitle *selected = count ? x4_catalog_browser_selected_title(b, c, h) : NULL;
        for (size_t i = 0; i < count; ++i) {
            masks[i] = 1u << X4_COVER_GRID;
            if (selected && same(ids[i], selected->product_id)) masks[i] |= 1u << X4_COVER_DETAIL;
        }
        return count;
    }

    memset(ids, 0, X4_PRESENTATION_DEMAND_MAX * X4_PRODUCT_ID_CAP);
    if (!p || !p->generation || !c || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX) return 0;
    size_t count = 0;
    const X4CatalogTitle *selected = x4_catalog_browser_selected_title(b, c, h);
    if (selected) append_surface(ids, masks, &count, selected->product_id,
        (1u << X4_COVER_ROW) | (1u << X4_COVER_DETAIL));
    if (x4_catalog_browser_home_product(c, b->home_background_id))
        append_surface(ids, masks, &count, b->home_background_id, 1u << X4_COVER_ROW);
    if (b->ui2_enabled && b->home_first_row + 1 >= b->home_rows.count)
        for (unsigned i = 0; i < b->ui2_collection_count; ++i) {
            unsigned source = b->ui2_collections[i].source;
            if (source < c->count)
                append_surface(ids, masks, &count, c->titles[source].product_id, 1u << X4_COVER_ROW);
        }
    const unsigned visible = b->ui2_enabled ? X4_CATALOG_UI2_DISCOVER_VISIBLE :
        X4_CATALOG_DISCOVER_ROW_VISIBLE;
    for (unsigned row = b->home_first_row; row < b->home_first_row + 2; ++row) {
        unsigned start = x4_catalog_browser_home_row_start(b, row);
        demand_row(b, c, row, start, visible, ids, masks, &count);
    }
    for (unsigned row = b->home_first_row; row < b->home_first_row + 2; ++row) {
        unsigned start = x4_catalog_browser_home_row_start(b, row);
        if (start) demand_row(b, c, row, start - 1, 1, ids, masks, &count);
        demand_row(b, c, row, start + visible, 1, ids, masks, &count);
    }
    return count;
}
