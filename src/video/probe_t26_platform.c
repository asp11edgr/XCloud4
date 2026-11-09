/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_platform.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool timeval_us(const X4T26Timeval *t,uint64_t *out)
{
    if(t->seconds<0||t->microseconds<0||t->microseconds>=1000000)return false;
    uint64_t seconds=(uint64_t)t->seconds,microseconds=(uint64_t)t->microseconds;
    if(seconds>(UINT64_MAX-microseconds)/1000000)return false;
    *out=seconds*1000000+microseconds;return true;
}
bool x4_t26_rusage_cpu_us(const X4T26Rusage *r,uint64_t *out)
{
    uint64_t user,system;
    if(!r||!out||!timeval_us(&r->user,&user)||!timeval_us(&r->system,&system)||user>UINT64_MAX-system)return false;
    *out=user+system;return true;
}
void x4_t26_cpu_snapshot_with(X4T26Getrusage call,int resolve_rc,X4T26CpuSnapshot *out)
{
    if(!out)return;
    *out=(X4T26CpuSnapshot){.resolve_rc=resolve_rc,.process_rc=X4_T26_PLATFORM_NOT_RUN,
        .thread_rc=X4_T26_PLATFORM_NOT_RUN};
    if(!call)return;
    X4T26Rusage r;memset(&r,0xa5,sizeof(r));
    out->process_rc=call(0,&r);
    out->process_available=out->process_rc==0&&x4_t26_rusage_cpu_us(&r,&out->process_us);
    memset(&r,0xa5,sizeof(r));out->thread_rc=call(1,&r);
    out->thread_available=out->thread_rc==0&&x4_t26_rusage_cpu_us(&r,&out->thread_us);
}
void x4_t26_cpu_delta(const X4T26CpuSnapshot *before,const X4T26CpuSnapshot *after,
    uint64_t wall,X4T26CpuDelta *out)
{
    if(!out)return;
    *out=(X4T26CpuDelta){.wall_us=wall};
    if(!before||!after||!wall)return;
    if(before->process_available&&after->process_available&&after->process_us>=before->process_us){
        out->process_available=true;out->process_us=after->process_us-before->process_us;
        out->process_percent=100.0*(double)out->process_us/(double)wall;
    }
    if(before->thread_available&&after->thread_available&&after->thread_us>=before->thread_us){
        out->thread_available=true;out->thread_us=after->thread_us-before->thread_us;
        out->thread_percent=100.0*(double)out->thread_us/(double)wall;
    }
}
bool x4_t26_query_contains(const X4T26DirectQuery *q,const X4T26Allocation *a)
{
    if(!q||!a||a->physical_offset<0||!a->bytes||q->start<0||q->end<=q->start||q->memory_type!=a->memory_type)return false;
    uint64_t begin=(uint64_t)a->physical_offset;
    return begin<=UINT64_MAX-a->bytes&&(uint64_t)q->start<=begin&&
        (uint64_t)q->end>=begin+a->bytes;
}
bool x4_t26_query_release_evidence(bool before_valid,int after_rc)
{return before_valid&&(uint32_t)after_rc==UINT32_C(0x8002000d);}

#ifndef X4_T26_PLATFORM_HELPERS_TEST
#include "../core/module.h"
#include <orbis/libkernel.h>

