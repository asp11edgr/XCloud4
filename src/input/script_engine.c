/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "script_engine.h"

#if !X4_PUBLIC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#else
#include <time.h>
static inline uint64_t sceKernelGetProcessTime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

#include "../ui/font.h"
#include "../ui/lang.h"

#define TIMING_HIST_BINS 2049u
#define SCRIPT_HOOK_STEP 1000
#define SCRIPT_CALL_USEC 2000u
#ifndef X4_SCRIPT_DIR_APP
#define X4_SCRIPT_DIR_APP "/app0/assets/scripts"
#endif
#ifndef X4_SCRIPT_DIR_REL
#define X4_SCRIPT_DIR_REL "assets/scripts"
#endif
#ifndef X4_SCRIPT_DIR_DATA
#define X4_SCRIPT_DIR_DATA "/data/xcloud4/scripts"
#endif
#ifndef X4_SCRIPT_DIR_USB
#define X4_SCRIPT_DIR_USB "/mnt/usb0/xcloud4/scripts"
#endif
#define PERSISTENCE_MAGIC 0x53435250u /* 'SCRP' */
#define PERSISTENCE_VERSION 2u

typedef struct {
    size_t current_bytes;
    size_t max_bytes;
    size_t peak_bytes;
} X4ScriptAlloc;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t warning_accepted;
    uint32_t origin;
    char selected_script[64];
    uint32_t checksum;
} X4ScriptPersistenceRecord;

typedef struct {
    lua_State *L;
    X4ScriptAlloc alloc;
    char active_script_path[256];
    char active_script_name[64];
    bool is_active;
    bool needs_gyro;
    bool disabled_by_error;
    char disabled_reason[128];
    int disabled_line;

    int on_input_ref;
    int pad_ref;
    int out_ref;

    /* Metrics */
    uint64_t total_calls;
    float last_call_ms;
    float p50_ms;
    float p95_ms;
    float max_ms;
    uint32_t timing_hist[TIMING_HIST_BINS];
    uint64_t window_calls;
    float window_max_ms;
    uint64_t call_started_us;
    uint32_t hook_instructions;
    uint64_t log_started_us;
    uint32_t log_burst;
    uint64_t log_emitted;
    uint64_t log_coalesced;
    uint64_t last_metric_log_us;

    /* Notification toast */
    char notify_text[128];
    uint64_t notify_expiry_us;

    /* Persistence state */
    bool warning_accepted;
    char saved_script_name[256];
} X4ScriptEngineState;

static X4ScriptEngineState s_engine;

#if X4_SCRIPT_ENGINE_TEST
static uint64_t (*s_test_clock)(void);
static uint32_t s_test_limit_us = SCRIPT_CALL_USEC;
void x4_script_engine_test_clock(uint64_t (*clock_us)(void), uint32_t limit_us)
{
    s_test_clock = clock_us;
    s_test_limit_us = limit_us ? limit_us : SCRIPT_CALL_USEC;
}
#endif

static uint64_t script_clock_us(void)
{
#if X4_SCRIPT_ENGINE_TEST
    if (s_test_clock) return s_test_clock();
#endif
    return sceKernelGetProcessTime();
}

static uint32_t script_limit_us(void)
{
#if X4_SCRIPT_ENGINE_TEST
    return s_test_limit_us;
#else
    return SCRIPT_CALL_USEC;
#endif
}

static void check_call_deadline(lua_State *L)
{
    uint64_t now = script_clock_us();
    if (now >= s_engine.call_started_us && now - s_engine.call_started_us >= script_limit_us())
        luaL_error(L, "call time budget exceeded");
}

static int script_panic_log(lua_State *L)
{
    (void)L;
    fprintf(stderr, "XCloud4 script: unexpected unprotected Lua panic\n");
    return 0; /* Diagnostic only: all script work must already be protected. */
}

static void raw_setfield(lua_State *L, int index, const char *key)
{
    index = lua_absindex(L, index);
    lua_pushstring(L, key);
    lua_insert(L, -2);
    lua_rawset(L, index);
}

static void raw_getfield(lua_State *L, int index, const char *key)
{
    index = lua_absindex(L, index);
    lua_pushstring(L, key);
    lua_rawget(L, index);
}

/* Capacity is authoritative: Lua may pass a smaller logical osize after shrink. */
typedef union {
    size_t capacity;
    max_align_t alignment;
} X4ScriptLuaBlock;

static void *x4_script_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    X4ScriptAlloc *a = (X4ScriptAlloc *)ud;
    X4ScriptLuaBlock *old = ptr ? (X4ScriptLuaBlock *)ptr - 1 : NULL;
    size_t old_bytes = old ? sizeof(*old) + old->capacity : 0;
    if (!nsize) {
        if (old) { free(old); a->current_bytes -= old_bytes; }
        return NULL;
    }
    /* Lua requires shrinking to succeed; keep capacity fully charged, not leaked. */
    if (old && nsize <= old->capacity) return ptr;
    if (old && osize > old->capacity) return NULL;
    if (a->max_bytes < sizeof(X4ScriptLuaBlock) ||
        nsize > a->max_bytes - sizeof(X4ScriptLuaBlock)) return NULL;
    size_t new_bytes = sizeof(X4ScriptLuaBlock) + nsize;
    /* Include both old and new blocks during copy in the physical 2 MiB cap. */
    if (a->current_bytes > a->max_bytes - new_bytes) return NULL;
    X4ScriptLuaBlock *block = malloc(new_bytes);
    if (!block) return NULL; /* Old pointer and its charge remain valid. */
    block->capacity = nsize;
    size_t transient_bytes = a->current_bytes + new_bytes;
    if (transient_bytes > a->peak_bytes) a->peak_bytes = transient_bytes;
    if (old) {
        memcpy(block + 1, ptr, osize < nsize ? osize : nsize);
        free(old);
    }
    a->current_bytes = transient_bytes - old_bytes;
    return block + 1;
}

