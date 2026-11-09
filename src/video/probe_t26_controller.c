/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_controller.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_runner.h"
#include "probe_t26_realistic.h"
#include "probe_t26_store.h"
#include "probe_t26_view.h"
#include "stream_pipeline.h"
#include "stream_wait.h"
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
    X4T26State state;X4T26Plan plan;X4T26Store store;
    X4T26ClipData clips[5];X4T26RecoveredResult recovered[43];
    bool recovered_valid[43],base_valid[5];char base[5][3][65];
    X4T26CellResult current;
    X4T26ViewSnapshot view;
    X4ProbeThread thread;OrbisPthread id;
    X4StreamWait main_wait,worker_wait;
    atomic_uint command;atomic_bool cancel,finished;
    atomic_uint_fast64_t deadline;
    X4StreamPipeline *requested_pipeline,*retained_pipeline;
    X4T26Backend *retained_backend;
    int handoff_result,result;bool retained,resumed,exit_requested;
    unsigned current_mode;uint64_t restart_charge_us;
    char manifest[65],failure[160];
    char *read_buffer;
} Bank;
static uint64_t clock_us(void *ignored){(void)ignored;return sceKernelGetProcessTime();}
static void sleep_us(void *ignored,unsigned us){(void)ignored;sceKernelUsleep(us);}
static bool expired(void *context)
{Bank *b=context;return clock_us(NULL)>=atomic_load_explicit(&b->deadline,memory_order_acquire);}
static void text(char *out,size_t capacity,const char *s){snprintf(out,capacity,"%s",s?s:"");}
/* No sampler runs during measurement. Only phase boundaries charge the
 * cumulative20-minute budget and persist it; a restart conservatively adds
 * one full logical cell. Native calls themselves remain non-interruptible. */
