/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "../media/live_media.h"
#include "../auth/xbox_session.h"

enum { X4_NET_HUD_OFF, X4_NET_HUD_WARNING, X4_NET_HUD_BASIC, X4_NET_HUD_FULL };
enum { X4_NET_HUD_FRAMES=60, X4_NET_HUD_MAX_WIDTH=560 };
/* All calls belong to main/UI, including the existing final stream JSON
 * writer after joined close. No borrowed snapshot or media pointer is read
 * later. session_key is an identity only; it is never dereferenced. */
unsigned x4_net_hud_mode(void);
const char *x4_net_hud_mode_label(void);
int x4_net_hud_cycle_mode(void);
bool x4_net_hud_prepare(const void *session_key,const X4LiveMediaSnapshot *,
    const X4SessionSnapshot *,int muted,uint64_t now_us);
/* Only opaque row copies from the prepared cache, never fonts or I/O. */
bool x4_net_hud_draw(uint32_t *pixels);
void x4_net_hud_note_copy_us(uint64_t us);
void x4_net_hud_note_compose_us(uint64_t us);
/* Ends this HUD session after writing; next prepare resets even if malloc
 * reuses the former media address. The persisted mode remains app-wide. */
void x4_net_hud_write_json(FILE *);

typedef struct {
    unsigned mode,height,frame_count;
    int rtt_ms,best_rtt_ms;
    double fps,mbps,loss_pct,mean_mbps,mean_loss_pct,max_frame_ms;
    bool have_stats,warning,reconnecting,first_picture;
    uint64_t degraded_episodes,degraded_seconds,warning_until_us;
    uint64_t compositions,copy_samples,copy_p95_upper_us,copy_max_us,compose_max_us;
    int x,y,width,height_px;
    float frame_ms[X4_NET_HUD_FRAMES]; /* oldest to newest */
    char line[256];
} X4NetHudSnapshot;
void x4_net_hud_snapshot(X4NetHudSnapshot *);

#ifdef X4_NET_HUD_TEST
/* New host fixtures only: caller supplies a temporary path. Never a second
 * production storage path, or a way to modify other settings formats. */
void x4_net_hud_test_reset(const char *settings_path);
#endif