#if X4_SCRIPT_ENGINE_TEST
bool x4_script_engine_test_allocator_contract(void)
{
    X4ScriptAlloc alloc = {0, 512, 0};
    void *first = x4_script_lua_alloc(&alloc, NULL, 0, 96);
    if (!first) return false;
    memset(first, 0x5a, 96);
    size_t filler_bytes = alloc.max_bytes - alloc.current_bytes - sizeof(X4ScriptLuaBlock);
    void *filler = x4_script_lua_alloc(&alloc, NULL, 0, filler_bytes);
    if (!filler) { x4_script_lua_alloc(&alloc, first, 96, 0); return false; }
    bool ok = alloc.current_bytes == alloc.max_bytes &&
        (uintptr_t)first % _Alignof(max_align_t) == 0;
    void *shrunk = x4_script_lua_alloc(&alloc, first, 96, 8);
    ok = ok && shrunk == first && alloc.current_bytes == alloc.max_bytes;
    void *denied = x4_script_lua_alloc(&alloc, first, 8, 192);
    ok = ok && denied == NULL && alloc.current_bytes == alloc.max_bytes;
    for (size_t i = 0; i < 8; ++i) ok = ok && ((unsigned char *)first)[i] == 0x5a;
    x4_script_lua_alloc(&alloc, filler, filler_bytes, 0);
    void *grown = x4_script_lua_alloc(&alloc, first, 8, 128);
    if (grown) {
        for (size_t i = 0; i < 8; ++i) ok = ok && ((unsigned char *)grown)[i] == 0x5a;
        x4_script_lua_alloc(&alloc, grown, 128, 0);
    } else { ok = false; x4_script_lua_alloc(&alloc, first, 8, 0); }
    ok = ok && alloc.current_bytes == 0 && alloc.peak_bytes <= alloc.max_bytes;
    ok = ok && x4_script_lua_alloc(&alloc, NULL, 0, SIZE_MAX) == NULL;
    return ok;
}
#endif

static void x4_script_instruction_hook(lua_State *L, lua_Debug *ar)
{
    /* Calls (including C library calls) check wall time but do not consume
     * count-hook instructions. The instruction budget remains unchanged. */
    if (ar->event == LUA_HOOKCOUNT)
        s_engine.hook_instructions += SCRIPT_HOOK_STEP;
    check_call_deadline(L);
    if (s_engine.hook_instructions >= X4_SCRIPT_INSTRUCTION_BUDGET)
        luaL_error(L, "instruction budget exceeded (limit %d)", X4_SCRIPT_INSTRUCTION_BUDGET);
}

static int x4_lua_safe_load(lua_State *L)
{
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    const char *chunkname = luaL_optstring(L, 2, "=(load)");
    const char *mode = luaL_optstring(L, 3, "t");
    if (strcmp(mode, "t") != 0) {
        return luaL_error(L, "load: only text mode ('t') is permitted in XCloud4 sandbox");
    }
    if (luaL_loadbufferx(L, s, len, chunkname, "t") != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

static bool allow_script_log(void)
{
    uint64_t now = script_clock_us();
    if (now < s_engine.log_started_us || now - s_engine.log_started_us >= 1000000ULL) {
        s_engine.log_started_us = now;
        s_engine.log_burst = 0;
    }
    if (s_engine.log_burst >= 10) {
        ++s_engine.log_coalesced;
        return false;
    }
    ++s_engine.log_burst;
    ++s_engine.log_emitted;
    return true;
}

static void emit_script_log(const char *text)
{
    char line[256];
    size_t i = 0;
    while (text[i] && i < sizeof(line) - 1) {
        line[i] = (text[i] == '\n' || text[i] == '\r') ? ' ' : text[i];
        ++i;
    }
    line[i] = '\0';
    printf("XCloud4 script log: %s\n", line);
}

static int x4_lua_xcloud_log(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    if (allow_script_log()) emit_script_log(str);
    return 0;
}

static int x4_lua_print(lua_State *L)
{
    if (!allow_script_log()) return 0;
    char line[256];
    size_t used = 0;
    int count = lua_gettop(L);
    for (int i = 1; i <= count && used < sizeof(line) - 1; ++i) {
        size_t length;
        const char *text = luaL_tolstring(L, i, &length);
        if (i != 1 && used < sizeof(line) - 1) line[used++] = ' ';
        for (size_t j = 0; j < length && used < sizeof(line) - 1; ++j)
            line[used++] = (text[j] == '\n' || text[j] == '\r') ? ' ' : text[j];
        lua_pop(L, 1);
    }
    line[used] = '\0';
    emit_script_log(line);
    return 0;
}

static int x4_lua_plain_find(lua_State *L)
{
    lua_settop(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_pushboolean(L, 1);
    lua_call(L, 4, LUA_MULTRET);
    return lua_gettop(L);
}

/* The stock numeric sorter stays in C, outside VM count hooks. Preserve its
 * algorithm and comparison semantics, with a deadline at each comparison. */
static int x4_lua_sort_compare(lua_State *L)
{
    check_call_deadline(L);
    int less;
    if (lua_isnil(L, lua_upvalueindex(1)))
        less = lua_compare(L, 1, 2, LUA_OPLT);
    else {
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_call(L, 2, 1);
        less = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    check_call_deadline(L);
    lua_pushboolean(L, less);
    return 1;
}

static int x4_lua_bounded_sort(lua_State *L)
{
    check_call_deadline(L);
    lua_settop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1)); /* Original Lua table.sort. */
    lua_insert(L, 1);
    /* Invalid comparator types retain the original validation, which only
     * checks the comparator for arrays with more than one element. */
    if (lua_isnil(L, 3) || lua_isfunction(L, 3)) {
        lua_pushvalue(L, 3);
        lua_pushcclosure(L, x4_lua_sort_compare, 1);
        lua_replace(L, 3);
    }
    lua_call(L, 2, 0);
    check_call_deadline(L);
    return 0;
}

static int x4_lua_xcloud_notify(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    x4_script_engine_set_toast(str);
    return 0;
}

void x4_script_engine_set_toast(const char *text)
{
    if (!text) return;
    snprintf(s_engine.notify_text, sizeof(s_engine.notify_text), "%.127s", text);
    s_engine.notify_expiry_us = script_clock_us() + 3000000ULL; /* 3 seconds */
}

static void open_safe_libraries(lua_State *L)
{
    /* Open only base, math, string, table, utf8 */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1);
    lua_pop(L, 1);

    /* Sanitize base library: remove dofile, loadfile, and replace load with safe text-only load */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");

    lua_pushcfunction(L, x4_lua_safe_load);
    lua_setglobal(L, "load");

    lua_pushcfunction(L, x4_lua_print);
    lua_setglobal(L, "print");
    lua_getglobal(L, "string");
    raw_getfield(L, -1, "find");
    lua_pushcclosure(L, x4_lua_plain_find, 1);
    raw_setfield(L, -2, "find");
    lua_pushnil(L); raw_setfield(L, -2, "match");
    lua_pushnil(L); raw_setfield(L, -2, "gmatch");
    lua_pushnil(L); raw_setfield(L, -2, "gsub");
    lua_pop(L, 1);

    lua_getglobal(L, "table");
    raw_getfield(L, -1, "sort");
    lua_pushcclosure(L, x4_lua_bounded_sort, 1);
    raw_setfield(L, -2, "sort");
    lua_pop(L, 1);

    /* Register xcloud helper table */
    lua_newtable(L);
    lua_pushcfunction(L, x4_lua_xcloud_log);
    raw_setfield(L, -2, "log");
    lua_pushcfunction(L, x4_lua_xcloud_notify);
    raw_setfield(L, -2, "notify");
    lua_setglobal(L, "xcloud");
}

static void create_preallocated_tables(lua_State *L)
{
    /* Pre-create pad table */
    lua_createtable(L, 0, 32);

    /* Pre-create buttons */
    static const char * const buttons[] = {
        "cross", "circle", "square", "triangle",
        "l1", "r1", "l3", "r3", "options", "touchpad",
        "up", "down", "left", "right"
    };
    for (size_t i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i) {
        lua_pushboolean(L, 0);
        raw_setfield(L, -2, buttons[i]);
    }

    /* Pre-create sticks & triggers */
    static const char * const axes[] = { "lx", "ly", "rx", "ry", "l2", "r2", "time" };
    for (size_t i = 0; i < sizeof(axes)/sizeof(axes[0]); ++i) {
        lua_pushnumber(L, 0.0);
        raw_setfield(L, -2, axes[i]);
    }

    /* Pre-create gyro subtable */
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, 0.0); raw_setfield(L, -2, "x");
    lua_pushnumber(L, 0.0); raw_setfield(L, -2, "y");
    lua_pushnumber(L, 0.0); raw_setfield(L, -2, "z");
    raw_setfield(L, -2, "gyro");

    /* Pre-create accel subtable */
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, 0.0); raw_setfield(L, -2, "x");
    lua_pushnumber(L, 0.0); raw_setfield(L, -2, "y");
    lua_pushnumber(L, 0.0); raw_setfield(L, -2, "z");
    raw_setfield(L, -2, "accel");

    /* Pre-create touch subtable with fingers 1 and 2 */
    lua_createtable(L, 2, 0);
    for (int f = 1; f <= 2; ++f) {
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "x");
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "y");
        lua_pushboolean(L, 0); raw_setfield(L, -2, "active");
        lua_rawseti(L, -2, f);
    }
    raw_setfield(L, -2, "touch");

    s_engine.pad_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Pre-create out table */
    lua_createtable(L, 0, 24);
    static const char * const out_buttons[] = {
        "a", "b", "x", "y", "lb", "rb", "ls", "rs",
        "view", "menu", "up", "down", "left", "right"
    };
    for (size_t i = 0; i < sizeof(out_buttons)/sizeof(out_buttons[0]); ++i) {
        lua_pushboolean(L, 0);
        raw_setfield(L, -2, out_buttons[i]);
    }
    static const char * const out_axes[] = { "lx", "ly", "rx", "ry", "lt", "rt" };
    for (size_t i = 0; i < sizeof(out_axes)/sizeof(out_axes[0]); ++i) {
        lua_pushnumber(L, 0.0);
        raw_setfield(L, -2, out_axes[i]);
    }

    s_engine.out_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

