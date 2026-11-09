/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_cache.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_hash.h"
#include <stdlib.h>
#include <string.h>

void x4_t26_clip_close(X4T26ClipData *clip)
{
    if(!clip)return;
    free(clip->data);
    memset(clip,0,sizeof(*clip));
}

int x4_t26_clip_load(X4T26ClipData *clip,const X4T26GeneratedSample *sample,
    const atomic_bool *cancel)
{
    if(!clip||clip->data||clip->bytes||clip->allocated_bytes||clip->v1_input_arena_bytes||
        clip->verified||!sample||!cancel||sample->frame_count!=1200||
        sample->fps_num!=60||sample->fps_den!=1||sample->profile_idc!=77||
        sample->level_idc!=42||sample->max_num_ref_frames!=1||
        sample->pic_order_cnt_type!=2||sample->chroma_format_idc!=1||
        sample->bit_depth_luma!=8||sample->bit_depth_chroma!=8||
        sample->cabac!=1||sample->idr_count!=1||
        !sample->clip_bytes||sample->clip_bytes>80u*1024u*1024u)
        return X4_PROBE_ARGUMENT;
    X4ProbeSource source={0};
    X4ProbeHashContext hash={0};
    int rc=x4_probe_hash_context_init(&hash);
    if(rc)return rc;
    rc=x4_probe_source_open_fully_verified(&source,sample->clip_path,sample->index_path,
        sample->clip_sha256,sample->index_sha256,x4_probe_hash_file,&hash,cancel);
    if(rc)goto done;
    if(source.info.records!=sample->frame_count||source.info.file_bytes!=sample->clip_bytes||
        source.info.visible_width!=sample->visible_width||source.info.visible_height!=sample->visible_height||
        source.info.coded_width!=sample->coded_width||source.info.coded_height!=sample->coded_height||
        source.info.fps_num!=sample->fps_num||source.info.fps_den!=sample->fps_den){rc=X4_PROBE_INDEX;goto done;}
    clip->data=malloc((size_t)sample->clip_bytes);
    if(!clip->data){rc=X4_PROBE_MEMORY;goto done;}
    clip->allocated_bytes=(size_t)sample->clip_bytes;
    rc=x4_probe_source_begin(&source,1,X4_PROBE_MAX_SPEED);
    if(rc)goto done;
    for(unsigned i=0;i<X4_T26_SOURCE_AUS;++i){
        X4ProbeAU au={0};
        size_t available=clip->allocated_bytes-clip->bytes;
        rc=x4_probe_source_next(&source,(uint64_t)i+1,clip->data+clip->bytes,available,&au);
        if(rc)goto done;
        if(au.ordinal!=i||!au.bytes||au.bytes>X4_PROBE_AU_MAX||au.bytes>available){rc=X4_PROBE_INDEX;goto done;}
        clip->offset[i]=clip->bytes;
        clip->size[i]=(uint32_t)au.bytes;
        clip->pts_us[i]=au.pts_us;
        clip->bytes+=(size_t)au.bytes;
        size_t aligned=((size_t)au.bytes+255u)&~(size_t)255u;
        if(aligned>SIZE_MAX-clip->v1_input_arena_bytes){rc=X4_PROBE_SIZE;goto done;}
        clip->v1_input_arena_bytes+=aligned;
    }
    /* Compact once during preparation, before any Decode/input publication.
     * Do not use the SDK's realloc path (it reaches unsupported mremap).
     * Peak heap is bounded by original+prefix; OOM retains the original
     * allocation and its exact allocated_bytes instead of losing the data. */
    if(clip->bytes<clip->allocated_bytes){
        if(atomic_load_explicit(cancel,memory_order_acquire)){rc=X4_PROBE_CANCELLED;goto done;}
        uint8_t *compact=malloc(clip->bytes);
        if(atomic_load_explicit(cancel,memory_order_acquire)){
            free(compact);rc=X4_PROBE_CANCELLED;goto done;
        }
        if(compact){
            memcpy(compact,clip->data,clip->bytes);
            if(atomic_load_explicit(cancel,memory_order_acquire)){
                free(compact);rc=X4_PROBE_CANCELLED;goto done;
            }
            uint8_t *original=clip->data;
            clip->data=compact;clip->allocated_bytes=clip->bytes;
            free(original);
        }
    }
    if(atomic_load_explicit(cancel,memory_order_acquire)){rc=X4_PROBE_CANCELLED;goto done;}
    clip->verified=source.stats.hash_verified&&source.stats.index_hash_verified;
    if(!clip->verified)rc=X4_PROBE_HASH;
done:
    {
        int close_rc=x4_probe_source_close(&source);
        if(!rc&&close_rc)rc=close_rc;
    }
    x4_probe_hash_context_close(&hash);
    if(rc)x4_t26_clip_close(clip);
    return rc;
}

const uint8_t *x4_t26_clip_au(const X4T26ClipData *clip,unsigned ordinal,
    size_t *bytes,uint64_t *pts)
{
    if(!clip||!clip->verified||!clip->data||ordinal>=X4_T26_SOURCE_AUS||!bytes||!pts)return NULL;
    if(clip->offset[ordinal]>clip->bytes||clip->size[ordinal]>clip->bytes-clip->offset[ordinal])return NULL;
    *bytes=clip->size[ordinal];*pts=clip->pts_us[ordinal];
    return clip->data+clip->offset[ordinal];
}
#endif