static int charge(Bank *b,uint64_t now)
{
    X4T26State *s=&b->state;
    if(now<s->started_us||now-s->started_us>UINT64_MAX-s->budget_used_us)return -1;
    s->budget_used_us+=now-s->started_us;s->started_us=now;
    uint64_t remaining=s->budget_used_us<X4_T26_BUDGET_US?X4_T26_BUDGET_US-s->budget_used_us:0;
    s->deadline_us=now+remaining;
    atomic_store_explicit(&b->deadline,s->deadline_us,memory_order_release);
    return x4_t26_state_valid(s)?0:-1;
}
static unsigned phase(const char *op)
{
    if(strstr(op,"arbitration")||strstr(op,"Arbitration")||strstr(op,"00fc"))return X4_T26_PHASE_ARBITRATION;
    if(strstr(op,"Delete")||strstr(op,"delete"))return X4_T26_PHASE_DELETE;
    if(strstr(op,"QueryCompute"))return X4_T26_PHASE_QUERY_COMPUTE;
    if(strstr(op,"AllocateCompute"))return X4_T26_PHASE_ALLOCATE_QUEUE;
    if(strstr(op,"Query")||strstr(op,"query")||strstr(op,"load"))return X4_T26_PHASE_QUERY_DECODER;
    if(strstr(op,"Create")||strstr(op,"create"))return X4_T26_PHASE_CREATE;
    if(strstr(op,"Flush")||strstr(op,"flush"))return X4_T26_PHASE_FLUSH;
    if(strstr(op,"Decode")||strstr(op,"decode"))return X4_T26_PHASE_DECODE;
    if(strstr(op,"warmup"))return X4_T26_PHASE_WARMUP;
    return X4_T26_PHASE_QUERY_DECODER;
}
static int before_native(void *context,const char *op)
{
    Bank *b=context;
    if(!b->state.active)return -7670;
    unsigned next_phase=phase(op);bool closing=next_phase==X4_T26_PHASE_DELETE;
    if(!closing){
        if(atomic_load_explicit(&b->cancel,memory_order_acquire))return 3;
        if(expired(b))return 4;
    }
    if(charge(b,clock_us(NULL))||x4_t26_state_mark(&b->state,b->current_mode,next_phase)||
        x4_t26_store_write_progress(&b->store,&b->state))return -7671;
    /* The durable write can itself cross the limit or receive cancellation.
     * Admit no new work after it; safe Delete still records its marker and
     * remains possible after stop. Codes3/4 retain the runner stop contract. */
    if(!closing){
        if(atomic_load_explicit(&b->cancel,memory_order_acquire))return 3;
        if(expired(b))return 4;
    }
    return 0;
}
static int handoff(Bank *b,unsigned command)
{
    if(atomic_load_explicit(&b->command,memory_order_acquire))return -7672;
    b->handoff_result=0;atomic_store_explicit(&b->command,command,memory_order_release);
    (void)x4_stream_wait_notify(&b->main_wait);
    while(atomic_load_explicit(&b->command,memory_order_acquire)){
        int rc=x4_stream_wait_block(&b->worker_wait,20000);if(rc<0)return rc;
    }
    return b->handoff_result;
}
static int display_open(void *context,X4StreamPipeline *p)
{Bank *b=context;b->requested_pipeline=p;return handoff(b,BANK_DISPLAY_OPEN);}
static int display_close(void *context,X4StreamPipeline *p)
{Bank *b=context;b->requested_pipeline=p;return handoff(b,BANK_DISPLAY_CLOSE);}
static int manifest_hash(char out[65])
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
    for(unsigned i=0;!rc&&i<5;++i){const X4T26GeneratedSample *s=&x4_t26_generated_samples[i];
        rc=H_UPDATE(&hash,(const unsigned char *)s->id,strlen(s->id)+1);
        if(!rc)rc=H_UPDATE(&hash,s->clip_sha256,32);
        if(!rc)rc=H_UPDATE(&hash,s->index_sha256,32);
    }
    if(!rc)rc=H_FINISH(&hash,digest);
    mbedtls_sha256_free(&hash);
    if(rc)return rc;
    static const char h[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){out[2*i]=h[digest[i]>>4];out[2*i+1]=h[digest[i]&15];}out[64]=0;return 0;
}
static int read_cell(Bank *b,unsigned cell,X4T26RecoveredResult *out)
{
    size_t bytes=0;int rc=x4_t26_store_cell_read(&b->store,cell,b->read_buffer,X4_T26_STORE_CELL_MAX+1u,&bytes);
    if(rc)return rc;
    X4JsonSpan envelope,result;
    if(x4_json_parse(b->read_buffer,bytes,&envelope)||x4_json_member(envelope,"result",&result)!=1)return -7673;
    return x4_t26_result_read(result.data,result.length,X4_BUILD_ID,b->manifest,out)?-7674:0;
}
static int recover(Bank *b)
{
    uint64_t durable=0;bool v1_passed=false;
    for(unsigned i=0;i<43;++i){
        int rc=read_cell(b,i,&b->recovered[i]);
        if(rc==1)continue;
        if(rc==2){
            if((!b->state.active||b->state.active_cell!=i)&&
                !(b->state.closed_app_mask&(UINT64_C(1)<<i)))return -7675;
            continue;
        }
        if(rc)return rc;
        if(b->recovered[i].cell!=i)return -7676;
        b->recovered_valid[i]=true;durable|=UINT64_C(1)<<i;
        if(i==38)v1_passed=b->recovered[i].outcome.accepted&&b->recovered[i].outcome.hashes_ok;
    }
    bool interrupted=b->state.active&&!(durable&(UINT64_C(1)<<b->state.active_cell));
    if(x4_t26_state_resume(&b->state,durable,v1_passed)<0)return -7677;
    uint64_t used=b->state.budget_used_us;
    if(interrupted){b->restart_charge_us=2*X4_T26_NOMINAL_MODE_US;used+=b->restart_charge_us;}
    if((b->state.completed_mask||b->state.skipped_mask)&&!used)used=1;
    if(x4_t26_state_rebase(&b->state,clock_us(NULL),used))return -7678;
    atomic_store_explicit(&b->deadline,b->state.deadline_us,memory_order_release);
    if(x4_t26_store_write_progress(&b->store,&b->state))return -7679;
    for(unsigned i=0;i<43;++i){
        if(i<26){X4T26Outcome o={.completed=true};
            if(b->recovered_valid[i])o=b->recovered[i].outcome;
            if((b->state.completed_mask|b->state.skipped_mask)&(UINT64_C(1)<<i))
                if(x4_t26_plan_note(&b->plan,i,&o))return -7680;}
        if(b->recovered_valid[i]&&b->recovered[i].base_hashes_valid){
            unsigned clip=b->recovered[i].clip;memcpy(b->base[clip],b->recovered[i].hashes[0],sizeof(b->base[clip]));b->base_valid[clip]=true;}
    }
    if(b->state.best_ready)return x4_t26_plan_restore_best(&b->plan,&b->state.best);
    return 0;
}
static void show_cell(Bank *b,const X4T26Cell *c,unsigned mode)
{
    memset(&b->view,0,sizeof(b->view));b->view.cell=c->index;b->view.total=43;b->view.mode=mode;
    b->view.remaining_us=x4_t26_plan_nominal_remaining_us(&b->plan,c->index);
    snprintf(b->view.status,sizeof(b->view.status),"%s · API v%u · profundidad %u",x4_t26_clip_id(c->clip),c->config.api,c->config.depth);
    b->view.row_count=4;
    snprintf(b->view.rows[0],160,"Recurso %u · salida %s · prioridad %d · afinidad 0x%llx",c->config.resource_type,
        c->config.output_memory_type==3?"WC_GARLIC":"WB_ONION",c->config.priority,(unsigned long long)c->config.affinity_mask);
    snprintf(b->view.rows[1],160,"Compute pipe %u / queue %u · calentamiento: 180 AU a 60 Hz",c->config.pipe_id,c->config.queue_id);
    text(b->view.rows[2],160,c->config.realistic?"Medida realista: GPU y presentación de producción a 60 Hz.":"Medida sin presentación: 600 salidas; pantalla quieta para no competir.");
    text(b->view.rows[3],160,"CÍRCULO u OPTIONS: detener tras terminar la llamada nativa en curso.");
}
static void initialize_result(Bank *b,const X4T26Cell *c)
{
    memset(&b->current,0,sizeof(b->current));X4T26CellResult *r=&b->current;
    text(r->build_id,sizeof(r->build_id),X4_BUILD_ID);text(r->manifest_sha256,sizeof(r->manifest_sha256),b->manifest);
    r->cell=c->index;r->group=c->group;r->clip=c->clip;r->factor=c->factor;r->config=c->config;
}
static int commit_result(Bank *b)
{
    X4T26CellResult *r=&b->current;r->final=true;
    FILE *f=x4_t26_store_cell_begin(&b->store,r->cell);if(!f)return -7681;
    if(x4_t26_result_write(f,r)){
        /* Finish still closes/preserves the partial file; it must never be
         * treated as a durable cell by the recovery reader. */
        (void)x4_t26_store_cell_finish(&b->store,f);return -7682;}
    if(x4_t26_store_cell_finish(&b->store,f))return -7683;
    int rc=read_cell(b,r->cell,&b->recovered[r->cell]);if(rc)return rc;
    b->recovered_valid[r->cell]=true;
    X4T26RecoveredResult *got=&b->recovered[r->cell];
    if(r->cell<26&&x4_t26_plan_note(&b->plan,r->cell,&got->outcome))return -7684;
    if(got->base_hashes_valid){memcpy(b->base[r->clip],got->hashes[0],sizeof(b->base[r->clip]));b->base_valid[r->clip]=true;}
    if(x4_t26_state_commit_cell(&b->state,r->cell,got->outcome.accepted&&got->outcome.hashes_ok))return -7685;
    if(charge(b,clock_us(NULL))||x4_t26_store_write_progress(&b->store,&b->state))return -7686;
    printf("XCloud4: T26 celda=%u/%u clip=%s api=%u depth=%u estado=%u errores=%llu p95_max_us=%llu cierre_limpio=%u\n",
        r->cell+1,43,x4_t26_clip_id(r->clip),r->config.api,r->config.depth,r->status,
        (unsigned long long)got->outcome.errors,(unsigned long long)got->outcome.decode_p95_us,!b->retained);
    return 0;
}
static int summary_failed(Bank *b,FILE *f,int error)
{(void)x4_t26_store_summary_finish(&b->store,f);return error;}
static bool same_integer(X4JsonSpan value,uint64_t expected)
{
    char digits[32];int n=snprintf(digits,sizeof(digits),"%" PRIu64,expected);
    return n>0&&x4_json_type(value)==X4_JSON_T_NUMBER&&value.length==(size_t)n&&!memcmp(value.data,digits,(size_t)n);
}
static int summary(Bank *b)
{
    /* A completed bank is viewable again. Reuse its identity-validated
     * existing summary; never replace a prior result or an interrupted file. */
    char path[X4_T26_STORE_PATH_MAX];
    if(x4_t26_store_summary_path(&b->store,path,sizeof(path)))return -7687;
    errno=0;FILE *old=fopen(path,"rb");
    if(old){
        int valid=0;if(!fseek(old,0,SEEK_END)){long n=ftell(old);
            if(n>0&&(uint64_t)n<=X4_T26_STORE_SUMMARY_MAX&&!fseek(old,0,SEEK_SET)){
                char *data=malloc((size_t)n+1u);if(data){
                    if(fread(data,1,(size_t)n,old)==(size_t)n&&!ferror(old)){
                        X4JsonSpan root,v;char token[65];data[n]=0;
                        valid=!x4_json_parse(data,(size_t)n,&root)&&
                            x4_json_member(root,"schema",&v)==1&&!x4_json_token(v,token,sizeof(token))&&!strcmp(token,"X4T26_BANK1")&&
                            x4_json_member(root,"build",&v)==1&&!x4_json_token(v,token,sizeof(token))&&!strcmp(token,X4_BUILD_ID)&&
                            x4_json_member(root,"manifest",&v)==1&&!x4_json_token(v,token,sizeof(token))&&!strcmp(token,b->manifest)&&
                            x4_json_member(root,"session",&v)==1&&!x4_json_token(v,token,sizeof(token))&&!strcmp(token,b->store.session);
                        if(valid){uint32_t terminal=0;X4JsonSpan cells,item;
                            valid=x4_json_member(root,"terminal",&v)==1&&!x4_json_uint32(v,&terminal)&&terminal==b->state.terminal&&
                                x4_json_member(root,"completed_mask",&v)==1&&same_integer(v,b->state.completed_mask)&&
                                x4_json_member(root,"closed_app_mask",&v)==1&&same_integer(v,b->state.closed_app_mask)&&
                                x4_json_member(root,"skipped_mask",&v)==1&&same_integer(v,b->state.skipped_mask)&&
                                x4_json_member(root,"cells",&cells)==1&&x4_json_type(cells)==X4_JSON_T_ARRAY;
                            size_t cursor=0;unsigned count=0;
                            while(valid&&x4_json_item(cells,&cursor,&item)==1){uint32_t index=0;
                                valid=count<43&&x4_json_member(item,"cell",&v)==1&&!x4_json_uint32(v,&index)&&index==count;
                                ++count;
                            }
                            valid=valid&&count==43;
                        }
                    }
                    free(data);
                }
            }
        }
        if(fclose(old))valid=0;return valid?0:-7687;
    }
    if(errno!=ENOENT)return -7687;
    FILE *f=x4_t26_store_summary_begin(&b->store);if(!f)return -7687;
    if(fprintf(f,"{\"schema\":\"X4T26_BANK1\",\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\","
        "\"budget_used_us\":%" PRIu64 ",\"restart_charge_us\":%" PRIu64 ",\"terminal\":%u,\"completed_mask\":%" PRIu64 ",\"closed_app_mask\":%" PRIu64
        ",\"skipped_mask\":%" PRIu64 ",\"bank_result\":%d,\"retained\":%s,\"cancelled\":%s,\"cells\":[",
        X4_BUILD_ID,b->manifest,b->store.session,b->state.budget_used_us,b->restart_charge_us,b->state.terminal,b->state.completed_mask,
        b->state.closed_app_mask,b->state.skipped_mask,b->result,b->retained?"true":"false",
        atomic_load_explicit(&b->cancel,memory_order_acquire)?"true":"false")<0)return summary_failed(b,f,-7688);
    for(unsigned i=0;i<43;++i){if(i&&fputc(',',f)==EOF)return summary_failed(b,f,-7688);
        int rc=x4_t26_store_cell_read(&b->store,i,b->read_buffer,X4_T26_STORE_CELL_MAX+1u,&(size_t){0});
        if(rc==0){X4JsonSpan envelope,result;
            size_t length=strlen(b->read_buffer);
            if(x4_json_parse(b->read_buffer,length,&envelope)||x4_json_member(envelope,"result",&result)!=1||
                fwrite(result.data,1,result.length,f)!=result.length)return summary_failed(b,f,-7689);
        }else if(b->state.active&&i==b->current.cell&&b->current.modes[b->current_mode].present){
            b->current.final=true;if(x4_t26_result_write(f,&b->current))return summary_failed(b,f,-7689);
        }else if(fprintf(f,"{\"cell\":%u,\"status\":\"%s\",\"native_executed\":null}",i,
            b->state.closed_app_mask&(UINT64_C(1)<<i)?"closed_app":
            b->state.skipped_mask&(UINT64_C(1)<<i)?"skipped":"not_completed")<0)return summary_failed(b,f,-7688);
    }
    if(fputs("]}\n",f)==EOF)return summary_failed(b,f,-7688);
    return x4_t26_store_summary_finish(&b->store,f);
}
static double fps(const X4T26ModeResult *m)
{return m->measured_wall_us?(double)m->measured_outputs*1000000.0/(double)m->measured_wall_us:0.0;}
static void final_view(Bank *b)
{
    memset(&b->view,0,sizeof(b->view));b->view.total=43;b->view.cell=42;b->view.done=true;b->view.retained=b->retained;
    text(b->view.status,160,b->failure[0]?b->failure:b->state.terminal==X4_T26_TERMINAL_V1_CRASH?
        "La API v1 cerró la app. Banco terminado; no se ejecutaron más celdas.":"Banco terminado. Resultados guardados; no se ha probado streaming.");
    b->view.row_count=0;
    for(unsigned clip=0;clip<5&&b->store.session[0];++clip){
        unsigned base=clip==0?26:clip==1?0:clip==2?27:clip==3?1:28;
        unsigned best=29+clip;char a[32]="no medido",z[32]="no medido";
        /* Recovery stores outcomes, while full modes stay in durable JSON.
         * Read final rows from their exact result objects without guessing. */
        if(b->recovered_valid[base]&&b->recovered[base].native_fps_available[0])snprintf(a,sizeof(a),"%.2f salidas/s",b->recovered[base].native_fps[0]);
        if(b->recovered_valid[best]&&b->recovered[best].native_fps_available[0])snprintf(z,sizeof(z),"%.2f salidas/s",b->recovered[best].native_fps[0]);
        snprintf(b->view.rows[b->view.row_count++],160,"%s · base %s · mejor v2 %s",x4_t26_clip_id(clip),a,z);
    }
    if(b->recovered_valid[38]&&b->recovered[38].native_fps_available[0])
        snprintf(b->view.rows[b->view.row_count++],160,"API v1 · 15 Mbps: %.2f salidas/s · comparación de toda la curva en JSON",b->recovered[38].native_fps[0]);
    else text(b->view.rows[b->view.row_count++],160,"API v1: no hay una medida válida de15 Mbps. Consulta retornos y estado en JSON.");
    snprintf(b->view.rows[b->view.row_count++],160,"Resultados: /data/xcloud4-probe/t26/results-%s.json",b->store.session);
    text(b->view.rows[b->view.row_count++],160,b->retained?"Recursos retenidos: banco detenido. No se inicia ninguna otra celda.":"OPTIONS: salir. Conserva el informe y los clips para que se analicen.");
}
static void *bank_worker(void *argument)
{
    Bank *b=argument;x4_t26_plan_init(&b->plan);
    int rc=manifest_hash(b->manifest);
    if(!rc)rc=x4_t26_store_open(&b->store,X4_BUILD_ID,b->manifest,clock_us(NULL),&b->state,&b->resumed);
    if(!rc)rc=recover(b);
    if(rc){snprintf(b->failure,160,"No se puede reanudar/guardar el banco (%d). Conserva progress.json.",rc);goto finished;}
    for(unsigned clip=0;clip<5&&!b->state.terminal;++clip){
        memset(&b->view,0,sizeof(b->view));b->view.total=43;b->view.mode=X4_T26_VIEW_PREPARING;
        snprintf(b->view.status,160,"Verificando SHA-256 y preparando %s (%u/5)…",x4_t26_clip_id(clip),clip+1);
        if((rc=handoff(b,BANK_SHOW)))goto finished;
        rc=x4_t26_clip_load(&b->clips[clip],&x4_t26_generated_samples[clip],&b->cancel);
        if(rc){snprintf(b->failure,160,"Muestra ausente/alterada o preparación cancelada (%d). No se inició el decoder.",rc);goto finished;}
    }
    while(!b->state.terminal){
        if(expired(b)||atomic_load_explicit(&b->cancel,memory_order_acquire)){
            rc=x4_t26_state_finish_budget(&b->state);if(!rc)rc=x4_t26_store_write_progress(&b->store,&b->state);break;}
        if(b->state.next_cell>=29&&!b->plan.best_ready){
            rc=x4_t26_plan_select_best(&b->plan);
            if(rc){snprintf(b->failure,160,"La base no produjo referencias válidas. Se detiene la comparación (%d).",rc);break;}
            rc=x4_t26_state_set_best(&b->state,&b->plan.best);
            if(!rc)rc=x4_t26_store_write_progress(&b->store,&b->state);
            if(rc)break;
        }
        const X4T26Cell *cell=x4_t26_plan_cell(&b->plan,b->state.next_cell);
        if(!cell){rc=-7690;break;}initialize_result(b,cell);
        unsigned first=x4_t26_cell_first_mode(cell),end=first+x4_t26_cell_mode_count(cell);
        rc=x4_t26_state_begin(&b->state,cell->index,first,X4_T26_PHASE_QUERY_DECODER);if(rc)break;
        bool reference=(cell->group==X4_T26_GROUP_FACTORS&&cell->factor==0)||cell->group==X4_T26_GROUP_BASE_EXTRA;
        if(!reference&&!b->base_valid[cell->clip]){
            b->current.status=X4_T26_RESULT_NOT_RUN;
            text(b->current.reason,128,"No hay hashes válidos de la base de este clip; no se ejecutan llamadas nativas sin comparación.");
            rc=commit_result(b);if(rc)break;continue;
        }
        for(unsigned mode=first;mode<end;++mode){
            b->current_mode=mode;
            if((rc=before_native(b,"query")))break;
            show_cell(b,cell,mode);if((rc=handoff(b,BANK_SHOW)))break;
            const char (*baseline)[65]=b->base_valid[cell->clip]?b->base[cell->clip]:NULL;
            if(reference&&mode==X4_T26_MODE_PACED&&b->current.modes[0].accepted){
                for(unsigned h=0;h<3;++h)text(b->base[cell->clip][h],65,b->current.modes[0].hashes[h].sha256);
                baseline=b->base[cell->clip];
            }
            X4T26RunOps ops={.context=b,.now_us=clock_us,.sleep_us=sleep_us,.budget_expired=expired,
                .before=before_native,.cancel=&b->cancel,.display_open=display_open,.display_close=display_close};
            if(cell->config.realistic)rc=x4_t26_run_realistic(&cell->config,&x4_t26_generated_samples[cell->clip],
                &b->clips[cell->clip],baseline,&ops,&b->current.modes[mode],&b->retained_pipeline);
            else rc=x4_t26_run_raw(&cell->config,mode,&x4_t26_generated_samples[cell->clip],
                &b->clips[cell->clip],baseline,&ops,&b->current.modes[mode],&b->retained_backend);
            b->retained=b->retained_backend||b->retained_pipeline;
            if(rc||b->retained){b->current.status=b->current.modes[mode].status;b->current.error_code=rc;
                snprintf(b->current.reason,128,"Modo %u detenido, retorno %d; recursos retenidos=%u",mode,rc,b->retained);break;}
            printf("XCloud4: T26 modo celda=%u modo=%u salidas=%llu tasa=%.3f sin_salida=%llu drenadas=%llu\n",
                cell->index+1,mode,(unsigned long long)b->current.modes[mode].measured_outputs,fps(&b->current.modes[mode]),
                (unsigned long long)b->current.modes[mode].no_output_calls,(unsigned long long)b->current.modes[mode].drained_outputs);
        }
        if(rc&&b->current.status==X4_T26_RESULT_PASS){b->current.status=X4_T26_RESULT_LOCAL_ERROR;
            b->current.error_code=rc;snprintf(b->current.reason,128,"Preparación/medida detenida (%d).",rc);}
        if(b->retained){text(b->failure,160,"Banco detenido: el cierre no probó la liberación de todos los recursos.");break;}
        int committed=commit_result(b);if(committed){rc=committed;break;}
        if(rc==3||rc==4){if(!b->state.terminal)(void)x4_t26_state_finish_budget(&b->state);(void)x4_t26_store_write_progress(&b->store,&b->state);break;}
        rc=0;
    }
    b->result=rc;
    if(summary(b)&&!rc){rc=-7691;text(b->failure,160,"El banco terminó, pero no se confirmó el guardado del resumen.");}
finished:
    b->result=rc;if(rc&&!b->failure[0])snprintf(b->failure,160,"Banco detenido, retorno %d. Conserva los archivos para revisión.",rc);
    if(!b->retained)for(unsigned i=0;i<5;++i)x4_t26_clip_close(&b->clips[i]);
    final_view(b);(void)handoff(b,BANK_SHOW);
    (void)handoff(b,BANK_FINISHED);
    atomic_store_explicit(&b->finished,true,memory_order_release);
    (void)x4_stream_wait_notify(&b->main_wait);return NULL;
}
static void show_exit_failure(X4Display *display,const char *reason,int rc)
{
    X4T26ViewSnapshot view={.total=43,.mode=X4_T26_VIEW_PREPARING,.done=true,.row_count=1};
    snprintf(view.status,sizeof(view.status),"%s (retorno %d).",reason,rc);
    text(view.rows[0],sizeof(view.rows[0]),"Sonda detenida. Se solicitará salir al menú de PS4.");
    /* A failed Close/Unmap may have detached VideoOut already. Do not use a
     * detached handle; its reason remains in klog even if no flip is possible. */
    if(display&&display->handle>=0&&display->ui){
        int drawn=x4_t26_view_draw(x4_display_pixels(display),&view,NULL);
        if(!drawn)drawn=x4_display_present(display);
        if(drawn)printf("XCloud4: T26 aviso de salida no presentado rc=%d\n",drawn);
    }
    for(unsigned i=0;i<50;++i)sceKernelUsleep(100000);
}
static void terminal_exit(Bank *b,X4Display *display,X4Controller *pad,bool ui_open,
    const char *reason,int rc) __attribute__((noreturn));