void x4_script_engine_init(void)
{
    memset(&s_engine, 0, sizeof(s_engine));
    s_engine.alloc.max_bytes = X4_SCRIPT_MAX_MEM_BYTES;
    s_engine.on_input_ref = LUA_NOREF;
    s_engine.pad_ref = LUA_NOREF;
    s_engine.out_ref = LUA_NOREF;

    /* Load persistence if available */
    bool accepted = false;
    char saved[256] = {0};
    if (x4_script_engine_load_persistence(&accepted, saved, sizeof(saved))) {
        s_engine.warning_accepted = accepted;
        snprintf(s_engine.saved_script_name, sizeof(s_engine.saved_script_name), "%s", saved);
    }
}

void x4_script_engine_shutdown(void)
{
    x4_script_engine_unload();
}

void x4_script_engine_unload(void)
{
    if (s_engine.L) {
        s_engine.on_input_ref = s_engine.pad_ref = s_engine.out_ref = LUA_NOREF;
        lua_close(s_engine.L);
        s_engine.L = NULL;
    }
    s_engine.is_active = false;
    s_engine.needs_gyro = false;
    s_engine.active_script_path[0] = '\0';
    s_engine.active_script_name[0] = '\0';
    s_engine.alloc.current_bytes = 0;
    s_engine.alloc.peak_bytes = 0;
}

bool x4_script_engine_is_active(void)
{
    return s_engine.is_active && !s_engine.disabled_by_error && s_engine.L != NULL;
}

const char *x4_script_engine_get_name(void)
{
    if (!s_engine.is_active || s_engine.disabled_by_error) return NULL;
    return s_engine.active_script_name;
}

bool x4_script_engine_needs_gyro(void)
{
    return s_engine.is_active && !s_engine.disabled_by_error && s_engine.needs_gyro;
}

static void extract_script_name(const char *path, char *out, size_t out_cap)
{
    if (!path || !out || out_cap == 0) return;
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    snprintf(out, out_cap, "%s", name);
}

typedef struct {
    const char *code;
    size_t length;
} X4ScriptLoadContext;

static int protected_load(lua_State *L)
{
#if X4_SCRIPT_ENGINE_TEST
    uint64_t setup_started_us = script_clock_us();
#endif
    X4ScriptLoadContext *context = lua_touserdata(L, 1);
    open_safe_libraries(L);
    create_preallocated_tables(L);
    int rc = luaL_loadbufferx(L, context->code, context->length,
                             s_engine.active_script_name, "t");
    if (rc != LUA_OK) return lua_error(L);
#if X4_SCRIPT_ENGINE_TEST
    printf("XCloud4 script test: host_setup_us=%llu (not user-code time)\n",
           (unsigned long long)(script_clock_us() - setup_started_us));
#endif
    /* Library setup/parser are protected but not a controller call. Time only user init. */
    s_engine.call_started_us = script_clock_us();
    s_engine.hook_instructions = 0;
    lua_sethook(L, x4_script_instruction_hook, LUA_MASKCOUNT | LUA_MASKCALL, SCRIPT_HOOK_STEP);
    lua_call(L, 0, 0);
    lua_pushglobaltable(L);
    raw_getfield(L, -1, "on_input");
    if (!lua_isfunction(L, -1)) return luaL_error(L, "missing on_input");
    s_engine.on_input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    check_call_deadline(L);
    return 0;
}

