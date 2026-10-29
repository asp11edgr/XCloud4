/* SPDX-License-Identifier: GPL-3.0-only */
#include "picture_settings.h"
#include "apple_ui.h"
#include "canvas.h"
#include "font.h"
#include "lang.h"
#include "server_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define x4_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define x4_mkdir(p) mkdir(p, 0777)
#endif

#define X4_PIC_SETTINGS_MAGIC 0x31533458u /* "X4S1" */
#define X4_PIC_SETTINGS_VERSION 1u
#define X4_PIC_SETTINGS_DEFAULT_PATH "/data/xcloud4/settings-public-v1.bin"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t quality;
    uint32_t checksum;
} X4PictureSettingsRecord;

static char custom_settings_path[256] = {0};
static _Atomic unsigned active_quality = X4_PIC_QUALITY_1080P_ALTA;

enum { SETTINGS_W=1920, SETTINGS_H=1080,
       SETTINGS_X=100, SETTINGS_Y=126, SETTINGS_PANEL_W=1720, SETTINGS_PANEL_H=836 };
static struct {
    uint32_t *backdrop;
    const X4PictureSettingsUi *owner;
    uint32_t *canvas;
    enum X4Lang language;
    bool first_time, access_footer;
    X4PictureSettingsCacheStats stats;
} settings_cache;

void x4_picture_settings_ui_cache_invalidate(void)
{
    free(settings_cache.backdrop);
    settings_cache.backdrop=NULL;
    settings_cache.owner=NULL;
    settings_cache.canvas=NULL;
    settings_cache.stats.ready=false;
    settings_cache.stats.cache_bytes=0;
    settings_cache.access_footer=false;
}

bool x4_picture_settings_ui_cache_ready(const X4PictureSettingsUi *s)
{
    return s&&s->open&&settings_cache.backdrop&&settings_cache.owner==s&&
        settings_cache.first_time==s->first_time_mode&&settings_cache.language==x4_lang_current();
}

void x4_picture_settings_ui_cache_snapshot(X4PictureSettingsCacheStats *out)
{
    if(out)*out=settings_cache.stats;
}

static uint32_t calc_checksum(uint32_t magic, uint32_t version, uint32_t quality)
{
    return magic ^ (version << 16) ^ (version >> 16) ^ (quality * 0x9e3779b9u) ^ 0xa5a55a5au;
}

static const char *get_settings_path(void)
{
    return custom_settings_path[0] ? custom_settings_path : X4_PIC_SETTINGS_DEFAULT_PATH;
}

void x4_picture_settings_set_path(const char *path)
{
    if (path && path[0]) {
        snprintf(custom_settings_path, sizeof(custom_settings_path), "%s", path);
    } else {
        custom_settings_path[0] = 0;
    }
}

static X4PictureQualityOption load_from_disk(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return X4_PIC_QUALITY_1080P_ALTA;
    X4PictureSettingsRecord rec;
    size_t n = fread(&rec, 1, sizeof(rec), f);
    fclose(f);
    if (n != sizeof(rec)) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.magic != X4_PIC_SETTINGS_MAGIC) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.version != X4_PIC_SETTINGS_VERSION) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.quality >= X4_PIC_QUALITY_COUNT) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.checksum != calc_checksum(rec.magic, rec.version, rec.quality)) return X4_PIC_QUALITY_1080P_ALTA;
    return (X4PictureQualityOption)rec.quality;
}

static void ensure_parent_dirs(const char *path)
{
    char dir[256];
    size_t len = strlen(path);
    if (len >= sizeof(dir)) return;
    memcpy(dir, path, len + 1);
    char *slash = strrchr(dir, '/');
#if defined(_WIN32)
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash && slash != dir) {
        *slash = '\0';
        if (strncmp(dir, "/data/xcloud4", 13) == 0) {
            (void)x4_mkdir("/data");
        }
        (void)x4_mkdir(dir);
    }
}

