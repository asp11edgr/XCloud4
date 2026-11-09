/* SPDX-License-Identifier: GPL-3.0-only */
/* HOST ONLY: exercise the actual controller's durable admission/recovery and
 * summary. Native/UI entry points are not run; linker GC removes them. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <unistd.h>
#include "../src/video/probe_t27_controller.c"
static uint64_t host_now=1000000u;
static T27Bank *stop_at_sync;
static const char host_manifest[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
uint64_t sceKernelGetProcessTime(void){return host_now;}
int __real_fsync(int);
int __wrap_fsync(int fd)
{int rc=__real_fsync(fd);if(!rc&&stop_at_sync){atomic_store_explicit(&stop_at_sync->cancel,true,memory_order_release);stop_at_sync=NULL;}return rc;}
static T27Bank *host_bank(const char *root,const char *name)
{
 T27Bank *b=calloc(1,sizeof(*b));assert(b);b->read_buffer=malloc(X4_T27_STORE_CELL_MAX+1u);assert(b->read_buffer);
 atomic_init(&b->cancel,false);atomic_init(&b->finished,false);atomic_init(&b->command,0u);atomic_init(&b->deadline,UINT64_MAX);
 memcpy(b->manifest,host_manifest,65u);char path[320];assert(snprintf(path,sizeof(path),"%s/%s",root,name)>0);
 assert(!x4_t27_store_open_at(&b->store,path,X4_BUILD_ID,b->manifest,host_now,&b->state,&b->resumed));return b;
}
static void host_free(T27Bank *b)
{assert(!b->retained&&!b->store.cell_file&&!b->store.summary_file);free(b->read_buffer);free(b);}
static void host_begin(T27Bank *b)
{assert(!x4_t27_state_begin(&b->state,0u,0u,X4_T27_PHASE_QUERY));assert(!x4_t27_store_write_progress(&b->store,&b->state));}
static void admission(const char *root)
{
 T27Bank *b=host_bank(root,"admission");host_begin(b);host_now+=10u;
 assert(!t27_before_native(b,"v2.QueryDecoderMemoryInfo")&&b->state.phase==X4_T27_PHASE_QUERY);
 stop_at_sync=b;assert(t27_before_native(b,"v2.CreateDecoder")==3);assert(!stop_at_sync&&b->state.phase==X4_T27_PHASE_CREATE);
 /* Cancellation arriving during the durable marker denies new work, while
  * safe Delete remains admitted and its phase is durably visible. */
 assert(!t27_before_native(b,"v2.DeleteDecoder")&&b->state.phase==X4_T27_PHASE_DELETE);
 X4T27Store again;X4T27State durable;bool resumed;assert(!x4_t27_store_open_at(&again,b->store.root,X4_BUILD_ID,b->manifest,host_now,&durable,&resumed));
 assert(resumed&&durable.active&&durable.phase==X4_T27_PHASE_DELETE);host_free(b);
 puts("PASS actual controller: stop during durable sync prevents Create; Delete remains safe/admitted");
}
static void interrupted(const char *root)
{
 T27Bank *b=host_bank(root,"interrupted");host_begin(b);
 FILE *f=x4_t27_store_cell_begin(&b->store,0u);assert(f&&fputs("{\"unfinished\":",f)>=0&&!fflush(f)&&!fsync(fileno(f))&&!fclose(f));b->store.cell_file=NULL;
 assert(!t27_recover(b)&&b->state.terminal==X4_T27_TERMINAL_INTERRUPTED&&b->state.closed_app_mask==1u&&b->state.skipped_mask==2047u);
 assert(!t27_summary(b));char path[X4_T27_STORE_PATH_MAX];assert(!x4_t27_store_summary_path(&b->store,path,sizeof(path)));
 FILE *report=fopen(path,"rb");assert(report);char *bytes=malloc(X4_T27_STORE_SUMMARY_MAX);assert(bytes);size_t n=fread(bytes,1,X4_T27_STORE_SUMMARY_MAX-1u,report);bytes[n]=0;assert(!fclose(report));
 assert(strstr(bytes,"\"status\":\"closed_app\"")&&strstr(bytes,"\"native_executed\":null"));
 /* A valid summary larger than CELL_MAX must be read using its own bound
  * and heap scratch; this is a host-owned fixture, never a product rewrite. */
 char *last=strrchr(bytes,'}');assert(last);f=fopen(path,"wb");assert(f);
 assert(fwrite(bytes,1,(size_t)(last-bytes),f)==(size_t)(last-bytes));
 assert(fputs(",\"host_padding\":\"",f)>=0);
 for(unsigned i=0;i<X4_T27_STORE_CELL_MAX+1024u;++i)assert(fputc('A',f)!=EOF);
 assert(fputs("\"}",f)>=0&&ftell(f)>X4_T27_STORE_CELL_MAX&&!fclose(f));
 assert(!t27_summary(b));
 assert(!t27_recover(b)); /* A second launch still preserves and skips the same partial file. */
 assert(!t27_summary(b)); /* Reopen a complete identity-bound summary, never overwrite. */
 f=fopen(path,"wb");assert(f);assert(fprintf(f,"{\"schema\":\"X4T27_BANK1\",\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\",\"terminal\":%u,\"cells\":[]}",X4_BUILD_ID,b->manifest,b->store.session,b->state.terminal)>0&&!fclose(f));
 assert(t27_summary(b)<0); /* Valid JSON with matching identity but missing cells fails closed. */
 free(bytes);host_free(b);puts("PASS actual controller: interrupted partial cell never retried; complete summary reused, missing cells rejected");
}
int main(int argc,char **argv)
{assert(argc==2);admission(argv[1]);interrupted(argv[1]);puts("PASS T27 actual controller helpers; no native/UI execution or console measurements");return 0;}
