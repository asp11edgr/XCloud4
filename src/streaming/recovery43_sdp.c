/* SPDX-License-Identifier: GPL-3.0-only */
/* Original XCloud4 negotiation; RFC4588 and flexfec-03 protocol data only. */
#include "recovery43_sdp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {const char *p;size_t n;} Span;
typedef struct {size_t start,line_end,body,end;bool active;const char *newline;} Video;
static bool prefix(Span s,const char *p)
{size_t n=strlen(p);return s.n>=n&&!memcmp(s.p,p,n);}
static bool equal(Span s,const char *p)
{size_t n=strlen(p);return s.n==n&&!memcmp(s.p,p,n);}
static bool contains(Span s,const char *p)
{size_t n=strlen(p);if(n>s.n)return false;for(size_t i=0;i<=s.n-n;++i)if(!memcmp(s.p+i,p,n))return true;return false;}
static Span token(Span *s)
{
    while(s->n&&(s->p[0]==' '||s->p[0]=='\t')){++s->p;--s->n;}
    size_t n=0;while(n<s->n&&s->p[n]!=' '&&s->p[n]!='\t'&&s->p[n]!=';')++n;
    Span t={s->p,n};s->p+=n;s->n-=n;return t;
}
static bool number(Span s,uint32_t max,uint32_t *out)
{
    if(!s.n)return false;uint32_t v=0;
    for(size_t i=0;i<s.n;++i){unsigned d=(unsigned char)s.p[i]-'0';
        if(d>9||v>(max-d)/10u)return false;v=v*10u+d;}
    *out=v;return true;
}
static bool line(const char *text,size_t bytes,size_t *at,Span *out)
{
    if(*at>=bytes)return false;size_t begin=*at,end=begin;
    while(end<bytes&&text[end]!='\n')++end;
    *at=end<bytes?end+1:end;
    if(end>begin&&text[end-1]=='\r')--end;
    *out=(Span){text+begin,end-begin};return true;
}
static int video(const char *s,size_t n,Video *v)
{
    memset(v,0,sizeof(*v));size_t at=0;Span l;unsigned found=0;
    while(at<n){size_t start=at;if(!line(s,n,&at,&l))break;
        if(prefix(l,"m=")){
            if(found&&v->end==n)v->end=start;
            if(prefix(l,"m=video ")){
                if(++found!=1)return -2;
                Span rest={l.p+8,l.n-8};uint32_t port=0;
                v->active=number(token(&rest),65535,&port)&&port!=0;
                v->start=start;v->line_end=(size_t)(l.p-s)+l.n;v->body=at;v->end=n;
                v->newline=(at>=2&&s[at-2]=='\r')?"\r\n":"\n";
            }
        }
    }
    return found==1&&v->active?0:-1;
}
static unsigned free_pt(bool used[128],unsigned preferred)
{
    if(!used[preferred]){used[preferred]=true;return preferred;}
    for(unsigned p=96;p<128;++p)if(!used[p]){used[p]=true;return p;}
    return 128;
}
int x4_recovery43_sdp_offer(char *sdp,size_t cap,unsigned mode,X4Recovery43Offer *offered)
{
    if(offered)*offered=(X4Recovery43Offer){102,0,0};
    if(!mode)return 0;
    if(!sdp||!cap||mode>2)return -1;
    size_t n=strnlen(sdp,cap);Video v;if(n==cap||video(sdp,n,&v))return -1;
    bool used[128]={false},h264=false;size_t at=v.start;Span l;
    if(!line(sdp,n,&at,&l))return -1;
    Span rest={l.p,l.n};(void)token(&rest);(void)token(&rest);(void)token(&rest);
    uint32_t p;
    while(rest.n){Span t=token(&rest);if(!t.n)break;if(number(t,127,&p))used[p]=true;}
    at=v.body;
    while(at<v.end&&line(sdp,v.end,&at,&l))if(prefix(l,"a=rtpmap:")){
        rest=(Span){l.p+9,l.n-9};if(number(token(&rest),127,&p)){
            used[p]=true;if(p==102&&equal(token(&rest),"H264/90000"))h264=true;}}
    if(!used[102]||!h264)return -1;
    unsigned rtx=free_pt(used,103),fec=mode==2?free_pt(used,49):0;
    if(rtx>127||fec>127)return -1;
    char extra[256];int len=snprintf(extra,sizeof(extra),
        "a=rtpmap:%u rtx/90000%sa=fmtp:%u apt=102%s",rtx,v.newline,rtx,v.newline);
    if(len<0||(size_t)len>=sizeof(extra))return -1;
    if(mode==2){int k=snprintf(extra+len,sizeof(extra)-(size_t)len,
        "a=rtpmap:%u flexfec-03/90000%sa=fmtp:%u repair-window=10000000%s",fec,v.newline,fec,v.newline);
        if(k<0||(size_t)k>=sizeof(extra)-(size_t)len)return -1;len+=k;}
    char pts[24];int ptlen=mode==2?snprintf(pts,sizeof(pts)," %u %u",rtx,fec):snprintf(pts,sizeof(pts)," %u",rtx);
    bool separator=v.end&&sdp[v.end-1]!='\n';size_t nl=strlen(v.newline);
    size_t add=(size_t)len+(size_t)ptlen+(separator?nl:0);
    if(add>cap||n>=cap-add)return -1;
    char *copy=malloc(n+add+1);if(!copy)return -1;
    size_t pos=0;
    memcpy(copy,sdp,v.line_end);pos=v.line_end;
    memcpy(copy+pos,pts,(size_t)ptlen);pos+=(size_t)ptlen;
    memcpy(copy+pos,sdp+v.line_end,v.end-v.line_end);pos+=v.end-v.line_end;
    if(separator){memcpy(copy+pos,v.newline,nl);pos+=nl;}
    memcpy(copy+pos,extra,(size_t)len);pos+=(size_t)len;
    memcpy(copy+pos,sdp+v.end,n-v.end+1);pos+=n-v.end;
    if(pos!=n+add){free(copy);return -1;}
    memcpy(sdp,copy,pos+1);free(copy);
    if(offered)*offered=(X4Recovery43Offer){102,(uint8_t)rtx,(uint8_t)fec};
    return 0;
}
typedef struct {uint8_t codec,seen,apt_seen;uint16_t apt;} Map;
static bool ssrc_declared(const uint32_t *ids,unsigned count,uint32_t id)
{for(unsigned i=0;i<count;++i)if(ids[i]==id)return true;return false;}
int x4_recovery43_sdp_answer(const char *sdp,size_t bytes,unsigned mode,X4Recovery43Negotiation *a)
{
    if(!a)return -1;memset(a,0,sizeof(*a));a->media_pt=102;
    if(!sdp||bytes>32767||mode>2)return -1;
    Video v;if(video(sdp,bytes,&v))return 0;a->video_found=true;
    Map maps[128]={0};bool listed[128]={false};uint32_t declared[64]={0};unsigned ndecl=0;
    uint32_t fid_media=0,fid_rtx=0,fec_media=0,fec_ssrc=0;
    unsigned fid_count=0,fec_count=0;bool overflow=false;
    size_t at=v.start;Span l;
    if(!line(sdp,bytes,&at,&l))return 0;
    Span formats={l.p,l.n};(void)token(&formats);(void)token(&formats);(void)token(&formats);
    while(formats.n){uint32_t pt=0;Span t=token(&formats);if(!t.n)break;
        if(number(t,127,&pt))listed[pt]=true;}
    at=v.body;
    while(at<v.end&&line(sdp,v.end,&at,&l)){
        uint32_t pt=0,first=0,second=0;Span rest;
        if(prefix(l,"a=rtpmap:")){
            rest=(Span){l.p+9,l.n-9};if(!number(token(&rest),127,&pt))continue;
            Span c=token(&rest);unsigned codec=equal(c,"H264/90000")?1:equal(c,"rtx/90000")?2:equal(c,"flexfec-03/90000")?3:4;
            if(maps[pt].seen&&maps[pt].codec!=codec)overflow=true;
            maps[pt].seen=1;maps[pt].codec=(uint8_t)codec;
        }else if(prefix(l,"a=fmtp:")){
            rest=(Span){l.p+7,l.n-7};if(!number(token(&rest),127,&pt))continue;
            while(rest.n){while(rest.n&&(rest.p[0]==' '||rest.p[0]=='\t'||rest.p[0]==';')){++rest.p;--rest.n;}
                size_t k=0;while(k<rest.n&&rest.p[k]!=';')++k;Span f={rest.p,k};
                while(f.n&&(f.p[f.n-1]==' '||f.p[f.n-1]=='\t'))--f.n;
                if(prefix(f,"apt=")){Span val={f.p+4,f.n-4};
                    if(!number(val,127,&first)||(maps[pt].apt_seen&&maps[pt].apt!=first))overflow=true;
                    else {maps[pt].apt=(uint16_t)first;maps[pt].apt_seen=1;}}
                rest.p+=k;rest.n-=k;
            }
        }else if(prefix(l,"a=ssrc:")){
            rest=(Span){l.p+7,l.n-7};if(number(token(&rest),UINT32_MAX,&first)&&first&&!ssrc_declared(declared,ndecl,first)){
                if(ndecl==64)overflow=true;else declared[ndecl++]=first;}
        }else if(prefix(l,"a=ssrc-group:FID ")||prefix(l,"a=ssrc-group:FEC-FR ")){
            bool fid=prefix(l,"a=ssrc-group:FID ");size_t skip=fid?17:20;
            rest=(Span){l.p+skip,l.n-skip};
            if(!number(token(&rest),UINT32_MAX,&first)||!number(token(&rest),UINT32_MAX,&second)||!first||!second||first==second||token(&rest).n){overflow=true;continue;}
            if(fid){++fid_count;fid_media=first;fid_rtx=second;}
            else{++fec_count;fec_media=first;fec_ssrc=second;}
        }else if(prefix(l,"a=extmap:")&&
            (contains(l,"abs-send-time")||contains(l,"transport-wide-cc")||
             contains(l,"toffset")||contains(l,"video-timing")))a->mutable_extensions=true;
    }
    /* Attributes may remain for rejected formats. Only the m= line actually
     * negotiates a PT; never enable a repair from an orphan rtpmap/fmtp. */
    if(overflow||!listed[102]||!maps[102].seen||maps[102].codec!=1)return 0;
    unsigned rtx=128,fec=128;
    for(unsigned i=0;i<128;++i){
        if(listed[i]&&maps[i].codec==2&&maps[i].apt_seen&&maps[i].apt==102){if(rtx!=128){rtx=129;break;}rtx=i;}}
    if(fid_count==1&&ssrc_declared(declared,ndecl,fid_media))a->media_ssrc=fid_media;
    if(mode&&fid_count==1&&rtx<128&&ssrc_declared(declared,ndecl,fid_media)&&ssrc_declared(declared,ndecl,fid_rtx)){
        a->rtx_accepted=true;a->rtx_ssrc=fid_rtx;a->rtx_pt=(uint8_t)rtx;}
    if(mode==2&&a->rtx_accepted&&!a->mutable_extensions){
        for(unsigned i=0;i<128;++i)if(listed[i]&&maps[i].codec==3){if(fec!=128){fec=129;break;}fec=i;}
        if(fec_count==1&&fec<128&&fec_media==fid_media&&fec_ssrc!=fid_rtx&&
            ssrc_declared(declared,ndecl,fec_ssrc)){
            a->fec_accepted=true;a->fec_ssrc=fec_ssrc;a->fec_pt=(uint8_t)fec;}
    }
    /* Loss-only mode still needs its real media SSRC if RTX was declined. */
    if(!a->media_ssrc&&ndecl==1)a->media_ssrc=declared[0];
    return 0;
}
