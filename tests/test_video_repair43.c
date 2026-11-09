/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/streaming/video_repair43.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_PACKETS=2400, PACKETS_PER_AU=20 };
typedef struct {uint8_t data[2048];size_t bytes;} Packet;
typedef struct {unsigned count,methods[3];uint8_t last[2048];size_t last_bytes;
    bool seen[TEST_PACKETS+1];uint64_t delivered[TEST_PACKETS+1];} Capture;
static uint16_t get16(const uint8_t *p){return (uint16_t)((unsigned)p[0]*256u+p[1]);}
static void set16(uint8_t *p,unsigned n){p[0]=(uint8_t)(n/256u);p[1]=(uint8_t)n;}
static void set32(uint8_t *p,uint32_t n){for(unsigned j=0;j<4;++j)p[j]=(uint8_t)(n>>(24u-8u*j));}
static X4VideoRepair43Config config(unsigned mode)
{return (X4VideoRepair43Config){.mode=mode,.seed=43,.media_ssrc=100,.rtx_ssrc=200,.fec_ssrc=300,.media_pt=102,.rtx_pt=103,.fec_pt=49,.rtx_accepted=mode>=1,.fec_accepted=mode==2};}
static void media(Packet *p,uint16_t sequence,unsigned payload)
{
    assert(payload&&payload<1900);memset(p,0,sizeof(*p));p->bytes=12u+payload;
    p->data[0]=128;p->data[1]=128u|102u;set16(p->data+2,sequence);set32(p->data+4,(uint32_t)sequence*3000u);set32(p->data+8,100);
    for(unsigned i=0;i<payload;++i)p->data[12u+i]=(uint8_t)(sequence*17u+i*3u);
}
static void rtx(Packet *out,const Packet *in,uint16_t sequence,bool padding)
{
    memset(out,0,sizeof(*out));out->bytes=in->bytes+2u+(padding?4u:0u);memcpy(out->data,in->data,12);
    out->data[0]=(uint8_t)(128u|(padding?32u:0u));out->data[1]=128u|103u;set16(out->data+2,sequence);set32(out->data+8,200);
    memcpy(out->data+12,in->data+2,2);memcpy(out->data+14,in->data+12,in->bytes-12u);
    if(padding)out->data[out->bytes-1u]=4;
}
static void emit(void *opaque,const uint8_t *packet,size_t bytes,unsigned method,uint64_t now)
{
    Capture *c=opaque;assert(packet&&bytes>=12&&bytes<=2048&&method<3&&(packet[0]>>6)==2);
    assert(packet[8]==0&&packet[9]==0&&packet[10]==0&&packet[11]==100&&(packet[1]&127u)==102);
    ++c->count;++c->methods[method];memcpy(c->last,packet,bytes);c->last_bytes=bytes;
    unsigned seq=get16(packet+2);if(seq<=TEST_PACKETS&&!c->seen[seq]){c->seen[seq]=true;c->delivered[seq]=now;}
}
static void feed(X4VideoRepair43 *r,const Packet *p,uint64_t now,Capture *c)
{assert(!x4_video_repair43_process(r,p->data,p->bytes,now,emit,c));}

/* Original test encoder. It lays out three termination positions and XORs
 * complete RTP bodies; no production decoder helper is called here. */
