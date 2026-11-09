/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SYNC42_FRAMES=18000, SYNC42_SAMPLES=SYNC42_FRAMES+128 };
enum Sync42Mode { SYNC42_FIFO, SYNC42_MAILBOX, SYNC42_SYNCED, SYNC42_HSYNC, SYNC42_AUTO };
typedef struct { uint64_t source_us,ready_us;unsigned decode_us; } Sync42Arrival;
typedef struct {
    uint64_t shown,ring_drops,mailbox_drops,recovery_drops,repeats,vblanks;
    uint64_t first_us,last_us;
    uint64_t ids[SYNC42_SAMPLES],times[SYNC42_SAMPLES];
    unsigned waits[SYNC42_SAMPLES],intervals[SYNC42_SAMPLES];
    size_t display_size,scanout_size,pending_offset,screen_offset;
} Sync42Result;
void synced42_engine(enum Sync42Mode,const Sync42Arrival *,size_t,Sync42Result *);
void legacy42_engine(enum Sync42Mode,const Sync42Arrival *,size_t,Sync42Result *);
