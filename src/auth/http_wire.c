/* SPDX-License-Identifier: GPL-3.0-only */
#include "http_wire.h"
#include <stdio.h>
#include <string.h>

enum { STATUS_LINE,HEADERS,BODY_LENGTH,BODY_CLOSE,CHUNK_SIZE,CHUNK_DATA,
       CHUNK_CR,CHUNK_LF,TRAILERS,FINISHED };
enum { FIELD_OTHER,FIELD_LENGTH,FIELD_TRANSFER,FIELD_ENCODING,FIELD_CONNECTION,FIELD_RETRY };
enum { EXT_SIZE,EXT_AFTER_SIZE,EXT_NAME_START,EXT_NAME,EXT_AFTER_NAME,
       EXT_VALUE_START,EXT_TOKEN,EXT_AFTER_VALUE,EXT_QUOTED,EXT_ESCAPE };
_Static_assert(sizeof(X4HttpWire)<=32768,"HTTP parser stays below 32KiB");

static unsigned char lower(unsigned char c)
{return c>='A'&&c<='Z'?(unsigned char)(c+'a'-'A'):c;}
static bool equal(const char *p,size_t n,const char *s)
{
    if(n!=strlen(s))return false;
    for(size_t i=0;i<n;++i)if(lower((unsigned char)p[i])!=(unsigned char)s[i])return false;
    return true;
}
static bool token(unsigned char c)
{
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
        strchr("!#$%&'*+-.^_`|~",c)!=NULL;
}
static bool value_ok(const char *s)
{
    if(!s)return false;
    for(;*s;++s)if(((unsigned char)*s<32&&*s!='\t')||(unsigned char)*s==127)return false;
    return true;
}
static int send_part(X4HttpWireSink sink,void *context,const char *s)
{int rc=sink(context,s,strlen(s));return rc>0?X4_HTTP_PROTOCOL:rc;}
static int send_header(X4HttpWireSink sink,void *context,const char *name,const char *value)
{
    int rc=send_part(sink,context,name);if(rc)return rc;
    rc=send_part(sink,context,": ");if(rc)return rc;
    rc=send_part(sink,context,value);if(rc)return rc;
    return send_part(sink,context,"\r\n");
}
int x4_http_wire_write_request(const X4HttpWireRequest *r,X4HttpWireSink sink,void *context)
{
    if(!r||!sink||!r->method||!r->host||!r->host[0]||!r->target||r->target[0]!='/'||
       !r->accept||!value_ok(r->accept)||(r->content_type&&!value_ok(r->content_type))||
       (r->user_agent&&!value_ok(r->user_agent))||r->header_count>X4_HTTP_HEADERS_MAX||
       (r->header_count&&!r->headers))return X4_HTTP_ARGUMENT;
    bool post=!strcmp(r->method,"POST");
    if(!post&&strcmp(r->method,"GET")&&strcmp(r->method,"DELETE"))return X4_HTTP_ARGUMENT;
    if(!post&&r->payload_size)return X4_HTTP_ARGUMENT;
    for(const char *s=r->host;*s;++s)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
       (*s>='0'&&*s<='9')||*s=='.'||*s=='-'))return X4_HTTP_ARGUMENT;
    for(const char *s=r->target;*s;++s)if((unsigned char)*s<=32||(unsigned char)*s>=127)return X4_HTTP_ARGUMENT;
    static const char *const reserved[]={"host","content-length","transfer-encoding","connection", "cookie",
        "accept","accept-encoding","content-type","user-agent"};
    for(size_t i=0;i<r->header_count;++i){
        const char *name=r->headers[i].name,*value=r->headers[i].value;
        if(!name||!*name||!value_ok(value))return X4_HTTP_ARGUMENT;
        size_t n=strlen(name);
        for(size_t k=0;k<n;++k)if(!token((unsigned char)name[k]))return X4_HTTP_ARGUMENT;
        for(size_t k=0;k<sizeof(reserved)/sizeof(*reserved);++k)
            if(equal(name,n,reserved[k]))return X4_HTTP_ARGUMENT;
    }
    int rc=send_part(sink,context,r->method);if(rc)return rc;
    if((rc=send_part(sink,context," "))||(rc=send_part(sink,context,r->target))||
       (rc=send_part(sink,context," HTTP/1.1\r\n")))return rc;
    if((rc=send_header(sink,context,"Host",r->host))||
       (rc=send_header(sink,context,"Accept",r->accept))||
       (rc=send_header(sink,context,"Accept-Encoding","identity"))||
       (rc=send_header(sink,context,"User-Agent",r->user_agent?r->user_agent:"XCloud4")))return rc;
    if(r->content_type&&(rc=send_header(sink,context,"Content-Type",r->content_type)))return rc;
    for(size_t i=0;i<r->header_count;++i)
        if((rc=send_header(sink,context,r->headers[i].name,r->headers[i].value)))return rc;
    if(post){char size[32];snprintf(size,sizeof(size),"%zu",r->payload_size);
        if((rc=send_header(sink,context,"Content-Length",size)))return rc;}
    if((rc=send_header(sink,context,"Connection",r->keepalive?"keep-alive":"close")))return rc;
    return send_part(sink,context,"\r\n");
}

