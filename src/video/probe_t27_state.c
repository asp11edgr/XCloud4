/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t27_state.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#define T27_ALL ((UINT64_C(1)<<X4_T27_CELL_COUNT)-1u)
static uint64_t t27_prefix(unsigned n){return n?((UINT64_C(1)<<n)-1u):0u;}
static bool t27_identity(const char *s,bool hash)
{
 if(!s)return false;for(unsigned n=0;n<=64u;++n){unsigned c=(unsigned char)s[n];
  if(!c)return hash?n==64u:n>0u;
  if(hash){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
  else if(!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='-'||c=='_'||c=='.'))return false;
 }return false;
}
static unsigned t27_cell_mode(unsigned cell){return cell<X4_T27_BASE_COUNT?X4_T26_MODE_MAXIMUM:X4_T26_MODE_PACED;}
bool x4_t27_state_valid(const X4T27State *s)
{
 if(!s||!t27_identity(s->build_id,false)||!t27_identity(s->manifest_sha256,true)||!s->generation||
  s->next_cell>X4_T27_CELL_COUNT||s->active_cell>X4_T27_CELL_COUNT||s->active_mode>=X4_T26_MODE_COUNT||
  s->phase>=X4_T27_PHASE_COUNT||s->terminal>X4_T27_TERMINAL_INTERRUPTED)return false;
 uint64_t remaining=s->budget_used_us<X4_T27_BUDGET_US?X4_T27_BUDGET_US-s->budget_used_us:0u;
 if(s->started_us>UINT64_MAX-remaining||s->deadline_us!=s->started_us+remaining)return false;
 uint64_t all=s->completed_mask|s->skipped_mask;
 if((all|s->closed_app_mask)&~T27_ALL||(s->completed_mask&s->skipped_mask)||
  (s->closed_app_mask&~s->skipped_mask)||all!=t27_prefix(s->next_cell))return false;
 if(s->active){if(s->terminal||s->next_cell>=X4_T27_CELL_COUNT||s->active_cell!=s->next_cell||
  s->active_mode!=t27_cell_mode(s->active_cell)||!s->phase)return false;}
 else if(s->phase||s->active_cell!=X4_T27_CELL_COUNT)return false;
 return s->terminal?(!s->active&&s->next_cell==X4_T27_CELL_COUNT):s->next_cell<X4_T27_CELL_COUNT;
}
static void t27_idle(X4T27State *s){s->active=false;s->active_cell=X4_T27_CELL_COUNT;s->active_mode=0;s->phase=0;}
static int t27_publish(X4T27State *out,X4T27State *next)
{if(next->generation==UINT64_MAX)return -1;++next->generation;if(!x4_t27_state_valid(next))return -1;*out=*next;return 0;}
int x4_t27_state_init(X4T27State *s,const char *build,const char *manifest,uint64_t now)
{
 if(!s||!t27_identity(build,false)||!t27_identity(manifest,true)||now>UINT64_MAX-X4_T27_BUDGET_US)return -1;
 X4T27State n={.generation=1,.started_us=now,.deadline_us=now+X4_T27_BUDGET_US,.active_cell=X4_T27_CELL_COUNT};
 memcpy(n.build_id,build,strlen(build)+1);memcpy(n.manifest_sha256,manifest,65);*s=n;return 0;
}
int x4_t27_state_rebase(X4T27State *s,uint64_t now,uint64_t used)
{
 if(!x4_t27_state_valid(s)||s->active||used<s->budget_used_us)return -1;
 uint64_t remaining=used<X4_T27_BUDGET_US?X4_T27_BUDGET_US-used:0u;if(now>UINT64_MAX-remaining)return -1;
 X4T27State n=*s;n.started_us=now;n.deadline_us=now+remaining;n.budget_used_us=used;
 if(!remaining&&!n.terminal){n.skipped_mask|=T27_ALL&~t27_prefix(n.next_cell);n.next_cell=X4_T27_CELL_COUNT;n.terminal=X4_T27_TERMINAL_STOPPED;}
 return t27_publish(s,&n);
}
int x4_t27_state_begin(X4T27State *s,unsigned cell,unsigned mode,unsigned phase)
{
 if(!x4_t27_state_valid(s)||s->terminal||s->active||cell!=s->next_cell||cell>=X4_T27_CELL_COUNT||
  mode!=t27_cell_mode(cell)||!phase||phase>=X4_T27_PHASE_COUNT)return -1;
 X4T27State n=*s;n.active=true;n.active_cell=cell;n.active_mode=mode;n.phase=phase;return t27_publish(s,&n);
}
int x4_t27_state_mark(X4T27State *s,unsigned mode,unsigned phase)
{
 if(!x4_t27_state_valid(s)||!s->active||mode!=s->active_mode||!phase||phase>=X4_T27_PHASE_COUNT)return -1;
 X4T27State n=*s;n.phase=phase;return t27_publish(s,&n);
}
int x4_t27_state_commit_cell(X4T27State *s,unsigned cell,bool passed)
{
 (void)passed;if(!x4_t27_state_valid(s)||!s->active||cell!=s->active_cell)return -1;
 X4T27State n=*s;n.completed_mask|=UINT64_C(1)<<cell;n.next_cell=cell+1u;t27_idle(&n);
 if(n.next_cell==X4_T27_CELL_COUNT)n.terminal=X4_T27_TERMINAL_COMPLETE;return t27_publish(s,&n);
}
int x4_t27_state_finish_budget(X4T27State *s)
{
 if(!x4_t27_state_valid(s)||s->active||s->terminal)return -1;X4T27State n=*s;
 n.skipped_mask|=T27_ALL&~t27_prefix(n.next_cell);n.next_cell=X4_T27_CELL_COUNT;n.terminal=X4_T27_TERMINAL_STOPPED;return t27_publish(s,&n);
}
int x4_t27_state_resume(X4T27State *s,uint64_t durable)
{
 if(!x4_t27_state_valid(s)||(durable&~T27_ALL)||(durable&s->skipped_mask))return -1;
 uint64_t available=t27_prefix(s->active?s->active_cell+1u:s->next_cell);
 if(durable&~available||(s->completed_mask&~durable))return -1;
 X4T27State n=*s;n.completed_mask|=durable;
 if(n.active){unsigned cell=n.active_cell;uint64_t bit=UINT64_C(1)<<cell;
  if(durable&bit){n.next_cell=cell+1u;t27_idle(&n);if(n.next_cell==X4_T27_CELL_COUNT)n.terminal=X4_T27_TERMINAL_COMPLETE;}
  else{n.closed_app_mask|=bit;n.skipped_mask|=T27_ALL&~t27_prefix(cell);n.next_cell=X4_T27_CELL_COUNT;n.terminal=X4_T27_TERMINAL_INTERRUPTED;t27_idle(&n);}
 }
 return t27_publish(s,&n);
}
static uint64_t t27_checksum(const X4T27State *s)
{
 uint64_t h=UINT64_C(14695981039346656037);const char *ids[]={s->build_id,s->manifest_sha256};
 for(unsigned i=0;i<2u;++i)for(const unsigned char *p=(const unsigned char *)ids[i];;p++){h=(h^*p)*UINT64_C(1099511628211);if(!*p)break;}
 const uint64_t words[]={s->generation,s->started_us,s->deadline_us,s->budget_used_us,s->completed_mask,s->skipped_mask,s->closed_app_mask,
  s->next_cell,s->active_cell,s->active_mode,s->phase,s->terminal,s->active};
 for(unsigned i=0;i<sizeof(words)/sizeof(*words);++i){uint64_t n=words[i];for(unsigned j=0;j<8u;++j){h=(h^(unsigned char)n)*UINT64_C(1099511628211);n>>=8;}}
 return h;
}
int x4_t27_state_serialize(const X4T27State *s,char *out,size_t capacity)
{
 if(!out||!capacity)return -1;out[0]=0;if(!x4_t27_state_valid(s)||capacity>X4_T27_STATE_TEXT_MAX)return -1;
 int n=snprintf(out,capacity,"{\"schema\":\"X4T27_STATE1\",\"build\":\"%s\",\"manifest\":\"%s\","
  "\"generation\":%" PRIu64 ",\"started_us\":%" PRIu64 ",\"deadline_us\":%" PRIu64 ",\"budget_used_us\":%" PRIu64 ","
  "\"completed\":%" PRIu64 ",\"skipped\":%" PRIu64 ",\"closed\":%" PRIu64 ","
  "\"next\":%u,\"cell\":%u,\"mode\":%u,\"phase\":%u,\"terminal\":%u,\"active\":%s,\"checksum\":\"%016" PRIx64 "\"}\n",
  s->build_id,s->manifest_sha256,s->generation,s->started_us,s->deadline_us,s->budget_used_us,s->completed_mask,s->skipped_mask,s->closed_app_mask,
  s->next_cell,s->active_cell,s->active_mode,s->phase,s->terminal,s->active?"true":"false",t27_checksum(s));
 if(n<0||(size_t)n>=capacity){out[0]=0;return -1;}return n;
}
typedef struct {const char *p,*end;} T27Reader;
static void t27_whitespace(T27Reader *r)
{ while(r->p<r->end&&(*r->p==' '||*r->p=='\t'||*r->p=='\r'||*r->p=='\n'))++r->p; }
static bool t27_token(T27Reader *r,char c)
{ t27_whitespace(r);if(r->p==r->end||*r->p!=c)return false;++r->p;return true; }
static bool t27_string(T27Reader *r,char *out,size_t capacity)
{
    if(!t27_token(r,'"'))return false;size_t n=0;
    while(r->p<r->end&&*r->p!='"'){
        unsigned char c=(unsigned char)*r->p++;
        if(c<32||c>126||c=='\\'||n+1>=capacity)return false;out[n++]=(char)c;
    }
    if(r->p==r->end)return false;++r->p;out[n]=0;return true;
}
static bool t27_key(T27Reader *r,const char *name,bool first)
{char got[32];return t27_token(r,first?'{':',')&&t27_string(r,got,sizeof(got))&&!strcmp(got,name)&&t27_token(r,':');}
static bool t27_integer(T27Reader *r,uint64_t *out)
{
    t27_whitespace(r);if(r->p==r->end||*r->p<'0'||*r->p>'9')return false;
    bool zero=*r->p=='0';uint64_t n=0;unsigned digits=0;
    while(r->p<r->end&&*r->p>='0'&&*r->p<='9'){
        unsigned d=(unsigned)(*r->p++-'0');if(n>(UINT64_MAX-d)/10)return false;n=n*10+d;++digits;
    }
    if(zero&&digits!=1)return false;*out=n;return true;
}
static bool t27_boolean(T27Reader *r,bool *out)
{
    t27_whitespace(r);if((size_t)(r->end-r->p)>=4&&!memcmp(r->p,"true",4)){r->p+=4;*out=true;return true;}
    if((size_t)(r->end-r->p)>=5&&!memcmp(r->p,"false",5)){r->p+=5;*out=false;return true;}return false;
}
static bool t27_hex_checksum(T27Reader *r,uint64_t *out)
{
    char text[17];if(!t27_string(r,text,sizeof(text))||strlen(text)!=16)return false;uint64_t n=0;
    for(unsigned i=0;i<16;++i){unsigned c=(unsigned char)text[i],v;
        if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else return false;n=(n<<4)|v;}
    *out=n;return true;
}
int x4_t27_state_parse(const char *data,size_t bytes,const char *build,const char *manifest,X4T27State *out)
{
 if(!data||!out||!bytes||bytes>=X4_T27_STATE_TEXT_MAX||memchr(data,0,bytes)||!t27_identity(build,false)||!t27_identity(manifest,true))return -1;
 T27Reader r={data,data+bytes};X4T27State s={0};char schema[32];uint64_t n,stored;
 if(!t27_key(&r,"schema",true)||!t27_string(&r,schema,sizeof(schema))||strcmp(schema,"X4T27_STATE1")||
  !t27_key(&r,"build",false)||!t27_string(&r,s.build_id,sizeof(s.build_id))||strcmp(s.build_id,build)||
  !t27_key(&r,"manifest",false)||!t27_string(&r,s.manifest_sha256,sizeof(s.manifest_sha256))||strcmp(s.manifest_sha256,manifest))return -1;
#define U64(name,field) do{if(!t27_key(&r,name,false)||!t27_integer(&r,&s.field))return -1;}while(0)
#define UNS(name,field,max) do{if(!t27_key(&r,name,false)||!t27_integer(&r,&n)||n>(max))return -1;s.field=(unsigned)n;}while(0)
 U64("generation",generation);U64("started_us",started_us);U64("deadline_us",deadline_us);U64("budget_used_us",budget_used_us);
 U64("completed",completed_mask);U64("skipped",skipped_mask);U64("closed",closed_app_mask);
 UNS("next",next_cell,X4_T27_CELL_COUNT);UNS("cell",active_cell,X4_T27_CELL_COUNT);
 UNS("mode",active_mode,X4_T26_MODE_COUNT-1u);UNS("phase",phase,X4_T27_PHASE_COUNT-1u);UNS("terminal",terminal,X4_T27_TERMINAL_INTERRUPTED);
#undef U64
#undef UNS
 if(!t27_key(&r,"active",false)||!t27_boolean(&r,&s.active)||!t27_key(&r,"checksum",false)||!t27_hex_checksum(&r,&stored)||!t27_token(&r,'}'))return -1;
 t27_whitespace(&r);if(r.p!=r.end||!x4_t27_state_valid(&s)||stored!=t27_checksum(&s))return -1;*out=s;return 0;
}
#endif