static const char *protected_error(lua_State *L)
{
    return lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "non-string Lua error";
}

bool x4_script_engine_load(const char *path)
{
    x4_script_engine_unload();
    s_engine.disabled_by_error = false;
    s_engine.disabled_reason[0] = '\0';
    s_engine.disabled_line = 0;
    memset(s_engine.timing_hist, 0, sizeof(s_engine.timing_hist));
    s_engine.window_calls = 0;
    s_engine.window_max_ms = 0;
    s_engine.last_metric_log_us = script_clock_us();
    s_engine.log_started_us = script_clock_us();
    s_engine.log_burst = 0;
    s_engine.log_emitted = s_engine.log_coalesced = 0;
    s_engine.total_calls = 0;
    s_engine.p50_ms = s_engine.p95_ms = s_engine.max_ms = 0.0f;

    if (!path || path[0] == '\0') {
        return true; /* Clean unloaded state */
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("XCloud4 script: cannot open %s (errno %d)\n", path, errno);
        snprintf(s_engine.disabled_reason, sizeof(s_engine.disabled_reason), "cannot open file");
        s_engine.disabled_by_error = true;
        x4_script_engine_set_toast(x4_tr(X4_TR_LAB_SCRIPT_OPEN_ERROR));
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len < 0 || len > X4_MAX_SCRIPT_SIZE) {
        fclose(f);
        printf("XCloud4 script: file %s exceeds 64 KB limit (%ld bytes)\n", path, len);
        snprintf(s_engine.disabled_reason, sizeof(s_engine.disabled_reason), "exceeds 64 KB");
        s_engine.disabled_by_error = true;
        x4_script_engine_set_toast(x4_tr(X4_TR_LAB_SCRIPT_SIZE_ERROR));
        return false;
    }
    rewind(f);

    char *code = malloc((size_t)len + 1);
    if (!code) {
        fclose(f);
        return false;
    }
    size_t read_bytes = fread(code, 1, (size_t)len, f);
    fclose(f);
    code[read_bytes] = '\0';

    /* Initialize Lua state with custom allocator */
    s_engine.alloc.current_bytes = 0;
    s_engine.alloc.peak_bytes = 0;
    s_engine.alloc.max_bytes = X4_SCRIPT_MAX_MEM_BYTES;

    lua_State *L = lua_newstate(x4_script_lua_alloc, &s_engine.alloc);
    if (!L) {
        free(code);
        printf("XCloud4 script: lua_newstate failed (out of memory)\n");
        return false;
    }
    s_engine.L = L;

    lua_atpanic(L, script_panic_log);
    extract_script_name(path, s_engine.active_script_name, sizeof(s_engine.active_script_name));
    snprintf(s_engine.active_script_path, sizeof(s_engine.active_script_path), "%s", path);
    s_engine.needs_gyro = (strstr(code, "gyro") != NULL || strstr(code, "accel") != NULL || strstr(code, "motion") != NULL);
    X4ScriptLoadContext context = {code, read_bytes};
    lua_pushcfunction(L, protected_load);
    lua_pushlightuserdata(L, &context);
    int run_rc = lua_pcall(L, 1, 0, 0);
    free(code);
    if (run_rc != LUA_OK) {
        const char *err = protected_error(L);
        snprintf(s_engine.disabled_reason, sizeof(s_engine.disabled_reason), "%s", err);
        s_engine.disabled_by_error = true;
        printf("XCloud4 script load error [%s]: %s\n", s_engine.active_script_name, s_engine.disabled_reason);
        lua_pop(L, 1);
        x4_script_engine_set_toast(x4_tr(strstr(err, "missing on_input") ? X4_TR_LAB_SCRIPT_MISSING_INPUT : X4_TR_LAB_SCRIPT_LOAD_ERROR));
        x4_script_engine_unload();
        return false;
    }

    s_engine.is_active = true;
    printf("XCloud4 script: loaded %s successfully (mem=%u B)\n",
           s_engine.active_script_name, (unsigned)s_engine.alloc.current_bytes);
    return true;
}

static inline float clampf(float v, float min_v, float max_v)
{
    if (v != v) return 0.0f;
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

static bool table_has_metatable(lua_State *L, int index)
{
    if (lua_getmetatable(L, index)) { lua_pop(L, 1); return true; }
    return false;
}

static bool ensure_child_table(lua_State *L, int parent, const char *key)
{
    parent = lua_absindex(L, parent);
    raw_getfield(L, parent, key);
    bool valid = lua_istable(L, -1);
    if (!valid) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        raw_setfield(L, parent, key);
    }
    return valid && !table_has_metatable(L, -1);
}

static void validate_tables(lua_State *L)
{
    bool valid = true;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.pad_ref);
    valid = !table_has_metatable(L, -1) && valid;
    valid = ensure_child_table(L, -1, "gyro") && valid; lua_pop(L, 1);
    valid = ensure_child_table(L, -1, "accel") && valid; lua_pop(L, 1);
    valid = ensure_child_table(L, -1, "touch") && valid;
    for (int finger = 1; finger <= 2; ++finger) {
        lua_rawgeti(L, -1, finger);
        if (!lua_istable(L, -1)) {
            valid = false;
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, -3, finger);
        }
        valid = !table_has_metatable(L, -1) && valid;
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.out_ref);
    valid = !table_has_metatable(L, -1) && valid;
    lua_pop(L, 1);
    if (!valid) luaL_error(L, "pad/out structure or metatable changed");
}

