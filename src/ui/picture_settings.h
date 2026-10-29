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
    X4_PIC_QUALITY_COUNT = 3
} X4PictureQualityOption;

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
    bool open;
    unsigned tab; /* 0 = Imagen, 1 = Servidor, 2 = Acceso */
    unsigned selected; /* 0..2 (quality in tab 0) */
    unsigned server_selected; /* 0..region_count (0=Automático, 1..N=regions in tab 1) */
    unsigned server_scroll_offset;
    bool first_time_mode;
    bool server_changed;
    X4AccessCountryUi access_ui;
    bool access_changed;
} X4PictureSettingsUi;

/* Initialization and persistence */
void x4_picture_settings_init(void);
X4PictureQualityOption x4_picture_settings_get(void);
bool x4_picture_settings_set(X4PictureQualityOption quality);
void x4_picture_settings_set_path(const char *path);

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
