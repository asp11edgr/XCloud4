/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_assets.h"
#include "catalog_disk_cache.h"
#include "lang.h"
#include "../auth/http_client.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef X4_ASSETS_HOST_TEST
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
typedef pthread_t AssetThread;
typedef pthread_mutex_t AssetMutex;
typedef sem_t AssetEvent;
static int event_init(AssetEvent *event) { return sem_init(event,0,0); }
static void event_post(AssetEvent *event) { (void)sem_post(event); }
static void event_wait(AssetEvent *event)
{
    while(sem_wait(event)<0&&errno==EINTR){}
}
static void event_destroy(AssetEvent *event) { (void)sem_destroy(event); }
static int start_thread(AssetThread *thread,void *(*entry)(void *),void *arg,const char *name)
{
    (void)name;
    return pthread_create(thread,NULL,entry,arg);
}
static void join_thread(AssetThread thread) { (void)pthread_join(thread,NULL); }
static int mutex_init(AssetMutex *m) { return pthread_mutex_init(m, NULL);
}
static void lock(AssetMutex *m) { pthread_mutex_lock(m);
}
static int trylock(AssetMutex *m) { return pthread_mutex_trylock(m);
}
static void unlock(AssetMutex *m) { pthread_mutex_unlock(m);
}
static void mutex_destroy(AssetMutex *m) { pthread_mutex_destroy(m);
}
static void pause_worker(void) { struct timespec t={0,10000000};
nanosleep(&t,NULL);
}
static uint64_t platform_clock(void *p) { (void)p;
struct timespec t;
clock_gettime(CLOCK_MONOTONIC,&t);
return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
#else
#include <orbis/libkernel.h>
#include "../core/x4_log.h"
typedef OrbisPthread AssetThread;
typedef OrbisPthreadMutex AssetMutex;
typedef OrbisKernelSema AssetEvent;
static int event_init(AssetEvent *event)
{
    return sceKernelCreateSema(event,"x4-cover-wake",ORBIS_KERNEL_SEMA_ATTR_TH_FIFO,0,1,NULL);
}
static void event_post(AssetEvent *event) { (void)sceKernelSignalSema(*event,1); }
static void event_wait(AssetEvent *event)
{
    if(sceKernelWaitSema(*event,1,NULL))sceKernelUsleep(10000);
}
static void event_destroy(AssetEvent *event) { (void)sceKernelDeleteSema(*event); }
static int start_thread(AssetThread *thread,void *(*entry)(void *),void *arg,const char *name)
{
    OrbisPthreadAttr attr;
    int rc=scePthreadAttrInit(&attr);
    if(!rc){
        rc=scePthreadAttrSetstacksize(&attr,256u*1024u);
        if(!rc)rc=scePthreadCreate(thread,&attr,entry,arg,name);
        (void)scePthreadAttrDestroy(&attr);
    }
    return rc;
}
static void join_thread(AssetThread thread) { (void)scePthreadJoin(thread,NULL); }
static int mutex_init(AssetMutex *m) { return scePthreadMutexInit(m,NULL,"x4-ui-assets");
}
static void lock(AssetMutex *m) { scePthreadMutexLock(m);
}
static int trylock(AssetMutex *m) { return scePthreadMutexTrylock(m);
}
static void unlock(AssetMutex *m) { scePthreadMutexUnlock(m);
}
static void mutex_destroy(AssetMutex *m) { scePthreadMutexDestroy(m);
}
static void pause_worker(void) { sceKernelUsleep(10000);
}
static uint64_t platform_clock(void *p) { (void)p;
return sceKernelGetProcessTime();
}
#endif
#define METADATA_CAP (1024u*1024u+1u)
#define METADATA_BATCH 4u
#define ATTEMPTS_MAX 3u
#define BACKGROUND_SLOTS 2u
#define BACKGROUND_MASK (1u << X4_COVER_BACKGROUND)
#define THUMBNAIL_MASK (BACKGROUND_MASK - 1u)
#define LEGACY_MASK ((1u << X4_COVER_GRID) | (1u << X4_COVER_DETAIL) | (1u << X4_COVER_RECENT))
#define METADATA_JOB (-1)
#define NO_JOB (-2)
#define GAME_FULLSCREEN_SLOT X4_GAME_SCREENSHOT_MAX
#define GAME_IMAGE_COUNT (X4_GAME_SCREENSHOT_MAX + 1u)
#define MARKET x4_lang_market()
#define LANGUAGE x4_lang_locale()

/* Old cached metadata never retained the Store's Poster entry. Keep the
 * original market/locale for HTTP and JPEG keys, and refresh metadata once. */
static void metadata_cache_locale(char locale[24])
{
    snprintf(locale, 24, "%s-x-t35b", LANGUAGE);
}
typedef struct {
    uint64_t due;
    unsigned attempts;
    enum X4CatalogAssetState state;
    int disk_bad;
#if !X4_PUBLIC
    uint64_t ready_at;
    enum X4AssetSource ready_source;
#endif
} ImageState;
typedef struct {
    X4ProductMetadata metadata;
    X4GameProductDetails game_details;
    int game_details_valid;
    X4CoverRasters cover;
    ImageState image[X4_COVER_SURFACES + 1u];
    uint64_t generation, used, queued_us, metadata_due;
    uint32_t wanted;
    unsigned pins, metadata_attempts;
    int occupied, busy, metadata_valid, metadata_missing, cover_announced;
    enum X4CatalogAssetState state;
} Slot;
typedef struct {
    X4CoverRaster raster;
    uint64_t generation;
    char id[X4_PRODUCT_ID_CAP];
    unsigned pins;
    int preloaded;
#if !X4_PUBLIC
    uint64_t ready_at;
    enum X4AssetSource ready_source;
#endif
} Background;
typedef struct {
    unsigned slot;
    int surface, background_slot;
    uint64_t generation, account, preload_revision;
    char id[X4_PRODUCT_ID_CAP];
} Job;
typedef struct {
    X4CatalogArtDemand demand;
    uint32_t attempted;
    int metadata_attempted;
} Preload;
enum DownloadState { DOWNLOAD_FREE, DOWNLOAD_QUEUED, DOWNLOAD_RUNNING, DOWNLOAD_DONE };
typedef struct {
    X4CatalogAssets *assets;
    AssetThread thread;
    AssetEvent wake;
    unsigned index;
    unsigned char *encoded;
    Job job;
    char url[X4_PUBLIC_COVER_URL_CAP];
    enum DownloadState state;
    size_t length;
    uint64_t retry_ms, sequence;
    int rc, status, deferred, reset_requested, reset_error;
} Downloader;
typedef struct {
    X4CoverRaster raster;
    unsigned pins;
    enum X4CatalogAssetState state;
} GameImage;
typedef struct {
    char id[X4_PRODUCT_ID_CAP], cached_id[X4_PRODUCT_ID_CAP];
    uint64_t request;
    int requested_full, full_index;
    enum X4CatalogAssetState metadata_state;
    X4GameProductDetails details;
    GameImage images[GAME_IMAGE_COUNT];
    X4GameAssetStats stats;
} GameAssets;
struct X4CatalogAssets {
    AssetThread thread;
    AssetMutex mutex, http_open_mutex;
    Downloader downloaders[X4_CATALOG_IMAGE_DOWNLOADERS];
    Downloader game_downloader;
    GameAssets game;
    /* Coordinator-only scratch shared by the singleton and recursive
     * batch parser. It is inside the heap-allocated asset owner. */
    X4GameProductDetails game_parse_scratch;
    _Atomic int game_metadata_cancel, game_image_cancel;
    _Atomic uint64_t game_revision;
    unsigned downloader_count;
    _Atomic int downloaders_stop;
    uint64_t image_retry_until, dispatch_sequence;
    _Atomic int stop, suspended, cancel, busy, unavailable;
    /* 0 cleaned/suspended, 1 enabled idle, 2 active, 3 cancelled active,
     * 4 cleanup pending. Auth handoff is permitted only in 0 or 1. */
    _Atomic int run_gate;
    _Atomic uint64_t generation;
    X4AssetClock clock;
    void *clock_context;
    char disk_root[256];
    uint64_t use_clock, revision;
    Slot slots[X4_CATALOG_ASSET_SLOTS];
    char demand[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP];
    uint32_t demand_masks[X4_CATALOG_ASSET_SLOTS];
    size_t demand_count;
    Preload preload[X4_CATALOG_PRELOAD_PRODUCTS];
    size_t preload_count;
    uint64_t preload_revision;
    char protected_ids[X4_CATALOG_DISK_PROTECTED][X4_PRODUCT_ID_CAP];
    unsigned protected_count;
    uint64_t protected_revision;
    Background backgrounds[BACKGROUND_SLOTS];
    int background_active;
    char background_id[X4_PRODUCT_ID_CAP];
    X4SiglList sigls[X4_SIGL_COUNT];
    uint64_t sigl_session, sigl_revision;
    unsigned sigl_started;
    X4CatalogAssetStats stats;
    X4AssetEvent events[X4_ASSET_EVENT_CAP];
    unsigned event_next, event_count;
};
/* Original art statistics and slot counts stay independent of the fourth lane. */
static Downloader *downloader_at(X4CatalogAssets *a,unsigned i)
{
    return i==a->downloader_count?&a->game_downloader:&a->downloaders[i];
}
static void game_changed(X4CatalogAssets *a)
{
    atomic_fetch_add_explicit(&a->game_revision,1,memory_order_release);
}
static int game_requested(const X4CatalogAssets *a,const char *id)
{
    return a->game.id[0]&&!strcmp(a->game.id,id);
}
static void game_reset_image(GameAssets *game,unsigned slot)
{
    GameImage *image=&game->images[slot];
    game->stats.raster_bytes-=(size_t)image->raster.width*image->raster.height*sizeof(uint32_t);
    free(image->raster.rgba);
    *image=(GameImage){0};
}
static void game_reset_cache(GameAssets *game)
{
    for(unsigned i=0;i<GAME_IMAGE_COUNT;++i)game_reset_image(game,i);
    game->cached_id[0]=0;
    game->metadata_state=X4_ASSET_PENDING;
    game->full_index=-1;
}
static int game_wanted_locked(X4CatalogAssets *a,const Job *job)
{
    GameAssets *game=&a->game;
    return game_requested(a,job->id)&&job->generation==game->request&&
        (job->surface!=(int)GAME_FULLSCREEN_SLOT||game->requested_full==(int)job->slot)&&
        !atomic_load(&a->suspended)&&!atomic_load(&a->cancel)&&!atomic_load(&a->stop);
}
static int download_needed_locked(X4CatalogAssets *,const Downloader *,const Job *);
static uint64_t now(X4CatalogAssets *a) { return a->clock(a->clock_context);
}
static uint64_t add_sat(uint64_t a,uint64_t b) { return UINT64_MAX-a<b?UINT64_MAX:a+b;
}
static void record(X4CatalogAssets *a,const Job *j,enum X4AssetStage stage,enum X4AssetSource source,uint64_t start,uint64_t end)
{
    lock(&a->mutex);
    uint64_t elapsed=end>=start?end-start:0;
    X4AssetTiming *t=&a->stats.stage[stage];
    ++t->count;
    t->total_us+=elapsed;
    if(elapsed>t->max_us)t->max_us=elapsed;
    X4AssetEvent *e=&a->events[a->event_next];
    *e=(X4AssetEvent){.start_us=start,.end_us=end,.generation=j->generation,.stage=stage,.source=source};
    memcpy(e->product_id,j->id,sizeof(e->product_id));
    a->event_next=(a->event_next+1u)%X4_ASSET_EVENT_CAP;
    if(a->event_count<X4_ASSET_EVENT_CAP)++a->event_count;
    else ++a->stats.events_omitted;
    unlock(&a->mutex);
}
static size_t raster_bytes(const X4CoverRaster *r)
{
    return r->rgba?(size_t)r->width*r->height*sizeof(*r->rgba):0;
}
static void evict_surface(X4CatalogAssets *a,Slot *s,unsigned surface)
{
    X4CoverRaster *r=&s->cover.surface[surface];
    a->stats.thumbnail_bytes-=raster_bytes(r);
    free(r->rgba);
    *r=(X4CoverRaster){0};
    s->image[surface]=(ImageState){0};
}
static void evict(X4CatalogAssets *a,Slot *s)
{
    for(unsigned i=0;i<X4_COVER_SURFACES;++i)evict_surface(a,s,i);
    memset(s,0,sizeof(*s));
}
static int make_room(X4CatalogAssets *a,size_t bytes)
{
    while(a->stats.thumbnail_bytes+bytes>X4_CATALOG_THUMBNAIL_BUDGET){
        Slot *oldest=NULL;
        unsigned surface=0;
        for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){
            Slot *s=&a->slots[i];
            if(s->pins||(oldest&&s->used>=oldest->used))continue;
            for(unsigned j=0;j<X4_COVER_SURFACES;++j){
                if((s->wanted&(1u<<j))||!s->cover.surface[j].rgba)continue;
                oldest=s;
                surface=j;
                break;
            }
        }
        if(!oldest)return 0;
        evict_surface(a,oldest,surface);
    }
    return 1;
}
static int find(X4CatalogAssets *a,const char *id,uint64_t generation)
{
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)
        if(a->slots[i].occupied&&a->slots[i].generation==generation&&!strcmp(a->slots[i].metadata.product_id,id))return (int)i;
    return -1;
}
static int allocate_slot(X4CatalogAssets *a)
{
    int candidate=-1;
    uint64_t oldest=UINT64_MAX;
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){
        Slot *s=&a->slots[i];
        if(!s->occupied)return (int)i;
        if(!s->busy&&!s->pins&&!s->wanted&&s->used<oldest){oldest=s->used;
        candidate=(int)i;
        }
    }
    return candidate;
}
static void occupy_slot(X4CatalogAssets *a,Slot *s,const char *id,uint64_t generation)
{
    evict(a,s);
    s->occupied=1;
    s->generation=generation;
    s->used=++a->use_clock;
    s->queued_us=now(a);
    s->metadata_missing=atomic_load(&a->unavailable)!=0;
    s->state=s->metadata_missing?X4_ASSET_MISSING:X4_ASSET_PENDING;
    memcpy(s->metadata.product_id,id,strlen(id)+1u);
    ++a->stats.requests;
}
static void fill_demand(X4CatalogAssets *a,uint64_t generation)
{
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)a->slots[i].wanted=0;
    /* Protect requested public products before choosing an eviction. Rebind
     * generation only when no borrower or old worker owns the slot. */
    size_t background=a->background_id[0]?1u:0u;
    size_t count=a->demand_count+background;
    if(count>X4_CATALOG_ASSET_SLOTS){
        size_t d=0;
        while(d<a->demand_count&&strcmp(a->demand[d],a->background_id))++d;
        /* A separately requested background outranks the last preload. */
        if(d==a->demand_count)--count;
    }
    for(size_t d=0;d<count;++d){
        const char *id=d<background?a->background_id:a->demand[d-background];
        uint32_t mask=d<background?BACKGROUND_MASK:a->demand_masks[d-background];
        int i=find(a,id,generation);
        if(i<0)for(unsigned k=0;k<X4_CATALOG_ASSET_SLOTS;++k){
            Slot *s=&a->slots[k];
            if(s->occupied&&!s->busy&&!s->pins&&!strcmp(s->metadata.product_id,id)){
                i=(int)k;
                s->generation=generation;
                s->image[X4_COVER_BACKGROUND]=(ImageState){
                    .state=s->metadata.hero_url[0]?X4_ASSET_PENDING:X4_ASSET_MISSING};
                s->queued_us=now(a);
                s->used=++a->use_clock;
                if(s->metadata_valid)++a->stats.ram_hits;
                ++a->revision;
                break;
            }
        }
        if(i>=0)a->slots[i].wanted|=mask;
    }
    for(size_t d=0;d<count;++d){
        const char *id=d<background?a->background_id:a->demand[d-background];
        uint32_t mask=d<background?BACKGROUND_MASK:a->demand_masks[d-background];
        int i=find(a,id,generation);
        if(i<0){
            int owned=0;
            for(unsigned k=0;k<X4_CATALOG_ASSET_SLOTS;++k)
                if(a->slots[k].occupied&&!strcmp(a->slots[k].metadata.product_id,id)&&
                   (a->slots[k].pins||a->slots[k].busy))owned=1;
            if(owned)continue;
            i=allocate_slot(a);
            if(i<0)continue;
            occupy_slot(a,&a->slots[i],id,generation);
        }
        a->slots[i].wanted|=mask;
    }
}
static Slot *job_slot(X4CatalogAssets *a,const Job *j)
{
    Slot *s=&a->slots[j->slot];
    return s->occupied&&s->generation==j->generation&&!strcmp(s->metadata.product_id,j->id)?s:NULL;
}
static int current_job(X4CatalogAssets *a,const Job *j)
{
    return j->generation==atomic_load(&a->generation)&&j->account==a->sigl_session;
}
static int needed_locked(X4CatalogAssets *a,const Job *j)
{
    Slot *s=job_slot(a,j);
    uint32_t mask=j->surface==METADATA_JOB?UINT32_MAX:1u<<(unsigned)j->surface;
    return s&&(j->preload_revision?j->preload_revision==a->preload_revision:(s->wanted&mask)!=0)&&
        current_job(a,j)&&!atomic_load(&a->cancel)&&!atomic_load(&a->suspended)&&!atomic_load(&a->stop);
}
static int needed(X4CatalogAssets *a,const Job *j)
{
    lock(&a->mutex);
    int yes=needed_locked(a,j);
    if(!yes)++a->stats.deferred_stages;
    unlock(&a->mutex);
    return yes;
}
static X4Http *open_http(X4CatalogAssets *a,int *error)
{
    /* The native module discovery fallback contains process-wide state. */
    lock(&a->http_open_mutex);
    X4Http *http=x4_http_open(error);
    unlock(&a->http_open_mutex);
    return http;
}
static int reset_http(X4Http **http)
{
    if(!*http)return 0;
    int rc=x4_http_public_reset(*http);
    if(!rc){
        x4_http_close(*http);
        *http=NULL;
    }
    return rc;
}
static int temporary(int rc,int status)
{
    if(rc==X4_HTTP_CANCELLED)return 0;
    if(rc==X4_HTTP_URL||rc==X4_HTTP_ARGUMENT||rc==X4_HTTP_BOUND)return 0;
    return rc!=0||status==200||status==408||status==425||status==429||status>=500;
}
static void failed(X4CatalogAssets *a,const Job *j,int rc,int status,uint64_t retry_ms)
{
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    if(s&&current_job(a,j)&&!atomic_load(&a->cancel)){
        ImageState *image=j->surface==METADATA_JOB?NULL:&s->image[j->surface];
        unsigned attempts=image?image->attempts:s->metadata_attempts;
        uint64_t backoff=attempts>1?4000000u:1000000u;
        uint64_t delay=retry_ms>UINT64_MAX/1000u?UINT64_MAX:retry_ms*1000u;
        if(delay<backoff)delay=backoff;
        if(image)image->due=add_sat(now(a),delay);
        else s->metadata_due=add_sat(now(a),delay);
        s->queued_us=image?image->due:s->metadata_due;
        enum X4CatalogAssetState result=temporary(rc,status)?X4_ASSET_TEMPORARY_FAILURE:X4_ASSET_MISSING;
        if(image)image->state=result;
        else{
            s->state=result;
            s->metadata_missing=result==X4_ASSET_MISSING;
        }
        ++a->stats.failures;
        ++a->revision;
    }else if(!current_job(a,j))++a->stats.stale_results;
    else ++a->stats.cancelled;
    unlock(&a->mutex);
}
static int publish_metadata(X4CatalogAssets *a,const Job *j,const X4ProductMetadata *m,enum X4AssetSource source)
{
    uint64_t start=now(a);
    int ok=0;
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    if(s&&(j->preload_revision?needed_locked(a,j):current_job(a,j)&&!atomic_load(&a->cancel))){
        s->metadata=*m;
        s->metadata_valid=1;
        s->metadata_missing=0;
        /* Disk keys are canonicalized, but the outstanding borrow/job identity
         * remains the validated caller ID until that job is retired. */
        memcpy(s->metadata.product_id,j->id,sizeof(s->metadata.product_id));
        s->queued_us=now(a);
        s->state=X4_ASSET_PENDING;
        for(unsigned i=0;i<=X4_COVER_SURFACES;++i){
            char url[X4_PUBLIC_COVER_URL_CAP];
            s->image[i].state=x4_product_surface_url(m,(enum X4CoverSurface)i,url)?
                X4_ASSET_PENDING:X4_ASSET_MISSING;
        }
        ++a->stats.metadata_ready;
        if(source==X4_ASSET_DISK)++a->stats.disk_metadata_hits;
        ++a->revision;
        ok=1;
    }else if(!current_job(a,j))++a->stats.stale_results;
    else ++a->stats.cancelled;
    unlock(&a->mutex);
    record(a,j,X4_ASSET_PUBLISH,source,start,now(a));
    return ok;
}
static void metadata_batch(X4CatalogAssets *a,X4Http **http,X4CatalogDiskCache *disk,Job *jobs,unsigned count,char *json)
{
    Job active[METADATA_BATCH];
    unsigned n=0;
    for(unsigned i=0;i<count;++i)if(needed(a,&jobs[i]))active[n++]=jobs[i];
    if(!n)return;
    char url[512];
    size_t used=(size_t)snprintf(url,sizeof(url),"https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=");
    for(unsigned i=0;i<n;++i)used+=(size_t)snprintf(url+used,sizeof(url)-used,"%s%s",i?",":"",active[i].id);
    snprintf(url+used,sizeof(url)-used,"&market=%s&languages=%s&fieldsTemplate=Details",MARKET,LANGUAGE);
    lock(&a->mutex);
    for(unsigned i=0;i<n;++i){Slot *s=job_slot(a,&active[i]);
    if(s){if(s->metadata_attempts)++a->stats.retries;
    ++s->metadata_attempts;
    }}
    ++a->stats.metadata_network_requests;
    unlock(&a->mutex);
    int status=0;
    size_t length=0;
    uint64_t start=now(a);
    int open_error=0;
    if(!*http)*http=open_http(a,&open_error);
    int rc=*http?x4_http_public_metadata_request(*http,url,json,METADATA_CAP,&length,&status,&a->cancel):open_error?open_error:X4_HTTP_ALLOCATION;
    uint64_t retry_ms=*http?x4_http_public_retry_after_ms(*http):0;
    uint64_t end=now(a);
    record(a,&active[0],X4_ASSET_METADATA,X4_ASSET_NETWORK,start,end);
    if(rc==X4_HTTP_BOUND&&n>1){
        lock(&a->mutex);
        ++a->stats.batch_splits;
        for(unsigned i=0;i<n;++i){Slot *s=job_slot(a,&active[i]);
        if(s&&s->metadata_attempts)--s->metadata_attempts;
        }
        unlock(&a->mutex);
        metadata_batch(a,http,disk,active,n/2u,json);
        metadata_batch(a,http,disk,active+n/2u,n-n/2u,json);
        return;
    }
    Job missing[METADATA_BATCH];
    unsigned missing_count=0;
    for(unsigned i=0;i<n;++i){X4ProductMetadata m;
        X4GameProductDetails *details=&a->game_parse_scratch;
        if(!rc&&status==200&&!x4_product_metadata_parse_details(json,length,active[i].id,&m,details)){
            if(publish_metadata(a,&active[i],&m,X4_ASSET_NETWORK)){
                lock(&a->mutex);
                Slot *s=job_slot(a,&active[i]);
                if(s&&current_job(a,&active[i])){
                    memcpy(&s->game_details,details,sizeof(*details));
                    s->game_details_valid=1;
                }
                unlock(&a->mutex);
            }
            if(disk&&!atomic_load(&a->cancel)){
                char locale[24];
                metadata_cache_locale(locale);
                x4_catalog_disk_cache_put_metadata(disk,active[i].id,MARKET,locale,&m,&a->cancel);
            }
        }else if(!rc&&status==200&&n>1)missing[missing_count++]=active[i];
        else failed(a,&active[i],rc,status,retry_ms);
    }
    if(missing_count){lock(&a->mutex);
    ++a->stats.partial_responses;
    unlock(&a->mutex);
        /* Retry only absent products, at most one immediate singleton each. */
        for(unsigned i=0;i<missing_count;++i)metadata_batch(a,http,disk,&missing[i],1,json);
    }
}
static void evict_background(X4CatalogAssets *a,unsigned slot)
{
    Background *bg=&a->backgrounds[slot];
    if(bg->raster.rgba){
        int owner=find(a,bg->id,bg->generation);
        if(owner>=0)a->slots[owner].image[X4_COVER_BACKGROUND]=(ImageState){0};
        --a->stats.background_count;
        a->stats.background_bytes-=raster_bytes(&bg->raster);
    }
    free(bg->raster.rgba);
    *bg=(Background){0};
}
static int spare_background(X4CatalogAssets *a)
{
    int spare=-1;
    for(unsigned i=0;i<BACKGROUND_SLOTS;++i){
        if((int)i==a->background_active||a->backgrounds[i].pins)continue;
        if(!a->backgrounds[i].raster.rgba)return (int)i;
        if(spare<0)spare=(int)i;
    }
    return spare;
}
static int find_background(const X4CatalogAssets *a,const char *id,uint64_t generation)
{
    int old=-1;
    for(unsigned i=0;i<BACKGROUND_SLOTS;++i){
        const Background *bg=&a->backgrounds[i];
        if(!bg->raster.rgba||strcmp(bg->id,id))continue;
        if(bg->generation==generation)return (int)i;
        if(bg->preloaded&&!bg->pins)old=(int)i;
    }
    return old;
}
static int activate_background(X4CatalogAssets *a,const char *id,uint64_t generation)
{
    int slot=find_background(a,id,generation);
    if(slot>=0){
        Background *bg=&a->backgrounds[slot];
        /* A current, validated foreground request must select this product
         * before showing disk-only public art. Never mutate a borrowed pin. */
        if(bg->generation!=generation&&bg->pins)return 0;
        bg->generation=generation;
        if(a->background_active!=slot){
            a->background_active=slot;
            ++a->stats.ram_hits;
            ++a->revision;
        }
        return 1;
    }
    return 0;
}
static void publish_image(X4CatalogAssets *a,const Job *j,X4CoverRaster *raster,enum X4AssetSource source)
{
    uint64_t start=now(a);
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    if(s&&needed_locked(a,j)){
        ImageState *image=&s->image[j->surface];
        if(j->surface==X4_COVER_BACKGROUND){
            Background *bg=&a->backgrounds[j->background_slot];
            bg->raster=*raster;
            bg->generation=j->generation;
            memcpy(bg->id,j->id,sizeof(bg->id));
            bg->preloaded=j->preload_revision!=0;
#if !X4_PUBLIC
            bg->ready_at=now(a);
            bg->ready_source=source;
#endif
            if(!j->preload_revision||!strcmp(a->background_id,j->id))
                a->background_active=j->background_slot;
            ++a->stats.background_count;
            a->stats.background_bytes+=raster_bytes(raster);
            *raster=(X4CoverRaster){0};
        }else if(make_room(a,raster_bytes(raster))){
            /* Publishing a missing raster cannot invalidate an existing borrow. */
            s->cover.surface[j->surface]=*raster;
            a->stats.thumbnail_bytes+=raster_bytes(raster);
            if(a->stats.thumbnail_bytes>a->stats.thumbnail_peak_bytes)
                a->stats.thumbnail_peak_bytes=a->stats.thumbnail_bytes;
            *raster=(X4CoverRaster){0};
            if(!s->cover_announced){
                ++a->stats.covers_ready;
                s->cover_announced=1;
            }
        }
        if(!raster->rgba){
            image->state=X4_ASSET_READY;
#if !X4_PUBLIC
            image->ready_at=now(a);
            image->ready_source=source;
#endif
            s->used=++a->use_clock;
            if(source==X4_ASSET_DISK)++a->stats.disk_image_hits;
        }else if(!j->preload_revision){
            if(source==X4_ASSET_DISK)++image->attempts;
            image->state=X4_ASSET_TEMPORARY_FAILURE;
            image->due=add_sat(now(a),1000000u);
            s->queued_us=image->due;
            ++a->stats.failures;
        }
        ++a->revision;
    }else if(!current_job(a,j))++a->stats.stale_results;
    else ++a->stats.cancelled;
    unlock(&a->mutex);
    record(a,j,X4_ASSET_PUBLISH,source,start,now(a));
}
void x4_catalog_assets_compose_card(X4CoverRaster *raster)
{
    if (!raster || !raster->rgba) return;
    unsigned start = raster->height * 35u / 100u;
    /* Same ramp, endpoint color and rounding as X4_GRAD_BOTTOM_DARK. */
    for (unsigned y = start + 1u; y < raster->height; ++y) {
        unsigned dark = (y - start) * 215u / (raster->height - start);
        unsigned keep = 256u - dark;
        uint32_t *row = raster->rgba + (size_t)y * raster->width;
        for (unsigned x = 0; x < raster->width; ++x) {
            uint32_t pixel = row[x];
            unsigned r = ((pixel & 255u) * keep + 0x09u * dark) >> 8;
            unsigned g = (((pixel >> 8) & 255u) * keep + 0x0eu * dark) >> 8;
            unsigned b = (((pixel >> 16) & 255u) * keep + 0x1du * dark) >> 8;
            row[x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    }
}

static void defer_image_locked(X4CatalogAssets *a,const Job *job)
{
    Slot *s=job_slot(a,job);
    if(s&&needed_locked(a,job)){
        ImageState *image=&s->image[job->surface];
        if(image->due<a->image_retry_until)image->due=a->image_retry_until;
    }
}
static int download_needed_locked(X4CatalogAssets *a,const Downloader *d,const Job *job)
{
    return d==&a->game_downloader?
        (game_wanted_locked(a,job)&&!atomic_load(&a->game_image_cancel)):needed_locked(a,job);
}
static void *image_downloader(void *argument)
{
    Downloader *d=argument;
    X4CatalogAssets *a=d->assets;
    const int screenshot=d==&a->game_downloader;
    X4Http *http=NULL;
    for(;;){
        lock(&a->mutex);
        if(d->reset_requested){
            unlock(&a->mutex);
            int rc=reset_http(&http);
            lock(&a->mutex);
            d->reset_error=rc;
            d->reset_requested=0;
            unlock(&a->mutex);
            continue;
        }
        if(atomic_load(&a->downloaders_stop)){
            unlock(&a->mutex);
            break;
        }
        if(d->state!=DOWNLOAD_QUEUED){
            unlock(&a->mutex);
            event_wait(&d->wake);
            continue;
        }
        Job job=d->job;
        if(!download_needed_locked(a,d,&job)||now(a)<a->image_retry_until){
            d->deferred=1;
            d->state=DOWNLOAD_DONE;
            unlock(&a->mutex);
            continue;
        }
        d->state=DOWNLOAD_RUNNING;
        unlock(&a->mutex);

        int open_error=0;
        if(!http)http=open_http(a,&open_error);
        lock(&a->mutex);
        /* Opening the context can wait. Recheck the common cooldown at entry. */
        if(!download_needed_locked(a,d,&job)||now(a)<a->image_retry_until){
            d->deferred=1;
            d->state=DOWNLOAD_DONE;
            unlock(&a->mutex);
            continue;
        }
        size_t cap=X4_HTTP_IMAGE_MAX;
        if(screenshot){
            if(job.surface==(int)GAME_FULLSCREEN_SLOT){
                ++a->game.stats.fullscreen_requests;
                cap=X4_GAME_SCREENSHOT_FULL_BYTES;
            }else{
                ++a->game.stats.thumbnail_requests;
                cap=X4_GAME_SCREENSHOT_THUMB_BYTES;
            }
        }else{
            Slot *s=job_slot(a,&job);
            ImageState *image=&s->image[job.surface];
            if(image->attempts)++a->stats.retries;
            ++image->attempts;
            image->state=X4_ASSET_DOWNLOADING;
            ++a->revision;
            ++a->stats.image_network_requests;
            ++a->stats.image_download_requests[d->index];
        }
        unlock(&a->mutex);

        size_t length=0;
        int status=0;
        uint64_t start=now(a);
        int rc=http?x4_http_public_image_request(http,d->url,d->encoded,cap+1u,&length,&status,
                screenshot?&a->game_image_cancel:&a->cancel):
            open_error?open_error:X4_HTTP_ALLOCATION;
        uint64_t end=now(a);
        uint64_t retry_ms=http?x4_http_public_retry_after_ms(http):0;
        lock(&a->mutex);
        if(retry_ms){
            uint64_t delay=retry_ms>UINT64_MAX/1000u?UINT64_MAX:retry_ms*1000u;
            uint64_t deadline=add_sat(end,delay);
            if(deadline>a->image_retry_until)a->image_retry_until=deadline;
        }
        d->rc=rc;
        d->status=status;
        d->retry_ms=retry_ms;
        d->length=length;
        unlock(&a->mutex);
        if(!screenshot)record(a,&job,X4_ASSET_IMAGE,X4_ASSET_NETWORK,start,end);
        lock(&a->mutex);
        d->state=DOWNLOAD_DONE;
        unlock(&a->mutex);
    }
    if(http)x4_http_close(http);
    return NULL;
}
/* Runs only on the coordinator. The background spare is chosen at decode,
 * so overlapping downloads never reserve or allocate extra RGBA rasters. */
static int decode_image(X4CatalogAssets *a,X4CatalogDiskCache *disk,Job job,const char *url,
    const unsigned char *encoded,size_t length,enum X4AssetSource source,uint64_t retry_ms)
{
    if(!needed(a,&job))return 1;
    if(job.surface==X4_COVER_BACKGROUND){
        lock(&a->mutex);
        if(!needed_locked(a,&job)){
            unlock(&a->mutex);
            return 1;
        }
        job.background_slot=spare_background(a);
        if(job.background_slot>=0)evict_background(a,(unsigned)job.background_slot);
        unlock(&a->mutex);
        if(job.background_slot<0)return 0;
    }
    X4CoverRaster raster={0};
    uint64_t start=now(a);
    int rc=x4_product_surface_decode(encoded,length,(enum X4CoverSurface)job.surface,&raster);
    if(!rc&&job.surface==X4_COVER_BACKGROUND)x4_product_compose_background(&raster);
    else if(!rc&&(job.surface==X4_COVER_HOME_FEATURE||job.surface==X4_COVER_HOME_FORTNITE))
        x4_catalog_assets_compose_card(&raster);
    record(a,&job,X4_ASSET_DECODE,source,start,now(a));
    if(rc){
        if(source==X4_ASSET_DISK&&disk){
            x4_catalog_disk_cache_remove_image(disk,job.id,MARKET,LANGUAGE,url);
            lock(&a->mutex);
            Slot *s=job_slot(a,&job);
            if(s)s->image[job.surface].disk_bad=1;
            unlock(&a->mutex);
        }
        if(!job.preload_revision)failed(a,&job,X4_HTTP_PROTOCOL,200,retry_ms);
        return 1;
    }
    /* Cache only successfully decoded public bytes. */
    if(disk&&source==X4_ASSET_NETWORK&&needed(a,&job))
        x4_catalog_disk_cache_put_image(disk,job.id,MARKET,LANGUAGE,url,encoded,length,&a->cancel);
    publish_image(a,&job,&raster,source);
    free(raster.rgba);
    return 1;
}
static Downloader *free_downloader(X4CatalogAssets *a)
{
    for(unsigned i=0;i<a->downloader_count;++i)
        if(a->downloaders[i].state==DOWNLOAD_FREE)return &a->downloaders[i];
    return NULL;
}
/* Returns true if the job and its busy slot have been handed to a worker. */
static int process_image(X4CatalogAssets *a,X4CatalogDiskCache *disk,const Job *job,unsigned char *encoded)
{
    char url[X4_PUBLIC_COVER_URL_CAP];
    lock(&a->mutex);
    Slot *s=job_slot(a,job);
    int valid=s&&x4_product_surface_url(&s->metadata,(enum X4CoverSurface)job->surface,url);
    int bypass=s&&s->image[job->surface].disk_bad;
    unlock(&a->mutex);
    if(!valid||!needed(a,job))return 0;
    size_t length=0;
    uint64_t start=now(a);
    int hit=disk&&!bypass?x4_catalog_disk_cache_get_image(disk,job->id,MARKET,LANGUAGE,url,
        encoded,X4_HTTP_IMAGE_MAX+1u,&length,&a->cancel):0;
    if(hit==1){
        record(a,job,X4_ASSET_IMAGE,X4_ASSET_DISK,start,now(a));
        (void)decode_image(a,disk,*job,url,encoded,length,X4_ASSET_DISK,0);
        return 0;
    }

    /* Warmup is strictly local, including misses and decode failures. */
    if(job->preload_revision)return 0;
    lock(&a->mutex);
    Downloader *d=free_downloader(a);
    if(!d||!needed_locked(a,job)){
        unlock(&a->mutex);
        return 0;
    }
    if(now(a)<a->image_retry_until){
        defer_image_locked(a,job);
        unlock(&a->mutex);
        return 0;
    }
    d->job=*job;
    memcpy(d->url,url,strlen(url)+1u);
    d->sequence=++a->dispatch_sequence;
    d->deferred=0;
    d->state=DOWNLOAD_QUEUED;
    unlock(&a->mutex);
    event_post(&d->wake);
    return 1;
}
static int surface_pending(const Slot *s,unsigned surface)
{
    const ImageState *image=&s->image[surface];
    return (s->wanted&(1u<<surface))&&image->state!=X4_ASSET_READY&&
        image->state!=X4_ASSET_MISSING&&image->attempts<ATTEMPTS_MAX;
}
static int background_is_active(const X4CatalogAssets *a,const Slot *s)
{
    if(a->background_active<0)return 0;
    const Background *bg=&a->backgrounds[a->background_active];
    return bg->generation==s->generation&&!strcmp(bg->id,s->metadata.product_id);
}
static int next_surface(X4CatalogAssets *a,const Slot *s,uint64_t t)
{
    if(!s->wanted||s->busy||s->metadata_missing)return NO_JOB;
    if(!s->metadata_valid)
        return s->metadata_attempts<ATTEMPTS_MAX&&t>=s->metadata_due?METADATA_JOB:NO_JOB;
    if(!background_is_active(a,s)&&surface_pending(s,X4_COVER_BACKGROUND)&&
        t>=s->image[X4_COVER_BACKGROUND].due&&
        spare_background(a)>=0)return X4_COVER_BACKGROUND;
    for(unsigned i=0;i<X4_COVER_SURFACES;++i)
        if(surface_pending(s,i)&&t>=s->image[i].due)return (int)i;
    return NO_JOB;
}
static enum X4CatalogAssetState asset_state(const Slot *s)
{
    if(!s->metadata_valid)return s->state;
    enum X4CatalogAssetState state=X4_ASSET_MISSING;
    for(unsigned i=0;i<X4_COVER_SURFACES;++i){
        if(s->cover.surface[i].rgba)return X4_ASSET_READY;
        if(!(s->wanted&(1u<<i)))continue;
        enum X4CatalogAssetState image=s->image[i].state;
        if(image==X4_ASSET_DOWNLOADING)return image;
        if(image==X4_ASSET_PENDING||image==X4_ASSET_TEMPORARY_FAILURE)state=image;
    }
    return state;
}
static void retire_job_locked(X4CatalogAssets *a,const Job *job)
{
    Slot *s=job_slot(a,job);
    if(!s)return;
    if(!s->wanted&&s->metadata_valid&&!s->cover_announced)++a->stats.deferred_stages;
    s->busy=0;
    if(job->surface>=0){
        ImageState *image=&s->image[job->surface];
        if(image->state==X4_ASSET_DOWNLOADING)image->state=X4_ASSET_PENDING;
    }
}
static void retire_jobs(X4CatalogAssets *a,const Job *jobs,unsigned n)
{
    lock(&a->mutex);
    for(unsigned i=0;i<n;++i)retire_job_locked(a,&jobs[i]);
    unlock(&a->mutex);
}
/* Prefer the earliest assigned job among those available to decode. An old
 * background pinned by the UI must not prevent publishing ready cards. */
static int consume_download(X4CatalogAssets *a,X4CatalogDiskCache *disk)
{
    lock(&a->mutex);
    Downloader *d=NULL;
    for(unsigned i=0;i<a->downloader_count;++i){
        Downloader *candidate=&a->downloaders[i];
        if(candidate->state!=DOWNLOAD_DONE)continue;
        if(candidate->job.surface==X4_COVER_BACKGROUND&&!candidate->deferred&&
            !candidate->rc&&candidate->status==200&&needed_locked(a,&candidate->job)&&
            spare_background(a)<0)continue;
        if(!d||candidate->sequence<d->sequence)d=candidate;
    }
    if(!d){
        unlock(&a->mutex);
        return 0;
    }
    unlock(&a->mutex);
    /* DOWNLOAD_DONE owns a stable buffer until the coordinator retires it. */
    if(d->deferred){
        lock(&a->mutex);
        defer_image_locked(a,&d->job);
        unlock(&a->mutex);
    }else if(d->rc||d->status!=200)failed(a,&d->job,d->rc,d->status,d->retry_ms);
    else if(!decode_image(a,disk,d->job,d->url,d->encoded,d->length,X4_ASSET_NETWORK,d->retry_ms))return 0;

    lock(&a->mutex);
    retire_job_locked(a,&d->job);
    d->state=DOWNLOAD_FREE;
    unlock(&a->mutex);
    return 1;
}
static int begin_game_work_locked(X4CatalogAssets *a)
{
    int gate=atomic_load(&a->run_gate);
    if(gate==1){
        atomic_store(&a->cancel,0);
        if(!atomic_load(&a->suspended)&&atomic_compare_exchange_strong(&a->run_gate,&gate,2))
            gate=2;
    }
    if(gate!=2||atomic_load(&a->cancel)||atomic_load(&a->suspended)||atomic_load(&a->stop))
        return 0;
    atomic_store(&a->busy,1);
    return 1;
}

/* The coordinator shares its existing public metadata HTTP context. An
 * ordinary batch can satisfy this demand before a singleton is needed. */
static void game_fetch_metadata(X4CatalogAssets *a,X4Http **http,X4CatalogDiskCache *disk,
    const Job *job,char *json)
{
    char url[512];
    snprintf(url,sizeof(url),"https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=%s"
        "&market=%s&languages=%s&fieldsTemplate=Details",job->id,MARKET,LANGUAGE);
    int open_error=0;
    if(!*http&&!atomic_load(&a->game_metadata_cancel))*http=open_http(a,&open_error);
    lock(&a->mutex);
    int wanted=game_wanted_locked(a,job)&&!atomic_load(&a->game_metadata_cancel);
    if(wanted&&*http)++a->game.stats.metadata_requests;
    unlock(&a->mutex);
    int status=0;
    size_t length=0;
    int rc=wanted&&*http?x4_http_public_metadata_request(*http,url,json,METADATA_CAP,
        &length,&status,&a->game_metadata_cancel):open_error;
    X4ProductMetadata basic;
    X4GameProductDetails *details=&a->game_parse_scratch;
    int valid=wanted&&*http&&!rc&&status==200&&
        !x4_product_metadata_parse_details(json,length,job->id,&basic,details);

    Job seed={.surface=METADATA_JOB};
    int seed_needed=0;
    lock(&a->mutex);
    if(game_wanted_locked(a,job)&&!atomic_load(&a->game_metadata_cancel)){
        GameAssets *game=&a->game;
        game->metadata_state=valid?X4_ASSET_READY:
            (temporary(rc,status)?X4_ASSET_TEMPORARY_FAILURE:X4_ASSET_MISSING);
        if(valid){
            memcpy(&game->details,details,sizeof(*details));
            uint64_t generation=atomic_load(&a->generation);
            int i=find(a,job->id,generation);
            if(i<0){
                i=allocate_slot(a);
                if(i>=0)occupy_slot(a,&a->slots[i],job->id,generation);
            }
            if(i>=0){
                Slot *s=&a->slots[i];
                memcpy(&s->game_details,details,sizeof(*details));
                s->game_details_valid=1;
                if(!s->metadata_valid&&!s->busy){
                    seed.slot=(unsigned)i;
                    seed.generation=generation;
                    seed.account=a->sigl_session;
                    memcpy(seed.id,job->id,sizeof(seed.id));
                    seed_needed=1;
                }
            }
        }
        game_changed(a);
    }else{
        ++a->game.stats.cancelled;
        if(job->generation==a->game.request&&a->game.metadata_state==X4_ASSET_DOWNLOADING)
            a->game.metadata_state=wanted&&*http?X4_ASSET_TEMPORARY_FAILURE:X4_ASSET_PENDING;
        game_changed(a);
    }
    unlock(&a->mutex);
    if(seed_needed&&publish_metadata(a,&seed,&basic,X4_ASSET_NETWORK)&&
        disk&&!atomic_load(&a->game_metadata_cancel)){
        char locale[24];
        metadata_cache_locale(locale);
        (void)x4_catalog_disk_cache_put_metadata(disk,job->id,MARKET,locale,
            &basic,&a->game_metadata_cancel);
    }
}

static size_t game_encoded_limit(const Job *job)
{
    return job->surface==(int)GAME_FULLSCREEN_SLOT?
        X4_GAME_SCREENSHOT_FULL_BYTES:X4_GAME_SCREENSHOT_THUMB_BYTES;
}

/* Both screenshot sizes use the existing coordinator decoder and validated
 * disk image records. A URL's dimensions keep preview and viewer distinct. */
static void game_finish_image(X4CatalogAssets *a,X4CatalogDiskCache *disk,const Job *job,
    const char *url,const unsigned char *encoded,size_t length,int disk_hit,
    enum X4CatalogAssetState failure)
{
    X4CoverRaster raster={0};
    lock(&a->mutex);
    int wanted=game_wanted_locked(a,job)&&!atomic_load(&a->game_image_cancel);
    unlock(&a->mutex);
    if(wanted&&length&&length<=game_encoded_limit(job)){
        enum X4CoverSurface surface=job->surface==(int)GAME_FULLSCREEN_SLOT?
            X4_COVER_BACKGROUND:X4_COVER_DETAIL;
        if(x4_product_surface_decode(encoded,length,surface,&raster)){
            if(disk_hit&&disk&&!atomic_load(&a->game_image_cancel))
                (void)x4_catalog_disk_cache_remove_image(disk,job->id,MARKET,LANGUAGE,url);
            if(disk_hit)failure=X4_ASSET_PENDING;
        }else if(disk&&!disk_hit&&!atomic_load(&a->game_image_cancel)){
            (void)x4_catalog_disk_cache_put_image(disk,job->id,MARKET,LANGUAGE,url,
                encoded,length,&a->game_image_cancel);
        }
    }
    lock(&a->mutex);
    GameAssets *game=&a->game;
    if(game_wanted_locked(a,job)&&!atomic_load(&a->game_image_cancel)){
        GameImage *image=&game->images[job->surface];
        if(raster.rgba){
            image->raster=raster;
            raster.rgba=NULL;
            image->state=X4_ASSET_READY;
            game->stats.raster_bytes+=(size_t)image->raster.width*image->raster.height*sizeof(uint32_t);
            if(game->stats.raster_bytes>game->stats.peak_raster_bytes)
                game->stats.peak_raster_bytes=game->stats.raster_bytes;
            if(disk_hit)++game->stats.disk_hits;
        }else image->state=failure;
        game_changed(a);
    }else{
        ++game->stats.cancelled;
        if(job->generation==game->request&&game->images[job->surface].state==X4_ASSET_DOWNLOADING)
            game->images[job->surface].state=X4_ASSET_PENDING;
    }
    unlock(&a->mutex);
    free(raster.rgba);
}

static int consume_game_download(X4CatalogAssets *a,X4CatalogDiskCache *disk)
{
    Downloader *d=&a->game_downloader;
    lock(&a->mutex);
    int done=d->state==DOWNLOAD_DONE;
    unlock(&a->mutex);
    if(!done)return 0;
    /* DOWNLOAD_DONE pins the encoded buffer until the coordinator retires it. */
    enum X4CatalogAssetState failure=d->deferred?X4_ASSET_PENDING:
        (temporary(d->rc,d->status)?X4_ASSET_TEMPORARY_FAILURE:X4_ASSET_MISSING);
    game_finish_image(a,disk,&d->job,d->url,d->encoded,
        !d->deferred&&!d->rc&&d->status==200?d->length:0,0,failure);
    lock(&a->mutex);
    d->state=DOWNLOAD_FREE;
    unlock(&a->mutex);
    return 1;
}

static int game_image_slot_locked(GameAssets *game)
{
    int index=game->requested_full;
    if(index>=0&&(unsigned)index<game->details.details.screenshot_count){
        GameImage *full=&game->images[GAME_FULLSCREEN_SLOT];
        if(game->full_index!=index&&!full->pins){
            game_reset_image(game,GAME_FULLSCREEN_SLOT);
            game->full_index=index;
        }
        if(game->full_index==index&&full->state==X4_ASSET_PENDING)return (int)GAME_FULLSCREEN_SLOT;
    }
    for(unsigned i=0;i<game->details.details.screenshot_count;++i)
        if(game->images[i].state==X4_ASSET_PENDING)return (int)i;
    return -1;
}

static int process_game_work(X4CatalogAssets *a,X4Http **http,X4CatalogDiskCache *disk,
    char *json,unsigned char *encoded)
{
    lock(&a->mutex);
    GameAssets *game=&a->game;
    if(!game->id[0]){
        unlock(&a->mutex);
        return 0;
    }
    if(strcmp(game->id,game->cached_id)){
        for(unsigned i=0;i<GAME_IMAGE_COUNT;++i)if(game->images[i].pins){
            unlock(&a->mutex);
            return 0;
        }
        game_reset_cache(game);
        memcpy(game->cached_id,game->id,sizeof(game->cached_id));
    }
    Job job={.generation=game->request,.surface=METADATA_JOB};
    memcpy(job.id,game->id,sizeof(job.id));
    if(game->metadata_state==X4_ASSET_PENDING){
        int i=find(a,game->id,atomic_load(&a->generation));
        if(i>=0&&a->slots[i].game_details_valid){
            memcpy(&game->details,&a->slots[i].game_details,sizeof(game->details));
            game->metadata_state=X4_ASSET_READY;
            game_changed(a);
            unlock(&a->mutex);
            return 1;
        }
        if(!begin_game_work_locked(a)){
            unlock(&a->mutex);
            return 0;
        }
        game->metadata_state=X4_ASSET_DOWNLOADING;
        atomic_store(&a->game_metadata_cancel,0);
        game_changed(a);
        unlock(&a->mutex);
        game_fetch_metadata(a,http,disk,&job,json);
        return 1;
    }

    Downloader *d=&a->game_downloader;
    if(game->metadata_state!=X4_ASSET_READY||d->state!=DOWNLOAD_FREE||now(a)<a->image_retry_until){
        unlock(&a->mutex);
        return 0;
    }
    job.surface=game_image_slot_locked(game);
    if(job.surface<0||!begin_game_work_locked(a)){
        unlock(&a->mutex);
        return 0;
    }
    int fullscreen=job.surface==(int)GAME_FULLSCREEN_SLOT;
    job.slot=(unsigned)(fullscreen?game->full_index:job.surface);
    char url[X4_PUBLIC_COVER_URL_CAP];
    if(!x4_product_screenshot_url(&game->details,job.slot,fullscreen,url)){
        game->images[job.surface].state=X4_ASSET_MISSING;
        game_changed(a);
        unlock(&a->mutex);
        return 1;
    }
    game->images[job.surface].state=X4_ASSET_DOWNLOADING;
    atomic_store(&a->game_image_cancel,0);
    unlock(&a->mutex);

    size_t length=0;
    int hit=disk?x4_catalog_disk_cache_get_image(disk,job.id,MARKET,LANGUAGE,url,
        encoded,game_encoded_limit(&job)+1u,&length,&a->game_image_cancel):0;
    if(hit==1){
        game_finish_image(a,disk,&job,url,encoded,length,1,X4_ASSET_MISSING);
        return 1;
    }

    lock(&a->mutex);
    if(game_wanted_locked(a,&job)&&!atomic_load(&a->game_image_cancel)){
        d->job=job;
        memcpy(d->url,url,strlen(url)+1u);
        d->deferred=0;
        d->state=DOWNLOAD_QUEUED;
        unlock(&a->mutex);
        event_post(&d->wake);
    }else{
        ++game->stats.cancelled;
        if(job.generation==game->request)
            game->images[job.surface].state=X4_ASSET_PENDING;
        unlock(&a->mutex);
    }
    return 1;
}
static unsigned select_jobs(X4CatalogAssets *a,uint64_t generation,uint64_t t,int allow_image,Job jobs[METADATA_BATCH])
{
    size_t background=a->background_id[0]?1u:0u;
    size_t count=a->demand_count+background;
    unsigned n=0;
    for(size_t d=0;d<count&&n<METADATA_BATCH;++d){
        const char *id=d<background?a->background_id:a->demand[d-background];
        int i=find(a,id,generation);
        if(i<0||a->slots[i].busy)continue;
        int duplicate=0;
        for(unsigned j=0;j<n;++j)if(jobs[j].slot==(unsigned)i)duplicate=1;
        if(duplicate)continue;
        int surface=next_surface(a,&a->slots[i],t);
        /* An open game gets at most the one UI detail request. Existing
         * catalogue work completed before opening still seeds this cache. */
        if(surface==METADATA_JOB&&game_requested(a,id))continue;
        if(surface==NO_JOB||(surface>=0&&!allow_image)||(n&&surface!=METADATA_JOB))continue;
        jobs[n]=(Job){.slot=(unsigned)i,.generation=generation,.account=a->sigl_session,.surface=surface};
        memcpy(jobs[n++].id,id,strlen(id)+1u);
        if(surface!=METADATA_JOB)break;
    }
    return n;
}
/* A single disk-only job is selected after all normal work. Attempts are
 * scoped to the current plan so a disk miss cannot spin or block later GETs. */
static unsigned select_preload(X4CatalogAssets *a,uint64_t generation,Job *job)
{
    for(size_t d=0;d<a->preload_count;++d){
        Preload *entry=&a->preload[d];
        uint32_t pending=entry->demand.mask&~entry->attempted;
        if(!pending)continue;
        const char *id=entry->demand.id;
        int i=find(a,id,generation);
        if(i<0){
            if(entry->metadata_attempted)continue;
            i=allocate_slot(a);
            if(i<0)continue;
            occupy_slot(a,&a->slots[i],id,generation);
        }
        Slot *s=&a->slots[i];
        if(s->busy||s->metadata_missing)continue;
        int surface=METADATA_JOB;
        if(!s->metadata_valid){
            if(entry->metadata_attempted)continue;
            entry->metadata_attempted=1;
        }else{
            surface=NO_JOB;
            for(unsigned j=0;j<=X4_COVER_SURFACES;++j){
                if(!(pending&(1u<<j)))continue;
                if(j==X4_COVER_BACKGROUND&&spare_background(a)<0&&
                    find_background(a,id,generation)<0)continue;
                entry->attempted|=1u<<j;
                int resident=j==X4_COVER_BACKGROUND?find_background(a,id,generation)>=0:
                    s->cover.surface[j].rgba!=NULL;
                if(resident){
                    s->used=++a->use_clock;
                    continue;
                }
                if(s->image[j].state==X4_ASSET_MISSING||s->image[j].disk_bad)continue;
                surface=(int)j;
                break;
            }
            if(surface==NO_JOB)continue;
        }
        *job=(Job){.slot=(unsigned)i,.surface=surface,.generation=generation,
            .account=a->sigl_session,.preload_revision=a->preload_revision};
        memcpy(job->id,id,strlen(id)+1u);
        return 1;
    }
    return 0;
}
static int next_sigl(const X4CatalogAssets *a)
{
    if(!a->sigl_session)return -1;
    for(unsigned i=0;i<X4_SIGL_COUNT;++i)
        if(!(a->sigl_started&(1u<<i)))return (int)i;
    return -1;
}
static void process_sigl(X4CatalogAssets *a,X4Http **http,int kind,uint64_t session,char *json,X4SiglList *parsed)
{
    char url[160];
    size_t length=0;
    int status=0,open_error=0,valid=0;
    if(x4_sigl_url((X4SiglKind)kind,url,sizeof(url))&&!atomic_load(&a->cancel)){
        if(!*http)*http=open_http(a,&open_error);
        if(*http&&!x4_http_public_metadata_request(*http,url,json,METADATA_CAP,&length,&status,&a->cancel)&&status==200)
            valid=x4_sigl_parse(parsed,json,length);
    }
    lock(&a->mutex);
    if(a->sigl_session==session&&atomic_load(&a->cancel)){
        /* A cancelled fetch does not exhaust this account's one opportunity. */
        a->sigl_started&=~(1u<<(unsigned)kind);
    }else if(a->sigl_session==session){
        X4SiglList *list=&a->sigls[kind];
        list->count=valid?parsed->count:0;
        list->valid=valid!=0;
        list->pending=false;
        if(valid)memcpy(list->ids,parsed->ids,parsed->count*sizeof(parsed->ids[0]));
        ++a->sigl_revision;
    }
    unlock(&a->mutex);
}
#if !defined(X4_ASSETS_HOST_TEST) && !X4_PUBLIC
typedef struct {
    uint64_t next_us, activity;
} CoverSummary;
static void diagnostic_summary(X4CatalogAssets *a,X4CatalogDiskCache *disk,CoverSummary *summary)
{
    uint64_t t=now(a);
    if(t<summary->next_us)return;
    summary->next_us=add_sat(t,10000000u);
    X4CatalogAssetStats s;
    lock(&a->mutex);
    s=a->stats;
    unlock(&a->mutex);
    X4CatalogDiskStats d={0};
    if(disk)x4_catalog_disk_cache_stats(disk,&d);
    uint64_t activity=s.requests+s.ram_hits+s.sigl_requests+s.failures+s.cancelled+
        s.stale_results+d.misses+d.evictions+d.io_errors+d.stores;
    for(unsigned i=0;i<X4_ASSET_STAGE_COUNT;++i)activity+=s.stage[i].count;
    if(activity==summary->activity)return;
    summary->activity=activity;
    x4_log_printf("XCloud4: COVER_METRICS ram=%llu disk_metadata=%llu disk_image=%llu metadata_get=%llu image_get=%llu splits=%llu partial=%llu retry=%llu deferred=%llu omitted=%llu image_worker_1=%llu image_worker_2=%llu image_worker_3=%llu disk_enabled=%d records=%u bytes=%zu evictions=%llu scan_ms=%llu io_errors=%llu miss_absent=%llu miss_expired=%llu miss_corrupt=%llu miss_key=%llu miss_evicted=%llu miss_filtered=%llu\n",
        (unsigned long long)s.ram_hits,(unsigned long long)s.disk_metadata_hits,(unsigned long long)s.disk_image_hits,
        (unsigned long long)s.metadata_network_requests,(unsigned long long)s.image_network_requests,
        (unsigned long long)s.batch_splits,(unsigned long long)s.partial_responses,(unsigned long long)s.retries,
        (unsigned long long)s.deferred_stages,(unsigned long long)s.events_omitted,
        (unsigned long long)s.image_download_requests[0],(unsigned long long)s.image_download_requests[1],
        (unsigned long long)s.image_download_requests[2],s.disk_enabled,d.records,d.bytes,
        (unsigned long long)d.evictions,(unsigned long long)d.scan_ms,(unsigned long long)d.io_errors,
        (unsigned long long)d.miss_absent,(unsigned long long)d.miss_expired,(unsigned long long)d.miss_corrupt,
        (unsigned long long)d.miss_key,(unsigned long long)d.miss_evicted,(unsigned long long)d.miss_filtered);
}
static void diagnostic_stages(X4CatalogAssets *a)
{
    X4CatalogAssetStats s;
    lock(&a->mutex);
    s=a->stats;
    unlock(&a->mutex);
    for(unsigned i=0;i<X4_ASSET_STAGE_COUNT;++i)
        x4_log_printf("XCloud4: COVER_STAGE stage=%u count=%llu total_us=%llu max_us=%llu\n",i,
            (unsigned long long)s.stage[i].count,(unsigned long long)s.stage[i].total_us,
            (unsigned long long)s.stage[i].max_us);
}
#endif
static void request_cleanup(X4CatalogAssets *a)
{
    atomic_store(&a->cancel,1);
    atomic_store(&a->game_metadata_cancel,1);
    atomic_store(&a->game_image_cancel,1);
    int gate=atomic_load(&a->run_gate);
    while(!atomic_compare_exchange_weak(&a->run_gate,&gate,gate==2||gate==3?3:gate==1?4:gate)){}
}
static int cleanup_connections(X4CatalogAssets *a,X4Http **http)
{
    atomic_store(&a->cancel,1);
    atomic_store(&a->game_metadata_cancel,1);
    atomic_store(&a->game_image_cancel,1);
    atomic_store(&a->run_gate,4);
    /* Do not tear down one network owner while another is still in a request. */
    for(;;){
        int running=0;
        lock(&a->mutex);
        for(unsigned i=0;i<=a->downloader_count;++i)
            if(downloader_at(a,i)->state==DOWNLOAD_RUNNING)running=1;
        if(!running){
            for(unsigned i=0;i<=a->downloader_count;++i)downloader_at(a,i)->reset_requested=1;
        }
        unlock(&a->mutex);
        if(!running)break;
        pause_worker();
    }
    for(unsigned i=0;i<=a->downloader_count;++i)event_post(&downloader_at(a,i)->wake);
    int rc=reset_http(http);
    for(;;){
        int pending=0;
        lock(&a->mutex);
        for(unsigned i=0;i<=a->downloader_count;++i){
            Downloader *d=downloader_at(a,i);
            if(d->reset_requested)pending=1;
            else if(!rc&&d->reset_error)rc=d->reset_error;
        }
        if(!pending){
            for(unsigned i=0;i<a->downloader_count;++i){
                Downloader *d=&a->downloaders[i];
                if(d->state!=DOWNLOAD_FREE){
                    if(!current_job(a,&d->job))++a->stats.stale_results;
                    else ++a->stats.cancelled;
                    retire_job_locked(a,&d->job);
                    d->state=DOWNLOAD_FREE;
                }
            }
            if(a->game_downloader.state!=DOWNLOAD_FREE){
                ++a->game.stats.cancelled;
                a->game_downloader.state=DOWNLOAD_FREE;
            }
            if(a->game.metadata_state==X4_ASSET_DOWNLOADING)a->game.metadata_state=X4_ASSET_PENDING;
            for(unsigned i=0;i<GAME_IMAGE_COUNT;++i)
                if(a->game.images[i].state==X4_ASSET_DOWNLOADING)
                    a->game.images[i].state=X4_ASSET_PENDING;
            if(rc)++a->stats.cleanup_failures;
        }
        unlock(&a->mutex);
        if(!pending)break;
        pause_worker();
    }
    atomic_store(&a->busy,0);
    if(rc){
        atomic_store(&a->suspended,1);
        atomic_store(&a->unavailable,X4_ASSET_FAULT_CLEANUP);
        return rc; /* Do not release the auth handoff gate on quarantine. */
    }
#if !defined(X4_ASSETS_HOST_TEST) && !X4_PUBLIC
    diagnostic_stages(a);
#endif
    atomic_store(&a->run_gate,atomic_load(&a->suspended)?0:1);
    return 0;
}
static void process_metadata(X4CatalogAssets *a,X4Http **http,X4CatalogDiskCache *disk,
    Job jobs[METADATA_BATCH],unsigned n,char *json)
{
    Job network[METADATA_BATCH];
    unsigned count=0;
    for(unsigned i=0;i<n;++i){
        X4ProductMetadata m;
        uint64_t start=now(a);
        char locale[24];
        metadata_cache_locale(locale);
        int hit=disk?x4_catalog_disk_cache_get_metadata(disk,jobs[i].id,MARKET,locale,&m,&a->cancel):0;
        if(hit==1){
            record(a,&jobs[i],X4_ASSET_METADATA,X4_ASSET_DISK,start,now(a));
            publish_metadata(a,&jobs[i],&m,X4_ASSET_DISK);
        }else if(!jobs[i].preload_revision)network[count++]=jobs[i];
    }
    if(count)metadata_batch(a,http,disk,network,count,json);
}
static void apply_disk_protection(X4CatalogAssets *a,X4CatalogDiskCache *disk,uint64_t *applied)
{
    if(!disk)return;
    char ids[X4_CATALOG_DISK_PROTECTED][X4_PRODUCT_ID_CAP];
    lock(&a->mutex);
    if(a->protected_revision==*applied){
        unlock(&a->mutex);
        return;
    }
    *applied=a->protected_revision;
    unsigned count=a->protected_count;
    memcpy(ids,a->protected_ids,count*sizeof(ids[0]));
    unlock(&a->mutex);
    x4_catalog_disk_cache_protect_products(disk,ids,count);
}
static void *asset_worker(void *argument)
{
    X4CatalogAssets *a=argument;
    X4Http *http=NULL;
    X4CatalogDiskCache *disk=x4_catalog_disk_cache_open(a->disk_root[0]?a->disk_root:NULL,X4_CATALOG_DISK_BUDGET,X4_CATALOG_DISK_PRODUCTS,NULL,NULL);
    lock(&a->mutex);
    a->stats.disk_enabled=disk!=NULL;
    unlock(&a->mutex);
    char *json=malloc(METADATA_CAP);
    unsigned char *encoded=malloc(X4_HTTP_IMAGE_MAX+1u);
    X4SiglList *parsed=malloc(sizeof(*parsed));
    if(!json||!encoded||!parsed){
        free(json);
        free(encoded);
        free(parsed);
        x4_catalog_disk_cache_close(disk);
        atomic_store(&a->suspended,1);
        atomic_store(&a->unavailable,X4_ASSET_FAULT_ALLOCATION);
        atomic_store(&a->run_gate,0);
        return NULL;
    }
    int prefer_sigl=0;
    uint64_t protected_applied=0;
#if !defined(X4_ASSETS_HOST_TEST) && !X4_PUBLIC
    CoverSummary summary={.next_us=add_sat(now(a),10000000u)};
#endif
    while(!atomic_load(&a->stop)){
#if !defined(X4_ASSETS_HOST_TEST) && !X4_PUBLIC
        diagnostic_summary(a,disk,&summary);
#endif
        apply_disk_protection(a,disk,&protected_applied);
        int gate=atomic_load(&a->run_gate);
        if(gate==3||gate==4){
            if(cleanup_connections(a,&http)){
                while(!atomic_load(&a->stop))pause_worker();
                break;
            }
            continue;
        }
        if(atomic_load(&a->suspended)){
            pause_worker();
            continue;
        }
        if(gate==0){
            atomic_compare_exchange_strong(&a->run_gate,&gate,1);
            continue;
        }
        if(consume_download(a,disk)||consume_game_download(a,disk)||
            process_game_work(a,&http,disk,json,encoded))continue;

        Job jobs[METADATA_BATCH];
        unsigned n=0;
        uint64_t t=now(a);
        lock(&a->mutex);
        uint64_t generation=atomic_load(&a->generation);
        fill_demand(a,generation);
        int sigl=next_sigl(a);
        uint64_t sigl_session=a->sigl_session;
        int allow_image=free_downloader(a)!=NULL;
        if(!prefer_sigl||sigl<0)n=select_jobs(a,generation,t,allow_image,jobs);
        if(n)sigl=-1;
        gate=atomic_load(&a->run_gate);
        int downloads_idle=a->game_downloader.state==DOWNLOAD_FREE;
        for(unsigned i=0;i<a->downloader_count;++i)
            if(a->downloaders[i].state!=DOWNLOAD_FREE)downloads_idle=0;
        if(disk&&!n&&sigl<0&&(gate==1||gate==2)&&downloads_idle)
            n=select_preload(a,generation,&jobs[0]);
        if(n||sigl>=0){
            if(gate==1){
                atomic_store(&a->cancel,0);
                if(!atomic_load(&a->suspended)&&atomic_compare_exchange_strong(&a->run_gate,&gate,2))
                    gate=2;
            }
            if(gate!=2||atomic_load(&a->cancel)||atomic_load(&a->suspended)){
                n=0;
                sigl=-1;
            }else{
                atomic_store(&a->busy,1);
                if(sigl>=0){
                    a->sigl_started|=1u<<(unsigned)sigl;
                    ++a->stats.sigl_requests;
                }
                for(unsigned i=0;i<n;++i)a->slots[jobs[i].slot].busy=1;
            }
        }else if(gate==2){
            if(downloads_idle&&atomic_compare_exchange_strong(&a->run_gate,&gate,1))
                atomic_store(&a->busy,0);
        }
        unlock(&a->mutex);
        if(sigl>=0){
            process_sigl(a,&http,sigl,sigl_session,json,parsed);
            prefer_sigl=0;
            continue;
        }
        if(!n){
            pause_worker();
            continue;
        }
        prefer_sigl=1;
        for(unsigned i=0;i<n;++i){
            lock(&a->mutex);
            uint64_t queued=a->slots[jobs[i].slot].queued_us;
            unlock(&a->mutex);
            record(a,&jobs[i],X4_ASSET_QUEUE,X4_ASSET_RAM,queued,now(a));
        }
        if(jobs[0].surface!=METADATA_JOB){
            if(!process_image(a,disk,&jobs[0],encoded))retire_jobs(a,jobs,1);
        }else{
            process_metadata(a,&http,disk,jobs,n,json);
            retire_jobs(a,jobs,n);
        }
    }
    if(!atomic_load(&a->unavailable)) (void)cleanup_connections(a,&http);
    if(http)x4_http_close(http);
    x4_catalog_disk_cache_close(disk);
    free(json);
    free(encoded);
    free(parsed);
    return NULL;
}
static void stop_downloaders(X4CatalogAssets *a,unsigned count)
{
    atomic_store(&a->downloaders_stop,1);
    for(unsigned i=0;i<count;++i)event_post(&downloader_at(a,i)->wake);
    for(unsigned i=0;i<count;++i){
        Downloader *d=downloader_at(a,i);
        join_thread(d->thread);
        event_destroy(&d->wake);
        free(d->encoded);
    }
}
X4CatalogAssets *x4_catalog_assets_create_config(const X4CatalogAssetsConfig *config)
{
    unsigned count=X4_CATALOG_IMAGE_DOWNLOADERS;
#ifdef X4_ASSETS_HOST_TEST
    count=config&&config->image_downloaders?config->image_downloaders:1;
    if(count>X4_CATALOG_IMAGE_DOWNLOADERS)return NULL;
#endif
    X4CatalogAssets *a=calloc(1,sizeof(*a));
    if(!a)return NULL;
    a->clock=config&&config->clock_us?config->clock_us:platform_clock;
    a->clock_context=config?config->clock_context:NULL;
    a->background_active=-1;
    if(config&&config->disk_root){
        size_t n=strlen(config->disk_root);
        if(n>=sizeof(a->disk_root)){
            free(a);
            return NULL;
        }
        memcpy(a->disk_root,config->disk_root,n+1u);
    }
    a->downloader_count=count;
    atomic_init(&a->generation,1);
    atomic_init(&a->suspended,1);
    atomic_init(&a->stop,0);
    atomic_init(&a->downloaders_stop,0);
    atomic_init(&a->unavailable,0);
    atomic_init(&a->cancel,0);
    atomic_init(&a->busy,0);
    atomic_init(&a->run_gate,0);
    atomic_init(&a->game_revision,0);
    atomic_init(&a->game_metadata_cancel,1);
    atomic_init(&a->game_image_cancel,1);
    a->game.requested_full=-1;
    a->game.full_index=-1;
    if(mutex_init(&a->mutex)){
        free(a);
        return NULL;
    }
    if(mutex_init(&a->http_open_mutex)){
        mutex_destroy(&a->mutex);
        free(a);
        return NULL;
    }
    unsigned started=0;
    while(started<=count){
        static const char *const names[]={"x4-cover-1","x4-cover-2","x4-cover-3"};
        Downloader *d=downloader_at(a,started);
        const char *name=started==count?"x4-game-screenshots":names[started];
        d->assets=a;
        d->index=started;
        d->encoded=malloc(X4_HTTP_IMAGE_MAX+1u);
        if(!d->encoded)break;
        if(event_init(&d->wake)){
            free(d->encoded);
            break;
        }
        if(start_thread(&d->thread,image_downloader,d,name)){
            event_destroy(&d->wake);
            free(d->encoded);
            break;
        }
        ++started;
    }
    if(started==count+1u&&!start_thread(&a->thread,asset_worker,a,"x4-ui-assets"))return a;
    stop_downloaders(a,started);
    mutex_destroy(&a->http_open_mutex);
    mutex_destroy(&a->mutex);
    free(a);
    return NULL;
}
X4CatalogAssets *x4_catalog_assets_create(void)
{
#ifdef X4_ASSETS_HOST_TEST
    return x4_catalog_assets_create_config(NULL);
#else
    X4CatalogAssetsConfig c={.disk_root=X4_CATALOG_DISK_ROOT};
    return x4_catalog_assets_create_config(&c);
#endif
}
void x4_catalog_assets_suspend(X4CatalogAssets *a)
{
    if(!a)return;
    atomic_store(&a->suspended,1);
    request_cleanup(a);
}
void x4_catalog_assets_resume(X4CatalogAssets *a)
{
    if(!a||atomic_load(&a->unavailable))return;
    atomic_store(&a->suspended,0);
    int gate=0;
    atomic_compare_exchange_strong(&a->run_gate,&gate,1);
}
int x4_catalog_assets_idle(X4CatalogAssets *a)
{ if(!a)return 1;
int gate=atomic_load(&a->run_gate);
return gate==0||gate==1;
}
enum X4AssetFault x4_catalog_assets_fault(X4CatalogAssets *a)
{ return a?(enum X4AssetFault)atomic_load(&a->unavailable):X4_ASSET_FAULT_ALLOCATION;
}
static void game_request_locked(X4CatalogAssets *a,const char *id,int fullscreen)
{
    GameAssets *game=&a->game;
    Downloader *d=&a->game_downloader;
    if(strcmp(game->id,id)){
        atomic_store(&a->game_metadata_cancel,1);
        atomic_store(&a->game_image_cancel,1);
        ++game->request;
        memcpy(game->id,id,strlen(id)+1);
        if(game->metadata_state!=X4_ASSET_READY)game->metadata_state=X4_ASSET_PENDING;
        for(unsigned i=0;i<GAME_IMAGE_COUNT;++i)
            if(game->images[i].state!=X4_ASSET_READY)game->images[i].state=X4_ASSET_PENDING;
    }else if(game->requested_full==fullscreen)return;
    else if(d->state!=DOWNLOAD_FREE&&d->job.surface==(int)GAME_FULLSCREEN_SLOT&&
        (int)d->job.slot!=fullscreen)atomic_store(&a->game_image_cancel,1);
    game->requested_full=fullscreen;
    game_changed(a);
}
static void clear_demand_locked(X4CatalogAssets *a)
{
    a->demand_count=0;
    a->preload_count=0;
    game_request_locked(a,"",-1);
    ++a->preload_revision;
    a->background_active=-1;
    a->background_id[0]=0;
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)a->slots[i].wanted=0;
    ++a->revision;
}
void x4_catalog_assets_set_generation(X4CatalogAssets *a,uint64_t generation)
{
    if(!a||!generation||generation==atomic_load(&a->generation))return;
    lock(&a->mutex);
    atomic_store(&a->generation,generation);
    clear_demand_locked(a);
    request_cleanup(a);
    unlock(&a->mutex);
}
void x4_catalog_assets_request(X4CatalogAssets *a,const char *const *ids,size_t count)
{
    x4_catalog_assets_request_surfaces(a,ids,NULL,count);
}
void x4_catalog_assets_request_surfaces(X4CatalogAssets *a,const char *const *ids,
    const uint32_t *masks,size_t count)
{
    if(!a||(!ids&&count)||trylock(&a->mutex))return;
    char demand[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP]={{0}};
    uint32_t wanted[X4_CATALOG_ASSET_SLOTS]={0};
    size_t n=0;
    if(count>X4_CATALOG_ASSET_SLOTS)count=X4_CATALOG_ASSET_SLOTS;
    for(size_t i=0;i<count;++i){
        uint32_t mask=masks?masks[i]&THUMBNAIL_MASK:LEGACY_MASK;
        if(!mask||!x4_product_id_valid(ids[i]))continue;
        size_t d=0;
        while(d<n&&strcmp(demand[d],ids[i]))++d;
        if(d==n)memcpy(demand[n++],ids[i],strlen(ids[i])+1u);
        wanted[d]|=mask;
    }
    int changed=n!=a->demand_count;
    for(size_t i=0;i<n&&!changed;++i)
        if(strcmp(a->demand[i],demand[i])||a->demand_masks[i]!=wanted[i])changed=1;
    if(changed)for(size_t i=0;i<n;++i){
        int hit=find(a,demand[i],atomic_load(&a->generation));
        if(hit<0)continue;
        for(unsigned j=0;j<X4_COVER_SURFACES;++j)
            if((wanted[i]&(1u<<j))&&a->slots[hit].cover.surface[j].rgba){
                ++a->stats.ram_hits;
                break;
            }
    }
    memcpy(a->demand,demand,sizeof(demand));
    memcpy(a->demand_masks,wanted,sizeof(wanted));
    a->demand_count=n;
    fill_demand(a,atomic_load(&a->generation));
    unlock(&a->mutex);
}
int x4_catalog_assets_preload(X4CatalogAssets *a,const X4CatalogArtDemand *demand,size_t count)
{
    if(!a||(!demand&&count)||count>X4_CATALOG_PRELOAD_PRODUCTS||trylock(&a->mutex))return 0;
    int changed=a->preload_count!=count;
    for(size_t i=0;i<count;++i){
        if(!x4_product_id_valid(demand[i].id)||!(demand[i].mask&(BACKGROUND_MASK|THUMBNAIL_MASK))){
            unlock(&a->mutex);
            return 0;
        }
        if(!changed&&(strcmp(a->preload[i].demand.id,demand[i].id)||
            a->preload[i].demand.mask!=demand[i].mask))changed=1;
    }
    if(changed){
        ++a->preload_revision;
        a->preload_count=count;
        for(size_t i=0;i<count;++i)a->preload[i]=(Preload){.demand=demand[i]};
    }
    unlock(&a->mutex);
    return 1;
}
void x4_catalog_assets_protect(X4CatalogAssets *a,const X4CatalogArtDemand *demand,size_t count)
{
    if(!a||(!demand&&count)||count>X4_CATALOG_DISK_PROTECTED||trylock(&a->mutex))return;
    int changed=a->protected_count!=count;
    for(size_t i=0;i<count;++i){
        if(!x4_product_id_valid(demand[i].id)){
            unlock(&a->mutex);
            return;
        }
        if(!changed&&strcmp(a->protected_ids[i],demand[i].id))changed=1;
    }
    if(changed){
        ++a->protected_revision;
        a->protected_count=(unsigned)count;
        for(size_t i=0;i<count;++i)memcpy(a->protected_ids[i],demand[i].id,strlen(demand[i].id)+1u);
    }
    unlock(&a->mutex);
}
#if !X4_PUBLIC
static void note_art_ready(X4CatalogArtReady *out,uint64_t entered,uint64_t ready_at,
    enum X4AssetSource source)
{
    if(ready_at<=entered)++out->ram;
    else if(source==X4_ASSET_DISK)++out->disk;
    else ++out->network;
}
int x4_catalog_assets_art_ready(X4CatalogAssets *a,const X4CatalogArtDemand *demand,
    size_t count,uint64_t entered,X4CatalogArtReady *out)
{
    if(!a||!out||(!demand&&count)||count>X4_CATALOG_PRELOAD_PRODUCTS||trylock(&a->mutex))return 0;
    *out=(X4CatalogArtReady){0};
    uint64_t generation=atomic_load(&a->generation);
    for(size_t i=0;i<count;++i){
        const char *id=demand[i].id;
        int at=find(a,id,generation);
        const Slot *s=at>=0?&a->slots[at]:NULL;
        for(unsigned j=0;j<=X4_COVER_SURFACES;++j){
            if(!(demand[i].mask&(1u<<j)))continue;
            if(j==X4_COVER_BACKGROUND&&a->background_active>=0){
                const Background *bg=&a->backgrounds[a->background_active];
                if(bg->raster.rgba&&bg->generation==generation&&!strcmp(bg->id,id)){
                    note_art_ready(out,entered,bg->ready_at,bg->ready_source);
                    continue;
                }
            }
            const ImageState *image=s?&s->image[j]:NULL;
            if(s&&j<X4_COVER_SURFACES&&s->cover.surface[j].rgba)
                note_art_ready(out,entered,image->ready_at,image->ready_source);
            else if(atomic_load(&a->unavailable)||(s&&(s->metadata_missing||
                (s->metadata_attempts>=ATTEMPTS_MAX&&!s->metadata_valid&&!s->busy)||
                (s->metadata_valid&&(image->state==X4_ASSET_MISSING||
                    (image->attempts>=ATTEMPTS_MAX&&image->state!=X4_ASSET_DOWNLOADING))))))
                ++out->fallback;
            else ++out->pending;
        }
    }
    unlock(&a->mutex);
    return 1;
}
#endif
void x4_catalog_assets_request_background(X4CatalogAssets *a,const char *id)
{
    if(!a||(id&&!x4_product_id_valid(id)))return;
    lock(&a->mutex);
    if(strcmp(a->background_id,id?id:"")){
        if(id){
            memcpy(a->background_id,id,strlen(id)+1u);
        }else{
            a->background_id[0]=0;
            a->background_active=-1;
            ++a->revision;
        }
        fill_demand(a,atomic_load(&a->generation));
    }
    if(id)(void)activate_background(a,id,atomic_load(&a->generation));
    unlock(&a->mutex);
}
int x4_catalog_assets_background_status(X4CatalogAssets *a,const char *id,
    enum X4CatalogAssetState *state)
{
    if(!a||!state||!x4_product_id_valid(id)||trylock(&a->mutex))return 0;
    int i=find(a,id,atomic_load(&a->generation));
    if(i>=0){
        const Slot *slot=&a->slots[i];
        *state=slot->metadata_valid?slot->image[X4_COVER_BACKGROUND].state:slot->state;
    }
    unlock(&a->mutex);
    return i>=0;
}
int x4_catalog_assets_acquire_background(X4CatalogAssets *a,X4CatalogBackgroundView *view)
{
    if(!a||!view||trylock(&a->mutex))return 0;
    int i=a->background_active;
    if(i>=0){
        Background *bg=&a->backgrounds[i];
        *view=(X4CatalogBackgroundView){.raster=bg->raster,.generation=bg->generation,.slot=(unsigned)i};
        memcpy(view->product_id,bg->id,sizeof(view->product_id));
        ++bg->pins;
    }
    unlock(&a->mutex);
    return i>=0;
}
void x4_catalog_assets_release_background(X4CatalogAssets *a,X4CatalogBackgroundView *view)
{
    if(!a||!view||view->slot>=BACKGROUND_SLOTS)return;
    lock(&a->mutex);
    Background *bg=&a->backgrounds[view->slot];
    if(bg->pins&&bg->generation==view->generation&&!strcmp(bg->id,view->product_id))--bg->pins;
    unlock(&a->mutex);
    *view=(X4CatalogBackgroundView){.slot=BACKGROUND_SLOTS};
}
void x4_catalog_assets_game_request(X4CatalogAssets *a,const char *id,int fullscreen)
{
    if(!a||(id&&!x4_product_id_valid(id))||fullscreen<-1||
        fullscreen>=(int)X4_GAME_SCREENSHOT_MAX)return;
    lock(&a->mutex);
    game_request_locked(a,id?id:"",id?fullscreen:-1);
    unlock(&a->mutex);
}
uint64_t x4_catalog_assets_game_revision(X4CatalogAssets *a)
{
    return a?atomic_load_explicit(&a->game_revision,memory_order_acquire):0;
}
int x4_catalog_assets_game_details(X4CatalogAssets *a,const char *id,X4GameDetails *out,
    enum X4CatalogAssetState *state)
{
    if(!a||!out||!state||!x4_product_id_valid(id)||trylock(&a->mutex))return 0;
    GameAssets *game=&a->game;
    int requested=game_requested(a,id);
    if(requested){
        *state=atomic_load(&a->unavailable)?X4_ASSET_MISSING:
            !strcmp(game->cached_id,id)?game->metadata_state:X4_ASSET_PENDING;
        if(*state==X4_ASSET_READY)memcpy(out,&game->details.details,sizeof(*out));
        else memset(out,0,sizeof(*out));
    }
    unlock(&a->mutex);
    return requested;
}
int x4_catalog_assets_game_acquire(X4CatalogAssets *a,const char *id,unsigned index,
    bool fullscreen,X4GameScreenshotView *out)
{
    if(!a||!out||index>=X4_GAME_SCREENSHOT_MAX||!x4_product_id_valid(id)||
        trylock(&a->mutex))return 0;
    GameAssets *game=&a->game;
    unsigned slot=fullscreen?GAME_FULLSCREEN_SLOT:index;
    GameImage *image=&game->images[slot];
    int ready=game_requested(a,id)&&!strcmp(game->cached_id,id)&&
        (!fullscreen||(game->requested_full==(int)index&&game->full_index==(int)index))&&
        image->state==X4_ASSET_READY;
    if(ready){
        *out=(X4GameScreenshotView){image->raster,slot};
        ++image->pins;
    }
    unlock(&a->mutex);
    return ready;
}
void x4_catalog_assets_game_release(X4CatalogAssets *a,X4GameScreenshotView *view)
{
    if(!a||!view||view->slot>=GAME_IMAGE_COUNT||!view->raster.rgba)return;
    lock(&a->mutex);
    GameImage *image=&a->game.images[view->slot];
    if(image->pins&&image->raster.rgba==view->raster.rgba)--image->pins;
    unlock(&a->mutex);
    *view=(X4GameScreenshotView){0};
}
int x4_catalog_assets_game_stats(X4CatalogAssets *a,X4GameAssetStats *out)
{
    if(!a||!out||trylock(&a->mutex))return 0;
    *out=a->game.stats;
    unlock(&a->mutex);
    return 1;
}
void x4_catalog_assets_request_sigls(X4CatalogAssets *a,uint64_t session)
{
    if(!a)return;
    lock(&a->mutex);
    if(a->sigl_session!=session){
        if(a->sigl_session)clear_demand_locked(a);
        request_cleanup(a);
        a->sigl_session=session;
        a->sigl_started=0;
        for(unsigned i=0;i<X4_SIGL_COUNT;++i){
            a->sigls[i].count=0;
            a->sigls[i].valid=false;
            a->sigls[i].pending=session!=0;
        }
        ++a->sigl_revision;
    }
    unlock(&a->mutex);
}
int x4_catalog_assets_copy_sigls(X4CatalogAssets *a,X4SiglList out[X4_SIGL_COUNT],uint64_t *revision)
{
    if(!a||!out||!revision||trylock(&a->mutex))return 0;
    int changed=*revision!=a->sigl_revision;
    if(changed){
        for(unsigned i=0;i<X4_SIGL_COUNT;++i){
            const X4SiglList *list=&a->sigls[i];
            out[i].count=list->count;
            out[i].valid=list->valid;
            out[i].pending=list->pending;
            memcpy(out[i].ids,list->ids,list->count*sizeof(list->ids[0]));
        }
        *revision=a->sigl_revision;
    }
    unlock(&a->mutex);
    return changed;
}
void x4_catalog_assets_retry(X4CatalogAssets *a)
{
    if(!a)return;
    lock(&a->mutex);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){Slot *s=&a->slots[i];
        if(!s->wanted||s->busy)continue;
        if(!s->metadata_valid&&s->state==X4_ASSET_TEMPORARY_FAILURE){
            ++a->stats.retries;
            if(now(a)<s->metadata_due)++a->stats.retry_suppressed;
            s->metadata_attempts=0;
            s->state=X4_ASSET_PENDING;
            ++a->revision;
        }
        for(unsigned j=0;j<=X4_COVER_SURFACES;++j){
            ImageState *image=&s->image[j];
            if(!(s->wanted&(1u<<j))||image->state!=X4_ASSET_TEMPORARY_FAILURE)continue;
            ++a->stats.retries;
            if(now(a)<image->due)++a->stats.retry_suppressed;
            image->attempts=0;
            image->state=X4_ASSET_PENDING;
            ++a->revision;
        }
    }
    unlock(&a->mutex);
}
int x4_catalog_assets_acquire(X4CatalogAssets *a,const char *id,X4CatalogAssetView *view)
{
    if(!a||!view||!x4_product_id_valid(id)||trylock(&a->mutex))return 0;
    int i=find(a,id,atomic_load(&a->generation));
    if(i>=0){Slot *s=&a->slots[i];
    *view=(X4CatalogAssetView){.metadata=s->metadata,.state=asset_state(s),.generation=s->generation,.slot=(unsigned)i};
        memcpy(view->surface,s->cover.surface,sizeof(view->surface));
        ++s->pins;
        s->used=++a->use_clock;
    }
    unlock(&a->mutex);
    return i>=0;
}
void x4_catalog_assets_release(X4CatalogAssets *a,X4CatalogAssetView *view)
{
    if(!a||!view||view->slot>=X4_CATALOG_ASSET_SLOTS)return;
    lock(&a->mutex);
    Slot *s=&a->slots[view->slot];
    if(s->occupied&&s->generation==view->generation&&!strcmp(s->metadata.product_id,view->metadata.product_id)&&s->pins)--s->pins;
    unlock(&a->mutex);
    memset(view,0,sizeof(*view));
    view->slot=X4_CATALOG_ASSET_SLOTS;
}
size_t x4_catalog_assets_copy_metadata(X4CatalogAssets *a,X4ProductMetadata *out,size_t capacity,uint64_t *revision)
{
    if(!a||!out||trylock(&a->mutex))return 0;
    size_t n=0;
    uint64_t generation=atomic_load(&a->generation);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS&&n<capacity;++i){Slot *s=&a->slots[i];
    if(s->occupied&&s->generation==generation&&s->metadata_valid)out[n++]=s->metadata;
    }
    if(revision)*revision=a->revision;
    unlock(&a->mutex);
    return n;
}
void x4_catalog_assets_stats(X4CatalogAssets *a,X4CatalogAssetStats *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    if(!a||trylock(&a->mutex))return;
    *out=a->stats;
    out->revision=a->revision;
    out->busy=atomic_load(&a->busy);
    out->suspended=atomic_load(&a->suspended);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){Slot *s=&a->slots[i];
    out->occupied+=s->occupied?1u:0u;
    out->pinned+=s->pins;
        if(s->metadata_valid){
            int pending=surface_pending(s,X4_COVER_BACKGROUND)&&!background_is_active(a,s);
            for(unsigned j=0;j<X4_COVER_SURFACES&&!pending;++j)pending=surface_pending(s,j);
            if(pending)++out->pending;
        }else if(s->wanted&&!s->metadata_missing&&s->metadata_attempts<ATTEMPTS_MAX)++out->pending;
    }
    unlock(&a->mutex);
}
size_t x4_catalog_assets_events(X4CatalogAssets *a,X4AssetEvent *out,size_t capacity)
{
    if(!a||!out||trylock(&a->mutex))return 0;
    size_t n=a->event_count;
    if(n>capacity)n=capacity;
    unsigned first=(a->event_next+X4_ASSET_EVENT_CAP-a->event_count)%X4_ASSET_EVENT_CAP;
    for(size_t i=0;i<n;++i)out[i]=a->events[(first+(unsigned)i)%X4_ASSET_EVENT_CAP];
    unlock(&a->mutex);
    return n;
}
void x4_catalog_assets_destroy(X4CatalogAssets *a)
{
    if(!a)return;
    x4_catalog_assets_suspend(a);
    atomic_store(&a->stop,1);
    join_thread(a->thread);
    stop_downloaders(a,a->downloader_count+1u);
    game_reset_cache(&a->game);
    mutex_destroy(&a->http_open_mutex);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)x4_cover_rasters_free(&a->slots[i].cover);
    for(unsigned i=0;i<BACKGROUND_SLOTS;++i)free(a->backgrounds[i].raster.rgba);
    mutex_destroy(&a->mutex);
    free(a);
}
