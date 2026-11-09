/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "ui/catalog_assets.h"
#include "ui/catalog_disk_cache.h"
#include "ui/catalog_presentation.h"
#include "ui/screen.h"
#include "ui/image_area.h"
#include "auth/http_client.h"
#include "auth/json.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"check failed %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void sleep_ms(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
static unsigned char *read_file(const char *path, size_t *length)
{
    FILE *f=fopen(path,"rb"); CHECK(f); CHECK(!fseek(f,0,SEEK_END)); long n=ftell(f); CHECK(n>0); rewind(f);
    unsigned char *p=malloc((size_t)n+1); CHECK(p); CHECK(fread(p,1,(size_t)n,f)==(size_t)n); fclose(f); p[n]=0; *length=(size_t)n; return p;
}
struct X4Http { pthread_t owner; int busy; uint64_t retry_after_ms; };
static _Atomic int block_json, block_image, block_reset, ignore_cancel, entered, image_entered, reset_entered;
static _Atomic int fake_missing, fake_corrupt, fake_image_status, fake_partial_multi, fake_single_missing, fake_reset_error;
static _Atomic int fake_no_fortnite_poster;
static _Atomic unsigned json_calls, image_calls, reset_calls, open_calls, active_requests, active_http, fake_batch_limit;
static _Atomic int release_rate_head, arm_rate_head, block_decode;
static _Atomic unsigned active_images, peak_images, image_arrivals, image_cancellations;
static _Atomic unsigned active_decodes, peak_decodes, decode_calls;
static unsigned fixture_downloaders = 1;
static unsigned fixture_game;
static _Atomic unsigned screenshot_bound_calls;
static _Atomic int bound_screenshots;
static int trace_disk;
static const char *disk_phase = "cold";
static _Atomic unsigned sigl_calls[X4_SIGL_COUNT];
static _Atomic uint64_t fake_now_us, fake_retry_ms;
static char metadata_order[128][256], image_order[128][32];
static char image_queries[128][64];
static unsigned char *fake_image;
static size_t fake_image_length;
static void check_owner(X4Http *h) { CHECK(h && pthread_equal(h->owner,pthread_self())); }
static void note_peak(_Atomic unsigned *peak, unsigned current)
{
    unsigned previous=atomic_load(peak);
    while(previous<current&&!atomic_compare_exchange_weak(peak,&previous,current)){}
}
static void begin_request(X4Http *h)
{
    check_owner(h);CHECK(!h->busy);h->busy=1;h->retry_after_ms=0;
    CHECK(atomic_fetch_add(&active_requests,1)<=fixture_downloaders+fixture_game);
}
static void end_request(X4Http *h)
{
    CHECK(h->busy);h->busy=0;CHECK(atomic_fetch_sub(&active_requests,1)>0);
}
X4Http *x4_http_open(int *error)
{
    if(error)*error=0;
    X4Http*h=calloc(1,sizeof(*h));CHECK(h);h->owner=pthread_self();
    CHECK(atomic_fetch_add(&active_http,1)<=fixture_downloaders+fixture_game);atomic_fetch_add(&open_calls,1);return h;
}
void x4_http_close(X4Http *h)
{
    if(!h)return;
    check_owner(h);CHECK(!h->busy);
    CHECK(atomic_fetch_sub(&active_http,1)>0);free(h);
}
int x4_http_public_reset(X4Http*h)
{
    check_owner(h);CHECK(!h->busy);atomic_fetch_add(&reset_calls,1);
    atomic_store(&reset_entered,1);while(atomic_load(&block_reset))sleep_ms(1);return atomic_load(&fake_reset_error);
}
uint64_t x4_http_public_retry_after_ms(const X4Http*h)
{CHECK(h&&pthread_equal(h->owner,pthread_self()));return h->retry_after_ms;}
int x4_http_public_metadata_request(X4Http *h,const char *url,
    char *body,size_t capacity,size_t *length,int *status,const _Atomic int *cancel)
{
    CHECK(cancel);begin_request(h);
    if(!strncmp(url,X4_SIGL_URL_PREFIX,strlen(X4_SIGL_URL_PREFIX))){
        unsigned kind=0;
        for(;kind<X4_SIGL_COUNT;++kind){
            char expected[160];CHECK(x4_sigl_url(kind,expected,sizeof(expected)));
            if(!strcmp(url,expected))break;
        }
        CHECK(kind<X4_SIGL_COUNT);atomic_fetch_add(&sigl_calls[kind],1);
        const char list[]="[{\"title\":\"Fixture\"},{\"id\":\"CARD0\"}]";
        CHECK(sizeof(list)<=capacity);memcpy(body,list,sizeof(list));*length=sizeof(list)-1;
        *status=kind==X4_SIGL_LEAVING?503:200;
        end_request(h);
        return 0;
    }
    unsigned call=atomic_fetch_add(&json_calls,1);CHECK(call<128);
    const char*prefix="https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=";
    CHECK(!strncmp(url,prefix,strlen(prefix)));
    CHECK(strstr(url,"&market=MX&languages=es-MX&fieldsTemplate=Details"));
    const char *id=strstr(url,"bigIds=");CHECK(id);id+=7;
    size_t query_length=strcspn(id,"&");CHECK(query_length<sizeof(metadata_order[0]));
    memcpy(metadata_order[call],id,query_length);metadata_order[call][query_length]=0;
    char products[4][32];unsigned count=0;size_t offset=0;
    while(offset<query_length){size_t end=offset;while(end<query_length&&id[end]!=',')++end;
        CHECK(count<4&&end>offset&&end-offset<sizeof(products[0]));
        memcpy(products[count],id+offset,end-offset);products[count][end-offset]=0;
        CHECK(x4_product_id_valid(products[count]));++count;offset=end+1;}
    CHECK(count>0);atomic_store(&entered,1);
    while(atomic_load(&block_json) && (atomic_load(&ignore_cancel) || !atomic_load(cancel))) sleep_ms(1);
    int rc=0;*length=0;*status=0;
    if(atomic_load(cancel)&&!atomic_load(&ignore_cancel)){rc=X4_HTTP_CANCELLED;goto done;}
    if(atomic_load(&fake_batch_limit)&&count>atomic_load(&fake_batch_limit)){rc=X4_HTTP_BOUND;goto done;}
    size_t used=0;int written=snprintf(body,capacity,"{\"Products\":[");CHECK(written>0&&(size_t)written<capacity);used=(size_t)written;
    unsigned emitted=0;
    for(unsigned i=0;i<count;++i){
        if(!strcmp(products[i],"ID2")&&((count>1&&atomic_load(&fake_partial_multi))||atomic_load(&fake_single_missing)))continue;
        const char *poster = "";
        if (!strcmp(products[i], X4_FORTNITE_PRODUCT_ID) && !atomic_load(&fake_no_fortnite_poster))
            poster = ",{\"ImagePurpose\":\"Poster\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.test-asset-"
                X4_FORTNITE_PRODUCT_ID "\",\"Width\":1440,\"Height\":2160}";
        char shots[2400]={0};
        if(fixture_game&&!strncmp(products[i],"GAME",4)){
            size_t offset=0;
            for(unsigned shot=0;shot<8;++shot){
                int n=snprintf(shots+offset,sizeof(shots)-offset,
                    ",{\"ImagePurpose\":\"Screenshot\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.test-asset-%s-SHOT%u\","
                    "\"Width\":1920,\"Height\":1080}",products[i],shot);
                CHECK(n>0&&(size_t)n<sizeof(shots)-offset);
                offset+=(size_t)n;
            }
        }
        written=snprintf(body+used,capacity-used,
            "%s{\"ProductId\":\"%s\",\"Properties\":{\"Category\":\"Test\"},"
            "\"MarketProperties\":[{\"OriginalReleaseDate\":\"2021-11-09T06:00:00.0000000Z\","
            "\"ContentRatings\":[{\"RatingSystem\":\"ESRB\",\"RatingId\":\"ESRB:E\"}]}],"
            "\"LocalizedProperties\":[{\"ProductTitle\":\"Edición ñü %s\",\"DeveloperName\":\"Example Studio\","
            "\"ShortDescription\":\"Short fixture\",\"ProductDescription\":\"Long fixture description. A second sentence.\",\"Images\":["
            "{\"ImagePurpose\":\"BoxArt\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.test-asset-%s\",\"Width\":80,\"Height\":120},"
            "{\"ImagePurpose\":\"SuperHeroArt\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.test-asset-%s\",\"Width\":1920,\"Height\":1080}%s%s]}]}",
            emitted?",":"",products[i],products[i],products[i],products[i],poster,shots);CHECK(written>0&&(size_t)written<capacity-used);
        used+=(size_t)written;++emitted;
    }
    written=snprintf(body+used,capacity-used,"]}");CHECK(written>0&&(size_t)written<capacity-used);
    *length=used+(size_t)written;*status=atomic_load(&fake_missing)?404:200;
done:
    end_request(h);return rc;
}
int x4_http_public_image_request(X4Http *h,const char *url,unsigned char *body,size_t capacity,size_t *length,int *status,const _Atomic int *cancel)
{
    CHECK(x4_http_public_image_url(url));CHECK(cancel);begin_request(h);
    unsigned current=atomic_fetch_add(&active_images,1)+1;
    CHECK(current<=fixture_downloaders+fixture_game);
    note_peak(&peak_images,current);
    unsigned call=atomic_fetch_add(&image_calls,1);CHECK(call<128);
    const char*id=strstr(url,"apps.test-asset-");CHECK(id);id+=16;
    size_t n=strcspn(id,"?");CHECK(n<sizeof(image_order[0]));memcpy(image_order[call],id,n);image_order[call][n]=0;
    const char *query = id + n;
    CHECK(*query == '?' && strlen(query) < sizeof(image_queries[0]));
    memcpy(image_queries[call], query, strlen(query) + 1);
    int screenshot=fixture_game&&strstr(image_order[call],"-SHOT")!=NULL;
#ifndef X4_ASSETS_TEST_BASELINE31D
    if(screenshot){
        if(!strcmp(query,"?w=524&h=295&q=85&format=jpg"))
            CHECK(capacity==X4_GAME_SCREENSHOT_THUMB_BYTES+1u);
        else{
            CHECK(!strcmp(query,"?w=1920&h=1080&q=85&format=jpg"));
            CHECK(capacity==X4_GAME_SCREENSHOT_FULL_BYTES+1u);
        }
    }
#endif
    int rate_head=!strcmp(image_order[call],"RATEHEAD");
    atomic_fetch_add(&image_arrivals,1);
    atomic_store(&image_entered,1);
    int observed_cancel=0,rc=0;
    while(atomic_load(&block_image)&&!(rate_head&&atomic_load(&release_rate_head))){
        if(atomic_load(cancel)){
            if(!observed_cancel)atomic_fetch_add(&image_cancellations,1);
            observed_cancel=1;
            if(!atomic_load(&ignore_cancel))break;
        }
        sleep_ms(1);
    }
    if(atomic_load(cancel)){
        if(!observed_cancel)atomic_fetch_add(&image_cancellations,1);
        if(!atomic_load(&ignore_cancel)){rc=X4_HTTP_CANCELLED;goto done;}
    }
    if(screenshot&&atomic_load(&bound_screenshots)){
        atomic_fetch_add(&screenshot_bound_calls,1);
        rc=X4_HTTP_BOUND;
        goto done;
    }
    if(atomic_load(&fake_corrupt)){memcpy(body,"corrupt",7);*length=7;}else{CHECK(fake_image_length<capacity);memcpy(body,fake_image,fake_image_length);*length=fake_image_length;}
    *status=atomic_load(&fake_image_status);
    if(rate_head&&atomic_exchange(&arm_rate_head,0))*status=503;
    if(*status>=400)h->retry_after_ms=atomic_load(&fake_retry_ms);
done:
    CHECK(atomic_fetch_sub(&active_images,1)>0);
    end_request(h);return rc;
}
int __real_x4_product_surface_decode(const unsigned char *encoded,size_t length,
    enum X4CoverSurface surface,X4CoverRaster *out);
