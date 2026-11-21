/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum { X4_MOTION_SCALE = 1024 };

/* Cubic ease-out. A single monotonic clock keeps dropped frames from
 * stretching a transition or slowing down the input that started it. */
static inline int x4_motion_remaining(uint64_t now, uint64_t start, unsigned duration)
{
    if (!start || !duration || (now >= start && now - start >= duration)) return 0;
    if (now <= start) return X4_MOTION_SCALE;
    const uint64_t left = (duration - (now - start)) * X4_MOTION_SCALE / duration;
    return (int)(left * left * left / (X4_MOTION_SCALE * X4_MOTION_SCALE));
}
