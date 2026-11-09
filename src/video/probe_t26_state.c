/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_state.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ALL_CELLS ((UINT64_C(1)<<X4_T26_CELL_COUNT)-1)
static uint64_t prefix(unsigned n){return n?((UINT64_C(1)<<n)-1):0;}
static bool identity(const char *s,bool manifest)
{
    if(!s)return false;
    for(unsigned n=0;n<=X4_T26_STATE_ID_MAX;++n){
        unsigned char c=(unsigned char)s[n];
        if(!c)return manifest?n==64:n>0;
        if(manifest){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
        else if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))return false;
    }
    return false;
}
static bool mode_valid(unsigned cell,unsigned mode)
{
    return mode<X4_T26_MODE_COUNT&&(!(cell>=X4_T26_REALISTIC_FIRST&&cell<X4_T26_V1_FIRST)||mode==X4_T26_MODE_PACED);
}
static bool phase_valid(unsigned cell,unsigned phase)
{
    if(!phase||phase>=X4_T26_PHASE_COUNT)return false;
    if(phase==X4_T26_PHASE_ARBITRATION)return cell>=X4_T26_V1_FIRST;
    return cell<X4_T26_V1_FIRST||(phase!=X4_T26_PHASE_QUERY_COMPUTE&&phase!=X4_T26_PHASE_ALLOCATE_QUEUE);
}
bool x4_t26_state_valid(const X4T26State *s)
{
    uint64_t remaining=s&&s->budget_used_us<X4_T26_BUDGET_US?X4_T26_BUDGET_US-s->budget_used_us:0;
    if(!s||!identity(s->build_id,false)||!identity(s->manifest_sha256,true)||!s->generation||
        s->started_us>UINT64_MAX-remaining||s->deadline_us!=s->started_us+remaining||
        s->next_cell>X4_T26_CELL_COUNT||s->active_cell>X4_T26_CELL_COUNT||
        s->active_mode>=X4_T26_MODE_COUNT||s->phase>=X4_T26_PHASE_COUNT||
        s->terminal>X4_T26_TERMINAL_V1_FAILED_15||!x4_t26_config_valid(&s->best)||
        s->best.api!=X4_T26_API_V2||s->best.realistic)return false;
    uint64_t covered=s->completed_mask|s->skipped_mask;
    if((covered|s->closed_app_mask)&~ALL_CELLS)return false;
    if((s->completed_mask&s->skipped_mask)||(s->closed_app_mask&~s->skipped_mask)||
        covered!=prefix(s->next_cell))return false;
    if(s->active){
        if(s->terminal||s->next_cell>=X4_T26_CELL_COUNT||s->active_cell!=s->next_cell||
            !mode_valid(s->active_cell,s->active_mode)||!phase_valid(s->active_cell,s->phase))return false;
        if(s->active_cell>=X4_T26_BEST_FIRST&&s->active_cell<X4_T26_V1_FIRST&&!s->best_ready)return false;
    }else if(s->phase||s->active_cell!=X4_T26_CELL_COUNT)return false;
    if(s->v1_passed_15&&!(s->completed_mask&(UINT64_C(1)<<X4_T26_V1_FIRST)))return false;
    if(!s->terminal){
        if(s->next_cell==X4_T26_CELL_COUNT||
            (s->next_cell>X4_T26_V1_FIRST&&!s->v1_passed_15))return false;
    }else{
        if(s->active||s->next_cell!=X4_T26_CELL_COUNT)return false;
        if(s->terminal==X4_T26_TERMINAL_V1_CRASH&&
            !(s->closed_app_mask&(ALL_CELLS&~prefix(X4_T26_V1_FIRST))))return false;
        if(s->terminal==X4_T26_TERMINAL_V1_FAILED_15&&
            (s->v1_passed_15||!(s->completed_mask&(UINT64_C(1)<<X4_T26_V1_FIRST))||
             (s->skipped_mask&(ALL_CELLS&~prefix(X4_T26_V1_FIRST+1)))!=(ALL_CELLS&~prefix(X4_T26_V1_FIRST+1))))return false;
    }
    return true;
}
static int replace(X4T26State *out,X4T26State *candidate)
{
    if(!x4_t26_state_valid(candidate))return -1;
    *out=*candidate;return 0;
}
static bool increment(X4T26State *s)
{ if(s->generation==UINT64_MAX)return false;++s->generation;return true; }
static void idle(X4T26State *s)
{ s->active=false;s->active_cell=X4_T26_CELL_COUNT;s->active_mode=X4_T26_MODE_MAXIMUM;s->phase=X4_T26_PHASE_NONE; }
int x4_t26_state_init(X4T26State *s,const char *build,const char *manifest,uint64_t now)
{
    if(!s||!identity(build,false)||!identity(manifest,true)||now>UINT64_MAX-X4_T26_BUDGET_US)return -1;
    X4T26State next={.generation=1,.started_us=now,.deadline_us=now+X4_T26_BUDGET_US,
        .active_cell=X4_T26_CELL_COUNT};
    memcpy(next.build_id,build,strlen(build)+1);memcpy(next.manifest_sha256,manifest,65);
    x4_t26_config_base(&next.best);return replace(s,&next);
}
bool x4_t26_state_deadline_reached(const X4T26State *s,uint64_t now)
{ return !x4_t26_state_valid(s)||now<s->started_us||now>=s->deadline_us; }
int x4_t26_state_rebase(X4T26State *s,uint64_t now,uint64_t used)
{
    if(!x4_t26_state_valid(s)||s->active||used<s->budget_used_us||
        (!used&&(s->completed_mask||s->skipped_mask)))return -1;
    uint64_t remaining=used<X4_T26_BUDGET_US?X4_T26_BUDGET_US-used:0;
    if(now>UINT64_MAX-remaining)return -1;
    X4T26State next=*s;if(!increment(&next))return -1;
    next.started_us=now;next.deadline_us=now+remaining;next.budget_used_us=used;
    if(!remaining&&!next.terminal){
        next.skipped_mask|=ALL_CELLS&~prefix(next.next_cell);next.next_cell=X4_T26_CELL_COUNT;
        next.terminal=X4_T26_TERMINAL_BUDGET;
    }
    return replace(s,&next);
}
int x4_t26_state_begin(X4T26State *s,unsigned cell,unsigned mode,unsigned phase)
{
    if(!x4_t26_state_valid(s)||s->terminal||s->active||cell!=s->next_cell||
        cell>=X4_T26_CELL_COUNT||!mode_valid(cell,mode)||!phase_valid(cell,phase)||
        mode!=((cell>=X4_T26_REALISTIC_FIRST&&cell<X4_T26_V1_FIRST)?X4_T26_MODE_PACED:X4_T26_MODE_MAXIMUM))return -1;
    X4T26State next=*s;if(!increment(&next))return -1;
    next.active=true;next.active_cell=cell;next.active_mode=mode;next.phase=phase;return replace(s,&next);
}
int x4_t26_state_mark(X4T26State *s,unsigned mode,unsigned phase)
{
    if(!x4_t26_state_valid(s)||!s->active||!mode_valid(s->active_cell,mode)||
        mode<s->active_mode||!phase_valid(s->active_cell,phase))return -1;
    X4T26State next=*s;if(!increment(&next))return -1;
    next.active_mode=mode;next.phase=phase;return replace(s,&next);
}
int x4_t26_state_set_best(X4T26State *s,const X4T26Config *best)
{
    if(!x4_t26_state_valid(s)||s->active||s->terminal||!x4_t26_config_valid(best)||
        best->api!=X4_T26_API_V2||best->realistic)return -1;
    X4T26State next=*s;if(!increment(&next))return -1;
    next.best=*best;next.best_ready=true;return replace(s,&next);
}
int x4_t26_state_commit_cell(X4T26State *s,unsigned cell,bool passed)
{
    if(!x4_t26_state_valid(s)||!s->active||cell!=s->active_cell||
        (passed&&s->active_mode!=X4_T26_MODE_PACED))return -1;
    X4T26State next=*s;if(!increment(&next))return -1;
    next.completed_mask|=UINT64_C(1)<<cell;next.next_cell=cell+1;idle(&next);
    if(cell==X4_T26_V1_FIRST){
        next.v1_passed_15=passed;
        if(!passed){next.skipped_mask|=ALL_CELLS&~prefix(cell+1);next.next_cell=X4_T26_CELL_COUNT;
            next.terminal=X4_T26_TERMINAL_V1_FAILED_15;}
    }
    if(next.next_cell==X4_T26_CELL_COUNT&&!next.terminal)next.terminal=X4_T26_TERMINAL_COMPLETE;
    return replace(s,&next);
}
int x4_t26_state_finish_budget(X4T26State *s)
{
    if(!x4_t26_state_valid(s)||s->active||s->terminal)return -1;
    X4T26State next=*s;if(!increment(&next))return -1;
    next.skipped_mask|=ALL_CELLS&~prefix(next.next_cell);next.next_cell=X4_T26_CELL_COUNT;
    next.terminal=X4_T26_TERMINAL_BUDGET;return replace(s,&next);
}
int x4_t26_state_resume(X4T26State *s,uint64_t durable,bool v1_passed)
{
    if(!x4_t26_state_valid(s)||(durable&~ALL_CELLS)||(durable&s->skipped_mask))return -1;
    uint64_t available=s->active?prefix(s->active_cell+1):prefix(s->next_cell+1);
    if(durable&~available)return -1;
    X4T26State next=*s;next.completed_mask|=durable;
    if(v1_passed){
        if(!(next.completed_mask&(UINT64_C(1)<<X4_T26_V1_FIRST)))return -1;
        next.v1_passed_15=true;
    }
    if(s->terminal)return x4_t26_state_valid(&next)?X4_T26_RESUME_IDLE:-1;
    int result=next.completed_mask!=s->completed_mask?X4_T26_RESUME_COMMITTED:X4_T26_RESUME_IDLE;
    if(next.active){
        unsigned cell=next.active_cell;uint64_t bit=UINT64_C(1)<<cell;
        if(next.completed_mask&bit){
            result=X4_T26_RESUME_COMMITTED;next.next_cell=cell+1;idle(&next);
            if(cell==X4_T26_V1_FIRST&&!next.v1_passed_15){
                next.skipped_mask|=ALL_CELLS&~prefix(cell+1);next.next_cell=X4_T26_CELL_COUNT;
                next.terminal=X4_T26_TERMINAL_V1_FAILED_15;
            }
        }else{
            next.closed_app_mask|=bit;next.skipped_mask|=bit;next.next_cell=cell+1;idle(&next);
            result=X4_T26_RESUME_SKIPPED;
            if(cell>=X4_T26_V1_FIRST){
                next.skipped_mask|=ALL_CELLS&~prefix(cell+1);next.next_cell=X4_T26_CELL_COUNT;
                next.terminal=X4_T26_TERMINAL_V1_CRASH;result=X4_T26_RESUME_V1_TERMINATED;
            }
        }
    }else{
        while(next.next_cell<X4_T26_CELL_COUNT&&
            ((next.completed_mask|next.skipped_mask)&(UINT64_C(1)<<next.next_cell)))++next.next_cell;
    }
    if(!next.terminal&&next.next_cell>X4_T26_V1_FIRST&&!next.v1_passed_15&&
        (next.completed_mask&(UINT64_C(1)<<X4_T26_V1_FIRST))){
        next.skipped_mask|=ALL_CELLS&~prefix(X4_T26_V1_FIRST+1);next.next_cell=X4_T26_CELL_COUNT;
        next.terminal=X4_T26_TERMINAL_V1_FAILED_15;
    }
    if(next.next_cell==X4_T26_CELL_COUNT&&!next.terminal)next.terminal=X4_T26_TERMINAL_COMPLETE;
    if(!increment(&next)||replace(s,&next))return -1;
    return result;
}

