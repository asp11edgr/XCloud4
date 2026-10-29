/* SPDX-License-Identifier: GPL-3.0-only */
#include "apple_ui.h"
#include "font.h"
#include "../core/build_identity.h"
#include "../../third_party/qrcodegen/qrcodegen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef X4_WIDTH
#define X4_WIDTH 1920
#endif
#ifndef X4_HEIGHT
#define X4_HEIGHT 1080
#endif

static unsigned animation_phase;
void x4_apple_set_animation_phase(unsigned phase) { animation_phase = phase; }
unsigned x4_apple_get_animation_phase(void) { return animation_phase; }

static bool update_tag_valid(const char *tag)
{
    if (!tag || !tag[0]) return false;
    unsigned i=0;
    for (; i<64 && tag[i]; ++i) {
        unsigned char c=(unsigned char)tag[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
              c=='.'||c=='_'||c=='-')) return false;
    }
    return i>0 && i<64 && strcmp(tag,".") && strcmp(tag,"..");
}

bool x4_apple_update_notice_offer(X4UpdateNotice *notice,const char *version,
    const char *tag,bool eligible,bool is_test)
{
    if (!notice || !eligible || notice->open || (!is_test && notice->shown) || !version) return false;
    size_t size=0;
    while (size<sizeof(notice->version) && version[size]) {
        unsigned char c=(unsigned char)version[size];
        if (c<0x20 || c>0x7e) return false;
        ++size;
    }
    if (!size || size>=sizeof(notice->version)) return false;
    memcpy(notice->version,version,size+1);
    if (update_tag_valid(tag))
        snprintf(notice->url,sizeof(notice->url),"https://github.com/asp11edgr/XCloud4/releases/tag/%s",tag);
    else
        snprintf(notice->url,sizeof(notice->url),"https://github.com/asp11edgr/XCloud4/releases");
    notice->open=true;
    if (!is_test) notice->shown=true;
    return true;
}

bool x4_apple_update_notice_handle(X4UpdateNotice *notice,bool circle)
{
    if (!notice || !notice->open) return false;
    if (circle) notice->open=false;
    return true;
}

static void update_notice_qr(uint32_t *pixels,int x,int y,const char *url)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX],temp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(url,temp,qr,qrcodegen_Ecc_MEDIUM,qrcodegen_VERSION_MIN,
        10,qrcodegen_Mask_AUTO,true)) return;
    int size=qrcodegen_getSize(qr),scale=330/(size+8),box=(size+8)*scale;
    int left=x+(330-box)/2,top=y+(330-box)/2;
    x4_apple_fill_round_rect(pixels,x,y,330,330,18,X4_RGB(255,255,255));
    const uint32_t black=X4_RGB(9,15,25);
    for(int row=0;row<size;++row)for(int col=0;col<size;++col){
        if(!qrcodegen_getModule(qr,col,row))continue;
        int dx=left+(col+4)*scale,dy=top+(row+4)*scale;
        for(int yy=0;yy<scale;++yy)for(int xx=0;xx<scale;++xx)
            pixels[(size_t)(dy+yy)*X4_WIDTH+dx+xx]=black;
    }
}

void x4_apple_update_notice_draw(const X4UpdateNotice *notice,uint32_t *p)
{
    if(!notice||!notice->open||!p)return;
    x4_apple_fill_round_rect(p,0,0,X4_WIDTH,X4_HEIGHT,0,X4_RGBA(3,7,15,185));
    x4_apple_glass_panel(p,300,218,1320,644,32,70,78);
    x4_text_ellipsis_px(p,362,274,42,X4_FONT_BOLD,x4_tr(X4_TR_UPDATE_NOTICE_TITLE),1196,X4_RGB(245,248,252));
    char version[192];
    snprintf(version,sizeof(version),x4_tr(X4_TR_UPDATE_NOTICE_VERSION_FMT),notice->version,X4_PRODUCT_VERSION);
    x4_apple_text_wrap(p,362,362,30,X4_FONT_SEMIBOLD,version,700,42,4,X4_RGB(76,230,151));
    update_notice_qr(p,1180,376,notice->url);
    x4_apple_text_wrap(p,1138,728,20,X4_FONT_REGULAR,x4_tr(X4_TR_UPDATE_NOTICE_DESCRIPTION),420,28,3,X4_RGB(197,209,225));
    x4_text_ellipsis_px(p,1138,824,18,X4_FONT_REGULAR,"github.com/asp11edgr/XCloud4/releases",420,X4_RGB(171,188,209));
    x4_apple_draw_footer_hint(p,362,770,X4_PAD_CIRCLE,x4_tr(X4_TR_UPDATE_NOTICE_CLOSE));
}

void x4_apple_draw_update_test_action(uint32_t *p,int x,int y)
{
#if !X4_PUBLIC
    x4_apple_draw_footer_hint(p,x,y,X4_PAD_TRIANGLE,x4_tr(X4_TR_UPDATE_NOTICE_TEST));
#else
    (void)p;(void)x;(void)y;
#endif
}

static void access_line(uint32_t *p, int x, int y, int size, int weight,
    const char *text, int width, uint32_t color)
{
    x4_text_ellipsis_px(p, x, y, size, weight, text, width, color);
}

void x4_apple_draw_access_controls(uint32_t *p, int x, int y, int w, int h,
    const X4AccessCountryUi *ui, X4AccessCountryCardPhase phase, bool settings)
{
    if (!p || !ui || w < 600 || h < 300) return;
    const uint32_t ink = X4_RGB(245,247,250), soft = X4_RGB(188,201,218);
    const uint32_t amber = X4_RGB(255,211,151);
    if (ui->help) {
        int reserved = ui->selected == X4_ACCESS_AUTO ? 52 : 160;
        x4_apple_draw_region_qr_block(p,x,y,w,h-reserved,x4_lang_current());
        if (ui->selected != X4_ACCESS_AUTO)
            x4_apple_text_wrap(p,x,y+h-133,21,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_WARNING),w,29,3,amber);
        x4_apple_draw_footer_hint(p,x,y+h-30,X4_PAD_CIRCLE,x4_tr(X4_TR_ACCESS_BACK));
        return;
    }
    if (phase == X4_ACCESS_CARD_TRYING) {
        char message[160];
        snprintf(message,sizeof(message),x4_tr(X4_TR_ACCESS_TRYING_FMT),x4_access_country_label(ui->selected));
        x4_apple_spinner(p,x,y+24,x4_apple_get_animation_phase());
        access_line(p,x+58,y+22,32,X4_FONT_SEMIBOLD,message,w-58,ink);
        if (ui->selected != X4_ACCESS_AUTO)
            x4_apple_text_wrap(p,x,y+118,22,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_WARNING),w,31,4,amber);
        x4_apple_draw_footer_hint(p,x,y+h-30,X4_PAD_CIRCLE,x4_tr(X4_TR_ACCESS_CANCEL));
        return;
    }
    if (ui->confirming) {
        access_line(p,x,y,32,X4_FONT_BOLD,x4_tr(X4_TR_ACCESS_CONFIRM_TITLE),w,ink);
        x4_apple_text_wrap(p,x,y+82,25,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_WARNING),w,36,4,amber);
        if (ui->save_error)
            x4_apple_text_wrap(p,x,y+h-145,22,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_SAVE_ERROR),w,30,2,amber);
        if (!settings) {
            int fx=x;
            fx+=x4_apple_draw_footer_hint(p,fx,y+h-30,X4_PAD_CROSS,x4_tr(X4_TR_ACCESS_CONTINUE))+42;
            x4_apple_draw_footer_hint(p,fx,y+h-30,X4_PAD_CIRCLE,x4_tr(X4_TR_ACCESS_CANCEL));
        }
        return;
    }
    access_line(p,x,y,settings?46:30,X4_FONT_BOLD,
        x4_tr(settings?X4_TR_ACCESS_COUNTRY:X4_TR_ACCESS_TITLE),w,ink);
    if (settings)
        access_line(p,x,y+62,25,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_NEXT_QUERY),w,soft);
    int sy=y+(settings?128:62);
    int label_width=settings?230:250;
    access_line(p,x,sy+17,25,X4_FONT_SEMIBOLD,x4_tr(X4_TR_ACCESS_COUNTRY),label_width,soft);
    int sw=w-label_width-32;
    if (sw>650) sw=650;
    int sx=x+label_width+32;
    x4_apple_fill_round_rect(p,sx,sy,sw,64,18,X4_RGBA(255,255,255,22));
    x4_apple_focus_ring(p,sx,sy,sw,64,18);
    access_line(p,sx+20,sy+15,29,X4_FONT_SEMIBOLD,"<",30,ink);
    access_line(p,sx+60,sy+15,29,X4_FONT_SEMIBOLD,x4_access_country_label(ui->selected),sw-120,ink);
    access_line(p,sx+sw-38,sy+15,29,X4_FONT_SEMIBOLD,">",30,ink);
    int dy=sy+96;
    x4_apple_text_wrap(p,x,dy,settings?25:23,X4_FONT_REGULAR,
        x4_tr(X4_TR_ACCESS_DESCRIPTION),w,settings?36:32,3,soft);
    if (ui->selected != X4_ACCESS_AUTO)
        x4_apple_text_wrap(p,x,dy+122,21,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_WARNING),w,29,3,amber);
    if (ui->save_error)
        x4_apple_text_wrap(p,x,y+h-105,20,X4_FONT_REGULAR,x4_tr(X4_TR_ACCESS_SAVE_ERROR),w,27,2,amber);
    if (!settings) {
        int fx=x;
        fx+=x4_apple_draw_footer_hint(p,fx,y+h-30,X4_PAD_CROSS,x4_tr(X4_TR_ACCESS_TRY))+42;
        fx+=x4_apple_draw_footer_hint(p,fx,y+h-30,X4_PAD_CIRCLE,x4_tr(X4_TR_ACCESS_BACK))+42;
        x4_apple_draw_footer_hint(p,fx,y+h-30,X4_PAD_TRIANGLE,x4_tr(X4_TR_ACCESS_HELP));
    }
}

