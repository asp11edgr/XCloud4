/* SPDX-License-Identifier: GPL-3.0-only */
#include "server_settings.h"
#include "lang.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define x4_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define x4_mkdir(p) mkdir(p, 0777)
#endif

#define X4_SERVER_MAGIC 0x31565358u /* "XSV1" */
#define X4_SERVER_VERSION 3u
#define X4_SERVER_DEFAULT_PATH "/data/xcloud4/server-v1.bin"

typedef struct {
    char name[64];
    char host[128];
    uint32_t is_default;
    int32_t last_game_ms;
} X4ServerDiskEntry;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t has_saved_choice;
    char selected_name[64];
    uint32_t count;
    X4ServerDiskEntry entries[X4_SERVER_MAX_REGIONS];
    uint32_t checksum;
} X4ServerStorageRecord;

static X4ServerStorageRecord s_disk_record;

typedef struct {
    const char *raw;
    enum X4TextId text;
} RegionTrans;

static bool str_equals_case_insensitive(const char *s1, const char *s2)
{
    if (!s1 || !s2) return false;
    while (*s1 && *s2) {
        unsigned char c1 = (unsigned char)*s1;
        unsigned char c2 = (unsigned char)*s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 = (unsigned char)(c1 + ('a' - 'A'));
        if (c2 >= 'A' && c2 <= 'Z') c2 = (unsigned char)(c2 + ('a' - 'A'));
        if (c1 != c2) return false;
        s1++;
        s2++;
    }
    return *s1 == *s2;
}

static const RegionTrans known_regions[] = {
    {"AustraliaEast", X4_TR_50_REGION_AUSTRALIAEAST},
    {"AustraliaSouthEast", X4_TR_50_REGION_AUSTRALIASOUTHEAST},
    {"BrazilSouth", X4_TR_50_REGION_BRAZILSOUTH},
    {"CanadaCentral", X4_TR_50_REGION_CANADACENTRAL},
    {"CanadaEast", X4_TR_50_REGION_CANADAEAST},
    {"CentralIndia", X4_TR_50_REGION_CENTRALINDIA},
    {"CentralUS", X4_TR_50_REGION_CENTRALUS},
    {"ChileCentral", X4_TR_50_REGION_CHILECENTRAL},
    {"EastAsia", X4_TR_50_REGION_EASTASIA},
    {"EastUS", X4_TR_50_REGION_EASTUS},
    {"EastUS2", X4_TR_50_REGION_EASTUS2},
    {"FranceCentral", X4_TR_50_REGION_FRANCECENTRAL},
    {"GermanyWestCentral", X4_TR_50_REGION_GERMANYWESTCENTRAL},
    {"ItalyNorth", X4_TR_50_REGION_ITALYNORTH},
    {"JapanEast", X4_TR_50_REGION_JAPANEAST},
    {"JapanWest", X4_TR_50_REGION_JAPANWEST},
    {"KoreaCentral", X4_TR_50_REGION_KOREACENTRAL},
    {"KoreaSouth", X4_TR_50_REGION_KOREASOUTH},
    {"MexicoCentral", X4_TR_50_REGION_MEXICOCENTRAL},
    {"NorthCentralUS", X4_TR_50_REGION_NORTHCENTRALUS},
    {"NorthEurope", X4_TR_50_REGION_NORTHEUROPE},
    {"NorwayEast", X4_TR_50_REGION_NORWAYEAST},
    {"PolandCentral", X4_TR_50_REGION_POLANDCENTRAL},
    {"SouthAfricaNorth", X4_TR_50_REGION_SOUTHAFRICANORTH},
    {"SouthCentralUS", X4_TR_50_REGION_SOUTHCENTRALUS},
    {"SoutheastAsia", X4_TR_50_REGION_SOUTHEASTASIA},
    {"SouthIndia", X4_TR_50_REGION_SOUTHINDIA},
    {"SpainCentral", X4_TR_50_REGION_SPAINCENTRAL},
    {"SwedenCentral", X4_TR_50_REGION_SWEDENCENTRAL},
    {"UAENorth", X4_TR_50_REGION_UAENORTH},
    {"UKSouth", X4_TR_50_REGION_UKSOUTH},
    {"UKWest", X4_TR_50_REGION_UKWEST},
    {"WestCentralUS", X4_TR_50_REGION_WESTCENTRALUS},
    {"WestEurope", X4_TR_50_REGION_WESTEUROPE},
    {"WestUS", X4_TR_50_REGION_WESTUS},
    {"WestUS2", X4_TR_50_REGION_WESTUS2},
    {"WestUS3", X4_TR_50_REGION_WESTUS3},
};

