/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "ui2_theme.h"

typedef enum X4Ui2Section {
    X4_UI2_HOME, X4_UI2_DISCOVER, X4_UI2_CATALOG, X4_UI2_LIBRARY,
    X4_UI2_SEARCH, X4_UI2_SETTINGS, X4_UI2_SUPPORT, X4_UI2_ABOUT,
    X4_UI2_SECTION_COUNT,
} X4Ui2Section;

typedef struct {
    X4Ui2Section section;
    X4Ui2Section selected;
    bool focused;
} X4Ui2Shell;

enum {
    X4_UI2_NAV_LEFT = 1u << 0,
    X4_UI2_NAV_RIGHT = 1u << 1,
    X4_UI2_NAV_UP = 1u << 2,
    X4_UI2_NAV_DOWN = 1u << 3,
    X4_UI2_NAV_SELECT = 1u << 4,
    X4_UI2_NAV_PREVIOUS = 1u << 5,
    X4_UI2_NAV_NEXT = 1u << 6,
    X4_UI2_NAV_CIRCLE = 1u << 7,
    X4_UI2_NAV_OPTIONS = 1u << 8,
};

typedef struct {
    bool consumed, changed, activate;
    X4Ui2Section target;
} X4Ui2NavResult;

/* Caller routes target and sets section only after accepting the request.
 * at_left_edge preserves normal movement inside a content row. */
X4Ui2NavResult x4_ui2_shell_handle(X4Ui2Shell *shell, unsigned flags, bool at_left_edge);
void x4_ui2_shell_set_section(X4Ui2Shell *shell, X4Ui2Section section);

typedef enum X4Ui2Network {
    X4_UI2_NETWORK_UNKNOWN, X4_UI2_NETWORK_CABLE, X4_UI2_NETWORK_WIFI,
} X4Ui2Network;

typedef struct {
    X4Ui2Network network;
    char quality[32];
    int minute; /* Console-local minutes since midnight, -1 if unavailable. */
    char initial[5]; /* At most one complete UTF-8 codepoint. */
    bool valid;
} X4Ui2Status;

/* Returns true only when the visible fields changed. */
bool x4_ui2_status_update(X4Ui2Status *status, X4Ui2Network network,
    unsigned height, unsigned fps, int minute, const char *gamertag);
bool x4_ui2_status_set(X4Ui2Status *status, X4Ui2Network network,
    const char *quality, int minute, const char *gamertag);
extern const X4UiRect x4_ui2_sidebar_rect, x4_ui2_status_rect;
void x4_ui2_shell_draw_sidebar(uint32_t *pixels, const X4Ui2Shell *shell);
/* Replace the first fresh_height rows with the newly drawn, untinted backdrop.
 * A value of zero redraws the sidebar from its retained backdrop. */
void x4_ui2_shell_draw_sidebar_backdrop(uint32_t *pixels, const X4Ui2Shell *shell, int fresh_height);
/* A retained sidebar can update only the rows whose highlight changed. */
size_t x4_ui2_shell_draw_sidebar_delta(uint32_t *pixels, const X4Ui2Shell *previous,
    const X4Ui2Shell *shell, X4UiRect damage[4]);
/* Pass a fresh background after replacing the status area; otherwise the
 * same retained canvas must still contain the previous composition. */
void x4_ui2_shell_draw_status(uint32_t *pixels, const X4Ui2Status *status, bool fresh_background);
void x4_ui2_shell_draw(uint32_t *pixels, const X4Ui2Shell *shell, const X4Ui2Status *status);

/* Phase 1 adapter for screens whose layouts belong to later phases. The
 * legacy renderer first draws to a separate full canvas. This copies those
 * pixels with a uniform 16:9 fit in the content area and clears the margins. */
void x4_ui2_shell_fit_legacy(uint32_t *pixels, const uint32_t *source);