void x4_apple_draw_access_card(uint32_t *p,int x,int y,int w,int h)
{
    X4AccessCountryUi ui; X4AccessCountryCardPhase phase;
    x4_access_country_card_get(&ui,&phase);
    x4_apple_draw_access_controls(p,x,y,w,h,&ui,phase,false);
}

void x4_apple_draw_access_success(uint32_t *p,int x,int y,int w)
{
    X4AccessCountryUi ui; X4AccessCountryCardPhase phase;
    x4_access_country_card_get(&ui,&phase);
    if(phase!=X4_ACCESS_CARD_SUCCESS)return;
    char message[160];
    snprintf(message,sizeof(message),x4_tr(X4_TR_ACCESS_SUCCESS_FMT),x4_access_country_label(ui.selected));
    x4_apple_fill_round_rect(p,x,y,w,52,18,X4_RGBA(18,57,39,235));
    access_line(p,x+20,y+13,23,X4_FONT_SEMIBOLD,message,w-40,X4_RGB(204,255,224));
}
void x4_apple_spinner(uint32_t *p, int x, int y, unsigned phase)
{
    for (unsigned i=0;i<12;++i) {
        float a=(float)i*6.28318530718f/12.0f;
        unsigned age=(i+12-phase%12)%12;
        x4_apple_fill_round_rect(p,x+16+(int)(cosf(a)*14),y+16+(int)(sinf(a)*14),
            5,5,2,X4_RGBA(240,245,255,55+(11-age)*17));
    }
}
/* Word wrapping measures the actual active Inter atlas. Emergency wrapping
 * of a single over-wide token advances by whole UTF-8 codepoints. */
int x4_apple_text_wrap(uint32_t *p,int x,int y,int size,int weight,
    const char *text,int width,int line_height,int max_lines,uint32_t color)
{
    if (!text || width<=0 || max_lines<=0) return 0;
    int lines=0;
    while (*text && lines<max_lines) {
        while (*text==' ') ++text;
        if (!*text) break;
        const char *start=text,*scan=text,*fit=text,*space=NULL;
        char line[1024];
        while (*scan && *scan!='\n') {
            const char *next=scan;
            (void)x4_utf8_next(&next);
            size_t n=(size_t)(next-start);
            if (n>=sizeof(line)) break;
            memcpy(line,start,n);line[n]=0;
            if (x4_text_width_px(size,weight,line)>width) break;
            if (*scan==' ') space=scan;
            fit=next;scan=next;
        }
        const char *end=fit;
        if (*scan && *scan!='\n' && space && space>start) end=space;
        if (end==start) { end=start;(void)x4_utf8_next(&end); }
        size_t n=(size_t)(end-start);
        while (n && start[n-1]==' ') --n;
        memcpy(line,start,n);line[n]=0;
        x4_text_px(p,x,y+lines*line_height,size,weight,line,color);
        ++lines;text=end;
        while (*text==' ') ++text;
        if (*text=='\n') ++text;
    }
    return lines;
}

/* One-time generated background and pre-blurred glass backing (480x270 = 1/4 res). */
static uint32_t *s_bg_full = NULL;
static uint32_t s_bg_blur_q[480 * 270];
static int s_bg_ready = 0;

