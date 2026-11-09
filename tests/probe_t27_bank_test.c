/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/probe_t27_plan.h"
#include "../src/video/probe_t27_state.h"
#include "../src/video/probe_t27_store.h"
#include "../src/video/probe_t27_result.h"
#include "../src/auth/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
static const char build[]="host-example27";
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
static void result_init(X4T26CellResult *r,unsigned index)
{
 X4T27Cell cell;assert(x4_t27_plan_cell(index,&cell));memset(r,0,sizeof(*r));strcpy(r->build_id,build);strcpy(r->manifest_sha256,manifest);
 r->cell=index;r->clip=cell.clip;r->group=cell.reference?X4_T26_GROUP_BASE_EXTRA:X4_T26_GROUP_REALISTIC;r->final=true;r->config=cell.config;
 unsigned mode=cell.reference?0u:1u;mode_init(&r->modes[mode],mode,&r->config);
 if(!cell.reference){r->modes[mode].gpu_requested=true;r->modes[mode].gpu_frames=780;strcpy(r->modes[mode].gpu_reason,"host simulated GPU");}
}
static char *write_result(const X4T26CellResult *r,const X4PipelinePresentationSummary *p,size_t *size)
{
 FILE *f=tmpfile();assert(f&&!x4_t27_result_write(f,r,p));assert(!fflush(f));long n=ftell(f);assert(n>0&&n<65536&&!fseek(f,0,SEEK_SET));
 char *text=malloc((size_t)n+1u);assert(text&&fread(text,1,(size_t)n,f)==(size_t)n&&!fclose(f));text[n]=0;*size=(size_t)n;return text;
}
static char *replace(const char *text,const char *old,const char *value)
{
 const char *p=strstr(text,old);assert(p);size_t prefix=(size_t)(p-text),a=strlen(old),b=strlen(value),n=strlen(text);
 char *out=malloc(n-a+b+1u);assert(out);memcpy(out,text,prefix);memcpy(out+prefix,value,b);memcpy(out+prefix+b,p+a,n-prefix-a+1u);return out;
}
static void result_reject(const char *text,const char *old,const char *value)
{
 char *changed=replace(text,old,value);X4T26RecoveredResult before,out;memset(&before,0xa5,sizeof(before));out=before;
 assert(x4_t27_result_read(changed,strlen(changed),build,manifest,&out,NULL)<0&&!memcmp(&before,&out,sizeof(out)));free(changed);
}
static void plan_state(void)
{
 unsigned raw=0,realistic=0,sixty=0,thirty=0,mailbox=0;
 X4T27State state;assert(!x4_t27_state_init(&state,build,manifest,100u));
 for(unsigned i=0;i<X4_T27_CELL_COUNT;++i){
  X4T27Cell cell;assert(x4_t27_plan_cell(i,&cell));assert(cell.config.api==2u&&cell.config.depth==2u&&cell.config.resource_type==1u&&
   cell.config.priority==700&&cell.config.affinity_mask==0x3fu&&cell.config.pipe_id==0u&&cell.config.queue_id==0u&&cell.config.output_memory_type==0);
  if(cell.reference){++raw;assert(i<2u&&!cell.config.realistic);}else{++realistic;assert(cell.config.realistic);
   if(cell.presentation.source_hz==60u)++sixty;else{assert(cell.presentation.source_hz==30u&&cell.clip==1u);++thirty;}
   if(cell.presentation.mailbox){++mailbox;assert(cell.presentation.vsync_policy==1u);}}
  assert(!x4_t27_state_begin(&state,i,cell.reference?0u:1u,X4_T27_PHASE_QUERY));
  assert(!x4_t27_state_mark(&state,cell.reference?0u:1u,X4_T27_PHASE_DELETE));
  char text[X4_T27_STATE_TEXT_MAX];int n=x4_t27_state_serialize(&state,text,sizeof(text));assert(n>0);
  X4T27State out;assert(!x4_t27_state_parse(text,(size_t)n,build,manifest,&out)&&out.active_cell==i);
  assert(n>=2&&text[n-1]=='\n'&&text[n-2]=='}');
  for(int prefix=0;prefix<n-1;++prefix)assert(x4_t27_state_parse(text,(size_t)prefix,build,manifest,&out)<0);
  assert(!x4_t27_state_parse(text,(size_t)n-1u,build,manifest,&out)); /* Optional trailing newline is not a truncated JSON value. */
  assert(!x4_t27_state_commit_cell(&state,i,true));
 }
 assert(raw==2u&&realistic==9u&&sixty==6u&&thirty==3u&&mailbox==3u&&state.terminal==X4_T27_TERMINAL_COMPLETE);
 assert(!x4_t27_plan_cell(11u,&(X4T27Cell){0}));
 assert(x4_t27_plan_remaining_us(0u)==UINT64_C(182000000));
 assert(!x4_t27_state_init(&state,build,manifest,100u));assert(!x4_t27_state_begin(&state,0u,0u,X4_T27_PHASE_DECODE));
 assert(!x4_t27_state_resume(&state,0u)&&state.terminal==X4_T27_TERMINAL_INTERRUPTED&&state.closed_app_mask==1u&&state.skipped_mask==2047u);
 assert(x4_t27_state_begin(&state,0u,0u,X4_T27_PHASE_QUERY)<0); /* Never repeat an uncertain native cell. */
 assert(!x4_t27_state_init(&state,build,manifest,100u));assert(!x4_t27_state_begin(&state,0u,0u,X4_T27_PHASE_DELETE));
 assert(!x4_t27_state_resume(&state,1u)&&state.next_cell==1u&&!state.active&&!state.terminal); /* Durable result wins old marker. */
 assert(!x4_t27_state_init(&state,build,manifest,100u));assert(!x4_t27_state_rebase(&state,1000u,X4_T27_BUDGET_US));
 assert(state.terminal==X4_T27_TERMINAL_STOPPED);
 puts("PASS plan/state: 2 references + 9 cells, unchanged decoder base, durable recovery, no uncertain rerun, bounded budget");
}
static void results(const char *dir)
{
 X4T26CellResult *r=malloc(sizeof(*r));assert(r);X4PipelinePresentationSummary p={0};
 p.scanouts=3u;p.scanout_bytes=3u*1920u*1080u*4u;
 p.decoder_slot_wait=(X4PipelineTimingSummary){.samples=1u,.sum_us=1u,.maximum_us=1u,.p50_us=100u,.p95_us=100u,.p99_us=100u,.finite_mask=7u};
 for(unsigned i=0;i<X4_T27_CELL_COUNT;++i){result_init(r,i);size_t n;char *text=write_result(r,i<2u?NULL:&p,&n);
  X4T26RecoveredResult out;X4PipelinePresentationSummary restored;
  assert(!x4_t27_result_read(text,n,build,manifest,&out,&restored)&&out.cell==i&&out.outcome.accepted&&out.outcome.hashes_ok);
  assert(out.base_hashes_valid==(i<2u)&&out.mode_mask==(i<2u?1u:2u));
  if(i==2u){
   for(size_t prefix=0;prefix<n;++prefix)assert(x4_t27_result_read(text,prefix,build,manifest,&out,NULL)<0);
   result_reject(text,"\"source_hz\":60","\"source_hz\":30");
   result_reject(text,"\"vsync_policy\":2","\"vsync_policy\":1");
   result_reject(text,"\"mailbox\":false","\"mailbox\":true");
   result_reject(text,"\"scanouts\":3","\"scanouts\":9");
   result_reject(text,"native_output_ids_181_780","whole_run");
   result_reject(text,"acquisition_before_decode_returning_measured_output","input_AU_ownership");
   result_reject(text,"whole_cell_including_180_au_warmup","measured_outputs_only");
   result_reject(text,"\"gpu_frames\":780","\"gpu_frames\":0");
   result_reject(text,"\"image\":300","\"image\":301");
   result_reject(text,"\"index\":2","\"index\":2,\"index\":2");
   char path[512];assert(snprintf(path,sizeof(path),"%s/host-example-cell.json",dir)>0);FILE *f=fopen(path,"wb");assert(f&&fwrite(text,1,n,f)==n&&!fclose(f));
  }
  free(text);
 }
 free(r);puts("PASS native result and presentation wrapper: all 11 cells, one-mode references, strict mutation/truncation rejection; simulated host fixtures only");
}
static void durable_store(const char *dir)
{
 char root[512];assert(snprintf(root,sizeof(root),"%s/store",dir)>0);X4T27Store store;X4T27State state;bool resumed;
 assert(!x4_t27_store_open_at(&store,root,build,manifest,100u,&state,&resumed)&&!resumed);
 char path[X4_T27_STORE_PATH_MAX];assert(!x4_t27_store_progress_path(&store,path,sizeof(path))&&strstr(path,"/progress.json"));
 X4T26CellResult *r=malloc(sizeof(*r));char *read=malloc(X4_T27_STORE_CELL_MAX+1u);assert(r&&read);X4PipelinePresentationSummary p={.scanouts=4u};
 for(unsigned i=0;i<11u;++i){X4T27Cell cell;assert(x4_t27_plan_cell(i,&cell));unsigned mode=cell.reference?0u:1u;
  assert(!x4_t27_state_begin(&state,i,mode,X4_T27_PHASE_QUERY)&&!x4_t27_store_write_progress(&store,&state));
  result_init(r,i);FILE *f=x4_t27_store_cell_begin(&store,i);assert(f&&!x4_t27_result_write(f,r,cell.reference?NULL:&p)&&!x4_t27_store_cell_finish(&store,f));
  size_t n;assert(!x4_t27_store_cell_read(&store,i,read,X4_T27_STORE_CELL_MAX+1u,&n));X4JsonSpan envelope,result;
  assert(!x4_json_parse(read,n,&envelope)&&x4_json_member(envelope,"result",&result)==1);X4T26RecoveredResult out;
  assert(!x4_t27_result_read(result.data,result.length,build,manifest,&out,NULL)&&out.cell==i);
  assert(!x4_t27_store_cell_begin(&store,i)); /* O_EXCL history cannot be overwritten. */
  assert(!x4_t27_state_commit_cell(&state,i,true)&&!x4_t27_store_write_progress(&store,&state));
 }
 X4T27Store again;X4T27State restored;assert(!x4_t27_store_open_at(&again,root,build,manifest,200u,&restored,&resumed)&&resumed&&restored.terminal==X4_T27_TERMINAL_COMPLETE);
 for(unsigned phase=1u;phase<=4u;++phase){x4_t27_store_test_fail(phase,EIO);assert(x4_t27_store_write_progress(&store,&state)<0);
  assert(!x4_t27_store_open_at(&again,root,build,manifest,200u,&restored,&resumed)&&resumed&&restored.completed_mask==2047u);}
 FILE *f=x4_t27_store_summary_begin(&store);assert(f&&fputs("{\"schema\":\"host-example-not-native\",\"padding\":\"",f)>=0);
 for(unsigned i=0;i<X4_T27_STORE_CELL_MAX+1024u;++i)assert(fputc('A',f)!=EOF);
 assert(fputs("\"}",f)>=0&&ftell(f)>X4_T27_STORE_CELL_MAX&&!x4_t27_store_summary_finish(&store,f));
 assert(!x4_t27_store_summary_begin(&store));free(read);free(r);
 puts("PASS durable store: T27-only root, 11 exclusive cells, real native-result reader, resume, four durability faults preserving progress/history");
}
int main(int argc,char **argv)
{assert(argc==2);plan_state();results(argv[1]);durable_store(argv[1]);puts("PASS T27 plan/state/store/results: host simulations, no console measurements");return 0;}