static void terminal_exit(Bank *b,X4Display *display,X4Controller *pad,bool ui_open,
    const char *reason,int rc)
{
    /* reason may point inside Bank. Capture it before any successful cleanup. */
    char cause[160];text(cause,sizeof(cause),reason);
    printf("XCloud4: T26 salida causa=%s rc=%d\n",cause,rc);
    if(b){
        if(b->retained||b->thread.created){
            printf("XCloud4: T26 salida conserva banco recursos=%u hilo=%u\n",
                b->retained,b->thread.created);
        }else{
            int worker=x4_stream_wait_destroy(&b->worker_wait);
            int main=x4_stream_wait_destroy(&b->main_wait);
            if(worker||main){
                printf("XCloud4: T26 salida conserva sincronización worker=%d main=%d\n",worker,main);
                if(!rc){rc=worker?worker:main;text(cause,sizeof(cause),"No se pudo cerrar la sincronización del banco");}
            }else{free(b->read_buffer);free(b);}
        }
    }
    if(rc&&ui_open)show_exit_failure(display,cause,rc);
    if(pad)x4_controller_close(pad);
    if(ui_open){
        int closed=x4_display_close_checked(display);
        if(closed){
            printf("XCloud4: T26 salida conserva VideoOut rc=%d\n",closed);
            if(!rc)show_exit_failure(display,"No se pudo cerrar VideoOut",closed);
        }
    }else if(display&&(display->handle>=0||display->memory||display->allocated||display->ui)){
        /* open() already attempted checked cleanup. Do not retry an uncertain
         * native close or release any storage still owned by that failed call. */
        printf("XCloud4: T26 salida conserva apertura parcial de VideoOut\n");
    }
    if(display)x4_font_close();
    int prepared=x4_exit_prepare();
    printf("XCloud4: T26 salida prepare=%d\n",prepared);
    if(!prepared){int requested=x4_exit_request();printf("XCloud4: T26 salida request=%d\n",requested);}
    /* LoadExec is asynchronous; even its failure must never reach libc _exit.
     * Retained contexts and this caller's display storage stay alive here. */
    for(;;)sceKernelUsleep(100000);
}
int x4_t26_controller_run(void)
{
    Bank *b=calloc(1,sizeof(*b));if(!b)terminal_exit(NULL,NULL,NULL,false,"No se pudo reservar el banco",-1);
    b->read_buffer=malloc(X4_T26_STORE_CELL_MAX+1u);
    if(!b->read_buffer)terminal_exit(b,NULL,NULL,false,"No se pudo reservar el lector del banco",-1);
    atomic_init(&b->cancel,false);atomic_init(&b->finished,false);atomic_init(&b->command,0);atomic_init(&b->deadline,UINT64_MAX);
    int rc=x4_stream_wait_init(&b->main_wait,"x4-t26-ui");if(!rc)rc=x4_stream_wait_init(&b->worker_wait,"x4-t26-bank");
    if(rc)terminal_exit(b,NULL,NULL,false,"No se pudo crear la sincronización del banco",rc);
    X4Display display={.handle=-1};X4Controller pad;X4StreamPipeline *active=NULL;
    (void)x4_font_init("/app0/assets/fonts/noto_sans.x4font");rc=x4_display_open(&display);
    if(rc)terminal_exit(b,&display,NULL,false,"T26 no pudo abrir VideoOut",rc);
    bool ui_open=true;unsigned frame=0;x4_controller_init(&pad);
    memset(&b->view,0,sizeof(b->view));b->view.total=43;b->view.mode=X4_T26_VIEW_PREPARING;
    text(b->view.status,160,"Preparando el banco y comprobando la reanudación…");
    rc=x4_t26_view_draw(x4_display_pixels(&display),&b->view,NULL);
    if(!rc)rc=x4_display_present(&display);
    if(rc)terminal_exit(b,&display,&pad,true,"No se pudo presentar el banco",rc);
    rc=x4_probe_thread_create(&b->id,&b->thread,bank_worker,b,"x4-t26-bank");
    if(rc){
        /* The wrapper can exist after an attribute/startup failure, without
         * entering bank_worker. It still owns Bank until a successful Join. */
        if(b->thread.created){int joined=scePthreadJoin(b->id,NULL);x4_probe_thread_joined(&b->thread,joined);
            if(joined)b->retained=true;}
        terminal_exit(b,&display,&pad,true,"No se pudo crear el hilo del banco",rc);
    }
    bool joined=!b->thread.created;
    for(;;){
        x4_controller_read(&pad,++frame);
        if(pad.pressed&(ORBIS_PAD_BUTTON_OPTIONS|ORBIS_PAD_BUTTON_CIRCLE)){
            atomic_store_explicit(&b->cancel,true,memory_order_release);b->exit_requested=true;}
        unsigned command=atomic_load_explicit(&b->command,memory_order_acquire);
        if(command){
            int result=0;
            if(command==BANK_SHOW&&ui_open){result=x4_t26_view_draw(x4_display_pixels(&display),&b->view,NULL);
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
            terminal_exit(b,&display,&pad,ui_open,b->failure[0]?b->failure:"Salida del banco solicitada",b->result);
        }
    }
}
#endif