static inline uint32_t blend_val(uint32_t dst, uint32_t src_rgba)
{
    unsigned a = (src_rgba >> 24) & 0xffu;
    if (a == 0u) return dst;
    if (a == 255u) return 0xff000000u | (src_rgba & 0x00ffffffu);
    unsigned inv = 255u - a;
    unsigned r = ((src_rgba & 0xffu) * a + (dst & 0xffu) * inv + 127u) / 255u;
    unsigned g = (((src_rgba >> 8) & 0xffu) * a + ((dst >> 8) & 0xffu) * inv + 127u) / 255u;
    unsigned b = (((src_rgba >> 16) & 0xffu) * a + ((dst >> 16) & 0xffu) * inv + 127u) / 255u;
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

void x4_apple_blend_pixel(uint32_t *dst, uint32_t src_rgba)
{
    if (!dst) return;
    *dst = blend_val(*dst, src_rgba);
}

static inline uint32_t lerp_rgb(uint32_t c0, uint32_t c1, unsigned t256)
{
    if (t256 == 0u) return c0;
    if (t256 >= 256u) return c1;
    unsigned inv = 256u - t256;
    unsigned r = ((c0 & 0xffu) * inv + (c1 & 0xffu) * t256) >> 8;
    unsigned g = (((c0 >> 8) & 0xffu) * inv + ((c1 >> 8) & 0xffu) * t256) >> 8;
    unsigned b = (((c0 >> 16) & 0xffu) * inv + ((c1 >> 16) & 0xffu) * t256) >> 8;
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

static void init_background_once(void)
{
    if (s_bg_ready) return;
    s_bg_full = (uint32_t *)malloc((size_t)X4_WIDTH * X4_HEIGHT * sizeof(uint32_t));
    static uint32_t q_raw[480 * 270];
    static uint32_t q_tmp[480 * 270];

    for (int qy = 0; qy < 270; ++qy) {
        float fy = (float)qy / 269.0f;
        /* Midnight blue vertical progression: #0B1020 -> #141C33 -> #090E1C */
        uint32_t top_c = X4_RGB(0x0B, 0x10, 0x20);
        uint32_t mid_c = X4_RGB(0x14, 0x1C, 0x33);
        uint32_t bot_c = X4_RGB(0x09, 0x0E, 0x1C);
        uint32_t base_v = (fy < 0.58f)
            ? lerp_rgb(top_c, mid_c, (unsigned)(fy / 0.58f * 256.0f))
            : lerp_rgb(mid_c, bot_c, (unsigned)((fy - 0.58f) / 0.42f * 256.0f));

        for (int qx = 0; qx < 480; ++qx) {
            float fx = (float)qx / 479.0f;
            float dx1 = fx - 0.24f, dy1 = fy - 0.22f;
            float d1 = dx1 * dx1 * 1.4f + dy1 * dy1 * 2.2f;
            float g1 = d1 < 0.32f ? (1.0f - d1 / 0.32f) : 0.0f;
            g1 = g1 * g1 * 0.45f;

            float dx2 = fx - 0.76f, dy2 = fy - 0.48f;
            float d2 = dx2 * dx2 * 1.2f + dy2 * dy2 * 1.8f;
            float g2 = d2 < 0.38f ? (1.0f - d2 / 0.38f) : 0.0f;
            g2 = g2 * g2 * 0.38f;

            uint32_t col = lerp_rgb(base_v, X4_RGB(0x20, 0x31, 0x5C), (unsigned)(g1 * 256.0f));
            col = lerp_rgb(col, X4_RGB(0x1A, 0x2A, 0x4E), (unsigned)(g2 * 256.0f));

            float ridge1 = 0.74f + 0.045f * sinf(fx * 5.2f + 0.6f) + 0.025f * cosf(fx * 11.0f);
            if (fy > ridge1) {
                float depth = (fy - ridge1) / (1.0f - ridge1);
                if (depth > 1.0f) depth = 1.0f;
                col = lerp_rgb(col, X4_RGB(0x08, 0x0C, 0x18), (unsigned)(depth * 180.0f));
            }
            q_raw[qy * 480 + qx] = col;
        }
    }

    /* Separable box blur (radius 12 on quarter-res = radius 48 on 1080p) */
    const int br = 12;
    for (int y = 0; y < 270; ++y) {
        for (int x = 0; x < 480; ++x) {
            unsigned sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int k = -br; k <= br; ++k) {
                int nx = x + k;
                if (nx < 0) nx = 0;
                if (nx >= 480) nx = 479;
                uint32_t c = q_raw[y * 480 + nx];
                sr += c & 0xffu;
                sg += (c >> 8) & 0xffu;
                sb += (c >> 16) & 0xffu;
                cnt++;
            }
            q_tmp[y * 480 + x] = X4_RGB(sr / cnt, sg / cnt, sb / cnt);
        }
    }
    for (int y = 0; y < 270; ++y) {
        for (int x = 0; x < 480; ++x) {
            unsigned sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int k = -br; k <= br; ++k) {
                int ny = y + k;
                if (ny < 0) ny = 0;
                if (ny >= 270) ny = 269;
                uint32_t c = q_tmp[ny * 480 + x];
                sr += c & 0xffu;
                sg += (c >> 8) & 0xffu;
                sb += (c >> 16) & 0xffu;
                cnt++;
            }
            uint32_t blurred = X4_RGB(sr / cnt, sg / cnt, sb / cnt);
            s_bg_blur_q[y * 480 + x] = lerp_rgb(blurred, X4_RGB(0x24, 0x32, 0x54), 32u);
        }
    }

    if (s_bg_full) {
        for (int y = 0; y < X4_HEIGHT; ++y) {
            int qy0 = y >> 2;
            int qy1 = qy0 + 1 < 270 ? qy0 + 1 : qy0;
            unsigned fy = (unsigned)(y & 3) * 64u;
            uint32_t *row = s_bg_full + (size_t)y * X4_WIDTH;
            for (int x = 0; x < X4_WIDTH; ++x) {
                int qx0 = x >> 2;
                int qx1 = qx0 + 1 < 480 ? qx0 + 1 : qx0;
                unsigned fx = (unsigned)(x & 3) * 64u;
                uint32_t top = lerp_rgb(q_raw[qy0 * 480 + qx0], q_raw[qy0 * 480 + qx1], fx);
                uint32_t bot = lerp_rgb(q_raw[qy1 * 480 + qx0], q_raw[qy1 * 480 + qx1], fx);
                uint32_t c = lerp_rgb(top, bot, fy);
                uint32_t h = (uint32_t)(x * 374761393u + y * 668265263u);
                h = (h ^ (h >> 13)) * 1274126177u;
                int dither = (int)(h & 3u) - 1;
                int r = (int)(c & 0xffu) + dither;
                int g = (int)((c >> 8) & 0xffu) + dither;
                int b = (int)((c >> 16) & 0xffu) + dither;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
                row[x] = X4_RGB(r, g, b);
            }
        }
    }
    s_bg_ready = 1;
}

void x4_apple_bg_draw(uint32_t *pixels)
{
    if (!pixels) return;
    init_background_once();
    if (s_bg_full) {
        memcpy(pixels, s_bg_full, (size_t)X4_WIDTH * X4_HEIGHT * sizeof(uint32_t));
    } else {
        for (int y = 0; y < X4_HEIGHT; ++y) {
            uint32_t c = s_bg_blur_q[(y >> 2) * 480];
            uint32_t *row = pixels + (size_t)y * X4_WIDTH;
            for (int x = 0; x < X4_WIDTH; ++x) row[x] = c;
        }
    }
}

static inline float sd_round_rect(float px, float py, float hw, float hh, float r)
{
    float qx = fabsf(px) - (hw - r);
    float qy = fabsf(py) - (hh - r);
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float outside = sqrtf(mx * mx + my * my);
    float inside = (qx > qy ? qx : qy);
    if (inside > 0.0f) inside = 0.0f;
    return outside + inside - r;
}

void x4_apple_fill_round_rect(uint32_t *pixels, int x, int y, int w, int h,
                              int radius, uint32_t rgba)
{
    if (!pixels || w <= 0 || h <= 0) return;
    unsigned base_a = (rgba >> 24) & 0xffu;
    if (!base_a) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > X4_WIDTH ? X4_WIDTH : x + w;
    int y1 = y + h > X4_HEIGHT ? X4_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    float rf = (float)radius;
    int mid_x0 = x + radius;
    int mid_x1 = x + w - radius;
    int mid_y0 = y + radius;
    int mid_y1 = y + h - radius;
    if (mid_x0 < x0) mid_x0 = x0;
    if (mid_x1 > x1) mid_x1 = x1;

    uint32_t rgb_only = rgba & 0x00ffffffu;
    for (int py = y0; py < y1; ++py) {
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        int in_mid_y = (py >= mid_y0 && py < mid_y1);
        float dy = (py + 0.5f) - cy;
        for (int px = x0; px < x1; ++px) {
            if (in_mid_y || (px >= mid_x0 && px < mid_x1)) {
                row[px] = blend_val(row[px], rgba);
                continue;
            }
            float dx = (px + 0.5f) - cx;
            float d = sd_round_rect(dx, dy, hw, hh, rf);
            if (d >= 0.5f) continue;
            unsigned a = base_a;
            if (d > -0.5f) {
                float cov = 0.5f - d;
                a = (unsigned)(base_a * cov + 0.5f);
            }
            row[px] = blend_val(row[px], rgb_only | (a << 24));
        }
    }
}

void x4_apple_stroke_round_rect(uint32_t *pixels, int x, int y, int w, int h,
                                int radius, float border_px, uint32_t rgba)
{
    if (!pixels || w <= 0 || h <= 0 || border_px <= 0.0f) return;
    unsigned base_a = (rgba >> 24) & 0xffu;
    if (!base_a) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int pad = (int)ceilf(border_px) + 2;
    int x0 = x - pad < 0 ? 0 : x - pad;
    int y0 = y - pad < 0 ? 0 : y - pad;
    int x1 = x + w + pad > X4_WIDTH ? X4_WIDTH : x + w + pad;
    int y1 = y + h + pad > X4_HEIGHT ? X4_HEIGHT : y + h + pad;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    float rf = (float)radius;
    float half_b = border_px * 0.5f;
    float center_d = -half_b;

    int inner_x0 = x + radius + pad;
    int inner_x1 = x + w - radius - pad;
    int inner_y0 = y + radius + pad;
    int inner_y1 = y + h - radius - pad;

    uint32_t rgb_only = rgba & 0x00ffffffu;
    for (int py = y0; py < y1; ++py) {
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        float dy = (py + 0.5f) - cy;
        int skip_mid = (py >= inner_y0 && py < inner_y1 && inner_x1 > inner_x0);
        for (int px = x0; px < x1; ++px) {
            if (skip_mid && px == inner_x0) {
                px = inner_x1 - 1;
                continue;
            }
            float dx = (px + 0.5f) - cx;
            float d = sd_round_rect(dx, dy, hw, hh, rf);
            float dist_stroke = fabsf(d - center_d) - half_b;
            if (dist_stroke >= 0.5f) continue;
            unsigned a = base_a;
            if (dist_stroke > -0.5f) {
                float cov = 0.5f - dist_stroke;
                a = (unsigned)(base_a * cov + 0.5f);
            }
            row[px] = blend_val(row[px], rgb_only | (a << 24));
        }
    }
}

