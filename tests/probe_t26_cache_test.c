/* SPDX-License-Identifier: GPL-3.0-only */
/* Parser-only bytes: no valid SPS/slices or native decoding. The descriptor
 * metadata exercises cache admission; it is NOT a measured video profile.
 * Real probe_source/probe_hash/MbedTLS perform every file/index/hash check. */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/probe_t26_cache.h"
#include "../src/video/probe_source.h"
#include "../src/video/probe_files.h"
#include "../src/video/probe_hash.h"
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "fixture_catalog.h"

static unsigned checks,allocations,releases,malloc_calls,open_calls,hash_initializations;
static void *cache_allocation,*cache_compaction,*first_allocation;
static size_t cache_allocation_bytes,compaction_bytes;
static X4T26ClipData *watched_clip;
static bool fail_allocation,fail_compaction;
static atomic_bool *cancel_after_allocation;
static atomic_bool *cancel_before_compaction,*cancel_after_compaction,*cancel_after_copy;
static uint64_t last_source_ordinal;
static void check(bool value,const char *expression,unsigned line)
{
    ++checks;
    if(!value){fprintf(stderr,"FAIL T26 cache line%u: %s\n",line,expression);exit(1);}
}
#define CHECK(value) check(!!(value),#value,__LINE__)

/* Only the cache object's allocator calls are redirected by the build.
 * Reader, FILE and MbedTLS allocations are untouched and checked by ASan/LSan. */
void *x4_t26_cache_test_malloc(size_t bytes)
{
    ++malloc_calls;
    bool compact=cache_allocation!=NULL;
    CHECK(!cache_compaction);
    if(compact)CHECK(bytes==T26_CACHE_EXPECTED_PREFIX&&bytes<cache_allocation_bytes);
    bool failed=compact?fail_compaction:fail_allocation;
    if(failed){
        if(compact&&cancel_after_compaction)
            atomic_store_explicit(cancel_after_compaction,true,memory_order_release);
        return NULL;
    }
    void *p=malloc(bytes);
    if(p){
        if(compact){cache_compaction=p;compaction_bytes=bytes;}
        else {cache_allocation=p;first_allocation=p;cache_allocation_bytes=bytes;}
        ++allocations;
    }
    if(!compact&&cancel_after_allocation)
        atomic_store_explicit(cancel_after_allocation,true,memory_order_release);
    if(compact&&cancel_after_compaction)
        atomic_store_explicit(cancel_after_compaction,true,memory_order_release);
    return p;
}
void x4_t26_cache_test_free(void *p)
{
    if(p){
        if(p==cache_compaction){
            CHECK(watched_clip&&watched_clip->data==cache_allocation);
            cache_compaction=NULL;compaction_bytes=0;
        }
        else {
            CHECK(p==cache_allocation);
            if(cache_compaction){
                /* The original may be released only AFTER the complete copy
                 * and ownership transfer; no intermediate pointer escapes. */
                CHECK(watched_clip&&watched_clip->data==cache_compaction);
                CHECK(watched_clip->bytes==compaction_bytes&&watched_clip->allocated_bytes==compaction_bytes);
                CHECK(!memcmp(cache_allocation,cache_compaction,compaction_bytes));
                cache_allocation=cache_compaction;cache_allocation_bytes=compaction_bytes;
                cache_compaction=NULL;compaction_bytes=0;
            }else {cache_allocation=NULL;cache_allocation_bytes=0;}
        }
        ++releases;
    }
    free(p);
}
void *x4_t26_cache_test_memcpy(void *destination,const void *source,size_t bytes)
{
    CHECK(destination==cache_compaction&&source==cache_allocation&&bytes==compaction_bytes);
    CHECK(watched_clip&&watched_clip->data==cache_allocation);
    void *copied=memcpy(destination,source,bytes);
    CHECK(watched_clip->data==cache_allocation); /* No ownership change during copy. */
    if(cancel_after_copy)atomic_store_explicit(cancel_after_copy,true,memory_order_release);
    return copied;
}

/* Observation-only linker seams: forward all operations to the real file
 * API and real SHA context. No parser, crypto result or data is substituted. */