int __wrap_x4_product_surface_decode(const unsigned char *encoded,size_t length,
    enum X4CoverSurface surface,X4CoverRaster *out)
{
    unsigned current=atomic_fetch_add(&active_decodes,1)+1;
    note_peak(&peak_decodes,current);
    atomic_fetch_add(&decode_calls,1);
    while(atomic_load(&block_decode))sleep_ms(1);
    int rc=__real_x4_product_surface_decode(encoded,length,surface,out);
    CHECK(atomic_fetch_sub(&active_decodes,1)>0);
    return rc;
}
X4CatalogDiskCache *__real_x4_catalog_disk_cache_open(const char *,size_t,unsigned,
    X4CatalogDiskClock,void *);
int __real_x4_catalog_disk_cache_get_metadata(X4CatalogDiskCache *,const char *,
    const char *,const char *,X4ProductMetadata *,const _Atomic int *);
int __real_x4_catalog_disk_cache_get_image(X4CatalogDiskCache *,const char *,
    const char *,const char *,const char *,unsigned char *,size_t,size_t *,const _Atomic int *);
int __real_x4_catalog_disk_cache_put_metadata(X4CatalogDiskCache *,const char *,
    const char *,const char *,const X4ProductMetadata *,const _Atomic int *);
int __real_x4_catalog_disk_cache_put_image(X4CatalogDiskCache *,const char *,
    const char *,const char *,const char *,const unsigned char *,size_t,const _Atomic int *);