static uint64_t hash_byte(uint64_t h,unsigned char c)
{ return (h^c)*UINT64_C(1099511628211); }
static uint64_t hash_u64(uint64_t h,uint64_t n)
{ for(unsigned i=0;i<8;++i){h=hash_byte(h,(unsigned char)n);n>>=8;}return h; }
static uint64_t checksum(const X4T26State *s)
{
    uint64_t h=UINT64_C(14695981039346656037);
    for(const unsigned char *p=(const unsigned char *)s->build_id;;++p){h=hash_byte(h,*p);if(!*p)break;}
    for(const unsigned char *p=(const unsigned char *)s->manifest_sha256;;++p){h=hash_byte(h,*p);if(!*p)break;}
    const uint64_t values[]={s->generation,s->started_us,s->deadline_us,s->budget_used_us,s->completed_mask,s->skipped_mask,s->closed_app_mask,
        s->next_cell,s->active_cell,s->active_mode,s->phase,s->terminal,s->active,s->v1_passed_15,s->best_ready,
        s->best.api,s->best.resource_type,s->best.depth,(uint64_t)s->best.output_memory_type,s->best.affinity_mask,
        (uint64_t)s->best.priority,s->best.pipe_id,s->best.queue_id,s->best.realistic};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i)h=hash_u64(h,values[i]);
    return h;
}
int x4_t26_state_serialize(const X4T26State *s,char *out,size_t capacity)
{
    if(!out||!capacity)return -1;out[0]=0;
    if(!x4_t26_state_valid(s)||capacity>X4_T26_STATE_TEXT_MAX)return -1;
    int n=snprintf(out,capacity,"{\"schema\":\"X4T26_STATE1\",\"build\":\"%s\",\"manifest\":\"%s\","
        "\"generation\":%" PRIu64 ",\"started_us\":%" PRIu64 ",\"deadline_us\":%" PRIu64 ",\"budget_used_us\":%" PRIu64 ","
        "\"completed\":%" PRIu64 ",\"skipped\":%" PRIu64 ",\"closed\":%" PRIu64 ","
        "\"next\":%u,\"cell\":%u,\"mode\":%u,\"phase\":%u,\"terminal\":%u,"
        "\"active\":%s,\"v1_passed\":%s,\"best_ready\":%s,\"best_api\":%u,\"best_resource\":%u,\"best_depth\":%u,"
        "\"best_memory\":%d,\"best_affinity\":%" PRIu64 ",\"best_priority\":%d,\"best_pipe\":%u,\"best_queue\":%u,"
        "\"best_realistic\":%s,\"checksum\":\"%016" PRIx64 "\"}\n",
        s->build_id,s->manifest_sha256,s->generation,s->started_us,s->deadline_us,s->budget_used_us,
        s->completed_mask,s->skipped_mask,s->closed_app_mask,s->next_cell,s->active_cell,s->active_mode,s->phase,s->terminal,
        s->active?"true":"false",s->v1_passed_15?"true":"false",s->best_ready?"true":"false",s->best.api,
        s->best.resource_type,s->best.depth,s->best.output_memory_type,s->best.affinity_mask,s->best.priority,
        s->best.pipe_id,s->best.queue_id,s->best.realistic?"true":"false",checksum(s));
    if(n<0||(size_t)n>=capacity){out[0]=0;return -1;}return n;
}