int __real_open(const char *,int,...);
int __wrap_open(const char *path,int flags,...)
{
    ++open_calls;
    if(flags&O_CREAT){
        va_list ap;va_start(ap,flags);int mode=va_arg(ap,int);va_end(ap);
        return __real_open(path,flags,(mode_t)mode);
    }
    return __real_open(path,flags);
}
int __real_x4_probe_hash_context_init(X4ProbeHashContext *);
int __wrap_x4_probe_hash_context_init(X4ProbeHashContext *context)
{++hash_initializations;return __real_x4_probe_hash_context_init(context);}
int __real_x4_probe_source_next(X4ProbeSource *,uint64_t,uint8_t *,size_t,X4ProbeAU *);
int __wrap_x4_probe_source_next(X4ProbeSource *source,uint64_t now,uint8_t *data,size_t capacity,X4ProbeAU *out)
{
    int rc=__real_x4_probe_source_next(source,now,data,capacity,out);
    if(!rc){
        last_source_ordinal=out->ordinal;
        if(cancel_before_compaction&&out->ordinal==X4_T26_SOURCE_AUS-1)
            atomic_store_explicit(cancel_before_compaction,true,memory_order_release);
    }
    return rc; /* Observe the real parser; never substitute its result/data. */
}

static bool empty(const X4T26ClipData *clip)
{
    if(clip->data||clip->bytes||clip->allocated_bytes||clip->v1_input_arena_bytes||clip->verified)return false;
    for(unsigned n=0;n<X4_T26_SOURCE_AUS;++n)
        if(clip->offset[n]||clip->size[n]||clip->pts_us[n])return false;
    return true;
}

static void require_failure(const X4T26GeneratedSample *sample,atomic_bool *cancel,int rc)
{
    X4T26ClipData clip={0};
    watched_clip=&clip;
    CHECK(x4_t26_clip_load(&clip,sample,cancel)==rc);
    CHECK(empty(&clip) && !cache_allocation&&!cache_compaction && allocations==releases);
    x4_t26_clip_close(&clip);CHECK(empty(&clip));
    watched_clip=NULL;
}

static void successful_prefix(bool keep_large_allocation)
{
    atomic_bool cancel;atomic_init(&cancel,false);
    X4T26ClipData clip={0};
    watched_clip=&clip;fail_compaction=keep_large_allocation;
    unsigned before_malloc=malloc_calls,before_release=releases;
    CHECK(x4_t26_clip_load(&clip,&t26_cache_fixtures[T26_CACHE_GOOD],&cancel)==0);
    CHECK(malloc_calls==before_malloc+2&&!cache_compaction);
    CHECK(clip.data && clip.verified && clip.bytes==T26_CACHE_EXPECTED_PREFIX);
    CHECK(clip.allocated_bytes>=clip.bytes && clip.allocated_bytes<=t26_cache_fixtures[T26_CACHE_GOOD].clip_bytes);
    if(keep_large_allocation){
        CHECK(clip.allocated_bytes==t26_cache_fixtures[T26_CACHE_GOOD].clip_bytes);
        CHECK(clip.data==first_allocation&&releases==before_release);
    }else CHECK(clip.allocated_bytes==clip.bytes&&releases==before_release+1);
    CHECK(clip.v1_input_arena_bytes==T26_CACHE_EXPECTED_V1_ARENA);
    CHECK(!(clip.v1_input_arena_bytes&255u));
    uint8_t *snapshot=malloc(clip.bytes);CHECK(snapshot);memcpy(snapshot,clip.data,clip.bytes);
    X4T26ClipData saved=clip;
    for(unsigned n=0;n<X4_T26_SOURCE_AUS;++n){
        size_t bytes=SIZE_MAX;uint64_t pts=UINT64_MAX;
        const uint8_t *p=x4_t26_clip_au(&clip,n,&bytes,&pts);
        CHECK(clip.offset[n]==t26_cache_expected_offset[n]);
        CHECK(clip.size[n]==t26_cache_expected_size[n]);
        CHECK(clip.pts_us[n]==t26_cache_expected_pts[n]);
        CHECK(p==clip.data+t26_cache_expected_offset[n]);
        CHECK(bytes==t26_cache_expected_size[n] && pts==t26_cache_expected_pts[n]);
        CHECK(bytes>=5 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1 && p[4]==9);
        for(size_t i=5;i<bytes;++i)CHECK(p[i]==(uint8_t)(1+(n*17u+(i-5)*29u)%251u));
    }
    CHECK(!memcmp(&clip,&saved,sizeof(clip)) && !memcmp(clip.data,snapshot,clip.bytes));
    size_t bytes=123;uint64_t pts=456;
    CHECK(!x4_t26_clip_au(&clip,X4_T26_SOURCE_AUS,&bytes,&pts) && bytes==123 && pts==456);
    CHECK(!x4_t26_clip_au(&clip,UINT_MAX,&bytes,&pts));
    CHECK(!x4_t26_clip_au(NULL,0,&bytes,&pts));
    CHECK(!x4_t26_clip_au(&clip,0,NULL,&pts));
    CHECK(!x4_t26_clip_au(&clip,0,&bytes,NULL));
    X4T26ClipData corrupt=clip;corrupt.offset[0]=corrupt.bytes+1;
    CHECK(!x4_t26_clip_au(&corrupt,0,&bytes,&pts));
    corrupt=clip;corrupt.size[0]=(uint32_t)corrupt.bytes+1;
    CHECK(!x4_t26_clip_au(&corrupt,0,&bytes,&pts));
    CHECK(x4_t26_clip_load(&clip,&t26_cache_fixtures[T26_CACHE_GOOD],&cancel)==X4_PROBE_ARGUMENT);
    CHECK(!memcmp(&clip,&saved,sizeof(clip)) && !memcmp(clip.data,snapshot,clip.bytes));
    free(snapshot);
    x4_t26_clip_close(&clip);CHECK(empty(&clip) && !cache_allocation&&!cache_compaction && allocations==releases);
    x4_t26_clip_close(&clip);CHECK(empty(&clip));
    CHECK(!x4_t26_clip_au(&clip,0,&bytes,&pts));
    fail_compaction=false;watched_clip=NULL;
}

