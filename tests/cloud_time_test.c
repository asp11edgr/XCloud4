/* SPDX-License-Identifier: GPL-3.0-only */
#include "auth/cloud_time.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t data[512]; size_t size; } Buffer;

static void byte(Buffer *b, uint8_t value)
{
    assert(b->size < sizeof(b->data));
    b->data[b->size++] = value;
}

static void uint_wire(Buffer *b, uint64_t value)
{
    while (value >= 128) {
        byte(b, (uint8_t)(128 | (value & 127)));
        value >>= 7;
    }
    byte(b, (uint8_t)value);
}

static void number(Buffer *b, unsigned field, uint64_t value)
{
    uint_wire(b, field * 8u);
    uint_wire(b, value);
}

static void blob(Buffer *b, unsigned field, const void *data, size_t size)
{
    uint_wire(b, field * 8u + 2u);
    uint_wire(b, size);
    assert(size <= sizeof(b->data) - b->size);
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static Buffer wrap(unsigned field, Buffer child)
{
    Buffer b = {0};
    blob(&b, field, child.data, child.size);
    return b;
}

static Buffer seconds(uint64_t remaining, uint64_t max)
{
    Buffer b = {0};
    number(&b, 1, 0);
    number(&b, 4, max);
    number(&b, 2, remaining);
    return b;
}

static void product(Buffer *body, uint64_t remaining, uint64_t max)
{
    Buffer user = {0};
    Buffer access = wrap(11, seconds(remaining, max));
    blob(&user, 1, "Q-FAKE-USER", 11);
    blob(&user, 2, access.data, access.size);
    Buffer list = wrap(2, wrap(2, user));
    blob(body, 2, list.data, list.size);
}

static void account(Buffer *body, uint64_t remaining, uint64_t max)
{
    Buffer user = {0};
    Buffer list = wrap(2, seconds(remaining, max));
    blob(&user, 1, "Q-FAKE-USER", 11);
    blob(&user, 2, list.data, list.size);
    blob(body, 3, user.data, user.size);
}

static void expect(const Buffer *body, enum X4CloudTimeSource source, uint64_t remaining,
    uint64_t max, bool unlimited)
{
    X4CloudTime v;
    assert(x4_cloud_time_parse(body->data, body->size, &v, NULL));
    assert(v.source == source && v.remaining_s == remaining && v.max_s == max);
    assert(v.unlimited == unlimited);
}

static void reject(const void *body, size_t length)
{
    X4CloudTime v = {.source = X4_CLOUD_TIME_ACCOUNT, .remaining_s = 1, .max_s = 1, .unlimited = true};
    assert(!x4_cloud_time_parse(body, length, &v, NULL));
    assert(v.source == X4_CLOUD_TIME_NONE && v.remaining_s == 0 && v.max_s == 0 && !v.unlimited);
}

static bool has_field(const X4CloudTimeShape *shape, unsigned level, uint32_t parent,
    uint32_t number, unsigned wire)
{
    for (size_t i = 0; i < shape->count; ++i) {
        const X4CloudTimeField *f = &shape->fields[i];
        if (f->level == level && f->parent == parent && f->number == number && f->wire == wire)
            return true;
    }
    return false;
}

static bool contains(const void *data, size_t size, const char *needle)
{
    const uint8_t *bytes = data;
    size_t length = strlen(needle);
    for (size_t i = 0; i + length <= size; ++i)
        if (!memcmp(bytes + i, needle, length)) return true;
    return false;
}

static void protocol(void)
{
    Buffer body = {0};
    product(&body, 900, 54000);
    expect(&body, X4_CLOUD_TIME_PRODUCT, 900, 54000, false);
    product(&body, UINT64_MAX, UINT64_MAX);
    product(&body, 300, 36000);
    product(&body, 500, 18000);
    expect(&body, X4_CLOUD_TIME_PRODUCT, 300, 36000, false);
    account(&body, 15000, 54000);
    expect(&body, X4_CLOUD_TIME_ACCOUNT, 15000, 54000, false);

    Buffer pass = {0};
    blob(&pass, 1, "Q-FAKE-USER", 11);
    blob(&body, 1, pass.data, pass.size);
    number(&body, 87, UINT64_MAX);
    blob(&body, 90, "Q-FAKE-SECRET", 13);
    uint_wire(&body, 91 * 8 + 1);
    for (unsigned i = 0; i < 8; ++i) byte(&body, UINT8_C(0xff));
    uint_wire(&body, 92 * 8 + 5);
    for (unsigned i = 0; i < 4; ++i) byte(&body, UINT8_C(0xff));
    uint_wire(&body, 93 * 8 + 3);
    number(&body, 2, 10);
    uint_wire(&body, 93 * 8 + 4);
    X4CloudTime v;
    X4CloudTimeShape shape;
    assert(x4_cloud_time_parse(body.data, body.size, &v, &shape));
    assert(v.source == X4_CLOUD_TIME_ACCOUNT && v.remaining_s == 15000 && v.max_s == 54000);
    assert(shape.length == body.size);
    assert(has_field(&shape, 1, 0, 87, 0));
    assert(has_field(&shape, 1, 0, 90, 2));
    assert(has_field(&shape, 1, 0, 91, 1));
    assert(has_field(&shape, 1, 0, 92, 5));
    assert(has_field(&shape, 1, 0, 93, 3));
    assert(has_field(&shape, 2, 1, 1, 2));
    assert(has_field(&shape, 2, 3, 2, 2));
    assert(!contains(&shape, sizeof(shape), "Q-FAKE-SECRET"));
    assert(!contains(&shape, sizeof(shape), "Q-FAKE-USER"));

    body = (Buffer){0};
    product(&body, 0, 36000);
    account(&body, UINT64_MAX, UINT64_MAX);
    expect(&body, X4_CLOUD_TIME_ACCOUNT, 0, 0, true);
    body = (Buffer){0};
    product(&body, UINT64_MAX, UINT64_MAX);
    expect(&body, X4_CLOUD_TIME_PRODUCT, 0, 0, true);
    body = (Buffer){0};
    account(&body, 0, 54000);
    expect(&body, X4_CLOUD_TIME_ACCOUNT, 0, 54000, false);

    reject(NULL, 0);
    reject(body.data, 0);
    reject(body.data, X4_CLOUD_TIME_BODY_MAX + 1u);
    reject(body.data, body.size - 1u);
    const uint8_t truncated_tag[] = {0x80};
    const uint8_t oversized_blob[] = {0x1a, 0xff, 0xff, 0xff, 0x7f, 0};
    const uint8_t bad_u64[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2};
    const uint8_t tag_zero[] = {0};
    const uint8_t unexpected_group_end[] = {0x0c};
    reject(truncated_tag, sizeof(truncated_tag));
    reject(oversized_blob, sizeof(oversized_blob));
    reject(bad_u64, sizeof(bad_u64));
    reject(tag_zero, sizeof(tag_zero));
    reject(unexpected_group_end, sizeof(unexpected_group_end));
    byte(&body, 0x80);
    reject(body.data, body.size);
    body = (Buffer){0};
    account(&body, UINT64_MAX - 1u, UINT64_MAX - 1u);
    reject(body.data, body.size);
    body = (Buffer){0};
    product(&body, 12, 11);
    expect(&body, X4_CLOUD_TIME_PRODUCT, 12, 11, false);
    body = (Buffer){0};
    product(&body, 10, 20);
    for (unsigned i = 0; i < 12; ++i) uint_wire(&body, 93 * 8 + 3);
    for (unsigned i = 0; i < 12; ++i) uint_wire(&body, 93 * 8 + 4);
    reject(body.data, body.size);
}

static void json_fallback(void)
{
    const char direct[] =
        "{\"remainingGameplayTimeInSeconds\":\"15000\",\"maxGameplayTimeInSeconds\":54000,"
        "\"Q-FAKE-USER\":{\"remainingGameplayTimeInSeconds\":0,\"maxGameplayTimeInSeconds\":36000},"
        "\"other\":\"Q-FAKE-SECRET\"}";
    X4CloudTimeShape shape;
    X4CloudTime v;
    assert(x4_cloud_time_parse(direct, sizeof(direct) - 1, &v, &shape));
    assert(v.source == X4_CLOUD_TIME_JSON && v.remaining_s == 15000 && v.max_s == 54000);
    assert(shape.length == sizeof(direct) - 1 && !shape.count);
    assert(!contains(&shape, sizeof(shape), "Q-FAKE-SECRET"));
    assert(!contains(&shape, sizeof(shape), "Q-FAKE-USER"));
    const char nested[] = "[{\"remainingGameplayTimeInSeconds\":300,\"maxGameplayTimeInSeconds\":900},"
        "{\"remainingGameplayTimeInSeconds\":200,\"maxGameplayTimeInSeconds\":1000}]";
    assert(x4_cloud_time_parse(nested, sizeof(nested) - 1, &v, NULL));
    assert(v.source == X4_CLOUD_TIME_JSON && v.remaining_s == 200 && v.max_s == 1000);
    const char forever[] = "{\"remainingGameplayTimeInSeconds\":18446744073709551615}";
    assert(x4_cloud_time_parse(forever, sizeof(forever) - 1, &v, NULL) && v.unlimited);
    const char too_large[] = "{\"remainingGameplayTimeInSeconds\":18446744073709551616}";
    reject(too_large, sizeof(too_large) - 1);
    const char non_integer[] = "{\"remainingGameplayTimeInSeconds\":1e4,\"maxGameplayTimeInSeconds\":20000}";
    reject(non_integer, sizeof(non_integer) - 1);
    const char too_deep[] = "[[[[[[[[[{\"remainingGameplayTimeInSeconds\":1,\"maxGameplayTimeInSeconds\":2}]]]]]]]]]";
    reject(too_deep, sizeof(too_deep) - 1);
    const char duplicate[] = "{\"remainingGameplayTimeInSeconds\":1,\"remainingGameplayTimeInSeconds\":2,"
        "\"maxGameplayTimeInSeconds\":2}";
    reject(duplicate, sizeof(duplicate) - 1);
}

#define AT(s) ((s) * UINT64_C(1000000))

static void incoherent_quota(void)
{
    const char empty_max[] = "{\"remainingGameplayTimeInSeconds\":0,\"maxGameplayTimeInSeconds\":0}";
    X4CloudTimeState s = {.playing = true};
    X4CloudTime valid = {.source=X4_CLOUD_TIME_ACCOUNT,.remaining_s=1300,.max_s=36000};
    X4CloudTime bad;
    x4_cloud_time_apply(&s, valid, AT(1));
    x4_cloud_time_apply(&s, (X4CloudTime){0}, AT(2));
    assert(x4_cloud_time_current(&s, AT(2)).remaining_s == 1299);
    assert(x4_cloud_time_parse(empty_max, sizeof(empty_max)-1u, &bad, NULL));
    assert(bad.source == X4_CLOUD_TIME_JSON && !bad.unlimited && !bad.max_s);
    x4_cloud_time_apply(&s, bad, AT(3));
    assert(x4_cloud_time_current(&s, AT(3)).source == X4_CLOUD_TIME_NONE);
    assert(!x4_cloud_time_alerts(&s, AT(3)));
    x4_cloud_time_apply(&s, valid, AT(4));
    assert(x4_cloud_time_current(&s, AT(4)).remaining_s == 1300);

    Buffer body = {0};
    account(&body, 9000, 8000);
    product(&body, 18000, 36000);
    assert(x4_cloud_time_parse(body.data, body.size, &bad, NULL));
    assert(bad.source == X4_CLOUD_TIME_ACCOUNT && !bad.unlimited && bad.remaining_s > bad.max_s);
    x4_cloud_time_apply(&s, bad, AT(5));
    assert(x4_cloud_time_current(&s, AT(5)).source == X4_CLOUD_TIME_NONE);
    x4_cloud_time_apply(&s, valid, AT(6));
    assert(x4_cloud_time_current(&s, AT(6)).remaining_s == 1300);

    body = (Buffer){0};
    account(&body, UINT64_MAX, 0);
    expect(&body, X4_CLOUD_TIME_ACCOUNT, 0, 0, true);
    puts("PASS T34 incoherent quota clears stale countdown; missing response retains value; unlimited preserved");
}

static void countdown(void)
{
    X4CloudTimeState s = {0};
    X4CloudTime v = {.source = X4_CLOUD_TIME_ACCOUNT, .remaining_s = 1000, .max_s = 54000};
    assert(x4_cloud_time_current(&s, 0).source == X4_CLOUD_TIME_NONE);
    x4_cloud_time_apply(&s, v, AT(0));
    assert(x4_cloud_time_current(&s, AT(60)).remaining_s == 1000);
    x4_cloud_time_set_playing(&s, true, AT(60));
    assert(x4_cloud_time_current(&s, AT(160)).remaining_s == 900);
    assert(x4_cloud_time_alerts(&s, AT(159)) == 0);
    assert(x4_cloud_time_alerts(&s, AT(160)) == X4_CLOUD_TIME_ALERT_15);
    assert(x4_cloud_time_alerts(&s, AT(161)) == 0);
    x4_cloud_time_apply(&s, v, AT(160) + 900000);
    assert(x4_cloud_time_current(&s, AT(161)).remaining_s == 899);
    v.remaining_s = 500;
    x4_cloud_time_apply(&s, v, AT(170));
    assert(x4_cloud_time_current(&s, AT(200)).remaining_s == 470);
    X4CloudTime unlimited = {.source = X4_CLOUD_TIME_ACCOUNT, .unlimited = true};
    x4_cloud_time_apply(&s, unlimited, AT(200));
    assert(!x4_cloud_time_current(&s, AT(200)).unlimited);
    assert(x4_cloud_time_alerts(&s, AT(369)) == 0);
    assert(x4_cloud_time_alerts(&s, AT(370)) == X4_CLOUD_TIME_ALERT_5);
    assert(x4_cloud_time_alerts(&s, AT(371)) == 0);
    x4_cloud_time_set_playing(&s, false, AT(400));
    assert(x4_cloud_time_current(&s, AT(1000)).remaining_s == 270);
    v.remaining_s = 650;
    x4_cloud_time_apply(&s, v, AT(1001));
    assert(x4_cloud_time_current(&s, AT(1001)).remaining_s == 650);
    x4_cloud_time_set_playing(&s, true, AT(1002));
    assert(x4_cloud_time_alerts(&s, AT(1352)) == 0);
    assert(x4_cloud_time_current(&s, UINT64_MAX).remaining_s == 0);

    s = (X4CloudTimeState){0};
    x4_cloud_time_apply(&s, v, AT(1003));
    x4_cloud_time_set_playing(&s, true, AT(1004));
    assert(x4_cloud_time_alerts(&s, AT(1004)) == X4_CLOUD_TIME_ALERT_15);
    assert(x4_cloud_time_alerts(&s, AT(1354)) == X4_CLOUD_TIME_ALERT_5);
    s = (X4CloudTimeState){0};
    x4_cloud_time_apply(&s, unlimited, 0);
    x4_cloud_time_set_playing(&s, true, 0);
    assert(x4_cloud_time_current(&s, UINT64_MAX).unlimited);
    assert(x4_cloud_time_alerts(&s, UINT64_MAX) == 0);
    v.remaining_s = 290;
    x4_cloud_time_apply(&s, v, 100);
    assert(x4_cloud_time_alerts(&s, 100) == X4_CLOUD_TIME_ALERT_5);
    assert(x4_cloud_time_alerts(&s, 101) == 0);
}

static void malformed_sweep(void)
{
    uint32_t state = 1;
    for (unsigned round = 0; round < 4096; ++round) {
        uint8_t raw[67];
        for (size_t i = 0; i < sizeof(raw); ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            raw[i] = (uint8_t)state;
        }
        X4CloudTime value;
        X4CloudTimeShape shape;
        size_t length = round % (sizeof(raw) + 1);
        (void)x4_cloud_time_parse(raw, length, &value, &shape);
        assert(shape.length == length && shape.count <= sizeof(shape.fields) / sizeof(shape.fields[0]));
    }
}

int main(void)
{
    protocol();
    json_fallback();
    incoherent_quota();
    countdown();
    malformed_sweep();
    puts("cloud time parser, sanitized shape, countdown and alerts: PASS");
    return 0;
}