static void update_pad_table(lua_State *L, const X4Controller *c, uint64_t session_now_us)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.pad_ref);

    uint32_t b = c ? c->data.buttons : 0;
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_CROSS) != 0); raw_setfield(L, -2, "cross");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_CIRCLE) != 0); raw_setfield(L, -2, "circle");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_SQUARE) != 0); raw_setfield(L, -2, "square");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_TRIANGLE) != 0); raw_setfield(L, -2, "triangle");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_L1) != 0); raw_setfield(L, -2, "l1");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_R1) != 0); raw_setfield(L, -2, "r1");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_L3) != 0); raw_setfield(L, -2, "l3");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_R3) != 0); raw_setfield(L, -2, "r3");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_OPTIONS) != 0); raw_setfield(L, -2, "options");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_TOUCH_PAD) != 0); raw_setfield(L, -2, "touchpad");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_UP) != 0); raw_setfield(L, -2, "up");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_DOWN) != 0); raw_setfield(L, -2, "down");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_LEFT) != 0); raw_setfield(L, -2, "left");
    lua_pushboolean(L, (b & ORBIS_PAD_BUTTON_RIGHT) != 0); raw_setfield(L, -2, "right");

    if (c && c->data.connected) {
        int n_lx = (int)c->data.leftStick.x - 128;
        int n_ly = (int)c->data.leftStick.y - 128;
        int n_rx = (int)c->data.rightStick.x - 128;
        int n_ry = (int)c->data.rightStick.y - 128;

        float lx = (n_lx >= 0 ? (float)n_lx / 127.0f : (float)n_lx / 128.0f);
        float ly = -(n_ly >= 0 ? (float)n_ly / 127.0f : (float)n_ly / 128.0f);
        float rx = (n_rx >= 0 ? (float)n_rx / 127.0f : (float)n_rx / 128.0f);
        float ry = -(n_ry >= 0 ? (float)n_ry / 127.0f : (float)n_ry / 128.0f);

        lua_pushnumber(L, clampf(lx, -1.0f, 1.0f)); raw_setfield(L, -2, "lx");
        lua_pushnumber(L, clampf(ly, -1.0f, 1.0f)); raw_setfield(L, -2, "ly");
        lua_pushnumber(L, clampf(rx, -1.0f, 1.0f)); raw_setfield(L, -2, "rx");
        lua_pushnumber(L, clampf(ry, -1.0f, 1.0f)); raw_setfield(L, -2, "ry");

        lua_pushnumber(L, (float)c->data.analogButtons.l2 / 255.0f); raw_setfield(L, -2, "l2");
        lua_pushnumber(L, (float)c->data.analogButtons.r2 / 255.0f); raw_setfield(L, -2, "r2");

        /* Gyro subtable */
        raw_getfield(L, -1, "gyro");
        lua_pushnumber(L, c->data.vel.x); raw_setfield(L, -2, "x");
        lua_pushnumber(L, c->data.vel.y); raw_setfield(L, -2, "y");
        lua_pushnumber(L, c->data.vel.z); raw_setfield(L, -2, "z");
        lua_pop(L, 1);

        /* Accel subtable */
        raw_getfield(L, -1, "accel");
        lua_pushnumber(L, c->data.acell.x); raw_setfield(L, -2, "x");
        lua_pushnumber(L, c->data.acell.y); raw_setfield(L, -2, "y");
        lua_pushnumber(L, c->data.acell.z); raw_setfield(L, -2, "z");
        lua_pop(L, 1);

        /* Touch subtable */
        raw_getfield(L, -1, "touch");
        for (int f = 0; f < 2; ++f) {
            lua_rawgeti(L, -1, f + 1);
            lua_pushboolean(L, c->data.touch.fingers > f); raw_setfield(L, -2, "active");
            lua_pushnumber(L, (float)c->data.touch.touch[f].x / 1920.0f); raw_setfield(L, -2, "x");
            lua_pushnumber(L, (float)c->data.touch.touch[f].y / 940.0f); raw_setfield(L, -2, "y");
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else {
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "lx");
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "ly");
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "rx");
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "ry");
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "l2");
        lua_pushnumber(L, 0.0); raw_setfield(L, -2, "r2");
    }

    lua_pushnumber(L, (double)session_now_us / 1000000.0);
    raw_setfield(L, -2, "time");

    lua_pop(L, 1);
}

static void update_out_table(lua_State *L, const X4GamepadFrame *frame)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.out_ref);

    uint16_t b = frame->buttons;
    lua_pushboolean(L, (b & X4_GAMEPAD_A) != 0); raw_setfield(L, -2, "a");
    lua_pushboolean(L, (b & X4_GAMEPAD_B) != 0); raw_setfield(L, -2, "b");
    lua_pushboolean(L, (b & X4_GAMEPAD_X) != 0); raw_setfield(L, -2, "x");
    lua_pushboolean(L, (b & X4_GAMEPAD_Y) != 0); raw_setfield(L, -2, "y");
    lua_pushboolean(L, (b & X4_GAMEPAD_LB) != 0); raw_setfield(L, -2, "lb");
    lua_pushboolean(L, (b & X4_GAMEPAD_RB) != 0); raw_setfield(L, -2, "rb");
    lua_pushboolean(L, (b & X4_GAMEPAD_L3) != 0); raw_setfield(L, -2, "ls");
    lua_pushboolean(L, (b & X4_GAMEPAD_R3) != 0); raw_setfield(L, -2, "rs");
    lua_pushboolean(L, (b & X4_GAMEPAD_VIEW) != 0); raw_setfield(L, -2, "view");
    lua_pushboolean(L, (b & X4_GAMEPAD_MENU) != 0); raw_setfield(L, -2, "menu");
    lua_pushboolean(L, (b & X4_GAMEPAD_UP) != 0); raw_setfield(L, -2, "up");
    lua_pushboolean(L, (b & X4_GAMEPAD_DOWN) != 0); raw_setfield(L, -2, "down");
    lua_pushboolean(L, (b & X4_GAMEPAD_LEFT) != 0); raw_setfield(L, -2, "left");
    lua_pushboolean(L, (b & X4_GAMEPAD_RIGHT) != 0); raw_setfield(L, -2, "right");

    lua_pushnumber(L, (float)frame->left_x / 32767.0f); raw_setfield(L, -2, "lx");
    lua_pushnumber(L, (float)frame->left_y / 32767.0f); raw_setfield(L, -2, "ly");
    lua_pushnumber(L, (float)frame->right_x / 32767.0f); raw_setfield(L, -2, "rx");
    lua_pushnumber(L, (float)frame->right_y / 32767.0f); raw_setfield(L, -2, "ry");

    lua_pushnumber(L, (float)frame->left_trigger / 65535.0f); raw_setfield(L, -2, "lt");
    lua_pushnumber(L, (float)frame->right_trigger / 65535.0f); raw_setfield(L, -2, "rt");

    lua_pop(L, 1);
}

