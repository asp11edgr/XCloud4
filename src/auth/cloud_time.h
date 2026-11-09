/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum X4CloudTimeSource {
    X4_CLOUD_TIME_NONE,
    X4_CLOUD_TIME_ACCOUNT,
    X4_CLOUD_TIME_PRODUCT,
    X4_CLOUD_TIME_JSON
};

typedef struct {
    uint64_t remaining_s, max_s;
    bool unlimited;
    enum X4CloudTimeSource source;
} X4CloudTime;

typedef struct {
    uint32_t number, parent;
    uint8_t level, wire;
} X4CloudTimeField;

typedef struct {
    size_t length, count;
    X4CloudTimeField fields[64];
} X4CloudTimeShape;

#define X4_CLOUD_TIME_BODY_MAX (2u * 1024u * 1024u)

/* On failure the value is unknown. Optional shape contains only protobuf
 * field numbers and wire types, including when a malformed body is rejected. */
bool x4_cloud_time_parse(const void *data, size_t length, X4CloudTime *out,
    X4CloudTimeShape *shape);
const char *x4_cloud_time_source_name(enum X4CloudTimeSource source);

enum {
    X4_CLOUD_TIME_ALERT_15 = 1u,
    X4_CLOUD_TIME_ALERT_5 = 2u
};

/* Main-thread state. Zero-initialize on first use and when switching accounts. */
typedef struct {
    X4CloudTime value;
    uint64_t since_us;
    unsigned warned;
    bool playing;
} X4CloudTimeState;

void x4_cloud_time_set_playing(X4CloudTimeState *state, bool playing, uint64_t now_us);
void x4_cloud_time_apply(X4CloudTimeState *state, X4CloudTime value, uint64_t now_us);
X4CloudTime x4_cloud_time_current(const X4CloudTimeState *state, uint64_t now_us);
unsigned x4_cloud_time_alerts(X4CloudTimeState *state, uint64_t now_us);