typedef struct {const char *p,*end;} Reader;
static void whitespace(Reader *r)
{ while(r->p<r->end&&(*r->p==' '||*r->p=='\t'||*r->p=='\r'||*r->p=='\n'))++r->p; }
static bool token(Reader *r,char c)
{ whitespace(r);if(r->p==r->end||*r->p!=c)return false;++r->p;return true; }
static bool string(Reader *r,char *out,size_t capacity)
{
    if(!token(r,'"'))return false;size_t n=0;
    while(r->p<r->end&&*r->p!='"'){
        unsigned char c=(unsigned char)*r->p++;
        if(c<32||c>126||c=='\\'||n+1>=capacity)return false;out[n++]=(char)c;
    }
    if(r->p==r->end)return false;++r->p;out[n]=0;return true;
}
static bool key(Reader *r,const char *name,bool first)
{char got[32];return token(r,first?'{':',')&&string(r,got,sizeof(got))&&!strcmp(got,name)&&token(r,':');}
static bool integer(Reader *r,uint64_t *out)
{
    whitespace(r);if(r->p==r->end||*r->p<'0'||*r->p>'9')return false;
    bool zero=*r->p=='0';uint64_t n=0;unsigned digits=0;
    while(r->p<r->end&&*r->p>='0'&&*r->p<='9'){
        unsigned d=(unsigned)(*r->p++-'0');if(n>(UINT64_MAX-d)/10)return false;n=n*10+d;++digits;
    }
    if(zero&&digits!=1)return false;*out=n;return true;
}
static bool boolean(Reader *r,bool *out)
{
    whitespace(r);if((size_t)(r->end-r->p)>=4&&!memcmp(r->p,"true",4)){r->p+=4;*out=true;return true;}
    if((size_t)(r->end-r->p)>=5&&!memcmp(r->p,"false",5)){r->p+=5;*out=false;return true;}return false;
}
static bool hex_checksum(Reader *r,uint64_t *out)
{
    char text[17];if(!string(r,text,sizeof(text))||strlen(text)!=16)return false;uint64_t n=0;
    for(unsigned i=0;i<16;++i){unsigned c=(unsigned char)text[i],v;
        if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else return false;n=(n<<4)|v;}
    *out=n;return true;
}
int x4_t26_state_parse(const char *data,size_t bytes,const char *build,const char *manifest,X4T26State *out)
{
    if(!data||!out||!bytes||bytes>=X4_T26_STATE_TEXT_MAX||memchr(data,0,bytes)||
        !identity(build,false)||!identity(manifest,true))return -1;
    Reader r={data,data+bytes};X4T26State s={0};char schema[32];uint64_t n,stored;
    if(!key(&r,"schema",true)||!string(&r,schema,sizeof(schema))||strcmp(schema,"X4T26_STATE1")||
        !key(&r,"build",false)||!string(&r,s.build_id,sizeof(s.build_id))||strcmp(s.build_id,build)||
        !key(&r,"manifest",false)||!string(&r,s.manifest_sha256,sizeof(s.manifest_sha256))||strcmp(s.manifest_sha256,manifest))return -1;
#define U64(name,field) do{if(!key(&r,name,false)||!integer(&r,&s.field))return -1;}while(0)
#define UNS(name,field,maximum) do{if(!key(&r,name,false)||!integer(&r,&n)||n>(maximum))return -1;s.field=(unsigned)n;}while(0)
#define BOOL(name,field) do{if(!key(&r,name,false)||!boolean(&r,&s.field))return -1;}while(0)
    U64("generation",generation);U64("started_us",started_us);U64("deadline_us",deadline_us);U64("budget_used_us",budget_used_us);
    U64("completed",completed_mask);U64("skipped",skipped_mask);U64("closed",closed_app_mask);
    UNS("next",next_cell,X4_T26_CELL_COUNT);UNS("cell",active_cell,X4_T26_CELL_COUNT);
    UNS("mode",active_mode,X4_T26_MODE_COUNT-1);UNS("phase",phase,X4_T26_PHASE_COUNT-1);
    UNS("terminal",terminal,X4_T26_TERMINAL_V1_FAILED_15);BOOL("active",active);
    BOOL("v1_passed",v1_passed_15);BOOL("best_ready",best_ready);
    UNS("best_api",best.api,UINT_MAX);UNS("best_resource",best.resource_type,UINT_MAX);UNS("best_depth",best.depth,UINT_MAX);
    if(!key(&r,"best_memory",false)||!integer(&r,&n)||n>INT_MAX)return -1;s.best.output_memory_type=(int)n;
    U64("best_affinity",best.affinity_mask);
    if(!key(&r,"best_priority",false)||!integer(&r,&n)||n>INT_MAX)return -1;s.best.priority=(int)n;
    UNS("best_pipe",best.pipe_id,UINT_MAX);UNS("best_queue",best.queue_id,UINT_MAX);BOOL("best_realistic",best.realistic);
#undef U64
#undef UNS
#undef BOOL
    if(!key(&r,"checksum",false)||!hex_checksum(&r,&stored)||!token(&r,'}'))return -1;
    whitespace(&r);if(r.p!=r.end||!x4_t26_state_valid(&s)||stored!=checksum(&s))return -1;
    *out=s;return 0;
}
#endif
