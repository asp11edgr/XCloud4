/* SPDX-License-Identifier: GPL-3.0-only */
#include "picture_settings.h"
#include "apple_ui.h"
#include "canvas.h"
#include "font.h"
#include "lang.h"
#include "server_settings.h"
#include "ui2_settings.h"
#include "ui2_modal.h"
#include "ui_lang_pref.h"
#include <errno.h>
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
#define X4_PIC_SETTINGS_V2_MAGIC 0x32533458u /* "X4S2" */
#define X4_PIC_SETTINGS_LEGACY_PATH "/data/xcloud4/settings-public-v1.bin"
#define X4_PIC_SETTINGS_DEFAULT_PATH "/data/xcloud4/settings-public-v2.bin"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t quality;
    uint32_t checksum;
} X4PictureSettingsRecord;
typedef struct {
    uint32_t magic, version, quality, backend, vsync, dither, checksum;
} X4PictureSettingsRecordV2;
_Static_assert(sizeof(X4PictureSettingsRecord)==16,"Public v1 record ABI");
_Static_assert(sizeof(X4PictureSettingsRecordV2)==28,"Public v2 record ABI");

static char custom_settings_path[256] = {0};
static char custom_legacy_path[256] = {0};
/* One publication: quality0..1, CPU2, VSync3..4, dither-off5. Zero is
 * Alta/GPU/Automatic/dither enabled. These are preferences, not wire fields. */
static _Atomic unsigned active_settings;
static _Atomic bool multiplayer_disabled;
static unsigned mic_options;

enum { SETTINGS_W=1920, SETTINGS_H=1080 };
static struct {
    const X4PictureSettingsUi *owner;
    uint32_t *canvas;
    enum X4Lang language;
    unsigned tab;
    bool section_focused;
    X4PictureSettingsCacheStats stats;
} settings_cache;

void x4_picture_settings_ui_cache_invalidate(void)
{
    settings_cache.owner = NULL;
    settings_cache.canvas = NULL;
    settings_cache.stats.ready = false;
}

bool x4_picture_settings_ui_cache_ready(const X4PictureSettingsUi *s)
{
    return s && s->open && !s->first_time_mode && settings_cache.stats.ready &&
        settings_cache.owner == s && settings_cache.language == x4_lang_current();
}

void x4_picture_settings_ui_cache_snapshot(X4PictureSettingsCacheStats *out)
{
    if (out) *out = settings_cache.stats;
}

static uint32_t calc_checksum(uint32_t magic, uint32_t version, uint32_t quality)
{
    return magic ^ (version << 16) ^ (version >> 16) ^ (quality * 0x9e3779b9u) ^ 0xa5a55a5au;
}

static const char *get_settings_path(void)
{
    return custom_settings_path[0] ? custom_settings_path : X4_PIC_SETTINGS_DEFAULT_PATH;
}
static const char *get_legacy_path(void)
{ return custom_legacy_path[0]?custom_legacy_path:X4_PIC_SETTINGS_LEGACY_PATH; }

void x4_picture_settings_set_path(const char *path)
{
    if(!path||!path[0]){(void)x4_picture_settings_set_paths(NULL,NULL);return;}
    char legacy[256];
    int length=snprintf(legacy,sizeof(legacy),"%s.v1",path);
    if(length>0&&(size_t)length<sizeof(legacy))
        (void)x4_picture_settings_set_paths(legacy,path);
}

bool x4_picture_settings_set_paths(const char *legacy,const char *current)
{
    if((legacy&&strlen(legacy)>=sizeof(custom_legacy_path))||
       (current&&strlen(current)>=sizeof(custom_settings_path)))return false;
    const char *lp=legacy&&legacy[0]?legacy:X4_PIC_SETTINGS_LEGACY_PATH;
    const char *cp=current&&current[0]?current:X4_PIC_SETTINGS_DEFAULT_PATH;
    if(!strcmp(lp,cp))return false;
    snprintf(custom_legacy_path,sizeof(custom_legacy_path),"%s",legacy?legacy:"");
    snprintf(custom_settings_path,sizeof(custom_settings_path),"%s",current?current:"");
    return true;
}

static X4PictureSettingsConfig defaults(void)
{
    X4PictureSettingsConfig c={X4_PIC_QUALITY_1080P_ALTA,1u,X4_VSYNC_AUTO,true};
    return c;
}
static unsigned config_word(const X4PictureSettingsConfig *c)
{ return (unsigned)c->quality|(c->conversion_backend?0u:4u)|((unsigned)c->vsync<<3)|(c->dither?0u:32u); }
static X4PictureSettingsConfig config_from_word(unsigned word)
{
    X4PictureSettingsConfig c={(X4PictureQualityOption)(word&3u),!(word&4u),
        (X4StreamVsyncPolicy)((word>>3)&3u),!(word&32u)};
#if X4_PUBLIC
    c.conversion_backend=1u;
    c.dither=true;
    /* V2 Auto and Always both read as On. Keep writing the old Always value
     * for On so an older version also honors the choice after a downgrade. */
    if(c.vsync!=X4_VSYNC_NEVER)c.vsync=X4_VSYNC_ALWAYS;
#else
    if((unsigned)c.vsync>X4_VSYNC_SYNCED)c.vsync=X4_VSYNC_AUTO;
#endif
    return c;
}
static bool config_valid(const X4PictureSettingsConfig *c)
{ return c&&(unsigned)c->quality<X4_PIC_QUALITY_COUNT&&c->conversion_backend<=1u&&(unsigned)c->vsync<=X4_VSYNC_SYNCED; }
static uint32_t checksum_v2(const X4PictureSettingsRecordV2 *r)
{
    const uint32_t words[6]={r->magic,r->version,r->quality,r->backend,r->vsync,r->dither};
    uint32_t value=0x72656332u;
    for(unsigned i=0;i<6u;++i)value=((value<<5)|(value>>27))^(words[i]+0x9e3779b9u);
    return value;
}

