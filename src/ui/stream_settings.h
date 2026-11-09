/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../streaming/stream_profile.h"
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include <stdbool.h>
#include <stdint.h>
enum {
    X4_SETTINGS_OPEN = 1u, X4_SETTINGS_UP = 2u, X4_SETTINGS_DOWN = 4u,
    X4_SETTINGS_LEFT = 8u, X4_SETTINGS_RIGHT = 16u,
    X4_SETTINGS_CONFIRM = 32u, X4_SETTINGS_CANCEL = 64u
};
typedef struct {
    bool open;
    bool applied;
    unsigned char interaction_modes;
    unsigned char convert_sharp; /* bit 0: filter, 1..2: sharpness, 3..4: request; Lab bit 5: engine, bit 6: save error */
    unsigned row;
    X4StreamRequestProfile profile;
    X4StreamColorPreference color;
    X4StreamCpuVariant cpu;
    X4StreamDecoderDepth depth;
} X4StreamSettingsUi;
/* Main thread only. Opening, navigation and confirmation consume the entire
 * catalog frame. Draft changes become global settings only on a separate X.
 * Losing edit permission cancels the draft; it cannot alter an active or
 * pending launch/auth operation. */
bool x4_stream_settings_ui_handle(X4StreamSettingsUi *, unsigned actions, bool can_edit);
void x4_stream_settings_ui_draw(const X4StreamSettingsUi *, uint32_t *pixels);
#if !X4_PUBLIC
/* Main-thread initialization before workers; a missing/old/unknown record
 * selects CPU. Only the new Lab record is read, never the public preferences.
 * The getter does no I/O and is captured once when creating a pipeline.
 * set() is main-thread only and publishes after an atomic on-disk replace. */
int x4_stream_settings_conversion_init(void);
unsigned x4_stream_settings_conversion_backend(void); /* 0 = CPU, 1 = GPU */
bool x4_stream_settings_conversion_set(unsigned backend);
#endif
void x4_stream_settings_summary(char *out, unsigned capacity);
/* Last confirmed selection only; opening/cancelling a draft never claims an
 * application. The real catalog draw uses this after the one-X commit. */
int x4_stream_settings_applied_text(const X4StreamSettingsUi *, char *out, unsigned capacity);
/* Bounded received-data banner, never inferred from the requested profile. */
int x4_stream_received_text(char *out, unsigned capacity, unsigned width,
    unsigned height, unsigned matrix_received, unsigned matrix_selected, bool full_range);
void x4_stream_received_draw(uint32_t *pixels, unsigned width, unsigned height,
    unsigned matrix_received, unsigned matrix_selected, bool full_range);
#if !X4_PUBLIC
bool x4_stream_settings_ui_warning_open(void);
void x4_stream_settings_ui_draw_page(const X4StreamSettingsUi *, uint32_t *);
void x4_stream_settings_ui_draw_modal(uint32_t *);
#endif
#endif