void x4_apple_outer_glow(uint32_t *pixels, int x, int y, int w, int h,
                         int radius, int spread_px, uint32_t rgba)
{
    if (!pixels || w <= 0 || h <= 0 || spread_px <= 0) return;
    unsigned base_a = (rgba >> 24) & 0xffu;
    if (!base_a) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int x0 = x - spread_px < 0 ? 0 : x - spread_px;
    int y0 = y - spread_px < 0 ? 0 : y - spread_px;
    int x1 = x + w + spread_px > X4_WIDTH ? X4_WIDTH : x + w + spread_px;
    int y1 = y + h + spread_px > X4_HEIGHT ? X4_HEIGHT : y + h + spread_px;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    float rf = (float)radius;
    float sp = (float)spread_px;

    int inner_x0 = x + radius;
    int inner_x1 = x + w - radius;
    int inner_y0 = y + radius;
    int inner_y1 = y + h - radius;

    uint32_t rgb_only = rgba & 0x00ffffffu;
    for (int py = y0; py < y1; ++py) {
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        float dy = (py + 0.5f) - cy;
        int skip_mid = (py >= inner_y0 && py < inner_y1 && inner_x1 > inner_x0);
        for (int px = x0; px < x1; ++px) {
            if (skip_mid && px == inner_x0) {
                px = inner_x1 - 1;
                continue;
            }
            float dx = (px + 0.5f) - cx;
            float d = sd_round_rect(dx, dy, hw, hh, rf);
            if (d <= -1.0f || d >= sp) continue;
            float t = d < 0.0f ? 1.0f : (1.0f - d / sp);
            t = t * t;
            unsigned a = (unsigned)(base_a * t + 0.5f);
            if (a > 0u) {
                row[px] = blend_val(row[px], rgb_only | (a << 24));
            }
        }
    }
}