static X4PictureQualityOption load_v1(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return X4_PIC_QUALITY_1080P_ALTA;
    X4PictureSettingsRecord rec;
    size_t n = fread(&rec, 1, sizeof(rec), f);
    int tail=fgetc(f);bool good=!ferror(f);if(fclose(f))good=false;
    if (!good||n != sizeof(rec)||tail!=EOF) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.magic != X4_PIC_SETTINGS_MAGIC) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.version != X4_PIC_SETTINGS_VERSION) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.quality >= 3u) return X4_PIC_QUALITY_1080P_ALTA;
    if (rec.checksum != calc_checksum(rec.magic, rec.version, rec.quality)) return X4_PIC_QUALITY_1080P_ALTA;
    return (X4PictureQualityOption)rec.quality;
}
static bool load_v2(const char *path,X4PictureSettingsConfig *out,bool *absent)
{
    *absent=false;errno=0;FILE *file=fopen(path,"rb");
    if(!file){*absent=errno==ENOENT;return false;}
    X4PictureSettingsRecordV2 r;
    size_t bytes=fread(&r,1,sizeof(r),file);int tail=fgetc(file);
    bool good=!ferror(file);if(fclose(file))good=false;
    if(!good||bytes!=sizeof(r)||tail!=EOF||r.magic!=X4_PIC_SETTINGS_V2_MAGIC||r.version!=2u||
        r.quality>=X4_PIC_QUALITY_COUNT||r.backend>1u||r.vsync>X4_VSYNC_SYNCED||r.dither>1u||
        r.checksum!=checksum_v2(&r))return false;
    *out=(X4PictureSettingsConfig){(X4PictureQualityOption)r.quality,r.backend,(X4StreamVsyncPolicy)r.vsync,r.dither!=0};
#if X4_PUBLIC
    *out=config_from_word(config_word(out));
#endif
    return true;
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

static bool save_to_disk(const char *path,const X4PictureSettingsConfig *c)
{
    if(!strcmp(path,get_legacy_path()))return false;
    ensure_parent_dirs(path);
    char tmp[270];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    X4PictureSettingsRecordV2 rec={X4_PIC_SETTINGS_V2_MAGIC,2u,(uint32_t)c->quality,
        c->conversion_backend,(uint32_t)c->vsync,c->dither?1u:0u,0u};
    rec.checksum=checksum_v2(&rec);
    size_t written = fwrite(&rec, 1, sizeof(rec), f);
    bool good=written==sizeof(rec);
    if(fflush(f))good=false;
    if(fclose(f))good=false;
    if (!good) {
        remove(tmp);
        return false;
    }
#if defined(_WIN32)
    /* Native product uses POSIX atomic replace; host Windows needs its
     * existing replace convention. This is only the v2 destination. */
    (void)remove(path);
#endif
    return rename(tmp, path) == 0;
}

/* Separate from the picture record so earlier versions still read it. */
static void multiplayer_path(char path[262])
{
    snprintf(path,262,"%s.pads",get_settings_path());
}

bool x4_picture_settings_multiplayer(void)
{
    return !atomic_load_explicit(&multiplayer_disabled,memory_order_acquire);
}

static void load_multiplayer(void)
{
    char path[262];
    multiplayer_path(path);
    FILE *f=fopen(path,"rb");
    bool disabled=false;
    if(f){
        int value=fgetc(f),tail=fgetc(f);
        disabled=value=='0'&&tail==EOF&&!ferror(f);
        if(fclose(f))disabled=false;
    }
    atomic_store_explicit(&multiplayer_disabled,disabled,memory_order_release);
}

bool x4_picture_settings_set_multiplayer(bool enabled)
{
    char path[262],tmp[270];
    multiplayer_path(path);
    ensure_parent_dirs(path);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *f=fopen(tmp,"wb");
    if(!f)return false;
    bool ok=fputc(enabled?'1':'0',f)!=EOF;
    if(fflush(f))ok=false;
    if(fclose(f))ok=false;
#if defined(_WIN32)
    if(ok)(void)remove(path);
#endif
    if(!ok||rename(tmp,path)){
        (void)remove(tmp);
        return false;
    }
    atomic_store_explicit(&multiplayer_disabled,!enabled,memory_order_release);
    return true;
}

/* Main is the sole owner. The RTC worker sees only the launch snapshot. */
bool x4_picture_settings_mic_enabled(void)
{
    return (mic_options & 1u) != 0;
}

unsigned x4_picture_settings_mic_ms(void)
{
#if X4_PUBLIC
    return 10;
#else
    return (mic_options & 2u) ? 20u : 10u;
#endif
}

static void mic_path(char path[262])
{
    snprintf(path,262,"%s.mic",get_settings_path());
}

static void load_mic(void)
{
    char path[262];
    mic_path(path);
    mic_options=0;
    FILE *f=fopen(path,"rb");
    if(!f)return;
    int value=fgetc(f),tail=fgetc(f);
    bool ok=tail==EOF&&!ferror(f)&&value>='0'&&value<='3';
    if(fclose(f))ok=false;
    if(ok)mic_options=(unsigned)(value-'0');
}

static bool save_mic(bool enabled,unsigned ms)
{
    char path[262],tmp[270];
    mic_path(path);
    ensure_parent_dirs(path);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *f=fopen(tmp,"wb");
    if(!f)return false;
    /* A public save preserves the Lab-only preference in a shared settings
     * directory. Public launch still unconditionally uses 10 ms. */
    unsigned options=(enabled?1u:0u)|(X4_PUBLIC?(mic_options&2u):(ms==20?2u:0u));
    bool ok=fputc((int)('0'+options),f)!=EOF;
    if(fflush(f))ok=false;
    if(fclose(f))ok=false;
#if defined(_WIN32)
    if(ok)(void)remove(path);
#endif
    if(!ok||rename(tmp,path)){
        (void)remove(tmp);
        return false;
    }
    mic_options=options;
    return true;
}

void x4_picture_settings_init(void)
{
    load_multiplayer();
    load_mic();
    x4_access_country_init();
    X4PictureSettingsConfig c=defaults();bool absent=false;
    if(!load_v2(get_settings_path(),&c,&absent)&&absent)c.quality=load_v1(get_legacy_path());
    atomic_store_explicit(&active_settings,config_word(&c),memory_order_release);
}

X4PictureSettingsConfig x4_picture_settings_config(void)
{ return config_from_word(atomic_load_explicit(&active_settings,memory_order_acquire)); }

X4PictureQualityOption x4_picture_settings_get(void)
{
    X4PictureQualityOption q=x4_picture_settings_config().quality;
#if !X4_PUBLIC
    if(q==X4_PIC_QUALITY_1080P_MAX)q=X4_PIC_QUALITY_1080P_ALTA;
#endif
    return q;
}

bool x4_picture_settings_set(X4PictureQualityOption quality)
{
    if ((unsigned)quality >= X4_PIC_QUALITY_COUNT) return false;
    X4PictureSettingsConfig c=x4_picture_settings_config();c.quality=quality;
    return x4_picture_settings_set_config(&c);
}
bool x4_picture_settings_set_config(const X4PictureSettingsConfig *c)
{
    if(!config_valid(c))return false;
    X4PictureSettingsConfig normalized=*c;
#if X4_PUBLIC
    normalized=config_from_word(config_word(c));
#endif
    if(!save_to_disk(get_settings_path(),&normalized))return false;
    atomic_store_explicit(&active_settings,config_word(&normalized),memory_order_release);
    return true;
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
    case X4_PIC_QUALITY_1080P_MAX:
        return X4_REQUEST_1080P_HQ;
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
    case X4_PIC_QUALITY_1080P_MAX:
        return x4_tr(X4_TR_PIC_SETTINGS_MAX_LABEL);
    default:
        return x4_tr(X4_TR_PIC_SETTINGS_BANNER_1080P_ALTA);
    }
}