X4CatalogDiskCache *__wrap_x4_catalog_disk_cache_open(const char *root,size_t budget,
    unsigned products,X4CatalogDiskClock clock,void *context)
{
    X4CatalogDiskCache *cache=__real_x4_catalog_disk_cache_open(root,budget,products,clock,context);
    if(trace_disk){
        int saved=errno;
        X4CatalogDiskStats s={0};
        if(cache)x4_catalog_disk_cache_stats(cache,&s);
        printf("CACHE phase=%s operation=open enabled=%d records=%u corrupt=%llu expired=%llu io=%llu errno=%d\n",
            disk_phase,cache!=NULL,s.records,(unsigned long long)s.corrupt,(unsigned long long)s.expired,
            (unsigned long long)s.io_errors,cache?0:saved);
        errno=saved;
    }
    return cache;
}
static uint64_t trace_url_hash(const char *url)
{
    if(!url)return 0;
    uint64_t hash=UINT64_C(14695981039346656037);
    for(const unsigned char *p=(const unsigned char *)url;*p;++p)
        hash=(hash^*p)*UINT64_C(1099511628211);
    return hash;
}
static void trace_disk_get(X4CatalogDiskCache *cache,const X4CatalogDiskStats *before,
    const char *kind,const char *id,const char *market,const char *language,const char *url,int rc)
{
    int saved=errno;
    X4CatalogDiskStats after={0};
    if(cache)x4_catalog_disk_cache_stats(cache,&after);
    const char *reason=rc==1?"hit":!cache?"disabled":
        after.cancelled>before->cancelled?"cancelled":
        after.corrupt>before->corrupt?"corrupt":
        after.expired>before->expired?"expired":
        after.io_errors>before->io_errors?"io":
        after.misses>before->misses?(!before->records?"absent":"absent_or_clock_unavailable"):
        rc<0?"argument_or_io":"unclassified";
    printf("CACHE phase=%s operation=get kind=%s id=%s market=%s language=%s url_hash=%016llx rc=%d reason=%s\n",
        disk_phase,kind,id,market,language,(unsigned long long)trace_url_hash(url),rc,reason);
    errno=saved;
}
int __wrap_x4_catalog_disk_cache_get_metadata(X4CatalogDiskCache *cache,const char *id,
    const char *market,const char *language,X4ProductMetadata *metadata,const _Atomic int *cancel)
{
    X4CatalogDiskStats before={0};
    if(trace_disk&&cache)x4_catalog_disk_cache_stats(cache,&before);
    int rc=__real_x4_catalog_disk_cache_get_metadata(cache,id,market,language,metadata,cancel);
    if(trace_disk)trace_disk_get(cache,&before,"metadata",id,market,language,NULL,rc);
    return rc;
}
int __wrap_x4_catalog_disk_cache_get_image(X4CatalogDiskCache *cache,const char *id,
    const char *market,const char *language,const char *url,unsigned char *bytes,
    size_t capacity,size_t *length,const _Atomic int *cancel)
{
    X4CatalogDiskStats before={0};
    if(trace_disk&&cache)x4_catalog_disk_cache_stats(cache,&before);
    int rc=__real_x4_catalog_disk_cache_get_image(cache,id,market,language,url,bytes,capacity,length,cancel);
    if(trace_disk)trace_disk_get(cache,&before,"image",id,market,language,url,rc);
    return rc;
}
static void trace_disk_put(const char *kind,const char *id,const char *market,const char *language,
    const char *url,int rc,const _Atomic int *cancel)
{
    int saved=errno;
    printf("CACHE phase=%s operation=put kind=%s id=%s market=%s language=%s url_hash=%016llx rc=%d cancelled=%d errno=%d\n",
        disk_phase,kind,id,market,language,(unsigned long long)trace_url_hash(url),rc,
        cancel&&atomic_load(cancel),rc<0?saved:0);
    errno=saved;
}
int __wrap_x4_catalog_disk_cache_put_metadata(X4CatalogDiskCache *cache,const char *id,
    const char *market,const char *language,const X4ProductMetadata *metadata,const _Atomic int *cancel)
{
    int rc=__real_x4_catalog_disk_cache_put_metadata(cache,id,market,language,metadata,cancel);
    if(trace_disk)trace_disk_put("metadata",id,market,language,NULL,rc,cancel);
    return rc;
}
int __wrap_x4_catalog_disk_cache_put_image(X4CatalogDiskCache *cache,const char *id,
    const char *market,const char *language,const char *url,const unsigned char *bytes,
    size_t length,const _Atomic int *cancel)
{
    int rc=__real_x4_catalog_disk_cache_put_image(cache,id,market,language,url,bytes,length,cancel);
    if(trace_disk)trace_disk_put("image",id,market,language,url,rc,cancel);
    return rc;
}
static X4CatalogAssetStats stats(X4CatalogAssets *a)
{
    X4CatalogAssetStats s;
    /* A try-lock miss produces zeros; it is not an observed empty cache. */
    for(unsigned i=0;i<5000;++i){x4_catalog_assets_stats(a,&s);
        if(s.suspended||s.busy||s.requests||s.occupied||s.revision)return s;
        sleep_ms(1);
    }
    CHECK(0);return s;
}
static void wait_ready(X4CatalogAssets *a,unsigned ready)
{
    for(unsigned i=0;i<5000;++i){X4CatalogAssetStats s=stats(a);if(s.covers_ready>=ready && !s.pending && x4_catalog_assets_idle(a))return;sleep_ms(1);}
    CHECK(0);
}
static void wait_entered(void) { for(unsigned i=0;i<2000&&!atomic_load(&entered);++i)sleep_ms(1); CHECK(atomic_load(&entered)); }
static void wait_idle(X4CatalogAssets *a) { for(unsigned i=0;i<2000&&!x4_catalog_assets_idle(a);++i)sleep_ms(1);CHECK(x4_catalog_assets_idle(a)); }
static int wait_acquire(X4CatalogAssets *a,const char *id,X4CatalogAssetView *view)
{
    for(unsigned i=0;i<5000;++i){if(x4_catalog_assets_acquire(a,id,view))return 1;sleep_ms(1);}
    return 0;
}
static void wait_state(X4CatalogAssets *a,const char *id,enum X4CatalogAssetState state,X4CatalogAssetView *view)
{
    /* stats/acquire deliberately use try-lock. A zeroed unavailable stats
     * snapshot is not proof that the asynchronous request has completed. */
    for(unsigned i=0;i<5000;++i){
        if(x4_catalog_assets_acquire(a,id,view)){
            if(view->state==state)return;
            x4_catalog_assets_release(a,view);
        }
        sleep_ms(1);
    }
    CHECK(0);
}
static void wait_flag(const _Atomic int *flag)
{for(unsigned i=0;i<5000&&!atomic_load(flag);++i)sleep_ms(1);CHECK(atomic_load(flag));}
static void wait_at_least(const _Atomic unsigned *counter,unsigned target)
{
    for(unsigned i=0;i<5000&&atomic_load(counter)<target;++i)sleep_ms(1);
    CHECK(atomic_load(counter)>=target);
}
static void fake_reset(void)
{
    CHECK(!atomic_load(&active_http));CHECK(!atomic_load(&active_requests));
    CHECK(!atomic_load(&active_images));CHECK(!atomic_load(&active_decodes));
    atomic_store(&block_json,0);atomic_store(&block_image,0);atomic_store(&block_reset,0);
    atomic_store(&ignore_cancel,0);atomic_store(&entered,0);atomic_store(&image_entered,0);atomic_store(&reset_entered,0);
    atomic_store(&fake_missing,0);atomic_store(&fake_corrupt,0);atomic_store(&fake_partial_multi,0);atomic_store(&fake_single_missing,0);
    atomic_store(&fake_image_status,200);atomic_store(&fake_batch_limit,0);
    atomic_store(&fake_reset_error,0);
    atomic_store(&fake_no_fortnite_poster,0);
    atomic_store(&json_calls,0);atomic_store(&image_calls,0);atomic_store(&reset_calls,0);atomic_store(&open_calls,0);
    for(unsigned i=0;i<X4_SIGL_COUNT;++i)atomic_store(&sigl_calls[i],0);
    atomic_store(&fake_now_us,UINT64_C(10000000));atomic_store(&fake_retry_ms,0);
    atomic_store(&release_rate_head,0);atomic_store(&arm_rate_head,0);atomic_store(&block_decode,0);
    atomic_store(&peak_images,0);atomic_store(&image_arrivals,0);atomic_store(&image_cancellations,0);
    atomic_store(&peak_decodes,0);atomic_store(&decode_calls,0);
    atomic_store(&screenshot_bound_calls,0);
    atomic_store(&bound_screenshots,0);
    memset(metadata_order,0,sizeof(metadata_order));memset(image_order,0,sizeof(image_order));
    memset(image_queries, 0, sizeof(image_queries));
}
static uint64_t clock_us(void *context) {(void)context;return atomic_load(&fake_now_us);}
static X4CatalogAssets *create_with_clock(const char *root,X4AssetClock clock)
{
    X4CatalogAssetsConfig config={.disk_root=root,.clock_us=clock};
#ifndef X4_ASSETS_TEST_BASELINE31D
    config.image_downloaders=fixture_downloaders;
#endif
    X4CatalogAssets*a=x4_catalog_assets_create_config(&config);CHECK(a);CHECK(x4_catalog_assets_idle(a));return a;
}
static X4CatalogAssets *create_assets(const char *root)
{return create_with_clock(root,clock_us);}
static void close_assets(X4CatalogAssets*a)
{x4_catalog_assets_suspend(a);wait_idle(a);x4_catalog_assets_destroy(a);CHECK(!atomic_load(&active_http));}
static void request_grid(X4CatalogAssets*a,const char*const*ids,size_t count)
{
    uint32_t masks[X4_CATALOG_ASSET_SLOTS];
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)masks[i]=1u<<X4_COVER_GRID;
    x4_catalog_assets_request_surfaces(a,ids,masks,count);
}
static void request_resume(X4CatalogAssets*a,const char*const*ids,size_t count)
{request_grid(a,ids,count);x4_catalog_assets_resume(a);}
static void advance_ms(uint64_t milliseconds)
{CHECK(milliseconds<=UINT64_MAX/1000);atomic_fetch_add(&fake_now_us,milliseconds*1000);}
static void test_text(void)
{
    X4JsonSpan s;char out[128];const char *j="\"Árbol, niño, pingüino ¿sí? ¡Sí!\\n\\u00f1\"";
    CHECK(!x4_json_parse(j,strlen(j),&s));CHECK(!x4_json_display_text(s,out,sizeof(out)));CHECK(!strcmp(out,"Árbol, niño, pingüino ¿sí? ¡Sí! ñ"));
    CHECK(x4_json_token(s,out,sizeof(out))<0);CHECK(!out[0]);
    CHECK(!x4_json_parse("\"ñüa\"",strlen("\"ñüa\""),&s));CHECK(!x4_json_display_text(s,out,4));CHECK(!strcmp(out,"ñ"));
    CHECK(x4_json_parse("\"\\uD800\"",8,&s)<0);CHECK(x4_json_parse("\"\xc0\xaf\"",4,&s)<0);
}
static void test_policy_metadata(const char *fixture)
{
    size_t n;unsigned char *j=read_file(fixture,&n);X4ProductMetadata m;
    CHECK(!x4_product_metadata_parse((char*)j,n,"9NKX70BBCDRN",&m));CHECK(strstr(m.name,"Edición"));CHECK(!strcmp(m.publisher,"Xbox Game Studios"));CHECK(m.category[0] && m.description[0]);CHECK(x4_http_public_image_url(m.cover_url));
    CHECK(x4_product_metadata_parse((char*)j,n,"NOTFOUND",&m)<0);free(j);
    CHECK(!x4_product_id_valid("../bad"));CHECK(!x4_product_id_valid(""));CHECK(!x4_product_id_valid("ID\r\n"));CHECK(x4_product_id_valid("9nkx70bbcdrn"));
    char url[512];CHECK(x4_product_cover_url("//store-images.s-microsoft.com/image/apps.test-asset",url));CHECK(x4_http_public_image_url(url));
    const char *bad[]={"http://store-images.s-microsoft.com/image/apps.a","//store-images.s-microsoft.com.evil/image/apps.a","//evil/image/apps.a","//store-images.s-microsoft.com/image/apps.a?secret=1","//store-images.s-microsoft.com:443/image/apps.a","//user@store-images.s-microsoft.com/image/apps.a","//store-images.s-microsoft.com/image/apps.a/../b","//store-images.s-microsoft.com/image/apps.a%2fb"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i)CHECK(!x4_product_cover_url(bad[i],url));
    CHECK(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=512&q=80&format=jpg&token=x"));
    CHECK(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=384&h=216&q=80&format=jpg"));
    CHECK(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=385&h=216&q=85&format=jpg"));
    CHECK(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?h=216&w=384&q=85&format=jpg"));
    CHECK(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=384&h=216&q=85&format=png"));
    const char *missing="{\"Products\":[{\"ProductId\":\"ID1\",\"LocalizedProperties\":[{}]}]}";CHECK(!x4_product_metadata_parse(missing,strlen(missing),"ID1",&m));CHECK(!m.name[0]&&!m.cover_url[0]);
    const char *duplicate="{\"Products\":[{\"ProductId\":\"ID1\"},{\"ProductId\":\"ID1\"}]}";CHECK(x4_product_metadata_parse(duplicate,strlen(duplicate),"ID1",&m)<0);
}
static void test_images(const char *png)
{
    X4CoverRasters r;CHECK(!x4_cover_decode(fake_image,fake_image_length,&r));CHECK(r.surface[0].width==320&&r.surface[0].height==224);CHECK(r.surface[1].width==512&&r.surface[1].height==288);CHECK(r.surface[2].width==96&&r.surface[2].height==96);
    CHECK(r.surface[0].rgba[0]==0xff30261du);CHECK(r.surface[0].rgba[112*320+160]!=0xff30261du);x4_cover_rasters_free(&r);
    size_t n;unsigned char *b=read_file(png,&n);CHECK(!x4_cover_decode(b,n,&r));x4_cover_rasters_free(&r);
    CHECK(x4_cover_decode((unsigned char*)"corrupt",7,&r)<0);CHECK(x4_cover_decode(b,2,&r)<0);CHECK(x4_cover_decode(b,2u*1024u*1024u+1u,&r)<0);
    /* PNG IHDR dimension preflight: decoder never allocates its large pixels. */
    b[16]=0;b[17]=0;b[18]=0x10;b[19]=0x01;CHECK(x4_cover_decode(b,n,&r)<0);free(b);
}
static void test_color_layout(const char *fixture)
{
    size_t n;unsigned char *b=read_file(fixture,&n);X4CoverRasters r;
    CHECK(!x4_cover_decode(b,n,&r));
    for(unsigned i=0;i<=X4_COVER_RECENT;++i){
        X4CoverRaster *s=&r.surface[i];unsigned y=s->height/2;
        CHECK(s->rgba[(size_t)y*s->width]==0xff0000ffu); /* opaque red */
        CHECK(s->rgba[(size_t)y*s->width+s->width-1]==0x80ff0000u); /* half-alpha blue */
        CHECK(s->rgba[0]==0xff30261du); /* letterbox RGB #1d2630 */
    }
    x4_cover_rasters_free(&r);free(b);
}
static void test_priority_metadata_before_cover(void)
{
    fake_reset();atomic_store(&block_image,1);X4CatalogAssets*a=create_assets(NULL);
    const char*ids[]={"SELECTED9","VISIBLE2","VISIBLE3","ADJACENT4"};
    request_resume(a,ids,4);wait_flag(&image_entered);
    CHECK(!strcmp(metadata_order[0],"SELECTED9,VISIBLE2,VISIBLE3,ADJACENT4"));
    CHECK(!strcmp(image_order[0],"SELECTED9"));
    X4ProductMetadata m[24];uint64_t revision=0;
    size_t count=0;for(unsigned i=0;i<2000&&count!=4;++i){count=x4_catalog_assets_copy_metadata(a,m,24,&revision);if(count!=4)sleep_ms(1);}
    CHECK(count==4&&revision);CHECK(m[0].name[0]);
    X4CatalogAssetView v;CHECK(wait_acquire(a,"SELECTED9",&v));
    CHECK(v.metadata.name[0]&&!v.surface[0].rgba&&v.state==X4_ASSET_DOWNLOADING);
    /* Borrowing a metadata-only view must not make the worker throw away a
     * newly decoded immutable cover or burn all network retries. */
    atomic_store(&block_image,0);wait_ready(a,4);
    CHECK(!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)==1);CHECK(atomic_load(&image_calls)==4);
    CHECK(!strcmp(image_order[1],"VISIBLE2"));CHECK(!strcmp(image_order[3],"ADJACENT4"));
    X4AssetEvent events[X4_ASSET_EVENT_CAP];size_t n=0;
    for(unsigned i=0;i<2000&&!n;++i){n=x4_catalog_assets_events(a,events,X4_ASSET_EVENT_CAP);if(!n)sleep_ms(1);}
    CHECK(n&&n<=X4_ASSET_EVENT_CAP);unsigned stage_bits=0;
    for(size_t i=0;i<n;++i){CHECK(events[i].stage<X4_ASSET_STAGE_COUNT);CHECK(events[i].source<=X4_ASSET_NETWORK);
        CHECK(events[i].end_us>=events[i].start_us);CHECK(x4_product_id_valid(events[i].product_id));stage_bits|=1u<<events[i].stage;}
    CHECK(stage_bits==(1u<<X4_ASSET_STAGE_COUNT)-1u);close_assets(a);
}
static void test_batches_large_and_partial(void)
{
    const char*ids[]={"ID1","ID2","ID3","ID4"};
    fake_reset();atomic_store(&fake_batch_limit,1);X4CatalogAssets*a=create_assets(NULL);
    request_resume(a,ids,4);wait_ready(a,4);CHECK(stats(a).batch_splits>=1);CHECK(atomic_load(&json_calls)>4);
    CHECK(atomic_load(&image_calls)==4);close_assets(a);
    fake_reset();atomic_store(&fake_partial_multi,1);a=create_assets(NULL);
    request_resume(a,ids,4);wait_ready(a,4);CHECK(stats(a).partial_responses>=1);
    CHECK(atomic_load(&json_calls)>=2);CHECK(atomic_load(&image_calls)==4);close_assets(a);
    fake_reset();atomic_store(&fake_partial_multi,1);atomic_store(&fake_single_missing,1);a=create_assets(NULL);
    request_resume(a,ids,4);X4CatalogAssetView v;wait_state(a,"ID2",X4_ASSET_TEMPORARY_FAILURE,&v);
    CHECK(!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    wait_state(a,"ID1",X4_ASSET_READY,&v);CHECK(v.metadata.name[0]);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)<=5);close_assets(a);
}
static void test_navigation_defers_obsolete_next_stage(void)
{
    fake_reset();atomic_store(&block_json,1);X4CatalogAssets*a=create_assets(NULL);
    const char*old[]={"OLD1","SHARED"};request_resume(a,old,2);wait_entered();
    const char*now[]={"SHARED","FRESH"};request_grid(a,now,2);
    atomic_store(&block_json,0);wait_ready(a,2);CHECK(!strcmp(image_order[0],"SHARED"));
    for(unsigned i=0;i<atomic_load(&image_calls);++i)CHECK(strcmp(image_order[i],"OLD1"));
    CHECK(stats(a).deferred_stages>=1);X4CatalogAssetView v;wait_state(a,"SHARED",X4_ASSET_READY,&v);
    CHECK(v.surface[0].rgba);x4_catalog_assets_release(a,&v);close_assets(a);
}
static void test_retry_after_and_manual_retry(void)
{
    fake_reset();atomic_store(&fake_image_status,503);atomic_store(&fake_retry_ms,2000);
    X4CatalogAssets*a=create_assets(NULL);const char*ids[]={"RETRY1"};request_resume(a,ids,1);
    X4CatalogAssetView v;wait_state(a,"RETRY1",X4_ASSET_TEMPORARY_FAILURE,&v);
    CHECK(v.metadata.name[0]&&!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)==1&&atomic_load(&image_calls)==1);
    atomic_store(&fake_image_status,200);x4_catalog_assets_retry(a);advance_ms(1999);sleep_ms(50);
    CHECK(atomic_load(&image_calls)==1);advance_ms(1);wait_ready(a,1);
    CHECK(atomic_load(&image_calls)==2);CHECK(atomic_load(&json_calls)==1);CHECK(stats(a).retries>=1);close_assets(a);
    /* A corrupt image is temporary, with bounded retries. A manual retry
     * after exhaustion reuses already-published metadata. */
    fake_reset();atomic_store(&fake_corrupt,1);a=create_assets(NULL);request_resume(a,ids,1);
    for(unsigned attempt=1;attempt<=3;++attempt){
        for(unsigned i=0;i<2000&&atomic_load(&image_calls)<attempt;++i)sleep_ms(1);
        CHECK(atomic_load(&image_calls)==attempt);wait_state(a,"RETRY1",X4_ASSET_TEMPORARY_FAILURE,&v);x4_catalog_assets_release(a,&v);
        if(attempt<3)advance_ms(attempt==1?1000:4000);
    }
    advance_ms(100000);sleep_ms(50);CHECK(atomic_load(&image_calls)==3);
    atomic_store(&fake_corrupt,0);x4_catalog_assets_retry(a);wait_ready(a,1);
    CHECK(atomic_load(&image_calls)==4&&atomic_load(&json_calls)==1);close_assets(a);
    /* Invalid/not-found metadata is permanent rather than a retry storm. */
    fake_reset();atomic_store(&fake_missing,1);a=create_assets(NULL);request_resume(a,ids,1);
    wait_state(a,"RETRY1",X4_ASSET_MISSING,&v);CHECK(!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    advance_ms(100000);x4_catalog_assets_retry(a);sleep_ms(50);CHECK(atomic_load(&json_calls)==1&&!atomic_load(&image_calls));close_assets(a);
}
static void test_ram_eviction_pinning_generation(void)
{
    fake_reset();X4CatalogAssets*a=create_assets(NULL);sleep_ms(20);CHECK(!atomic_load(&json_calls));
    const char*ids[]={"ID1","ID2","../bad","ID1"};x4_catalog_assets_request(a,ids,4);
    CHECK(!atomic_load(&json_calls));x4_catalog_assets_resume(a);wait_ready(a,2);
    X4CatalogAssetView v;CHECK(wait_acquire(a,"ID1",&v));CHECK(v.surface[0].rgba);CHECK(stats(a).pinned==1);
    x4_catalog_assets_release(a,&v);CHECK(stats(a).pinned==0);
    char names[30][32];const char*many[30];for(unsigned i=0;i<30;++i){snprintf(names[i],32,"PROD%u",i);many[i]=names[i];}
    x4_catalog_assets_request(a,many,30);wait_ready(a,26);X4CatalogAssetStats s=stats(a);
    CHECK(s.occupied==24);CHECK(s.thumbnail_bytes==24u*(356u*200u+524u*295u+96u*96u)*4u);
    CHECK(s.thumbnail_bytes<X4_CATALOG_THUMBNAIL_BUDGET);CHECK(s.events_omitted>0);
    CHECK(!x4_catalog_assets_acquire(a,"PROD29",&v));
    CHECK(wait_acquire(a,"PROD0",&v));uint32_t*held=v.surface[0].rgba;
    uint32_t row_masks[X4_CATALOG_ASSET_SLOTS];
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)row_masks[i]=1u<<X4_COVER_ROW;
    x4_catalog_assets_request_surfaces(a,many,row_masks,X4_CATALOG_ASSET_SLOTS);
    wait_ready(a,26);s=stats(a);
    CHECK(s.thumbnail_bytes<=X4_CATALOG_THUMBNAIL_BUDGET);
    CHECK(s.thumbnail_bytes<24u*(356u*200u+524u*295u+96u*96u+384u*216u)*4u);
    printf("asset_memory thumbnails_bytes=%zu limit_bytes=%u products=%u\n",
        s.thumbnail_bytes,X4_CATALOG_THUMBNAIL_BUDGET,s.occupied);
    X4CatalogAssetView row;CHECK(wait_acquire(a,"PROD23",&row));
    CHECK(row.surface[X4_COVER_ROW].width==384&&row.surface[X4_COVER_ROW].height==216);
    x4_catalog_assets_release(a,&row);
    const char*other[]={"OTHER"};x4_catalog_assets_request(a,other,1);wait_ready(a,27);
    CHECK(held[100*356+178]!=0xff30261du);x4_catalog_assets_release(a,&v);
    CHECK(wait_acquire(a,"OTHER",&v));held=v.surface[0].rgba;
    x4_catalog_assets_suspend(a);wait_idle(a);unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    x4_catalog_assets_set_generation(a,2);X4CatalogAssetView current;CHECK(!x4_catalog_assets_acquire(a,"OTHER",&current));
    x4_catalog_assets_request(a,other,1);CHECK(held[100*356+178]!=0xff30261du);
    x4_catalog_assets_release(a,&v);x4_catalog_assets_resume(a);wait_state(a,"OTHER",X4_ASSET_READY,&current);
    CHECK(current.generation==2);x4_catalog_assets_release(a,&current);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);close_assets(a);
}
static void wait_background(X4CatalogAssets *a,const char *id,X4CatalogBackgroundView *view)
{
    for(unsigned i=0;i<5000;++i){
        if(x4_catalog_assets_acquire_background(a,view)){
            if(!strcmp(view->product_id,id))return;
            x4_catalog_assets_release_background(a,view);
        }
        sleep_ms(1);
    }
    CHECK(0);
}
static void test_two_backgrounds_and_pins(void)
{
    fake_reset();X4CatalogAssets *a=create_assets(NULL);
    x4_catalog_assets_request_background(a,"BACK1");
    x4_catalog_assets_resume(a);
    X4CatalogBackgroundView one,two,three;
    wait_background(a,"BACK1",&one);
    CHECK(one.raster.width==1920&&one.raster.height==1080&&one.raster.rgba);
    uint32_t held=one.raster.rgba[540u*1920u+960u];
    x4_catalog_assets_request_background(a,"BACK2");
    wait_background(a,"BACK2",&two);
    X4CatalogAssetStats s=stats(a);
    CHECK(s.background_count==2&&s.background_bytes==2u*1920u*1080u*4u&&!s.thumbnail_bytes);
    printf("asset_memory backgrounds=%u bytes=%zu\n",s.background_count,s.background_bytes);
    x4_catalog_assets_request_background(a,"BACK3");
    sleep_ms(50);
    CHECK(atomic_load(&image_calls)==2);
    wait_background(a,"BACK2",&three);
    CHECK(three.raster.rgba==two.raster.rgba&&one.raster.rgba[540u*1920u+960u]==held);
    x4_catalog_assets_release_background(a,&three);
    x4_catalog_assets_release_background(a,&one);
    wait_background(a,"BACK3",&three);
    CHECK(stats(a).background_count==2&&atomic_load(&image_calls)==3);
    CHECK(three.raster.rgba!=two.raster.rgba);
    x4_catalog_assets_request_background(a,"BACK2");
    wait_background(a,"BACK2",&one);
    CHECK(one.raster.rgba==two.raster.rgba&&atomic_load(&image_calls)==3);
    x4_catalog_assets_release_background(a,&one);
    x4_catalog_assets_release_background(a,&three);
    x4_catalog_assets_release_background(a,&two);
    x4_catalog_assets_set_generation(a,2);
    CHECK(!x4_catalog_assets_acquire_background(a,&one));
    close_assets(a);
}
static void wait_sigls(X4CatalogAssets *a,X4SiglList out[X4_SIGL_COUNT],uint64_t *revision)
{
    for(unsigned i=0;i<5000;++i){
        (void)x4_catalog_assets_copy_sigls(a,out,revision);
        if(!out[X4_SIGL_LEAVING].pending&&out[X4_SIGL_FREE].valid)return;
        sleep_ms(1);
    }
    CHECK(0);
}
static void wait_sigl_change(X4CatalogAssets *a,X4SiglList out[X4_SIGL_COUNT],uint64_t *revision)
{
    for(unsigned i=0;i<5000;++i){
        if(x4_catalog_assets_copy_sigls(a,out,revision))return;
        sleep_ms(1);
    }
    CHECK(0);
}
static void test_one_sigl_per_account(void)
{
    fake_reset();X4CatalogAssets *a=create_assets(NULL);
    X4SiglList *out=calloc(X4_SIGL_COUNT,sizeof(*out));CHECK(out);
    uint64_t revision=0;
    x4_catalog_assets_request_sigls(a,10);
    wait_sigl_change(a,out,&revision);
    for(unsigned i=0;i<X4_SIGL_COUNT;++i)CHECK(out[i].pending&&!out[i].valid);
    x4_catalog_assets_resume(a);
    wait_sigls(a,out,&revision);
    for(unsigned i=0;i<X4_SIGL_COUNT;++i){
        CHECK(atomic_load(&sigl_calls[i])==1&&!out[i].pending);
        CHECK(out[i].valid==(i!=X4_SIGL_LEAVING));
        if(out[i].valid)CHECK(out[i].count==1&&!strcmp(out[i].ids[0],"CARD0"));
    }
    x4_catalog_assets_set_generation(a,2);
    x4_catalog_assets_request_sigls(a,10);
    sleep_ms(30);
    CHECK(!x4_catalog_assets_copy_sigls(a,out,&revision));
    CHECK(stats(a).sigl_requests==4);
    x4_catalog_assets_suspend(a);wait_idle(a);
    x4_catalog_assets_request_sigls(a,0);
    wait_sigl_change(a,out,&revision);
    for(unsigned i=0;i<X4_SIGL_COUNT;++i)CHECK(!out[i].pending&&!out[i].valid&&!out[i].count);
    x4_catalog_assets_request_sigls(a,11);
    wait_sigl_change(a,out,&revision);
    x4_catalog_assets_resume(a);wait_sigls(a,out,&revision);
    for(unsigned i=0;i<X4_SIGL_COUNT;++i)CHECK(atomic_load(&sigl_calls[i])==2);
    close_assets(a);free(out);
}
static void test_old_account_result_and_cancel(void)
{
    fake_reset();atomic_store(&block_json,1);atomic_store(&ignore_cancel,1);
    X4CatalogAssets*a=create_assets(NULL);const char*old[]={"OLDGEN"};request_resume(a,old,1);wait_entered();
    CHECK(!x4_catalog_assets_idle(a));x4_catalog_assets_set_generation(a,2);
    const char*fresh[]={"FRESHGEN"};request_grid(a,fresh,1);atomic_store(&block_json,0);
    wait_ready(a,1);atomic_store(&ignore_cancel,0);CHECK(stats(a).stale_results>=1);
    X4CatalogAssetView v;CHECK(!x4_catalog_assets_acquire(a,"OLDGEN",&v));wait_state(a,"FRESHGEN",X4_ASSET_READY,&v);
    CHECK(v.generation==2);x4_catalog_assets_release(a,&v);
    atomic_store(&block_json,1);atomic_store(&entered,0);const char*cancelled[]={"CANCELME"};
    request_grid(a,cancelled,1);wait_entered();x4_catalog_assets_suspend(a);wait_idle(a);
    CHECK(stats(a).cancelled>=1);atomic_store(&block_json,0);close_assets(a);
}
static void test_auth_boundary_waits_for_cleanup(void)
{
    fake_reset();atomic_store(&block_image,1);X4CatalogAssets*a=create_assets(NULL);
    const char*ids[]={"AUTHBOUNDARY"};request_resume(a,ids,1);wait_flag(&image_entered);
    atomic_store(&block_reset,1);x4_catalog_assets_suspend(a);wait_flag(&reset_entered);
    CHECK(!x4_catalog_assets_idle(a));CHECK(!atomic_load(&active_requests));
    sleep_ms(30);CHECK(!x4_catalog_assets_idle(a));
    /* The foreground login/session may begin only after this cleanup
     * contract. This is a local gate test, not a Microsoft login. */
    atomic_store(&block_reset,0);wait_idle(a);CHECK(atomic_load(&reset_calls)>=1);
    unsigned images=atomic_load(&image_calls);sleep_ms(30);CHECK(atomic_load(&image_calls)==images);
    atomic_store(&block_image,0);close_assets(a);
}
static void cache_directory_action(const char*root,int corrupt)
{
    CHECK(!strncmp(root,"/tmp/x4-assets-cache.",21));DIR*d=opendir(root);CHECK(d);struct dirent*entry;
    while((entry=readdir(d))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        CHECK(!strncmp(entry->d_name,"x4c-",4));char path[512];CHECK(snprintf(path,sizeof(path),"%s/%s",root,entry->d_name)<(int)sizeof(path));
        if(corrupt){FILE*f=fopen(path,"wb");CHECK(f);CHECK(fwrite("corrupt",1,7,f)==7);CHECK(!fclose(f));}
        else CHECK(!unlink(path));
    }
    CHECK(!closedir(d));
}
typedef struct { unsigned metadata,image; } GetCounts;
static GetCounts test_disk_reopen(const char *root)
{
    fake_reset();
    disk_phase="cold";
    const char*ids[]={"CACHE1","CACHE2"};X4CatalogAssets*a=create_assets(root);request_resume(a,ids,2);wait_ready(a,2);close_assets(a);
    GetCounts cold={atomic_load(&json_calls),atomic_load(&image_calls)};
    if(trace_disk)printf("CACHE phase=cold metadata_get=%u image_get=%u\n",cold.metadata,cold.image);
    atomic_store(&open_calls,0);
    disk_phase="warm";
    a=create_assets(root);x4_catalog_assets_set_generation(a,2);request_resume(a,ids,2);wait_ready(a,2);
    if(trace_disk){
        X4CatalogAssetStats s=stats(a);
        printf("CACHE phase=warm metadata_get_delta=%u image_get_delta=%u disk_metadata=%llu disk_image=%llu http_open=%u\n",
            atomic_load(&json_calls)-cold.metadata,atomic_load(&image_calls)-cold.image,
            (unsigned long long)s.disk_metadata_hits,(unsigned long long)s.disk_image_hits,atomic_load(&open_calls));
    }
    CHECK(atomic_load(&json_calls)==cold.metadata&&atomic_load(&image_calls)==cold.image);
    CHECK(!atomic_load(&open_calls));
    CHECK(stats(a).disk_metadata_hits==2&&stats(a).disk_image_hits==2);close_assets(a);
    return cold;
}
static void test_disk_corrupt_fallback(const char *root,GetCounts cold)
{
    disk_phase="corrupt_fallback";
    const char*ids[]={"CACHE1","CACHE2"};
    cache_directory_action(root,1);X4CatalogAssets *a=create_assets(root);request_resume(a,ids,2);wait_ready(a,2);
    if(trace_disk)printf("CACHE phase=corrupt_fallback metadata_get_delta=%u image_get_delta=%u\n",
        atomic_load(&json_calls)-cold.metadata,atomic_load(&image_calls)-cold.image);
    CHECK(atomic_load(&json_calls)>cold.metadata&&atomic_load(&image_calls)>cold.image);close_assets(a);
}
static void test_disk_reopen_and_corrupt_fallback(void)
{
    char root[]="/tmp/x4-assets-cache.XXXXXX";CHECK(mkdtemp(root));
    GetCounts cold=test_disk_reopen(root);
    test_disk_corrupt_fallback(root,cold);
    cache_directory_action(root,0);CHECK(!rmdir(root));
}
static X4CatalogAssets *create_realtime_assets(const char*root)
{return create_with_clock(root,NULL);}
static uint64_t wall_monotonic_us(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*UINT64_C(1000000)+(uint64_t)t.tv_nsec/1000u;
}
static void test_cleanup_fault_is_fail_closed_and_destroyable(void)
{
    fake_reset();X4CatalogAssets*a=create_assets(NULL);const char*ids[]={"FAULTCHECK"};
    request_resume(a,ids,1);wait_ready(a,1);CHECK(x4_catalog_assets_fault(a)==X4_ASSET_FAULT_NONE);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls),opens=atomic_load(&open_calls);
    atomic_store(&fake_reset_error,X4_HTTP_UNUSABLE);x4_catalog_assets_suspend(a);
    for(unsigned i=0;i<5000&&x4_catalog_assets_fault(a)!=X4_ASSET_FAULT_CLEANUP;++i)sleep_ms(1);
    CHECK(x4_catalog_assets_fault(a)==X4_ASSET_FAULT_CLEANUP);
    CHECK(!x4_catalog_assets_idle(a)&&stats(a).cleanup_failures==1);
    x4_catalog_assets_resume(a);const char*later[]={"AFTERFAULT"};x4_catalog_assets_request(a,later,1);sleep_ms(50);
    CHECK(x4_catalog_assets_fault(a)==X4_ASSET_FAULT_CLEANUP);
    CHECK(!x4_catalog_assets_idle(a)&&stats(a).cleanup_failures==1);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images&&atomic_load(&open_calls)==opens);
    uint64_t start=wall_monotonic_us();x4_catalog_assets_destroy(a);uint64_t end=wall_monotonic_us();
    CHECK(end-start<UINT64_C(2000000));CHECK(!atomic_load(&active_http)&&!atomic_load(&active_requests));
}
static void test_lowercase_disk_reopen_preserves_job_identity(void)
{
    fake_reset();char root[]="/tmp/x4-assets-cache.XXXXXX";CHECK(mkdtemp(root));
    const char*ids[]={"lowercase1"};X4CatalogAssets*a=create_realtime_assets(root);
    request_resume(a,ids,1);wait_ready(a,1);close_assets(a);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    atomic_store(&open_calls,0);
    a=create_realtime_assets(root);x4_catalog_assets_set_generation(a,2);request_resume(a,ids,1);wait_ready(a,1);
    X4CatalogAssetView v;wait_state(a,"lowercase1",X4_ASSET_READY,&v);
    CHECK(!strcmp(v.metadata.product_id,"lowercase1"));CHECK(v.generation==2&&v.surface[0].rgba);
    x4_catalog_assets_release(a,&v);CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    CHECK(!atomic_load(&open_calls));
    CHECK(stats(a).disk_metadata_hits==1&&stats(a).disk_image_hits==1&&!stats(a).busy);
    /* Completing a cached, normalized metadata record must clear its job's
     * busy bit. A subsequent unrelated job verifies the worker can progress. */
    const char*following[]={"following2"};request_grid(a,following,1);wait_ready(a,2);
    wait_state(a,"following2",X4_ASSET_READY,&v);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)==queries+1&&atomic_load(&image_calls)==images+1);close_assets(a);
    cache_directory_action(root,0);CHECK(!rmdir(root));
}
static void measure_page(X4CatalogAssets*a,const char*scenario,unsigned page,
    const char*const*ids,unsigned count,unsigned covers)
{
    X4CatalogAssetStats before=stats(a);uint64_t start=wall_monotonic_us();
    unsigned open_before=atomic_load(&open_calls);
    x4_catalog_assets_request(a,ids,count);x4_catalog_assets_resume(a);wait_ready(a,covers);
    for(unsigned i=0;i<count;++i){X4CatalogAssetView v;wait_state(a,ids[i],X4_ASSET_READY,&v);
        CHECK(v.surface[0].rgba&&v.surface[1].rgba&&v.surface[2].rgba);x4_catalog_assets_release(a,&v);}
    uint64_t end=wall_monotonic_us();X4CatalogAssetStats after=stats(a);
    static const char*stage_names[]={"queue","metadata","image","decode_scale","publish"};
    printf("MEASUREMENT {\"scenario\":\"%s\",\"environment\":\"host_fake_public_http\",\"page\":%u,"
        "\"visible_cards\":6,\"demanded_cards\":%u,\"covers_ready_cumulative\":%llu,\"wall_monotonic_us\":%llu,"
        "\"http_open_delta\":%u,\"metadata_request_delta\":%llu,\"image_request_delta\":%llu,\"ram_hit_delta\":%llu,"
        "\"disk_metadata_hit_delta\":%llu,\"disk_image_hit_delta\":%llu,\"events_omitted_delta\":%llu,\"stage_delta\":[",
        scenario,page,count,(unsigned long long)after.covers_ready,(unsigned long long)(end-start),
        atomic_load(&open_calls)-open_before,
        (unsigned long long)(after.metadata_network_requests-before.metadata_network_requests),
        (unsigned long long)(after.image_network_requests-before.image_network_requests),
        (unsigned long long)(after.ram_hits-before.ram_hits),
        (unsigned long long)(after.disk_metadata_hits-before.disk_metadata_hits),
        (unsigned long long)(after.disk_image_hits-before.disk_image_hits),
        (unsigned long long)(after.events_omitted-before.events_omitted));
    for(unsigned i=0;i<X4_ASSET_STAGE_COUNT;++i)
        printf("%s{\"stage\":\"%s\",\"count\":%llu,\"total_us\":%llu,\"max_cumulative_us\":%llu}",i?",":"",stage_names[i],
            (unsigned long long)(after.stage[i].count-before.stage[i].count),
            (unsigned long long)(after.stage[i].total_us-before.stage[i].total_us),
            (unsigned long long)after.stage[i].max_us);
    puts("]}");
}
static void test_six_card_loading_comparison(void)
{
    fake_reset();char root[]="/tmp/x4-assets-cache.XXXXXX";CHECK(mkdtemp(root));
    char product[12][32];const char*ids[12];for(unsigned i=0;i<12;++i){snprintf(product[i],32,"CARD%u",i);ids[i]=product[i];}
    X4CatalogAssets*a=create_realtime_assets(root);
    measure_page(a,"cold_page",1,ids,6,6);CHECK(atomic_load(&json_calls)==2&&atomic_load(&image_calls)==18);
    measure_page(a,"preload_adjacent",1,ids,12,12);CHECK(atomic_load(&json_calls)==4&&atomic_load(&image_calls)==36);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    measure_page(a,"preloaded_page",2,ids+6,6,12);CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    measure_page(a,"revisit_page",1,ids,6,12);CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    close_assets(a);atomic_store(&open_calls,0);a=create_realtime_assets(root);x4_catalog_assets_set_generation(a,2);
    measure_page(a,"reopen_app",1,ids,6,6);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    CHECK(!atomic_load(&open_calls));
    CHECK(stats(a).disk_metadata_hits==6&&stats(a).disk_image_hits==18);close_assets(a);
    cache_directory_action(root,0);CHECK(!rmdir(root));
}
static void request_parallel(X4CatalogAssets *a,const char *const *ids,size_t count)
{
    /* Demand submission is intentionally a try-lock. Retry from the test
     * driver just as the next main-loop frame would. */
    X4CatalogAssetView v;
    for(unsigned i=0;i<5000;++i){
        request_grid(a,ids,count);
        if(x4_catalog_assets_acquire(a,ids[count-1],&v)){
            x4_catalog_assets_release(a,&v);
            x4_catalog_assets_resume(a);
            return;
        }
        sleep_ms(1);
    }
    CHECK(0);
}
static void test_parallel_dispatch_and_decode(void)
{
    fake_reset();
    atomic_store(&block_image,1);
    atomic_store(&block_decode,1);
    const char *ids[]={"SELECTED9","VISIBLE2","VISIBLE3","ADJACENT4"};
    X4CatalogAssets *a=create_assets(NULL);
    request_parallel(a,ids,4);
    wait_at_least(&image_arrivals,3);
    CHECK(atomic_load(&active_images)==3&&atomic_load(&peak_images)==3);
    CHECK(atomic_load(&image_calls)==3&&!atomic_load(&decode_calls));
    CHECK(!strcmp(metadata_order[0],"SELECTED9,VISIBLE2,VISIBLE3,ADJACENT4"));
    for(unsigned i=0;i<3;++i){
        unsigned matches=0;
        for(unsigned j=0;j<3;++j)matches+=!strcmp(ids[i],image_order[j]);
        CHECK(matches==1);
    }
    X4AssetEvent events[X4_ASSET_EVENT_CAP];size_t n=0;
    for(unsigned i=0;i<2000&&!n;++i){n=x4_catalog_assets_events(a,events,X4_ASSET_EVENT_CAP);if(!n)sleep_ms(1);}
    const char *dispatched[3]={0};unsigned found=0;
    for(size_t i=n;i>0&&found<3;--i){
        if(events[i-1].stage!=X4_ASSET_QUEUE)continue;
        dispatched[2-found]=events[i-1].product_id;
        ++found;
    }
    CHECK(found==3);
    for(unsigned i=0;i<3;++i)CHECK(!strcmp(dispatched[i],ids[i]));
    atomic_store(&block_image,0);
    wait_at_least(&decode_calls,1);
    sleep_ms(50);
    CHECK(atomic_load(&decode_calls)==1&&atomic_load(&active_decodes)==1);
    atomic_store(&block_decode,0);
    wait_ready(a,4);
    CHECK(atomic_load(&decode_calls)==4&&atomic_load(&peak_decodes)==1);
    CHECK(atomic_load(&image_calls)==4&&!strcmp(image_order[3],ids[3]));
#ifndef X4_ASSETS_TEST_BASELINE31D
    X4CatalogAssetStats s=stats(a);
    CHECK(s.image_download_requests[0]&&s.image_download_requests[1]&&s.image_download_requests[2]);
    CHECK(s.image_download_requests[0]+s.image_download_requests[1]+s.image_download_requests[2]==4);
#endif
    close_assets(a);
    puts("PASS parallel covers: demand-order dispatch, three simultaneous GETs, one decoder");
}
static void test_parallel_cancel(int account_change)
{
    fake_reset();
    atomic_store(&block_image,1);
    /* Allow a late success even after all workers observed cancellation. */
    atomic_store(&ignore_cancel,1);
    const char *old[]={"OLDONE","OLDTWO","OLDTHREE"};
    const char *fresh[]={"NEWONE","NEWTWO","NEWTHREE"};
    X4CatalogAssets *a=create_assets(NULL);
    if(account_change)x4_catalog_assets_request_sigls(a,10);
    request_parallel(a,old,3);
    wait_at_least(&image_arrivals,3);
    CHECK(!x4_catalog_assets_idle(a));
    if(account_change)x4_catalog_assets_request_sigls(a,0);
    else x4_catalog_assets_set_generation(a,2);
    wait_at_least(&image_cancellations,3);
    CHECK(atomic_load(&active_images)==3&&!x4_catalog_assets_idle(a));
    atomic_store(&block_image,0);
    wait_idle(a);
    CHECK(!atomic_load(&active_http)&&!atomic_load(&active_requests));
    CHECK(atomic_load(&image_calls)==3&&!atomic_load(&decode_calls)&&stats(a).covers_ready==0);
    if(account_change){
        x4_catalog_assets_set_generation(a,2);
        x4_catalog_assets_request_sigls(a,11);
    }
    atomic_store(&ignore_cancel,0);
    request_parallel(a,fresh,3);
    wait_ready(a,3);
    for(unsigned i=0;i<3;++i){
        X4CatalogAssetView v;
        CHECK(!x4_catalog_assets_acquire(a,old[i],&v));
        wait_state(a,fresh[i],X4_ASSET_READY,&v);
        CHECK(v.generation==2&&v.surface[X4_COVER_GRID].rgba);
        x4_catalog_assets_release(a,&v);
    }
    CHECK(atomic_load(&decode_calls)==3);
    close_assets(a);
    printf("PASS parallel covers: three cancelled by %s, late results discarded, new demand ready\n",
        account_change?"account handoff":"generation change");
}
static void test_parallel_retry_after(void)
{
    fake_reset();
    atomic_store(&block_image,1);
    atomic_store(&arm_rate_head,1);
    atomic_store(&fake_retry_ms,2000);
    const char *ids[]={"RATEHEAD","VISIBLE2","VISIBLE3","AFTERWAIT"};
    X4CatalogAssets *a=create_assets(NULL);
    request_parallel(a,ids,4);
    wait_at_least(&image_arrivals,3);
    atomic_store(&release_rate_head,1);
    X4CatalogAssetView v;wait_state(a,ids[0],X4_ASSET_TEMPORARY_FAILURE,&v);
    x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&image_calls)==3&&atomic_load(&active_images)==2);
    atomic_store(&block_image,0);
    advance_ms(1999);
    sleep_ms(70);
    CHECK(atomic_load(&image_calls)==3);
    advance_ms(1);
    wait_ready(a,4);
    CHECK(atomic_load(&image_calls)==5&&atomic_load(&decode_calls)==4);
    close_assets(a);
    puts("PASS parallel covers: Retry-After blocks all new GETs until the shared deadline");
}
static void test_parallel_assets(void)
{
    fixture_downloaders=3;
    test_parallel_dispatch_and_decode();
    test_parallel_cancel(0);
    test_parallel_cancel(1);
    test_parallel_retry_after();
    test_ram_eviction_pinning_generation();
    test_two_backgrounds_and_pins();
    puts("PASS parallel covers: LRU limit and two pinned backgrounds");
}
static void test_separate_cache_process(const char *root,int warm)
{
    fixture_downloaders=3;
    fake_reset();
    const char *ids[]={"CACHEONE","CACHETWO","CACHETHREE"};
    X4CatalogAssets *a=create_assets(root);
    request_parallel(a,ids,3);
    wait_ready(a,3);
    X4CatalogAssetStats s=stats(a);
    CHECK(s.disk_enabled&&s.covers_ready==3&&atomic_load(&decode_calls)==3);
    unsigned metadata=atomic_load(&json_calls),images=atomic_load(&image_calls);
    if(warm){
        CHECK(!metadata&&!images&&!atomic_load(&open_calls));
        CHECK(s.disk_metadata_hits==3&&s.disk_image_hits==3);
    }else{
        CHECK(metadata==1&&images==3&&!s.disk_metadata_hits&&!s.disk_image_hits);
    }
    close_assets(a);
    printf("PASS cache %s pid=%ld disk_metadata=%llu disk_image=%llu metadata_get=%u image_get=%u\n",
        warm?"warm":"cold",(long)getpid(),(unsigned long long)s.disk_metadata_hits,
        (unsigned long long)s.disk_image_hits,metadata,images);
}
#ifndef X4_ASSETS_TEST_BASELINE31D
enum { LARGE_WARM_PRODUCTS = 12, LARGE_TEST_PRODUCTS = 14, LARGE_NOISE_IMAGES = 520 };
static void large_warm_fixture(X4CatalogArtDemand art[LARGE_TEST_PRODUCTS])
{
    strcpy(art[0].id,X4_FORTNITE_PRODUCT_ID);
    art[0].mask=1u<<X4_COVER_HOME_FORTNITE;
    strcpy(art[1].id,"FEATURED40");
    art[1].mask=(1u<<X4_COVER_HOME_FEATURE)|(1u<<X4_COVER_BACKGROUND);
    for(unsigned i=2;i<LARGE_WARM_PRODUCTS;++i){
        snprintf(art[i].id,sizeof(art[i].id),"ROW40%u",i);
        art[i].mask=1u<<X4_COVER_ROW;
    }
    strcpy(art[LARGE_WARM_PRODUCTS].id,"MISSINGMETADATA40");
    art[LARGE_WARM_PRODUCTS].mask=1u<<X4_COVER_ROW;
    strcpy(art[LARGE_WARM_PRODUCTS+1].id,"MISSINGIMAGE40");
    art[LARGE_WARM_PRODUCTS+1].mask=1u<<X4_COVER_ROW;
}
static X4ProductMetadata large_metadata(const char *id)
{
    X4ProductMetadata m={0};
    char uri[128];
    int n=snprintf(uri,sizeof(uri),"//store-images.s-microsoft.com/image/apps.test-asset-%s",id);
    CHECK(n>0&&(size_t)n<sizeof(uri));
    snprintf(m.product_id,sizeof(m.product_id),"%s",id);
    snprintf(m.name,sizeof(m.name),"Cover %s",id);
    CHECK(x4_product_cover_url(uri,m.cover_url));
    memcpy(m.hero_url,m.cover_url,sizeof(m.hero_url));
    return m;
}
static const char *public_fixture_name(unsigned i,enum X4CoverSurface surface)
{
    static const char *const row_images[]={
        "9N0QWXC09MPT-384x216.jpg","9N21ZCV0JW6V-384x216.jpg","9N36BLQF3FX6-384x216.jpg",
        "9N4PT8HGCDHQ-384x216.jpg","9N7271QN4SGB-384x216.jpg","9N76QGDS5GJR-384x216.jpg",
        "9NBLGGH4PBBM-384x216.jpg","9NGP0DJVKSFL-384x216.jpg","9NKX70BBCDRN-384x216.jpg",
        "9NMNZ4CK59DM-384x216.jpg"
    };
    switch(surface){
    case X4_COVER_HOME_FORTNITE:return "BT5P2X999VH2-796x448.jpg";
    case X4_COVER_HOME_FEATURE:return "9NKX70BBCDRN-852x480.jpg";
    case X4_COVER_BACKGROUND:return "9NKX70BBCDRN-1920x1080.jpg";
    case X4_COVER_ROW:CHECK(i>=2&&i-2<sizeof(row_images)/sizeof(row_images[0]));return row_images[i-2];
    default:CHECK(0);return NULL;
    }
}
static void test_large_disk_seed(const char *root)
{
    X4CatalogArtDemand art[LARGE_TEST_PRODUCTS]={0};
    const char *public_jpeg_dir=getenv("X4_T40_PUBLIC_JPEG_DIR");
    large_warm_fixture(art);
    X4CatalogDiskCache *cache=x4_catalog_disk_cache_open(root,0,0,NULL,NULL);
    CHECK(cache);
    char protected_ids[LARGE_WARM_PRODUCTS][X4_PRODUCT_ID_CAP];
    for(unsigned i=0;i<LARGE_WARM_PRODUCTS;++i)memcpy(protected_ids[i],art[i].id,sizeof(art[i].id));
    x4_catalog_disk_cache_protect_products(cache,protected_ids,LARGE_WARM_PRODUCTS);
    unsigned essential_images=0;
    for(unsigned i=0;i<LARGE_WARM_PRODUCTS;++i){
        X4ProductMetadata m=large_metadata(art[i].id);
        CHECK(x4_catalog_disk_cache_put_metadata(cache,art[i].id,"MX","es-MX-x-t35b",&m,NULL)==1);
        for(unsigned j=0;j<=X4_COVER_SURFACES;++j){
            if(!(art[i].mask&(1u<<j)))continue;
            char url[X4_PUBLIC_COVER_URL_CAP];
            CHECK(x4_product_surface_url(&m,(enum X4CoverSurface)j,url));
            unsigned char *public_jpeg=NULL;
            size_t length=fake_image_length;
            if(public_jpeg_dir){
                char path[1024];
                int n=snprintf(path,sizeof(path),"%s/%s",public_jpeg_dir,
                    public_fixture_name(i,(enum X4CoverSurface)j));
                CHECK(n>0&&(size_t)n<sizeof(path));
                public_jpeg=read_file(path,&length);
            }
            CHECK(x4_catalog_disk_cache_put_image(cache,art[i].id,"MX","es-MX",url,
                public_jpeg?public_jpeg:fake_image,length,NULL)==1);
            free(public_jpeg);
            ++essential_images;
        }
    }
    const char *no_image=art[LARGE_WARM_PRODUCTS+1].id;
    X4ProductMetadata absent_image_metadata=large_metadata(no_image);
    CHECK(x4_catalog_disk_cache_put_metadata(cache,no_image,"MX","es-MX-x-t35b",
        &absent_image_metadata,NULL)==1);
    /* Valid JPEG comment segments increase disk pressure without inventing
     * new artwork or growing the decoder workload of the visible fixtures. */
    enum { SEGMENT=50000, SEGMENTS=3 };
    CHECK(fake_image_length>2&&fake_image[0]==0xff&&fake_image[1]==0xd8);
    size_t padded_length=fake_image_length+SEGMENT*SEGMENTS;
    unsigned char *padded=calloc(1,padded_length);
    CHECK(padded);
    memcpy(padded,fake_image,2);
    for(unsigned i=0;i<SEGMENTS;++i){
        size_t at=2+i*SEGMENT;
        padded[at]=0xff;padded[at+1]=0xfe;
        padded[at+2]=(SEGMENT-2)>>8;padded[at+3]=(SEGMENT-2)&255u;
    }
    memcpy(padded+2+SEGMENT*SEGMENTS,fake_image+2,fake_image_length-2);
    for(unsigned i=0;i<LARGE_NOISE_IMAGES;++i){
        char id[X4_PRODUCT_ID_CAP],url[X4_PUBLIC_COVER_URL_CAP];
        snprintf(id,sizeof(id),"NOISE40%u",i);
        X4ProductMetadata m=large_metadata(id);
        CHECK(x4_product_surface_url(&m,X4_COVER_ROW,url));
        CHECK(x4_catalog_disk_cache_put_image(cache,id,"MX","es-MX",url,padded,padded_length,NULL)==1);
    }
    free(padded);
    X4CatalogDiskStats s;
    x4_catalog_disk_cache_stats(cache,&s);
    CHECK(s.records==LARGE_WARM_PRODUCTS+1u+essential_images+LARGE_NOISE_IMAGES&&s.evictions==0);
    CHECK(s.bytes>64u*1024u*1024u&&s.bytes<=X4_CATALOG_DISK_BUDGET);
    x4_catalog_disk_cache_close(cache);
    printf("PASS T40 disk seed pid=%ld images=%u bytes=%zu essential=%u fixture=%s\n",
        (long)getpid(),LARGE_NOISE_IMAGES+essential_images,s.bytes,essential_images,
        public_jpeg_dir?"public_jpeg":"synthetic");
}
static uint64_t realtime_us(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
static void test_large_disk_warm(const char *root)
{
    fixture_downloaders=3;
    fake_reset();
    X4CatalogArtDemand art[LARGE_TEST_PRODUCTS]={0};
    large_warm_fixture(art);
    X4CatalogAssets *a=create_with_clock(root,NULL);
    const char *ids[LARGE_WARM_PRODUCTS];
    uint32_t masks[LARGE_WARM_PRODUCTS];
    for(unsigned i=0;i<LARGE_WARM_PRODUCTS;++i){
        ids[i]=art[i].id;
        masks[i]=art[i].mask&~(1u<<X4_COVER_BACKGROUND);
    }
    x4_catalog_assets_request_surfaces(a,ids,masks,2);
    int accepted=0;
    for(unsigned i=0;i<5000&&!accepted;++i){
        accepted=x4_catalog_assets_preload(a,art,LARGE_TEST_PRODUCTS);
        if(!accepted)sleep_ms(1);
    }
    CHECK(accepted);
    uint64_t warm_at=realtime_us();
    x4_catalog_assets_resume(a);
    X4CatalogAssetStats s={0};
    for(unsigned i=0;i<10000;++i){
        s=stats(a);
        if(s.disk_image_hits==LARGE_WARM_PRODUCTS+1u&&s.background_count==1&&
           s.requests>=LARGE_TEST_PRODUCTS&&s.disk_metadata_hits==LARGE_WARM_PRODUCTS+1u&&!s.busy)break;
        sleep_ms(1);
    }
    CHECK(s.disk_enabled&&s.disk_image_hits==LARGE_WARM_PRODUCTS+1u&&s.background_count==1);
    CHECK(s.disk_metadata_hits==LARGE_WARM_PRODUCTS+1u&&s.requests>=LARGE_TEST_PRODUCTS&&!s.busy);
    CHECK(!atomic_load(&json_calls)&&!atomic_load(&image_calls)&&!atomic_load(&open_calls));
    CHECK(s.thumbnail_peak_bytes<=X4_CATALOG_THUMBNAIL_BUDGET&&s.background_count<=2);
    uint64_t warmed_at=realtime_us();
    CHECK(atomic_load(&peak_decodes)==1);
    /* Entering Discover with a refreshed catalog must reuse the prepared
     * public rasters, including the exact background after generation change. */
    accepted=0;
    for(unsigned i=0;i<5000&&!accepted;++i){
        accepted=x4_catalog_assets_preload(a,NULL,0);
        if(!accepted)sleep_ms(1);
    }
    CHECK(accepted);
    x4_catalog_assets_set_generation(a,2);
    wait_idle(a);
    X4CatalogArtReady ready={0};
    accepted=0;
    for(unsigned i=0;i<5000;++i){
        x4_catalog_assets_request_surfaces(a,ids,masks,LARGE_WARM_PRODUCTS);
        x4_catalog_assets_request_background(a,art[1].id);
        accepted=x4_catalog_assets_art_ready(a,art,LARGE_WARM_PRODUCTS,warmed_at,&ready);
        if(accepted&&!ready.pending)break;
        sleep_ms(1);
    }
    CHECK(accepted&&!ready.pending&&!ready.fallback&&!ready.disk&&!ready.network&&ready.ram==LARGE_WARM_PRODUCTS+1u);
    x4_catalog_assets_resume(a);
    wait_idle(a);
    CHECK(!atomic_load(&json_calls)&&!atomic_load(&image_calls)&&!atomic_load(&open_calls));
    CHECK(atomic_load(&decode_calls)==LARGE_WARM_PRODUCTS+1u);
    close_assets(a);
    printf("PASS T40 disk warm pid=%ld images=%u RAM=%u metadata_get=0 image_get=0 entry_decodes=0 disk_read_us=%llu decode_us=%llu warm_us=%llu\n",
        (long)getpid(),LARGE_WARM_PRODUCTS+1u,ready.ram,
        (unsigned long long)s.stage[X4_ASSET_IMAGE].total_us,
        (unsigned long long)s.stage[X4_ASSET_DECODE].total_us,(unsigned long long)(warmed_at-warm_at));
}
#endif
static int test_35b_urls(void)
{
    static X4CatalogSnapshot catalog;
    static X4CatalogPresentation presentation;
    X4CatalogBrowser browser;
    X4RecentGames recent;
    char recent_ids[][96] = {"TITLE_2", "TITLE_1"};
    char wanted[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP];
    uint32_t masks[X4_PRESENTATION_DEMAND_MAX];
    const char *ids[X4_PRESENTATION_DEMAND_MAX];

    catalog.state = X4_CATALOG_READY;
    catalog.count = 2;
    strcpy(catalog.titles[0].id, "TITLE_1");
    strcpy(catalog.titles[0].product_id, "ID1");
    strcpy(catalog.titles[1].id, "TITLE_2");
    strcpy(catalog.titles[1].product_id, "ID2");
    x4_catalog_presentation_init(&presentation);
    x4_catalog_presentation_set_generation(&presentation, 1);
    x4_catalog_browser_init(&browser);
    x4_catalog_browser_rebuild(&browser, &catalog);
    x4_recent_init(&recent);
    x4_recent_begin(&recent, 1, 1);
    CHECK(x4_recent_publish(&recent, 1, 1, recent_ids, 2, 0, 200, &catalog));
    x4_catalog_browser_sync_recents(&browser, &recent);
    browser.apple_mode = true;
    browser.active_tab = X4_CAT_TAB_RECENTS;
    browser.section = X4_CATALOG_SECTION_RECENT;
    size_t count = x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, wanted, masks);
    CHECK(count == 2 && !strcmp(wanted[0], "ID2"));
    for (size_t i = 0; i < count; ++i) ids[i] = wanted[i];

    fake_reset();
    X4CatalogAssets *assets = create_assets(NULL);
    x4_catalog_assets_request_surfaces(assets, ids, masks, count);
    x4_catalog_assets_resume(assets);
    wait_ready(assets, 2);
    close_assets(assets);
    unsigned grid = 0, detail = 0;
    for (unsigned i = 0; i < atomic_load(&image_calls); ++i) {
        printf("URL 35b Recientes: %s%s\n", image_order[i], image_queries[i]);
        if (!strcmp(image_queries[i], "?w=356&h=200&q=85&format=jpg")) ++grid;
        else if (!strcmp(image_queries[i], "?w=524&h=295&q=85&format=jpg")) {
            CHECK(!strcmp(image_order[i], "ID2"));
            ++detail;
        } else CHECK(0);
    }
    CHECK(grid == 2 && detail == 1);
    puts("PASS URL 35b Recientes: 2 GRID y 1 DETAIL, ninguna de 96x96");

    browser.active_tab = X4_CAT_TAB_CATALOG;
    browser.section = X4_CATALOG_SECTION_CATALOG;
    count = x4_catalog_presentation_demand_surfaces(&presentation, &browser,
        &catalog, &recent, wanted, masks);
    CHECK(count == 2);
    for (size_t i = 0; i < count; ++i) ids[i] = wanted[i];
    fake_reset();
    assets = create_assets(NULL);
    x4_catalog_assets_request_surfaces(assets, ids, masks, count);
    x4_catalog_assets_resume(assets);
    wait_ready(assets, 2);
    close_assets(assets);
    grid = 0;
    detail = 0;
    for (unsigned i = 0; i < atomic_load(&image_calls); ++i) {
        printf("URL 35b Catálogo: %s%s\n", image_order[i], image_queries[i]);
        if (!strcmp(image_queries[i], "?w=356&h=200&q=85&format=jpg")) ++grid;
        else if (!strcmp(image_queries[i], "?w=524&h=295&q=85&format=jpg")) ++detail;
        else CHECK(0);
    }
    CHECK(grid == 2 && detail == 1);
    puts("PASS URL 35b Catálogo: 2 GRID y 1 DETAIL");

    /* Call the same demand calculator as main.c and the actual worker. */
    const char *home_ids[2];
    uint32_t home_masks[2];
    fake_reset();
    assets = create_assets(NULL);
    count = x4_screen_cover_demand(0, "9NKX70BBCDRN", home_ids, home_masks);
    CHECK(count == 2);
    x4_catalog_assets_request_surfaces(assets, home_ids, home_masks, count);
    x4_catalog_assets_resume(assets);
    wait_ready(assets, 2);
    CHECK(atomic_load(&image_calls) == 2);
    count = x4_screen_cover_demand(8, NULL, home_ids, home_masks);
    CHECK(count == 1);
    x4_catalog_assets_request_surfaces(assets, home_ids, home_masks, count);
    wait_ready(assets, 2);
    X4CatalogAssetStats peak = stats(assets);
    close_assets(assets);
    CHECK(atomic_load(&image_calls) == 3);
    unsigned home = 0, card = 0, poster = 0;
    for (unsigned i = 0; i < 3; ++i) {
        printf("URL 35b Inicio/Fortnite: %s%s\n", image_order[i], image_queries[i]);
        if (!strcmp(image_queries[i], "?w=852&h=480&q=85&format=jpg")) {
            CHECK(!strcmp(image_order[i], "9NKX70BBCDRN"));
            ++home;
        } else {
            CHECK(!strcmp(image_order[i], X4_FORTNITE_PRODUCT_ID));
            if (!strcmp(image_queries[i], "?w=796&h=448&q=85&format=jpg")) ++card;
            else if (!strcmp(image_queries[i], "?w=550&h=825&q=85&format=jpg")) ++poster;
            else CHECK(0);
        }
    }
    CHECK(home == 1 && card == 1 && poster == 1);
    CHECK(peak.thumbnail_peak_bytes == (852u*448u + 420u*448u + 550u*770u)*sizeof(uint32_t));
    CHECK(peak.thumbnail_peak_bytes <= X4_CATALOG_THUMBNAIL_BUDGET);
    printf("LRU 35b pico=%zu B límite=%u B\n", peak.thumbnail_peak_bytes, X4_CATALOG_THUMBNAIL_BUDGET);

    fake_reset();
    atomic_store(&fake_no_fortnite_poster, 1);
    assets = create_assets(NULL);
    x4_catalog_assets_request_surfaces(assets, home_ids, home_masks, count);
    x4_catalog_assets_resume(assets);
    wait_ready(assets, 1);
    close_assets(assets);
    CHECK(atomic_load(&image_calls) == 1);
    CHECK(!strcmp(image_order[0], X4_FORTNITE_PRODUCT_ID));
    CHECK(!strcmp(image_queries[0], "?w=770&h=770&q=85&format=jpg"));
    printf("URL 35b Fortnite sin Poster: %s%s\n", image_order[0], image_queries[0]);
    puts("PASS URL 35b Inicio/Fortnite: tamaños exactos, respaldo BoxArt y ninguna de 96x96");
    return 0;
}
static void wait_background_status(X4CatalogAssets *a,const char *id,enum X4CatalogAssetState expected)
{
    for(unsigned i=0;i<5000;++i){
        enum X4CatalogAssetState got;
        if(x4_catalog_assets_background_status(a,id,&got)&&got==expected)return;
        sleep_ms(1);
    }
    CHECK(0);
}

