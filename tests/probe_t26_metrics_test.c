/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_t26_metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static unsigned checks,rejections;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static X4T26Samples *samples(void)
{X4T26Samples *s=calloc(1,sizeof(*s));CHECK(s!=NULL);return s;}
static X4T26Matcher *matcher(unsigned first,unsigned last)
{X4T26Matcher *m=calloc(1,sizeof(*m));CHECK(m&&x4_t26_matcher_reset(m,true,first,last)==0);return m;}
static void invalid_json(X4T26Stats s)
{
    FILE *f=tmpfile();CHECK(f&&ftell(f)==0);
    CHECK(x4_t26_stats_json(f,&s)<0&&ftell(f)==0&&!ferror(f));CHECK(fclose(f)==0);++rejections;
}
static void samples_tests(X4T26Stats *rank,X4T26Stats *huge)
{
    X4T26Samples *s=samples();X4T26Stats r;
    CHECK(x4_t26_samples_add(NULL,1)<0&&x4_t26_samples_stats(NULL,&r)<0);
    CHECK(x4_t26_samples_stats(s,NULL)<0);
    CHECK(x4_t26_samples_stats(s,&r)==0&&!r.n&&r.mean_us==0);
    FILE *f=tmpfile();char null_text[5]={0};CHECK(f&&x4_t26_stats_json(f,&r)==0);
    rewind(f);CHECK(fread(null_text,1,4,f)==4&&!strcmp(null_text,"null")&&fclose(f)==0);
    for(unsigned i=1024;i>0;--i)CHECK(x4_t26_samples_add(s,i)==0);
    CHECK(x4_t26_samples_add(s,1025)<0&&s->count==1024);
    CHECK(x4_t26_samples_stats(s,&r)==0&&r.n==1024&&r.p50_us==512&&r.p95_us==973&&
        r.p99_us==1014&&r.max_us==1024&&r.mean_us==512.5);
    CHECK(s->values[0]==1024&&s->values[1023]==1);*rank=r;
    x4_t26_samples_reset(s);CHECK(!s->count);
    for(unsigned i=0;i<1024;++i)CHECK(x4_t26_samples_add(s,UINT64_MAX)==0);
    CHECK(x4_t26_samples_stats(s,&r)==0&&r.n==1024&&r.max_us==UINT64_MAX&&
        r.p50_us==UINT64_MAX&&r.p95_us==UINT64_MAX&&r.p99_us==UINT64_MAX&&
        isfinite(r.mean_us)&&r.mean_us==(double)UINT64_MAX);*huge=r;
    x4_t26_samples_reset(s);CHECK(x4_t26_samples_add(s,0)==0&&x4_t26_samples_add(s,UINT64_MAX)==0);
    CHECK(x4_t26_samples_stats(s,&r)==0&&r.n==2&&!r.p50_us&&r.p95_us==UINT64_MAX&&
        r.p99_us==UINT64_MAX&&r.mean_us==(double)UINT64_MAX/2.0);
    x4_t26_samples_reset(s);for(unsigned i=10;i>0;--i)CHECK(x4_t26_samples_add(s,i)==0);
    CHECK(x4_t26_samples_stats(s,&r)==0&&r.p50_us==5&&r.p95_us==10&&r.p99_us==10&&r.mean_us==5.5);
    s->values[1000]=UINT64_MAX;CHECK(x4_t26_samples_stats(s,&r)==0&&r.mean_us==5.5);
    X4T26Stats before=r;s->count=1025;CHECK(x4_t26_samples_stats(s,&r)<0&&!memcmp(&r,&before,sizeof(r)));
    CHECK(x4_t26_samples_add(s,1)<0);x4_t26_samples_reset(s);
    CHECK(x4_t26_samples_add(s,0)==0&&x4_t26_samples_stats(s,&r)==0&&r.n==1&&r.mean_us==0&&!r.max_us);
    CHECK(x4_t26_stats_json(NULL,&r)<0);
    X4T26Stats bad=*rank;bad.mean_us=NAN;invalid_json(bad);
    bad=*rank;bad.mean_us=INFINITY;invalid_json(bad);
    bad=*rank;bad.mean_us=-INFINITY;invalid_json(bad);
    bad=*rank;bad.mean_us=-1;invalid_json(bad);
    bad=*rank;bad.mean_us=(double)bad.max_us+1;invalid_json(bad);
    bad=*rank;bad.n=1025;invalid_json(bad);
    bad=*rank;bad.p50_us=bad.p95_us+1;invalid_json(bad);
    bad=*rank;bad.p95_us=bad.p99_us+1;invalid_json(bad);
    bad=*rank;bad.p99_us=bad.max_us+1;invalid_json(bad);
    bad=(X4T26Stats){.mean_us=1};invalid_json(bad);
    bad=(X4T26Stats){.mean_us=NAN};invalid_json(bad);
    free(s);
}
static X4T26CallRecord data_call(unsigned n,unsigned delay)
{
    uint64_t begin=UINT64_C(1000000)+(uint64_t)(n-1)*16667;
    return (X4T26CallRecord){.call_index=n,.au_ordinal=n,.au_begin_us=begin,
        .output_ready_us=begin+7000,.has_au=true,.output_valid=n>delay,.picture_count=n>delay?1u:0u};
}
static void delayed(unsigned delay,bool lookahead)
{
    X4T26Matcher *m=matcher(181,780);X4T26Association a;unsigned n=lookahead?780+delay:780;
    CHECK(delay<=4);
    uint64_t last_end=0;
    for(unsigned i=1;i<=n;++i){
        X4T26CallRecord c=data_call(i,delay);last_end=c.output_ready_us;
        CHECK(x4_t26_matcher_call(m,&c,&a)==0&&a.has_output==c.output_valid);
        if(c.output_valid){CHECK(a.inferred&&a.au_ordinal==i-delay&&a.submitted_call==i-delay&&
            a.returned_call==i&&a.call_delay==delay&&!a.from_flush&&a.latency_us==(uint64_t)delay*16667+7000);}
        else CHECK(!a.inferred);
    }
    CHECK(m->no_output_calls==delay&&m->pending==delay&&m->decode_calls==n);
    for(unsigned i=0;i<delay;++i){
        X4T26CallRecord c={.call_index=n,.flushing=true,.output_valid=true,.picture_count=1,
            .output_ready_us=last_end+(uint64_t)(i+1)*1000};
        CHECK(x4_t26_matcher_call(m,&c,&a)==0&&a.inferred&&a.from_flush&&!a.returned_call&&!a.call_delay);
        CHECK(a.au_ordinal==n-delay+i+1);
    }
    X4T26CallRecord done={.call_index=n,.flushing=true,.output_ready_us=last_end+10000};
    CHECK(x4_t26_matcher_call(m,&done,&a)==0&&!a.has_output&&!a.inferred);
    CHECK(x4_t26_matcher_finish(m,n,n)&&m->inference_valid&&m->measured_outputs==600);
    CHECK(m->au_latency.count==600&&m->output_interval.count==599&&m->drained_outputs==delay);
    uint64_t total=0;for(unsigned b=0;b<4;++b)total+=m->call_delay_hist[b];
    CHECK(total+m->measured_flush_outputs==600);
    if(lookahead||!delay){
        CHECK(m->measured_flush_outputs==0&&m->call_delay_hist[delay<3?delay:3]==600);
        X4T26Stats r;CHECK(x4_t26_samples_stats(&m->au_latency,&r)==0&&r.p50_us==(uint64_t)delay*16667+7000);
        CHECK(x4_t26_samples_stats(&m->output_interval,&r)==0&&r.n==599&&r.p50_us==16667&&r.max_us==16667);
    }else CHECK(m->measured_flush_outputs==delay&&m->call_delay_hist[delay<3?delay:3]==600-delay);
    free(m);
}
static void matcher_rejections(void)
{
    X4T26Matcher *m=matcher(1,2);X4T26Association a;X4T26CallRecord c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==0&&a.inferred&&m->au_latency.count==1);
    c=data_call(2,0);c.native_rc=-1234;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&!a.inferred&&!m->inference_valid&&
        (m->invalid_reasons&X4_T26_MATCH_NATIVE_ERROR)&&!m->au_latency.count&&!m->output_interval.count);
    CHECK(c.native_rc==-1234&&!c.is_error_frame&&c.picture_count==1&&m->outputs==2);
    CHECK(!x4_t26_matcher_finish(m,2,2));
    CHECK(x4_t26_matcher_reset(m,true,1,2)==0);c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==0);x4_t26_matcher_invalidate(m,X4_T26_MATCH_LOCAL_CONTRACT);
    CHECK(m->invalid_reasons==X4_T26_MATCH_LOCAL_CONTRACT&&m->decode_calls==1&&m->outputs==1&&!m->au_latency.count);
    c=data_call(2,0);CHECK(x4_t26_matcher_call(m,&c,&a)==1&&c.native_rc==0&&!c.is_error_frame&&
        !(m->invalid_reasons&X4_T26_MATCH_NATIVE_ERROR));
    CHECK(x4_t26_matcher_reset(m,false,1,1)==0);c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&m->invalid_reasons==X4_T26_MATCH_NOT_VERIFIED&&!a.inferred);
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);c.is_error_frame=true;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_NATIVE_ERROR));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);c.picture_count=2;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_COUNT));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,1);c.picture_count=1;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_COUNT));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);c.au_ordinal=2;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_ORDER));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);c.call_index=0;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_ORDER));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);c.has_au=false;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_ORDER));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);c.output_ready_us=c.au_begin_us-1;
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_CLOCK));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);
    c=(X4T26CallRecord){.flushing=true,.output_valid=true,.picture_count=1,.output_ready_us=100};
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_COUNT));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,1);
    CHECK(x4_t26_matcher_call(m,&c,&a)==0&&!x4_t26_matcher_finish(m,1,1)&&
        (m->invalid_reasons&X4_T26_MATCH_COUNT)&&!m->au_latency.count);
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==0&&!x4_t26_matcher_finish(m,2,1)&&!m->au_latency.count);
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);m->pending=1025;c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_CAPACITY));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);m->head=1024;c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&(m->invalid_reasons&X4_T26_MATCH_CAPACITY));
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);m->decode_calls=UINT64_MAX;c=data_call(1,0);
    CHECK(x4_t26_matcher_call(m,&c,&a)==1&&m->decode_calls==UINT64_MAX);
    CHECK(x4_t26_matcher_reset(m,true,1,1)==0);
    c=(X4T26CallRecord){.call_index=1,.au_ordinal=1,.has_au=true,.output_valid=true,.picture_count=1};
    CHECK(x4_t26_matcher_call(m,&c,&a)==0&&a.inferred&&!a.latency_us&&x4_t26_matcher_finish(m,1,1));
    X4T26Stats r;CHECK(x4_t26_samples_stats(&m->au_latency,&r)==0&&r.n==1&&!r.p50_us&&r.mean_us==0);
    CHECK(x4_t26_matcher_reset(m,true,0,1)<0&&x4_t26_matcher_reset(m,true,2,1)<0&&
        x4_t26_matcher_reset(m,true,1,1025)<0);
    CHECK(x4_t26_matcher_call(NULL,&c,&a)<0&&x4_t26_matcher_call(m,NULL,&a)<0&&
        x4_t26_matcher_call(m,&c,NULL)<0);
    free(m);
}
int main(int argc,char **argv)
{
    CHECK(argc==1||argc==2);X4T26Stats rank,huge;samples_tests(&rank,&huge);
    delayed(0,true);delayed(1,true);delayed(2,true);delayed(4,true);delayed(1,false);matcher_rejections();
    if(argc==2){
        FILE *f=fopen(argv[1],"wb");CHECK(f!=NULL);X4T26Stats empty={0};
        CHECK(fputs("{\"empty\":",f)>=0&&x4_t26_stats_json(f,&empty)==0&&fputs(",\"rank\":",f)>=0&&
            x4_t26_stats_json(f,&rank)==0&&fputs(",\"huge\":",f)>=0&&x4_t26_stats_json(f,&huge)==0&&fputs("}\n",f)>=0&&fclose(f)==0);
    }
    printf("{\"passed\":true,\"checks\":%u,\"invalid_stats_rejected\":%u,\"delayed_runs\":5,\"measured_outputs_per_run\":600,\"native_execution\":false}\n",checks,rejections);
    return 0;
}