static unsigned active_picture_word(void)
{
    X4PictureSettingsConfig config = x4_picture_settings_config();
    config.quality = x4_picture_settings_get();
    return config_word(&config);
}

void x4_picture_settings_ui_open_section(X4PictureSettingsUi *s, X4PictureSettingsSection tab)
{
    if (!s || tab >= X4_PIC_SECTION_COUNT) return;
    x4_picture_settings_ui_cache_invalidate();
    unsigned revision = s->revision + 1u;
    *s = (X4PictureSettingsUi){
        .open = true, .tab = tab, .revision = revision,
        .draft_settings = active_picture_word(),
        .draft_language = x4_ui_lang_pref_get(),
        .draft_multiplayer = x4_picture_settings_multiplayer(),
        .draft_mic_enabled = x4_picture_settings_mic_enabled(),
        .draft_mic_ms = x4_picture_settings_mic_ms(),
    };
    x4_access_country_ui_open(&s->access_ui);
    const char *saved = x4_server_settings_get_selected();
    snprintf(s->saved_server, sizeof(s->saved_server), "%s", saved ? saved : "");
    memcpy(s->draft_server, s->saved_server, sizeof(s->draft_server));
    s->server_selected = (unsigned)x4_server_settings_find_index(s->draft_server);
}

void x4_picture_settings_open_server_first_time(X4PictureSettingsUi *s)
{
    if (!s) return;
    x4_picture_settings_ui_cache_invalidate();
    unsigned revision = s->revision + 1u;
    *s = (X4PictureSettingsUi){
        .open = true, .tab = X4_PIC_SECTION_SERVER,
        .first_time_mode = true, .revision = revision,
    };
}

bool x4_picture_settings_ui_dirty(const X4PictureSettingsUi *s)
{
    return s && (s->draft_settings != active_picture_word() ||
        strcmp(s->draft_server, s->saved_server) ||
        s->access_ui.selected != x4_access_country_get() ||
        s->draft_multiplayer != x4_picture_settings_multiplayer() ||
        s->draft_mic_enabled != x4_picture_settings_mic_enabled() ||
        s->draft_mic_ms != x4_picture_settings_mic_ms() ||
        s->draft_language != (unsigned)x4_ui_lang_pref_get() || s->save_failed);
}

bool x4_picture_settings_ui_has_modal(const X4PictureSettingsUi *s)
{
    return s && s->open && (s->first_time_mode || s->leave_pending || s->access_ui.confirming);
}

static void settings_close(X4PictureSettingsUi *s, X4PictureSettingsRoute route)
{
    s->open = s->first_time_mode = s->leave_pending = s->access_ui.confirming = false;
    s->route = route;
    x4_picture_settings_ui_cache_invalidate();
}

static void settings_request_leave(X4PictureSettingsUi *s, X4PictureSettingsRoute route)
{
    if (!x4_picture_settings_ui_dirty(s)) {
        settings_close(s, route);
        return;
    }
    s->leave_pending = true;
    s->leave_selected = 0;
    s->leave_route = route;
}

static bool settings_save_server(X4PictureSettingsUi *s)
{
    if (!x4_server_settings_set_selected(s->draft_server, true)) return false;
    s->server_changed = strcmp(s->saved_server, s->draft_server) != 0;
    memcpy(s->saved_server, s->draft_server, sizeof(s->saved_server));
    x4_server_settings_clear_not_available();
    x4_server_settings_notify_choice_done();
    return true;
}

static void settings_saved(X4PictureSettingsUi *s)
{
    s->save_failed = false;
    if (s->leave_pending) settings_close(s, s->leave_route);
    else s->save_focused = false;
}

static void settings_save(X4PictureSettingsUi *s, bool accept_access)
{
    const bool access_changed = s->access_ui.selected != x4_access_country_get();
    if (access_changed && s->access_ui.selected != X4_ACCESS_AUTO &&
        !x4_access_country_warning_accepted() && !accept_access) {
        s->access_ui.confirming = true;
        return;
    }
    if (s->draft_settings != active_picture_word()) {
        X4PictureSettingsConfig config = config_from_word(s->draft_settings);
        if (!x4_picture_settings_set_config(&config)) goto failed;
    }
    if (s->draft_multiplayer != x4_picture_settings_multiplayer() &&
        !x4_picture_settings_set_multiplayer(s->draft_multiplayer)) goto failed;
    if ((s->draft_mic_enabled != x4_picture_settings_mic_enabled() ||
        s->draft_mic_ms != x4_picture_settings_mic_ms()) &&
        !save_mic(s->draft_mic_enabled, s->draft_mic_ms)) goto failed;
    if (access_changed) {
        if (!x4_access_country_apply(s->access_ui.selected, accept_access, "ajustes")) {
            s->access_ui.save_error = true;
            goto failed;
        }
        s->access_changed = true;
    }
    s->access_ui.confirming = s->access_ui.save_error = false;
    if (strcmp(s->draft_server, s->saved_server) && !settings_save_server(s)) goto failed;
    const X4UiLangSave result = x4_ui_lang_pref_request((X4UiLangPreference)s->draft_language);
    if (result == X4_UI_LANG_SAVE_FAILED) goto failed;
    if (result == X4_UI_LANG_SAVE_PENDING) {
        s->save_failed = false;
        s->language_saving = true;
    } else settings_saved(s);
    return;

failed:
    s->save_failed = true;
}

static unsigned settings_server_index(const X4PictureSettingsUi *s)
{
    return (unsigned)x4_server_settings_find_index(s->draft_server);
}

static bool settings_server_move(X4PictureSettingsUi *s, bool down)
{
    const unsigned count = x4_server_settings_get_region_count();
    unsigned current = settings_server_index(s);
    if (!s->first_time_mode && down && current == count && x4_picture_settings_ui_dirty(s)) {
        s->save_focused = true;
        return true;
    }
    unsigned next = (current + (down ? 1u : count)) % (count + 1u);
    if (next == current) return false;
    char choice[sizeof(s->draft_server)] = {0};
    if (next && !x4_server_settings_get_region_name(next - 1u, choice, sizeof(choice)))
        return false;
    memcpy(s->draft_server, choice, sizeof(s->draft_server));
    s->server_selected = next;
    return true;
}

static unsigned settings_values(const X4PictureSettingsUi *s)
{
    if (s->tab == X4_PIC_SECTION_INTERFACE) return X4_UI_LANG_COUNT;
    if (s->tab == X4_PIC_SECTION_ACCESS) return X4_ACCESS_COUNTRY_COUNT;
    if (s->tab == X4_PIC_SECTION_MIC) return s->mic_selected ? 2u : 0u;
    if (s->tab != X4_PIC_SECTION_TRANSMISSION) return 0;
#if X4_PUBLIC
    return s->selected ? 2u : 4u;
#else
    return 3u;
#endif
}