static void test_background_status39(void)
{
    static const struct {int http;enum X4CatalogAssetState expected;} cases[]={
        {200,X4_ASSET_READY},{404,X4_ASSET_MISSING},{503,X4_ASSET_TEMPORARY_FAILURE}
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        fake_reset();
        atomic_store(&fake_image_status,cases[i].http);
        atomic_store(&block_image,1);
        X4CatalogAssets *a=create_assets(NULL);
        enum X4CatalogAssetState result=X4_ASSET_READY;
        CHECK(!x4_catalog_assets_background_status(a,"NOPE",&result)&&result==X4_ASSET_READY);
        CHECK(!x4_catalog_assets_background_status(a,"../BAD",&result)&&result==X4_ASSET_READY);
        x4_catalog_assets_request_background(a,"BACK39");
        wait_background_status(a,"BACK39",X4_ASSET_PENDING);
        x4_catalog_assets_resume(a);
        wait_at_least(&image_arrivals,1);
        wait_background_status(a,"BACK39",X4_ASSET_DOWNLOADING);
        CHECK(atomic_load(&image_calls)==1);
        atomic_store(&block_image,0);
        wait_background_status(a,"BACK39",cases[i].expected);
        X4CatalogBackgroundView view;
        if(cases[i].expected==X4_ASSET_READY){
            wait_background(a,"BACK39",&view);
            x4_catalog_assets_release_background(a,&view);
        }else CHECK(!x4_catalog_assets_acquire_background(a,&view));
        x4_catalog_assets_suspend(a);
        wait_idle(a);
        x4_catalog_assets_set_generation(a,2);
        CHECK(!x4_catalog_assets_background_status(a,"BACK39",&result));
        close_assets(a);
    }
    puts("PASS background state 39: pending, downloading, ready, missing, failed and generation isolation");
}

