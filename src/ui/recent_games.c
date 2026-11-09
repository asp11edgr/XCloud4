/* SPDX-License-Identifier: GPL-3.0-only */
#include "recent_games.h"
#include <string.h>

_Static_assert(sizeof(X4RecentGames)<=7u*1024,"server MRU presentation budget is 7 KiB");
static size_t length(const char *s,size_t capacity)
{
    size_t n=0; if (s) while (n<capacity&&s[n]) ++n; return n;
}
static bool alnum(unsigned c)
{
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9');
}
bool x4_recent_title_id_valid(const char *id,size_t capacity)
{
    size_t n=length(id,capacity);
    if (!id||!n||n==capacity) return false;
    for (size_t i=0;i<n;++i) if (!alnum((unsigned char)id[i])&&id[i]!='-'&&id[i]!='_'&&id[i]!='.') return false;
    return true;
}
bool x4_recent_display_name_valid(const char *name)
{
    size_t n=length(name,128); if (!name||n==128) return false;
    for (size_t i=0;i<n;) {
        unsigned c=(unsigned char)name[i++];
        if (c<0x80) { if (c<0x20||c==0x7f) return false; continue; }
        unsigned extra,code,minimum;
        if (c>=0xc2&&c<=0xdf) { extra=1;code=c&31;minimum=0x80; }
        else if (c>=0xe0&&c<=0xef) { extra=2;code=c&15;minimum=0x800; }
        else if (c>=0xf0&&c<=0xf4) { extra=3;code=c&7;minimum=0x10000; }
        else return false;
        if (extra>n-i) return false;
        while (extra--) { unsigned t=(unsigned char)name[i++];if((t&0xc0)!=0x80)return false;code=(code<<6)|(t&63); }
        if (code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff)) return false;
    }
    return true;
}
static bool same_id(const char *a,const char *b)
{
    for (;*a&&*b;++a,++b) {
        unsigned x=(unsigned char)*a,y=(unsigned char)*b;
        if (x>='a'&&x<='z') x-=32;
        if (y>='a'&&y<='z') y-=32;
        if (x!=y) return false;
    }
    return *a==*b;
}
void x4_recent_init(X4RecentGames *h) { if(h)memset(h,0,sizeof(*h)); }
unsigned x4_recent_count(const X4RecentGames *h)
{
    return h&&h->state==X4_RECENT_READY&&h->count<=X4_RECENT_MAX?h->count:0;
}
const X4RecentGame *x4_recent_get(const X4RecentGames *h,unsigned index)
{
    return index<x4_recent_count(h)?&h->entries[index]:NULL;
}
void x4_recent_begin(X4RecentGames *h,uint64_t account,uint64_t generation)
{
    if (!h) return;
    if (!account||!generation) { x4_recent_init(h); return; }
    if (h->account_generation!=account) x4_recent_init(h);
    h->account_generation=account; h->generation=generation;
    h->state=X4_RECENT_LOADING; h->error=h->http_status=0;
}
bool x4_recent_catalog_index(const X4RecentGame *game,const X4CatalogSnapshot *c,unsigned *index)
{
    if (!game||!index||!x4_recent_title_id_valid(game->id,sizeof(game->id))||!c||c->state!=X4_CATALOG_READY||c->count>X4_CATALOG_MAX) return false;
    for (unsigned i=0;i<c->count;++i) if (x4_recent_title_id_valid(c->titles[i].id,sizeof(c->titles[i].id))&&same_id(game->id,c->titles[i].id)) {
        *index=i; return true;
    }
    return false;
}
void x4_recent_refresh_catalog(X4RecentGames *h,const X4CatalogSnapshot *c)
{
    if (!h||h->state!=X4_RECENT_READY||h->count>X4_RECENT_MAX) return;
    for (unsigned i=0;i<h->count;++i) {
        X4RecentGame *game=&h->entries[i];
        /* Previously resolved public art is useful even if the current
         * catalog no longer allows launching the same title. */
        unsigned index;
        if (!x4_recent_catalog_index(game,c,&index)) continue;
        const X4CatalogTitle *title=&c->titles[index];
        size_t n=length(title->product_id,sizeof(game->product_id));
        bool valid=n && n<sizeof(game->product_id);
        for(size_t k=0;k<n&&valid;++k) valid=alnum((unsigned char)title->product_id[k]);
        if(valid)memcpy(game->product_id,title->product_id,n+1);
        if(title->name[0]&&x4_recent_display_name_valid(title->name))
            memcpy(game->name,title->name,length(title->name,sizeof(game->name))+1);
    }
}
bool x4_recent_publish(X4RecentGames *h,uint64_t account,uint64_t generation,
    const char ids[][96],unsigned count,int error,int status,const X4CatalogSnapshot *c)
{
    if (!h) return false;
    if (!account||!generation||h->account_generation!=account||h->generation!=generation||h->state!=X4_RECENT_LOADING) {
        ++h->stale_results; return false;
    }
    if (count>X4_RECENT_MAX||(!ids&&count)) error=error?error:-1;
    if (!error) for(unsigned i=0;i<count;++i) if(!x4_recent_title_id_valid(ids[i],96)) { error=-1;break; }
    h->error=error;h->http_status=status;
    if(error){
        memset(h->entries,0,sizeof(h->entries));h->count=0;
        h->state=X4_RECENT_ERROR;return false;
    }
    X4RecentGame next[X4_RECENT_MAX]={0};
    unsigned used=0;
    for(unsigned i=0;i<count;++i) {
        bool duplicate=false;
        for(unsigned j=0;j<used;++j) if(same_id(ids[i],next[j].id)){duplicate=true;break;}
        if(duplicate) continue;
        if(h->count<=X4_RECENT_MAX) for(unsigned j=0;j<h->count;++j) {
            if(same_id(ids[i],h->entries[j].id)){
                next[used]=h->entries[j];
                break;
            }
        }
        memcpy(next[used++].id,ids[i],length(ids[i],96)+1);
    }
    memcpy(h->entries,next,sizeof(next));h->count=used;
    h->state=X4_RECENT_READY;x4_recent_refresh_catalog(h,c);return true;
}