static unsigned settings_value(const X4PictureSettingsUi *s)
{
    if (s->tab == X4_PIC_SECTION_INTERFACE) return s->draft_language;
    if (s->tab == X4_PIC_SECTION_ACCESS) return s->access_ui.selected;
    if (s->tab == X4_PIC_SECTION_MIC) return s->draft_mic_ms == 20;
    X4PictureSettingsConfig config = config_from_word(s->draft_settings);
    return s->selected ? config.vsync != X4_VSYNC_NEVER : (unsigned)config.quality;
}

static void settings_set_value(X4PictureSettingsUi *s, unsigned value)
{
    if (s->tab == X4_PIC_SECTION_INTERFACE) {
        s->draft_language = value;
    } else if (s->tab == X4_PIC_SECTION_ACCESS) {
        s->access_ui.selected = (X4AccessCountry)value;
        s->access_ui.save_error = false;
    } else if (s->tab == X4_PIC_SECTION_MIC) {
        s->draft_mic_ms = value ? 20u : 10u;
    } else {
        X4PictureSettingsConfig config = config_from_word(s->draft_settings);
        if (s->selected) config.vsync = value ? X4_VSYNC_ALWAYS : X4_VSYNC_NEVER;
        else config.quality = (X4PictureQualityOption)value;
        s->draft_settings = config_word(&config);
    }
    s->save_failed = false;
}

static void settings_focus_section(X4PictureSettingsUi *s, bool down)
{
    const unsigned count = X4_PIC_SECTION_COUNT;
    s->tab = (s->tab + (down ? 1u : count - 1u)) % count;
    s->section_focused = true;
    s->save_focused = s->dropdown_open = false;
}

static void settings_enter_section(X4PictureSettingsUi *s)
{
    if (s->tab == X4_PIC_SECTION_ACCOUNT)
        settings_request_leave(s, X4_PIC_ROUTE_ACCOUNT);
    else if (s->tab == X4_PIC_SECTION_ABOUT)
        settings_request_leave(s, X4_PIC_ROUTE_ABOUT);
    else
        s->section_focused = false;
}

bool x4_picture_settings_ui_handle(X4PictureSettingsUi *s, unsigned a, bool can_edit)
{
    if (!s) return false;
    if (s->language_saving) {
        const X4UiLangSave result = x4_ui_lang_pref_poll();
        if (result != X4_UI_LANG_SAVE_PENDING) {
            s->language_saving = false;
            ++s->revision;
            if (result == X4_UI_LANG_SAVE_DONE) settings_saved(s);
            else s->save_failed = true;
        }
        return true;
    }
    if (!can_edit && !s->first_time_mode) {
        bool consumed = s->open;
        if (consumed) settings_close(s, X4_PIC_ROUTE_NONE);
        return consumed;
    }
    if (!s->open) {
        if (!(a & X4_PIC_SETTINGS_OPEN)) return false;
        x4_picture_settings_ui_open_section(s, X4_PIC_SECTION_TRANSMISSION);
        return true;
    }
    if (!a) return true;
    ++s->revision;
    const bool up = (a & X4_PIC_SETTINGS_UP) != 0;
    const bool down = (a & X4_PIC_SETTINGS_DOWN) != 0;
    const bool left = (a & X4_PIC_SETTINGS_LEFT) != 0;
    const bool right = (a & X4_PIC_SETTINGS_RIGHT) != 0;
    const bool confirm = (a & X4_PIC_SETTINGS_CONFIRM) != 0;
    const bool cancel = (a & X4_PIC_SETTINGS_CANCEL) != 0;

    if (s->first_time_mode) {
        if (cancel) {
            x4_server_settings_notify_choice_done();
            settings_close(s, X4_PIC_ROUTE_NONE);
        } else if (up || down) {
            (void)settings_server_move(s, down);
            s->save_failed = false;
        } else if (confirm) {
            if (settings_save_server(s)) settings_close(s, X4_PIC_ROUTE_NONE);
            else s->save_failed = true;
        }
        return true;
    }
    if (s->access_ui.confirming) {
        if (cancel) s->access_ui.confirming = false;
        else if (confirm) settings_save(s, true);
        return true;
    }
    if (s->leave_pending) {
        if (cancel) s->leave_pending = false;
        else if (left || right) s->leave_selected ^= 1u;
        else if (confirm) {
            if (s->leave_selected) settings_close(s, s->leave_route);
            else settings_save(s, false);
        }
        return true;
    }
    if (s->dropdown_open) {
        const unsigned count = settings_values(s);
        if (cancel) s->dropdown_open = false;
        else if (count && (up || down))
            s->dropdown_choice = (s->dropdown_choice + (down ? 1u : count - 1u)) % count;
        else if (count && confirm) {
            settings_set_value(s, s->dropdown_choice);
            s->dropdown_open = false;
        }
        return true;
    }
    if (cancel || (a & X4_PIC_SETTINGS_OPEN)) {
        settings_request_leave(s, X4_PIC_ROUTE_NONE);
        return true;
    }
    if (a & (X4_PIC_SETTINGS_PREV_TAB | X4_PIC_SETTINGS_NEXT_TAB)) {
        settings_focus_section(s, (a & X4_PIC_SETTINGS_NEXT_TAB) != 0);
        return true;
    }
    if (s->section_focused) {
        if (up || down) settings_focus_section(s, down);
        else if (right || confirm) settings_enter_section(s);
        return true;
    }
    if (s->save_focused) {
        if (up || left) s->save_focused = false;
        else if (confirm && x4_picture_settings_ui_dirty(s)) settings_save(s, false);
        return true;
    }
    if (s->tab == X4_PIC_SECTION_SERVER) {
        if (up || down) {
            if (settings_server_move(s, down)) s->save_failed = false;
        } else if (left) s->section_focused = true;
        else if (confirm && x4_picture_settings_ui_dirty(s)) s->save_focused = true;
        return true;
    }
    if (s->tab == X4_PIC_SECTION_ACCOUNT || s->tab == X4_PIC_SECTION_ABOUT) {
        if (confirm) settings_enter_section(s);
        else if (left) s->section_focused = true;
        return true;
    }

    if (up || down) {
        unsigned *row = s->tab == X4_PIC_SECTION_MIC ? &s->mic_selected : &s->selected;
        unsigned rows = 1;
#if X4_PUBLIC
        if (s->tab == X4_PIC_SECTION_TRANSMISSION) rows = 2;
#else
        if (s->tab == X4_PIC_SECTION_MIC) rows = 2;
#endif
        if (up && rows > 1 && *row) --*row;
        else if (down && rows > 1 && *row + 1u < rows) ++*row;
        else if (down && x4_picture_settings_ui_dirty(s)) s->save_focused = true;
        else if (up) s->section_focused = true;
        return true;
    }
    const unsigned count = settings_values(s);
    if (count && (left || right))
        settings_set_value(s, (settings_value(s) + (right ? 1u : count - 1u)) % count);
    else if (count && confirm) {
        s->dropdown_open = true;
        s->dropdown_choice = settings_value(s);
    } else if (!count && (left || right || confirm)) {
        if (s->tab == X4_PIC_SECTION_PADS) s->draft_multiplayer = !s->draft_multiplayer;
        else if (s->tab == X4_PIC_SECTION_MIC) s->draft_mic_enabled = !s->draft_mic_enabled;
        s->save_failed = false;
    }
    return true;
}