#ifndef X4_ASSETS_TEST_BASELINE31D
static X4GameAssetStats game_stats(X4CatalogAssets *a)
{
    X4GameAssetStats result;
    for(unsigned i=0;i<5000;++i){
        if(x4_catalog_assets_game_stats(a,&result))return result;
        sleep_ms(1);
    }
    CHECK(0);
    return (X4GameAssetStats){0};
}
static void game_wait_metadata(X4CatalogAssets *a,const char *id,X4GameDetails *out)
{
    for(unsigned i=0;i<5000;++i){
        enum X4CatalogAssetState state;
        if(x4_catalog_assets_game_details(a,id,out,&state)&&state==X4_ASSET_READY)return;
        sleep_ms(1);
    }
    CHECK(0);
}
static void game_wait_image(X4CatalogAssets *a,const char *id,unsigned index,bool fullscreen)
{
    for(unsigned i=0;i<5000;++i){
        X4GameScreenshotView view;
        if(x4_catalog_assets_game_acquire(a,id,index,fullscreen,&view)){
            CHECK(view.raster.rgba&&view.raster.width==(fullscreen?1920u:524u)&&
                view.raster.height==(fullscreen?1080u:295u));
            x4_catalog_assets_game_release(a,&view);
            return;
        }
        sleep_ms(1);
    }
    CHECK(0);
}
static void game_wait_retired(X4CatalogAssets *a)
{
    for(unsigned i=0;i<5000;++i){
        if(game_stats(a).cancelled)return;
        sleep_ms(1);
    }
    CHECK(0);
}
static void test_game_parse(const char *fixture)
{
    size_t length;
    unsigned char *json=read_file(fixture,&length);
    X4ProductMetadata metadata;
    X4GameProductDetails game;
    CHECK(!x4_product_metadata_parse_details((const char *)json,length,"9NKX70BBCDRN",&metadata,&game));
    CHECK(strstr(game.details.title,"Forza Horizon 5")==game.details.title);
    CHECK(!strcmp(game.details.year,"2021")&&!strcmp(game.details.age_rating,"ESRB:E"));
    CHECK(strlen(game.details.description)>strlen(metadata.description));
    CHECK(game.details.screenshot_count==X4_GAME_SCREENSHOT_MAX);
    for(unsigned i=0;i<game.details.screenshot_count;++i){
        char mini[X4_PUBLIC_COVER_URL_CAP],full[X4_PUBLIC_COVER_URL_CAP];
        CHECK(x4_product_screenshot_url(&game,i,0,mini)&&x4_http_public_image_url(mini));
        CHECK(x4_product_screenshot_url(&game,i,1,full)&&x4_http_public_image_url(full));
        CHECK(!strcmp(strchr(mini,'?'),"?w=524&h=295&q=85&format=jpg"));
        CHECK(!strcmp(strchr(full,'?'),"?w=1920&h=1080&q=85&format=jpg"));
        CHECK(!strncmp(mini,full,(size_t)(strchr(mini,'?')-mini)));
    }
    char url[X4_PUBLIC_COVER_URL_CAP];
    CHECK(!x4_product_screenshot_url(&game,X4_GAME_SCREENSHOT_MAX,1,url));
    free(json);
}
static void test_full_store_description46(void)
{
    static const char prefix[]="{\"Products\":[{\"ProductId\":\"TEXT46\",\"LocalizedProperties\":[{\"ProductDescription\":\"";
    static const char suffix[]="\"}]}]}";
    static const unsigned char point[]={0xf0,0x9f,0x8e,0xae};
    const size_t bytes=X4_GAME_DESCRIPTION_CHARS*sizeof(point);
    const size_t length=sizeof(prefix)-1u+bytes+sizeof(suffix)-1u;
    char *json=malloc(length+1u);
    X4GameProductDetails *game=malloc(sizeof(*game));
    CHECK(json&&game);
    memcpy(json,prefix,sizeof(prefix)-1u);
    char *payload=json+sizeof(prefix)-1u;
    for(size_t offset=0;offset<bytes;offset+=sizeof(point))memcpy(payload+offset,point,sizeof(point));
    memcpy(payload+bytes,suffix,sizeof(suffix));

    X4ProductMetadata metadata;
    CHECK(!x4_product_metadata_parse_details(json,length,"TEXT46",&metadata,game));
    CHECK(strlen(game->details.description)==bytes&&
        !memcmp(game->details.description,payload,bytes));
    CHECK(strlen(metadata.description)<sizeof(metadata.description));
    free(game);
    free(json);
    puts("PASS ficha: 10.000 caracteres de 4 bytes conservados en la descripcion completa");
}
static void test_game_cache46(void)
{
    char disk[]="/tmp/x4-assets-cache.XXXXXX";
    CHECK(mkdtemp(disk));
    X4GameDetails details;
    for(unsigned warm=0;warm<2;++warm){
        fake_reset();
        X4CatalogAssets *a=create_assets(disk);
        x4_catalog_assets_game_request(a,"GAME46",-1);
        sleep_ms(30);
        CHECK(!atomic_load(&image_calls)&&!atomic_load(&json_calls));
        uint64_t requested=x4_catalog_assets_game_revision(a);
        x4_catalog_assets_game_request(a,"GAME46",-1);
        CHECK(x4_catalog_assets_game_revision(a)==requested);
        uint64_t resume_at = realtime_us();
        x4_catalog_assets_resume(a);
        game_wait_metadata(a,"GAME46",&details);
        uint64_t details_us = realtime_us() - resume_at;
        CHECK(details.screenshot_count==6&&!strcmp(details.year,"2021")&&
            !strcmp(details.age_rating,"ESRB:E")&&!strcmp(details.developer,"Example Studio"));
        CHECK(!strcmp(details.description,"Long fixture description. A second sentence."));
        CHECK(x4_catalog_assets_game_revision(a)>requested);
        game_wait_image(a,"GAME46",0,false);
        uint64_t first_image_us = realtime_us() - resume_at;
        for(unsigned i=1;i<6;++i)game_wait_image(a,"GAME46",i,false);
        X4GameAssetStats before=game_stats(a);
        CHECK(before.metadata_requests==1&&before.thumbnail_requests==(warm?0u:6u)&&
            !before.fullscreen_requests);
        x4_catalog_assets_game_request(a,"GAME46",2);
        game_wait_image(a,"GAME46",2,true);
        X4GameAssetStats after=game_stats(a);
        CHECK(after.fullscreen_requests==(warm?0u:1u)&&after.disk_hits==(warm?7u:0u));
        CHECK(after.peak_raster_bytes==(6u*524u*295u+1920u*1080u)*sizeof(uint32_t));
        unsigned images=atomic_load(&image_calls);
        x4_catalog_assets_game_request(a,NULL,-1);
        x4_catalog_assets_game_request(a,"GAME46",2);
        game_wait_metadata(a,"GAME46",&details);
        game_wait_image(a,"GAME46",2,true);
        CHECK(atomic_load(&json_calls)==1&&atomic_load(&image_calls)==images);
        close_assets(a);
        printf("PASS ficha %s: metadata=%llu mini=%llu completa=%llu disco=%llu pico=%zu B\n",
            warm?"disco":"fria",(unsigned long long)after.metadata_requests,
            (unsigned long long)after.thumbnail_requests,(unsigned long long)after.fullscreen_requests,
            (unsigned long long)after.disk_hits,after.peak_raster_bytes);
        printf("TIEMPO46 ficha %s detalles_us=%llu primera_miniatura_us=%llu (transporte simulado)\n",
            warm?"disco":"fria",(unsigned long long)details_us,(unsigned long long)first_image_us);
    }
    cache_directory_action(disk,0);
    CHECK(!rmdir(disk));
}
static void test_game_cancellation46(void)
{
    fake_reset();
    atomic_store(&block_image,1);
    const char *cover_ids[]={"ORDINARY1","ORDINARY2","ORDINARY3"};
    X4CatalogAssets *a=create_assets(NULL);
    request_parallel(a,cover_ids,3);
    wait_at_least(&image_arrivals,3);
    x4_catalog_assets_game_request(a,"GAME46",-1);
    wait_at_least(&image_arrivals,4);
    CHECK(atomic_load(&active_images)==4);
    x4_catalog_assets_game_request(a,NULL,-1);
    wait_at_least(&image_cancellations,1);
    game_wait_retired(a);
    CHECK(atomic_load(&active_images)==3&&atomic_load(&image_cancellations)==1);
    atomic_store(&block_image,0);
    wait_ready(a,3);
    CHECK(atomic_load(&image_calls)==4&&atomic_load(&peak_images)==4&&
        atomic_load(&peak_decodes)==1);
    close_assets(a);
    puts("PASS ficha: cancelar solo el cuarto descargador; tres portadas y un decoder");
}
static void test_game_limits46(void)
{
    fake_reset();
    atomic_store(&bound_screenshots,1);
    X4CatalogAssets *a=create_assets(NULL);
    x4_catalog_assets_game_request(a,"GAME46",-1);
    x4_catalog_assets_resume(a);
    wait_at_least(&screenshot_bound_calls,6);
    wait_idle(a);
    CHECK(atomic_load(&image_calls)==6&&!atomic_load(&decode_calls));
    CHECK(game_stats(a).metadata_requests==1);
    x4_catalog_assets_game_request(a,"GAME46",3);
    wait_at_least(&screenshot_bound_calls,7);
    wait_idle(a);
    CHECK(atomic_load(&image_calls)==7&&!atomic_load(&decode_calls));
    CHECK(game_stats(a).fullscreen_requests==1);
    close_assets(a);
    puts("PASS ficha: seis miniaturas max, tope por transferencia, completa solo bajo demanda");
}
#endif

