/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../auth/cloud_time.h"
#include "lang.h"
#include "ui_surface.h"
#include <stdio.h>

/* All views are published and drawn by the main/UI thread. */
void x4_cloud_time_home_set(X4CloudTime value);
bool x4_cloud_time_home_damage(X4UiRect *rect);
void x4_cloud_time_game_draw(uint32_t *pixels, X4CloudTime value, unsigned alert, bool menu_open);
void x4_cloud_time_confirm_draw(uint32_t *pixels);

static inline void x4_cloud_time_duration(char *text, size_t capacity, uint64_t seconds)
{
    unsigned long long hours = seconds / 3600u, minutes = seconds / 60u % 60u;
    if (hours && minutes) snprintf(text, capacity, "%llu h %llu min", hours, minutes);
    else if (hours) snprintf(text, capacity, "%llu h", hours);
    else if (seconds && !minutes) snprintf(text, capacity, "%s", x4_tr(X4_TR_CLOUD_TIME_LT_MIN));
    else snprintf(text, capacity, "%llu min", minutes);
}

static inline void x4_cloud_time_label(X4CloudTime value, char *text, size_t capacity)
{
    if (value.unlimited) {
        snprintf(text, capacity, "%s", x4_tr(X4_TR_CLOUD_TIME_UNLIMITED));
        return;
    }
    char remaining[40], maximum[40];
    x4_cloud_time_duration(remaining, sizeof(remaining), value.remaining_s);
    x4_cloud_time_duration(maximum, sizeof(maximum), value.max_s);
    snprintf(text, capacity, x4_tr(X4_TR_CLOUD_TIME_REMAINING), remaining, maximum);
}