enum {
    PAGE_X = X4_UI2_CONTENT_X, PAGE_R = X4_UI_SAFE_RIGHT,
    SECTION_X = PAGE_X, SECTION_Y = 177, SECTION_W = 320, SECTION_H = 738,
    PANEL_X = 626, PANEL_W = PAGE_R - PANEL_X, PANEL_H = SECTION_H,
    ROW_X = PANEL_X + 30, ROW_W = PANEL_W - 60,
    ROW_Y = 338, ROW_H = 58, ROW_PITCH = 70,
    SERVER_VISIBLE = 6, SERVER_ROW_H = 48, SERVER_ROW_PITCH = 57,
};
static const X4UiRect sections_rect = {SECTION_X, SECTION_Y, SECTION_W, SECTION_H};
static const X4UiRect panel_rect = {PANEL_X, SECTION_Y, PANEL_W, PANEL_H};
static const X4UiRect panel_body_rect = {PANEL_X + 15, 324, PANEL_W - 30, 580};

static const enum X4TextId section_labels[X4_PIC_SECTION_COUNT] = {
    X4_TR_UI2_SETTINGS_TRANSMISSION, X4_TR_OPTIONS_TAB_SERVER, X4_TR_ACCESS_TAB,
    X4_TR_PAD_SETTINGS_TAB, X4_TR_MIC_SETTINGS_TAB, X4_TR_APPLE_YOUR_ACCOUNT,
    X4_TR_APPLE_PILL_ABOUT, X4_TR_50_SETTINGS_INTERFACE,
};

static const enum X4TablerIconId section_icons[X4_PIC_SECTION_COUNT] = {
    X4_ICON_DEVICE_DESKTOP, X4_ICON_SERVER, X4_ICON_WORLD, X4_ICON_DEVICE_GAMEPAD,
    X4_ICON_MICROPHONE, X4_ICON_USER, X4_ICON_INFO, X4_ICON_NONE,
};

static const enum X4TextId section_help[X4_PIC_SECTION_COUNT] = {
    X4_TR_PIC_SETTINGS_SUBTITLE, X4_TR_SERVER_SETTINGS_SUBTITLE, X4_TR_ACCESS_DESCRIPTION,
    X4_TR_PAD_SETTINGS_NEXT_MATCH, X4_TR_PAD_SETTINGS_NEXT_MATCH, X4_TR_APPLE_CARD_ACCOUNT_SUB_IN,
    X4_TR_ABOUT_LINE3, X4_TR_50_SETTINGS_INTERFACE_HELP,
};

static const char *quality_value(unsigned value)
{
    static const enum X4TextId ids[] = {X4_TR_PIC_SETTINGS_BANNER_1080P_ALTA,
        X4_TR_PIC_SETTINGS_BANNER_1080P, X4_TR_PIC_SETTINGS_BANNER_720P,
        X4_TR_PIC_SETTINGS_MAX_LABEL};
    return x4_tr(ids[value < sizeof(ids) / sizeof(ids[0]) ? value : 0]);
}

static const char *quality_help(unsigned value)
{
    static const enum X4TextId ids[] = {X4_TR_U49_QUALITY_HIGH_HELP,
        X4_TR_U49_QUALITY_NORMAL_HELP, X4_TR_U49_QUALITY_720_HELP,
        X4_TR_PIC_SETTINGS_MAX_DESC};
    return x4_tr(ids[value < sizeof(ids) / sizeof(ids[0]) ? value : 0]);
}

static const char *dropdown_value(const X4PictureSettingsUi *s, unsigned value)
{
    if (s->tab == X4_PIC_SECTION_INTERFACE) return x4_ui_lang_pref_label((X4UiLangPreference)value);
    if (s->tab == X4_PIC_SECTION_ACCESS) return x4_access_country_label((X4AccessCountry)value);
    if (s->tab == X4_PIC_SECTION_MIC) return value ? "20 ms" : "10 ms";
    if (s->selected) return x4_tr(value ? X4_TR_VSYNC_ALWAYS : X4_TR_VSYNC_NEVER);
    return quality_value(value);
}

static void settings_draw_sections(const X4PictureSettingsUi *s, uint32_t *p)
{
    x4_rect(p, sections_rect.x, sections_rect.y, sections_rect.w, sections_rect.h, X4_UI2_BACKGROUND);
    x4_ui2_card(p, SECTION_X, SECTION_Y, SECTION_W, SECTION_H, X4_UI2_RADIUS_CARD);
    for (unsigned i = 0; i < X4_PIC_SECTION_COUNT; ++i) {
        const int x = SECTION_X + 20, y = SECTION_Y + 31 + (int)i * 79, w = SECTION_W - 40;
        const bool active = i == s->tab;
        if (active) {
            x4_apple_fill_round_rect(p, x, y, w, 58, X4_UI2_RADIUS_CONTROL, X4_UI2_ACTIVE);
            x4_rect(p, x + 4, y + 15, 3, 28, X4_UI2_ACCENT);
            if (s->section_focused) x4_ui2_focus(p, x, y, w, 58, X4_UI2_RADIUS_CONTROL);
        }
        const uint32_t color = active ? X4_UI2_ACCENT : X4_UI2_MUTED;
        if (i == X4_PIC_SECTION_INTERFACE)
            x4_text_px(p, x + 18, y + 19, 20, X4_FONT_SEMIBOLD, "Aa", color);
        else
            x4_apple_draw_icon(p, x + 18, y + 16, section_icons[i], color);
        x4_ui2_settings_line(p, x + 52, y + 16, 24, X4_FONT_SEMIBOLD,
            x4_tr(section_labels[i]), w - 70, color);
    }
}

static bool row_focus(const X4PictureSettingsUi *s)
{
    return !s->section_focused && !s->save_focused;
}

static void settings_draw_toggle(uint32_t *p, int y, const char *label, bool enabled, bool focused)
{
    x4_ui2_settings_row(p, ROW_X, y, ROW_W, ROW_H, label, "", focused, false);
    x4_ui2_settings_line(p, ROW_X + ROW_W - 350, y + 17, 24, X4_FONT_SEMIBOLD,
        x4_tr(enabled ? X4_TR_PAD_SETTINGS_ON : X4_TR_PAD_SETTINGS_OFF),
        251, focused ? X4_UI2_ACCENT : X4_UI2_MUTED);
    x4_ui2_settings_toggle(p, ROW_X + ROW_W - 78, y + 15, enabled);
}