enum X4Continent {
    X4_CONTINENT_AMERICA = 1,
    X4_CONTINENT_EUROPE = 2,
    X4_CONTINENT_ASIA_OCEANIA = 3,
    X4_CONTINENT_OTHER = 4
};

static enum X4Continent get_region_continent(const char *name)
{
    if (!name || !name[0]) return X4_CONTINENT_OTHER;

    if (str_equals_case_insensitive(name, "BrazilSouth") ||
        str_equals_case_insensitive(name, "CanadaCentral") ||
        str_equals_case_insensitive(name, "CanadaEast") ||
        str_equals_case_insensitive(name, "CentralUS") ||
        str_equals_case_insensitive(name, "ChileCentral") ||
        str_equals_case_insensitive(name, "EastUS") ||
        str_equals_case_insensitive(name, "EastUS2") ||
        str_equals_case_insensitive(name, "MexicoCentral") ||
        str_equals_case_insensitive(name, "NorthCentralUS") ||
        str_equals_case_insensitive(name, "SouthCentralUS") ||
        str_equals_case_insensitive(name, "WestCentralUS") ||
        str_equals_case_insensitive(name, "WestUS") ||
        str_equals_case_insensitive(name, "WestUS2") ||
        str_equals_case_insensitive(name, "WestUS3")) {
        return X4_CONTINENT_AMERICA;
    }

    if (str_equals_case_insensitive(name, "FranceCentral") ||
        str_equals_case_insensitive(name, "GermanyWestCentral") ||
        str_equals_case_insensitive(name, "ItalyNorth") ||
        str_equals_case_insensitive(name, "NorthEurope") ||
        str_equals_case_insensitive(name, "NorwayEast") ||
        str_equals_case_insensitive(name, "PolandCentral") ||
        str_equals_case_insensitive(name, "SpainCentral") ||
        str_equals_case_insensitive(name, "SwedenCentral") ||
        str_equals_case_insensitive(name, "UKSouth") ||
        str_equals_case_insensitive(name, "UKWest") ||
        str_equals_case_insensitive(name, "WestEurope")) {
        return X4_CONTINENT_EUROPE;
    }

    if (str_equals_case_insensitive(name, "AustraliaEast") ||
        str_equals_case_insensitive(name, "AustraliaSouthEast") ||
        str_equals_case_insensitive(name, "CentralIndia") ||
        str_equals_case_insensitive(name, "EastAsia") ||
        str_equals_case_insensitive(name, "JapanEast") ||
        str_equals_case_insensitive(name, "JapanWest") ||
        str_equals_case_insensitive(name, "KoreaCentral") ||
        str_equals_case_insensitive(name, "KoreaSouth") ||
        str_equals_case_insensitive(name, "SoutheastAsia") ||
        str_equals_case_insensitive(name, "SouthIndia")) {
        return X4_CONTINENT_ASIA_OCEANIA;
    }

    return X4_CONTINENT_OTHER;
}

static char custom_path[256] = {0};
static atomic_flag server_lock = ATOMIC_FLAG_INIT;
static inline void lock_server(void) {
    while (atomic_flag_test_and_set_explicit(&server_lock, memory_order_acquire)) {}
}
static inline void unlock_server(void) {
    atomic_flag_clear_explicit(&server_lock, memory_order_release);
}

static inline void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || !cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static bool stored_has_saved_choice = false;
static char stored_selected_name[64] = {0};
static unsigned current_region_count = 0;
static X4ServerRegion current_regions[X4_SERVER_MAX_REGIONS];
static atomic_bool server_unavailable_flag = false;