void x4_apple_vertical_gradient_round_rect(uint32_t *pixels, int x, int y, int w, int h,
                                           int radius, uint32_t top_rgba, uint32_t bot_rgba)
{
    if (!pixels || w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > X4_WIDTH ? X4_WIDTH : x + w;
    int y1 = y + h > X4_HEIGHT ? X4_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    float rf = (float)radius;
    int mid_x0 = x + radius;
    int mid_x1 = x + w - radius;
    int mid_y0 = y + radius;
    int mid_y1 = y + h - radius;

    unsigned top_a = (top_rgba >> 24) & 0xffu;
    unsigned bot_a = (bot_rgba >> 24) & 0xffu;

    for (int py = y0; py < y1; ++py) {
        unsigned t256 = h > 1 ? (unsigned)((py - y) * 256 / (h - 1)) : 0u;
        uint32_t row_rgb = lerp_rgb(top_rgba, bot_rgba, t256) & 0x00ffffffu;
        unsigned row_a = (top_a * (256u - t256) + bot_a * t256) >> 8;
        if (!row_a) continue;
        uint32_t row_rgba = row_rgb | (row_a << 24);
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        int in_mid_y = (py >= mid_y0 && py < mid_y1);
        float dy = (py + 0.5f) - cy;
        for (int px = x0; px < x1; ++px) {
            if (in_mid_y || (px >= mid_x0 && px < mid_x1)) {
                row[px] = blend_val(row[px], row_rgba);
                continue;
            }
            float dx = (px + 0.5f) - cx;
            float d = sd_round_rect(dx, dy, hw, hh, rf);
            if (d >= 0.5f) continue;
            unsigned a = row_a;
            if (d > -0.5f) a = (unsigned)(row_a * (0.5f - d) + 0.5f);
            row[px] = blend_val(row[px], row_rgb | (a << 24));
        }
    }
}

void x4_apple_focus_ring(uint32_t *pixels, int x, int y, int w, int h, int radius)
{
    x4_apple_outer_glow(pixels, x, y, w, h, radius, 14, X4_RGBA(0x90, 0xB8, 0xFF, 82));
    x4_apple_stroke_round_rect(pixels, x, y, w, h, radius, 3.0f, X4_RGBA(0xFF, 0xFF, 0xFF, 245));
}

void x4_apple_glass_panel(uint32_t *pixels, int x, int y, int w, int h,
                          int radius, unsigned fill_alpha, unsigned border_alpha)
{
    if (!pixels || w <= 0 || h <= 0) return;
    init_background_once();
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (fill_alpha > 255u) fill_alpha = 255u;
    if (border_alpha > 255u) border_alpha = 255u;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > X4_WIDTH ? X4_WIDTH : x + w;
    int y1 = y + h > X4_HEIGHT ? X4_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    float rf = (float)radius;
    int mid_x0 = x + radius;
    int mid_x1 = x + w - radius;
    int mid_y0 = y + radius;
    int mid_y1 = y + h - radius;

    for (int py = y0; py < y1; ++py) {
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        const uint32_t *blur_row = s_bg_blur_q + (size_t)(py >> 2) * 480;
        int in_mid_y = (py >= mid_y0 && py < mid_y1);
        float dy = (py + 0.5f) - cy;
        for (int px = x0; px < x1; ++px) {
            uint32_t blurred = blur_row[px >> 2];
            uint32_t frosted = blend_val(blurred, X4_RGBA(0xFF, 0xFF, 0xFF, fill_alpha));
            if (in_mid_y || (px >= mid_x0 && px < mid_x1)) {
                row[px] = frosted;
                continue;
            }
            float dx = (px + 0.5f) - cx;
            float d = sd_round_rect(dx, dy, hw, hh, rf);
            if (d >= 0.5f) continue;
            if (d <= -0.5f) {
                row[px] = frosted;
            } else {
                unsigned cov = (unsigned)((0.5f - d) * 255.0f + 0.5f);
                row[px] = blend_val(row[px], (frosted & 0x00ffffffu) | (cov << 24));
            }
        }
    }

    if (border_alpha > 0u) {
        x4_apple_stroke_round_rect(pixels, x, y, w, h, radius, 1.5f,
                                   X4_RGBA(0xFF, 0xFF, 0xFF, border_alpha));
    }
}

static void draw_icon_scaled(uint32_t *pixels, int x, int y, int size,
                             enum X4TablerIconId icon_id, uint32_t color)
{
    if (!pixels || size <= 0 || icon_id < 0 || icon_id >= X4_ICON_COUNT) return;
    unsigned base_a = (color >> 24) & 0xffu;
    if (!base_a) return;
    uint32_t rgb_only = color & 0x00ffffffu;

    const unsigned char *src = x4_tabler_icons_24[icon_id];
    int src_dim = 24;
    if (icon_id == X4_ICON_USER && size >= 40) {
        src = x4_tabler_user_56;
        src_dim = 56;
    }

    for (int dy = 0; dy < size; ++dy) {
        int py = y + dy;
        if (py < 0 || py >= X4_HEIGHT) continue;
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        int sy_fp = (dy * (src_dim - 1) << 8) / (size > 1 ? size - 1 : 1);
        int sy0 = sy_fp >> 8;
        int sy1 = sy0 + 1 < src_dim ? sy0 + 1 : sy0;
        unsigned fy = (unsigned)(sy_fp & 0xff);

        for (int dx = 0; dx < size; ++dx) {
            int px = x + dx;
            if (px < 0 || px >= X4_WIDTH) continue;
            unsigned a;
            if (size == src_dim) {
                a = src[dy * src_dim + dx];
            } else {
                int sx_fp = (dx * (src_dim - 1) << 8) / (size > 1 ? size - 1 : 1);
                int sx0 = sx_fp >> 8;
                int sx1 = sx0 + 1 < src_dim ? sx0 + 1 : sx0;
                unsigned fx = (unsigned)(sx_fp & 0xff);
                unsigned top = src[sy0 * src_dim + sx0] * (256u - fx) + src[sy0 * src_dim + sx1] * fx;
                unsigned bot = src[sy1 * src_dim + sx0] * (256u - fx) + src[sy1 * src_dim + sx1] * fx;
                a = (top * (256u - fy) + bot * fy + 32768u) >> 16;
            }
            a = (a * base_a) / 255u;
            if (a > 0u) {
                row[px] = blend_val(row[px], rgb_only | (a << 24));
            }
        }
    }
}

void x4_apple_draw_icon(uint32_t *pixels, int x, int y,
                        enum X4TablerIconId icon_id, uint32_t color)
{
    draw_icon_scaled(pixels, x, y, 24, icon_id, color);
}

void x4_apple_draw_user_large(uint32_t *pixels, int x, int y, uint32_t color)
{
    draw_icon_scaled(pixels, x, y, 56, X4_ICON_USER, color);
}

void x4_apple_pill_button(uint32_t *pixels, int x, int y, int w, int h,
                          const char *label, bool primary, bool focused,
                          enum X4TablerIconId icon)
{
    if (!pixels || w <= 0 || h <= 0) return;
    int r = h / 2;
    uint32_t txt_col;
    if (primary) {
        if (focused) {
            x4_apple_outer_glow(pixels, x, y, w, h, r, 12, X4_RGBA(0xFF, 0xFF, 0xFF, 96));
        }
        x4_apple_fill_round_rect(pixels, x, y, w, h, r,
                                 focused ? X4_RGBA(0xFF, 0xFF, 0xFF, 255)
                                         : X4_RGBA(0xF5, 0xF7, 0xFA, 248));
        if (focused) {
            x4_apple_focus_ring(pixels, x, y, w, h, r);
        }
        txt_col = X4_RGB(0x0B, 0x10, 0x20);
    } else {
        x4_apple_glass_panel(pixels, x, y, w, h, r,
                             focused ? 32u : 20u,
                             focused ? 84u : 38u);
        if (focused) {
            x4_apple_focus_ring(pixels, x, y, w, h, r);
        }
        txt_col = X4_RGB(0xF5, 0xF7, 0xFA);
    }

    if (!label || !label[0]) return;
    int font_sz = 24;
    int weight = X4_FONT_SEMIBOLD;
    int tw = x4_text_width_px(font_sz, weight, label);
    int has_icon = (icon >= 0 && icon < X4_ICON_COUNT);
    int has_chev = (primary && w <= 260);
    int total_w = tw + (has_icon ? 34 : 0) + (has_chev ? 26 : 0);
    int start_x = x + (w - total_w) / 2;
    if (start_x < x + 18) start_x = x + 18;
    int cur_x = start_x;
    if (has_icon) {
        draw_icon_scaled(pixels, cur_x, y + (h - 24) / 2, 24, icon, txt_col);
        cur_x += 34;
    }
    int max_tw = (x + w - 18) - cur_x - (has_chev ? 22 : 0);
    if (max_tw > 0) {
        x4_text_ellipsis_px(pixels, cur_x, y + (h - 28) / 2, font_sz, weight, label, max_tw, txt_col);
    }
    if (has_chev) {
        int chev_x = cur_x + (tw < max_tw ? tw : max_tw) + 4;
        if (chev_x + 20 <= x + w - 12) {
            draw_icon_scaled(pixels, chev_x, y + (h - 22) / 2, 22, X4_ICON_CHEVRON_RIGHT, txt_col);
        }
    }
}

static void blit_image_policy(uint32_t *pixels, int dst_x, int dst_y, int dst_w, int dst_h,
                                 int radius, const X4CoverRaster *raster,
                                 enum X4ImageGradientMode grad_mode, float max_crop)
{
    if (!pixels || !raster || !raster->rgba || dst_w <= 0 || dst_h <= 0 ||
        raster->width <= 0 || raster->height <= 0) return;
    const uint32_t *src_rgba = raster->rgba;
    int src_w = (int)raster->width;
    int src_h = (int)raster->height;
    if (radius < 0) radius = 0;
    if (radius * 2 > dst_w) radius = dst_w / 2;
    if (radius * 2 > dst_h) radius = dst_h / 2;

    /* Detect and trim solid letterbox/pillarbox pad bars (0xff30261du) from catalog_assets */
    int crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;
    const uint32_t pad_color = 0xff30261du;
    while (crop_h > 16) {
        const uint32_t *top_r = src_rgba + (size_t)crop_y * src_w;
        int all_pad = 1;
        for (int i = crop_x; i < crop_x + crop_w; i += 4) {
            if (top_r[i] != pad_color) { all_pad = 0; break; }
        }
        if (!all_pad) break;
        crop_y++;
        crop_h--;
    }
    while (crop_h > 16) {
        const uint32_t *bot_r = src_rgba + (size_t)(crop_y + crop_h - 1) * src_w;
        int all_pad = 1;
        for (int i = crop_x; i < crop_x + crop_w; i += 4) {
            if (bot_r[i] != pad_color) { all_pad = 0; break; }
        }
        if (!all_pad) break;
        crop_h--;
    }
    while (crop_w > 16) {
        int all_pad = 1;
        for (int j = crop_y; j < crop_y + crop_h; j += 4) {
            if (src_rgba[(size_t)j * src_w + crop_x] != pad_color) { all_pad = 0; break; }
        }
        if (!all_pad) break;
        crop_x++;
        crop_w--;
    }
    while (crop_w > 16) {
        int all_pad = 1;
        for (int j = crop_y; j < crop_y + crop_h; j += 4) {
            if (src_rgba[(size_t)j * src_w + (crop_x + crop_w - 1)] != pad_color) { all_pad = 0; break; }
        }
        if (!all_pad) break;
        crop_w--;
    }

    float dst_aspect = (float)dst_w / (float)dst_h;
    float src_aspect = (float)crop_w / (float)crop_h;
    float aspect_diff = src_aspect > dst_aspect ? 1.0f - dst_aspect / src_aspect
        : 1.0f - src_aspect / dst_aspect;

    int fit_x = dst_x, fit_y = dst_y, fit_w = dst_w, fit_h = dst_h;
    bool contain_mode = (aspect_diff > max_crop);

    if (!contain_mode) {
        if (src_aspect > dst_aspect) {
            int new_w = (int)(crop_h * dst_aspect + 0.5f);
            if (new_w > 0 && new_w < crop_w) {
                crop_x += (crop_w - new_w) / 2;
                crop_w = new_w;
            }
        } else if (dst_aspect > src_aspect) {
            int new_h = (int)(crop_w / dst_aspect + 0.5f);
            if (new_h > 0 && new_h < crop_h) {
                crop_y += (crop_h - new_h) / 2;
                crop_h = new_h;
            }
        }
    } else {
        if (src_aspect > dst_aspect) {
            fit_h = (int)((float)dst_w / src_aspect + 0.5f);
            if (fit_h < 1) fit_h = 1;
        } else {
            fit_w = (int)((float)dst_h * src_aspect + 0.5f);
            if (fit_w < 1) fit_w = 1;
        }
        fit_x = dst_x + (dst_w - fit_w) / 2;
        fit_y = dst_y + (dst_h - fit_h) / 2;
    }

    int x0 = dst_x < 0 ? 0 : dst_x;
    int y0 = dst_y < 0 ? 0 : dst_y;
    int x1 = dst_x + dst_w > X4_WIDTH ? X4_WIDTH : dst_x + dst_w;
    int y1 = dst_y + dst_h > X4_HEIGHT ? X4_HEIGHT : dst_y + dst_h;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = dst_x + dst_w * 0.5f;
    float cy = dst_y + dst_h * 0.5f;
    float hw = dst_w * 0.5f;
    float hh = dst_h * 0.5f;
    float rf = (float)radius;
    int mid_x0 = dst_x + radius;
    int mid_x1 = dst_x + dst_w - radius;
    int mid_y0 = dst_y + radius;
    int mid_y1 = dst_y + dst_h - radius;

    for (int py = y0; py < y1; ++py) {
        int dy_dst = py - dst_y;
        int sy_fp = (int)(((int64_t)dy_dst * (crop_h - 1) << 8) / (dst_h > 1 ? dst_h - 1 : 1));
        int sy0 = crop_y + (sy_fp >> 8);
        int sy1 = sy0 + 1 < crop_y + crop_h ? sy0 + 1 : sy0;
        unsigned fy = (unsigned)(sy_fp & 0xff);

        unsigned bot_dark = 0u;
        if (grad_mode == X4_GRAD_CARD_BOTTOM) {
            int start_y = (dst_h * 35) / 100;
            if (dy_dst > start_y) {
                bot_dark = (unsigned)((dy_dst - start_y) * 215 / (dst_h - start_y));
                if (bot_dark > 215u) bot_dark = 215u;
            }
        } else if (grad_mode == X4_GRAD_HERO_LEFT_BOTTOM) {
            int start_y = (dst_h * 45) / 100;
            if (dy_dst > start_y) {
                bot_dark = (unsigned)((dy_dst - start_y) * 195 / (dst_h - start_y));
                if (bot_dark > 195u) bot_dark = 195u;
            }
        }

        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        const uint32_t *srow0 = src_rgba + (size_t)sy0 * src_w;
        const uint32_t *srow1 = src_rgba + (size_t)sy1 * src_w;
        int in_mid_y = (py >= mid_y0 && py < mid_y1);
        float dy = (py + 0.5f) - cy;

        for (int px = x0; px < x1; ++px) {
            unsigned cov = 255u;
            if (!in_mid_y && !(px >= mid_x0 && px < mid_x1)) {
                float dx = (px + 0.5f) - cx;
                float d = sd_round_rect(dx, dy, hw, hh, rf);
                if (d >= 0.5f) continue;
                if (d > -0.5f) cov = (unsigned)((0.5f - d) * 255.0f + 0.5f);
            }

            int dx_dst = px - dst_x;
            uint32_t sample = 0;
            if (contain_mode && (px < fit_x || px >= fit_x + fit_w || py < fit_y || py >= fit_y + fit_h)) {
                int sx_fp = (int)(((int64_t)dx_dst * (crop_w - 1) << 8) / (dst_w > 1 ? dst_w - 1 : 1));
                int sx0 = crop_x + (sx_fp >> 8);
                int sx1 = sx0 + 1 < crop_x + crop_w ? sx0 + 1 : sx0;
                unsigned fx = (unsigned)(sx_fp & 0xff);

                uint32_t c00 = srow0[sx0], c10 = srow0[sx1];
                uint32_t c01 = srow1[sx0], c11 = srow1[sx1];
                uint32_t top = lerp_rgb(c00, c10, fx);
                uint32_t bot = lerp_rgb(c01, c11, fx);
                sample = lerp_rgb(top, bot, fy);
                sample = lerp_rgb(sample, X4_RGB(0x06, 0x0A, 0x14), 185);
            } else {
                int dx_in = contain_mode ? (px - fit_x) : dx_dst;
                int dy_in = contain_mode ? (py - fit_y) : dy_dst;
                int tw = contain_mode ? fit_w : dst_w;
                int th = contain_mode ? fit_h : dst_h;

                int sy_fp_in = (int)(((int64_t)dy_in * (crop_h - 1) << 8) / (th > 1 ? th - 1 : 1));
                int sy0_in = crop_y + (sy_fp_in >> 8);
                int sy1_in = sy0_in + 1 < crop_y + crop_h ? sy0_in + 1 : sy0_in;
                unsigned fy_in = (unsigned)(sy_fp_in & 0xff);

                int sx_fp_in = (int)(((int64_t)dx_in * (crop_w - 1) << 8) / (tw > 1 ? tw - 1 : 1));
                int sx0_in = crop_x + (sx_fp_in >> 8);
                int sx1_in = sx0_in + 1 < crop_x + crop_w ? sx0_in + 1 : sx0_in;
                unsigned fx_in = (unsigned)(sx_fp_in & 0xff);

                const uint32_t *r0 = src_rgba + (size_t)sy0_in * src_w;
                const uint32_t *r1 = src_rgba + (size_t)sy1_in * src_w;
                uint32_t c00 = r0[sx0_in], c10 = r0[sx1_in];
                uint32_t c01 = r1[sx0_in], c11 = r1[sx1_in];
                uint32_t top = lerp_rgb(c00, c10, fx_in);
                uint32_t bot = lerp_rgb(c01, c11, fx_in);
                sample = lerp_rgb(top, bot, fy_in);
            }

            unsigned dark = bot_dark;
            if (grad_mode == X4_GRAD_HERO_LEFT_BOTTOM) {
                int end_x = (dst_w * 62) / 100;
                if (dx_dst < end_x) {
                    unsigned left_dark = (unsigned)((end_x - dx_dst) * 200 / end_x);
                    dark = 255u - ((255u - dark) * (255u - left_dark) / 255u);
                }
            }
            if (dark > 0u) {
                sample = lerp_rgb(sample, X4_RGB(0x09, 0x0E, 0x1D), dark);
            }
            if (cov == 255u) {
                row[px] = sample;
            } else {
                row[px] = blend_val(row[px], (sample & 0x00ffffffu) | (cov << 24));
            }
        }
    }
}

void x4_apple_blit_image_rounded(uint32_t *p,int x,int y,int w,int h,int radius,
    const X4CoverRaster *r,enum X4ImageGradientMode g)
{ blit_image_policy(p,x,y,w,h,radius,r,g,0.05f); }
void x4_apple_blit_card_rounded(uint32_t *p,int x,int y,int w,int h,int radius,
    const X4CoverRaster *r,enum X4ImageGradientMode g)
{ blit_image_policy(p,x,y,w,h,radius,r,g,0.10f); }

void x4_apple_draw_hero_banner(uint32_t *pixels, int x, int y, int w, int h, int radius,
                               const X4CoverRaster *raster)
{
    if (!pixels || w <= 0 || h <= 0) return;
    x4_apple_glass_panel(pixels, x, y, w, h, radius, 24u, 44u);
    if (!raster || !raster->rgba || raster->width <= 0 || raster->height <= 0) return;

    int img_w = (int)((float)h * (16.0f / 9.0f) + 0.5f);
    if (img_w > w) img_w = w;
    int img_x = x + w - img_w;
    int img_y = y;

    const uint32_t *src_rgba = raster->rgba;
    int src_w = (int)raster->width;
    int src_h = (int)raster->height;

    int x0 = img_x < 0 ? 0 : img_x;
    int y0 = img_y < 0 ? 0 : img_y;
    int x1 = img_x + img_w > X4_WIDTH ? X4_WIDTH : img_x + img_w;
    int y1 = img_y + h > X4_HEIGHT ? X4_HEIGHT : img_y + h;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    float rf = (float)radius;

    for (int py = y0; py < y1; ++py) {
        int dy_dst = py - img_y;
        int sy_fp = (int)(((int64_t)dy_dst * (src_h - 1) << 8) / (h > 1 ? h - 1 : 1));
        int sy0 = sy_fp >> 8;
        int sy1 = sy0 + 1 < src_h ? sy0 + 1 : sy0;
        unsigned fy = (unsigned)(sy_fp & 0xff);

        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        const uint32_t *srow0 = src_rgba + (size_t)sy0 * src_w;
        const uint32_t *srow1 = src_rgba + (size_t)sy1 * src_w;
        float dy = (py + 0.5f) - cy;

        for (int px = x0; px < x1; ++px) {
            float dx = (px + 0.5f) - cx;
            float d = sd_round_rect(dx, dy, hw, hh, rf);
            if (d >= 0.5f) continue;
            unsigned cov = 255u;
            if (d > -0.5f) cov = (unsigned)((0.5f - d) * 255.0f + 0.5f);

            int dx_dst = px - img_x;
            int sx_fp = (int)(((int64_t)dx_dst * (src_w - 1) << 8) / (img_w > 1 ? img_w - 1 : 1));
            int sx0 = sx_fp >> 8;
            int sx1 = sx0 + 1 < src_w ? sx0 + 1 : sx0;
            unsigned fx = (unsigned)(sx_fp & 0xff);

            uint32_t c00 = srow0[sx0], c10 = srow0[sx1];
            uint32_t c01 = srow1[sx0], c11 = srow1[sx1];
            uint32_t top = lerp_rgb(c00, c10, fx);
            uint32_t bot = lerp_rgb(c01, c11, fx);
            uint32_t sample = lerp_rgb(top, bot, fy);

            int fade_w = 260;
            unsigned img_alpha = 255u;
            if (dx_dst < fade_w) {
                img_alpha = (unsigned)((dx_dst * 255) / fade_w);
            }
            unsigned final_cov = (cov * img_alpha) / 255u;
            if (final_cov > 0) {
                row[px] = blend_val(row[px], (sample & 0x00ffffffu) | (final_cov << 24));
            }
        }
    }
}

void x4_apple_procedural_art(uint32_t *pixels, int dst_x, int dst_y, int dst_w, int dst_h,
                             int radius, unsigned variant, enum X4ImageGradientMode grad_mode)
{
    (void)grad_mode;
    if (!pixels || dst_w <= 0 || dst_h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > dst_w) radius = dst_w / 2;
    if (radius * 2 > dst_h) radius = dst_h / 2;

    uint32_t sky_top, sky_mid, horizon_col, ridge_far, ridge_near;
    switch (variant & 3u) {
    case 0:
        sky_top = X4_RGB(0x13, 0x22, 0x3E);
        sky_mid = X4_RGB(0x28, 0x45, 0x68);
        horizon_col = X4_RGB(0xD8, 0x8A, 0x4E);
        ridge_far = X4_RGB(0x18, 0x28, 0x42);
        ridge_near = X4_RGB(0x0B, 0x13, 0x24);
        break;
    case 1:
        sky_top = X4_RGB(0x18, 0x1A, 0x40);
        sky_mid = X4_RGB(0x3B, 0x2D, 0x6E);
        horizon_col = X4_RGB(0x7C, 0x52, 0x95);
        ridge_far = X4_RGB(0x1E, 0x1B, 0x3A);
        ridge_near = X4_RGB(0x0E, 0x10, 0x22);
        break;
    case 2:
        sky_top = X4_RGB(0x0F, 0x29, 0x3A);
        sky_mid = X4_RGB(0x1E, 0x52, 0x66);
        horizon_col = X4_RGB(0x52, 0xA8, 0x96);
        ridge_far = X4_RGB(0x12, 0x31, 0x40);
        ridge_near = X4_RGB(0x09, 0x18, 0x24);
        break;
    default:
        sky_top = X4_RGB(0x24, 0x16, 0x32);
        sky_mid = X4_RGB(0x58, 0x2A, 0x46);
        horizon_col = X4_RGB(0xC8, 0x6D, 0x4A);
        ridge_far = X4_RGB(0x26, 0x16, 0x2C);
        ridge_near = X4_RGB(0x10, 0x0C, 0x1A);
        break;
    }

    int x0 = dst_x < 0 ? 0 : dst_x;
    int y0 = dst_y < 0 ? 0 : dst_y;
    int x1 = dst_x + dst_w > X4_WIDTH ? X4_WIDTH : dst_x + dst_w;
    int y1 = dst_y + dst_h > X4_HEIGHT ? X4_HEIGHT : dst_y + dst_h;
    if (x0 >= x1 || y0 >= y1) return;

    float cx = dst_x + dst_w * 0.5f;
    float cy = dst_y + dst_h * 0.5f;
    float hw = dst_w * 0.5f;
    float hh = dst_h * 0.5f;
    float rf = (float)radius;
    int mid_x0 = dst_x + radius;
    int mid_x1 = dst_x + dst_w - radius;
    int mid_y0 = dst_y + radius;
    int mid_y1 = dst_y + dst_h - radius;
    float phase = (float)(variant * 1.7f);

    for (int py = y0; py < y1; ++py) {
        float fy = (float)(py - dst_y) / (float)(dst_h > 1 ? dst_h - 1 : 1);
        uint32_t sky = fy < 0.52f
            ? lerp_rgb(sky_top, sky_mid, (unsigned)(fy / 0.52f * 256.0f))
            : lerp_rgb(sky_mid, horizon_col, (unsigned)((fy - 0.52f) / 0.48f * 256.0f));

        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        int in_mid_y = (py >= mid_y0 && py < mid_y1);
        float dy = (py + 0.5f) - cy;

        for (int px = x0; px < x1; ++px) {
            unsigned cov = 255u;
            if (!in_mid_y && !(px >= mid_x0 && px < mid_x1)) {
                float dx = (px + 0.5f) - cx;
                float d = sd_round_rect(dx, dy, hw, hh, rf);
                if (d >= 0.5f) continue;
                if (d > -0.5f) cov = (unsigned)((0.5f - d) * 255.0f + 0.5f);
            }

            float fx = (float)(px - dst_x) / (float)(dst_w > 1 ? dst_w - 1 : 1);
            uint32_t col = sky;

            float m1 = 0.50f - 0.12f * sinf(fx * 4.5f + phase) - 0.06f * cosf(fx * 9.5f - phase);
            if (fy > m1) col = ridge_far;
            float m2 = 0.64f - 0.09f * cosf(fx * 3.8f - phase * 0.7f) + 0.04f * sinf(fx * 8.2f);
            if (fy > m2) col = ridge_near;
            if (fy > 0.42f) {
                unsigned dark = (unsigned)((fy - 0.42f) / 0.58f * 210.0f);
                col = lerp_rgb(col, X4_RGB(0x08, 0x0D, 0x1A), dark);
            }
            if (cov == 255u) {
                row[px] = col;
            } else {
                row[px] = blend_val(row[px], (col & 0x00ffffffu) | (cov << 24));
            }
        }
    }
}

static void draw_aa_line(uint32_t *pixels, float x0, float y0, float x1, float y1,
                         float thickness, uint32_t rgba)
{
    unsigned base_a = (rgba >> 24) & 0xffu;
    if (!pixels || !base_a) return;
    uint32_t rgb_only = rgba & 0x00ffffffu;
    int min_x = (int)floorf((x0 < x1 ? x0 : x1) - thickness - 2.0f);
    int max_x = (int)ceilf((x0 > x1 ? x0 : x1) + thickness + 2.0f);
    int min_y = (int)floorf((y0 < y1 ? y0 : y1) - thickness - 2.0f);
    int max_y = (int)ceilf((y0 > y1 ? y0 : y1) + thickness + 2.0f);
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > X4_WIDTH) max_x = X4_WIDTH;
    if (max_y > X4_HEIGHT) max_y = X4_HEIGHT;

    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    float half_t = thickness * 0.5f;

    for (int py = min_y; py < max_y; ++py) {
        uint32_t *row = pixels + (size_t)py * X4_WIDTH;
        for (int px = min_x; px < max_x; ++px) {
            float wx = (px + 0.5f) - x0;
            float wy = (py + 0.5f) - y0;
            float t = len2 > 1e-6f ? (wx * vx + wy * vy) / len2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;
            float dx = wx - t * vx;
            float dy = wy - t * vy;
            float d = sqrtf(dx * dx + dy * dy) - half_t;
            if (d >= 0.5f) continue;
            unsigned a = base_a;
            if (d > -0.5f) a = (unsigned)(base_a * (0.5f - d) + 0.5f);
            row[px] = blend_val(row[px], rgb_only | (a << 24));
        }
    }
}