static const char *settings_draw_picture(const X4PictureSettingsUi *s, uint32_t *p)
{
    X4PictureSettingsConfig config = config_from_word(s->draft_settings);
    x4_ui2_settings_row(p, ROW_X, ROW_Y, ROW_W, ROW_H, x4_tr(X4_TR_UI2_SETTINGS_QUALITY),
        quality_value(config.quality), row_focus(s) && !s->selected, true);
#if X4_PUBLIC
    const bool vsync = config.vsync == X4_VSYNC_ALWAYS;
    x4_ui2_settings_row(p, ROW_X, ROW_Y + ROW_PITCH, ROW_W, ROW_H, x4_tr(X4_TR_PIC_VSYNC),
        x4_tr(vsync ? X4_TR_VSYNC_ALWAYS : X4_TR_VSYNC_NEVER), row_focus(s) && s->selected, true);
    if (s->selected) return x4_tr(vsync ? X4_TR_VSYNC_HELP_ALWAYS : X4_TR_U49_VSYNC_NEVER_HELP);
#endif
    return quality_help(config.quality);
}

static void settings_draw_pads(const X4PictureSettingsUi *s, uint32_t *p)
{
    settings_draw_toggle(p, ROW_Y, x4_tr(X4_TR_PAD_SETTINGS_LABEL), s->draft_multiplayer, row_focus(s));
    for (unsigned i = 0; i < 4; ++i) {
        const X4PictureSettingsPad *pad = &s->pads[i];
        char label[64];
        snprintf(label, sizeof(label), x4_tr(X4_TR_UI2_SETTINGS_CONTROLLER_FMT), i + 1u);
        int y = ROW_Y + 92 + (int)i * 62;
        x4_rect(p, ROW_X, y + 48, ROW_W, 1, X4_UI2_BORDER);
        x4_ui2_settings_line(p, ROW_X + 20, y, 24, X4_FONT_SEMIBOLD, label, 240, X4_UI2_TEXT);
        x4_ui2_settings_line(p, ROW_X + 302, y, 24, X4_FONT_REGULAR,
            pad->assigned && pad->name[0] ? pad->name : x4_tr(X4_TR_PAD_SETTINGS_FREE), 390, X4_UI2_MUTED);
        if (pad->assigned) x4_ui2_settings_line(p, ROW_X + 766, y, 24, X4_FONT_REGULAR,
            x4_tr(pad->connected ? X4_TR_PAD_SETTINGS_PRESENT : X4_TR_PAD_SETTINGS_ABSENT),
            ROW_W - 790, X4_UI2_MUTED);
    }
}

static void settings_draw_mic(const X4PictureSettingsUi *s, uint32_t *p)
{
    settings_draw_toggle(p, ROW_Y, x4_tr(X4_TR_MIC_SETTINGS_TAB), s->draft_mic_enabled,
        row_focus(s) && !s->mic_selected);
#if !X4_PUBLIC
    x4_ui2_settings_row(p, ROW_X, ROW_Y + ROW_PITCH, ROW_W, ROW_H,
        x4_tr(X4_TR_MIC_SETTINGS_DURATION), dropdown_value(s, s->draft_mic_ms == 20),
        row_focus(s) && s->mic_selected, true);
#endif
}

static const char *settings_draw_access(const X4PictureSettingsUi *s, uint32_t *p)
{
    x4_ui2_settings_row(p, ROW_X, ROW_Y, ROW_W, ROW_H, x4_tr(X4_TR_ACCESS_COUNTRY),
        x4_access_country_label(s->access_ui.selected), row_focus(s), true);
    return x4_tr(X4_TR_ACCESS_NEXT_QUERY);
}

static uint32_t server_rtt_color(int ms)
{
    if (ms >= 1 && ms < 100) return X4_UI2_ACCENT;
    if (ms >= 100 && ms <= 200) return X4_RGB(252, 224, 130);
    if (ms > 200 && ms <= 2000) return X4_RGB(254, 165, 154);
    return X4_UI2_MUTED;
}

static int settings_server_height(unsigned total, int width)
{
    const unsigned rows = total < SERVER_VISIBLE ? total : SERVER_VISIBLE;
    if (rows == 1) {
        const int lines = x4_ui2_text_bounded(NULL, 0, 0, 22, X4_FONT_REGULAR,
            x4_tr(X4_TR_SERVER_NO_LIST), width - 32, 30, 2, X4_UI2_MUTED);
        if (lines) return SERVER_ROW_PITCH + 13 + 22 + (lines - 1) * 30;
    }
    return ((int)rows - 1) * SERVER_ROW_PITCH + SERVER_ROW_H;
}

static void settings_draw_server(const X4PictureSettingsUi *s, uint32_t *p,
    int x, int top, int w, unsigned total)
{
    const unsigned selected = settings_server_index(s);
    const unsigned off = selected >= SERVER_VISIBLE ? selected - SERVER_VISIBLE + 1u : 0u;
    for (unsigned slot = 0; slot < SERVER_VISIBLE && off + slot < total; ++slot) {
        const unsigned index = off + slot;
        const int y = top + (int)slot * SERVER_ROW_PITCH;
        const bool chosen = selected == index;
        x4_ui2_card(p, x, y, w, SERVER_ROW_H, X4_UI2_RADIUS_CONTROL);
        if (chosen && row_focus(s)) x4_ui2_focus(p, x, y, w, SERVER_ROW_H, X4_UI2_RADIUS_CONTROL);
        x4_apple_stroke_round_rect(p, x + 18, y + 14, 20, 20, 10, 2.0f,
            chosen ? X4_UI2_ACCENT : X4_UI2_MUTED);
        if (chosen)
            x4_apple_fill_round_rect(p, x + 23, y + 19, 10, 10, 5, X4_UI2_ACCENT);

        const char *label = x4_tr(X4_TR_SERVER_OPT_AUTO);
        char mapped[156] = {0};
        int ms = 0;
        if (index) {
            X4ServerRegion region;
            if (x4_server_settings_get_region(index - 1u, &region)) {
                snprintf(mapped, sizeof(mapped), "%s", x4_server_region_display_name(region.name));
                label = mapped;
                if (!s->first_time_mode) ms = region.last_game_ms;
            }
        } else if (!s->first_time_mode) {
            const char *automatic = x4_server_settings_default_region_name();
            if (automatic && *automatic) {
                char assigned[110];
                snprintf(assigned, sizeof(assigned), x4_tr(X4_TR_SERVER_TAG_AUTO_FMT),
                    x4_server_region_display_name(automatic));
                snprintf(mapped, sizeof(mapped), "%s · %s", label, assigned);
                label = mapped;
            }
            ms = x4_server_settings_get_region_rtt(automatic);
        }
        char measure[108];
        if (ms > 0 && ms <= 2000)
            snprintf(measure, sizeof(measure), "%d ms · %s", ms, x4_tr(X4_TR_SERVER_LAST_GAME));
        else snprintf(measure, sizeof(measure), "%s", x4_tr(X4_TR_SERVER_NOT_MEASURED));
        x4_ui2_settings_line(p, x + 53, y + 13, 22, X4_FONT_SEMIBOLD, label,
            w - 460, chosen ? X4_UI2_ACCENT : X4_UI2_TEXT);
        const int measure_w = x4_text_width_px(20, X4_FONT_REGULAR, measure);
        x4_ui2_settings_line(p, x + w - 20 - (measure_w > 370 ? 370 : measure_w), y + 14,
            20, X4_FONT_REGULAR, measure, 370, server_rtt_color(ms));
    }
    if (total > SERVER_VISIBLE) {
        const int length = SERVER_VISIBLE * SERVER_ROW_PITCH - (SERVER_ROW_PITCH - SERVER_ROW_H);
        const int thumb = length * SERVER_VISIBLE / (int)total;
        const int y = top + (int)off * (length - thumb) / (int)(total - SERVER_VISIBLE);
        x4_apple_fill_round_rect(p, x + w + 9, top, 4, length, 2, X4_UI2_BORDER);
        x4_apple_fill_round_rect(p, x + w + 9, y, 4, thumb, 2, X4_UI2_MUTED);
    }
    if (total == 1)
        x4_ui2_text_bounded(p, x + 16, top + SERVER_ROW_PITCH + 13, 22, X4_FONT_REGULAR,
            x4_tr(X4_TR_SERVER_NO_LIST), w - 32, 30, 2, X4_UI2_MUTED);
}