static atomic_bool awaiting_choice_flag = false;
static atomic_bool first_choice_completed_flag = false;
static atomic_bool first_time_ui_drawn = false;

static uint32_t calc_checksum(const X4ServerStorageRecord *r)
{
    uint32_t sum = r->magic ^ (r->version << 16) ^ (r->version >> 16) ^
                   (r->has_saved_choice * 0x9e3779b9u) ^ (r->count * 0x85ebca6bu);
    const uint8_t *pname = (const uint8_t *)r->selected_name;
    for (size_t i = 0; i < sizeof(r->selected_name); ++i) {
        sum = (sum * 31u) + pname[i];
    }
    for (uint32_t i = 0; i < r->count && i < X4_SERVER_MAX_REGIONS; ++i) {
        const uint8_t *pe = (const uint8_t *)&r->entries[i];
        for (size_t j = 0; j < sizeof(X4ServerDiskEntry); ++j) {
            sum = (sum * 33u) + pe[j];
        }
    }
    return sum ^ 0x5a5aa5a5u;
}

static const char *get_active_path(void)
{
    return custom_path[0] ? custom_path : X4_SERVER_DEFAULT_PATH;
}

void x4_server_settings_set_path(const char *path)
{
    lock_server();
    if (path && path[0]) {
        snprintf(custom_path, sizeof(custom_path), "%s", path);
    } else {
        custom_path[0] = 0;
    }
    unlock_server();
}