int main(int argc,char **argv)
{
#ifndef X4_ASSETS_TEST_BASELINE31D
    if(argc==6&&!strcmp(argv[5],"--46-game-assets")){
        fixture_game=1;
        fixture_downloaders=3;
        fake_image=read_file(argv[2],&fake_image_length);
        test_game_parse(argv[1]);
        test_full_store_description46();
        test_game_cache46();
        test_game_cancellation46();
        test_game_limits46();
        free(fake_image);
        printf("PASS assets ficha T46: %u comprobaciones\n",atomic_load(&checks));
        return 0;
    }
    if(argc==7&&(!strcmp(argv[5],"--40-cache-seed")||!strcmp(argv[5],"--40-cache-warm"))){
        fake_image=read_file(argv[2],&fake_image_length);
        if(!strcmp(argv[5],"--40-cache-seed"))test_large_disk_seed(argv[6]);
        else test_large_disk_warm(argv[6]);
        free(fake_image);
        return 0;
    }
    if(argc==6&&!strcmp(argv[5],"--40-assets-parallel")){
        fake_image=read_file(argv[2],&fake_image_length);
        test_parallel_assets();
        free(fake_image);
        return 0;
    }
#endif
    if(argc==6&&!strcmp(argv[5],"--39-background-status")){
        fake_image=read_file(argv[2],&fake_image_length);
        test_background_status39();
        free(fake_image);
        return 0;
    }
    if(argc==7&&!strcmp(argv[5],"--36-cache-diagnostic")){
        trace_disk=1;
        fake_image=read_file(argv[2],&fake_image_length);
        /* Preserve this explicitly named workspace fixture. The corruption
         * case runs only on the suite's own mkdtemp cache. */
        (void)test_disk_reopen(argv[6]);
        free(fake_image);
        puts("PASS cache original restart: two metadata hits, two image hits, zero new GET");
        return 0;
    }
    if(argc==7&&(!strcmp(argv[5],"--36-cache-cold")||!strcmp(argv[5],"--36-cache-warm"))){
        fake_image=read_file(argv[2],&fake_image_length);
        test_separate_cache_process(argv[6],!strcmp(argv[5],"--36-cache-warm"));
        free(fake_image);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[5], "--35b-urls")) {
        fake_image = read_file(argv[2], &fake_image_length);
        int result = test_35b_urls();
        free(fake_image);
        return result;
    }
    CHECK(argc==5);fake_image=read_file(argv[2],&fake_image_length);test_text();test_policy_metadata(argv[1]);test_images(argv[3]);test_color_layout(argv[4]);
    test_priority_metadata_before_cover();test_batches_large_and_partial();test_navigation_defers_obsolete_next_stage();
    test_retry_after_and_manual_retry();test_ram_eviction_pinning_generation();test_old_account_result_and_cancel();
    test_auth_boundary_waits_for_cleanup();test_disk_reopen_and_corrupt_fallback();
    test_cleanup_fault_is_fail_closed_and_destroyable();test_lowercase_disk_reopen_preserves_job_identity();
    test_six_card_loading_comparison();test_two_backgrounds_and_pins();test_one_sigl_per_account();
    test_parallel_assets();free(fake_image);
    printf("catalog assets PASS: legacy and three-download tests, %u checks (fake public HTTP; no real network)\n",atomic_load(&checks));return 0;
}