static void parity(Packet *out,const Packet *packets,unsigned count,unsigned mask_bits,uint16_t fec_seq)
{
    assert(count>0&&count<=109&&(mask_bits==15||mask_bits==46||mask_bits==109));
    size_t header=mask_bits==15?20u:mask_bits==46?24u:32u,body=0;
    for(unsigned i=0;i<count;++i)if(packets[i].bytes-12u>body)body=packets[i].bytes-12u;
    assert(12u+header+body<=2048);memset(out,0,sizeof(*out));out->bytes=12u+header+body;
    out->data[0]=128;out->data[1]=49;set16(out->data+2,fec_seq);set32(out->data+8,300);
    uint8_t *p=out->data+12;p[8]=1;set32(p+12,100);set16(p+16,get16(packets[0].data+2));
    unsigned stop=mask_bits==15?0u:mask_bits==46?16u:48u;p[18u+stop/8u]|=(uint8_t)(128u>>(stop%8u));
    for(unsigned i=0;i<count;++i){
        unsigned offset=(uint16_t)(get16(packets[i].data+2)-get16(packets[0].data+2));assert(offset<mask_bits);
        unsigned wire_bit=offset+1u+(offset>=15u?1u:0u)+(offset>=46u?1u:0u);
        p[18u+wire_bit/8u]|=(uint8_t)(128u>>(wire_bit%8u));
        p[0]^=packets[i].data[0]&63u;p[1]^=packets[i].data[1];
        size_t n=packets[i].bytes-12u;p[2]^=(uint8_t)(n>>8);p[3]^=(uint8_t)n;
        for(unsigned j=4;j<8;++j)p[j]^=packets[i].data[j];
        for(size_t j=0;j<n;++j)p[header+j]^=packets[i].data[12+j];
    }
}
static void rtx_cases(void)
{
    X4VideoRepair43Config cfg=config(1);X4VideoRepair43 *r=x4_video_repair43_create(&cfg);assert(r);
    Capture c={0};Packet a,b,repair;
    media(&a,65534,20);feed(r,&a,1000,&c);media(&b,0,21);feed(r,&b,2000,&c);
    media(&a,65535,17);rtx(&repair,&a,900,true);feed(r,&repair,4000,&c);
    assert(c.methods[1]==1&&c.last_bytes==a.bytes&&!memcmp(c.last,a.data,a.bytes));
    feed(r,&repair,4100,&c);feed(r,&a,4200,&c);assert(c.count==3);
    X4VideoRepair43Snapshot *s=calloc(1,sizeof(*s));assert(s);x4_video_repair43_snapshot(r,s);
    assert(s->gaps_detected==1&&s->gaps_closed[1]==1&&s->duplicate[1]==1&&s->duplicate[0]==1);
    assert(s->closure_p50_finite[1]&&s->closure_p50_us[1]==3000&&s->closure_max_us[1]==2000);
    repair.bytes=14;repair.data[0]=128;feed(r,&repair,5000,&c);
    repair.bytes=12;feed(r,&repair,5100,&c);
    set32(repair.data+8,201);repair.bytes=14;feed(r,&repair,5200,&c);
    x4_video_repair43_snapshot(r,s);assert(s->rtx_empty==2&&s->foreign==1&&c.count==3);
    free(s);x4_video_repair43_free(r);
    puts("PASS RTX OSN, padding/probe, duplicate, normal-after-RTX, foreign SSRC and sequence wrap");
}
static void fec_case(unsigned mask_bits,bool two_missing)
{
    Packet *p=calloc(mask_bits,sizeof(*p)),*f=calloc(1,sizeof(*f));Capture *c=calloc(1,sizeof(*c));assert(p&&f&&c);
    for(unsigned i=0;i<mask_bits;++i)media(&p[i],(uint16_t)(65500u+i),16u+i%17u);
    parity(f,p,mask_bits,mask_bits,222);
    X4VideoRepair43Config cfg=config(2);X4VideoRepair43 *r=x4_video_repair43_create(&cfg);assert(r);
    unsigned missing=mask_bits/2u;
    for(unsigned i=0;i<mask_bits;++i)if(i!=missing&&(!two_missing||i!=missing+1u))feed(r,&p[i],1000u+i*100u,c);
    unsigned before=c->count;feed(r,f,30000,c);
    if(two_missing){assert(c->count==before&&c->methods[2]==0);feed(r,&p[missing+1u],31000,c);}
    assert(c->methods[2]==1&&c->last_bytes==p[missing].bytes&&!memcmp(c->last,p[missing].data,p[missing].bytes));
    before=c->count;feed(r,&p[missing],32000,c);assert(c->count==before);
    X4VideoRepair43Snapshot *s=calloc(1,sizeof(*s));assert(s);x4_video_repair43_snapshot(r,s);
    assert(s->header_count==1&&s->header_bytes[0]==(mask_bits==15?20u:mask_bits==46?24u:32u));
    assert(s->gaps_closed[2]==1&&s->duplicate[0]>=1&&(!two_missing||s->fec_multiple_missing==1));
    assert(s->allocated_bytes<2u*1024u*1024u);
    printf("PASS FEC mask=%u one-recovery exact bytes, two-loss=%u, native memory=%zu\n",mask_bits,two_missing,s->allocated_bytes);
    free(s);x4_video_repair43_free(r);free(c);free(f);free(p);
}
static void malformed_and_expiry(void)
{
    X4VideoRepair43Config cfg=config(2);X4VideoRepair43 *r=x4_video_repair43_create(&cfg);assert(r);
    Packet p[3],f,bad;for(unsigned i=0;i<3;++i)media(&p[i],(uint16_t)(10u+i),20u+i);
    parity(&f,p,3,15,200);Capture c={0};feed(r,&p[0],1000,&c);feed(r,&p[2],2000,&c);
    for(size_t n=0;n<f.bytes;++n){bad=f;bad.bytes=n;feed(r,&bad,3000,&c);assert(c.methods[2]==0);}
    const unsigned offsets[]={12,20,21,24};
    for(unsigned i=0;i<4;++i){bad=f;bad.data[offsets[i]]^=128u;feed(r,&bad,4000,&c);assert(c.methods[2]==0);}
    uint32_t rng=12345;
    for(unsigned i=0;i<10000;++i){bad=f;for(unsigned j=0;j<24;++j){rng=rng*1664525u+1013904223u;bad.data[12u+j]=(uint8_t)(rng>>24);}
        bad.data[12]|=128u;bad.bytes=12u+(rng%70u);feed(r,&bad,5000,&c);assert(c.methods[2]==0);}
    x4_video_repair43_tick(r,300001);feed(r,&f,310000,&c);feed(r,&p[1],320000,&c);assert(c.methods[2]==0&&c.count==2);
    X4VideoRepair43Snapshot *s=calloc(1,sizeof(*s));assert(s);x4_video_repair43_snapshot(r,s);
    assert(s->gaps_expired==1&&s->late[0]==1&&s->format_errors[2]>4);
    free(s);x4_video_repair43_free(r);
    cfg.mutable_extensions=true;r=x4_video_repair43_create(&cfg);assert(r);memset(&c,0,sizeof(c));feed(r,&p[0],1000,&c);feed(r,&p[2],2000,&c);feed(r,&f,3000,&c);
    s=calloc(1,sizeof(*s));assert(s);x4_video_repair43_snapshot(r,s);assert(s->unsupported>=1&&c.methods[2]==0);free(s);x4_video_repair43_free(r);
    puts("PASS truncation, malformed FEC, bounded fuzz, expiry and unsupported mutable extensions");
}
static void csrc_and_padding(void)
{
    Packet p[3],f;Capture c={0};
    for(unsigned i=0;i<3;++i){media(&p[i],(uint16_t)(100u+i),17u+i);
        unsigned csrc=i==1u?2u:1u;size_t old=p[i].bytes;
        memmove(p[i].data+12u+4u*csrc,p[i].data+12u,old-12u);
        for(unsigned j=0;j<csrc;++j)set32(p[i].data+12u+4u*j,900u+j);
        p[i].data[0]|=(uint8_t)csrc;p[i].bytes+=4u*csrc;
        if(i==1u){p[i].data[0]|=32u;memset(p[i].data+p[i].bytes,0,4);p[i].bytes+=4;p[i].data[p[i].bytes-1u]=4;}}
    parity(&f,p,3,15,200);X4VideoRepair43Config cfg=config(2);X4VideoRepair43 *r=x4_video_repair43_create(&cfg);assert(r);
    feed(r,&p[0],1000,&c);feed(r,&p[2],2000,&c);feed(r,&f,3000,&c);
    assert(c.methods[2]==1&&c.last_bytes==p[1].bytes&&!memcmp(c.last,p[1].data,p[1].bytes));
    x4_video_repair43_free(r);puts("PASS FEC XOR starts at fixed RTP byte12, including CSRC/padding and variable lengths");
}
typedef struct {unsigned sequence,attempt;uint64_t at;} Retry;
static void simulation(unsigned mode)
{
    static unsigned first_reference_losses;static uint32_t first_reference_hash;
    X4VideoRepair43Config cfg=config(mode);cfg.loss_percent=3;cfg.seed=UINT32_C(0x143d7);
    X4VideoRepair43 *r=x4_video_repair43_create(&cfg);assert(r);
    Capture *c=calloc(1,sizeof(*c));Packet *p=calloc(TEST_PACKETS+1,sizeof(*p));Retry *q=calloc(TEST_PACKETS*3u,sizeof(*q));assert(c&&p&&q);
    for(unsigned i=0;i<=TEST_PACKETS;++i)media(&p[i],(uint16_t)i,25u+i%15u);
    unsigned original=0,head=0,tail=0,first_losses=0;uint32_t loss_mask_hash=2166136261u;
    uint16_t rtx_sequence=30000,fec_sequence=10000;Packet repair,f;
    while(original<=TEST_PACKETS||head<tail){
        for(unsigned i=head+1u;i<tail;++i)if(q[i].at<q[head].at){Retry swap=q[head];q[head]=q[i];q[i]=swap;}
        uint64_t next=original<=TEST_PACKETS?1000u+(uint64_t)original*1000000u/1200u:UINT64_MAX;
        if(head<tail&&q[head].at<=next){Retry item=q[head++];
            if(mode){rtx(&repair,&p[item.sequence],rtx_sequence++,false);feed(r,&repair,item.at,c);}
            else feed(r,&p[item.sequence],item.at,c);
            if(!c->seen[item.sequence]&&item.attempt<2){assert(tail<TEST_PACKETS*3u);q[tail++]=(Retry){item.sequence,item.attempt+1u,item.at+40000u};}
        }else{
            feed(r,&p[original],next,c);
            if(original<TEST_PACKETS){bool lost=!c->seen[original];first_losses+=lost?1u:0u;loss_mask_hash=(loss_mask_hash^(lost?1u:0u))*16777619u;
                if(lost){assert(tail<TEST_PACKETS*3u);q[tail++]=(Retry){original,1,next+40000u};}}
            if(mode==2&&original<TEST_PACKETS&&original%10u==9u){parity(&f,p+original-9u,10,15,fec_sequence++);feed(r,&f,next+1u,c);}
            ++original;
        }
    }
    unsigned incomplete=0;
    for(unsigned au=0;au<TEST_PACKETS/PACKETS_PER_AU;++au){bool complete=true;
        uint64_t deadline=1000u+(uint64_t)(au*PACKETS_PER_AU+PACKETS_PER_AU-1u)*1000000u/1200u+45000u;
        for(unsigned j=0;j<PACKETS_PER_AU;++j){unsigned i=au*PACKETS_PER_AU+j;if(!c->seen[i]||c->delivered[i]>deadline)complete=false;}
        if(!complete)++incomplete;
    }
    x4_video_repair43_tick(r,3000000u);X4VideoRepair43Snapshot *s=calloc(1,sizeof(*s));assert(s);x4_video_repair43_snapshot(r,s);
    assert(s->lost_simulated[0]>40&&s->lost_simulated[0]<110);
    if(!mode){first_reference_losses=first_losses;first_reference_hash=loss_mask_hash;}
    else assert(first_reference_losses==first_losses&&first_reference_hash==loss_mask_hash);
    printf("SIM43 mode=%u seed=%u loss=3 gaps=%llu closed_normal=%llu closed_rtx=%llu closed_fec=%llu expired=%llu incomplete_AU=%u total_AU=%u close_normal_p50_us=%llu close_normal_p95_us=%llu close_rtx_p50_us=%llu close_rtx_p95_us=%llu close_fec_p50_us=%llu close_fec_p95_us=%llu injected_media=%llu injected_rtx=%llu injected_fec=%llu first_media_losses=%u first_media_loss_mask_hash=%u nack_RTT_us=40000 grace_us=45000 source_pps=1200\n",
        mode,cfg.seed,(unsigned long long)s->gaps_detected,(unsigned long long)s->gaps_closed[0],(unsigned long long)s->gaps_closed[1],(unsigned long long)s->gaps_closed[2],(unsigned long long)s->gaps_expired,incomplete,TEST_PACKETS/PACKETS_PER_AU,
        (unsigned long long)s->closure_p50_us[0],(unsigned long long)s->closure_p95_us[0],(unsigned long long)s->closure_p50_us[1],(unsigned long long)s->closure_p95_us[1],(unsigned long long)s->closure_p50_us[2],(unsigned long long)s->closure_p95_us[2],
        (unsigned long long)s->lost_simulated[0],(unsigned long long)s->lost_simulated[1],(unsigned long long)s->lost_simulated[2],first_losses,loss_mask_hash);
    free(s);free(q);free(p);free(c);x4_video_repair43_free(r);
}
typedef struct {X4VideoRepair43 *r;atomic_bool done;} Observer;
static void ignore(void *context,const uint8_t *p,size_t n,unsigned method,uint64_t now)
{(void)context;(void)p;(void)n;(void)method;(void)now;}
static void *publish(void *opaque)
{
    Observer *o=opaque;Packet p,f;
    for(unsigned i=0;i<10000;++i){media(&p,(uint16_t)i,16);assert(!x4_video_repair43_process(o->r,p.data,p.bytes,1000u+i,ignore,NULL));
        if(i%1000u==0u){parity(&f,&p,1,15,(uint16_t)(20000u+i));assert(!x4_video_repair43_process(o->r,f.data,f.bytes,1000u+i,ignore,NULL));}}
    atomic_store_explicit(&o->done,true,memory_order_release);return NULL;
}
static void observer(void)
{
    X4VideoRepair43Config cfg=config(2);Observer o={.r=x4_video_repair43_create(&cfg)};assert(o.r);atomic_init(&o.done,false);
    pthread_t worker;assert(!pthread_create(&worker,NULL,publish,&o));X4VideoRepair43Snapshot *s=calloc(1,sizeof(*s));assert(s);uint64_t seen=0;
    while(!atomic_load_explicit(&o.done,memory_order_acquire)){x4_video_repair43_snapshot(o.r,s);assert(s->packets[0]>=seen&&s->header_count<=3);seen=s->packets[0];}
    assert(!pthread_join(worker,NULL));x4_video_repair43_snapshot(o.r,s);assert(s->packets[0]==10000&&s->emitted[0]==10000&&s->header_count==3);
    free(s);x4_video_repair43_free(o.r);puts("PASS concurrent observer and owner quiescence before free");
}
int main(void)
{
    rtx_cases();fec_case(15,false);fec_case(46,false);fec_case(109,false);fec_case(15,true);
    malformed_and_expiry();csrc_and_padding();for(unsigned mode=0;mode<3;++mode)simulation(mode);observer();puts("VIDEO_REPAIR43_PASS");return 0;
}