int x4_apple_draw_pad_glyph(uint32_t *pixels, int x, int y,
                            enum X4PadGlyph glyph)
{
    uint32_t color = X4_RGB(0xE2, 0xE8, 0xF0);
    if (glyph <= X4_GLYPH_SQUARE) {
        const int d = 28;
        float cx = x + d * 0.5f;
        float cy = y + d * 0.5f;
        x4_apple_fill_round_rect(pixels, x, y, d, d, d / 2, X4_RGBA(0xFF, 0xFF, 0xFF, 28));
        x4_apple_stroke_round_rect(pixels, x, y, d, d, d / 2, 1.6f, color);
        if (glyph == X4_GLYPH_CROSS) {
            draw_aa_line(pixels, cx - 4.8f, cy - 4.8f, cx + 4.8f, cy + 4.8f, 2.0f, color);
            draw_aa_line(pixels, cx + 4.8f, cy - 4.8f, cx - 4.8f, cy + 4.8f, 2.0f, color);
        } else if (glyph == X4_GLYPH_CIRCLE) {
            x4_apple_stroke_round_rect(pixels, x + 7, y + 7, 14, 14, 7, 2.0f, color);
        } else if (glyph == X4_GLYPH_TRIANGLE) {
            draw_aa_line(pixels, cx, cy - 5.5f, cx + 5.8f, cy + 4.8f, 1.9f, color);
            draw_aa_line(pixels, cx + 5.8f, cy + 4.8f, cx - 5.8f, cy + 4.8f, 1.9f, color);
            draw_aa_line(pixels, cx - 5.8f, cy + 4.8f, cx, cy - 5.5f, 1.9f, color);
        } else if (glyph == X4_GLYPH_SQUARE) {
            x4_apple_stroke_round_rect(pixels, x + 8, y + 8, 12, 12, 2, 2.0f, color);
        }
        return d;
    }