static void rejected_inputs(void)
{
    atomic_bool cancel;atomic_init(&cancel,false);
    require_failure(&t26_cache_fixtures[T26_CACHE_CLIP_DAMAGED],&cancel,X4_PROBE_HASH);
    require_failure(&t26_cache_fixtures[T26_CACHE_INDEX_DAMAGED],&cancel,X4_PROBE_HASH);
    const unsigned bad_index[]={T26_CACHE_RECORDS780,T26_CACHE_RECORDS1201,T26_CACHE_BAD_OFFSET,
        T26_CACHE_BAD_PTS,T26_CACHE_BAD_HEADER_COUNT};
    for(unsigned i=0;i<sizeof(bad_index)/sizeof(bad_index[0]);++i)
        require_failure(&t26_cache_fixtures[bad_index[i]],&cancel,X4_PROBE_INDEX);
    require_failure(&t26_cache_fixtures[T26_CACHE_BAD_SIZE],&cancel,X4_PROBE_SIZE);
    X4T26GeneratedSample sample=t26_cache_fixtures[T26_CACHE_GOOD];
    sample.clip_sha256[0]^=1;require_failure(&sample,&cancel,X4_PROBE_HASH);
    sample=t26_cache_fixtures[T26_CACHE_GOOD];sample.index_sha256[0]^=1;
    require_failure(&sample,&cancel,X4_PROBE_HASH);
    sample=t26_cache_fixtures[T26_CACHE_GOOD];++sample.clip_bytes;
    require_failure(&sample,&cancel,X4_PROBE_INDEX);
    sample=t26_cache_fixtures[T26_CACHE_GOOD];sample.visible_width=1280;
    require_failure(&sample,&cancel,X4_PROBE_INDEX);
    sample=t26_cache_fixtures[T26_CACHE_GOOD];sample.coded_height=1080;
    require_failure(&sample,&cancel,X4_PROBE_INDEX);

#define BAD_FIELD(field,value) do {sample=t26_cache_fixtures[T26_CACHE_GOOD];sample.field=(value); \
    require_failure(&sample,&cancel,X4_PROBE_ARGUMENT);} while(0)
    BAD_FIELD(frame_count,0);BAD_FIELD(frame_count,1199);BAD_FIELD(frame_count,1201);
    BAD_FIELD(fps_num,59);BAD_FIELD(fps_den,2);BAD_FIELD(profile_idc,66);
    BAD_FIELD(level_idc,41);BAD_FIELD(max_num_ref_frames,2);BAD_FIELD(pic_order_cnt_type,0);
    BAD_FIELD(chroma_format_idc,2);BAD_FIELD(bit_depth_luma,10);BAD_FIELD(bit_depth_chroma,10);
    BAD_FIELD(cabac,0);BAD_FIELD(idr_count,0);BAD_FIELD(clip_bytes,0);
    BAD_FIELD(clip_bytes,80u*1024u*1024u+1u);