static bool save_to_disk(const char *path, X4PictureQualityOption quality)
{
    ensure_parent_dirs(path);
    char tmp[270];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    X4PictureSettingsRecord rec;
    rec.magic = X4_PIC_SETTINGS_MAGIC;
    rec.version = X4_PIC_SETTINGS_VERSION;
    rec.quality = (uint32_t)quality;
    rec.checksum = calc_checksum(rec.magic, rec.version, rec.quality);
    size_t written = fwrite(&rec, 1, sizeof(rec), f);
    fflush(f);
    fclose(f);
    if (written != sizeof(rec)) {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

void x4_picture_settings_init(void)
{
    x4_access_country_init();
    X4PictureQualityOption loaded = load_from_disk(get_settings_path());
    atomic_store_explicit(&active_quality, (unsigned)loaded, memory_order_release);
}

X4PictureQualityOption x4_picture_settings_get(void)
{
    return (X4PictureQualityOption)atomic_load_explicit(&active_quality, memory_order_acquire);
}

bool x4_picture_settings_set(X4PictureQualityOption quality)
{
    if ((unsigned)quality >= X4_PIC_QUALITY_COUNT) return false;
    atomic_store_explicit(&active_quality, (unsigned)quality, memory_order_release);
    return save_to_disk(get_settings_path(), quality);
}

X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption opt)
{
    switch (opt) {
    case X4_PIC_QUALITY_1080P_ALTA:
        return X4_REQUEST_1080P_G;
    case X4_PIC_QUALITY_1080P:
        return X4_REQUEST_1080P_G8;
    case X4_PIC_QUALITY_720P:
        return X4_REQUEST_720P_HQ;
    default:
        return X4_REQUEST_1080P_G;
    }
}

X4StreamRequestProfile x4_picture_settings_current_profile(void)
{
    return x4_picture_settings_to_profile(x4_picture_settings_get());
}

const char *x4_picture_settings_active_quality_label(void)
{
    X4PictureQualityOption q = x4_picture_settings_get();
    switch (q) {
    case X4_PIC_QUALITY_1080P_ALTA:
        return x4_tr(X4_TR_PIC_SETTINGS_BANNER_1080P_ALTA);
    case X4_PIC_QUALITY_1080P:
        return x4_tr(X4_TR_PIC_SETTINGS_BANNER_1080P);
    case X4_PIC_QUALITY_720P:
        return x4_tr(X4_TR_PIC_SETTINGS_BANNER_720P);
    default:
        return x4_tr(X4_TR_PIC_SETTINGS_BANNER_1080P_ALTA);
    }
}

void x4_picture_settings_open_server_first_time(X4PictureSettingsUi *s)
{
    if (!s) return;
    x4_picture_settings_ui_cache_invalidate();
    s->open = true;
    s->tab = 1;
    s->first_time_mode = true;
    s->server_selected = 0; /* Automático */
    s->server_scroll_offset = 0;
    s->server_changed = false;
}

bool x4_picture_settings_ui_handle(X4PictureSettingsUi *s, unsigned a, bool can_edit)
{
    if (!s) return false;
    if (!can_edit && !s->first_time_mode) {
        bool consumed = s->open;
        s->open = false;
        x4_picture_settings_ui_cache_invalidate();
        return consumed;
    }
    if (!s->open) {
        if (!(a & X4_PIC_SETTINGS_OPEN)) return false;
        x4_picture_settings_ui_cache_invalidate();
        s->tab = 0;
        s->first_time_mode = false;
        s->server_changed = false;
        s->access_changed = false;
        x4_access_country_ui_open(&s->access_ui);
        s->selected = (unsigned)x4_picture_settings_get();
        if (s->selected >= X4_PIC_QUALITY_COUNT) s->selected = 0;

        /* Initialize server selection */
        const char *saved_server = x4_server_settings_get_selected();
        s->server_selected = (unsigned)x4_server_settings_find_index(saved_server);
        s->server_scroll_offset = 0;
        s->open = true;
        return true;
    }

    /* Cancel or Re-open toggle */
    if ((a & X4_PIC_SETTINGS_CANCEL) || (!s->first_time_mode && (a & X4_PIC_SETTINGS_OPEN))) {
        if (s->first_time_mode) {
            x4_server_settings_notify_choice_done();
            s->first_time_mode = false;
        }
        s->open = false;
        x4_picture_settings_ui_cache_invalidate();
        return true;
    }

    /* Tab switching (only in normal mode) */
    if (!s->first_time_mode) {
        if (s->tab == 2 && s->access_ui.confirming) {
            unsigned actions = (a & X4_PIC_SETTINGS_CONFIRM) ? X4_ACCESS_UI_CONFIRM : 0u;
            if (x4_access_country_ui_handle(&s->access_ui, actions, false, "ajustes") == X4_ACCESS_UI_APPLIED) {
                s->access_changed = true;
                s->open = false;
                x4_picture_settings_ui_cache_invalidate();
            }
            return true;
        }
        if ((a & X4_PIC_SETTINGS_PREV_TAB) && s->tab > 0) {
            s->tab--;
            return true;
        }
        if ((a & X4_PIC_SETTINGS_NEXT_TAB) && s->tab < 2) {
            s->tab++;
            return true;
        }
    }

    if (s->tab == 2 && !s->first_time_mode) {
        unsigned actions = 0;
        if (a & X4_PIC_SETTINGS_LEFT) actions |= X4_ACCESS_UI_LEFT;
        if (a & X4_PIC_SETTINGS_RIGHT) actions |= X4_ACCESS_UI_RIGHT;
        if (a & X4_PIC_SETTINGS_CONFIRM) actions |= X4_ACCESS_UI_CONFIRM;
        if (x4_access_country_ui_handle(&s->access_ui, actions, false, "ajustes") == X4_ACCESS_UI_APPLIED) {
            s->access_changed = true;
            s->open = false;
            x4_picture_settings_ui_cache_invalidate();
        }
        return true;
    }

    if (s->tab == 0) {
        if (a & X4_PIC_SETTINGS_UP) {
            s->selected = (s->selected + X4_PIC_QUALITY_COUNT - 1u) % X4_PIC_QUALITY_COUNT;
            return true;
        }
        if (a & X4_PIC_SETTINGS_DOWN) {
            s->selected = (s->selected + 1u) % X4_PIC_QUALITY_COUNT;
            return true;
        }
        if (a & X4_PIC_SETTINGS_CONFIRM) {
            x4_picture_settings_set((X4PictureQualityOption)s->selected);
            s->open = false;
            x4_picture_settings_ui_cache_invalidate();
            return true;
        }
    } else {
        unsigned count = x4_server_settings_get_region_count();
        unsigned total_options = 1 + count;

        if (a & X4_PIC_SETTINGS_UP) {
            s->server_selected = (s->server_selected + total_options - 1u) % total_options;
            return true;
        }
        if (a & X4_PIC_SETTINGS_DOWN) {
            s->server_selected = (s->server_selected + 1u) % total_options;
            return true;
        }
        if (a & X4_PIC_SETTINGS_CONFIRM) {
            char choice[64] = {0};
            if (s->server_selected > 0 && s->server_selected <= count) {
                x4_server_settings_get_region_name(s->server_selected - 1, choice, sizeof(choice));
            }
            const char *prev = x4_server_settings_get_selected();
            s->server_changed = (strcmp(prev ? prev : "", choice) != 0);
            x4_server_settings_set_selected(choice, true);
            x4_server_settings_clear_not_available();
            x4_server_settings_notify_choice_done();
            s->first_time_mode = false;
            s->open = false;
            x4_picture_settings_ui_cache_invalidate();
            return true;
        }
    }
    return true;
}

/* TAREA 20 changes presentation only. Input ownership and persistence above
 * remain the TAREA 19b implementation. All labels are measured before drawing. */
static const uint32_t white = 0xfff7f7f7u;
static const uint32_t muted = 0xffc9bcadu;

static const char *local(const char *es, const char *en)
{
    return x4_lang_current() == X4_LANG_EN ? en : es;
}

static void line(uint32_t *p, int x, int y, int size, int weight,
                 const char *text, int width, uint32_t color)
{
    x4_text_ellipsis_px(p, x, y, size, weight, text, width, color);
}

static void wrapped(uint32_t *p, int x, int y, int size, const char *text,
                    int width, unsigned max_lines, uint32_t color)
{
    x4_apple_text_wrap(p, x, y, size, X4_FONT_REGULAR, text, width,
                      size + 10, (int)max_lines, color);
}

static void radio(uint32_t *p, int x, int y, bool checked)
{
    x4_apple_stroke_round_rect(p, x, y, 30, 30, 15, 2.0f,
                              checked ? white : 0xffa9a299u);
    if (checked) x4_apple_fill_round_rect(p, x + 7, y + 7, 16, 16, 8, white);
}

static void badge(uint32_t *p, int x, int y, const char *text, int available)
{
    int width = x4_text_width_px(18, X4_FONT_SEMIBOLD, text) + 24;
    if (width > available) width = available;
    x4_apple_fill_round_rect(p, x, y, width, 32, 16, X4_RGBA(255, 255, 255, 25));
    line(p, x + 12, y + 5, 18, X4_FONT_SEMIBOLD, text, width - 24, white);
}

static void settings_sidebar(uint32_t *p, unsigned tab)
{
    line(p, 144, 176, 44, X4_FONT_BOLD, local("Ajustes", "Settings"), 252, white);
    const enum X4TextId labels[3] = {X4_TR_OPTIONS_TAB_PICTURE, X4_TR_OPTIONS_TAB_SERVER, X4_TR_ACCESS_TAB};
    const enum X4TablerIconId icons[3] = {X4_ICON_DEVICE_DESKTOP, X4_ICON_SERVER, X4_ICON_INFO};
    for (unsigned i = 0; i < 3; ++i) {
        int y = 300 + (int)i * 104;
        if (i == tab) {
            x4_apple_fill_round_rect(p, 132, y, 266, 82, 22, X4_RGBA(255,255,255,28));
            x4_apple_stroke_round_rect(p, 132, y, 266, 82, 22, 1.0f, X4_RGBA(255,255,255,52));
        }
        x4_apple_draw_icon(p, 154, y + 25, icons[i], i == tab ? white : muted);
        line(p, 204, y + 25, 27, i == tab ? X4_FONT_SEMIBOLD : X4_FONT_REGULAR,
             x4_tr(labels[i]), 180, i == tab ? white : muted);
    }
    x4_apple_fill_round_rect(p, 422, 170, 1, 742, 0, X4_RGBA(255,255,255,32));
}

static void settings_footer_buttons(uint32_t *p, int x)
{
    x4_apple_pill_button(p, x, 852, 236, 62, local("Guardar", "Save"),
                         true, false, X4_ICON_NONE);
    x4_apple_pill_button(p, x + 260, 852, 236, 62, local("Cancelar", "Cancel"),
                         false, false, X4_ICON_NONE);
}

static void settings_footer_hints(uint32_t *p, bool first_time)
{
    int hx = 150;
    if (!first_time) {
        hx += x4_apple_draw_pad_glyph(p, hx, 993, X4_GLYPH_L1) + 12;
        hx += x4_apple_draw_footer_hint(p, hx, 993, X4_GLYPH_R1,
                                       local("Cambiar sección", "Change section")) + 42;
    }
    hx += x4_apple_draw_footer_hint(p, hx, 993, X4_GLYPH_CROSS,
                                   local("Guardar", "Save")) + 42;
    x4_apple_draw_footer_hint(p, hx, 993, X4_GLYPH_CIRCLE, local("Cancelar", "Cancel"));
}

static void settings_footer(uint32_t *p, int x, bool first_time)
{
    settings_footer_buttons(p,x);
    settings_footer_hints(p,first_time);
}

static void picture_rows(const X4PictureSettingsUi *s, uint32_t *p)
{
    const char *labels[3] = {local("1080p Alta", "1080p High"), "1080p", "720p"};
    const char *descs_es[3] = {
        "1920x1080 a 60 fps, unos 10-13 Mbps. La mejor nitidez.",
        "Hasta 8 Mbps. Usa menos internet.",
        "1280x720 a 60 fps, unos 7 Mbps. Menos retraso."
    };
    const char *descs_en[3] = {
        "1920x1080 at 60 fps, about 10-13 Mbps. The sharpest picture.",
        "Up to 8 Mbps. Uses less bandwidth.",
        "1280x720 at 60 fps, about 7 Mbps. Less delay."
    };
    line(p, 470, 184, 46, X4_FONT_BOLD, local("Calidad de imagen", "Picture quality"), 1250, white);
    line(p, 470, 246, 25, X4_FONT_REGULAR,
         local("Se aplicará a tu próxima partida.", "Applies to your next game."), 1250, muted);
    for (unsigned i = 0; i < 3; ++i) {
        int y = 324 + (int)i * 148;
        bool focused = s->selected == i;
        x4_apple_fill_round_rect(p, 470, y, 1280, 128, 20, X4_RGBA(255,255,255,focused ? 21 : 12));
        x4_apple_stroke_round_rect(p, 470, y, 1280, 128, 20, 1.0f, X4_RGBA(255,255,255,36));
        if (focused) x4_apple_focus_ring(p, 470, y, 1280, 128, 20);
        line(p, 506, y + 22, 32, X4_FONT_SEMIBOLD, labels[i], 460, white);
        if (!i) {
            int bx = 506 + x4_text_width_px(32, X4_FONT_SEMIBOLD, labels[i]) + 24;
            badge(p, bx, y + 22, local("Recomendada", "Recommended"), 360);
        }
        line(p, 506, y + 74, 23, X4_FONT_REGULAR,
             x4_lang_current() == X4_LANG_EN ? descs_en[i] : descs_es[i], 1130, muted);
        radio(p, 1686, y + 48, focused);
    }
}

static uint32_t rtt_color(int ms)
{
    if (ms >= 1 && ms < 100) return 0xff00ff00u;
    if (ms >= 100 && ms <= 200) return 0xffffff00u;
    if (ms > 200 && ms <= 2000) return 0xffff4040u;
    return muted;
}

static void server_rows(const X4PictureSettingsUi *s, uint32_t *p)
{
    const int x = s->first_time_mode ? 170 : 470;
    const int row_width = s->first_time_mode ? 1110 : 900;
    const int info_x = x + row_width + 42;
    const int info_width = 1750 - info_x;
    const unsigned max_vis = 6;
    bool unavailable = x4_server_settings_server_not_available();
    int top = unavailable ? 352 : 302;

    line(p, x, 184, 46, X4_FONT_BOLD,
         s->first_time_mode ? x4_tr(X4_TR_SERVER_FIRST_TITLE) : x4_tr(X4_TR_OPTIONS_TAB_SERVER),
         1600 - x, white);
    if (s->first_time_mode) {
        wrapped(p, x, 246, 23, x4_tr(X4_TR_SERVER_FIRST_SUBTITLE), 1580, 1, muted);
    } else {
        line(p, x, 246, 25, X4_FONT_REGULAR,
             local("Elige dónde conectar tu próxima partida.", "Choose where to connect your next game."),
             1250, muted);
    }
    if (unavailable) {
        x4_apple_fill_round_rect(p, x, 293, 1750 - x, 44, 12, X4_RGBA(180,91,31,130));
        line(p, x + 16, 304, 20, X4_FONT_SEMIBOLD,
             x4_tr(X4_TR_SERVER_NOT_AVAILABLE), 1718 - x, 0xfffef3c7u);
    }

    unsigned count = x4_server_settings_get_region_count();
    unsigned total = count + 1u;
    unsigned off = s->server_scroll_offset;
    if (s->server_selected < off) off = s->server_selected;
    if (s->server_selected >= off + max_vis) off = s->server_selected - max_vis + 1;
    if (off + max_vis > total) off = total > max_vis ? total - max_vis : 0;
    /* Preserved presentation-owned scroll state, as in TAREA 19b. */
    ((X4PictureSettingsUi *)s)->server_scroll_offset = off;

    for (unsigned slot = 0; slot < max_vis && off + slot < total; ++slot) {
        unsigned index = off + slot;
        int y = top + (int)slot * 80;
        bool focused = s->server_selected == index;
        x4_apple_fill_round_rect(p, x, y, row_width, 70, 16, X4_RGBA(255,255,255,focused ? 24 : 12));
        x4_apple_stroke_round_rect(p, x, y, row_width, 70, 16, 1.0f, X4_RGBA(255,255,255,38));
        if (focused) x4_apple_focus_ring(p, x, y, row_width, 70, 16);
        radio(p, x + 20, y + 20, focused);

        char name[128] = {0}, tag[128] = {0}, subtitle[160] = {0};
        int ms = 0;
        if (!index) {
            snprintf(name, sizeof(name), "%s", local("Automático", "Automatic"));
            const char *raw = x4_server_settings_default_region_name();
            const char *display = x4_server_region_display_name(raw);
            snprintf(subtitle, sizeof(subtitle), x4_tr(X4_TR_SERVER_TAG_AUTO_FMT),
                     display && display[0] ? display : local("Sin asignar", "Not assigned"));
            if (!s->first_time_mode) ms = x4_server_settings_get_region_rtt(raw);
        } else {
            X4ServerRegion region;
            if (x4_server_settings_get_region(index - 1, &region)) {
                snprintf(name, sizeof(name), "%s", x4_server_region_display_name(region.name));
                if (!s->first_time_mode) ms = region.last_game_ms;
            }
        }
        if (ms >= 1 && ms <= 2000) {
            snprintf(tag, sizeof(tag), "%d ms · %s", ms, x4_tr(X4_TR_SERVER_LAST_GAME));
        } else {
            snprintf(tag, sizeof(tag), "%s", x4_tr(X4_TR_SERVER_NOT_MEASURED));
        }
        int tag_width = x4_text_width_px(18, X4_FONT_REGULAR, tag);
        if (tag_width > 238) tag_width = 238;
        int tag_x = x + row_width - 24 - tag_width;
        int name_width = tag_x - (x + 66) - 18;
        if (!index) {
            line(p, x + 66, y + 8, 24, X4_FONT_SEMIBOLD, name, 226, white);
            badge(p, x + 302, y + 7, local("Recomendado", "Recommended"), name_width - 238);
            line(p, x + 66, y + 41, 18, X4_FONT_REGULAR, subtitle, name_width, muted);
        } else {
            line(p, x + 66, y + 23, 25, X4_FONT_SEMIBOLD, name, name_width, white);
        }
        line(p, tag_x, y + 26, 18, X4_FONT_REGULAR, tag, tag_width, rtt_color(ms));
    }

    if (!count) {
        wrapped(p, x + 20, top + 100, 23, x4_tr(X4_TR_SERVER_NO_LIST), row_width - 40, 3, muted);
    }
    if (total > max_vis) {
        int height = max_vis * 80 - 10;
        int thumb = height * (int)max_vis / (int)total;
        if (thumb < 24) thumb = 24;
        int thumb_y = top + (int)off * (height - thumb) / (int)(total - max_vis);
        x4_apple_fill_round_rect(p, x + row_width + 12, top, 5, height, 2, X4_RGBA(255,255,255,22));
        x4_apple_fill_round_rect(p, x + row_width + 12, thumb_y, 5, thumb, 2, X4_RGBA(255,255,255,150));
    }

    x4_apple_fill_round_rect(p, info_x - 16, 302, 1, 510, 0, X4_RGBA(255,255,255,28));
    x4_apple_fill_round_rect(p, info_x + (info_width - 96) / 2, 332, 96, 96, 48, X4_RGBA(255,255,255,15));
    x4_apple_draw_icon(p, info_x + (info_width - 32) / 2, 364, X4_ICON_INFO, white);
    char chosen[128] = {0};
    if (s->server_selected == 0) {
        snprintf(chosen, sizeof(chosen), "%s", local("Automático", "Automatic"));
    } else {
        char raw[64] = {0};
        x4_server_settings_get_region_name(s->server_selected - 1, raw, sizeof(raw));
        snprintf(chosen, sizeof(chosen), "%s", x4_server_region_display_name(raw));
    }
    x4_apple_text_wrap(p, info_x + 4, 466, 27, X4_FONT_SEMIBOLD, chosen, info_width - 8, 37, 2, white);
    wrapped(p, info_x + 4, 554, 23,
            s->server_selected == 0
                ? local("Usa el servidor que Xbox asigna a tu cuenta.",
                        "Use the server Xbox assigns to your account.")
                : local("Conecta tu próxima partida a este servidor. Los ms corresponden a tu última partida; no son una prueba nueva.",
                        "Connect your next game to this server. ms are from your last game, not a new measurement."),
            info_width - 8, 6, muted);
}

static void settings_background(uint32_t *pixels)
{
    /* This is always a WB UI canvas. Never accept a VideoOut destination. */
    for (size_t i = 0; i < 1920u * 1080u; ++i) {
        pixels[i] = ((pixels[i] >> 1) & 0x007f7f7f) | 0xff000000u;
    }
    x4_apple_glass_panel(pixels, 100, 126, 1720, 836, 30, 18, 44);
}

static void settings_contents(const X4PictureSettingsUi *s,uint32_t *pixels)
{
    if (!s->first_time_mode) settings_sidebar(pixels, s->tab);
    if (s->tab == 0 && !s->first_time_mode) picture_rows(s, pixels);
    else if (s->tab == 2 && !s->first_time_mode)
        x4_apple_draw_access_controls(pixels,470,184,1280,600,&s->access_ui,X4_ACCESS_CARD_SELECT,true);
    else server_rows(s, pixels);
}

static void access_footer(const X4PictureSettingsUi *s,uint32_t *p,bool full)
{
    const char *save = x4_tr(s->access_ui.confirming ? X4_TR_ACCESS_CONTINUE : X4_TR_ACCESS_SAVE);
    x4_apple_pill_button(p,470,852,370,62,save,true,false,X4_ICON_NONE);
    x4_apple_pill_button(p,864,852,236,62,x4_tr(X4_TR_ACCESS_CANCEL),false,false,X4_ICON_NONE);
    if (full) {
        int hx=150;
        if (!s->access_ui.confirming) {
            hx+=x4_apple_draw_pad_glyph(p,hx,993,X4_GLYPH_L1)+12;
            hx+=x4_apple_draw_footer_hint(p,hx,993,X4_GLYPH_R1,local("Cambiar sección", "Change section"))+42;
        }
        hx+=x4_apple_draw_footer_hint(p,hx,993,X4_GLYPH_CROSS,save)+42;
        x4_apple_draw_footer_hint(p,hx,993,X4_GLYPH_CIRCLE,x4_tr(X4_TR_ACCESS_CANCEL));
    }
}

void x4_picture_settings_ui_draw(const X4PictureSettingsUi *s, uint32_t *pixels)
{
    if (!s || !s->open || !pixels) return;
    x4_server_settings_mark_ui_drawn();
    settings_background(pixels);
    settings_contents(s,pixels);
    if (s->tab == 2 && !s->first_time_mode) access_footer(s,pixels,true);
    else settings_footer(pixels, s->first_time_mode ? 170 : 470, s->first_time_mode);
}

void x4_picture_settings_ui_draw_cached(const X4PictureSettingsUi *s,uint32_t *pixels)
{
    if(!s||!s->open||!pixels)return;
    x4_server_settings_mark_ui_drawn();
    bool full=false;
    if(!x4_picture_settings_ui_cache_ready(s)){
        x4_picture_settings_ui_cache_invalidate();
        settings_cache.backdrop=malloc((size_t)SETTINGS_W*SETTINGS_H*sizeof(uint32_t));
        if(!settings_cache.backdrop){
            ++settings_cache.stats.allocation_failures;
            ++settings_cache.stats.background_preparations;
            settings_cache.stats.last_draw_full=true;
            x4_picture_settings_ui_draw(s,pixels);
            return;
        }
        settings_background(pixels);
        ++settings_cache.stats.background_preparations;
        memcpy(settings_cache.backdrop,pixels,(size_t)SETTINGS_W*SETTINGS_H*sizeof(uint32_t));
        settings_cache.owner=s;settings_cache.canvas=pixels;
        settings_cache.first_time=s->first_time_mode;settings_cache.language=x4_lang_current();
        settings_cache.stats.ready=true;
        settings_cache.stats.cache_bytes=(size_t)SETTINGS_W*SETTINGS_H*sizeof(uint32_t);
        full=true;
    }else if(settings_cache.canvas!=pixels){
        /* A new CPU canvas still needs the complete frozen backdrop once.
         * The production display has one persistent WB work surface. */
        memcpy(pixels,settings_cache.backdrop,(size_t)SETTINGS_W*SETTINGS_H*sizeof(uint32_t));
        settings_cache.canvas=pixels;++settings_cache.stats.full_canvas_copies;
        full=true;
    }else{
        for(int row=SETTINGS_Y;row<SETTINGS_Y+SETTINGS_PANEL_H;++row){
            size_t offset=(size_t)row*SETTINGS_W+SETTINGS_X;
            memcpy(pixels+offset,settings_cache.backdrop+offset,SETTINGS_PANEL_W*sizeof(uint32_t));
        }
    }
    bool access = s->tab == 2 && !s->first_time_mode;
    if (access || settings_cache.access_footer) {
        /* Access confirmation has a different footer. Restore it from the
         * original WB backdrop, then redraw its hints; keep other tabs' cache
         * behavior unchanged. Mark the extra dirty area for the display. */
        for (int row=980;row<1048;++row) {
            size_t offset=(size_t)row*SETTINGS_W+SETTINGS_X;
            memcpy(pixels+offset,settings_cache.backdrop+offset,SETTINGS_PANEL_W*sizeof(uint32_t));
        }
        full=true;
    }
    settings_contents(s,pixels);
    if (s->tab == 2 && !s->first_time_mode) access_footer(s,pixels,full);
    else {
        settings_footer_buttons(pixels,s->first_time_mode?170:470);
        if(full)settings_footer_hints(pixels,s->first_time_mode);
    }
    ++settings_cache.stats.cached_draws;settings_cache.stats.last_draw_full=full;
    settings_cache.access_footer=access;
}