    const char *txt = "OPTIONS";
    if (glyph == X4_GLYPH_L1) txt = "L1";
    else if (glyph == X4_GLYPH_R1) txt = "R1";
    else if (glyph == X4_GLYPH_L2) txt = "L2";
    else if (glyph == X4_GLYPH_R2) txt = "R2";

    int tw = x4_text_width_px(18, X4_FONT_SEMIBOLD, txt);
    int pw = tw + 20;
    int ph = 28;
    x4_apple_fill_round_rect(pixels, x, y, pw, ph, 8, X4_RGBA(0xFF, 0xFF, 0xFF, 28));
    x4_apple_stroke_round_rect(pixels, x, y, pw, ph, 8, 1.5f, color);
    x4_text_px(pixels, x + 10, y + 4, 18, X4_FONT_SEMIBOLD, txt, color);
    return pw;
}

int x4_apple_draw_footer_hint(uint32_t *pixels, int x, int y,
                              enum X4PadGlyph glyph, const char *label)
{
    int gw = x4_apple_draw_pad_glyph(pixels, x, y, glyph);
    int tx = x + gw + 10;
    x4_text_px(pixels, tx, y + 2, 22, X4_FONT_REGULAR, label ? label : "", X4_RGB(0xC5, 0xD0, 0xE0));
    int tw = x4_text_width_px(22, X4_FONT_REGULAR, label ? label : "");
    return gw + 10 + tw;
}

