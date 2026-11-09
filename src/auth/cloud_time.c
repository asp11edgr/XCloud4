/* SPDX-License-Identifier: GPL-3.0-only */
#include "cloud_time.h"
#include "json.h"
#if !X4_PUBLIC
#include <stdio.h>
#endif

/* Allow purchased hours well beyond a monthly allocation while rejecting
 * implausible finite values, independently of the unlimited wire sentinel. */
#define CLOUD_TIME_MAX_SECONDS (366u * 86400u)
enum { CLOUD_DEPTH_MAX = 8, CLOUD_FIELDS_MAX = 262144 };

typedef struct { const uint8_t *next, *end; } Proto;
typedef struct {
    uint32_t number;
    uint8_t wire;
    uint64_t value;
    Proto child;
} Field;
typedef struct {
    X4CloudTimeShape *shape;
    X4CloudTime account, other_account, product;
    size_t fields;
} Scan;
enum Node {
    ROOT, PASSES, ACCOUNT, BUCKETS, ACCOUNT_TIME, PRODUCT, PRODUCT_DATA,
    USER_ACCESS, ACCESS, PRODUCT_TIME, IGNORE
};

static bool varint(Proto *r, uint64_t *value)
{
    uint64_t number = 0;
    for (unsigned shift = 0; shift < 64 && r->next != r->end; shift += 7) {
        uint8_t byte = *r->next++;
        if (shift == 63 && byte > 1) return false;
        number |= (uint64_t)(byte & 127u) << shift;
        if (!(byte & 128u)) {
            *value = number;
            return true;
        }
    }
    return false;
}

static bool field_read(Proto *r, Scan *scan, unsigned depth, Field *field)
{
    uint64_t tag, size;
    if (++scan->fields > CLOUD_FIELDS_MAX || !varint(r, &tag) || !(tag >> 3) ||
        (tag >> 3) > UINT32_C(0x1fffffff)) return false;
    *field = (Field){.number = (uint32_t)(tag >> 3), .wire = (uint8_t)(tag & 7u)};
    switch (field->wire) {
    case 0:
        return varint(r, &field->value);
    case 1:
        size = 8;
        break;
    case 2:
        if (!varint(r, &size) || size > (size_t)(r->end - r->next)) return false;
        field->child = (Proto){r->next, r->next + (size_t)size};
        break;
    case 3:
        /* Deprecated groups can still appear as unknown fields. */
        if (depth >= CLOUD_DEPTH_MAX) return false;
        while (r->next != r->end) {
            Field nested;
            if (!field_read(r, scan, depth + 1, &nested)) return false;
            if (nested.wire == 4) return nested.number == field->number;
        }
        return false;
    case 4:
        return true;
    case 5:
        size = 4;
        break;
    default:
        return false;
    }
    if (size > (size_t)(r->end - r->next)) return false;
    r->next += (size_t)size;
    return true;
}

static void shape_field(X4CloudTimeShape *shape, unsigned level, uint32_t parent,
    const Field *field)
{
    if (!shape || level > 2 || shape->count >= sizeof(shape->fields) / sizeof(shape->fields[0]))
        return;
    for (size_t i = 0; i < shape->count; ++i) {
        const X4CloudTimeField *saved = &shape->fields[i];
        if (saved->level == level && saved->parent == parent &&
            saved->number == field->number && saved->wire == field->wire) return;
    }
    shape->fields[shape->count++] = (X4CloudTimeField){
        .number = field->number, .parent = parent, .level = (uint8_t)level, .wire = field->wire
    };
}

static X4CloudTime quota(enum X4CloudTimeSource source, uint64_t remaining, uint64_t max,
    unsigned present)
{
    if (((present & 1u) && remaining == UINT64_MAX) || ((present & 2u) && max == UINT64_MAX))
        return (X4CloudTime){.unlimited = true, .source = source};
    /* A recognized quota can invalidate a previous measurement. NONE is
     * reserved for a missing response/transport error and keeps the old one. */
    if ((present & 2u) && (!max || ((present & 1u) && remaining > max)))
        return (X4CloudTime){.source = source, .remaining_s = remaining, .max_s = max};
    if (present != 3u || max > CLOUD_TIME_MAX_SECONDS)
        return (X4CloudTime){0};
    return (X4CloudTime){.remaining_s = remaining, .max_s = max, .source = source};
}

static bool invalid_quota(X4CloudTime value)
{
    return value.source != X4_CLOUD_TIME_NONE && !value.unlimited &&
        (!value.max_s || value.remaining_s > value.max_s);
}

