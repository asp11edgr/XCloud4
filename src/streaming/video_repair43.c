/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent implementation from RFC4588 and the documented flexfec-03 wire
 * format. No libwebrtc implementation code is included or adapted here. */
#include "recovery43_bridge.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { LEDGER=2048, AGE_US=250000, MAX_RECOVERIES=8 };
typedef struct { uint64_t extended,at;size_t bytes;bool valid;
    uint8_t data[X4_REPAIR43_PACKET_CAP]; } Media;
typedef struct { uint64_t extended,at;unsigned state; } Seen;
#if !X4_PUBLIC
enum { LOSS_SLOTS=2048 };
typedef struct {uint32_t key,occurrence;bool valid;} LossSlot;
#endif
typedef struct { bool valid,multiple_counted;uint64_t at;size_t bytes,header;
    uint16_t base;unsigned count;uint16_t offsets[109];
    uint8_t data[X4_REPAIR43_PACKET_CAP]; } Fec;
typedef struct {
    atomic_uint_fast64_t packets[3],emitted[3],errors[3];
#if !X4_PUBLIC
    atomic_uint_fast64_t dropped[3];
#endif
    atomic_uint_fast64_t detected,closed[3],expired,duplicate[3],late[3];
    atomic_uint_fast64_t empty,foreign,unsupported,no_missing,multiple,fec_expired;
    atomic_uint_fast64_t bins[3][X4_REPAIR43_CLOSE_BINS],samples[3],maximum[3];
} Stats;
struct X4VideoRepair43 {
    X4VideoRepair43Config config;Stats stats;
    Media media[X4_REPAIR43_MEDIA_SLOTS];Fec fec[X4_REPAIR43_FEC_SLOTS];Seen seen[LEDGER];
    uint64_t high,last_now,last_expiry;unsigned fec_next;bool have_high;
#if !X4_PUBLIC
    LossSlot loss[3][LOSS_SLOTS];
    bool have_loss_high[3];uint16_t loss_high[3];uint32_t loss_miss_nonce[3];
    atomic_uint header_count;unsigned header_bytes[3];uint8_t headers[3][32];
#endif
};
typedef struct {size_t header,body_end;uint16_t sequence;uint32_t ssrc;uint8_t pt;} Rtp;
static uint16_t be16(const uint8_t *p){return (uint16_t)((unsigned)p[0]<<8|p[1]);}
static uint32_t be32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put16(uint8_t *p,uint16_t n){p[0]=(uint8_t)(n>>8);p[1]=(uint8_t)n;}
static void put32(uint8_t *p,uint32_t n){p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
static void inc(atomic_uint_fast64_t *p){atomic_fetch_add_explicit(p,1,memory_order_relaxed);}
static uint64_t load(const atomic_uint_fast64_t *p){return atomic_load_explicit(p,memory_order_relaxed);}
static bool parse_rtp(const uint8_t *p,size_t n,Rtp *v)
{
    if(!p||!v||n<12||(p[0]>>6)!=2)return false;
    size_t h=12u+4u*(p[0]&15u);if(h>n)return false;
    if(p[0]&16u){if(n-h<4)return false;size_t words=be16(p+h+2);if(words>(n-h-4)/4)return false;h+=4+words*4;}
    size_t end=n;if(p[0]&32u){unsigned padding=p[n-1];if(!padding||padding>n-h)return false;end-=padding;}
    *v=(Rtp){h,end,be16(p+2),be32(p+8),(uint8_t)(p[1]&127u)};return true;
}
#if !X4_PUBLIC
static uint32_t mix(uint32_t x)
{x^=x>>16;x*=UINT32_C(0x7feb352d);x^=x>>15;x*=UINT32_C(0x846ca68b);return x^(x>>16);}
static bool simulated_loss(X4VideoRepair43 *r,const Rtp *p,const uint8_t *wire,unsigned method)
{
    if(!r->config.loss_percent)return false;
    /* Separate bounded occurrence ledgers keep the first media losses equal
     * across cases, while an identical normal retransmission gets a fresh
     * sample instead of being lost forever with a purely stateless hash. */
    uint32_t key=r->config.seed^mix(p->ssrc)^((uint32_t)p->sequence<<16)^be32(wire+4)^p->pt;
    bool newer=!r->have_loss_high[method];
    if(!newer){unsigned delta=(uint16_t)(p->sequence-r->loss_high[method]);newer=delta>0&&delta<32768u;}
    if(newer){r->have_loss_high[method]=true;r->loss_high[method]=p->sequence;}
    LossSlot *slot=&r->loss[method][p->sequence%LOSS_SLOTS];
    if(!slot->valid||slot->key!=key){
        /* An old retransmission whose occurrence slot was evicted must not
         * restart forever at its initial loss sample. */
        uint32_t occurrence=newer?0u:++r->loss_miss_nonce[method];
        *slot=(LossSlot){key,occurrence,true};
    }
    uint32_t occurrence=slot->occurrence++;
    return mix(key^mix(occurrence))%100u<r->config.loss_percent;
}
#endif
static bool extended(const X4VideoRepair43 *r,uint16_t sequence,uint64_t *out)
{
    if(!r->have_high){*out=sequence;return true;}
    unsigned distance=(uint16_t)(sequence-(uint16_t)r->high);
    if(distance==32768u)return false;
    int delta=distance<32768u?(int)distance:(int)distance-65536;
    if(delta<0&&(uint64_t)(-delta)>r->high)return false;
    if(delta>0&&r->high>UINT64_MAX-(unsigned)delta)return false;
    *out=delta<0?r->high-(unsigned)(-delta):r->high+(unsigned)delta;return true;
}
static void expire_seen(X4VideoRepair43 *r,Seen *s)
{if(s->state==2){s->state=3;inc(&r->stats.expired);}}
void x4_video_repair43_tick(X4VideoRepair43 *r,uint64_t now)
{
    if(!r)return;if(now<r->last_now)now=r->last_now;r->last_now=now;
    if(r->last_expiry&&now-r->last_expiry<10000u)return;r->last_expiry=now;
    for(unsigned i=0;i<LEDGER;++i)if(r->seen[i].state==2&&now-r->seen[i].at>AGE_US)expire_seen(r,&r->seen[i]);
    for(unsigned i=0;i<X4_REPAIR43_FEC_SLOTS;++i)if(r->fec[i].valid&&now-r->fec[i].at>AGE_US){r->fec[i].valid=false;inc(&r->stats.fec_expired);}
}
static void new_gap(X4VideoRepair43 *r,uint64_t seq,uint64_t now)
{
    Seen *s=&r->seen[seq%LEDGER];if(s->state==2)expire_seen(r,s);
    *s=(Seen){seq,now,2};inc(&r->stats.detected);
}
static void closure(X4VideoRepair43 *r,unsigned method,uint64_t elapsed)
{
    Stats *s=&r->stats;inc(&s->closed[method]);inc(&s->samples[method]);
    unsigned bin=elapsed/1000u<X4_REPAIR43_CLOSE_BINS-1u?(unsigned)(elapsed/1000u):X4_REPAIR43_CLOSE_BINS-1u;
    inc(&s->bins[method][bin]);
    /* One writer, atomic observers: no CAS loop is needed for this maximum. */
    if(elapsed>load(&s->maximum[method]))atomic_store_explicit(&s->maximum[method],elapsed,memory_order_relaxed);
}
static bool admit(X4VideoRepair43 *r,const uint8_t *p,size_t bytes,uint64_t now,
    unsigned method,X4VideoRepair43Emit emit,void *context)
{
    uint16_t sequence=be16(p+2);uint64_t seq;
    if(!extended(r,sequence,&seq)){inc(&r->stats.late[method]);return false;}
    if(r->have_high&&seq<=r->high&&r->high-seq>=LEDGER){inc(&r->stats.late[method]);return false;}
    Seen *s=&r->seen[seq%LEDGER];
    if(s->state&&s->extended==seq){
        if(s->state==1){inc(&r->stats.duplicate[method]);return false;}
        if(s->state==2&&now-s->at>AGE_US)expire_seen(r,s);
        if(s->state==3){inc(&r->stats.late[method]);return false;}
        if(s->state==2)closure(r,method,now-s->at);
    }
    if(!r->have_high){r->have_high=true;r->high=seq;}
    else if(seq>r->high){
        uint64_t missed=seq-r->high-1u,keep=missed<LEDGER-1u?missed:LEDGER-1u;
        if(missed>keep){atomic_fetch_add_explicit(&r->stats.detected,missed-keep,memory_order_relaxed);atomic_fetch_add_explicit(&r->stats.expired,missed-keep,memory_order_relaxed);}
        uint64_t first=seq-keep;
        for(uint64_t at=first;at<seq;++at)new_gap(r,at,now);
        r->high=seq;
    }
    if(s->state==2&&s->extended!=seq)expire_seen(r,s);
    *s=(Seen){seq,now,1};
    Media *m=&r->media[seq%X4_REPAIR43_MEDIA_SLOTS];m->valid=true;m->extended=seq;m->at=now;m->bytes=0;
    if(bytes<=X4_REPAIR43_PACKET_CAP&&!(p[0]&16u)&&!r->config.mutable_extensions){m->bytes=bytes;memcpy(m->data,p,bytes);}
    else if(r->config.mode==2&&r->config.fec_accepted)inc(&r->stats.unsupported);
    inc(&r->stats.emitted[method]);emit(context,p,bytes,method,now);return true;
}
static bool mask_bit(const uint8_t *mask,unsigned bit)
{
    if(bit<15u)return (mask[(bit+1u)/8u]&(128u>>((bit+1u)%8u)))!=0;
    if(bit<46u){unsigned at=bit-15u+1u;return (mask[2u+at/8u]&(128u>>(at%8u)))!=0;}
    unsigned at=bit-46u+1u;return (mask[6u+at/8u]&(128u>>(at%8u)))!=0;
}
static bool fec_header(X4VideoRepair43 *r,Fec *f,const uint8_t *p,size_t n,uint64_t now)
{
    if(n<20u||n>X4_REPAIR43_PACKET_CAP||(p[0]&192u)||p[8]!=1||p[9]||p[10]||p[11]||be32(p+12)!=r->config.media_ssrc)return false;
    const uint8_t *mask=p+18;unsigned bits;size_t header;
    if(mask[0]&128u){bits=15;header=20;}
    else {if(n<24)return false;if(mask[2]&128u){bits=46;header=24;}
        else {if(n<32||!(mask[6]&128u))return false;bits=109;header=32;}}
    f->count=0;for(unsigned i=0;i<bits;++i)if(mask_bit(mask,i))f->offsets[f->count++]=(uint16_t)i;
    if(!f->count||n<=header)return false;
    f->valid=true;f->multiple_counted=false;f->at=now;f->bytes=n;f->header=header;f->base=be16(p+16);memcpy(f->data,p,n);
#if !X4_PUBLIC
    unsigned count=atomic_load_explicit(&r->header_count,memory_order_relaxed);
    if(count<3){r->header_bytes[count]=(unsigned)header;memcpy(r->headers[count],p,header);atomic_store_explicit(&r->header_count,count+1,memory_order_release);}
#endif
    return true;
}
static Media *cached(X4VideoRepair43 *r,uint64_t seq,uint64_t now)
{
    Media *m=&r->media[seq%X4_REPAIR43_MEDIA_SLOTS];
    return m->valid&&m->extended==seq&&m->bytes&&now-m->at<=AGE_US?m:NULL;
}
static bool recover_one(X4VideoRepair43 *r,Fec *f,uint64_t now,X4VideoRepair43Emit emit,void *context)
{
    unsigned missing=0;uint64_t target=0;
    for(unsigned i=0;i<f->count;++i){uint64_t seq;
        if(!extended(r,(uint16_t)(f->base+f->offsets[i]),&seq)){f->valid=false;inc(&r->stats.unsupported);return false;}
        if(!cached(r,seq,now)){++missing;target=seq;}}
    if(!missing){f->valid=false;inc(&r->stats.no_missing);return false;}
    if(missing!=1){if(!f->multiple_counted){inc(&r->stats.multiple);f->multiple_counted=true;}return false;}
    Seen *seen=&r->seen[target%LEDGER];
    if(seen->state&&seen->extended==target&&seen->state!=2){
        f->valid=false;inc(seen->state==1?&r->stats.duplicate[2]:&r->stats.late[2]);return false;}
    size_t body=f->bytes-f->header;
    if(body>X4_REPAIR43_PACKET_CAP-12u){f->valid=false;inc(&r->stats.errors[2]);return false;}
    uint8_t packet[X4_REPAIR43_PACKET_CAP]={0};memcpy(packet,f->data,8);memcpy(packet+12,f->data+f->header,body);
    for(unsigned i=0;i<f->count;++i){uint64_t seq;
        if(!extended(r,(uint16_t)(f->base+f->offsets[i]),&seq))return false;
        Media *m=cached(r,seq,now);if(!m)continue;
        if(m->bytes<12u||m->bytes-12u>body){f->valid=false;inc(&r->stats.errors[2]);return false;}
        packet[0]^=m->data[0];packet[1]^=m->data[1];
        uint16_t length=(uint16_t)(m->bytes-12u);packet[2]^=(uint8_t)(length>>8);packet[3]^=(uint8_t)length;
        for(unsigned j=4;j<8;++j)packet[j]^=m->data[j];
        for(size_t j=12;j<m->bytes;++j)packet[j]^=m->data[j];
    }
    size_t recovered=be16(packet+2);f->valid=false;
    if(recovered>body){inc(&r->stats.errors[2]);return false;}
    packet[0]=(uint8_t)((packet[0]&63u)|128u);put16(packet+2,(uint16_t)target);put32(packet+8,r->config.media_ssrc);
    Rtp parsed;
    if(!parse_rtp(packet,recovered+12u,&parsed)||parsed.pt!=r->config.media_pt){inc(&r->stats.errors[2]);return false;}
    if(packet[0]&16u){inc(&r->stats.unsupported);return false;}
    return admit(r,packet,recovered+12u,now,2,emit,context);
}
static void attempt(X4VideoRepair43 *r,uint64_t now,X4VideoRepair43Emit emit,void *context)
{
    unsigned recovered=0;
    /* At most eight passes/outputs. A successful recovery can unlock a group
     * earlier in the array, but cannot recurse or run an unbounded cascade. */
    for(unsigned pass=0;pass<MAX_RECOVERIES;++pass){bool progress=false;
        for(unsigned i=0;i<X4_REPAIR43_FEC_SLOTS&&recovered<MAX_RECOVERIES;++i)
            if(r->fec[i].valid&&recover_one(r,&r->fec[i],now,emit,context)){++recovered;progress=true;}
        if(!progress||recovered==MAX_RECOVERIES)break;
    }
}
X4VideoRepair43 *x4_video_repair43_create(const X4VideoRepair43Config *c)
{
    if(!c||c->mode>2||!c->media_ssrc||c->media_pt>127)return NULL;
#if X4_PUBLIC
    if(c->loss_percent||c->seed)return NULL;
#else
    if(!(c->loss_percent==0||c->loss_percent==1||c->loss_percent==3)||
        (c->loss_percent&&!c->seed))return NULL;
#endif
    X4VideoRepair43 *r=calloc(1,sizeof(*r));if(!r)return NULL;r->config=*c;
    if(!c->mode||!c->rtx_ssrc||c->rtx_ssrc==c->media_ssrc||c->rtx_pt==c->media_pt||c->rtx_pt>127)r->config.rtx_accepted=false;
    if(c->mode<2||!c->fec_ssrc||c->fec_ssrc==c->media_ssrc||c->fec_ssrc==c->rtx_ssrc||c->fec_pt==c->media_pt||c->fec_pt==c->rtx_pt||c->fec_pt>127)r->config.fec_accepted=false;
#if !X4_PUBLIC
    atomic_init(&r->header_count,0);
    for(unsigned i=0;i<3;++i)atomic_init(&r->stats.dropped[i],0);
#endif
    for(unsigned i=0;i<3;++i){atomic_init(&r->stats.packets[i],0);atomic_init(&r->stats.emitted[i],0);atomic_init(&r->stats.errors[i],0);atomic_init(&r->stats.closed[i],0);atomic_init(&r->stats.duplicate[i],0);atomic_init(&r->stats.late[i],0);atomic_init(&r->stats.samples[i],0);atomic_init(&r->stats.maximum[i],0);for(unsigned j=0;j<X4_REPAIR43_CLOSE_BINS;++j)atomic_init(&r->stats.bins[i][j],0);}
    atomic_init(&r->stats.detected,0);atomic_init(&r->stats.expired,0);atomic_init(&r->stats.empty,0);atomic_init(&r->stats.foreign,0);atomic_init(&r->stats.unsupported,0);atomic_init(&r->stats.no_missing,0);atomic_init(&r->stats.multiple,0);atomic_init(&r->stats.fec_expired,0);
    return r;
}
void x4_video_repair43_free(X4VideoRepair43 *r){free(r);}
int x4_video_repair43_process_observed(X4VideoRepair43 *r,const uint8_t *p,size_t n,
    uint64_t now,X4VideoRepair43Emit emit,void (*observe_original)(void *),void *context)
{
    if(!r||!p||!emit)return -1;
    Rtp v;if(!parse_rtp(p,n,&v)){inc(&r->stats.errors[0]);return 0;}
    unsigned method;
    if(v.ssrc==r->config.media_ssrc&&v.pt==r->config.media_pt)method=0;
    else if(v.pt==r->config.rtx_pt&&r->config.rtx_accepted){if(v.ssrc!=r->config.rtx_ssrc){inc(&r->stats.foreign);return 0;}method=1;}
    else if(v.pt==r->config.fec_pt&&r->config.fec_accepted){if(v.ssrc!=r->config.fec_ssrc){inc(&r->stats.foreign);return 0;}method=2;}
    else {inc(&r->stats.foreign);return 0;}
    inc(&r->stats.packets[method]);
#if !X4_PUBLIC
    if(simulated_loss(r,&v,p,method)){inc(&r->stats.dropped[method]);return 0;}
#endif
    if(now<r->last_now)now=r->last_now;x4_video_repair43_tick(r,now);
    if(!method){
        if(observe_original)observe_original(context);
        (void)admit(r,p,n,now,0,emit,context);
    }
    else if(method==1){
        size_t body=v.body_end-v.header;
        if(body<=2){inc(&r->stats.empty);return 0;}
        size_t size=v.header+body-2u;if(size>X4_REPAIR43_PACKET_CAP){inc(&r->stats.errors[1]);return 0;}
        uint8_t normal[X4_REPAIR43_PACKET_CAP];memcpy(normal,p,v.header);memcpy(normal+v.header,p+v.header+2u,body-2u);
        normal[0]&=(uint8_t)~32u;normal[1]=(uint8_t)((normal[1]&128u)|r->config.media_pt);
        put16(normal+2,be16(p+v.header));put32(normal+8,r->config.media_ssrc);
        (void)admit(r,normal,size,now,1,emit,context);
    }else {
        if(r->config.mutable_extensions){inc(&r->stats.unsupported);return 0;}
        Fec *f=&r->fec[r->fec_next++%X4_REPAIR43_FEC_SLOTS];
        if(f->valid)inc(&r->stats.fec_expired);
        f->valid=false;if(!fec_header(r,f,p+v.header,v.body_end-v.header,now)){inc(&r->stats.errors[2]);return 0;}
    }
    if(r->config.fec_accepted&&!r->config.mutable_extensions)attempt(r,now,emit,context);
    return 0;
}
int x4_video_repair43_process(X4VideoRepair43 *r,const uint8_t *p,size_t n,
    uint64_t now,X4VideoRepair43Emit emit,void *context)
{
    return x4_video_repair43_process_observed(r,p,n,now,emit,NULL,context);
}
static uint64_t quantile(const uint64_t *bins,uint64_t count,unsigned p,bool *finite)
{
    *finite=false;if(!count)return 0;uint64_t needed=count/100u*p+(count%100u*p+99u)/100u,seen=0;
    for(unsigned i=0;i<X4_REPAIR43_CLOSE_BINS;++i){seen+=bins[i];if(seen>=needed){if(i==X4_REPAIR43_CLOSE_BINS-1u)return 0;*finite=true;return (uint64_t)(i+1u)*1000u;}}return 0;
}
void x4_video_repair43_snapshot(const X4VideoRepair43 *r,X4VideoRepair43Snapshot *v)
{
    if(!v)return;memset(v,0,sizeof(*v));if(!r)return;
    v->allocated_bytes=sizeof(*r);v->mode=r->config.mode;
#if !X4_PUBLIC
    v->seed=r->config.seed;v->loss_percent=r->config.loss_percent;
    for(unsigned i=0;i<3;++i)v->lost_simulated[i]=load(&r->stats.dropped[i]);
#endif
    for(unsigned i=0;i<3;++i){v->packets[i]=load(&r->stats.packets[i]);v->emitted[i]=load(&r->stats.emitted[i]);v->format_errors[i]=load(&r->stats.errors[i]);v->gaps_closed[i]=load(&r->stats.closed[i]);v->duplicate[i]=load(&r->stats.duplicate[i]);v->late[i]=load(&r->stats.late[i]);v->closure_samples[i]=load(&r->stats.samples[i]);v->closure_max_us[i]=load(&r->stats.maximum[i]);for(unsigned j=0;j<X4_REPAIR43_CLOSE_BINS;++j)v->closure_histogram[i][j]=load(&r->stats.bins[i][j]);v->closure_p50_us[i]=quantile(v->closure_histogram[i],v->closure_samples[i],50,&v->closure_p50_finite[i]);v->closure_p95_us[i]=quantile(v->closure_histogram[i],v->closure_samples[i],95,&v->closure_p95_finite[i]);}
    v->gaps_detected=load(&r->stats.detected);v->gaps_expired=load(&r->stats.expired);v->rtx_empty=load(&r->stats.empty);v->foreign=load(&r->stats.foreign);v->unsupported=load(&r->stats.unsupported);v->fec_no_missing=load(&r->stats.no_missing);v->fec_multiple_missing=load(&r->stats.multiple);v->fec_cache_expired=load(&r->stats.fec_expired);
#if !X4_PUBLIC
    v->header_count=atomic_load_explicit(&r->header_count,memory_order_acquire);
    for(unsigned i=0;i<v->header_count;++i){v->header_bytes[i]=r->header_bytes[i];memcpy(v->headers[i],r->headers[i],r->header_bytes[i]);}
#endif
}
