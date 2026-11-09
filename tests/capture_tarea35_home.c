/* SPDX-License-Identifier: GPL-3.0-only */
/* T35 Phase A ONLY. Independent layout prototype using the real XCloud4
 * renderer/font and bundled stb_image. No production integration or network.
 * PsBox is a visual reference only; no PsBox code or resources are included. */
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"
#include "../src/ui/ui_surface.h"
#include "../src/ui/ui_draw_metrics.h"
#include "../src/auth/json.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { W=1920,H=1080,CW=384,CH=216,PITCH=408,LEFT=84,RIGHT=1836,
    MAX_PRODUCTS=32,MAX_ROWS=6,MAX_ROW_ITEMS=30,GUARD=16 };
_Static_assert(CW*9==CH*16,"cards must remain 16:9");
_Static_assert(812+CH+6<1044,"second row/focus must clear footer");
static const uint32_t WHITE=X4_RGB(245,247,250),SOFT=X4_RGB(204,215,228),
    MUTED=X4_RGB(163,178,197),GREEN=X4_RGB(74,222,128);
static size_t tracked_live,tracked_peak;
typedef union { max_align_t alignment;size_t bytes; } HeapHeader;
static void *tracked_alloc(size_t bytes)
{
    assert(bytes<SIZE_MAX-sizeof(HeapHeader));
    HeapHeader *h=malloc(sizeof(*h)+bytes);if(!h)return NULL;
    h->bytes=bytes;tracked_live+=sizeof(*h)+bytes;
    if(tracked_live>tracked_peak)tracked_peak=tracked_live;
    return h+1;
}
static void tracked_free(void *p)
{if(p){HeapHeader *h=(HeapHeader *)p-1;tracked_live-=sizeof(*h)+h->bytes;free(h);}}
static inline void *tracked_realloc(void *p,size_t bytes)
{
    if(!p)return tracked_alloc(bytes);
    HeapHeader *h=(HeapHeader *)p-1;void *next=tracked_alloc(bytes);if(!next)return NULL;
    memcpy(next,p,h->bytes<bytes?h->bytes:bytes);tracked_free(p);return next;
}
/* The prototype needs exact decoded rasters rather than T31's three scaled
 * surfaces. Static names keep this decoder isolated from production hooks.
 * Its complete image/IO allocation peak is reported below. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(n) tracked_alloc(n)
#define STBI_FREE(p) tracked_free(p)
#define STBI_REALLOC(p,n) tracked_realloc(p,n)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../third_party/stb/stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    char id[32],name[160],category[128],publisher[160],description[2048];
    char card_path[256],hero_path[256];
    X4CoverRaster card;
} Product;
typedef struct { unsigned item[MAX_ROW_ITEMS],count,kind; } Row;
typedef struct {
    Product product[MAX_PRODUCTS];unsigned product_count;
    Row row[MAX_ROWS];unsigned row_count;
    unsigned selected_row,selected_col[MAX_ROWS],first_row,hero_product;
    bool play_focused,pending;
    uint64_t focus_since_us;
    X4CoverRaster background;
    unsigned background_product,background_preparations,background_peak;
    size_t card_bytes;
} Home;
static const char *const keys[MAX_ROWS]={"recent","mine","free","new","popular","leaving"};
static const char *const labels_es[MAX_ROWS]={"Continúa","Mis juegos","Gratis","Novedades","Más populares","Se van pronto"};
static const char *const labels_en[MAX_ROWS]={"Continue playing","My games","Free to play","Recently added","Most popular","Leaving soon"};
static bool english;
static const char *tr(const char *es,const char *en){return english?en:es;}
static uint64_t now_us(void)
{
    struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));
    return (uint64_t)ts.tv_sec*1000000u+(uint64_t)ts.tv_nsec/1000u;
}
static unsigned char *read_file(const char *path,size_t *length)
{
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>=0&&size<=2*1024*1024);
    rewind(f);unsigned char *body=tracked_alloc((size_t)size+1);assert(body);
    assert(fread(body,1,(size_t)size,f)==(size_t)size);assert(!fclose(f));
    body[size]=0;*length=(size_t)size;return body;
}
static void dataset_path(char out[1024],const char *directory,const char *relative)
{
    assert(relative[0]&&relative[0]!='/'&&!strstr(relative,"..")&&!strchr(relative,'\\'));
    int n=snprintf(out,1024,"%s/%s",directory,relative);assert(n>0&&n<1024);
}
static X4CoverRaster decode_exact(const char *directory,const char *relative,unsigned want_w,unsigned want_h)
{
    X4CoverRaster r={0};if(!relative[0])return r;
    char path[1024];dataset_path(path,directory,relative);size_t n;
    unsigned char *body=read_file(path,&n);if(!body)return r;
    int w,h,channels;unsigned char *rgba=stbi_load_from_memory(body,(int)n,&w,&h,&channels,4);
    tracked_free(body);assert(rgba&&w==(int)want_w&&h==(int)want_h);
    const unsigned char endian[4]={1,2,3,4};uint32_t value;memcpy(&value,endian,4);
    assert(value==UINT32_C(0x04030201)); /* same x86 RGBA layout as PS4 */
    r=(X4CoverRaster){(unsigned)w,(unsigned)h,(uint32_t *)(void *)rgba};return r;
}
static void text_member(X4JsonSpan object,const char *key,char *out,size_t size,bool token)
{
    X4JsonSpan value;int rc=x4_json_member(object,key,&value);assert(rc>=0);out[0]=0;
    if(rc)assert(!(token?x4_json_token(value,out,size):x4_json_display_text(value,out,size)));
}
static unsigned product_index(const Home *h,const char *id)
{
    for(unsigned i=0;i<h->product_count;++i)if(!strcmp(h->product[i].id,id))return i;
    return UINT32_MAX;
}
static void load_dataset(Home *h,const char *directory)
{
    char path[1024];dataset_path(path,directory,"dataset.json");size_t n;
    unsigned char *body=read_file(path,&n);assert(body);
    X4JsonSpan root,products,item;assert(!x4_json_parse((const char *)body,n,&root));
    assert(x4_json_member(root,"products",&products)==1);size_t cursor=0;int rc;
    while((rc=x4_json_item(products,&cursor,&item))>0){
        assert(h->product_count<MAX_PRODUCTS);Product *p=&h->product[h->product_count++];
        text_member(item,"id",p->id,sizeof(p->id),true);assert(p->id[0]);
        text_member(item,"name",p->name,sizeof(p->name),false);
        text_member(item,"category",p->category,sizeof(p->category),false);
        text_member(item,"publisher",p->publisher,sizeof(p->publisher),false);
        text_member(item,"description",p->description,sizeof(p->description),false);
        text_member(item,"card_path",p->card_path,sizeof(p->card_path),true);
        text_member(item,"hero_path",p->hero_path,sizeof(p->hero_path),true);
        p->card=decode_exact(directory,p->card_path,CW,CH);assert(p->card.rgba);
        h->card_bytes+=(size_t)CW*CH*4;
    }
    assert(rc==0&&h->product_count>=4);
    X4JsonSpan row_object;assert(x4_json_member(root,"rows",&row_object)==1);
    for(unsigned kind=0;kind<MAX_ROWS;++kind){
        X4JsonSpan array;rc=x4_json_member(row_object,keys[kind],&array);assert(rc>=0);if(!rc)continue;
        Row row={.kind=kind};cursor=0;
        while((rc=x4_json_item(array,&cursor,&item))>0){
            char id[32];assert(!x4_json_token(item,id,sizeof(id)));unsigned i=product_index(h,id);
            if(i!=UINT32_MAX&&row.count<MAX_ROW_ITEMS)row.item[row.count++]=i;
        }
        assert(rc==0);if(row.count)h->row[h->row_count++]=row;
    }
    tracked_free(body);assert(h->row_count>=2);h->play_focused=true;h->background_product=UINT32_MAX;
}
static unsigned focused_product(const Home *h)
{
    if(h->play_focused)return h->hero_product;
    const Row *r=&h->row[h->selected_row];return r->item[h->selected_col[h->selected_row]];
}
static void navigate(Home *h,int dx,int dy,uint64_t timestamp)
{
    assert(!(dx&&dy));
    if(dy){
        if(h->play_focused){if(dy<0)return;h->play_focused=false;h->selected_row=0;}
        else if(dy<0&&h->selected_row==0)h->play_focused=true;
        else{int row=(int)h->selected_row+(dy<0?-1:1);if(row<0||row>=(int)h->row_count)return;h->selected_row=(unsigned)row;}
    }else if(dx&&!h->play_focused){
        int col=(int)h->selected_col[h->selected_row]+(dx<0?-1:1);
        if(col<0||col>=(int)h->row[h->selected_row].count)return;
        h->selected_col[h->selected_row]=(unsigned)col;
    }else return;
    if(h->selected_row<h->first_row)h->first_row=h->selected_row;
    if(h->selected_row>=h->first_row+2)h->first_row=h->selected_row-1;
    h->focus_since_us=timestamp;
}
static void precompose(X4CoverRaster *r)
{
    assert(r->width==W&&r->height==H);
    for(unsigned y=0;y<H;++y){
        unsigned bottom=y>390?((y-390)*248u)/(H-390):0;
        for(unsigned x=0;x<W;++x){
            unsigned left=x<1320?((1320-x)*224u)/1320:0;
            unsigned alpha=255u-((255u-left)*(255u-bottom))/255u;
            uint32_t c=r->rgba[(size_t)y*W+x];unsigned keep=255u-alpha;
            unsigned red=((c&255u)*keep+6u*alpha+127u)/255u;
            unsigned green=(((c>>8)&255u)*keep+10u*alpha+127u)/255u;
            unsigned blue=(((c>>16)&255u)*keep+18u*alpha+127u)/255u;
            r->rgba[(size_t)y*W+x]=X4_RGB(red,green,blue);
        }
    }
}
static bool update_background(Home *h,const char *directory,uint64_t timestamp)
{
    unsigned index=focused_product(h);
    if(timestamp<h->focus_since_us||timestamp-h->focus_since_us<300000||index==h->background_product)return false;
    X4CoverRaster next=decode_exact(directory,h->product[index].hero_path,W,H);
    if(!next.rgba)return false; /* previous art remains intact while pending */
    unsigned live=h->background.rgba?2:1;if(live>h->background_peak)h->background_peak=live;
    precompose(&next); /* worker-equivalent one-time preparation, outside draw */
    stbi_image_free(h->background.rgba);h->background=next;h->background_product=index;
    ++h->background_preparations;return true;
}
static void text(uint32_t *p,int x,int y,int size,int weight,const char *s,int width,uint32_t color)
{x4_text_ellipsis_px(p,x,y,size,weight,s,width,color);}
static void top_bar(uint32_t *p)
{
    x4_apple_draw_brand_logo(p,84,48);
    x4_apple_draw_pad_glyph(p,600,52,X4_PAD_L1);
    x4_apple_glass_panel(p,660,42,600,56,28,18,36);
    x4_apple_draw_pad_glyph(p,1276,52,X4_PAD_R1);
    const char *tabs[3]={tr("Descubrir","Discover"),tr("Catálogo","Catalog"),tr("Recientes","Recent")};
    for(unsigned i=0;i<3;++i){
        int x=666+(int)i*196;
        if(!i){x4_apple_fill_round_rect(p,x,47,192,46,23,X4_RGBA(255,255,255,46));x4_apple_stroke_round_rect(p,x,47,192,46,23,1.2f,X4_RGBA(255,255,255,85));}
        int width=x4_text_width_px(22,i?X4_FONT_REGULAR:X4_FONT_SEMIBOLD,tabs[i]);
        text(p,x+(192-width)/2,57,22,i?X4_FONT_REGULAR:X4_FONT_SEMIBOLD,tabs[i],172,i?MUTED:WHITE);
    }
    x4_apple_glass_panel(p,1444,44,52,52,26,20,38);
    x4_apple_draw_icon(p,1458,58,X4_ICON_SEARCH,WHITE);
    x4_apple_glass_panel(p,1510,44,52,52,26,20,38);
    x4_apple_draw_icon(p,1524,58,X4_ICON_SETTINGS,WHITE);
    x4_apple_draw_account_pill(p,1646,44,52,tr("Tu cuenta","Your account"),true,false);
}
static void hero_text(const Home *h,uint32_t *p)
{
    const Product *game=&h->product[focused_product(h)];
    const char *name=game->name[0]?game->name:game->id;
    x4_apple_text_wrap(p,LEFT,164,54,X4_FONT_BOLD,name,880,62,2,WHITE);
    char metadata[320];snprintf(metadata,sizeof(metadata),"%s%s%s",game->category,
        game->category[0]&&game->publisher[0]?" · ":"",game->publisher);
    text(p,LEFT,296,21,X4_FONT_SEMIBOLD,metadata,850,MUTED);
    x4_apple_text_wrap(p,LEFT,336,24,X4_FONT_REGULAR,game->description,850,32,2,SOFT);
    const char *label=h->pending?tr("Consultando…","Checking…"):tr("Jugar","Play");
    int width=h->pending?270:228;
    if(h->play_focused)x4_apple_stroke_round_rect(p,LEFT-6,410,width+12,76,38,3.0f,WHITE);
    x4_apple_fill_round_rect(p,LEFT,416,width,64,32,h->pending?X4_RGB(51,75,62):GREEN);
    if(h->pending)x4_apple_spinner(p,LEFT+22,436,3);
    int tw=x4_text_width_px(25,X4_FONT_SEMIBOLD,label);
    text(p,LEFT+(width-tw)/2+(h->pending?12:0),433,25,X4_FONT_SEMIBOLD,label,width-42,h->pending?SOFT:X4_RGB(7,23,14));
}
static void copy_card(uint32_t *p,const X4CoverRaster *image,int x,int y)
{
    assert(image->width==CW&&image->height==CH);
    int visible=RIGHT-x;if(visible>CW)visible=CW;if(visible<=0)return;
    assert(x>=LEFT&&y>=0&&y+CH<=H);
    for(int row=0;row<CH;++row)memcpy(p+(size_t)(y+row)*W+x,image->rgba+(size_t)row*CW,(size_t)visible*4);
}
static void draw_row(const Home *h,uint32_t *p,unsigned row_index,unsigned visual_row)
{
    const Row *row=&h->row[row_index];int header=492+(int)visual_row*282,cy=530+(int)visual_row*282;
    const char *title=(english?labels_en:labels_es)[row->kind];
    text(p,LEFT,header,29,X4_FONT_SEMIBOLD,title,1320,WHITE);
    char count[80];snprintf(count,sizeof(count),"%u / %u",h->selected_col[row_index]+1,row->count);
    text(p,1710,header+5,18,X4_FONT_REGULAR,count,126,MUTED);
    unsigned first=h->selected_col[row_index]>=4?h->selected_col[row_index]-3:0;
    for(unsigned slot=0;slot<5&&first+slot<row->count;++slot){
        unsigned item=first+slot;int cx=LEFT+(int)slot*PITCH;
        bool selected=!h->play_focused&&h->selected_row==row_index&&h->selected_col[row_index]==item;
        if(selected)x4_apple_stroke_round_rect(p,cx-6,cy-6,CW+12,CH+12,12,3.0f,WHITE);
        const Product *game=&h->product[row->item[item]];
        if(h->pending&&slot>0){
            int visible=RIGHT-cx;if(visible>CW)visible=CW;
            x4_apple_fill_round_rect(p,cx,cy,visible,CH,0,X4_RGB(23,32,42));
            if(visible>220){x4_apple_spinner(p,cx+24,cy+88,slot+2);text(p,cx+60,cy+88,20,X4_FONT_REGULAR,tr("Portada pendiente","Cover pending"),visible-84,MUTED);}
        }else copy_card(p,&game->card,cx,cy);
    }
}
static void footer(const Home *h,uint32_t *p)
{
    int x=LEFT;
    x+=x4_apple_draw_footer_hint(p,x,1044,X4_PAD_CROSS,tr("Seleccionar","Select"))+34;
    x+=x4_apple_draw_footer_hint(p,x,1044,X4_PAD_TRIANGLE,tr("Buscar","Search"))+34;
    (void)x4_apple_draw_footer_hint(p,x,1044,X4_PAD_OPTIONS,tr("Ajustes","Settings"));
    char position[64];snprintf(position,sizeof(position),tr("Fila %u de %u","Row %u of %u"),h->selected_row+1,h->row_count);
    text(p,1640,1051,18,X4_FONT_REGULAR,position,196,MUTED);
}
static void render(const Home *h,uint32_t *p)
{
    assert(h->background.rgba);memcpy(p,h->background.rgba,(size_t)W*H*4);
    top_bar(p);hero_text(h,p);
    for(unsigned i=0;i<2&&h->first_row+i<h->row_count;++i)draw_row(h,p,h->first_row+i,i);
    footer(h,p);
}
static void assert_cards_exact(const Home *h,const uint32_t *p)
{
    if(h->pending)return;
    for(unsigned visual=0;visual<2&&h->first_row+visual<h->row_count;++visual){
        unsigned ri=h->first_row+visual;const Row *row=&h->row[ri];unsigned first=h->selected_col[ri]>=4?h->selected_col[ri]-3:0;
        for(unsigned slot=0;slot<4&&first+slot<row->count;++slot){
            int x=LEFT+(int)slot*PITCH,y=530+(int)visual*282;
            const X4CoverRaster *image=&h->product[row->item[first+slot]].card;
            for(int line=0;line<CH;++line)assert(!memcmp(p+(size_t)(y+line)*W+x,image->rgba+(size_t)line*CW,(size_t)CW*4));
        }
    }
}
static void save(const char *directory,const char *name,const uint32_t *p,bool verify_only)
{
    if(verify_only)return;
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s.ppm",directory,name);assert(n>0&&n<(int)sizeof(path));
    FILE *f=fopen(path,"wbx");assert(f);assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    unsigned char row[W*3];
    for(unsigned y=0;y<H;++y){for(unsigned x=0;x<W;++x){uint32_t c=p[(size_t)y*W+x];row[x*3]=(unsigned char)c;row[x*3+1]=(unsigned char)(c>>8);row[x*3+2]=(unsigned char)(c>>16);}assert(fwrite(row,1,sizeof(row),f)==sizeof(row));}
    assert(!fclose(f));printf("capture PASS %s\n",name);
}
static void capture(const Home *h,const char *directory,const char *name,uint32_t *raw,bool verify_only)
{
    uint32_t *p=raw+GUARD;uint64_t start=now_us();render(h,p);uint64_t draw=now_us()-start;
    for(unsigned i=0;i<GUARD;++i)assert(raw[i]==0xac512943&&raw[GUARD+(size_t)W*H+i]==0xac512943);
    assert_cards_exact(h,p);save(directory,name,p,verify_only);
    printf("{\"capture\":\"%s\",\"draw_us\":%" PRIu64 ",\"focus_product\":\"%s\",\"background_product\":\"%s\",\"cards_1_to_1\":true}\n",
        name,draw,h->product[focused_product(h)].id,h->product[h->background_product].id);
}
static unsigned row_of_kind(const Home *h,unsigned kind)
{for(unsigned i=0;i<h->row_count;++i)if(h->row[i].kind==kind)return i;return UINT32_MAX;}
int main(int argc,char **argv)
{
    assert(argc==4||argc==5);english=!strcmp(argv[3],"en");assert(english||!strcmp(argv[3],"es"));
    bool verify_only=argc==5;assert(!verify_only||!strcmp(argv[4],"verify-only"));
    x4_lang_set(english?X4_LANG_EN:X4_LANG_ES,english?"en-US":"es-MX",english?"US":"MX");
    assert(!x4_font_init("assets/fonts/inter.x4font"));assert(!x4_font_is_fallback());
    Home *h=calloc(1,sizeof(*h));assert(h);load_dataset(h,argv[1]);
    uint32_t *raw=malloc(((size_t)W*H+2*GUARD)*4);assert(raw);
    for(size_t i=0;i<(size_t)W*H+2*GUARD;++i)raw[i]=0xac512943;
    assert(update_background(h,argv[1],300000));
    capture(h,argv[2],"01-jugar",raw,verify_only);
    unsigned new_row=row_of_kind(h,3);assert(new_row!=UINT32_MAX&&h->row[new_row].count>=3);
    navigate(h,0,1,1000000);
    while(h->selected_row<new_row)navigate(h,0,1,1000000);
    navigate(h,1,0,1000000);navigate(h,1,0,1000000);
    assert(!update_background(h,argv[1],1299999));assert(update_background(h,argv[1],1300000));
    assert(h->background_product==focused_product(h));
    capture(h,argv[2],"02-novedades-tercera",raw,verify_only);
    unsigned free_row=row_of_kind(h,2);assert(free_row!=UINT32_MAX);
    while(h->selected_row>free_row)navigate(h,0,-1,2000000);
    h->first_row=free_row;
    (void)update_background(h,argv[1],2300000);
    capture(h,argv[2],"03-gratis-desplazada",raw,verify_only);
    h->pending=true;h->play_focused=true;h->hero_product=0;h->first_row=0;h->selected_row=0;h->focus_since_us=3000000;
    (void)update_background(h,argv[1],3300000);
    capture(h,argv[2],"04-portadas-pendientes",raw,verify_only);
    assert(h->background_peak<=2&&tracked_peak<=64u*1024u*1024u);
    printf("{\"phase\":\"A-only\",\"renderer\":\"XCloud4 font+apple_ui+stb_image\",\"products\":%u,\"rows\":%u,\"card_width\":%u,\"card_height\":%u,\"card_bytes\":%zu,\"background_bytes\":%zu,\"background_peak_count\":%u,\"background_preparations\":%u,\"image_and_IO_peak_bytes\":%zu,\"font_heap_bytes\":%zu,\"PS4_runtime\":false}\n",
        h->product_count,h->row_count,CW,CH,h->card_bytes,(size_t)W*H*4,h->background_peak,h->background_preparations,tracked_peak,x4_font_heap_bytes());
    stbi_image_free(h->background.rgba);
    for(unsigned i=0;i<h->product_count;++i)stbi_image_free(h->product[i].card.rgba);
    assert(tracked_live==0);free(raw);free(h);x4_font_close();
    puts("PASS four real-renderer 1920x1080 prototype states; guarded canvas; full-card decoded JPEG pixel identity; debounce 300ms; <=2 backgrounds; no network/PS4");return 0;
}