static void read_out_table(lua_State *L, X4GamepadFrame *frame)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.out_ref);

    uint16_t buttons = 0;
    /* NEXUS is preserved from host */
    if (frame->buttons & X4_GAMEPAD_NEXUS) buttons |= X4_GAMEPAD_NEXUS;

    static const struct { const char *key; uint16_t mask; } btn_map[] = {
        { "a", X4_GAMEPAD_A }, { "b", X4_GAMEPAD_B },
        { "x", X4_GAMEPAD_X }, { "y", X4_GAMEPAD_Y },
        { "lb", X4_GAMEPAD_LB }, { "rb", X4_GAMEPAD_RB },
        { "ls", X4_GAMEPAD_L3 }, { "rs", X4_GAMEPAD_R3 },
        { "view", X4_GAMEPAD_VIEW }, { "menu", X4_GAMEPAD_MENU },
        { "up", X4_GAMEPAD_UP }, { "down", X4_GAMEPAD_DOWN },
        { "left", X4_GAMEPAD_LEFT }, { "right", X4_GAMEPAD_RIGHT }
    };
    for (size_t i = 0; i < sizeof(btn_map)/sizeof(btn_map[0]); ++i) {
        raw_getfield(L, -1, btn_map[i].key);
        if (lua_toboolean(L, -1)) buttons |= btn_map[i].mask;
        lua_pop(L, 1);
    }
    frame->buttons = buttons;

    raw_getfield(L, -1, "lx");
    float lx = clampf((float)lua_tonumber(L, -1), -1.0f, 1.0f);
    frame->left_x = (int16_t)(lx * 32767.0f);
    lua_pop(L, 1);

    raw_getfield(L, -1, "ly");
    float ly = clampf((float)lua_tonumber(L, -1), -1.0f, 1.0f);
    frame->left_y = (int16_t)(ly * 32767.0f);
    lua_pop(L, 1);

    raw_getfield(L, -1, "rx");
    float rx = clampf((float)lua_tonumber(L, -1), -1.0f, 1.0f);
    frame->right_x = (int16_t)(rx * 32767.0f);
    lua_pop(L, 1);

    raw_getfield(L, -1, "ry");
    float ry = clampf((float)lua_tonumber(L, -1), -1.0f, 1.0f);
    frame->right_y = (int16_t)(ry * 32767.0f);
    lua_pop(L, 1);

    raw_getfield(L, -1, "lt");
    float lt = clampf((float)lua_tonumber(L, -1), 0.0f, 1.0f);
    frame->left_trigger = (uint16_t)(lt * 65535.0f);
    lua_pop(L, 1);

    raw_getfield(L, -1, "rt");
    float rt = clampf((float)lua_tonumber(L, -1), 0.0f, 1.0f);
    frame->right_trigger = (uint16_t)(rt * 65535.0f);
    lua_pop(L, 1);

    lua_pop(L, 1);
}

static void histogram_values(float *p50, float *p95, float *maximum)
{
    if (!s_engine.window_calls) {
        *p50 = s_engine.p50_ms; *p95 = s_engine.p95_ms; *maximum = s_engine.max_ms;
        return;
    }
    uint64_t count = 0;
    uint64_t n50 = (s_engine.window_calls + 1) / 2;
    uint64_t n95 = (s_engine.window_calls * 95 + 99) / 100;
    bool found50 = false;
    for (unsigned i = 0; i < TIMING_HIST_BINS; ++i) {
        count += s_engine.timing_hist[i];
        /* Overflow is represented conservatively by the exact window maximum. */
        float estimate = i == TIMING_HIST_BINS - 1 ? s_engine.window_max_ms : (float)i / 1000.0f;
        if (!found50 && count >= n50) { *p50 = estimate; found50 = true; }
        if (count >= n95) { *p95 = estimate; break; }
    }
    *maximum = s_engine.window_max_ms;
}

static void record_call_time(uint64_t elapsed_us)
{
    ++s_engine.total_calls;
    ++s_engine.window_calls;
    unsigned bin = elapsed_us < TIMING_HIST_BINS ? (unsigned)elapsed_us : TIMING_HIST_BINS - 1;
    ++s_engine.timing_hist[bin];
    s_engine.last_call_ms = (float)elapsed_us / 1000.0f;
    if (s_engine.last_call_ms > s_engine.window_max_ms)
        s_engine.window_max_ms = s_engine.last_call_ms;
}

typedef struct {
    const X4Controller *controller;
    X4GamepadFrame frame;
    float dt;
    uint64_t session_now_us;
} X4ScriptInputContext;

static int protected_input(lua_State *L)
{
    X4ScriptInputContext *context = lua_touserdata(L, 1);
    validate_tables(L);
    update_pad_table(L, context->controller, context->session_now_us);
    update_out_table(L, &context->frame);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.on_input_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.pad_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_engine.out_ref);
    lua_pushnumber(L, context->dt);
    lua_call(L, 3, 0);
    validate_tables(L);
    read_out_table(L, &context->frame);
    lua_gc(L, LUA_GCSTEP, 50);
    check_call_deadline(L);
    return 0;
}

void x4_script_engine_process_input(
    const X4Controller *controller, X4GamepadFrame *gamepad,
    float dt, uint64_t session_now_us)
{
    if (!gamepad || !x4_script_engine_is_active()) return;
    lua_State *L = s_engine.L;
    uint64_t t_start = script_clock_us();
    X4ScriptInputContext context = {controller, *gamepad, clampf(dt, 0.0f, 0.1f), session_now_us};
    s_engine.call_started_us = t_start;
    s_engine.hook_instructions = 0;
    lua_sethook(L, x4_script_instruction_hook, LUA_MASKCOUNT | LUA_MASKCALL, SCRIPT_HOOK_STEP);
    lua_pushcfunction(L, protected_input);
    lua_pushlightuserdata(L, &context);
    int rc = lua_pcall(L, 1, 0, 0);
    uint64_t t_end = script_clock_us();
    record_call_time(t_end >= t_start ? t_end - t_start : 0);
    if (rc != LUA_OK) {
        const char *err = protected_error(L);
        const char *colon = strchr(err, ':');
        s_engine.disabled_by_error = true;
        s_engine.disabled_line = colon ? atoi(colon + 1) : 0;
        snprintf(s_engine.disabled_reason, sizeof(s_engine.disabled_reason), "%s", err);
        printf("XCloud4 script runtime error [%s:%d]: %s (deactivated for session)\n",
               s_engine.active_script_name, s_engine.disabled_line, s_engine.disabled_reason);
        char toast[192];
        snprintf(toast, sizeof(toast), "%s %.128s", x4_tr(X4_TR_LAB_SCRIPT_DISABLED), s_engine.disabled_reason);
        x4_script_engine_set_toast(toast);
        lua_pop(L, 1);
        return; /* Candidate is committed only after protected fill/call/read/GC succeeds. */
    }
    *gamepad = context.frame;
}