static void retain_lower(X4CloudTime *best, X4CloudTime candidate)
{
    if (invalid_quota(*best)) return;
    if (invalid_quota(candidate)) {
        *best = candidate;
        return;
    }
    if (candidate.source != X4_CLOUD_TIME_NONE && (best->source == X4_CLOUD_TIME_NONE ||
        (!candidate.unlimited && (best->unlimited || candidate.remaining_s < best->remaining_s))))
        *best = candidate;
}

static bool publish_quota(X4CloudTime value)
{
#if !X4_PUBLIC
    if (invalid_quota(value))
        printf("XCloud4: tiempo nube incoherente fuente=%s motivo=%s\n",
            x4_cloud_time_source_name(value.source), value.max_s ? "restante_superior" : "maximo_cero");
#endif
    return value.source != X4_CLOUD_TIME_NONE;
}

static enum Node child_node(enum Node node, uint32_t number)
{
    switch (node) {
    case ROOT:
        if (number == 1) return PASSES;
        if (number == 2) return PRODUCT;
        if (number == 3) return ACCOUNT;
        break;
    case ACCOUNT:
        if (number == 2) return BUCKETS;
        break;
    case BUCKETS:
        return ACCOUNT_TIME;
    case PRODUCT:
        if (number == 2) return PRODUCT_DATA;
        break;
    case PRODUCT_DATA:
        if (number == 2) return USER_ACCESS;
        break;
    case USER_ACCESS:
        if (number == 2) return ACCESS;
        break;
    case ACCESS:
        if (number == 11) return PRODUCT_TIME;
        break;
    default:
        break;
    }
    return IGNORE;
}

static bool proto_node(Proto r, Scan *scan, enum Node node, unsigned depth, uint32_t parent)
{
    if (depth > CLOUD_DEPTH_MAX) return false;
    uint64_t remaining = 0, max = 0;
    unsigned present = 0;
    while (r.next != r.end) {
        Field field;
        if (!field_read(&r, scan, depth, &field) || field.wire == 4) return false;
        shape_field(scan->shape, depth, parent, &field);
        if ((node == ACCOUNT_TIME || node == PRODUCT_TIME) && (field.number == 2 || field.number == 4)) {
            unsigned bit = field.number == 2 ? 1u : 2u;
            if (field.wire != 0 || (present & bit)) return false;
            present |= bit;
            if (bit == 1u) remaining = field.value;
            else max = field.value;
        } else if (field.wire == 2) {
            enum Node child = child_node(node, field.number);
            if (child == PASSES) {
                /* Pass records are diagnostic-only. Their contents do not
                 * participate in selection or affect a usable allowance. */
                if (scan->shape && scan->shape->count < sizeof(scan->shape->fields) / sizeof(scan->shape->fields[0])) {
                    Scan diagnostics = {.shape = scan->shape};
                    (void)proto_node(field.child, &diagnostics, child, depth + 1, field.number);
                }
            } else if (child != IGNORE &&
                !proto_node(field.child, scan, child, depth + 1, field.number)) return false;
        }
    }
    if (node == ACCOUNT_TIME) {
        /* The public captures describe bucket 2 as the account allowance. */
        X4CloudTime *best = parent == 2 ? &scan->account : &scan->other_account;
        retain_lower(best, quota(X4_CLOUD_TIME_ACCOUNT, remaining, max, present));
    } else if (node == PRODUCT_TIME) {
        retain_lower(&scan->product, quota(X4_CLOUD_TIME_PRODUCT, remaining, max, present));
    }
    return true;
}