static void settings_dropdown(const X4PictureSettingsUi *s, uint32_t *p)
{
    const unsigned count = settings_values(s);
    unsigned field = s->tab == X4_PIC_SECTION_MIC ? s->mic_selected :
        s->tab == X4_PIC_SECTION_TRANSMISSION ? s->selected : 0;
    const int x = ROW_X + ROW_W - 564, y = ROW_Y + ROW_H + 12 + (int)field * ROW_PITCH;
    const int w = 564, h = 20 + (int)count * 48;
    x4_ui2_card(p, x, y, w, h, X4_UI2_RADIUS_CARD);
    for (unsigned i = 0; i < count; ++i) {
        const int row = y + 10 + (int)i * 48;
        if (i == s->dropdown_choice) {
            x4_apple_fill_round_rect(p, x + 10, row, w - 20, 45, 10, X4_UI2_ACTIVE);
            x4_ui2_focus(p, x + 10, row, w - 20, 45, 10);
        }
        x4_ui2_settings_line(p, x + 27, row + 12, 22, X4_FONT_SEMIBOLD,
            dropdown_value(s, i), w - 54, i == s->dropdown_choice ? X4_UI2_ACCENT : X4_UI2_TEXT);
    }
}

static void settings_draw_panel(const X4PictureSettingsUi *s, uint32_t *p, bool fresh)
{
    unsigned section = s->tab < X4_PIC_SECTION_COUNT ? s->tab : X4_PIC_SECTION_TRANSMISSION;
    if (fresh) {
        x4_rect(p, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h, X4_UI2_BACKGROUND);
        x4_ui2_card(p, PANEL_X, SECTION_Y, PANEL_W, PANEL_H, X4_UI2_RADIUS_CARD);
        x4_ui2_heading(p, ROW_X, 199, x4_tr(section_labels[section]));
        x4_ui2_text_bounded(p, ROW_X, 277, 22, X4_FONT_REGULAR, x4_tr(section_help[section]),
            ROW_W, 28, 2, X4_UI2_MUTED);
    } else {
        x4_rect(p, panel_body_rect.x, panel_body_rect.y,
            panel_body_rect.w, panel_body_rect.h, X4_UI2_SURFACE);
    }
    const char *help = NULL;
    int help_y = ROW_Y + ROW_H + 20;
    switch (section) {
    case X4_PIC_SECTION_TRANSMISSION:
        help = settings_draw_picture(s, p);
#if X4_PUBLIC
        help_y += ROW_PITCH;
#endif
        break;
    case X4_PIC_SECTION_SERVER: {
        const unsigned total = x4_server_settings_get_region_count() + 1u;
        settings_draw_server(s, p, ROW_X, ROW_Y, ROW_W - 22, total);
        help_y = ROW_Y + settings_server_height(total, ROW_W - 22) + 20;
        if (x4_server_settings_server_not_available()) help = x4_tr(X4_TR_SERVER_NOT_AVAILABLE);
        else if (!settings_server_index(s)) help = x4_tr(X4_TR_UI2_SETTINGS_AUTO_SERVER);
        break;
    }
    case X4_PIC_SECTION_ACCESS:
        help = settings_draw_access(s, p);
        break;
    case X4_PIC_SECTION_PADS:
        settings_draw_pads(s, p);
        break;
    case X4_PIC_SECTION_MIC:
        settings_draw_mic(s, p);
        break;
    case X4_PIC_SECTION_INTERFACE:
        x4_ui2_settings_row(p, ROW_X, ROW_Y, ROW_W, ROW_H, x4_tr(X4_TR_50_SETTINGS_LANGUAGE),
            dropdown_value(s, s->draft_language), row_focus(s) && !s->dropdown_open, true);
        break;
    default:
        x4_ui2_button(p, ROW_X, ROW_Y, 330, ROW_H, x4_tr(X4_TR_APPLE_BTN_ENTER), true,
            row_focus(s), section_icons[section]);
        break;
    }
    if (help) {
        x4_apple_draw_icon(p, ROW_X, help_y, X4_ICON_INFO, X4_UI2_MUTED);
        x4_ui2_text_bounded(p, ROW_X + 36, help_y, 22, X4_FONT_REGULAR, help,
            ROW_W - 36, 28, 2, X4_UI2_MUTED);
    }
    if (s->save_failed)
        x4_ui2_settings_line(p, ROW_X, 812, 20, X4_FONT_SEMIBOLD,
            x4_tr(s->access_ui.save_error ? X4_TR_ACCESS_SAVE_ERROR : X4_TR_PIC_SAVE_ERROR),
            ROW_W, X4_RGB(254, 165, 154));
    if (x4_picture_settings_ui_dirty(s))
        x4_ui2_button(p, PAGE_R - 282, 840, 252, 54,
            x4_tr(s->language_saving ? X4_TR_50_SETTINGS_SAVING : X4_TR_UI2_SETTINGS_SAVE),
            !s->language_saving, s->save_focused && !s->language_saving, X4_ICON_NONE);
    else {
        x4_apple_fill_round_rect(p, PAGE_R - 282, 840, 252, 54, 27, X4_UI2_BORDER);
        x4_ui2_settings_line(p, PAGE_R - 255, 855, 24, X4_FONT_SEMIBOLD,
            x4_tr(X4_TR_UI2_SETTINGS_SAVE), 200, X4_UI2_MUTED);
    }
    if (s->dropdown_open) settings_dropdown(s, p);
}