typedef int32_t (*QueryFn)(int64_t,int32_t,X4T26DirectQuery *,size_t);
typedef int32_t (*AvailableFn)(int64_t,int64_t,size_t,int64_t *,size_t *);
typedef int32_t (*ModuleFn)(uint16_t);
_Static_assert(sizeof(QueryFn)==sizeof(void *),"T26 query function pointer");
_Static_assert(sizeof(AvailableFn)==sizeof(void *),"T26 available function pointer");
_Static_assert(sizeof(X4T26Getrusage)==sizeof(void *),"T26 rusage function pointer");
_Static_assert(sizeof(ModuleFn)==sizeof(void *),"T26 sysmodule function pointer");
struct X4T26Platform {
    X4T26BeforeNative before;void *context;
    X4T26PlatformInfo info;
    X4T26Getrusage usage;QueryFn query;AvailableFn available;
    ModuleFn is_loaded,load_module;
    bool kernel_resolved,sysmodule_resolved,decoder_resolved;
};
static void kernel_symbols(X4T26Platform *p)
{
    if(p->kernel_resolved)return;
    p->kernel_resolved=true;
    p->info.kernel_handle=x4_module_open("libkernel");
    if(p->info.kernel_handle<0){
        p->info.usage_resolve_rc=p->info.query_resolve_rc=p->info.available_resolve_rc=p->info.kernel_handle;return;
    }
    void *a=NULL;
    p->info.usage_resolve_rc=x4_module_symbol(p->info.kernel_handle,"getrusage",&a);
    if(p->info.usage_resolve_rc>=0&&a)memcpy(&p->usage,&a,sizeof(a));
    a=NULL;p->info.query_resolve_rc=x4_module_symbol(p->info.kernel_handle,"sceKernelDirectMemoryQuery",&a);
    if(p->info.query_resolve_rc>=0&&a)memcpy(&p->query,&a,sizeof(a));
    a=NULL;p->info.available_resolve_rc=x4_module_symbol(p->info.kernel_handle,"sceKernelAvailableDirectMemorySize",&a);
    if(p->info.available_resolve_rc>=0&&a)memcpy(&p->available,&a,sizeof(a));
}
int x4_t26_platform_create(X4T26Platform **out,X4T26BeforeNative before,void *context)
{
    if(!out||!before)return X4_T26_PLATFORM_CONTRACT;
    *out=NULL;
    X4T26Platform *p=calloc(1,sizeof(*p));if(!p)return X4_T26_V1_NO_MEMORY;
    p->before=before;p->context=context;
    p->info.kernel_handle=p->info.sysmodule_handle=p->info.decoder_handle=-1;
    p->info.usage_resolve_rc=p->info.query_resolve_rc=p->info.available_resolve_rc=X4_T26_PLATFORM_NOT_RUN;
    for(unsigned i=0;i<2;++i)p->info.modules[i]=(X4T26ModuleObservation){
        .before_rc=X4_T26_PLATFORM_NOT_RUN,.load_rc=X4_T26_PLATFORM_NOT_RUN,.after_rc=X4_T26_PLATFORM_NOT_RUN};
    *out=p;return 0;
}
static X4T26MemoryObservation *memory_record(X4T26Platform *p,int64_t offset)
{
    for(unsigned n=p->info.memory_observations;n;--n){
        X4T26MemoryObservation *r=&p->info.memory[n-1];
        if(r->offset==offset&&r->allocate_rc==0&&r->release_rc!=0)return r;
    }
    return NULL;
}
static int allocate_v1(void *opaque,uint64_t size,uint64_t alignment,int type,X4T26V1Memory *m)
{
    X4T26Platform *p=opaque;
    if(!p||!m||!size||size>X4_T26_V1_NATIVE_LIMIT||!alignment||(alignment&(alignment-1))||
        alignment>0x200000||(type!=0&&type!=3))return X4_T26_PLATFORM_CONTRACT;
    if(alignment<0x4000)alignment=0x4000;
    if(size>UINT64_MAX-(alignment-1))return X4_T26_PLATFORM_CONTRACT;
    size=(size+alignment-1)&~(alignment-1);
    if(size>X4_T26_V1_NATIVE_LIMIT||size>SIZE_MAX)return X4_T26_V1_BUDGET;
    *m=(X4T26V1Memory){.bytes=size,.offset=-1};
    X4T26MemoryObservation *observation=NULL;
    if(p->info.memory_observations<16){
        observation=&p->info.memory[p->info.memory_observations++];
        *observation=(X4T26MemoryObservation){.offset=-1,.bytes=size,.memory_type=type,
            .allocate_rc=X4_T26_PLATFORM_NOT_RUN,.map_rc=X4_T26_PLATFORM_NOT_RUN,
            .unmap_rc=X4_T26_PLATFORM_NOT_RUN,.release_rc=X4_T26_PLATFORM_NOT_RUN};
    }else ++p->info.memory_observations_dropped;
    off_t offset=-1;size_t limit=sceKernelGetDirectMemorySize();
    if(limit>INT64_MAX)return X4_T26_PLATFORM_CONTRACT;
    int rc=sceKernelAllocateDirectMemory(0,(off_t)limit,(size_t)size,(size_t)alignment,type,&offset);
    if(observation)observation->allocate_rc=rc;
    if(rc)return rc;
    m->allocated=true;m->offset=offset;if(observation)observation->offset=offset;
    if(offset<0)return X4_T26_PLATFORM_CONTRACT;
    rc=sceKernelMapDirectMemory(&m->pointer,(size_t)size,0x33,0,offset,(size_t)alignment);
    if(observation)observation->map_rc=rc;
    if(rc){m->pointer=NULL;return rc;}
    if(!m->pointer||((uintptr_t)m->pointer&(alignment-1)))return X4_T26_PLATFORM_CONTRACT;
    memset(m->pointer,0,(size_t)size);return 0;
}
static int release_v1(void *opaque,X4T26V1Memory *m)
{
    X4T26Platform *p=opaque;if(!p||!m)return X4_T26_PLATFORM_CONTRACT;
    if(!m->allocated)return 0;
    if(m->offset<0||!m->bytes||m->bytes>SIZE_MAX)return X4_T26_PLATFORM_CONTRACT;
    X4T26MemoryObservation *observation=memory_record(p,m->offset);
    if(m->pointer){
        int rc=sceKernelMunmap(m->pointer,(size_t)m->bytes);if(observation)observation->unmap_rc=rc;
        if(rc)return rc;
        m->pointer=NULL;
    }
    int rc=sceKernelReleaseDirectMemory((off_t)m->offset,(size_t)m->bytes);
    if(observation)observation->release_rc=rc;
    if(rc)return rc;
    m->allocated=false;return 0;
}
static int module_v1(void *opaque,unsigned id)
{
    X4T26Platform *p=opaque;if(!p||(id!=0x008e&&id!=0x00fc))return X4_T26_PLATFORM_CONTRACT;
    X4T26ModuleObservation *o=&p->info.modules[id==0x008e?0:1];o->module_id=id;
    if(!p->sysmodule_resolved){
        p->sysmodule_resolved=true;p->info.sysmodule_handle=x4_module_open("libSceSysmodule");
        if(p->info.sysmodule_handle<0)return p->info.sysmodule_handle;
        void *a=NULL;int rc=x4_module_symbol(p->info.sysmodule_handle,"sceSysmoduleIsLoaded",&a);
        if(rc<0||!a)return rc<0?rc:X4_T26_PLATFORM_CONTRACT;
        memcpy(&p->is_loaded,&a,sizeof(a));a=NULL;
        rc=x4_module_symbol(p->info.sysmodule_handle,"sceSysmoduleLoadModule",&a);
        if(rc<0||!a)return rc<0?rc:X4_T26_PLATFORM_CONTRACT;
        memcpy(&p->load_module,&a,sizeof(a));
    }
    if(!p->is_loaded||!p->load_module)return X4_T26_PLATFORM_CONTRACT;
    o->before_called=true;o->before_rc=p->is_loaded((uint16_t)id);
    if(o->before_rc){o->load_called=true;o->load_rc=p->load_module((uint16_t)id);if(o->load_rc)return o->load_rc;}
    o->after_called=true;o->after_rc=p->is_loaded((uint16_t)id);return o->after_rc;
}
static int resolve_v1(void *opaque,const char *symbol,void **address)
{
    X4T26Platform *p=opaque;
    if(!p||!symbol||!address)return X4_T26_PLATFORM_CONTRACT;
    *address=NULL;
    if(!p->decoder_resolved){p->decoder_resolved=true;p->info.decoder_handle=x4_module_open("libSceVideodec");}
    return p->info.decoder_handle<0?p->info.decoder_handle:x4_module_symbol(p->info.decoder_handle,symbol,address);
}
static int before_v1(void *opaque,const char *operation)
{X4T26Platform *p=opaque;return p&&p->before?p->before(p->context,operation):X4_T26_PLATFORM_CONTRACT;}
static uint64_t clock_v1(void *opaque)
{(void)opaque;return sceKernelGetProcessTime();}
void x4_t26_platform_v1_ops(X4T26Platform *p,X4T26V1Ops *out)
{
    if(out)*out=(X4T26V1Ops){.context=p,.allocate=allocate_v1,.release=release_v1,
        .load_module=module_v1,.resolve=resolve_v1,.before_native=before_v1,.clock_us=clock_v1};
}
void x4_t26_platform_info(const X4T26Platform *p,X4T26PlatformInfo *out)
{if(out)*out=p?p->info:(X4T26PlatformInfo){0};}
void x4_t26_platform_cpu(X4T26Platform *p,X4T26CpuSnapshot *out)
{
    if(!p){x4_t26_cpu_snapshot_with(NULL,X4_T26_PLATFORM_CONTRACT,out);return;}
    kernel_symbols(p);x4_t26_cpu_snapshot_with(p->usage,p->info.usage_resolve_rc,out);
}
int x4_t26_platform_query(X4T26Platform *p,int64_t offset,X4T26DirectQuery *out,bool *called)
{
    if(called)*called=false;
    if(!p||!out||!called||offset<0)return X4_T26_PLATFORM_CONTRACT;
    kernel_symbols(p);memset(out,0xa5,sizeof(*out));
    if(!p->query)return p->info.query_resolve_rc;
    *called=true;return p->query(offset,0,out,sizeof(*out));
}
int x4_t26_platform_largest(X4T26Platform *p,uint64_t *bytes,bool *called)
{
    if(called)*called=false;
    if(bytes)*bytes=0;
    if(!p||!bytes||!called)return X4_T26_PLATFORM_CONTRACT;
    kernel_symbols(p);if(!p->available)return p->info.available_resolve_rc;
    size_t total=sceKernelGetDirectMemorySize();if(total>INT64_MAX)return X4_T26_PLATFORM_CONTRACT;
    int64_t offset=-1;size_t largest=SIZE_MAX;*called=true;
    int rc=p->available(0,(int64_t)total,0x4000,&offset,&largest);
    if(!rc&&offset>=0&&largest<=total&&(uint64_t)offset<=total-largest)*bytes=largest;
    /* A success stub leaving sentinels is not a measured free block. */
    else if(!rc)return X4_T26_PLATFORM_CONTRACT;
    return rc;
}
void x4_t26_platform_free(X4T26Platform **p)
{if(p&&*p){free(*p);*p=NULL;}}
#endif
#endif