static bool json_seconds(X4JsonSpan span, uint64_t *out)
{
    X4JsonType type = x4_json_type(span);
    if (type == X4_JSON_T_STRING) {
        span.data++;
        span.length -= 2;
    } else if (type != X4_JSON_T_NUMBER) return false;
    if (!span.length || span.length > 20) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < span.length; ++i) {
        unsigned digit = (unsigned char)span.data[i] - (unsigned)'0';
        if (digit > 9 || value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static bool json_node(X4JsonSpan span, unsigned depth, size_t *fields,
    X4CloudTime *best, X4CloudTime *direct)
{
    X4JsonType type = x4_json_type(span);
    if (type != X4_JSON_T_OBJECT && type != X4_JSON_T_ARRAY) return true;
    if (depth > CLOUD_DEPTH_MAX) return false;
    if (type == X4_JSON_T_OBJECT) {
        X4JsonSpan remaining, max;
        int r = x4_json_member(span, "remainingGameplayTimeInSeconds", &remaining);
        int m = x4_json_member(span, "maxGameplayTimeInSeconds", &max);
        if (r < 0 || m < 0) return false;
        uint64_t rem_s = 0, max_s = 0;
        unsigned present = 0;
        if (r && json_seconds(remaining, &rem_s)) present |= 1u;
        if (m && json_seconds(max, &max_s)) present |= 2u;
        X4CloudTime value = quota(X4_CLOUD_TIME_JSON, rem_s, max_s, present);
        if (depth == 1) *direct = value;
        retain_lower(best, value);
    }
    size_t cursor = 0;
    for (;;) {
        X4JsonSpan key, value;
        int next = type == X4_JSON_T_ARRAY ? x4_json_item(span, &cursor, &value) :
            x4_json_object_member(span, &cursor, &key, &value);
        if (next <= 0) return next == 0;
        if (++*fields > CLOUD_FIELDS_MAX || !json_node(value, depth + 1, fields, best, direct))
            return false;
    }
}

bool x4_cloud_time_parse(const void *data, size_t length, X4CloudTime *out, X4CloudTimeShape *shape)
{
    if (shape) *shape = (X4CloudTimeShape){.length = length};
    if (!out) return false;
    *out = (X4CloudTime){0};
    if (!data || !length || length > X4_CLOUD_TIME_BODY_MAX) return false;
    const uint8_t *bytes = data;
    Scan scan = {.shape = shape};
    if (proto_node((Proto){bytes, bytes + length}, &scan, ROOT, 1, 0)) {
        *out = scan.account.source ? scan.account :
            (scan.other_account.source ? scan.other_account : scan.product);
        if (publish_quota(*out)) return true;
    }

    X4JsonSpan root;
    if (x4_json_parse(data, length, &root) != 0) return false;
    if (shape) shape->count = 0; /* JSON keys may be private. */
    X4CloudTime best = {0}, direct = {0};
    size_t fields = 0;
    if (!json_node(root, 1, &fields, &best, &direct)) return false;
    *out = direct.source ? direct : best;
    return publish_quota(*out);
}

const char *x4_cloud_time_source_name(enum X4CloudTimeSource source)
{
    switch (source) {
    case X4_CLOUD_TIME_ACCOUNT: return "cuenta";
    case X4_CLOUD_TIME_PRODUCT: return "producto";
    case X4_CLOUD_TIME_JSON: return "json";
    default: return "ninguna";
    }
}

X4CloudTime x4_cloud_time_current(const X4CloudTimeState *state, uint64_t now_us)
{
    X4CloudTime value = state->value;
    if (state->playing && value.source != X4_CLOUD_TIME_NONE && !value.unlimited &&
        now_us >= state->since_us) {
        uint64_t elapsed_s = (now_us - state->since_us) / UINT64_C(1000000);
        value.remaining_s = elapsed_s < value.remaining_s ? value.remaining_s - elapsed_s : 0;
    }
    return value;
}

void x4_cloud_time_set_playing(X4CloudTimeState *state, bool playing, uint64_t now_us)
{
    if (state->playing == playing) return;
    state->value = x4_cloud_time_current(state, now_us);
    state->since_us = now_us;
    state->playing = playing;
}

void x4_cloud_time_apply(X4CloudTimeState *state, X4CloudTime value, uint64_t now_us)
{
    if (value.source == X4_CLOUD_TIME_NONE) return;
    if (invalid_quota(value)) {
        state->value = (X4CloudTime){0};
        state->since_us = now_us;
        return;
    }
    X4CloudTime current = x4_cloud_time_current(state, now_us);
    if (state->playing && current.source != X4_CLOUD_TIME_NONE && !current.unlimited &&
        (value.unlimited || value.remaining_s >= current.remaining_s)) return;
    state->value = value;
    state->since_us = now_us;
}

unsigned x4_cloud_time_alerts(X4CloudTimeState *state, uint64_t now_us)
{
    X4CloudTime value = x4_cloud_time_current(state, now_us);
    if (!state->playing || value.unlimited || value.source == X4_CLOUD_TIME_NONE) return 0;
    if (value.remaining_s <= 5 * 60u) {
        unsigned alert = value.remaining_s && !(state->warned & X4_CLOUD_TIME_ALERT_5) ?
            X4_CLOUD_TIME_ALERT_5 : 0;
        state->warned |= X4_CLOUD_TIME_ALERT_15 | X4_CLOUD_TIME_ALERT_5;
        return alert;
    }
    if (value.remaining_s <= 15 * 60u && !(state->warned & X4_CLOUD_TIME_ALERT_15)) {
        state->warned |= X4_CLOUD_TIME_ALERT_15;
        return X4_CLOUD_TIME_ALERT_15;
    }
    return 0;
}
