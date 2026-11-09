/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_plan.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <limits.h>
#include <string.h>

void x4_t26_config_base(X4T26Config *c)
{
    if(c)*c=(X4T26Config){.api=X4_T26_API_V2,.resource_type=1,.depth=2,
        .output_memory_type=0,.affinity_mask=UINT64_C(0x3f),.priority=700};
}
bool x4_t26_config_valid(const X4T26Config *c)
{
    if(!c)return false;
    if(c->api==X4_T26_API_V1)return c->resource_type==0&&c->depth==1&&
        c->output_memory_type==0&&!c->affinity_mask&&!c->priority&&
        !c->pipe_id&&!c->queue_id&&!c->realistic;
    return c->api==X4_T26_API_V2&&c->resource_type<=3&&
        (c->depth==1||c->depth==2)&&(c->output_memory_type==0||c->output_memory_type==3)&&
        (c->affinity_mask==UINT64_C(0x3f)||c->affinity_mask==UINT64_C(0x7f))&&
        (c->priority==700||c->priority==256||c->priority==512)&&c->pipe_id<=4&&!c->queue_id;
}
bool x4_t26_config_equal(const X4T26Config *a,const X4T26Config *b)
{
    return a&&b&&a->api==b->api&&a->resource_type==b->resource_type&&a->depth==b->depth&&
        a->output_memory_type==b->output_memory_type&&a->affinity_mask==b->affinity_mask&&
        a->priority==b->priority&&a->pipe_id==b->pipe_id&&a->queue_id==b->queue_id&&a->realistic==b->realistic;
}
unsigned x4_t26_factor_family(unsigned f)
{
    if(f==0)return X4_T26_FAMILY_BASE;
    if(f==1)return X4_T26_FAMILY_DEPTH;
    if(f==2)return X4_T26_FAMILY_MEMORY;
    if(f==3)return X4_T26_FAMILY_AFFINITY;
    if(f==4||f==5)return X4_T26_FAMILY_PRIORITY;
    if(f>=6&&f<=9)return X4_T26_FAMILY_PIPE;
    if(f>=10&&f<=12)return X4_T26_FAMILY_RESOURCE;
    return X4_T26_FAMILY_COUNT;
}
int x4_t26_factor_config(unsigned f,X4T26Config *c)
{
    if(!c||f>=X4_T26_FACTOR_COUNT)return -1;
    x4_t26_config_base(c);
    if(f==1)c->depth=1;
    else if(f==2)c->output_memory_type=3;
    else if(f==3)c->affinity_mask=UINT64_C(0x7f);
    else if(f==4)c->priority=256;
    else if(f==5)c->priority=512;
    else if(f>=6&&f<=9)c->pipe_id=f-5;
    else if(f>=10)c->resource_type=f==10?0:f-9;
    return 0;
}
const char *x4_t26_clip_id(unsigned c)
{
    static const char *const names[]={"1080p60-8M","1080p60-15M","1080p60-20M","1080p60-27M","720p60-8M"};
    return c<X4_T26_CLIP_COUNT?names[c]:NULL;
}
static void cell(X4T26Plan *p,unsigned i,unsigned group,unsigned clip,unsigned f,X4T26Config c)
{ p->cells[i]=(X4T26Cell){.index=i,.group=group,.clip=clip,.factor=f,.config=c}; }
void x4_t26_plan_init(X4T26Plan *p)
{
    if(!p)return;
    memset(p,0,sizeof(*p));X4T26Config base;x4_t26_config_base(&base);p->best=base;
    for(unsigned f=0;f<X4_T26_FACTOR_COUNT;++f){
        X4T26Config c;(void)x4_t26_factor_config(f,&c);
        cell(p,f*2,X4_T26_GROUP_FACTORS,X4_T26_CLIP_1080_15,f,c);
        cell(p,f*2+1,X4_T26_GROUP_FACTORS,X4_T26_CLIP_1080_27,f,c);
    }
    static const unsigned extra[]={X4_T26_CLIP_1080_8,X4_T26_CLIP_1080_20,X4_T26_CLIP_720_8};
    for(unsigned n=0;n<3;++n)cell(p,X4_T26_BASE_EXTRA_FIRST+n,X4_T26_GROUP_BASE_EXTRA,extra[n],0,base);
    for(unsigned n=0;n<X4_T26_CLIP_COUNT;++n)cell(p,X4_T26_BEST_FIRST+n,X4_T26_GROUP_BEST,n,UINT_MAX,base);
    X4T26Config display=base;display.realistic=true;
    for(unsigned n=0;n<4;++n)cell(p,X4_T26_REALISTIC_FIRST+n,X4_T26_GROUP_REALISTIC,
        n<2?X4_T26_CLIP_1080_15:X4_T26_CLIP_1080_20,(n&1)?UINT_MAX:0,display);
    X4T26Config v1={.api=X4_T26_API_V1,.depth=1};
    static const unsigned last[]={X4_T26_CLIP_1080_15,X4_T26_CLIP_1080_8,
        X4_T26_CLIP_1080_20,X4_T26_CLIP_1080_27,X4_T26_CLIP_720_8};
    for(unsigned n=0;n<5;++n)cell(p,X4_T26_V1_FIRST+n,X4_T26_GROUP_V1,last[n],UINT_MAX,v1);
}
const X4T26Cell *x4_t26_plan_cell(const X4T26Plan *p,unsigned i)
{
    if(!p||i>=X4_T26_CELL_COUNT)return NULL;
    const X4T26Cell *c=&p->cells[i];
    if(!p->best_ready&&(c->group==X4_T26_GROUP_BEST||
        (c->group==X4_T26_GROUP_REALISTIC&&c->factor==UINT_MAX)))return NULL;
    return c;
}
unsigned x4_t26_cell_first_mode(const X4T26Cell *c)
{ return c&&c->group==X4_T26_GROUP_REALISTIC?X4_T26_MODE_PACED:X4_T26_MODE_MAXIMUM; }
unsigned x4_t26_cell_mode_count(const X4T26Cell *c)
{ return c?(c->group==X4_T26_GROUP_REALISTIC?1u:2u):0u; }
int x4_t26_plan_note(X4T26Plan *p,unsigned i,const X4T26Outcome *o)
{
    if(!p||!o||i>=X4_T26_FACTOR_CELLS||!o->completed||p->best_ready)return -1;
    p->factors[i]=*o;return 0;
}
static bool good(const X4T26Outcome *o)
{ return o->completed&&o->accepted&&o->hashes_ok&&!o->errors&&o->decode_p95_us&&o->decode_p95_us<=X4_T26_BUDGET_US; }
static void merge(X4T26Config *to,const X4T26Config *from,unsigned family)
{
    if(family==X4_T26_FAMILY_DEPTH)to->depth=from->depth;
    else if(family==X4_T26_FAMILY_MEMORY)to->output_memory_type=from->output_memory_type;
    else if(family==X4_T26_FAMILY_AFFINITY)to->affinity_mask=from->affinity_mask;
    else if(family==X4_T26_FAMILY_PRIORITY)to->priority=from->priority;
    else if(family==X4_T26_FAMILY_PIPE)to->pipe_id=from->pipe_id;
    else if(family==X4_T26_FAMILY_RESOURCE)to->resource_type=from->resource_type;
}
static void publish_best(X4T26Plan *p)
{
    for(unsigned n=0;n<X4_T26_CLIP_COUNT;++n)p->cells[X4_T26_BEST_FIRST+n].config=p->best;
    for(unsigned n=1;n<4;n+=2){p->cells[X4_T26_REALISTIC_FIRST+n].config=p->best;
        p->cells[X4_T26_REALISTIC_FIRST+n].config.realistic=true;}
    p->best_ready=true;
}
int x4_t26_plan_select_best(X4T26Plan *p)
{
    if(!p||p->best_ready)return -1;
    for(unsigned i=0;i<X4_T26_FACTOR_CELLS;++i)if(!p->factors[i].completed)return -2;
    if(!good(&p->factors[0])||!good(&p->factors[1]))return -3;
    unsigned winners[X4_T26_FAMILY_COUNT]={0};
    uint64_t scores[X4_T26_FAMILY_COUNT];
    uint64_t base_score=p->factors[0].decode_p95_us+p->factors[1].decode_p95_us;
    for(unsigned i=0;i<X4_T26_FAMILY_COUNT;++i)scores[i]=base_score;
    for(unsigned f=1;f<X4_T26_FACTOR_COUNT;++f){
        const X4T26Outcome *a=&p->factors[f*2],*b=&p->factors[f*2+1];
        if(!good(a)||!good(b)||a->decode_p95_us>=p->factors[0].decode_p95_us||
            b->decode_p95_us>=p->factors[1].decode_p95_us)continue;
        unsigned family=x4_t26_factor_family(f);uint64_t score=a->decode_p95_us+b->decode_p95_us;
        if(score<scores[family]){scores[family]=score;winners[family]=f;}
    }
    x4_t26_config_base(&p->best);p->selected_factors=0;
    for(unsigned family=1;family<X4_T26_FAMILY_COUNT;++family)if(winners[family]){
        X4T26Config c;(void)x4_t26_factor_config(winners[family],&c);
        merge(&p->best,&c,family);p->selected_factors|=UINT32_C(1)<<winners[family];
    }
    publish_best(p);return 0;
}
int x4_t26_plan_restore_best(X4T26Plan *p,const X4T26Config *c)
{
    if(!p||!x4_t26_config_valid(c)||c->api!=X4_T26_API_V2||c->realistic)return -1;
    p->best=*c;p->selected_factors=0;X4T26Config base;x4_t26_config_base(&base);
    for(unsigned f=1;f<X4_T26_FACTOR_COUNT;++f){X4T26Config single,test=base;
        (void)x4_t26_factor_config(f,&single);unsigned family=x4_t26_factor_family(f);
        merge(&test,c,family);
        if(x4_t26_config_equal(&test,&single))p->selected_factors|=UINT32_C(1)<<f;
    }
    publish_best(p);return 0;
}
uint64_t x4_t26_plan_nominal_remaining_us(const X4T26Plan *p,unsigned next)
{
    if(!p||next>X4_T26_CELL_COUNT)return 0;
    uint64_t modes=0;for(unsigned i=next;i<X4_T26_CELL_COUNT;++i)modes+=x4_t26_cell_mode_count(&p->cells[i]);
    return modes*X4_T26_NOMINAL_MODE_US;
}
#endif