void x4_apple_draw_brand_logo(uint32_t *pixels, int x, int y)
{
    const char *brand = "XCloud4";
    uint32_t white = X4_RGB(0xF5, 0xF7, 0xFA);
    uint32_t green = X4_RGB(0x4A, 0xDE, 0x80);
    x4_text_px(pixels, x, y, 32, X4_FONT_BOLD, brand, white);
    int bw = x4_text_width_px(32, X4_FONT_BOLD, brand);
    draw_aa_line(pixels, (float)(x + bw + 12), (float)(y + 6),
                 (float)(x + bw + 4), (float)(y + 32), 3.4f, green);
}

void x4_apple_draw_account_pill(uint32_t *pixels, int x, int y, int h,
                                const char *label, bool signed_in, bool focused)
{
    int tw = x4_text_width_px(22, X4_FONT_SEMIBOLD, label ? label : "");
    int w = tw + 74;
    if (w < 190) w = 190;
    if (w > 340) w = 340;

    x4_apple_glass_panel(pixels, x, y, w, h, h / 2,
                         focused ? 32u : 20u,
                         focused ? 84u : 38u);
    if (focused) {
        x4_apple_focus_ring(pixels, x, y, w, h, h / 2);
    }

    uint32_t icon_col = signed_in ? X4_RGB(0x4A, 0xDE, 0x80) : X4_RGB(0xD8, 0xE2, 0xF2);
    uint32_t txt_col = X4_RGB(0xF5, 0xF7, 0xFA);
    int c_sz = h - 14;
    int cx = x + 8;
    int cy = y + 7;
    x4_apple_fill_round_rect(pixels, cx, cy, c_sz, c_sz, c_sz / 2, X4_RGBA(0xFF, 0xFF, 0xFF, 28));
    draw_icon_scaled(pixels, cx + (c_sz - 20) / 2, cy + (c_sz - 20) / 2, 20, X4_ICON_USER, icon_col);
    int tx = cx + c_sz + 10;
    int max_tw = w - (tx - x) - 16;
    x4_text_ellipsis_px(pixels, tx, y + (h - 26) / 2, 22, X4_FONT_SEMIBOLD,
                        label ? label : "", max_tw, txt_col);
}

const char *x4_region_qr_url(enum X4Lang lang)
{
    return (lang == X4_LANG_EN)
        ? "https://github.com/asp11edgr/XCloud4/blob/main/docs/region-en.md"
        : "https://github.com/asp11edgr/XCloud4/blob/main/docs/region-es.md";
}

const char *x4_region_qr_short_label(enum X4Lang lang)
{
    return (lang == X4_LANG_EN)
        ? "github.com/asp11edgr/XCloud4 \xe2\x80\xba docs \xe2\x80\xba region-en.md"
        : "github.com/asp11edgr/XCloud4 \xe2\x80\xba docs \xe2\x80\xba region-es.md";
}

const char *x4_region_qr_caption(enum X4Lang lang)
{
    return (lang == X4_LANG_EN)
        ? "Scan the QR to see how to play if your country doesn't have Xbox Cloud Gaming."
        : "Escanea el QR para ver c\xc3\xb3mo jugar si tu pa\xc3\xads no tiene Xbox Cloud Gaming.";
}

void x4_apple_draw_region_qr_block(uint32_t *pixels, int x, int y, int w, int h,
                                   enum X4Lang lang)
{
    if (!pixels || w <= 0 || h <= 0) return;
    x4_apple_glass_panel(pixels, x, y, w, h, 20, 22u, 42u);

    static uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];
    const char *url = x4_region_qr_url(lang);
    bool ok = qrcodegen_encodeText(url, qr_temp, qr_data, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);
    int qr_box = h - 32;
    if (qr_box > 296) qr_box = 296;
    if (qr_box < 96) qr_box = h - 16;
    int qx = x + 24;
    int qy = y + (h - qr_box) / 2;

    x4_apple_fill_round_rect(pixels, qx, qy, qr_box, qr_box, 16, X4_RGB(0xFA, 0xFC, 0xFF));
    if (ok) {
        int qr_size = qrcodegen_getSize(qr_data);
        int scale = (qr_box - 20) / (qr_size > 0 ? qr_size : 1);
        if (scale < 2) scale = 2;
        int total_px = qr_size * scale;
        int ox = qx + (qr_box - total_px) / 2;
        int oy = qy + (qr_box - total_px) / 2;
        uint32_t dark = X4_RGB(0x0B, 0x10, 0x20);
        for (int ry = 0; ry < qr_size; ++ry) {
            for (int rx = 0; rx < qr_size; ++rx) {
                if (!qrcodegen_getModule(qr_data, rx, ry)) continue;
                for (int dy = 0; dy < scale; ++dy) {
                    int py = oy + ry * scale + dy;
                    if (py < 0 || py >= X4_HEIGHT) continue;
                    uint32_t *row = pixels + (size_t)py * X4_WIDTH;
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = ox + rx * scale + dx;
                        if (px >= 0 && px < X4_WIDTH) row[px] = dark;
                    }
                }
            }
        }
    }

    int tx = qx + qr_box + 36;
    int tw = (x + w - 32) - tx;
    if (tw <= 40) return;

    const char *caption = x4_region_qr_caption(lang);
    const char *short_lbl = x4_region_qr_short_label(lang);

    static char line1[256];
    static char line2[256];
    line1[0] = 0;
    line2[0] = 0;
    int ty = y + (h - 96) / 2;
    if (x4_text_width_px(26, X4_FONT_SEMIBOLD, caption) <= tw) {
        x4_text_px(pixels, tx, ty + 12, 26, X4_FONT_SEMIBOLD, caption, X4_RGB(0xF5, 0xF7, 0xFA));
        x4_text_ellipsis_px(pixels, tx, ty + 56, 22, X4_FONT_REGULAR, short_lbl, tw, X4_RGB(0x94, 0xA3, 0xB8));
    } else {
        size_t len = strlen(caption);
        size_t split = len;
        for (size_t i = 1; i < len; ++i) {
            if (caption[i] == ' ') {
                size_t copy_n = i < sizeof(line1) - 1 ? i : sizeof(line1) - 1;
                memcpy(line1, caption, copy_n);
                line1[copy_n] = 0;
                if (x4_text_width_px(26, X4_FONT_SEMIBOLD, line1) <= tw) {
                    split = i;
                } else {
                    break;
                }
            }
        }
        size_t n1 = split < sizeof(line1) - 1 ? split : sizeof(line1) - 1;
        memcpy(line1, caption, n1);
        line1[n1] = 0;
        const char *rest = caption + split;
        while (*rest == ' ') rest++;
        snprintf(line2, sizeof(line2), "%s", rest);
        x4_text_ellipsis_px(pixels, tx, ty, 26, X4_FONT_SEMIBOLD, line1, tw, X4_RGB(0xF5, 0xF7, 0xFA));
        x4_text_ellipsis_px(pixels, tx, ty + 36, 26, X4_FONT_SEMIBOLD, line2, tw, X4_RGB(0xF5, 0xF7, 0xFA));
        x4_text_ellipsis_px(pixels, tx, ty + 76, 22, X4_FONT_REGULAR, short_lbl, tw, X4_RGB(0x94, 0xA3, 0xB8));
    }
}
