/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../input/controller.h"
#include "../input/touch_menu.h"
#include "../video/h264_demo.h"
#include "../audio/demo_audio.h"
#include "../auth/device_auth.h"
#include "../auth/xbox_catalog.h"
#include "../auth/xbox_session.h"
#include "../media/live_media.h"
#include "catalog_browser.h"
#include "product_metadata.h"

typedef struct {
    int page;
    int selected;
    int exit_requested;
    int exit_error;
    int signed_in;
    int explore_catalog_requested;
    int auto_catalog_after_login;
    int open_settings_requested;
    int catalog_from_home;
    int open_recent_requested;
} X4Screen;

typedef struct {
    int signed_in;
    char gamertag[32];
    const X4CoverRaster *featured_hero;
    const X4CoverRaster *fortnite_cover;
    const X4CoverRaster *fortnite_poster;
    const X4CoverRaster *app_icon; /* Existing assets/icon0.png, owned by main. */
    const X4CoverRaster *continue_cover; /* Borrowed Recent row raster for this draw. */
    const char *continue_name; /* NULL shows the neutral Recent tile. */
} X4HomeDrawContext;

#define X4_FORTNITE_PRODUCT_ID "BT5P2X999VH2"
size_t x4_screen_cover_demand(int page, const char *featured_id,
    const char *ids[2], uint32_t masks[2]);
void x4_screen_set_home_context(const X4HomeDrawContext *ctx);
void x4_screen_update(X4Screen *screen, uint32_t pressed, uint32_t held);
void x4_screen_draw(const X4Screen *screen, const X4Controller *controller, uint32_t *pixels);
void x4_media_draw(const X4DemoVideo *video, const X4DemoAudio *audio, uint32_t *pixels);
void x4_exit_error_draw(int error, uint32_t *pixels);
void x4_auth_draw(const X4AuthSnapshot *auth, int busy, int closing, uint32_t *pixels);
/* Account-only opt-in; no credential is accepted or drawn. */
void x4_auth_remember_draw(bool enabled, bool available, bool confirming, uint32_t *pixels);
void x4_session_draw(const X4SessionSnapshot *session, int busy, int closing, uint32_t *pixels);
void x4_live_status_draw(const X4LiveMediaSnapshot *media, int error, uint32_t *pixels);
/* Main-thread-owned cache. Prepare before conversion/presentation so a state
 * change is not charged to the steady-state opaque copy. No video-buffer read. */
typedef struct {
    uint64_t regenerations;
    uint64_t frames;
    uint64_t copied_bytes;
    size_t cache_bytes;
    bool is_ready;
} X4TouchMenuCacheStats;
bool x4_touch_menu_prepare(const X4TouchMenu *menu, int muted);
void x4_touch_menu_cache_reset(void);
void x4_touch_menu_cache_snapshot(X4TouchMenuCacheStats *out);
/* Separate leaf for host/native disassembly: loads source, stores destination. */
void x4_touch_menu_copy_opaque_row(uint32_t *restrict dst,
                                 const uint32_t *restrict src, size_t pixels);
void x4_touch_menu_draw(const X4TouchMenu *menu, int muted, uint32_t *pixels);
void x4_live_overlay(const X4LiveMediaSnapshot *media, const X4SessionSnapshot *session, int muted,
                     const X4TouchMenu *touch_menu, bool show_banner, uint32_t *pixels);
void x4_fortnite_summary_draw(const X4Screen *screen, const X4CatalogSnapshot *catalog, int busy, uint32_t *pixels);
bool x4_auth_qr_payload(const char *uri, const char *code, char *payload_out, size_t cap);

/* Exact public donation destination, also used by the QR renderer. */
const char *x4_support_qr_url(void);