void x4_script_engine_get_metrics(X4ScriptMetrics *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = x4_script_engine_is_active();
    out->disabled_by_error = s_engine.disabled_by_error;
    snprintf(out->disabled_reason, sizeof(out->disabled_reason), "%s", s_engine.disabled_reason);
    out->disabled_line = s_engine.disabled_line;
    out->total_calls = s_engine.total_calls;
    out->current_mem_bytes = (uint32_t)s_engine.alloc.current_bytes;
    out->peak_mem_bytes = (uint32_t)s_engine.alloc.peak_bytes;
    out->last_call_ms = s_engine.last_call_ms;
    histogram_values(&out->p50_ms, &out->p95_ms, &out->max_ms);
    out->window_calls = s_engine.window_calls;
    out->log_messages = s_engine.log_emitted;
    out->log_coalesced = s_engine.log_coalesced;
    snprintf(out->active_script_name, sizeof(out->active_script_name), "%s", s_engine.active_script_name);
}

void x4_script_engine_log_metrics_5s(uint64_t now_us)
{
    if (now_us < s_engine.last_metric_log_us || now_us - s_engine.last_metric_log_us < 5000000ULL) return;
    s_engine.last_metric_log_us = now_us;
    if (!s_engine.is_active && !s_engine.disabled_by_error) return;
    histogram_values(&s_engine.p50_ms, &s_engine.p95_ms, &s_engine.max_ms);
    printf("XCloud4 script metrics: script=%s window=5s calls=%llu p50=%.3fms p95=%.3fms max=%.3fms mem=%uKB status=%s\n",
           s_engine.active_script_name[0] ? s_engine.active_script_name : "none",
           (unsigned long long)s_engine.window_calls, s_engine.p50_ms, s_engine.p95_ms, s_engine.max_ms,
           (unsigned)(s_engine.alloc.current_bytes / 1024),
           s_engine.disabled_by_error ? "disabled" : (s_engine.is_active ? "active" : "inactive"));
    memset(s_engine.timing_hist, 0, sizeof(s_engine.timing_hist));
    s_engine.window_calls = 0;
    s_engine.window_max_ms = 0.0f;
}

static void json_string(FILE *f, const char *text)
{
    const unsigned char *end = (const unsigned char *)text + strlen(text);
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)text; p < end; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
        else if (*p < 0x80) fputc(*p, f);
        else {
            size_t width = *p >= 0xc2 && *p <= 0xdf ? 2u :
                (*p >= 0xe0 && *p <= 0xef ? 3u : (*p >= 0xf0 && *p <= 0xf4 ? 4u : 0u));
            bool valid = width && (size_t)(end - p) >= width;
            for (size_t i = 1; valid && i < width; ++i)
                valid = (p[i] & 0xc0) == 0x80;
            if (valid && ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] >= 0xa0) ||
                          (*p == 0xf0 && p[1] < 0x90) || (*p == 0xf4 && p[1] >= 0x90))) valid = false;
            if (valid) { fwrite(p, 1, width, f); p += width - 1; }
            else fprintf(f, "\\u%04x", (unsigned)*p);
        }
    }
    fputc('"', f);
}

