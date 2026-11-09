/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_t26_plan.h"
#include "../src/video/probe_t26_state.h"
#include "../src/video/decoder_depth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static unsigned checks,mutations,logical_frees;
static const char manifest[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static unsigned different(const X4T26Config *a,const X4T26Config *b)
{
    return (a->api!=b->api)+(a->resource_type!=b->resource_type)+(a->depth!=b->depth)+
        (a->output_memory_type!=b->output_memory_type)+(a->affinity_mask!=b->affinity_mask)+
        (a->priority!=b->priority)+(a->pipe_id!=b->pipe_id)+(a->queue_id!=b->queue_id)+(a->realistic!=b->realistic);
}
static X4T26State fresh(void)
{
    X4T26State s;CHECK(x4_t26_state_init(&s,"0123456789ab-t26",manifest,1000000)==0);return s;
}
static void roundtrip(const X4T26State *s)
{
    char a[X4_T26_STATE_TEXT_MAX],b[X4_T26_STATE_TEXT_MAX];X4T26State copy;
    int n=x4_t26_state_serialize(s,a,sizeof(a));CHECK(n>0&&n<X4_T26_STATE_TEXT_MAX);
    CHECK(x4_t26_state_parse(a,(size_t)n,s->build_id,s->manifest_sha256,&copy)==0);
    CHECK(x4_t26_state_serialize(&copy,b,sizeof(b))==n&&!memcmp(a,b,(size_t)n+1));
}
static void go_to(X4T26State *s,unsigned goal)
{
    while(s->next_cell<goal){
        if(s->next_cell>=X4_T26_BEST_FIRST&&!s->best_ready){
            X4T26Config c;x4_t26_config_base(&c);CHECK(x4_t26_state_set_best(s,&c)==0);
        }
        unsigned i=s->next_cell;
        unsigned mode=i>=X4_T26_REALISTIC_FIRST&&i<X4_T26_V1_FIRST?X4_T26_MODE_PACED:X4_T26_MODE_MAXIMUM;
        CHECK(x4_t26_state_begin(s,i,mode,X4_T26_PHASE_QUERY_DECODER)==0);
        CHECK(x4_t26_state_mark(s,X4_T26_MODE_PACED,X4_T26_PHASE_DELETE)==0);
        CHECK(x4_t26_state_commit_cell(s,i,true)==0);roundtrip(s);
    }
}
static void outcomes(X4T26Plan *p)
{
    x4_t26_plan_init(p);
    for(unsigned i=0;i<X4_T26_FACTOR_CELLS;++i){
        X4T26Outcome o={.completed=true,.accepted=true,.hashes_ok=true,.decode_p95_us=(i&1)?27000:15000};
        CHECK(x4_t26_plan_note(p,i,&o)==0);
    }
}
static void improve(X4T26Plan *p,unsigned factor,uint64_t a,uint64_t b)
{p->factors[factor*2].decode_p95_us=a;p->factors[factor*2+1].decode_p95_us=b;}
static void plan_tests(void)
{
    X4T26Config base;x4_t26_config_base(&base);CHECK(x4_t26_config_valid(&base));
    CHECK(base.api==2&&base.resource_type==1&&base.depth==2&&base.output_memory_type==0&&
        base.affinity_mask==0x3f&&base.priority==700&&!base.pipe_id&&!base.queue_id&&!base.realistic);
    for(unsigned f=0;f<X4_T26_FACTOR_COUNT;++f){
        X4T26Config c;CHECK(x4_t26_factor_config(f,&c)==0&&x4_t26_config_valid(&c));
        CHECK(different(&base,&c)==(f?1u:0u));
        CHECK(x4_t26_factor_family(f)<X4_T26_FAMILY_COUNT);
    }
    X4T26Config c;CHECK(x4_t26_factor_config(13,&c)<0&&x4_t26_factor_family(13)==X4_T26_FAMILY_COUNT);
    CHECK(x4_t26_factor_config(4,&c)==0&&c.priority==256);
    CHECK(x4_t26_factor_config(5,&c)==0&&c.priority==512);
    for(unsigned f=10;f<13;++f){CHECK(x4_t26_factor_config(f,&c)==0);CHECK(c.resource_type==(f==10?0:f-9));}
    X4T26Plan p;x4_t26_plan_init(&p);unsigned modes=0;
    for(unsigned i=0;i<X4_T26_CELL_COUNT;++i){
        const X4T26Cell *v=&p.cells[i];CHECK(v->index==i&&v->clip<X4_T26_CLIP_COUNT&&x4_t26_config_valid(&v->config));
        modes+=x4_t26_cell_mode_count(v);
        if(i<26)CHECK(v->factor==i/2&&v->clip==((i&1)?X4_T26_CLIP_1080_27:X4_T26_CLIP_1080_15));
        if(i>=34&&i<38)CHECK(v->config.realistic&&x4_t26_cell_first_mode(v)==X4_T26_MODE_PACED);
        if(i>=38)CHECK(v->group==X4_T26_GROUP_V1&&v->config.api==1&&!v->config.realistic);
    }
    CHECK(modes==82&&x4_t26_plan_nominal_remaining_us(&p,0)==UINT64_C(1066000000));
    CHECK(p.cells[38].clip==X4_T26_CLIP_1080_15&&p.cells[39].clip==X4_T26_CLIP_1080_8&&
        p.cells[40].clip==X4_T26_CLIP_1080_20&&p.cells[41].clip==X4_T26_CLIP_1080_27&&p.cells[42].clip==X4_T26_CLIP_720_8);
    CHECK(!x4_t26_plan_cell(&p,29)&&!x4_t26_plan_cell(&p,35)&&x4_t26_plan_cell(&p,34));
    CHECK(!x4_t26_plan_cell(&p,43)&&!x4_t26_clip_id(5));
    CHECK(x4_t26_plan_select_best(&p)==-2);
    outcomes(&p);CHECK(x4_t26_plan_select_best(&p)==0&&x4_t26_config_equal(&base,&p.best)&&!p.selected_factors);
    CHECK(x4_t26_plan_cell(&p,29)&&x4_t26_plan_cell(&p,35));
    outcomes(&p);improve(&p,1,14000,26000);improve(&p,2,13000,25000);
    improve(&p,3,12000,24000);p.factors[6].hashes_ok=false;
    improve(&p,4,13000,25000);improve(&p,5,12000,24000);
    improve(&p,7,11000,23000);improve(&p,9,10000,28000);
    improve(&p,10,9000,22000);p.factors[20].errors=1;
    improve(&p,11,10000,22000);improve(&p,12,0,21000);
    CHECK(x4_t26_plan_select_best(&p)==0);
    CHECK(p.best.depth==1&&p.best.output_memory_type==3&&p.best.affinity_mask==0x3f&&
        p.best.priority==512&&p.best.pipe_id==2&&p.best.resource_type==2);
    CHECK(p.selected_factors==((1u<<1)|(1u<<2)|(1u<<5)|(1u<<7)|(1u<<11)));
    X4T26Plan restored;x4_t26_plan_init(&restored);CHECK(x4_t26_plan_restore_best(&restored,&p.best)==0);
    CHECK(restored.selected_factors==p.selected_factors&&x4_t26_config_equal(&restored.best,&p.best));
    for(unsigned i=29;i<34;++i)CHECK(x4_t26_config_equal(&p.cells[i].config,&p.best));
    CHECK(p.cells[35].config.realistic&&p.cells[35].config.priority==512);
    outcomes(&p);improve(&p,4,14000,26000);improve(&p,5,14000,26000);
    CHECK(x4_t26_plan_select_best(&p)==0&&p.best.priority==256);
    outcomes(&p);p.factors[0].hashes_ok=false;CHECK(x4_t26_plan_select_best(&p)==-3&&!p.best_ready);
    outcomes(&p);improve(&p,4,14000,27000);CHECK(x4_t26_plan_select_best(&p)==0&&p.best.priority==700);
}
static void state_tests(void)
{
    X4T26State s=fresh();CHECK(x4_t26_state_valid(&s));roundtrip(&s);
    CHECK(!x4_t26_state_deadline_reached(&s,s.deadline_us-1));
    CHECK(x4_t26_state_deadline_reached(&s,s.deadline_us)&&x4_t26_state_deadline_reached(&s,s.started_us-1));
    CHECK(x4_t26_state_begin(&s,1,0,X4_T26_PHASE_CREATE)<0);
    CHECK(x4_t26_state_begin(&s,0,0,X4_T26_PHASE_QUERY_DECODER)==0);
    CHECK(x4_t26_state_commit_cell(&s,0,true)<0); /* second mode not reached */
    CHECK(x4_t26_state_finish_budget(&s)<0&&x4_t26_state_rebase(&s,0,100)<0);roundtrip(&s);
    CHECK(x4_t26_state_resume(&s,0,false)==X4_T26_RESUME_SKIPPED);
    CHECK(s.next_cell==1&&s.closed_app_mask==1&&s.skipped_mask==1&&!s.completed_mask&&!s.active);roundtrip(&s);
    s=fresh();CHECK(x4_t26_state_begin(&s,0,0,X4_T26_PHASE_CREATE)==0);
    CHECK(x4_t26_state_resume(&s,1,false)==X4_T26_RESUME_COMMITTED);
    CHECK(s.next_cell==1&&s.completed_mask==1&&!s.closed_app_mask&&!s.skipped_mask);roundtrip(&s);
    CHECK(x4_t26_state_resume(&s,UINT64_C(1)<<5,false)<0);
    CHECK(x4_t26_state_rebase(&s,20,0)<0);
    CHECK(x4_t26_state_rebase(&s,20,UINT64_C(10000000))==0);
    CHECK(s.started_us==20&&s.budget_used_us==10000000&&s.deadline_us==UINT64_C(1190000020));
    CHECK(x4_t26_state_rebase(&s,0,0)<0&&x4_t26_state_rebase(&s,0,9999999)<0);roundtrip(&s);
    CHECK(x4_t26_state_rebase(&s,100,X4_T26_BUDGET_US)==0&&s.terminal==X4_T26_TERMINAL_BUDGET&&s.next_cell==43);roundtrip(&s);
    s=fresh();go_to(&s,38);CHECK(s.next_cell==38&&!s.v1_passed_15);
    CHECK(x4_t26_state_begin(&s,38,0,X4_T26_PHASE_QUERY_DECODER)==0);
    CHECK(x4_t26_state_mark(&s,1,X4_T26_PHASE_DECODE)==0);
    CHECK(x4_t26_state_resume(&s,(UINT64_C(1)<<38)-1,false)==X4_T26_RESUME_V1_TERMINATED);
    CHECK(s.terminal==X4_T26_TERMINAL_V1_CRASH&&s.closed_app_mask==(UINT64_C(1)<<38)&&s.next_cell==43);roundtrip(&s);
    s=fresh();go_to(&s,38);CHECK(x4_t26_state_begin(&s,38,0,X4_T26_PHASE_CREATE)==0);
    CHECK(x4_t26_state_commit_cell(&s,38,false)==0&&s.terminal==X4_T26_TERMINAL_V1_FAILED_15);roundtrip(&s);
    s=fresh();go_to(&s,38);CHECK(x4_t26_state_begin(&s,38,0,X4_T26_PHASE_CREATE)==0);
    CHECK(x4_t26_state_resume(&s,(UINT64_C(1)<<39)-1,true)==X4_T26_RESUME_COMMITTED);
    CHECK(s.next_cell==39&&s.v1_passed_15&&!s.terminal&&!s.closed_app_mask);roundtrip(&s);
    go_to(&s,43);CHECK(s.terminal==X4_T26_TERMINAL_COMPLETE&&s.v1_passed_15);roundtrip(&s);
    s=fresh();go_to(&s,38);
    CHECK(x4_t26_state_resume(&s,(UINT64_C(1)<<39)-1,false)==X4_T26_RESUME_COMMITTED);
    CHECK(s.terminal==X4_T26_TERMINAL_V1_FAILED_15&&!s.closed_app_mask);roundtrip(&s);
    s=fresh();go_to(&s,40);CHECK(x4_t26_state_begin(&s,40,0,X4_T26_PHASE_CREATE)==0);
    CHECK(x4_t26_state_resume(&s,(UINT64_C(1)<<40)-1,true)==X4_T26_RESUME_V1_TERMINATED);
    CHECK(s.terminal==X4_T26_TERMINAL_V1_CRASH&&s.v1_passed_15&&s.closed_app_mask==(UINT64_C(1)<<40));roundtrip(&s);
    s=fresh();CHECK(x4_t26_state_finish_budget(&s)==0&&s.skipped_mask==((UINT64_C(1)<<43)-1));roundtrip(&s);
    s=fresh();s.generation=UINT64_MAX;CHECK(x4_t26_state_valid(&s));roundtrip(&s);
    CHECK(x4_t26_state_begin(&s,0,0,X4_T26_PHASE_CREATE)<0);
    CHECK(x4_t26_state_init(&s,"x",manifest,UINT64_MAX-X4_T26_BUDGET_US+1)<0);
    s=fresh();s.completed_mask=1;CHECK(!x4_t26_state_valid(&s));
    s=fresh();s.skipped_mask=UINT64_C(1)<<43;CHECK(!x4_t26_state_valid(&s));
    s=fresh();s.closed_app_mask=1;CHECK(!x4_t26_state_valid(&s));
}
static void reject(const char *text,size_t n,const X4T26State *identity_state)
{
    X4T26State out,before;memset(&out,0x5a,sizeof(out));memcpy(&before,&out,sizeof(out));
    CHECK(x4_t26_state_parse(text,n,identity_state->build_id,identity_state->manifest_sha256,&out)<0);
    CHECK(!memcmp(&before,&out,sizeof(out)));++mutations;
}
static void replace_text(char *out,size_t capacity,const char *source,const char *from,const char *to)
{
    const char *at=strstr(source,from);CHECK(at!=NULL);
    size_t n=(size_t)(at-source),tail=strlen(at+strlen(from)),insert=strlen(to);
    CHECK(n+insert+tail<capacity);memcpy(out,source,n);memcpy(out+n,to,insert);
    memcpy(out+n+insert,at+strlen(from),tail+1);
}
static bool white(unsigned char c){return c==' '||c=='\n'||c=='\r'||c=='\t';}
static void json_tests(const char *fixture)
{
    X4T26State s=fresh(),out;char text[X4_T26_STATE_TEXT_MAX],work[X4_T26_STATE_TEXT_MAX];
    int n=x4_t26_state_serialize(&s,text,sizeof(text));CHECK(n>0);roundtrip(&s);
    for(int i=0;i<n-1;++i)reject(text,(size_t)i,&s);
    for(int i=0;i<n;++i)if(!white((unsigned char)text[i])){
        memcpy(work,text,(size_t)n+1);work[i]^=1;reject(work,(size_t)n,&s);
    }
    for(size_t cap=1;cap<=(size_t)n;cap+=7){
        unsigned char guarded[X4_T26_STATE_TEXT_MAX+2];memset(guarded,0xa5,sizeof(guarded));
        CHECK(x4_t26_state_serialize(&s,(char *)guarded+1,cap)<0);
        CHECK(guarded[0]==0xa5&&guarded[cap+1]==0xa5&&guarded[1]==0);
    }
    CHECK(x4_t26_state_parse(text,(size_t)n,"other",manifest,&out)<0);
    CHECK(x4_t26_state_parse(text,(size_t)n,s.build_id,"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",&out)<0);
    memcpy(work,text,(size_t)n);work[n/2]=0;reject(work,(size_t)n,&s);
    replace_text(work,sizeof(work),text,"\"generation\":1","\"generation\":18446744073709551616");reject(work,strlen(work),&s);
    replace_text(work,sizeof(work),text,"\"generation\":1","\"generation\":01");reject(work,strlen(work),&s);
    replace_text(work,sizeof(work),text,"\"generation\":1","\"generation\":-1");reject(work,strlen(work),&s);
    replace_text(work,sizeof(work),text,"\"generation\":1","\"generation\":1,\"generation\":1");reject(work,strlen(work),&s);
    replace_text(work,sizeof(work),text,"\"generation\":1","\"unknown\":1");reject(work,strlen(work),&s);
    replace_text(work,sizeof(work),text,"0123456789ab-t26","0123456789ab\\nt26");reject(work,strlen(work),&s);
    replace_text(work,sizeof(work),text,"\"active\":false","\"active\":falseX");reject(work,strlen(work),&s);
    CHECK((size_t)n+2<sizeof(work));work[0]=' ';work[1]='\n';memcpy(work+2,text,(size_t)n);
    CHECK(x4_t26_state_parse(work,(size_t)n+2,s.build_id,manifest,&out)==0);
    memcpy(work,text,(size_t)n);work[n]='X';reject(work,(size_t)n+1,&s);
    if(fixture){FILE *f=fopen(fixture,"wb");CHECK(f&&fwrite(text,1,(size_t)n,f)==(size_t)n&&fclose(f)==0);}
}

typedef struct {X4DecoderDepthLedger ledger;int previous;uint64_t source[5];unsigned calls,outputs,reserved,allocations;} FakeNative;
static void native_open(FakeNative *f)
{memset(f,0,sizeof(*f));CHECK(x4_decoder_depth_init(&f->ledger,2)==0);f->previous=-1;f->allocations=13;}
static void native_feed(FakeNative *f,unsigned source)
{
    unsigned slot=5,input=0;
    for(unsigned i=0;i<5;++i)if(!f->ledger.output_held[i]&&!(f->reserved&(1u<<i))){slot=i;break;}
    CHECK(slot<5&&!(f->reserved&(1u<<slot)));
    CHECK(x4_decoder_depth_input_acquire(&f->ledger,&input)==0&&input<4);
    CHECK(x4_decoder_depth_output_offer(&f->ledger,slot)==0);
    f->source[slot]=source;++f->calls;
    x4_decoder_depth_decode_returned(&f->ledger);
    if(f->previous>=0){
        unsigned returned=(unsigned)f->previous;CHECK(returned!=slot);
        CHECK(x4_decoder_depth_output_return(&f->ledger,returned)==0);++f->outputs;
        uint64_t image=f->source[returned];CHECK(image+1==source);
        if(image==181||image==480||image==780)f->reserved|=1u<<returned;
    }
    f->previous=(int)slot;
}
static bool native_delete(FakeNative *f,int rc)
{
    if(rc||f->reserved)return false;
    x4_decoder_depth_delete_complete(&f->ledger);logical_frees+=f->allocations;f->allocations=0;
    return true;
}
static void ownership_tests(void)
{
    /* Conservative depth-two bound: two decoder-held and two app readers
     * leave exactly one offer. A third held slot is transient until return. */
    X4DecoderDepthLedger two;CHECK(x4_decoder_depth_init(&two,2)==0);
    unsigned held[2]={0,1},reserved=(1u<<2)|(1u<<3),in=0;
    for(unsigned n=0;n<2;++n){CHECK(x4_decoder_depth_input_acquire(&two,&in)==0);
        CHECK(x4_decoder_depth_output_offer(&two,held[n])==0);x4_decoder_depth_decode_returned(&two);}
    for(unsigned n=0;n<30;++n){
        unsigned offer=5;for(unsigned i=0;i<5;++i)if(!two.output_held[i]&&!(reserved&(1u<<i))){offer=i;break;}
        CHECK(offer<5&&x4_decoder_depth_input_acquire(&two,&in)==0);
        CHECK(x4_decoder_depth_output_offer(&two,offer)==0);x4_decoder_depth_decode_returned(&two);
        CHECK(x4_decoder_depth_output_return(&two,held[0])==0);held[0]=held[1];held[1]=offer;
        CHECK(x4_decoder_depth_outputs_held(&two)==2&&!(reserved&((1u<<held[0])|(1u<<held[1]))));
    }
    CHECK(x4_decoder_depth_output_return(&two,held[0])==0&&x4_decoder_depth_output_return(&two,held[1])==0);
    x4_decoder_depth_flush_complete(&two);x4_decoder_depth_delete_complete(&two);
    CHECK(!x4_decoder_depth_outputs_held(&two)&&!x4_decoder_depth_inputs_live(&two));
    FakeNative f;native_open(&f);
    for(unsigned n=1;n<=781;++n)native_feed(&f,n);
    CHECK(f.calls==781&&f.outputs==780&&x4_decoder_depth_outputs_held(&f.ledger)==1);
    unsigned bits=0;for(unsigned i=0;i<5;++i)if(f.reserved&(1u<<i)){
        CHECK(f.source[i]==181||f.source[i]==480||f.source[i]==780);++bits;
    }
    CHECK(bits==3&&!native_delete(&f,0)); /* hash readers still own slots */
    f.reserved=0; /* hashes after window, before Flush */
    CHECK(x4_decoder_depth_output_return(&f.ledger,(unsigned)f.previous)==0);
    x4_decoder_depth_flush_complete(&f.ledger);
    CHECK(!x4_decoder_depth_outputs_held(&f.ledger)&&x4_decoder_depth_inputs_live(&f.ledger)>0);
    CHECK(!native_delete(&f,-1)&&f.allocations==13&&x4_decoder_depth_inputs_live(&f.ledger)>0);
    CHECK(native_delete(&f,0)&&!f.allocations&&!x4_decoder_depth_inputs_live(&f.ledger));
    for(unsigned cell=0;cell<43;++cell){
        native_open(&f);for(unsigned n=1;n<=17;++n)native_feed(&f,n);
        CHECK(x4_decoder_depth_outputs_held(&f.ledger)==1);
        CHECK(!native_delete(&f,-1)&&f.allocations==13);
        CHECK(native_delete(&f,0)&&!f.allocations&&!x4_decoder_depth_outputs_held(&f.ledger));
    }
    CHECK(logical_frees==44*13);
}
int main(int argc,char **argv)
{
    CHECK(argc==1||argc==2);plan_tests();state_tests();json_tests(argc==2?argv[1]:NULL);ownership_tests();
    printf("{\"passed\":true,\"checks\":%u,\"json_rejections\":%u,\"logical_native_frees\":%u,\"cells\":43,\"modes\":82,\"native_execution\":false}\n",checks,mutations,logical_frees);
    return 0;
}