static unsigned date_digits(const char *p,size_t n)
{
    unsigned v=0;for(size_t i=0;i<n;++i){if(p[i]<'0'||p[i]>'9')return UINT32_MAX;v=v*10+(unsigned)(p[i]-'0');}return v;
}
uint64_t x4_http_wire_retry_after(const char *p,size_t n,int64_t now)
{
    if(!p||!n)return 0;
    uint64_t seconds=0;bool digits=true;
    for(size_t i=0;i<n;++i)if(p[i]<'0'||p[i]>'9'){digits=false;break;}
    if(digits){
        for(size_t i=0;i<n;++i){unsigned digit=(unsigned)(p[i]-'0');
            if(seconds>(UINT64_MAX-digit)/10)return UINT64_MAX;seconds=seconds*10+digit;}
        return seconds>UINT64_MAX/1000?UINT64_MAX:seconds*1000;
    }
    if(n!=29||p[3]!=','||p[4]!=' '||p[7]!=' '||p[11]!=' '||p[16]!=' '||p[19]!=':'||p[22]!=':'||
       memcmp(p+25," GMT",4)||now<0)return 0;
    static const char weekdays[]="MonTueWedThuFriSatSun",months[]="JanFebMarAprMayJunJulAugSepOctNovDec";
    bool weekday=false;for(size_t i=0;i<7;++i)if(!memcmp(p,weekdays+3*i,3))weekday=true;
    if(!weekday)return 0;
    unsigned month=0;for(unsigned i=0;i<12;++i)if(!memcmp(p+8,months+3*i,3))month=i+1;
    unsigned day=date_digits(p+5,2),year=date_digits(p+12,4),hour=date_digits(p+17,2),minute=date_digits(p+20,2),second=date_digits(p+23,2);
    if(!month||year<1970||year>9999||hour>23||minute>59||second>59)return 0;
    static const unsigned days_in_month[]={31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap=year%4==0&&(year%100!=0||year%400==0);
    if(!day||day>days_in_month[month-1]+(month==2&&leap?1u:0u))return 0;
    unsigned adjusted=year-(month<=2),era=adjusted/400,yoe=adjusted-era*400;
    unsigned mp=month>2?month-3:month+9,doy=(153*mp+2)/5+day-1,doe=yoe*365+yoe/4-yoe/100+doy;
    int64_t days=(int64_t)era*146097+doe-719468,target=days*86400+hour*3600+minute*60+second;
    return target>now?(uint64_t)(target-now)*1000:0;
}
static int fail(X4HttpWire *p,int error)
{
    p->error=error;p->complete=false;p->body_bytes=0;
    if(p->body&&p->capacity)memset(p->body,0,p->capacity);
    return error;
}
static void reset_line(X4HttpWire *p)
{
    p->cr=p->colon=false;p->line_length=p->name_length=p->value_length=p->value_stored=0;
    p->line_kind=FIELD_OTHER;p->cl_number=0;p->cl_digit=p->cl_after_digit=p->cl_list_value=false;
}
void x4_http_wire_init(X4HttpWire *p,void *body,size_t capacity,int64_t now)
{
    if(!p)return;
    *p=(X4HttpWire){.state=STATUS_LINE,.body=body,.capacity=capacity,.now_seconds=now};
    if(!body||!capacity){p->error=X4_HTTP_ARGUMENT;return;}
    p->body[0]=0;
}
static int finish(X4HttpWire *p)
{p->state=FINISHED;p->complete=true;p->body[p->body_bytes]=0;return X4_HTTP_WIRE_DONE;}
static int append(X4HttpWire *p,unsigned char c)
{
    if(p->body_bytes>=p->capacity-1)return fail(p,X4_HTTP_BOUND);
    p->body[p->body_bytes++]=c;p->body[p->body_bytes]=0;return 0;
}
static unsigned classify_name(const X4HttpWire *p)
{
    if(p->name_length>=sizeof(p->name))return FIELD_OTHER;
    if(equal(p->name,p->name_length,"content-length"))return FIELD_LENGTH;
    if(equal(p->name,p->name_length,"transfer-encoding"))return FIELD_TRANSFER;
    if(equal(p->name,p->name_length,"content-encoding"))return FIELD_ENCODING;
    if(equal(p->name,p->name_length,"connection"))return FIELD_CONNECTION;
    if(equal(p->name,p->name_length,"retry-after"))return FIELD_RETRY;
    return FIELD_OTHER;
}
static int cl_component(X4HttpWire *p)
{
    if(!p->cl_digit||(p->cl_seen&&p->content_length!=p->cl_number))return fail(p,X4_HTTP_PROTOCOL);
    p->content_length=p->cl_number;p->cl_seen=true;p->cl_list_value=true;
    p->cl_number=0;p->cl_digit=p->cl_after_digit=false;return 0;
}
static int value_byte(X4HttpWire *p,unsigned char c)
{
    if((c<32&&c!='\t')||c==127)return fail(p,X4_HTTP_PROTOCOL);
    /* Trailer values never change framing, reuse or Retry-After metadata. */
    if(p->state==TRAILERS||p->line_kind==FIELD_OTHER)return 0;
    if(p->line_kind==FIELD_LENGTH){
        if(c==' '||c=='\t'){if(p->cl_digit)p->cl_after_digit=true;return 0;}
        if(c==',')return cl_component(p);
        if(c<'0'||c>'9'||p->cl_after_digit)return fail(p,X4_HTTP_PROTOCOL);
        unsigned digit=(unsigned)(c-'0');
        if(p->cl_number>(UINT64_MAX-digit)/10)return fail(p,X4_HTTP_PROTOCOL);
        p->cl_number=p->cl_number*10+digit;p->cl_digit=true;return 0;
    }
    if(p->line_kind==FIELD_CONNECTION&&c==','){
        size_t start=0,end=p->value_length;
        if(end<sizeof(p->value)){
            while(start<end&&(p->value[start]==' '||p->value[start]=='\t'))++start;
            while(end>start&&(p->value[end-1]==' '||p->value[end-1]=='\t'))--end;
            if(equal(p->value+start,end-start,"close"))p->connection_close=true;
            if(equal(p->value+start,end-start,"keep-alive"))p->connection_keepalive=true;
        }
        p->value_length=p->value_stored=0;return 0;
    }
    if(p->value_stored<sizeof(p->value))p->value[p->value_stored++]=(char)c;
    ++p->value_length;return 0;
}
static int end_field(X4HttpWire *p)
{
    if(!p->colon)return fail(p,X4_HTTP_PROTOCOL);
    if(p->state==TRAILERS){
        if(p->line_kind==FIELD_LENGTH||p->line_kind==FIELD_TRANSFER||p->line_kind==FIELD_ENCODING)
            return fail(p,X4_HTTP_PROTOCOL);
        return 0; /* Trailers cannot replace framing, cookies, or Retry-After. */
    }
    if(p->line_kind==FIELD_LENGTH)return cl_component(p);
    if(p->line_kind==FIELD_CONNECTION)return value_byte(p,',');
    if(p->line_kind==FIELD_OTHER)return 0;
    size_t start=0,end=p->value_length;
    if(end>=sizeof(p->value)){
        if(p->line_kind==FIELD_RETRY){p->retry_seen=p->retry_invalid=true;p->retry_after_ms=0;return 0;}
        return fail(p,X4_HTTP_PROTOCOL);
    }
    while(start<end&&(p->value[start]==' '||p->value[start]=='\t'))++start;
    while(end>start&&(p->value[end-1]==' '||p->value[end-1]=='\t'))--end;
    if(p->line_kind==FIELD_TRANSFER){
        if(p->te_seen||!equal(p->value+start,end-start,"chunked"))return fail(p,X4_HTTP_PROTOCOL);
        p->te_seen=true;
    }else if(p->line_kind==FIELD_ENCODING){
        if(!equal(p->value+start,end-start,"identity"))return fail(p,X4_HTTP_PROTOCOL);
        p->ce_seen=true;
    }else if(p->line_kind==FIELD_RETRY){
        if(p->retry_seen){p->retry_invalid=true;p->retry_after_ms=0;}
        else{p->retry_seen=true;p->retry_after_ms=end-start<=64?x4_http_wire_retry_after(p->value+start,end-start,p->now_seconds):0;}
    }
    return 0;
}
static int end_headers(X4HttpWire *p)
{
    if(p->cl_seen&&p->te_seen)return fail(p,X4_HTTP_PROTOCOL);
    if(p->status<200){
        if(p->status==101)return fail(p,X4_HTTP_PROTOCOL); /* No protocol switch was requested. */
        ++p->interim_count;p->state=STATUS_LINE;p->status_pos=0;p->status=0;p->http10=false;
        p->cl_seen=p->te_seen=p->ce_seen=p->connection_close=p->connection_keepalive=false;
        p->retry_seen=p->retry_invalid=false;p->retry_after_ms=0;p->content_length=0;
        return 0;
    }
    p->body_forbidden=p->status==204||p->status==304;
    if(p->body_forbidden)return finish(p);
    if(p->te_seen){p->state=CHUNK_SIZE;p->chunk_size=0;p->chunk_digits=0;p->chunk_ext_state=EXT_SIZE;return 0;}
    if(p->cl_seen){p->state=BODY_LENGTH;p->remaining=p->content_length;return p->remaining?0:finish(p);}
    p->state=BODY_CLOSE;return 0;
}
static int end_line(X4HttpWire *p)
{
    if(++p->header_lines>X4_HTTP_WIRE_LINES_MAX)return fail(p,X4_HTTP_BOUND);
    int rc=0;
    if(p->state==STATUS_LINE){
        if(p->status_pos<12||memcmp(p->status_prefix,"HTTP/1.",7)||
           (p->status_prefix[7]!='0'&&p->status_prefix[7]!='1')||p->status_prefix[8]!=' '||
           p->status_prefix[9]<'1'||p->status_prefix[9]>'5'||p->status_prefix[10]<'0'||p->status_prefix[10]>'9'||
           p->status_prefix[11]<'0'||p->status_prefix[11]>'9')return fail(p,X4_HTTP_PROTOCOL);
        p->http10=p->status_prefix[7]=='0';p->status=(p->status_prefix[9]-'0')*100+(p->status_prefix[10]-'0')*10+p->status_prefix[11]-'0';
        p->state=HEADERS;
    }else if(!p->line_length){rc=p->state==TRAILERS?finish(p):end_headers(p);}
    else rc=end_field(p);
    reset_line(p);return rc;
}
static int header_byte(X4HttpWire *p,unsigned char c)
{
    if(p->header_bytes>=X4_HTTP_WIRE_HEADERS_MAX)return fail(p,X4_HTTP_BOUND);
    ++p->header_bytes;
    if(p->cr){if(c!='\n')return fail(p,X4_HTTP_PROTOCOL);return end_line(p);}
    if(c=='\r'){p->cr=true;return 0;}
    if(c=='\n'||!c||c==127)return fail(p,X4_HTTP_PROTOCOL);
    if(p->state==STATUS_LINE){
        if(c<32&&c!='\t')return fail(p,X4_HTTP_PROTOCOL);
        if(p->status_pos<12)p->status_prefix[p->status_pos]=(char)c;
        else if(p->status_pos==12&&c!=' '&&c!='\t')return fail(p,X4_HTTP_PROTOCOL);
        ++p->status_pos;++p->line_length;return 0;
    }
    if(!p->line_length&&(c==' '||c=='\t'))return fail(p,X4_HTTP_PROTOCOL); /* obs-fold */
    ++p->line_length;
    if(!p->colon){
        if(c==':'){
            if(!p->name_length)return fail(p,X4_HTTP_PROTOCOL);
            p->colon=true;p->line_kind=classify_name(p);return 0;
        }
        if(!token(c))return fail(p,X4_HTTP_PROTOCOL);
        if(p->name_length<sizeof(p->name))p->name[p->name_length]=(char)c;
        ++p->name_length;return 0;
    }
    return value_byte(p,c);
}
static int hex(unsigned char c)
{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool chunk_end_ok(const X4HttpWire *p)
{
    return p->chunk_digits&&(p->chunk_ext_state==EXT_SIZE||p->chunk_ext_state==EXT_AFTER_SIZE||
        p->chunk_ext_state==EXT_NAME||p->chunk_ext_state==EXT_AFTER_NAME||
        p->chunk_ext_state==EXT_TOKEN||p->chunk_ext_state==EXT_AFTER_VALUE);
}
static int chunk_byte(X4HttpWire *p,unsigned char c)
{
    if(++p->line_length>X4_HTTP_WIRE_HEADERS_MAX)return fail(p,X4_HTTP_BOUND);
    if(p->cr){
        if(c!='\n')return fail(p,X4_HTTP_PROTOCOL);
        if(!p->chunk_size){p->state=TRAILERS;reset_line(p);}
        else{p->remaining=p->chunk_size;p->state=CHUNK_DATA;p->cr=false;}
        return 0;
    }
    if(c=='\r'){if(!chunk_end_ok(p))return fail(p,X4_HTTP_PROTOCOL);p->cr=true;return 0;}
    if(!c||c=='\n'||c==127||(c<32&&c!='\t'))return fail(p,X4_HTTP_PROTOCOL);
    unsigned st=p->chunk_ext_state;bool white=c==' '||c=='\t';
    if(st==EXT_SIZE){
        int digit=hex(c);
        if(digit>=0){if(p->chunk_size>(UINT64_MAX-(unsigned)digit)/16)return fail(p,X4_HTTP_PROTOCOL);
            p->chunk_size=p->chunk_size*16+(unsigned)digit;++p->chunk_digits;return 0;}
        if(!p->chunk_digits)return fail(p,X4_HTTP_PROTOCOL);
        if(white){p->chunk_ext_state=EXT_AFTER_SIZE;return 0;}
        if(c==';'){p->chunk_ext_state=EXT_NAME_START;return 0;}
    }else if(st==EXT_AFTER_SIZE){if(white)return 0;if(c==';'){p->chunk_ext_state=EXT_NAME_START;return 0;}}
    else if(st==EXT_NAME_START){if(white)return 0;if(token(c)){p->chunk_ext_state=EXT_NAME;return 0;}}
    else if(st==EXT_NAME){if(token(c))return 0;if(white){p->chunk_ext_state=EXT_AFTER_NAME;return 0;}
        if(c=='='){p->chunk_ext_state=EXT_VALUE_START;return 0;}if(c==';'){p->chunk_ext_state=EXT_NAME_START;return 0;}}
    else if(st==EXT_AFTER_NAME){if(white)return 0;if(c=='='){p->chunk_ext_state=EXT_VALUE_START;return 0;}
        if(c==';'){p->chunk_ext_state=EXT_NAME_START;return 0;}}
    else if(st==EXT_VALUE_START){if(white)return 0;if(c=='"'){p->chunk_ext_state=EXT_QUOTED;return 0;}
        if(token(c)){p->chunk_ext_state=EXT_TOKEN;return 0;}}
    else if(st==EXT_TOKEN){if(token(c))return 0;if(white){p->chunk_ext_state=EXT_AFTER_VALUE;return 0;}
        if(c==';'){p->chunk_ext_state=EXT_NAME_START;return 0;}}
    else if(st==EXT_AFTER_VALUE){if(white)return 0;if(c==';'){p->chunk_ext_state=EXT_NAME_START;return 0;}}
    else if(st==EXT_QUOTED){if(c=='"')p->chunk_ext_state=EXT_AFTER_VALUE;else if(c=='\\')p->chunk_ext_state=EXT_ESCAPE;return 0;}
    else if(st==EXT_ESCAPE){p->chunk_ext_state=EXT_QUOTED;return 0;}
    return fail(p,X4_HTTP_PROTOCOL);
}
int x4_http_wire_feed(X4HttpWire *p,const void *bytes,size_t size,size_t *consumed)
{
    if(consumed)*consumed=0;
    if(!p||!consumed||(!bytes&&size))return X4_HTTP_ARGUMENT;
    if(p->error)return p->error;if(p->complete)return X4_HTTP_WIRE_DONE;
    const unsigned char *data=bytes;
    for(size_t i=0;i<size;++i){
        unsigned char c=data[i];p->response_started=true;int rc=0;
        if(p->state==STATUS_LINE||p->state==HEADERS||p->state==TRAILERS)rc=header_byte(p,c);
        else if(p->state==CHUNK_SIZE)rc=chunk_byte(p,c);
        else if(p->state==BODY_LENGTH||p->state==CHUNK_DATA){
            rc=append(p,c);if(!rc&&!--p->remaining){
                if(p->state==BODY_LENGTH)rc=finish(p);else p->state=CHUNK_CR;
            }
        }else if(p->state==BODY_CLOSE)rc=append(p,c);
        else if(p->state==CHUNK_CR){if(c!='\r')rc=fail(p,X4_HTTP_PROTOCOL);else p->state=CHUNK_LF;}
        else if(p->state==CHUNK_LF){if(c!='\n')rc=fail(p,X4_HTTP_PROTOCOL);else{
            p->state=CHUNK_SIZE;p->chunk_size=0;p->chunk_digits=0;p->chunk_ext_state=EXT_SIZE;p->line_length=0;p->cr=false;}}
        else rc=fail(p,X4_HTTP_PROTOCOL);
        *consumed=i+1;if(rc)return rc;
    }
    return p->complete?X4_HTTP_WIRE_DONE:X4_HTTP_WIRE_MORE;
}
int x4_http_wire_eof(X4HttpWire *p)
{
    if(!p)return X4_HTTP_ARGUMENT;if(p->error)return p->error;
    if(p->complete)return X4_HTTP_WIRE_DONE;
    return p->state==BODY_CLOSE?finish(p):fail(p,X4_HTTP_PROTOCOL);
}
X4HttpWireResult x4_http_wire_result(const X4HttpWire *p)
{
    if(!p)return (X4HttpWireResult){0};
    bool framed=p->cl_seen||p->te_seen||p->body_forbidden;
    return (X4HttpWireResult){.status=p->error?0:p->status,.body_bytes=p->body_bytes,
        .reusable=p->complete&&framed&&!p->connection_close&&(!p->http10||p->connection_keepalive),
        .response_started=p->response_started,.complete=p->complete,.retry_after_ms=p->error?0:p->retry_after_ms};
}