bool x4_script_engine_export_session_json(const char *path)
{
    if (!path || path[0] == '\0') return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    X4ScriptMetrics metrics;
    x4_script_engine_get_metrics(&metrics);
    fputs("{\n  \"schema\": \"X4_SCRIPT_SESSION_V1\",\n  \"active_script\": ", f);
    json_string(f, metrics.active_script_name);
    fputs(",\n  \"status\": ", f);
    json_string(f, metrics.disabled_by_error ? "disabled" : (metrics.active ? "active" : "none"));
    fputs(",\n  \"disabled_reason\": ", f);
    json_string(f, metrics.disabled_reason);
    fprintf(f, ",\n  \"disabled_line\": %d,\n  \"total_calls\": %llu,\n"
               "  \"call_time_ms\": {\"p50\": %.4f, \"p95\": %.4f, \"max\": %.4f},\n"
               "  \"window_calls\": %llu,\n  \"memory_bytes\": %u,\n  \"peak_memory_bytes\": %u,\n"
               "  \"log_messages\": %llu,\n  \"log_coalesced\": %llu\n}\n",
            metrics.disabled_line, (unsigned long long)metrics.total_calls,
            metrics.p50_ms, metrics.p95_ms, metrics.max_ms,
            (unsigned long long)metrics.window_calls, metrics.current_mem_bytes, metrics.peak_mem_bytes,
            (unsigned long long)metrics.log_messages, (unsigned long long)metrics.log_coalesced);
    bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

int x4_script_engine_scan(X4ScriptEntry *out_entries, int max_entries)
{
    if (!out_entries || max_entries <= 0) return 0;
    int count = 0;

    static const char * const search_paths[] = {
        X4_SCRIPT_DIR_APP, X4_SCRIPT_DIR_REL, X4_SCRIPT_DIR_DATA, X4_SCRIPT_DIR_USB
    };

    for (size_t p = 0; p < sizeof(search_paths)/sizeof(search_paths[0]); ++p) {
        const char *dir_path = search_paths[p];
        DIR *dir = opendir(dir_path);
        if (!dir) {
            if (strstr(dir_path, "usb0")) {
                printf("XCloud4 scripts: USB path %s not available\n", dir_path);
            }
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            size_t nlen = strlen(ent->d_name);
            if (nlen >= sizeof(out_entries[0].name)) continue;
            if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".lua") != 0) continue;

            /* Check for duplicate name already registered */
            bool duplicate = false;
            for (int i = 0; i < count; ++i) {
                if (strcmp(out_entries[i].name, ent->d_name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            char full_path[512];
            int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
            if (path_len < 0 || (size_t)path_len >= sizeof(out_entries[0].path)) continue;

            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= X4_MAX_SCRIPT_SIZE) {
                snprintf(out_entries[count].path, sizeof(out_entries[count].path), "%.255s", full_path);
                snprintf(out_entries[count].name, sizeof(out_entries[count].name), "%.63s", ent->d_name);
                out_entries[count].size = (uint32_t)st.st_size;
                count++;
                if (count >= max_entries || count >= X4_MAX_SCRIPTS) break;
            }
        }
        closedir(dir);
        if (count >= max_entries || count >= X4_MAX_SCRIPTS) break;
    }

    return count;
}

bool x4_script_engine_get_notification(char *out_buf, size_t buf_size)
{
    if (!out_buf || buf_size == 0) return false;
    uint64_t now_us = script_clock_us();
    if (now_us < s_engine.notify_expiry_us && s_engine.notify_text[0] != '\0') {
        snprintf(out_buf, buf_size, "%s", s_engine.notify_text);
        return true;
    }
    return false;
}

void x4_script_engine_draw_notification(uint32_t *pixels)
{
    if (!pixels) return;
    char text[128];
    if (!x4_script_engine_get_notification(text, sizeof(text))) return;

    /* Draw toast at top of screen: centered pill */
    int text_w = x4_text_width_px(22, 1, text);
    int pill_w = text_w + 48;
    if (pill_w > 1200) pill_w = 1200;
    int pill_x = (1920 - pill_w) / 2;
    int pill_y = 36;
    int pill_h = 44;

    /* Dark translucent pill */
    uint32_t bg_color = 0xee161a24;
    uint32_t border_color = 0xff64e78c; /* Green accent */
    if (s_engine.disabled_by_error) {
        border_color = 0xffe76464; /* Red accent on error */
    }

    for (int y = pill_y; y < pill_y + pill_h; ++y) {
        for (int x = pill_x; x < pill_x + pill_w; ++x) {
            if (y == pill_y || y == pill_y + pill_h - 1 || x == pill_x || x == pill_x + pill_w - 1) {
                pixels[y * 1920 + x] = border_color;
            } else {
                pixels[y * 1920 + x] = bg_color;
            }
        }
    }

    x4_text_ellipsis_px(pixels, pill_x + 24, pill_y + 11, 22, 1, text, pill_w - 48, 0xffffffff);
}

static const char * const script_origins[] = {
    "", X4_SCRIPT_DIR_APP, X4_SCRIPT_DIR_REL, X4_SCRIPT_DIR_DATA, X4_SCRIPT_DIR_USB
};

static uint32_t calc_checksum(const X4ScriptPersistenceRecord *r)
{
    uint32_t sum = r->magic ^ r->version ^ r->warning_accepted ^ r->origin;
    for (size_t i = 0; i < sizeof(r->selected_script); ++i)
        sum = sum * 31u + (uint8_t)r->selected_script[i];
    return sum;
}

static const char *get_persistence_path(void)
{
#ifdef X4_SCRIPT_PERSIST_PATH
    return X4_SCRIPT_PERSIST_PATH;
#elif defined(X4_OPENORBIS)
    return "/data/xcloud4/lab-scripts-v2.bin";
#else
    struct stat st;
    if (stat("/data/xcloud4", &st) == 0 && S_ISDIR(st.st_mode))
        return "/data/xcloud4/lab-scripts-v2.bin";
    return "lab-scripts-v2.bin";
#endif
}

bool x4_script_engine_load_persistence(bool *warning_accepted, char *saved_script, size_t max_len)
{
    if (warning_accepted) *warning_accepted = false;
    if (saved_script && max_len) saved_script[0] = '\0';
    FILE *f = fopen(get_persistence_path(), "rb");
    if (!f) return false;
    X4ScriptPersistenceRecord rec;
    size_t rd = fread(&rec, 1, sizeof(rec), f);
    bool exact = fgetc(f) == EOF;
    fclose(f);
    if (rd != sizeof(rec) || !exact || rec.magic != PERSISTENCE_MAGIC ||
        rec.version != PERSISTENCE_VERSION || rec.origin >= sizeof(script_origins) / sizeof(script_origins[0]) ||
        memchr(rec.selected_script, '\0', sizeof(rec.selected_script)) == NULL ||
        strchr(rec.selected_script, '/') || strchr(rec.selected_script, '\\') ||
        rec.checksum != calc_checksum(&rec)) return false;
    char full[256];
    int length = rec.origin && rec.selected_script[0]
        ? snprintf(full, sizeof(full), "%s/%s", script_origins[rec.origin], rec.selected_script)
        : snprintf(full, sizeof(full), "%s", rec.selected_script);
    if (length < 0 || (size_t)length >= sizeof(full) ||
        (saved_script && ((size_t)length >= max_len))) return false;
    if (warning_accepted) *warning_accepted = rec.warning_accepted != 0;
    if (saved_script && max_len) memcpy(saved_script, full, (size_t)length + 1);
    return true;
}

bool x4_script_engine_save_persistence(bool warning_accepted, const char *saved_script)
{
    X4ScriptPersistenceRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = PERSISTENCE_MAGIC;
    rec.version = PERSISTENCE_VERSION;
    rec.warning_accepted = warning_accepted ? 1u : 0u;
    const char *name = saved_script ? saved_script : "";
    if (strchr(name, '/')) {
        bool found = false;
        for (unsigned origin = 1; origin < sizeof(script_origins) / sizeof(script_origins[0]); ++origin) {
            size_t length = strlen(script_origins[origin]);
            if (strncmp(name, script_origins[origin], length) == 0 && name[length] == '/') {
                rec.origin = origin;
                name += length + 1;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    if (strlen(name) >= sizeof(rec.selected_script) || strchr(name, '/') || strchr(name, '\\')) return false;
    memcpy(rec.selected_script, name, strlen(name) + 1);
    rec.checksum = calc_checksum(&rec);
    const char *path = get_persistence_path();
    FILE *f = fopen(path, "wb");
#ifdef X4_OPENORBIS
    if (!f) {
        (void)mkdir("/data/xcloud4", 0755);
        f = fopen(path, "wb");
    }
#endif
    if (!f) return false;
    bool ok = fwrite(&rec, 1, sizeof(rec), f) == sizeof(rec) && !ferror(f);
    return fclose(f) == 0 && ok;
}

#endif /* !X4_PUBLIC */
