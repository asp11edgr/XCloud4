/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_t26_result.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char build[]="result-test26";
static const char manifest[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void mode_init(X4T26ModeResult *m,unsigned index,const X4T26Config *c)
{
    memset(m,0,sizeof(*m));m->present=m->completed=m->accepted=m->inference_valid=true;m->mode=index;
    m->native_calls=m->decode_calls=781;m->measured_calls=m->measured_outputs=600;
    m->elapsed_us=13000000;m->measured_wall_us=10000000;m->output_span_us=9983533;
    m->no_output_calls=1;m->flush_calls=1;m->drained_outputs=1;m->call_delay_hist[1]=600;
    m->decode=(X4T26Stats){600,8000,9000,10000,11000,8500};
    m->au_latency=(X4T26Stats){600,20000,25000,30000,35000,22000};
    m->output_interval=(X4T26Stats){599,16667,16667,16667,16667,16667};
    m->cpu=(X4T26CpuDelta){.process_available=true,.thread_available=true,.process_us=20000000,
        .thread_us=1000000,.wall_us=10000000,.process_percent=200.0,.thread_percent=10.0};
    static const uint64_t ordinals[]={1,300,600};
    for(unsigned i=0;i<3;++i){m->hashes[i].observed=true;m->hashes[i].measured_ordinal=ordinals[i];
        m->hashes[i].compared=index==1;m->hashes[i].matches_base=index==1;memcpy(m->hashes[i].sha256,manifest,65);}
    X4T26BackendInfo *b=&m->backend;b->config=*c;b->attempted=b->opened=b->closed=b->owned_clean=true;
    b->output_count=c->api==2?c->depth+3:5;b->allocations_before=1;b->owned_bytes_before=0x100000;
    b->allocation_checks[0]=(X4T26AllocationCheck){.allocation={0x4000,0x100000,0},
        .before_called=true,.before_valid=true,.after_called=true,.after_rc=(int32_t)UINT32_C(0x8002000d),
        .release_evidence=true,.before={0x4000,0x104000,0,0}};
    b->v2.requested_depth=b->v2.active_depth=c->depth;b->v2.input_count=4;b->v2.output_count=b->output_count;
    b->v2.attempt_count=1;b->v2.native_budget_bytes=192*1024*1024;b->v2.planned_native_bytes=127107072;
    b->v2.attempts[0].depth=c->depth;b->v2.attempts[0].profile=77;b->v2.attempts[0].level=42;
    b->platform.modules[0].module_id=0x008e;b->platform.modules[1].module_id=0x00fc;
    b->platform.memory_observations=1;b->platform.memory[0]=(X4T26MemoryObservation){.offset=0x4000,.bytes=0x100000};
}
static void result_init(X4T26CellResult *r)
{
    memset(r,0,sizeof(*r));strcpy(r->build_id,build);strcpy(r->manifest_sha256,manifest);
    strcpy(r->reason,"ready \"NV12\" \\ verified");r->final=true;r->clip=X4_T26_CLIP_1080_15;
    x4_t26_config_base(&r->config);for(unsigned i=0;i<2;++i)mode_init(&r->modes[i],i,&r->config);
}
static char *replace(const char *text,const char *old,const char *value)
{
    const char *found=strstr(text,old);assert(found);size_t prefix=(size_t)(found-text),a=strlen(old),b=strlen(value),n=strlen(text);
    char *changed=malloc(n-a+b+1);assert(changed);memcpy(changed,text,prefix);memcpy(changed+prefix,value,b);
    memcpy(changed+prefix+b,found+a,n-prefix-a+1);return changed;
}
static void rejects(const char *text,const char *old,const char *value)
{
    char *changed=replace(text,old,value);X4T26RecoveredResult untouched;memset(&untouched,0xa5,sizeof(untouched));
    X4T26RecoveredResult result=untouched;
    assert(x4_t26_result_read(changed,strlen(changed),build,manifest,&result)<0&&!memcmp(&result,&untouched,sizeof(result)));
    free(changed);
}
int main(int argc,char **argv)
{
    X4T26CellResult *r=malloc(sizeof(*r));char *text=malloc(X4_T26_RESULT_TEXT_MAX);assert(r&&text);result_init(r);
    int bytes=x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX);assert(bytes>0&&bytes<X4_T26_RESULT_TEXT_MAX);
    X4T26RecoveredResult result;
    assert(!x4_t26_result_read(text,(size_t)bytes,build,manifest,&result));
    assert(result.outcome.completed&&result.outcome.accepted&&result.outcome.hashes_ok&&!result.outcome.errors&&
        result.outcome.decode_p95_us==9000&&result.base_hashes_valid&&result.elapsed_us==26000000&&result.mode_mask==3&&
        !strcmp(result.reason,r->reason)&&!strcmp(result.hashes[0][0],manifest)&&result.native_fps_available[0]&&
        result.native_fps_available[1]&&result.native_fps[0]==60.0&&result.native_fps[1]==60.0);
    if(argc==2){FILE *f=fopen(argv[1],"wb");assert(f&&!x4_t26_result_write(f,r)&&!fclose(f));}
    for(int i=0;i<bytes;++i)assert(x4_t26_result_read(text,(size_t)i,build,manifest,&result)<0);
    assert(x4_t26_result_read(text,(size_t)bytes,"wrong-build",manifest,&result)<0);
    rejects(text,"\"depth\":2","\"depth\":3");
    rejects(text,"\"cell\":0","\"cell\":1");
    rejects(text,"\"schema\":\"xcloud4-t26-cell-v1\"","\"schema\":\"xcloud4-t26-cell-v0\"");
    rejects(text,"\"build\":\"result-test26\"","\"build\":\"result-test26\",\"build\":\"result-test26\"");
    rejects(text,"\"final\":true","\"final\":false");
    rejects(text,"\"image\":300","\"image\":301");
    rejects(text,"\"local_rc\":0","\"local_rc\":-99");
    rejects(text,"\"mean_us\":8500.000000","\"mean_us\":NaN");
    rejects(text,"[600,600,781,781","[600,600,18446744073709551616,781");
    rejects(text,"\"process\":[20000000,200.000000]","\"process\":[20000000,201.000000]");
    rejects(text,"\"reason\":","\"alien\":1,\"reason\":");
    char *changed=replace(text,"\"compared\":true","\"compared\":false");
    /* A base hash is a self-reference even if the comparison bit is absent. */
    assert(!x4_t26_result_read(changed,strlen(changed),build,manifest,&result)&&result.base_hashes_valid);free(changed);
    changed=replace(text,"\"sha256\":\"aaaaaaaa","\"sha256\":\"baaaaaaa");
    assert(!x4_t26_result_read(changed,strlen(changed),build,manifest,&result)&&!result.base_hashes_valid&&!result.outcome.accepted);free(changed);
    char small[4]="old";assert(x4_t26_result_encode(r,small,sizeof(small))<0&&!strcmp(small,"old"));
    r->modes[0].decode.mean_us=NAN;assert(x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX)<0);
    r->modes[0].decode.mean_us=8500;r->modes[0].cpu.process_percent=INFINITY;
    assert(x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX)<0);result_init(r);
    r->status=X4_T26_RESULT_QUERY_REJECTED;r->error_code=-123;
    memset(r->modes,0,sizeof(r->modes));X4T26ModeResult *m=&r->modes[0];
    m->present=m->completed=true;m->status=X4_T26_RESULT_QUERY_REJECTED;m->native_result=X4_T26_PLATFORM_NOT_RUN;
    m->backend.config=r->config;m->backend.closed=m->backend.owned_clean=true;m->backend.open_result=-123;
    bytes=x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX);assert(bytes>0);
    assert(!x4_t26_result_read(text,(size_t)bytes,build,manifest,&result)&&result.outcome.completed&&!result.outcome.accepted&&
        !result.base_hashes_valid&&!result.native_fps_available[0]&&!result.native_fps_available[1]);
    assert(strstr(text,"\"decode\":null")&&strstr(text,"\"process\":null")&&strstr(text,"\"output_fps\":null"));
    result_init(r);r->cell=38;r->group=X4_T26_GROUP_V1;r->factor=UINT_MAX;
    r->config=(X4T26Config){.api=X4_T26_API_V1,.depth=1};
    for(unsigned i=0;i<2;++i){mode_init(&r->modes[i],i,&r->config);for(unsigned h=0;h<3;++h)r->modes[i].hashes[h].compared=r->modes[i].hashes[h].matches_base=true;}
    bytes=x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX);assert(bytes>0);
    assert(!x4_t26_result_read(text,(size_t)bytes,build,manifest,&result)&&result.outcome.accepted&&!result.base_hashes_valid);
    result_init(r);r->cell=34;r->group=X4_T26_GROUP_REALISTIC;r->config.realistic=true;
    memset(&r->modes[0],0,sizeof(r->modes[0]));mode_init(&r->modes[1],1,&r->config);
    r->modes[1].gpu_requested=true;r->modes[1].gpu_frames=779;r->modes[1].cpu_frames=1;
    strcpy(r->modes[1].gpu_reason,"GPU ready after warmup");
    r->modes[1].decode_to_flip=r->modes[1].submit_to_observe=(X4T26Stats){600,1000,2000,3000,4000,1500};
    bytes=x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX);assert(bytes>0);
    assert(!x4_t26_result_read(text,(size_t)bytes,build,manifest,&result)&&result.outcome.accepted&&result.mode_mask==2&&!result.base_hashes_valid);
    rejects(text,"\"gpu_frames\":779","\"gpu_frames\":0");
    rejects(text,"\"scope\":\"whole_run_warmup_and_measured\"","\"scope\":\"steady_only\"");
    result_init(r);for(unsigned i=0;i<2;++i){X4T26BackendInfo *b=&r->modes[i].backend;b->allocations_before=16;
        b->platform.memory_observations=16;b->v2.attempt_count=6;
        for(unsigned j=1;j<16;++j)b->allocation_checks[j]=b->allocation_checks[0];}
    bytes=x4_t26_result_encode(r,text,X4_T26_RESULT_TEXT_MAX);assert(bytes>0&&bytes<X4_T26_RESULT_TEXT_MAX);
    assert(!x4_t26_result_read(text,(size_t)bytes,build,manifest,&result)&&result.outcome.accepted);
    uint8_t image[24];memset(image,0xaa,sizeof(image));image[0]=1;image[1]=2;image[4]=3;image[5]=4;image[16]=5;image[17]=6;
    X4T26Output output={.valid=true,.layout_verified=true,.pointer=image,.owned_bytes=24,.returned_bytes=24,
        .layout={.visible_width=2,.visible_height=2,.coded_width=2,.coded_height=4,.storage_height=4,.pitch=4,
            .y_bytes=16,.uv_bytes=8,.uv_plane_offset=16,.total_nv12_bytes=24,.visible_uv_offset=16}};
    char hash[65];assert(!x4_t26_result_hash_nv12(&output,hash)&&!strcmp(hash,"7192385c3c0605de55bb9476ce1d90748190ecb32a8eed7f5207b30cf6a1fe89"));
    image[2]=0x11;image[3]=0x22;image[6]=0x33;image[18]=0x44;
    assert(!x4_t26_result_hash_nv12(&output,hash)&&!strcmp(hash,"7192385c3c0605de55bb9476ce1d90748190ecb32a8eed7f5207b30cf6a1fe89"));
    output.owned_bytes=23;assert(x4_t26_result_hash_nv12(&output,hash)<0);output.owned_bytes=24;
    output.layout.uv_plane_offset=0;assert(x4_t26_result_hash_nv12(&output,hash)<0);output.layout.uv_plane_offset=16;
    output.local_result=-1;assert(x4_t26_result_hash_nv12(&output,hash)<0);
    free(text);free(r);puts("PASS T26 real cell JSON, resume outcome/base hashes, truncation/mutations and visible NV12 SHA256");return 0;
}
