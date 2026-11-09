/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t27_controller.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include "probe_t27_plan.h"
#include "probe_t27_store.h"
#include "probe_t27_result.h"
#include "probe_t27_view.h"
#include "probe_t26_runner.h"
#include "probe_t26_realistic.h"
#include "stream_pipeline.h"
#include "stream_wait.h"
#include "stream_diagnostics.h"
#include "display.h"
#include "../core/build_identity.h"
#include "../core/lifecycle.h"
#include "../auth/json.h"
#include "../input/controller.h"
#include "../ui/font.h"
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <orbis/libkernel.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { BANK_SHOW=1,BANK_DISPLAY_OPEN,BANK_DISPLAY_CLOSE,BANK_FINISHED };
typedef struct {
 X4T27State state;X4T27Store store;X4T26ClipData clips[2];
 X4T26RecoveredResult recovered[X4_T27_CELL_COUNT];bool recovered_valid[X4_T27_CELL_COUNT],base_valid[2];char base[2][3][65];
 X4T26CellResult current;X4PipelinePresentationSummary presentation;X4T27ViewSnapshot view;
 X4ProbeThread thread;OrbisPthread id;X4StreamWait main_wait,worker_wait;
 atomic_uint command;atomic_bool cancel,finished;atomic_uint_fast64_t deadline;
 X4StreamPipeline *requested_pipeline,*retained_pipeline;X4T26Backend *retained_backend;
 int handoff_result,result;bool retained,resumed,exit_requested;unsigned current_mode;
 char manifest[65],failure[160];char *read_buffer;
} T27Bank;
static uint64_t t27_clock_us(void *ignored){(void)ignored;return sceKernelGetProcessTime();}
static void t27_sleep_us(void *ignored,unsigned us){(void)ignored;sceKernelUsleep(us);}
static bool t27_expired(void *context)
{T27Bank *b=context;return t27_clock_us(NULL)>=atomic_load_explicit(&b->deadline,memory_order_acquire);}
static void t27_text(char *out,size_t capacity,const char *s){snprintf(out,capacity,"%s",s?s:"");}
static int t27_charge(T27Bank *b)
{
 X4T27State *s=&b->state;uint64_t now=t27_clock_us(NULL);
 if(now<s->started_us||now-s->started_us>UINT64_MAX-s->budget_used_us)return -1;
 s->budget_used_us+=now-s->started_us;s->started_us=now;
 s->deadline_us=now+(s->budget_used_us<X4_T27_BUDGET_US?X4_T27_BUDGET_US-s->budget_used_us:0u);
 atomic_store_explicit(&b->deadline,s->deadline_us,memory_order_release);return x4_t27_state_valid(s)?0:-1;
}
static int t27_before_native(void *context,const char *op)
{
 T27Bank *b=context;bool closing=strstr(op,"Delete")||strstr(op,"delete");
 if(!b->state.active)return -7770;
 if(!closing&&(atomic_load_explicit(&b->cancel,memory_order_acquire)||t27_expired(b)))return t27_expired(b)?4:3;
 unsigned phase=closing?X4_T27_PHASE_DELETE:strstr(op,"Query")||strstr(op,"query")?X4_T27_PHASE_QUERY:
  strstr(op,"Create")||strstr(op,"create")?X4_T27_PHASE_CREATE:
  strstr(op,"Decode")||strstr(op,"decode")||strstr(op,"warmup")?X4_T27_PHASE_DECODE:X4_T27_PHASE_QUERY;
 if(t27_charge(b)||x4_t27_state_mark(&b->state,b->current_mode,phase)||x4_t27_store_write_progress(&b->store,&b->state))return -7771;
 if(!closing&&(atomic_load_explicit(&b->cancel,memory_order_acquire)||t27_expired(b)))return t27_expired(b)?4:3;
 return 0;
}
static int t27_handoff(T27Bank *b,unsigned command)
{
 if(atomic_load_explicit(&b->command,memory_order_acquire))return -7772;
 b->handoff_result=0;atomic_store_explicit(&b->command,command,memory_order_release);(void)x4_stream_wait_notify(&b->main_wait);
 while(atomic_load_explicit(&b->command,memory_order_acquire)){int rc=x4_stream_wait_block(&b->worker_wait,20000);if(rc<0)return rc;}
 return b->handoff_result;
}
static int t27_display_open(void *context,X4StreamPipeline *p)
{T27Bank *b=context;b->requested_pipeline=p;return t27_handoff(b,BANK_DISPLAY_OPEN);}
static int t27_display_close(void *context,X4StreamPipeline *p)
{T27Bank *b=context;b->requested_pipeline=p;return t27_handoff(b,BANK_DISPLAY_CLOSE);}
static int t27_manifest_hash(char out[65])
{
 mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
#if MBEDTLS_VERSION_MAJOR >= 3
#define H_START mbedtls_sha256_starts
#define H_UPDATE mbedtls_sha256_update
#define H_FINISH mbedtls_sha256_finish
#else
#define H_START mbedtls_sha256_starts_ret
#define H_UPDATE mbedtls_sha256_update_ret
#define H_FINISH mbedtls_sha256_finish_ret
#endif
 int rc=H_START(&hash,0);uint8_t digest[32];
 for(unsigned i=1u;!rc&&i<=2u;++i){const X4T26GeneratedSample *s=&x4_t26_generated_samples[i];
  rc=H_UPDATE(&hash,(const unsigned char *)s->id,strlen(s->id)+1u);
  if(!rc)rc=H_UPDATE(&hash,s->clip_sha256,32u);if(!rc)rc=H_UPDATE(&hash,s->index_sha256,32u);}
 if(!rc)rc=H_FINISH(&hash,digest);mbedtls_sha256_free(&hash);if(rc)return rc;
 static const char hex[]="0123456789abcdef";for(unsigned i=0;i<32u;++i){out[2u*i]=hex[digest[i]>>4];out[2u*i+1u]=hex[digest[i]&15u];}out[64]=0;return 0;
}
static int t27_read_cell(T27Bank *b,unsigned index,X4T26RecoveredResult *out)
{
 size_t bytes=0;int rc=x4_t27_store_cell_read(&b->store,index,b->read_buffer,X4_T27_STORE_CELL_MAX+1u,&bytes);if(rc)return rc;
 X4JsonSpan envelope,result;if(x4_json_parse(b->read_buffer,bytes,&envelope)||x4_json_member(envelope,"result",&result)!=1)return -7773;
 rc=x4_t27_result_read(result.data,result.length,X4_BUILD_ID,b->manifest,out,NULL);
 return rc||out->cell!=index?-7774:0;
}
static int t27_recover(T27Bank *b)
{
 uint64_t durable=0;
 for(unsigned i=0;i<X4_T27_CELL_COUNT;++i){int rc=t27_read_cell(b,i,&b->recovered[i]);
  if(rc==1)continue;if(rc==2&&((b->state.active&&b->state.active_cell==i)||(b->state.closed_app_mask&(UINT64_C(1)<<i))))continue;if(rc)return rc;
  b->recovered_valid[i]=true;durable|=UINT64_C(1)<<i;
  if(i<X4_T27_BASE_COUNT&&b->recovered[i].base_hashes_valid){memcpy(b->base[i],b->recovered[i].hashes[0],sizeof(b->base[i]));b->base_valid[i]=true;}
 }
 if(x4_t27_state_resume(&b->state,durable)||x4_t27_state_rebase(&b->state,t27_clock_us(NULL),b->state.budget_used_us)||
  x4_t27_store_write_progress(&b->store,&b->state))return -7775;
 atomic_store_explicit(&b->deadline,b->state.deadline_us,memory_order_release);return 0;
}
static int t27_commit_result(T27Bank *b,const X4T27Cell *cell)
{
 b->current.final=true;FILE *f=x4_t27_store_cell_begin(&b->store,cell->index);if(!f)return -7776;
 int rc=x4_t27_result_write(f,&b->current,cell->reference?NULL:&b->presentation);
 int closed=x4_t27_store_cell_finish(&b->store,f);if(rc||closed)return -7777;
 rc=t27_read_cell(b,cell->index,&b->recovered[cell->index]);if(rc)return rc;b->recovered_valid[cell->index]=true;
 if(cell->reference&&b->recovered[cell->index].base_hashes_valid){memcpy(b->base[cell->index],b->recovered[cell->index].hashes[0],sizeof(b->base[0]));b->base_valid[cell->index]=true;}
 if(x4_t27_state_commit_cell(&b->state,cell->index,b->recovered[cell->index].outcome.accepted)||t27_charge(b)||x4_t27_store_write_progress(&b->store,&b->state))return -7778;
 return 0;
}
static int t27_summary(T27Bank *b)
{
 char path[X4_T27_STORE_PATH_MAX];if(x4_t27_store_summary_path(&b->store,path,sizeof(path)))return -7779;
 errno=0;FILE *old=fopen(path,"rb");if(old){bool valid=false;char token[65];X4JsonSpan root,value;
  if(!fseek(old,0,SEEK_END)){long n=ftell(old);if(n>0&&(uint64_t)n<=X4_T27_STORE_SUMMARY_MAX&&!fseek(old,0,SEEK_SET)){
   char *data=malloc((size_t)n+1u);if(data){if(fread(data,1,(size_t)n,old)==(size_t)n){data[n]=0;valid=!x4_json_parse(data,(size_t)n,&root)&&
    x4_json_member(root,"schema",&value)==1&&!x4_json_token(value,token,sizeof(token))&&!strcmp(token,"X4T27_BANK1")&&
    x4_json_member(root,"build",&value)==1&&!x4_json_token(value,token,sizeof(token))&&!strcmp(token,X4_BUILD_ID)&&
    x4_json_member(root,"session",&value)==1&&!x4_json_token(value,token,sizeof(token))&&!strcmp(token,b->store.session)&&
    x4_json_member(root,"manifest",&value)==1&&!x4_json_token(value,token,sizeof(token))&&!strcmp(token,b->manifest);
    X4JsonSpan cells,item;uint32_t terminal=0;valid=valid&&x4_json_member(root,"terminal",&value)==1&&!x4_json_uint32(value,&terminal)&&terminal==b->state.terminal&&
     x4_json_member(root,"cells",&cells)==1&&x4_json_type(cells)==X4_JSON_T_ARRAY;
    size_t cursor=0;unsigned count=0;while(valid&&x4_json_item(cells,&cursor,&item)==1){uint32_t index=0;
     valid=count<X4_T27_CELL_COUNT&&x4_json_member(item,"index",&value)==1&&!x4_json_uint32(value,&index)&&index==count;++count;}
    valid=valid&&count==X4_T27_CELL_COUNT;}
    free(data);}}}if(fclose(old))valid=false;return valid?0:-7779;}
 if(errno!=ENOENT)return -7779;
 FILE *f=x4_t27_store_summary_begin(&b->store);if(!f)return -7779;int rc=0;
 if(fprintf(f,"{\"schema\":\"X4T27_BANK1\",\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\" ,"
  "\"reference_cells\":2,\"realistic_cells\":9,\"budget_used_us\":%" PRIu64 ",\"terminal\":%u,\"retained\":%s,\"bank_result\":%d,\"cells\":[",
  X4_BUILD_ID,b->manifest,b->store.session,b->state.budget_used_us,b->state.terminal,b->retained?"true":"false",b->result)<0)rc=-1;
 for(unsigned i=0;!rc&&i<X4_T27_CELL_COUNT;++i){if(i&&fputc(',',f)==EOF){rc=-1;break;}size_t size=0;
  int read=x4_t27_store_cell_read(&b->store,i,b->read_buffer,X4_T27_STORE_CELL_MAX+1u,&size);
  if(!read){X4JsonSpan envelope,result;if(x4_json_parse(b->read_buffer,size,&envelope)||x4_json_member(envelope,"result",&result)!=1||fwrite(result.data,1,result.length,f)!=result.length)rc=-1;}
  else if(b->state.active&&b->current.cell==i&&b->current.modes[b->current_mode].present){
   X4T27Cell cell;(void)x4_t27_plan_cell(i,&cell);b->current.final=true;
   if(x4_t27_result_write(f,&b->current,cell.reference?NULL:&b->presentation))rc=-1;
  }else if(fprintf(f,"{\"index\":%u,\"status\":\"%s\",\"native_executed\":null}",i,
   b->state.closed_app_mask&(UINT64_C(1)<<i)?"closed_app":b->state.skipped_mask&(UINT64_C(1)<<i)?"skipped":"not_completed")<0)rc=-1;
 }
 if(!rc&&fputs("]}\n",f)==EOF)rc=-1;int closed=x4_t27_store_summary_finish(&b->store,f);return rc||closed?-7780:0;
}
static void t27_final_view(T27Bank *b)
{
 memset(&b->view,0,sizeof(b->view));b->view.total=X4_T27_CELL_COUNT;b->view.cell=X4_T27_CELL_COUNT-1u;b->view.done=true;b->view.retained=b->retained;
 t27_text(b->view.status,160,b->failure[0]?b->failure:"Banco terminado. Medidas locales; no demuestran streaming.");
 for(unsigned i=0;i<X4_T27_CELL_COUNT;++i){X4T27Cell cell;(void)x4_t27_plan_cell(i,&cell);unsigned mode=cell.reference?0u:1u;
  char rate[40]="no medido";if(b->recovered_valid[i]&&b->recovered[i].native_fps_available[mode])snprintf(rate,sizeof(rate),"%.2f salidas/s",b->recovered[i].native_fps[mode]);
  if(cell.reference)snprintf(b->view.rows[b->view.row_count++],160,"Base %s · máxima velocidad · %s",cell.clip==1u?"15 Mbps":"20 Mbps",rate);
  else{
   X4PipelinePresentationSummary p={0};X4T26RecoveredResult recovered;size_t bytes=0;bool ready=false;
   if(!x4_t27_store_cell_read(&b->store,i,b->read_buffer,X4_T27_STORE_CELL_MAX+1u,&bytes)){
    X4JsonSpan envelope,result;ready=!x4_json_parse(b->read_buffer,bytes,&envelope)&&x4_json_member(envelope,"result",&result)==1&&
     !x4_t27_result_read(result.data,result.length,X4_BUILD_ID,b->manifest,&recovered,&p);
   }
   unsigned presentation_mode=cell.presentation.vsync_policy==2u?X4_PRESENT_HSYNC:cell.presentation.mailbox?X4_PRESENT_VSYNC_MAILBOX:X4_PRESENT_VSYNC_COLA;
   const X4PipelinePresentationModeSummary *m=&p.mode[presentation_mode];
   double fps=ready&&m->frames>1u&&m->last_observed_us>m->first_observed_us?(double)(m->frames-1u)*1000000.0/(double)(m->last_observed_us-m->first_observed_us):0.0;
   char present_rate[40]="no medido";if(ready&&m->frames>1u&&m->last_observed_us>m->first_observed_us)snprintf(present_rate,sizeof(present_rate),"%.2f fps",fps);
   char timing[48]="sin medida";if(ready&&(m->decode_to_flip.finite_mask&3u)==3u)snprintf(timing,sizeof(timing),"%.2f/%.2f ms",(double)m->decode_to_flip.p50_us/1000.0,(double)m->decode_to_flip.p95_us/1000.0);
   snprintf(b->view.rows[b->view.row_count++],160,"%s · %u Hz · %s · %s · decode→flip %s",cell.clip==1u?"15M":"20M",cell.presentation.source_hz,
    cell.presentation.vsync_policy==2u?"Nunca":cell.presentation.mailbox?"Mailbox":"Cola",present_rate,timing);
  }
 }
 snprintf(b->view.rows[b->view.row_count++],160,"/data/xcloud4-probe/t27/results-%s.json · OPTIONS: salir",b->store.session);
}
static void *t27_bank_worker(void *argument)
{
 T27Bank *b=argument;int rc=t27_manifest_hash(b->manifest);
 if(!rc)rc=x4_t27_store_open(&b->store,X4_BUILD_ID,b->manifest,t27_clock_us(NULL),&b->state,&b->resumed);
 if(!rc)rc=t27_recover(b);if(rc){snprintf(b->failure,160,"No se puede guardar/reanudar T27 (%d). Conserva todos los archivos.",rc);goto finished;}
 if(b->state.terminal==X4_T27_TERMINAL_INTERRUPTED){t27_text(b->failure,160,"La ejecución previa se interrumpió en una llamada; banco detenido sin repetirla.");goto summarize;}
 for(unsigned n=0;n<2u&&!b->state.terminal;++n){unsigned clip=n+1u;memset(&b->view,0,sizeof(b->view));b->view.total=X4_T27_CELL_COUNT;b->view.mode=X4_T27_VIEW_PREPARING;
  snprintf(b->view.status,160,"Verificando SHA-256 del clip %s (solo lectura)…",x4_t26_clip_id(clip));if((rc=t27_handoff(b,BANK_SHOW)))goto finished;
  if((rc=x4_t26_clip_load(&b->clips[n],&x4_t26_generated_samples[clip],&b->cancel))){t27_text(b->failure,160,"Clip ausente/alterado o carga cancelada. No se inició el decoder.");goto finished;}
 }
 while(!b->state.terminal){
  if(t27_expired(b)||atomic_load_explicit(&b->cancel,memory_order_acquire)){rc=x4_t27_state_finish_budget(&b->state);if(!rc)rc=x4_t27_store_write_progress(&b->store,&b->state);break;}
  X4T27Cell cell;if(!x4_t27_plan_cell(b->state.next_cell,&cell)){rc=-7781;break;}
  unsigned base=cell.clip-1u;b->current_mode=cell.reference?X4_T26_MODE_MAXIMUM:X4_T26_MODE_PACED;
  memset(&b->current,0,sizeof(b->current));memset(&b->presentation,0,sizeof(b->presentation));
  t27_text(b->current.build_id,65,X4_BUILD_ID);t27_text(b->current.manifest_sha256,65,b->manifest);
  b->current.cell=cell.index;b->current.group=cell.reference?X4_T26_GROUP_BASE_EXTRA:X4_T26_GROUP_REALISTIC;b->current.clip=cell.clip;b->current.config=cell.config;
  rc=x4_t27_state_begin(&b->state,cell.index,b->current_mode,X4_T27_PHASE_QUERY);if(rc)break;
  if(!cell.reference&&!b->base_valid[base]){b->current.status=X4_T26_RESULT_NOT_RUN;t27_text(b->current.reason,128,"No hay hashes válidos de la base; no se ejecuta esta celda.");}
  else{
   memset(&b->view,0,sizeof(b->view));b->view.total=X4_T27_CELL_COUNT;b->view.cell=cell.index;b->view.mode=b->current_mode;b->view.remaining_us=x4_t27_plan_remaining_us(cell.index);
   snprintf(b->view.status,160,"%s · %s",x4_t26_clip_id(cell.clip),x4_t27_plan_mode(&cell));b->view.row_count=3;
   snprintf(b->view.rows[0],160,"%s: 180 AU de calentamiento; medida de 600 salidas (181…780).",cell.reference?"Base máxima velocidad":"Presentación GPU de producción");
   snprintf(b->view.rows[1],160,"Fuente %u Hz · decoder v2 base sin cambios · tres hashes 1/300/600",cell.reference?0u:cell.presentation.source_hz);
   t27_text(b->view.rows[2],160,"CÍRCULO u OPTIONS: detener con cierre seguro, tras la llamada nativa.");
   if((rc=t27_handoff(b,BANK_SHOW)))break;
   X4T26RunOps ops={.context=b,.now_us=t27_clock_us,.sleep_us=t27_sleep_us,.budget_expired=t27_expired,.before=t27_before_native,.cancel=&b->cancel,.display_open=t27_display_open,.display_close=t27_display_close};
   const char (*baseline)[65]=b->base_valid[base]?b->base[base]:NULL;
   if(cell.reference)rc=x4_t26_run_raw(&cell.config,b->current_mode,&x4_t26_generated_samples[cell.clip],&b->clips[base],NULL,&ops,&b->current.modes[b->current_mode],&b->retained_backend);
   else rc=x4_t26_run_realistic_options(&cell.config,&x4_t26_generated_samples[cell.clip],&b->clips[base],baseline,&ops,&cell.presentation,&b->presentation,&b->current.modes[b->current_mode],&b->retained_pipeline);
   b->retained=b->retained_backend||b->retained_pipeline;
   b->current.status=b->current.modes[b->current_mode].status;b->current.error_code=rc;
   if(rc)snprintf(b->current.reason,128,"Medida detenida con retorno %d; recursos retenidos=%u",rc,b->retained);
  }
  if(b->retained){if(!rc)rc=-7783;b->current.modes[b->current_mode].accepted=false;b->current.modes[b->current_mode].status=X4_T26_RESULT_LOCAL_ERROR;if(!b->current.modes[b->current_mode].local_result)b->current.modes[b->current_mode].local_result=rc;b->current.status=X4_T26_RESULT_LOCAL_ERROR;b->current.error_code=rc;t27_text(b->current.reason,128,"Cierre no confirmado; se conservan los recursos.");t27_text(b->failure,160,"Banco detenido: el cierre no probó la liberación de los recursos.");break;}
  {int committed=t27_commit_result(b,&cell);if(committed){rc=committed;break;}}
  printf("XCloud4: T27 celda=%u/11 clip=%s fuente_hz=%u vsync=%u mailbox=%u salidas=%llu errores=%llu rc=%d\n",
   cell.index+1u,x4_t26_clip_id(cell.clip),cell.presentation.source_hz,cell.presentation.vsync_policy,cell.presentation.mailbox,
   (unsigned long long)b->current.modes[b->current_mode].measured_outputs,(unsigned long long)b->current.modes[b->current_mode].error_outputs,rc);
  if(rc||!b->recovered[cell.index].outcome.accepted){if(!b->failure[0])snprintf(b->failure,160,"Celda %u sin resultado válido; banco detenido. Consulta retornos y hashes.",cell.index+1u);if(!b->state.terminal)(void)x4_t27_state_finish_budget(&b->state);(void)x4_t27_store_write_progress(&b->store,&b->state);break;}
 }
summarize:b->result=rc;if(t27_summary(b)&&!rc){rc=-7782;t27_text(b->failure,160,"No se confirmó el resumen; conserva las celdas y progress.json.");}
finished:
 b->result=rc;if(rc&&!b->failure[0])snprintf(b->failure,160,"Banco detenido, retorno %d. Conserva los archivos.",rc);
 if(!b->retained)for(unsigned i=0;i<2u;++i)x4_t26_clip_close(&b->clips[i]);
 t27_final_view(b);(void)t27_handoff(b,BANK_SHOW);(void)t27_handoff(b,BANK_FINISHED);
 atomic_store_explicit(&b->finished,true,memory_order_release);(void)x4_stream_wait_notify(&b->main_wait);return NULL;
}
static void t27_show_exit_failure(X4Display *display,const char *reason,int rc)
{
    X4T27ViewSnapshot view={.total=X4_T27_CELL_COUNT,.mode=X4_T27_VIEW_PREPARING,.done=true,.row_count=1};
    snprintf(view.status,sizeof(view.status),"%s (retorno %d).",reason,rc);
    t27_text(view.rows[0],sizeof(view.rows[0]),"Sonda detenida. Se solicitará salir al menú de PS4.");
    /* A failed Close/Unmap may have detached VideoOut already. Do not use a
     * detached handle; its reason remains in klog even if no flip is possible. */
    if(display&&display->handle>=0&&display->ui){
        int drawn=x4_t27_view_draw(x4_display_pixels(display),&view,NULL);
        if(!drawn)drawn=x4_display_present(display);
        if(drawn)printf("XCloud4: T27 aviso de salida no presentado rc=%d\n",drawn);
    }
    for(unsigned i=0;i<50;++i)sceKernelUsleep(100000);
}
static void t27_terminal_exit(T27Bank *b,X4Display *display,X4Controller *pad,bool ui_open,
    const char *reason,int rc) __attribute__((noreturn));