static void settings_draw_static(uint32_t *p)
{
    x4_rect(p, 0, 0, SETTINGS_W, SETTINGS_H, X4_UI2_BACKGROUND);
    x4_ui2_heading(p, PAGE_X, 57, x4_tr(X4_TR_APPLE_PILL_SETTINGS));
    const X4Ui2Hint hints[] = {
        {X4_PAD_L1, ""}, {X4_PAD_R1, x4_tr(X4_TR_UI2_SETTINGS_SECTION)},
        {X4_PAD_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)},
    };
    x4_ui2_footer(p, hints, sizeof(hints) / sizeof(hints[0]));
}

void x4_picture_settings_ui_draw_page(const X4PictureSettingsUi *s, uint32_t *p)
{
    if (!s || !s->open || s->first_time_mode || !p) return;
    x4_server_settings_mark_ui_drawn();
    settings_draw_static(p);
    settings_draw_sections(s, p);
    settings_draw_panel(s, p, true);
}

size_t x4_picture_settings_ui_draw_page_cached(const X4PictureSettingsUi *s, uint32_t *p, X4UiRect damage[2])
{
    if (!s || !s->open || s->first_time_mode || !p) return 0;
    if (!x4_picture_settings_ui_cache_ready(s) || settings_cache.canvas != p) {
        x4_picture_settings_ui_draw_page(s, p);
        settings_cache.owner = s;
        settings_cache.canvas = p;
        settings_cache.language = x4_lang_current();
        settings_cache.tab = s->tab;
        settings_cache.section_focused = s->section_focused;
        settings_cache.stats.ready = settings_cache.stats.last_draw_full = true;
        ++settings_cache.stats.background_preparations;
        return SIZE_MAX;
    }
    size_t count = 0;
    const bool section_changed = settings_cache.tab != s->tab;
    if (section_changed || settings_cache.section_focused != s->section_focused) {
        settings_draw_sections(s, p);
        settings_cache.tab = s->tab;
        settings_cache.section_focused = s->section_focused;
        if (damage) damage[count] = sections_rect;
        ++count;
    }
    settings_draw_panel(s, p, section_changed);
    if (damage) damage[count] = section_changed ? panel_rect : panel_body_rect;
    ++settings_cache.stats.cached_draws;
    settings_cache.stats.last_draw_full = false;
    return count + 1u;
}

void x4_picture_settings_ui_draw_modal(const X4PictureSettingsUi *s, uint32_t *p)
{
    if (!x4_picture_settings_ui_has_modal(s) || !p) return;
    const uint32_t error_color = X4_RGB(254, 165, 154);
    if (s->first_time_mode) {
        x4_server_settings_mark_ui_drawn();
        enum { LIST_W = X4_UI2_MODAL_BODY_W - 18, GAP = 16 };
        const unsigned total = x4_server_settings_get_region_count() + 1u;
        const char *subtitle = x4_tr(X4_TR_SERVER_FIRST_SUBTITLE);
        const int list_y = x4_ui2_modal_text_height(subtitle, X4_UI2_MODAL_BODY_W, 2) + GAP;
        const int list_bottom = list_y + settings_server_height(total, LIST_W);
        const char *error = s->save_failed ? x4_tr(X4_TR_PIC_SAVE_ERROR) : NULL;
        const int error_h = x4_ui2_modal_text_height(error, X4_UI2_MODAL_BODY_W, 2);
        const X4UiRect r = x4_ui2_modal_panel(p, x4_tr(X4_TR_SERVER_FIRST_TITLE),
            list_bottom + (error_h ? GAP + error_h : 0));
        x4_ui2_modal_text(p, subtitle, r.x, r.y, r.w, 2);
        settings_draw_server(s, p, r.x, r.y + list_y, LIST_W, total);
        x4_ui2_text_bounded(p, r.x, r.y + list_bottom + GAP, X4_UI2_LABEL, X4_FONT_REGULAR,
            error, r.w, 36, 2, error_color);
        const X4Ui2ModalAction actions[] = {
            {X4_PAD_CROSS, x4_tr(X4_TR_UI2_SETTINGS_SAVE), true, true, false},
            {X4_PAD_CIRCLE, x4_tr(X4_TR_UI2_SETTINGS_LAB_CANCEL), false, false, false},
        };
        x4_ui2_modal_actions(p, actions, sizeof(actions) / sizeof(actions[0]));
    } else if (s->access_ui.confirming) {
        const X4Ui2ModalAction actions[] = {
            {X4_PAD_CROSS, x4_tr(X4_TR_ACCESS_CONTINUE), true, true, false},
            {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_CANCEL), false, false, false},
        };
        x4_ui2_modal_message(p, x4_tr(X4_TR_ACCESS_CONFIRM_TITLE), x4_tr(X4_TR_ACCESS_WARNING),
            s->save_failed ? x4_tr(X4_TR_ACCESS_SAVE_ERROR) : NULL,
            error_color, actions, sizeof(actions) / sizeof(actions[0]));
    } else {
        const X4Ui2ModalAction actions[] = {
            {X4_PAD_CROSS, x4_tr(X4_TR_UI2_SETTINGS_SAVE), true, !s->leave_selected, false},
            {X4_PAD_CROSS, x4_tr(X4_TR_UI2_SETTINGS_DISCARD), false, s->leave_selected != 0, false},
            {X4_PAD_CIRCLE, x4_tr(X4_TR_U49_KEEP_EDITING), false, false, false},
        };
        x4_ui2_modal_message(p, x4_tr(X4_TR_UI2_SETTINGS_UNSAVED_TITLE),
            x4_tr(s->language_saving ? X4_TR_50_SETTINGS_SAVING : X4_TR_UI2_SETTINGS_UNSAVED_BODY),
            s->save_failed ? x4_tr(X4_TR_PIC_SAVE_ERROR) : NULL,
            error_color, actions, sizeof(actions) / sizeof(actions[0]));
    }
}

void x4_picture_settings_ui_draw(const X4PictureSettingsUi *s, uint32_t *p)
{
    x4_picture_settings_ui_draw_page(s, p);
    if (x4_picture_settings_ui_has_modal(s)) {
        x4_ui2_modal_scrim(p);
        x4_picture_settings_ui_draw_modal(s, p);
    }
}

void x4_picture_settings_ui_draw_cached(const X4PictureSettingsUi *s, uint32_t *p)
{
    if (x4_picture_settings_ui_has_modal(s)) {
        x4_picture_settings_ui_draw(s, p);
        return;
    }
    (void)x4_picture_settings_ui_draw_page_cached(s, p, NULL);
}
