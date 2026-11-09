/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../streaming/stream_profile.h"
#include "access_country.h"

typedef enum {
    X4_PIC_QUALITY_1080P_ALTA = 0,
    X4_PIC_QUALITY_1080P = 1,
    X4_PIC_QUALITY_720P = 2,
    X4_PIC_QUALITY_1080P_MAX = 3,
    X4_PIC_QUALITY_COUNT = 4
} X4PictureQualityOption;
typedef struct {
    X4PictureQualityOption quality;
    unsigned conversion_backend; /* 0=CPU, 1=GPU */
    X4StreamVsyncPolicy vsync;
    bool dither;
} X4PictureSettingsConfig;

enum {
    X4_PIC_SETTINGS_OPEN = 1u,
    X4_PIC_SETTINGS_UP = 2u,
    X4_PIC_SETTINGS_DOWN = 4u,
    X4_PIC_SETTINGS_CONFIRM = 8u,
    X4_PIC_SETTINGS_CANCEL = 16u,
    X4_PIC_SETTINGS_PREV_TAB = 32u,
    X4_PIC_SETTINGS_NEXT_TAB = 64u,
    X4_PIC_SETTINGS_LEFT = 128u,
    X4_PIC_SETTINGS_RIGHT = 256u
};

typedef struct {
    bool assigned;
    bool connected;
    char name[32];
} X4PictureSettingsPad;

typedef enum {
    X4_PIC_SECTION_TRANSMISSION, X4_PIC_SECTION_SERVER, X4_PIC_SECTION_ACCESS,
    X4_PIC_SECTION_PADS, X4_PIC_SECTION_MIC, X4_PIC_SECTION_ACCOUNT,
    X4_PIC_SECTION_ABOUT, X4_PIC_SECTION_INTERFACE, X4_PIC_SECTION_COUNT
} X4PictureSettingsSection;
typedef enum { X4_PIC_ROUTE_NONE, X4_PIC_ROUTE_ACCOUNT, X4_PIC_ROUTE_ABOUT } X4PictureSettingsRoute;

typedef struct {
    bool open;
    unsigned tab; /* Imagen, Servidor, Acceso, Mandos, Micrófono */
    unsigned selected; /* Public image field 0..3; Lab quality 0..2. */
    unsigned draft_settings; /* Public image options, published only on Save. */
    bool save_failed, language_saving;
    unsigned server_selected; /* 0..region_count (0=Automático, 1..N=regions in tab 1) */
    unsigned server_scroll_offset;
    bool first_time_mode;
    bool server_changed;
    X4AccessCountryUi access_ui;
    bool access_changed;
    bool draft_multiplayer;
    X4PictureSettingsPad pads[4];
    bool draft_mic_enabled;
    unsigned draft_mic_ms, mic_selected;
    unsigned revision;
    bool section_focused, save_focused, dropdown_open, leave_pending;
    unsigned dropdown_choice, leave_selected;
    X4PictureSettingsRoute route, leave_route;
    char draft_server[64], saved_server[64];
    unsigned draft_language;

} X4PictureSettingsUi;

/* Keep declarations used outside the UI at their original source positions
 * for byte comparisons that include the linked DWARF. */
#line 59
/* Initialization and persistence */
void x4_picture_settings_init(void);
bool x4_picture_settings_multiplayer(void);
bool x4_picture_settings_set_multiplayer(bool enabled);
bool x4_picture_settings_mic_enabled(void);
unsigned x4_picture_settings_mic_ms(void);
X4PictureQualityOption x4_picture_settings_get(void);
bool x4_picture_settings_set(X4PictureQualityOption quality);
void x4_picture_settings_set_path(const char *path);
/* Tests may isolate both records explicitly. Production uses the fixed v1
 * read-only migration source and the new v2 destination. */
bool x4_picture_settings_set_paths(const char *legacy_path, const char *current_path);
X4PictureSettingsConfig x4_picture_settings_config(void);
bool x4_picture_settings_set_config(const X4PictureSettingsConfig *);

/* Mapping and labels */
X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption opt);
X4StreamRequestProfile x4_picture_settings_current_profile(void);
const char *x4_picture_settings_active_quality_label(void);

/* UI handling and rendering */
bool x4_picture_settings_ui_handle(X4PictureSettingsUi *ui, unsigned actions, bool can_edit);
void x4_picture_settings_ui_draw(const X4PictureSettingsUi *ui, uint32_t *pixels);
void x4_picture_settings_open_server_first_time(X4PictureSettingsUi *ui);

/* Main-owned persistent WB canvas path. The ordinary draw entry point keeps
 * its independent full-render contract for captures and other callers. */
typedef struct {
    uint64_t background_preparations, cached_draws, full_canvas_copies;
    uint64_t allocation_failures;
    size_t cache_bytes;
    bool ready, last_draw_full;
} X4PictureSettingsCacheStats;
bool x4_picture_settings_ui_cache_ready(const X4PictureSettingsUi *ui);
void x4_picture_settings_ui_draw_cached(const X4PictureSettingsUi *ui, uint32_t *pixels);
/* Opening/closing handlers already invalidate. Explicit callers must also
 * invalidate when replacing the underlying scene without a close event. */
void x4_picture_settings_ui_cache_invalidate(void);
void x4_picture_settings_ui_cache_snapshot(X4PictureSettingsCacheStats *out);

#include "ui_surface.h"
void x4_picture_settings_ui_open_section(X4PictureSettingsUi *, X4PictureSettingsSection);
bool x4_picture_settings_ui_dirty(const X4PictureSettingsUi *);
bool x4_picture_settings_ui_has_modal(const X4PictureSettingsUi *);
void x4_picture_settings_ui_draw_page(const X4PictureSettingsUi *, uint32_t *);
void x4_picture_settings_ui_draw_modal(const X4PictureSettingsUi *, uint32_t *);
/* Cached ordinary page redraws at most the section strip and the right panel.
 * SIZE_MAX means the background was repainted; otherwise damage has 0..2 rects. */
size_t x4_picture_settings_ui_draw_page_cached(const X4PictureSettingsUi *, uint32_t *,
    X4UiRect damage[2]);
