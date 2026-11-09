/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/play_history.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_us(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static X4History history, restored;

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *path = argv[1];
    const char *first = "9N0QWXC09MPT";
    assert(!x4_history_load(&history, path) && !history.count);
    assert(x4_history_record(&history, first, 90000, 0, path));
    assert(x4_history_load(&restored, path));
    const X4HistoryEntry *entry = x4_history_find(&restored, "9n0qwxc09mpt");
    assert(entry && entry->last_session_s == 90000 && !entry->played_s);
    assert(x4_history_record(&history, first, 104400, 14400, path));
    assert(x4_history_load(&restored, path));
    entry = x4_history_find(&restored, first);
    assert(entry && entry->last_session_s == 104400 && entry->played_s == 14400);
    char last[X4_HISTORY_LABEL], played[X4_HISTORY_LABEL];
    x4_history_text(entry, 104400 + 86400, false, last, played);
    assert(!strcmp(last, "Ayer") && !strcmp(played, "4 h"));
    x4_history_text(entry, 104400 + 3 * 86400, true, last, played);
    assert(!strcmp(last, "3 days ago") && !strcmp(played, "4 h"));
    x4_history_text(NULL, 200000, true, last, played);
    assert(!last[0] && !played[0]);
    assert(!x4_history_record(&history, "persona@example.test", 104400, 10, path));
    char invalid[512];
    int n = snprintf(invalid, sizeof(invalid), "%s/child", path);
    assert(n > 0 && (size_t)n < sizeof(invalid));
    assert(!x4_history_record(&history, first, 104500, 7, invalid));
    assert(history.count == 1 && x4_history_find(&history, first)->played_s == 14400);

    FILE *file = fopen(path, "rb");
    unsigned char raw[64];
    assert(file && fread(raw, 1, sizeof(raw), file) == sizeof(raw));
    assert(fgetc(file) == EOF && fclose(file) == 0);
    assert(!memcmp(raw, "X4HIST1", 8) && raw[8] == 1);
    assert(!memcmp(raw + 16, first, strlen(first)));
    for (size_t i = 16 + strlen(first); i < 48; ++i) assert(raw[i] == 0);

    for (unsigned i = 1; i <= X4_HISTORY_MAX; ++i) {
        char id[X4_PRODUCT_ID_CAP];
        snprintf(id, sizeof(id), "GAME%08u", i);
        assert(x4_history_record(&history, id, 110000 + i, i, i == X4_HISTORY_MAX ? path : NULL));
    }
    assert(history.count == X4_HISTORY_MAX && !x4_history_find(&history, first));
    assert(x4_history_load(&restored, path) && restored.count == X4_HISTORY_MAX);
    assert(x4_history_find(&restored, "GAME00000500")->played_s == 500);

    uint64_t max_write_us = 0;
    for (unsigned i = 0; i < 30; ++i) {
        const uint64_t start = monotonic_us();
        assert(x4_history_record(&history, "GAME00000500", 120000 + i, 0, path));
        const uint64_t elapsed = monotonic_us() - start;
        if (elapsed > max_write_us) max_write_us = elapsed;
        printf("HISTORY48_WRITE sample=%u records=%u us=%llu\n", i, history.count,
            (unsigned long long)elapsed);
    }
    printf("HISTORY48_WRITE_MAX records=%u max_us=%llu\n",
        history.count, (unsigned long long)max_write_us);
    file = fopen(path, "r+b");
    assert(file && fseek(file, 56, SEEK_SET) == 0);
    int byte = fgetc(file);
    assert(byte != EOF && fseek(file, 56, SEEK_SET) == 0);
    assert(fputc(byte ^ 0x01, file) != EOF && fclose(file) == 0);
    assert(!x4_history_load(&restored, path) && !restored.count);
    file = fopen(path, "wb");
    assert(file && fputs("X4HIST1", file) >= 0 && fclose(file) == 0);
    assert(!x4_history_load(&restored, path) && !restored.count);
    puts("PASS historial: 500 IDs, inicio/final, formato sin cuenta, respaldo atómico y corrupción");
    return 0;
}