static void t27_terminal_exit(T27Bank *b,X4Display *display,X4Controller *pad,bool ui_open,
    const char *reason,int rc)
{
    /* reason may point inside T27Bank. Capture it before any successful cleanup. */
    char cause[160];t27_text(cause,sizeof(cause),reason);
    printf("XCloud4: T27 salida causa=%s rc=%d\n",cause,rc);
    if(b){
        if(b->retained||b->thread.created){
            printf("XCloud4: T27 salida conserva banco recursos=%u hilo=%u\n",
                b->retained,b->thread.created);
        }else{
            int worker=x4_stream_wait_destroy(&b->worker_wait);
            int main=x4_stream_wait_destroy(&b->main_wait);
            if(worker||main){
                printf("XCloud4: T27 salida conserva sincronización worker=%d main=%d\n",worker,main);
                if(!rc){rc=worker?worker:main;t27_text(cause,sizeof(cause),"No se pudo cerrar la sincronización del banco");}
            }else{free(b->read_buffer);free(b);}
        }
    }
    if(rc&&ui_open)t27_show_exit_failure(display,cause,rc);
    if(pad)x4_controller_close(pad);
    if(ui_open){
        int closed=x4_display_close_checked(display);
        if(closed){
            printf("XCloud4: T27 salida conserva VideoOut rc=%d\n",closed);
            if(!rc)t27_show_exit_failure(display,"No se pudo cerrar VideoOut",closed);
        }
    }else if(display&&(display->handle>=0||display->memory||display->allocated||display->ui)){
        /* open() already attempted checked cleanup. Do not retry an uncertain
         * native close or release any storage still owned by that failed call. */
        printf("XCloud4: T27 salida conserva apertura parcial de VideoOut\n");
    }
    if(display)x4_font_close();
    int prepared=x4_exit_prepare();
    printf("XCloud4: T27 salida prepare=%d\n",prepared);
    if(!prepared){int requested=x4_exit_request();printf("XCloud4: T27 salida request=%d\n",requested);}
    /* LoadExec is asynchronous; even its failure must never reach libc _exit.
     * Retained contexts and this caller's display storage stay alive here. */
    for(;;)sceKernelUsleep(100000);
}
int x4_t27_controller_run(void)
{
    T27Bank *b=calloc(1,sizeof(*b));if(!b)t27_terminal_exit(NULL,NULL,NULL,false,"No se pudo reservar el banco",-1);
    b->read_buffer=malloc(X4_T27_STORE_CELL_MAX+1u);
    if(!b->read_buffer)t27_terminal_exit(b,NULL,NULL,false,"No se pudo reservar el lector del banco",-1);
    atomic_init(&b->cancel,false);atomic_init(&b->finished,false);atomic_init(&b->command,0);atomic_init(&b->deadline,UINT64_MAX);
    int rc=x4_stream_wait_init(&b->main_wait,"x4-t27-ui");if(!rc)rc=x4_stream_wait_init(&b->worker_wait,"x4-t27-bank");
    if(rc)t27_terminal_exit(b,NULL,NULL,false,"No se pudo crear la sincronización del banco",rc);
    X4Display display={.handle=-1};X4Controller pad;X4StreamPipeline *active=NULL;
    (void)x4_font_init("/app0/assets/fonts/noto_sans.x4font");rc=x4_display_open(&display);
    if(rc)t27_terminal_exit(b,&display,NULL,false,"T27 no pudo abrir VideoOut",rc);
    bool ui_open=true;unsigned frame=0;x4_controller_init(&pad);
    memset(&b->view,0,sizeof(b->view));b->view.total=X4_T27_CELL_COUNT;b->view.mode=X4_T27_VIEW_PREPARING;
    t27_text(b->view.status,160,"Preparando el banco y comprobando la reanudación…");
    rc=x4_t27_view_draw(x4_display_pixels(&display),&b->view,NULL);
    if(!rc)rc=x4_display_present(&display);
    if(rc)t27_terminal_exit(b,&display,&pad,true,"No se pudo presentar el banco",rc);
    rc=x4_probe_thread_create(&b->id,&b->thread,t27_bank_worker,b,"x4-t27-bank");
    if(rc){
        /* The wrapper can exist after an attribute/startup failure, without
         * entering bank_worker. It still owns T27Bank until a successful Join. */
        if(b->thread.created){int joined=scePthreadJoin(b->id,NULL);x4_probe_thread_joined(&b->thread,joined);
            if(joined)b->retained=true;}
        t27_terminal_exit(b,&display,&pad,true,"No se pudo crear el hilo del banco",rc);
    }
    bool joined=!b->thread.created;
    for(;;){
        x4_controller_read(&pad,++frame);
        if(pad.pressed&(ORBIS_PAD_BUTTON_OPTIONS|ORBIS_PAD_BUTTON_CIRCLE)){
            atomic_store_explicit(&b->cancel,true,memory_order_release);b->exit_requested=true;}
        unsigned command=atomic_load_explicit(&b->command,memory_order_acquire);
        if(command){
            int result=0;
            if(command==BANK_SHOW&&ui_open){result=x4_t27_view_draw(x4_display_pixels(&display),&b->view,NULL);
                if(!result)result=x4_display_present(&display);}
            else if(command==BANK_DISPLAY_OPEN){
                if(ui_open){result=x4_display_close_checked(&display);if(!result)ui_open=false;}
                if(!result){result=x4_stream_pipeline_display_open(b->requested_pipeline);
                    if(!result)active=b->requested_pipeline;}
            }else if(command==BANK_DISPLAY_CLOSE){
                X4StreamPipeline *p=b->requested_pipeline;x4_stream_pipeline_request_stop(p);
                result=x4_stream_pipeline_close(p);X4StreamPipelineSnapshot s;x4_stream_pipeline_snapshot(p,&s);active=NULL;
                if(!ui_open&&s.display_detached){int opened=x4_display_open(&display);ui_open=opened==0;if(!result)result=opened;}
            }
            b->handoff_result=result;atomic_store_explicit(&b->command,0,memory_order_release);
            (void)x4_stream_wait_notify(&b->worker_wait);
        }
        if(active){int presented=x4_stream_pipeline_present(active,NULL,NULL);
            if(presented<0)atomic_store_explicit(&b->cancel,true,memory_order_release);
            unsigned state=x4_stream_pipeline_main_state(active);
            (void)x4_stream_pipeline_wait_present(active,state?1000:5000);
        }else (void)x4_stream_wait_block(&b->main_wait,20000);
        if(atomic_load_explicit(&b->finished,memory_order_acquire)&&!joined){
            rc=scePthreadJoin(b->id,NULL);x4_probe_thread_joined(&b->thread,rc);joined=rc==0;
            if(rc){b->retained=true;b->exit_requested=false;}
        }
        if(joined&&b->exit_requested&&!b->retained){
            t27_terminal_exit(b,&display,&pad,ui_open,b->failure[0]?b->failure:"Salida del banco solicitada",b->result);
        }
    }
}
#endif