#undef BAD_FIELD
    X4T26ClipData clip={0};
    CHECK(x4_t26_clip_load(NULL,&t26_cache_fixtures[0],&cancel)==X4_PROBE_ARGUMENT);
    CHECK(x4_t26_clip_load(&clip,NULL,&cancel)==X4_PROBE_ARGUMENT && empty(&clip));
    CHECK(x4_t26_clip_load(&clip,&t26_cache_fixtures[0],NULL)==X4_PROBE_ARGUMENT && empty(&clip));
    x4_t26_clip_close(NULL);
}

static void cancel_and_allocation(void)
{
    atomic_bool cancel;atomic_init(&cancel,true);
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_CANCELLED);
    atomic_store(&cancel,false);fail_allocation=true;
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_MEMORY);fail_allocation=false;
    cancel_after_allocation=&cancel;
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_CANCELLED);
    CHECK(atomic_load(&cancel));cancel_after_allocation=NULL;atomic_store(&cancel,false);
    unsigned before=malloc_calls;
    cancel_before_compaction=&cancel;
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_CANCELLED);
    CHECK(atomic_load(&cancel)&&last_source_ordinal==X4_T26_SOURCE_AUS-1&&malloc_calls==before+1);
    cancel_before_compaction=NULL;atomic_store(&cancel,false);
    before=malloc_calls;cancel_after_compaction=&cancel;
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_CANCELLED);
    CHECK(atomic_load(&cancel)&&malloc_calls==before+2);
    atomic_store(&cancel,false);before=malloc_calls;fail_compaction=true;
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_CANCELLED);
    CHECK(atomic_load(&cancel)&&malloc_calls==before+2);
    fail_compaction=false;cancel_after_compaction=NULL;atomic_store(&cancel,false);
    before=malloc_calls;cancel_after_copy=&cancel;
    require_failure(&t26_cache_fixtures[0],&cancel,X4_PROBE_CANCELLED);
    CHECK(atomic_load(&cancel)&&malloc_calls==before+2);
    cancel_after_copy=NULL;atomic_store(&cancel,false);
    for(unsigned n=0;n<8;++n)successful_prefix((n&1u)!=0);
}

static void reject_dirty_state(void)
{
    atomic_bool cancel;atomic_init(&cancel,false);
    uint8_t outside=0;
    for(unsigned kind=0;kind<5;++kind){
        X4T26ClipData dirty={0};
        switch(kind){
        case 0:dirty.data=&outside;break;
        case 1:dirty.bytes=1;break;
        case 2:dirty.allocated_bytes=1;break;
        case 3:dirty.v1_input_arena_bytes=256;break;
        default:dirty.verified=true;break;
        }
        X4T26ClipData saved=dirty;
        unsigned prior_malloc=malloc_calls,prior_open=open_calls,prior_hash=hash_initializations;
        CHECK(x4_t26_clip_load(&dirty,&t26_cache_fixtures[0],&cancel)==X4_PROBE_ARGUMENT);
        CHECK(!memcmp(&dirty,&saved,sizeof(dirty)));
        CHECK(malloc_calls==prior_malloc && open_calls==prior_open && hash_initializations==prior_hash);
        CHECK(!cache_allocation&&!cache_compaction && allocations==releases);
        /* The rejected object still belongs to its caller; do not close the
         * stack pointer or silently erase its intentionally dirty fields. */
    }
}

int main(void)
{
    CHECK(!x4_probe_files_test_root(T26_CACHE_FIXTURE_ROOT));
    CHECK(T26_CACHE_FIXTURE_COUNT==9 && X4_T26_SOURCE_AUS==781);
    successful_prefix(false);successful_prefix(true);
    rejected_inputs();cancel_and_allocation();reject_dirty_state();
    CHECK(!cache_allocation&&!cache_compaction && allocations==releases);
    printf("PASS T26 cache: real reader+MbedTLS, parser-only1200-record fixtures; %uchecks, %uallocations/%ureleases; no console data\n",
        checks,allocations,releases);
    return 0;
}
