/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only compressed-input acceptance with the FIVE REAL generated clips.
 * Uses the real cache, X4AU1/AnnexB reader, file policy, SPS parser and MbedTLS.
 * No decoder, native memory, GPU or console performance is exercised. */
#include "../src/video/probe_t26_cache.h"
#include "../src/video/probe_files.h"
#include "../src/video/probe_source.h"
#include "../src/video/stream_sps.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t le64(const uint8_t *p)
{
    uint64_t n=0;for(unsigned i=0;i<8;++i)n|=(uint64_t)p[i]<<(8u*i);return n;
}
static uint32_t le32(const uint8_t *p)
{
    uint32_t n=0;for(unsigned i=0;i<4;++i)n|=(uint32_t)p[i]<<(8u*i);return n;
}
static bool nal_type(const uint8_t *p,size_t bytes,unsigned type)
{
    for(size_t i=0;i+3<bytes;++i){
        size_t start=0;
        if(!p[i]&&!p[i+1]&&p[i+2]==1)start=i+3;
        else if(i+4<bytes&&!p[i]&&!p[i+1]&&!p[i+2]&&p[i+3]==1)start=i+4;
        if(start&&start<bytes&&(p[start]&31u)==type)return true;
    }
    return false;
}
static bool contains(const uint8_t *p,size_t bytes,const uint8_t *part,size_t part_bytes)
{
    if(!part_bytes||part_bytes>bytes)return false;
    for(size_t i=0;i<=bytes-part_bytes;++i)if(!memcmp(p+i,part,part_bytes))return true;
    return false;
}
static void relocated(char *out,size_t capacity,const char *root,const char *native_path)
{
    const char *leaf=strrchr(native_path,'/');assert(leaf&&leaf[1]);
    int n=snprintf(out,capacity,"%s/xcloud4-probe/t26/%s",root,leaf+1);
    assert(n>0&&(size_t)n<capacity);
}
static void verify_one(const X4T26GeneratedSample *compiled,const char *root)
{
    X4T26GeneratedSample sample=*compiled;
    char clip_path[X4_PROBE_FILE_PATH_MAX],index_path[X4_PROBE_FILE_PATH_MAX];
    relocated(clip_path,sizeof(clip_path),root,compiled->clip_path);
    relocated(index_path,sizeof(index_path),root,compiled->index_path);
    sample.clip_path=clip_path;sample.index_path=index_path;
    atomic_bool cancel;atomic_init(&cancel,false);X4T26ClipData cache={0};
    int loaded=x4_t26_clip_load(&cache,&sample,&cancel);
    if(loaded)fprintf(stderr,"FAIL real cache id=%s rc=%d\n",sample.id,loaded);
    assert(!loaded&&cache.verified&&cache.data&&cache.bytes&&cache.allocated_bytes>=cache.bytes);
    assert(cache.bytes<=sample.clip_bytes&&cache.offset[0]==0&&cache.pts_us[0]==0);
    /* Parse the compiled SPS with the production parser, independently of
     * the index header. Its bytes must also occur verbatim in cached AU0. */
    X4StreamSps sps={0};X4StreamSpsDiagnostics diagnostics={0};
    assert(!x4_stream_sps_inspect_diagnostic(sample.sps,sample.sps_size,&sps,&diagnostics));
    assert(diagnostics.syntax_complete&&sps.profile_idc==sample.profile_idc&&sps.level_idc==sample.level_idc);
    assert(sps.visible_width==sample.visible_width&&sps.visible_height==sample.visible_height&&
        sps.coded_width==sample.coded_width&&sps.coded_height==sample.coded_height);
    assert(sps.max_num_ref_frames==sample.max_num_ref_frames&&sps.pic_order_cnt_type==2&&
        sps.chroma_format_idc==1&&sps.bit_depth_luma==8&&sps.bit_depth_chroma==8&&sps.frame_mbs_only_flag==1);
    size_t first_bytes=0;uint64_t first_pts=1;
    const uint8_t *first=x4_t26_clip_au(&cache,0,&first_bytes,&first_pts);
    assert(first&&first_pts==0&&nal_type(first,first_bytes,5)&&nal_type(first,first_bytes,7)&&nal_type(first,first_bytes,8));
    assert(contains(first,first_bytes,sample.sps,sample.sps_size)&&contains(first,first_bytes,sample.pps,sample.pps_size));
    X4ProbeFileStatus status={0};FILE *index=x4_probe_files_sample_open(index_path,&status);assert(index);
    uint8_t header[X4_PROBE_INDEX_HEADER];assert(fread(header,1,sizeof(header),index)==sizeof(header));
    assert(!memcmp(header,"X4AU1\0\0\0",8)&&le32(header+8)==1&&le32(header+12)==96);
    assert(le64(header+16)==sample.clip_bytes&&le64(header+24)==1200&&
        !memcmp(header+64,sample.clip_sha256,32));
    uint64_t expected_offset=0;size_t arena=0;unsigned idrs=0;
    for(unsigned i=0;i<X4_T26_SOURCE_AUS;++i){
        uint8_t record[X4_PROBE_INDEX_RECORD];assert(fread(record,1,sizeof(record),index)==sizeof(record));
        uint64_t offset=le64(record),bytes=le64(record+8),pts=le64(record+16);
        assert(offset==expected_offset&&bytes>0&&bytes<=X4_PROBE_AU_MAX&&pts==(uint64_t)i*1000000/60);
        assert(cache.offset[i]==offset&&cache.size[i]==bytes&&cache.pts_us[i]==pts);
        size_t got_bytes=0;uint64_t got_pts=0;const uint8_t *au=x4_t26_clip_au(&cache,i,&got_bytes,&got_pts);
        assert(au==cache.data+offset&&got_bytes==bytes&&got_pts==pts);
        if(nal_type(au,got_bytes,5))++idrs;
        expected_offset+=bytes;arena+=((size_t)bytes+255u)&~(size_t)255u;
    }
    assert(cache.bytes==expected_offset&&cache.v1_input_arena_bytes==arena&&idrs==1);
    assert(!x4_probe_files_close(index,&status));
    printf("PASS REAL_T26_CACHE id=%s records_verified=1200 cached_AU=781 prefix_bytes=%zu arena256_bytes=%zu SPS=%ux%u/%ux%u profile=Main(77) level_idc=%u ref=%u POC=%u initial_IDR=1 full_clip_SHA=verified full_index_SHA=verified\n",
        sample.id,cache.bytes,arena,sps.visible_width,sps.visible_height,sps.coded_width,sps.coded_height,
        sps.level_idc,sps.max_num_ref_frames,sps.pic_order_cnt_type);
    x4_t26_clip_close(&cache);assert(!cache.data&&!cache.bytes&&!cache.verified);
    x4_t26_clip_close(&cache);
}
int main(int argc,char **argv)
{
    assert(argc==2&&X4_T26_SAMPLE_COUNT==5&&X4_T26_SOURCE_AUS==781);
    assert(!x4_probe_files_test_root(argv[1]));
    x4_probe_files_test_manifest(1); /* Do not relax the compiled allowlist. */
    for(unsigned i=0;i<X4_T26_SAMPLE_COUNT;++i)verify_one(&x4_t26_generated_samples[i],argv[1]);
    puts("PASS five real generated T26 clips through production cache+reader+SPS+MbedTLS; no decoder/native WB_ONION/PS4 call");
    return 0;
}