bool x4_server_settings_validate_name(const char *name)
{
    if (!name || !name[0]) return false;
    size_t len = strlen(name);
    if (len >= 64) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool x4_server_settings_validate_host(const char *host)
{
    if (!host || !host[0]) return false;
    size_t len = strlen(host);
    if (len >= 128) return false;
    static const char suffix[] = ".gssv-play-prod.xboxlive.com";
    size_t s_len = sizeof(suffix) - 1;
    if (len <= s_len) return false;
    if (memcmp(host + len - s_len, suffix, s_len) != 0) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = host[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

static const char *region_display_name(const char *raw_name, enum X4Lang language)
{
    if (!raw_name || !raw_name[0]) return "";
    for (size_t i = 0; i < sizeof(known_regions) / sizeof(known_regions[0]); ++i) {
        if (str_equals_case_insensitive(raw_name, known_regions[i].raw)) {
            return x4_tr_for_lang(language, known_regions[i].text);
        }
    }
    return raw_name;
}

const char *x4_server_region_display_name(const char *raw_name)
{
    return region_display_name(raw_name, x4_lang_current());
}

static void ensure_parent_dirs(const char *path)
{
    char dir[256];
    size_t len = strlen(path);
    if (len >= sizeof(dir)) return;
    memcpy(dir, path, len + 1);
    char *slash = strrchr(dir, '/');
#if defined(_WIN32)
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash && slash != dir) {
        *slash = '\0';
        if (strncmp(dir, "/data/xcloud4", 13) == 0) {
            (void)x4_mkdir("/data");
        }
        (void)x4_mkdir(dir);
    }
}

static bool save_to_disk_locked(void)
{
    const char *path = get_active_path();
    ensure_parent_dirs(path);
    char tmp[270];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;

    X4ServerStorageRecord *rec = &s_disk_record;
    memset(rec, 0, sizeof(*rec));
    rec->magic = X4_SERVER_MAGIC;
    rec->version = X4_SERVER_VERSION;
    rec->has_saved_choice = stored_has_saved_choice ? 1 : 0;
    copy_str(rec->selected_name, sizeof(rec->selected_name), stored_selected_name);
    rec->count = current_region_count < X4_SERVER_MAX_REGIONS ? current_region_count : X4_SERVER_MAX_REGIONS;
    for (uint32_t i = 0; i < rec->count; ++i) {
        copy_str(rec->entries[i].name, sizeof(rec->entries[i].name), current_regions[i].name);
        copy_str(rec->entries[i].host, sizeof(rec->entries[i].host),
                 current_regions[i].api_host[0] ? current_regions[i].api_host : current_regions[i].host);
        rec->entries[i].is_default = current_regions[i].is_default ? 1 : 0;
        rec->entries[i].last_game_ms = current_regions[i].last_game_ms;
    }
    rec->checksum = calc_checksum(rec);

    size_t written = fwrite(rec, 1, sizeof(*rec), f);
    fflush(f);
    fclose(f);
    if (written != sizeof(*rec)) {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

static int compare_regions(const X4ServerRegion *a, const X4ServerRegion *b)
{
    bool a_meas = (a->last_game_ms >= 1 && a->last_game_ms <= 2000);
    bool b_meas = (b->last_game_ms >= 1 && b->last_game_ms <= 2000);

    /* 1. Measured before unmeasured */
    if (a_meas && !b_meas) return -1;
    if (!a_meas && b_meas) return 1;

    /* 2. Measured: lower ms first, then name */
    if (a_meas && b_meas) {
        if (a->last_game_ms != b->last_game_ms) {
            return a->last_game_ms - b->last_game_ms;
        }
        return strcmp(a->name, b->name);
    }

    /* 3. Unmeasured: continent group (America, Europe, Asia/Oceania, Other), then name */
    int cont_a = (int)get_region_continent(a->name);
    int cont_b = (int)get_region_continent(b->name);
    if (cont_a != cont_b) {
        return cont_a - cont_b;
    }
    return strcmp(a->name, b->name);
}

static void sort_regions_locked(void)
{
    unsigned count = current_region_count;
    for (unsigned i = 0; i < count - 1; ++i) {
        for (unsigned j = i + 1; j < count; ++j) {
            if (compare_regions(&current_regions[i], &current_regions[j]) > 0) {
                X4ServerRegion tmp = current_regions[i];
                current_regions[i] = current_regions[j];
                current_regions[j] = tmp;
            }
        }
    }
}

static void load_from_disk_locked(void)
{
    stored_has_saved_choice = false;
    stored_selected_name[0] = 0;
    current_region_count = 0;

    const char *path = get_active_path();
    FILE *f = fopen(path, "rb");
    if (!f) return;

    X4ServerStorageRecord *rec = &s_disk_record;
    size_t n = fread(rec, 1, sizeof(*rec), f);
    fclose(f);
    if (n != sizeof(*rec)) return;
    if (rec->magic != X4_SERVER_MAGIC || rec->version != X4_SERVER_VERSION) return;
    if (rec->count > X4_SERVER_MAX_REGIONS) return;
    if (rec->checksum != calc_checksum(rec)) return;

    /* Validate selected name if non-empty */
    if (rec->selected_name[0]) {
        rec->selected_name[sizeof(rec->selected_name) - 1] = 0;
        if (!x4_server_settings_validate_name(rec->selected_name)) {
            rec->selected_name[0] = 0;
        }
    }

    /* Validate entries */
    unsigned valid_count = 0;
    for (uint32_t i = 0; i < rec->count; ++i) {
        rec->entries[i].name[sizeof(rec->entries[i].name) - 1] = 0;
        rec->entries[i].host[sizeof(rec->entries[i].host) - 1] = 0;
        if (x4_server_settings_validate_name(rec->entries[i].name) &&
            (rec->entries[i].host[0] == '\0' || x4_server_settings_validate_host(rec->entries[i].host))) {
            copy_str(current_regions[valid_count].name, sizeof(current_regions[valid_count].name), rec->entries[i].name);
            if (rec->entries[i].host[0] != '\0') {
                copy_str(current_regions[valid_count].api_host, sizeof(current_regions[valid_count].api_host), rec->entries[i].host);
                copy_str(current_regions[valid_count].host, sizeof(current_regions[valid_count].host), rec->entries[i].host);
            } else {
                char lower[64];
                size_t k = 0;
                for (size_t c = 0; rec->entries[i].name[c] && k < sizeof(lower) - 1; ++c) {
                    char ch = rec->entries[i].name[c];
                    if (ch != ' ' && ch != '-' && ch != '_') {
                        lower[k++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
                    }
                }
                lower[k] = '\0';
                snprintf(current_regions[valid_count].host, sizeof(current_regions[valid_count].host), "%s.gssv-play-prod.xboxlive.com", lower);
                copy_str(current_regions[valid_count].api_host, sizeof(current_regions[valid_count].api_host), current_regions[valid_count].host);
            }
            current_regions[valid_count].net_test_host[0] = 0;
            snprintf(current_regions[valid_count].base_uri, sizeof(current_regions[valid_count].base_uri), "https://%s", current_regions[valid_count].host);
            current_regions[valid_count].is_default = (rec->entries[i].is_default != 0);

            int32_t ms = rec->entries[i].last_game_ms;
            if (ms >= 1 && ms <= 2000) {
                current_regions[valid_count].last_game_ms = (int)ms;
            } else {
                current_regions[valid_count].last_game_ms = 0;
            }
            valid_count++;
        }
    }

    current_region_count = valid_count;
    sort_regions_locked();
    stored_has_saved_choice = (rec->has_saved_choice != 0);
    copy_str(stored_selected_name, sizeof(stored_selected_name), rec->selected_name);
}

void x4_server_settings_init(void)
{
    lock_server();
    load_from_disk_locked();
    unlock_server();
}

bool x4_server_settings_has_saved_choice(void)
{
    lock_server();
    bool res = stored_has_saved_choice;
    unlock_server();
    return res;
}

const char *x4_server_settings_get_selected(void)
{
    lock_server();
    const char *res = stored_selected_name;
    unlock_server();
    return res;
}

bool x4_server_settings_set_selected(const char *name, bool save_choice)
{
    lock_server();
    if (!name || !name[0]) {
        stored_selected_name[0] = 0;
    } else {
        if (!x4_server_settings_validate_name(name)) {
            unlock_server();
            return false;
        }
        copy_str(stored_selected_name, sizeof(stored_selected_name), name);
    }
    if (save_choice) {
        stored_has_saved_choice = true;
    }
    bool saved = save_to_disk_locked();
    unlock_server();
    return saved;
}

unsigned x4_server_settings_get_regions(X4ServerRegion *out, unsigned max_out)
{
    lock_server();
    unsigned count = current_region_count < max_out ? current_region_count : max_out;
    if (out && count) {
        memcpy(out, current_regions, count * sizeof(X4ServerRegion));
    }
    unlock_server();
    return count;
}

unsigned x4_server_settings_get_region_count(void)
{
    lock_server();
    unsigned c = current_region_count;
    unlock_server();
    return c;
}

bool x4_server_settings_get_region(unsigned index, X4ServerRegion *out)
{
    if (!out) return false;
    lock_server();
    if (index >= current_region_count) {
        unlock_server();
        return false;
    }
    *out = current_regions[index];
    unlock_server();
    return true;
}

bool x4_server_settings_get_region_name(unsigned index, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    lock_server();
    if (index >= current_region_count) {
        unlock_server();
        return false;
    }
    copy_str(out, cap, current_regions[index].name);
    unlock_server();
    return true;
}

int x4_server_settings_find_index(const char *name)
{
    if (!name || !name[0]) return 0;
    lock_server();
    for (unsigned i = 0; i < current_region_count; ++i) {
        if (str_equals_case_insensitive(name, current_regions[i].name)) {
            unlock_server();
            return (int)(i + 1);
        }
    }
    unlock_server();
    return 0;
}

bool x4_server_settings_update_regions(const X4ServerRegion *regions, unsigned count)
{
    if (!regions || count == 0) return false;
    lock_server();

    X4ServerRegion temp_regions[X4_SERVER_MAX_REGIONS];
    unsigned valid_count = 0;
    for (unsigned i = 0; i < count && valid_count < X4_SERVER_MAX_REGIONS; ++i) {
        const char *chk_host = regions[i].api_host[0] ? regions[i].api_host : regions[i].host;
        if (x4_server_settings_validate_name(regions[i].name) &&
            x4_server_settings_validate_host(chk_host)) {

            /* Preserve previously measured ms if already known for this region */
            int prev_ms = 0;
            for (unsigned p = 0; p < current_region_count; ++p) {
                if (str_equals_case_insensitive(current_regions[p].name, regions[i].name)) {
                    prev_ms = current_regions[p].last_game_ms;
                    break;
                }
            }

            temp_regions[valid_count] = regions[i];
            if (!temp_regions[valid_count].api_host[0])
                copy_str(temp_regions[valid_count].api_host, sizeof(temp_regions[valid_count].api_host), chk_host);
            if (!temp_regions[valid_count].host[0])
                copy_str(temp_regions[valid_count].host, sizeof(temp_regions[valid_count].host), chk_host);

            if (prev_ms > 0) {
                temp_regions[valid_count].last_game_ms = prev_ms;
            } else if (regions[i].last_game_ms >= 1 && regions[i].last_game_ms <= 2000) {
                temp_regions[valid_count].last_game_ms = regions[i].last_game_ms;
            } else {
                temp_regions[valid_count].last_game_ms = 0;
            }
            valid_count++;
        }
    }
    if (valid_count == 0) {
        unlock_server();
        return false;
    }
    memcpy(current_regions, temp_regions, valid_count * sizeof(X4ServerRegion));
    current_region_count = valid_count;
    sort_regions_locked();

    /* Check if currently selected server is still in the list */
    if (stored_selected_name[0]) {
        bool found = false;
        for (unsigned i = 0; i < current_region_count; ++i) {
            if (str_equals_case_insensitive(stored_selected_name, current_regions[i].name)) {
                found = true;
                break;
            }
        }
        if (!found) {
            atomic_store(&server_unavailable_flag, true);
            stored_selected_name[0] = 0; /* Fall back to automatic */
        }
    }

    save_to_disk_locked();
    unlock_server();
    return true;
}

bool x4_server_settings_record_game_rtt(const char *region_name, int rtt_ms)
{
    if (!region_name || !region_name[0] || rtt_ms < 1 || rtt_ms > 2000) return false;
    if (!x4_server_settings_validate_name(region_name)) return false;
    lock_server();
    bool found = false;
    for (unsigned i = 0; i < current_region_count; ++i) {
        if (str_equals_case_insensitive(current_regions[i].name, region_name)) {
            current_regions[i].last_game_ms = rtt_ms;
            found = true;
            break;
        }
    }
    if (!found && current_region_count < X4_SERVER_MAX_REGIONS) {
        unsigned idx = current_region_count++;
        memset(&current_regions[idx], 0, sizeof(current_regions[idx]));
        copy_str(current_regions[idx].name, sizeof(current_regions[idx].name), region_name);
        current_regions[idx].last_game_ms = rtt_ms;

        char lower[64];
        size_t k = 0;
        for (size_t c = 0; region_name[c] && k < sizeof(lower) - 1; ++c) {
            char ch = region_name[c];
            if (ch != ' ' && ch != '-' && ch != '_') {
                lower[k++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
            }
        }
        lower[k] = '\0';
        snprintf(current_regions[idx].host, sizeof(current_regions[idx].host), "%s.gssv-play-prod.xboxlive.com", lower);
        copy_str(current_regions[idx].api_host, sizeof(current_regions[idx].api_host), current_regions[idx].host);
        snprintf(current_regions[idx].base_uri, sizeof(current_regions[idx].base_uri), "https://%s", current_regions[idx].host);
        found = true;
    }
    if (found) {
        sort_regions_locked();
        save_to_disk_locked();
    }
    unlock_server();
    return found;
}

int x4_server_settings_get_region_rtt(const char *region_name)
{
    if (!region_name || !region_name[0]) return 0;
    lock_server();
    for (unsigned i = 0; i < current_region_count; ++i) {
        if (str_equals_case_insensitive(current_regions[i].name, region_name)) {
            int ms = current_regions[i].last_game_ms;
            unlock_server();
            return ms;
        }
    }
    unlock_server();
    return 0;
}

const char *x4_server_settings_default_region_name(void)
{
    lock_server();
    for (unsigned i = 0; i < current_region_count; ++i) {
        if (current_regions[i].is_default) {
            const char *res = current_regions[i].name;
            unlock_server();
            return res;
        }
    }
    if (current_region_count > 0) {
        const char *res = current_regions[0].name;
        unlock_server();
        return res;
    }
    unlock_server();
    return "";
}

const char *x4_server_settings_active_display_name(void)
{
    lock_server();
    if (stored_selected_name[0]) {
        for (unsigned i = 0; i < current_region_count; ++i) {
            if (!strcmp(stored_selected_name, current_regions[i].name)) {
                const char *res = x4_server_region_display_name(current_regions[i].name);
                unlock_server();
                return res;
            }
        }
        const char *res = x4_server_region_display_name(stored_selected_name);
        unlock_server();
        return res;
    }
    /* Automatic: return default region's display name */
    for (unsigned i = 0; i < current_region_count; ++i) {
        if (current_regions[i].is_default) {
            const char *res = x4_server_region_display_name(current_regions[i].name);
            unlock_server();
            return res;
        }
    }
    if (current_region_count > 0) {
        const char *res = x4_server_region_display_name(current_regions[0].name);
        unlock_server();
        return res;
    }
    unlock_server();
    return x4_tr(X4_TR_SERVER_OPT_AUTO);
}

bool x4_server_settings_server_not_available(void)
{
    return atomic_load(&server_unavailable_flag);
}

void x4_server_settings_clear_not_available(void)
{
    atomic_store(&server_unavailable_flag, false);
}

bool x4_server_settings_awaiting_first_choice(void)
{
    return atomic_load(&awaiting_choice_flag);
}

void x4_server_settings_set_awaiting_first_choice(bool awaiting)
{
    atomic_store(&awaiting_choice_flag, awaiting);
    if (awaiting) atomic_store(&first_choice_completed_flag, false);
}

void x4_server_settings_notify_choice_done(void)
{
    atomic_store(&first_choice_completed_flag, true);
    atomic_store(&awaiting_choice_flag, false);
}

bool x4_server_settings_first_choice_completed(void)
{
    return atomic_load(&first_choice_completed_flag);
}

void x4_server_settings_mark_ui_drawn(void)
{
    atomic_store(&first_time_ui_drawn, true);
}

bool x4_server_settings_is_ui_drawn(void)
{
    return atomic_load(&first_time_ui_drawn);
}

void x4_server_settings_reset_ui_drawn(void)
{
    atomic_store(&first_time_ui_drawn, false);
}

void x4_server_settings_write_json(FILE *f)
{
    if (!f) return;
    lock_server();
    const char *def_name = "";
    for (unsigned i = 0; i < current_region_count; ++i) {
        if (current_regions[i].is_default) { def_name = current_regions[i].name; break; }
    }
    bool is_auto = (!stored_selected_name[0]);
    const enum X4Lang export_lang = x4_lang_system_ui() == X4_LANG_ES ? X4_LANG_ES : X4_LANG_EN;
    fprintf(f, "\"server\":{\"selected\":\"%s\",\"is_automatic\":%s,\"default\":\"%s\",\"has_saved_choice\":%s,\"regions\":[",
        stored_selected_name,
        is_auto ? "true" : "false",
        def_name,
        stored_has_saved_choice ? "true" : "false");

    for (unsigned i = 0; i < current_region_count; ++i) {
        if (i) fputc(',', f);
        fprintf(f, "{\"name\":\"%s\",\"api_host\":\"%s\",\"host\":\"%s\",\"is_default\":%s,\"last_game_ms\":%d,\"display_name\":\"%s\"}",
            current_regions[i].name,
            current_regions[i].api_host,
            current_regions[i].host,
            current_regions[i].is_default ? "true" : "false",
            current_regions[i].last_game_ms,
            region_display_name(current_regions[i].name, export_lang));
    }
    fprintf(f, "]},");
    unlock_server();
}
