/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#include "../src/input/script_engine.h"
#include "../src/input/controller.h"
#include "../src/input/gamepad.h"
#ifndef X4_SCRIPT_TEST_TMPDIR
#error X4_SCRIPT_TEST_TMPDIR must be an isolated test directory
#endif
#ifndef X4_SCRIPT_PERSIST_PATH
#error X4_SCRIPT_PERSIST_PATH must be an isolated test file
#endif

/* Stubs for OpenOrbis functions when compiling controller.c on host */
int32_t scePadInit(void) { return 0; }
int32_t scePadOpen(int32_t u, int32_t t, int32_t i, void *p) { (void)u; (void)t; (void)i; (void)p; return 1; }
int32_t scePadClose(int32_t h) { (void)h; return 0; }
int32_t scePadReadState(int32_t h, OrbisPadData *d) { (void)h; (void)d; return 0; }
int32_t sceUserServiceInitialize(void *p) { (void)p; return 0; }
int32_t sceUserServiceGetInitialUser(int32_t *u) { if (u) *u = 1; return 0; }
void scePadSetMotionSensorState(void) { }

/* Stubs for font functions used in notification drawing */
int x4_text_width_px(int size, int bold, const char *text) {
    (void)size; (void)bold;
    return text ? (int)strlen(text) * 10 : 0;
}
int x4_text_ellipsis_px(uint32_t *pixels, int x, int y, int size, int bold,
                        const char *text, int max_width, uint32_t color) {
    (void)pixels; (void)x; (void)y; (void)size; (void)bold; (void)text;
    (void)max_width; (void)color;
    return 0;
}

static void test_sandbox(void)
{
    printf("1. Testing sandbox security (io, os, debug, bytecode, infinite loop, memory bomb)...\n");

    /* Create temporary script trying to access forbidden libraries */
    const char *forbidden_script =
        "function on_input(pad, out, dt)\n"
        "    if io ~= nil then error('io exists!') end\n"
        "    if os ~= nil then error('os exists!') end\n"
        "    if package ~= nil then error('package exists!') end\n"
        "    if debug ~= nil then error('debug exists!') end\n"
        "    if dofile ~= nil then error('dofile exists!') end\n"
        "    if loadfile ~= nil then error('loadfile exists!') end\n"
        "    out.a = true\n"
        "end\n";

    char path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_forbidden_XXXXXX.lua";
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    assert(write(fd, forbidden_script, strlen(forbidden_script)) == (ssize_t)strlen(forbidden_script));
    close(fd);

    assert(x4_script_engine_load(path));
    assert(x4_script_engine_is_active());

    X4Controller c = {0};
    c.data.connected = 1;
    X4GamepadFrame f = {0};
    x4_script_engine_process_input(&c, &f, 0.0034f, 100000);

    /* Script ran without error, meaning all forbidden libraries were nil */
    assert(f.buttons & X4_GAMEPAD_A);
    assert(x4_script_engine_is_active());
    unlink(path);

    /* Test bytecode rejection: create a script file containing Lua bytecode header */
    char bc_path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_bc_XXXXXX.lua";
    fd = mkstemps(bc_path, 4);
    assert(fd >= 0);
    const char bc_header[] = "\x1bLua\x54\x00\x19\x93\r\n\x1a\n";
    assert(write(fd, bc_header, sizeof(bc_header)) > 0);
    close(fd);

    /* Must fail to load bytecode */
    bool load_bc = x4_script_engine_load(bc_path);
    assert(!load_bc);
    assert(!x4_script_engine_is_active());
    unlink(bc_path);

    /* Test infinite loop protection: while true do end */
    const char *loop_script =
        "function on_input(pad, out, dt)\n"
        "    while true do end\n"
        "end\n";
    char loop_path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_loop_XXXXXX.lua";
    fd = mkstemps(loop_path, 4);
    assert(fd >= 0);
    assert(write(fd, loop_script, strlen(loop_script)) == (ssize_t)strlen(loop_script));
    close(fd);

    assert(x4_script_engine_load(loop_path));
    assert(x4_script_engine_is_active());

    memset(&f, 0, sizeof(f));
    x4_script_engine_process_input(&c, &f, 0.0034f, 200000);

    /* Infinite loop must be aborted by hook and script deactivated */
    assert(!x4_script_engine_is_active());
    X4ScriptMetrics m;
    x4_script_engine_get_metrics(&m);
    assert(m.disabled_by_error);
    assert(strstr(m.disabled_reason, "instruction budget exceeded") != NULL || strstr(m.disabled_reason, "time budget exceeded") != NULL);
    unlink(loop_path);

    /* Test memory limit (2 MB bomb) */
    const char *mem_script =
        "function on_input(pad, out, dt)\n"
        "    local t = {}\n"
        "    while true do\n"
        "        t[#t + 1] = string.rep('A', 32768)\n"
        "    end\n"
        "end\n";
    char mem_path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_mem_XXXXXX.lua";
    fd = mkstemps(mem_path, 4);
    assert(fd >= 0);
    assert(write(fd, mem_script, strlen(mem_script)) == (ssize_t)strlen(mem_script));
    close(fd);

    assert(x4_script_engine_load(mem_path));
    assert(x4_script_engine_is_active());

    memset(&f, 0, sizeof(f));
    x4_script_engine_process_input(&c, &f, 0.0034f, 300000);

    assert(!x4_script_engine_is_active());
    x4_script_engine_get_metrics(&m);
    assert(m.disabled_by_error);
    assert(strstr(m.disabled_reason, "memory") != NULL || strstr(m.disabled_reason, "instruction") != NULL || strstr(m.disabled_reason, "time budget") != NULL);
    unlink(mem_path);

    /* Test runtime error with line number reporting */
    const char *err_script =
        "function on_input(pad, out, dt)\n"
        "    local a = nil\n"
        "    local b = a.foo -- line 3\n"
        "end\n";
    char err_path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_err_XXXXXX.lua";
    fd = mkstemps(err_path, 4);
    assert(fd >= 0);
    assert(write(fd, err_script, strlen(err_script)) == (ssize_t)strlen(err_script));
    close(fd);

    assert(x4_script_engine_load(err_path));
    assert(x4_script_engine_is_active());

    memset(&f, 0, sizeof(f));
    x4_script_engine_process_input(&c, &f, 0.0034f, 400000);

    assert(!x4_script_engine_is_active());
    x4_script_engine_get_metrics(&m);
    assert(m.disabled_by_error);
    assert(m.disabled_line == 3);
    assert(strstr(m.disabled_reason, "nil") != NULL);
    unlink(err_path);

    /* Test syntax error: script does not compile but host continues safely */
    const char *syn_script = "function on_input( this is invalid syntax";
    char syn_path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_syn_XXXXXX.lua";
    fd = mkstemps(syn_path, 4);
    assert(fd >= 0);
    assert(write(fd, syn_script, strlen(syn_script)) == (ssize_t)strlen(syn_script));
    close(fd);

    bool syn_load = x4_script_engine_load(syn_path);
    assert(!syn_load);
    assert(!x4_script_engine_is_active());
    unlink(syn_path);

    printf("PASS: Sandbox security verified.\n");
}

static void test_api_and_conversion(void)
{
    printf("2. Testing API v1 ranges, saturation, and exact conversion...\n");

    /* Script that modifies axes, triggers, and buttons */
    const char *script =
        "function on_input(pad, out, dt)\n"
        "    -- Test saturation: set out of range\n"
        "    out.lx = 2.5\n"
        "    out.ly = -3.0\n"
        "    out.rx = 0.5\n"
        "    out.ry = -0.5\n"
        "    out.lt = 1.8\n"
        "    out.rt = -0.4\n"
        "    out.a = true\n"
        "    out.b = false\n"
        "    out.x = true\n"
        "    out.y = false\n"
        "    out.lb = true\n"
        "    out.rb = false\n"
        "end\n";

    char path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_api_XXXXXX.lua";
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    assert(write(fd, script, strlen(script)) == (ssize_t)strlen(script));
    close(fd);

    assert(x4_script_engine_load(path));
    assert(x4_script_engine_is_active());

    X4Controller c = {0};
    c.data.connected = 1;
    X4GamepadFrame f = {0};
    f.buttons = X4_GAMEPAD_NEXUS; /* Host set nexus */

    x4_script_engine_process_input(&c, &f, 0.0034f, 500000);

    /* Saturation checks: lx clamped to 32767, ly to -32767, lt to 65535, rt to 0 */
    assert(f.left_x == 32767);
    assert(f.left_y == -32767);
    assert(f.right_x >= 16380 && f.right_x <= 16385);
    assert(f.right_y <= -16380 && f.right_y >= -16385);
    assert(f.left_trigger == 65535);
    assert(f.right_trigger == 0);

    /* Button checks */
    assert(f.buttons & X4_GAMEPAD_A);
    assert(!(f.buttons & X4_GAMEPAD_B));
    assert(f.buttons & X4_GAMEPAD_X);
    assert(!(f.buttons & X4_GAMEPAD_Y));
    assert(f.buttons & X4_GAMEPAD_LB);
    assert(!(f.buttons & X4_GAMEPAD_RB));

    /* NEXUS must remain set (host immunity) */
    assert(f.buttons & X4_GAMEPAD_NEXUS);

    unlink(path);
    x4_script_engine_unload();

    /* Without script, passthrough matches x4_controller_gamepad byte for byte */
    c.data.buttons = ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_UP;
    c.data.leftStick.x = 200;
    c.data.leftStick.y = 50;
    c.data.rightStick.x = 30;
    c.data.rightStick.y = 220;
    c.data.analogButtons.l2 = 180;
    c.data.analogButtons.r2 = 90;

    X4GamepadFrame direct = {0};
    x4_controller_gamepad(&c, &direct);

    X4GamepadFrame test_frame = direct;
    assert(!x4_script_engine_is_active());
    x4_script_engine_process_input(&c, &test_frame, 0.0034f, 600000);

    assert(memcmp(&direct, &test_frame, sizeof(X4GamepadFrame)) == 0);
    printf("PASS: API v1 ranges, saturation, and exact conversion verified.\n");
}

static void test_stock_scripts(void)
{
    printf("3. Testing stock scripts with synthetic inputs...\n");

    /* A. reasignar.lua: Swaps Cross (A) and Circle (B) */
    assert(x4_script_engine_load("assets/scripts/reasignar.lua"));
    assert(x4_script_engine_is_active());

    X4Controller c = {0};
    c.data.connected = 1;
    c.data.buttons = ORBIS_PAD_BUTTON_CROSS; /* Cross pressed */

    X4GamepadFrame f = {0};
    x4_controller_gamepad(&c, &f);
    assert(f.buttons == X4_GAMEPAD_A); /* Normal without script */

    x4_script_engine_process_input(&c, &f, 0.0034f, 100000);
    /* In reasignar.lua: a = circle, b = cross. So Cross pressed on pad makes out.b true! */
    assert(f.buttons == X4_GAMEPAD_B);

    /* Circle pressed on pad makes out.a true */
    c.data.buttons = ORBIS_PAD_BUTTON_CIRCLE;
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.0034f, 103400);
    assert(f.buttons == X4_GAMEPAD_A);

    x4_script_engine_unload();

    /* B. sticks.lua: Deadzones and curves */
    assert(x4_script_engine_load("assets/scripts/sticks.lua"));
    assert(x4_script_engine_is_active());

    /* Small stick input inside deadzone (0.04 < 0.08 deadzone) */
    c.data.buttons = 0;
    c.data.leftStick.x = 128 + 5; /* ~0.039 */
    c.data.leftStick.y = 128;
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.0034f, 200000);
    assert(f.left_x == 0 && f.left_y == 0); /* Cut to zero by deadzone */

    /* Large stick input (full deflection 1.0) */
    c.data.leftStick.x = 255;
    c.data.leftStick.y = 128;
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.0034f, 203400);
    assert(f.left_x == 32767);

    x4_script_engine_unload();

    /* C. turbo.lua: 10 Hz turbo on button A */
    assert(x4_script_engine_load("assets/scripts/turbo.lua"));
    assert(x4_script_engine_is_active());

    c.data.buttons = ORBIS_PAD_BUTTON_CROSS;
    /* At dt = 0.02s: should be ON (period is 0.1s, half period is 0.05s) */
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.02f, 300000);
    assert(f.buttons & X4_GAMEPAD_A);

    /* Advance by 0.04s (total 0.06s in period): should be OFF */
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.04f, 340000);
    assert(!(f.buttons & X4_GAMEPAD_A));

    /* Advance by 0.05s (total 0.11s -> wrapped to 0.01s): should be ON again */
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.05f, 390000);
    assert(f.buttons & X4_GAMEPAD_A);

    x4_script_engine_unload();

    /* D. giroscopio.lua: Gyro aiming to right stick */
    assert(x4_script_engine_load("assets/scripts/giroscopio.lua"));
    assert(x4_script_engine_is_active());

    /* In reposo (stationary): gyro = 0, sticks centered */
    memset(&c, 0, sizeof(c));
    c.data.connected = 1;
    c.data.leftStick.x = 128;
    c.data.leftStick.y = 128;
    c.data.rightStick.x = 128;
    c.data.rightStick.y = 128;
    c.data.analogButtons.l2 = 255; /* L2 held (aiming mode) */
    c.data.vel.x = 0.0f;
    c.data.vel.y = 0.0f;
    c.data.vel.z = 0.0f;

    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.0034f, 400000);
    assert(f.right_x == 0 && f.right_y == 0);

    /* Manual calibration: L3 + R3 pressed */
    c.data.buttons = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
    c.data.vel.x = 0.05f; /* Small resting drift */
    c.data.vel.y = -0.05f;
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.0034f, 403400);

    /* Now when at the calibrated rest velocity, gyro output is 0 */
    c.data.buttons = 0;
    c.data.leftStick.x = 128;
    c.data.leftStick.y = 128;
    c.data.rightStick.x = 128;
    c.data.rightStick.y = 128;
    x4_controller_gamepad(&c, &f);
    x4_script_engine_process_input(&c, &f, 0.0034f, 406800);
    assert(abs(f.right_x) <= 100 && abs(f.right_y) <= 100);

    /* Constant rotation on Yaw (turning right, e.g. 0.5 rad/s) produces right-stick deflection */
    c.data.vel.y = 0.5f;
    for (int step = 0; step < 10; ++step) {
        x4_controller_gamepad(&c, &f);
        x4_script_engine_process_input(&c, &f, 0.0034f, 410000 + step * 3400);
    }
    assert(f.right_x > 5000); /* Smooth deflection to the right */

    x4_script_engine_unload();
    printf("PASS: Stock scripts verified.\n");
}

static void test_local_controls_and_bounded_memory(void)
{
    printf("4. Testing local NEXUS immunity and bounded memory...\n");

    /* Script that maliciously tries to zero out all buttons and sticks */
    const char *zero_script =
        "function on_input(pad, out, dt)\n"
        "    for k, _ in pairs(out) do\n"
        "        if type(out[k]) == 'boolean' then out[k] = false\n"
        "        else out[k] = 0.0 end\n"
        "    end\n"
        "end\n";

    char path[] = X4_SCRIPT_TEST_TMPDIR "/x4_test_zero_XXXXXX.lua";
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    assert(write(fd, zero_script, strlen(zero_script)) == (ssize_t)strlen(zero_script));
    close(fd);

    assert(x4_script_engine_load(path));
    assert(x4_script_engine_is_active());

    X4Controller c = {0};
    c.data.connected = 1;
    X4GamepadFrame f = {0};
    f.buttons = X4_GAMEPAD_NEXUS; /* Local nexus */

    x4_script_engine_process_input(&c, &f, 0.0034f, 500000);

    /* NEXUS must survive even if script zeros everything */
    assert(f.buttons & X4_GAMEPAD_NEXUS);

    /* Verify performance & allocations: run 1000 iterations */
    X4ScriptMetrics m_start, m_end;
    x4_script_engine_get_metrics(&m_start);

    for (int i = 0; i < 1000; ++i) {
        x4_script_engine_process_input(&c, &f, 0.0034f, 500000 + i * 3400);
    }

    x4_script_engine_get_metrics(&m_end);
    assert(m_end.total_calls >= 1000);
    assert(x4_script_engine_is_active());

    /* Host allocations check: memory should remain bounded and not leak */
    assert(m_end.current_mem_bytes < 512 * 1024);

    unlink(path);
    x4_script_engine_unload();
    printf("PASS: Local NEXUS immunity and bounded memory verified.\n");
}

static void test_persistence(void)
{
    printf("5. Testing persistence of lab-scripts-v2.bin...\n");

    unlink(X4_SCRIPT_PERSIST_PATH);

    /* Absent file: must default safely */
    bool warning_acc = true;
    char saved[64] = "initial";
    bool ok = x4_script_engine_load_persistence(&warning_acc, saved, sizeof(saved));
    assert(!ok);
    assert(!warning_acc);
    assert(saved[0] == '\0');

    /* Save accepted warning with selected script */
    assert(x4_script_engine_save_persistence(true, "giroscopio.lua"));

    /* Reload */
    assert(x4_script_engine_load_persistence(&warning_acc, saved, sizeof(saved)));
    assert(warning_acc == true);
    assert(strcmp(saved, "giroscopio.lua") == 0);

    /* Corrupt file: wrong magic */
    FILE *f = fopen(X4_SCRIPT_PERSIST_PATH, "r+b");
    assert(f != NULL);
    uint32_t bad_magic = 0x11223344;
    assert(fwrite(&bad_magic, 1, 4, f) == 4);
    fclose(f);

    warning_acc = true;
    saved[0] = 'X';
    ok = x4_script_engine_load_persistence(&warning_acc, saved, sizeof(saved));
    assert(!ok);
    assert(!warning_acc);

    /* Clean up */
    unlink(X4_SCRIPT_PERSIST_PATH);

    printf("PASS: Persistence verified.\n");
}


static void load_text(const char *text, char path[512])
{
    snprintf(path, 512, "%s/case-XXXXXX.lua", X4_SCRIPT_TEST_TMPDIR);
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
    assert(x4_script_engine_load(path));
}

static void test_corrupted_tables(void)
{
    static const char * const bodies[] = {
        "pad.gyro = nil", "pad.touch = 5", "pad.touch[1] = nil",
        "setmetatable(pad,{__index=function() error('index') end})",
        "setmetatable(pad,{__newindex=function() error('newindex') end})",
        "setmetatable(out,{__index=function() error('index') end})",
        "setmetatable(out,{__newindex=function() error('newindex') end})"
    };
    for (size_t i = 0; i < sizeof(bodies)/sizeof(bodies[0]); ++i) {
        char code[512], path[512];
        snprintf(code, sizeof(code), "function on_input(pad,out,dt) out.a=true; %s end", bodies[i]);
        load_text(code, path);
        X4Controller c = {0}; c.data.connected = 1;
        X4GamepadFrame direct = {0}; direct.buttons = X4_GAMEPAD_NEXUS;
        direct.left_x = 1234;
        X4GamepadFrame frame = direct;
        x4_script_engine_process_input(&c, &frame, .004f, 1);
        assert(!x4_script_engine_is_active());
        assert(memcmp(&frame, &direct, sizeof(frame)) == 0);
        x4_script_engine_process_input(&c, &frame, .004f, 4001);
        assert(memcmp(&frame, &direct, sizeof(frame)) == 0);
        unlink(path);
    }
    puts("PASS: seven table/metatable mutations disable safely and preserve current/next passthrough.");
}

static void test_literal_strings(void)
{
    const char *code = "function on_input(pad,out,dt) "
        "assert(string.match==nil and string.gmatch==nil and string.gsub==nil); "
        "assert(string.find(string.rep('a',300), '.-.-.-.-b')==nil); "
        "local a,b=string.find('a.b','.'); assert(a==2 and b==2); "
        "a,b=string.find('a.b','.',1,false); assert(a==2 and b==2); out.a=true end";
    char path[512]; load_text(code, path);
    X4Controller c = {0}; X4GamepadFrame f = {0};
    x4_script_engine_process_input(&c, &f, .004f, 1);
    assert(x4_script_engine_is_active() && (f.buttons & X4_GAMEPAD_A));
    unlink(path);
    puts("PASS: pattern functions absent; find is literal even with plain=false.");
}

static uint64_t fake_now;
static uint64_t fake_step;
static uint64_t fake_clock(void) { fake_now += fake_step; return fake_now; }

static void test_time_budget(void)
{
    char path[512];
    load_text("function on_input(pad,out,dt) local n=0 for i=1,100000 do n=n+i end out.a=true end", path);
    fake_now = 10000000; fake_step = 10;
    x4_script_engine_test_clock(fake_clock, 5);
    X4Controller c = {0}; X4GamepadFrame f = {0};
    x4_script_engine_process_input(&c, &f, .004f, 1);
    X4ScriptMetrics m; x4_script_engine_get_metrics(&m);
    assert(!m.active && m.disabled_by_error);
    assert(strstr(m.disabled_reason, "time budget exceeded"));
    assert(m.total_calls == 1 && f.buttons == 0);
    x4_script_engine_test_clock(NULL, 0);
    unlink(path);
    puts("PASS: deterministic low test deadline disables before instruction cap.");
}

static void test_c_sort_real_deadline(void)
{
    /* Prepare the expensive input without making setup CPU speed part of
     * the deadline assertion. The invocation below uses the REAL clock. */
    fake_now=50000000;fake_step=0;
    x4_script_engine_test_clock(fake_clock,2000);
    char path[512];
    load_text("local t={} local k=0 function on_input(pad,out,dt) "
              "if k<30000 then for i=1,1000 do k=k+1 t[k]=30001-k end return end "
              "while true do table.sort(t) end end",path);
    X4Controller c={0};X4GamepadFrame f={0};
    /* Thirty bounded setup calls preserve the unchanged 50000-instruction
     * budget; building all 30000 values in one chunk exceeds that budget. */
    for(unsigned i=0;i<30;++i) {
        x4_script_engine_process_input(&c,&f,.004f,1+i*4000);
        assert(x4_script_engine_is_active());
    }
    x4_script_engine_test_clock(NULL,0);
    struct timespec start,done;
    assert(clock_gettime(CLOCK_MONOTONIC,&start)==0);
    x4_script_engine_process_input(&c,&f,.004f,1);
    assert(clock_gettime(CLOCK_MONOTONIC,&done)==0);
    uint64_t elapsed=(uint64_t)(done.tv_sec-start.tv_sec)*1000000ULL;
    if(done.tv_nsec>=start.tv_nsec)elapsed+=(uint64_t)(done.tv_nsec-start.tv_nsec)/1000;
    else elapsed-=(uint64_t)(start.tv_nsec-done.tv_nsec)/1000;
    X4ScriptMetrics m;x4_script_engine_get_metrics(&m);
    printf("C_SORT_REAL_DEADLINE elapsed_us=%llu limit_us=50000\n",(unsigned long long)elapsed);
    assert(!m.active&&m.disabled_by_error&&strstr(m.disabled_reason,"time budget exceeded"));
    assert(elapsed<50000);
    unlink(path);
    puts("PASS: repeated C table.sort of30000 elements stops within50ms using the real clock.");
}

static unsigned turbo_edges(unsigned hz)
{
    assert(x4_script_engine_load("assets/scripts/turbo.lua"));
    X4Controller c = {0}; c.data.connected=1; c.data.buttons=ORBIS_PAD_BUTTON_CROSS;
    unsigned rises=0; bool previous=false;
    for (unsigned i=0; i<hz*4; ++i) {
        X4GamepadFrame f={0};
        x4_controller_gamepad(&c,&f);
        x4_script_engine_process_input(&c,&f,1.0f/(float)hz,(uint64_t)i*1000000/hz);
        bool current=(f.buttons&X4_GAMEPAD_A)!=0;
        if(current&&!previous)++rises;
        previous=current;
        assert(x4_script_engine_is_active());
    }
    return rises;
}

static void test_turbo_frequency_and_lt(void)
{
    unsigned a=turbo_edges(250),b=turbo_edges(330);
    assert(a>=39&&a<=41&&b>=39&&b<=41);
    assert(abs((int)a-(int)b)<=1);
    /* Generate an isolated lt configuration from the actual stock file. */
    FILE *input=fopen("assets/scripts/turbo.lua","rb"); assert(input);
    char code[8192]; size_t n=fread(code,1,sizeof(code)-1,input); fclose(input); code[n]=0;
    const char *needle="local turbo_button = \"a\"";
    char *at=strstr(code,needle); assert(at);
    char configured[8192];
    size_t prefix=(size_t)(at-code);
    memcpy(configured,code,prefix);
    snprintf(configured+prefix,sizeof(configured)-prefix,"local turbo_button = \"lt\"%s",at+strlen(needle));
    char path[512]; load_text(configured,path);
    X4Controller c={0}; c.data.connected=1;
    X4GamepadFrame f={0};
    c.data.analogButtons.l2=0;
    x4_script_engine_process_input(&c,&f,.004f,1); assert(f.left_trigger==0);
    c.data.analogButtons.l2=127;
    x4_script_engine_process_input(&c,&f,.004f,4001); assert(f.left_trigger==0);
    c.data.analogButtons.l2=255;
    x4_script_engine_process_input(&c,&f,.004f,8001); assert(f.left_trigger==65535);
    assert(x4_script_engine_is_active()); unlink(path);
    printf("PASS: turbo edges over4s 250Hz=%u 330Hz=%u; lt threshold works.\n",a,b);
}

static void test_long_USB_persistence(void)
{
    const char *full="/mnt/usb0/xcloud4/scripts/usb-abcdefghijklmnopqrstuvwxyz-01234567890123456789.lua";
    assert(strlen(full)>64);
    assert(x4_script_engine_save_persistence(true,full));
    bool accepted=false; char restored[256];
    assert(x4_script_engine_load_persistence(&accepted,restored,sizeof(restored)));
    assert(accepted&&strcmp(restored,full)==0);
    char short_result[32]; accepted=true;
    assert(!x4_script_engine_load_persistence(&accepted,short_result,sizeof(short_result)));
    assert(!accepted&&short_result[0]==0);
    unlink(X4_SCRIPT_PERSIST_PATH);
    puts("PASS: persistence reconstructs complete USB origin+long filename without truncation.");
}

static void test_escaped_export_and_log_limit(void)
{
    char path[512];
    load_text("function on_input(pad,out,dt) for i=1,50 do print('print',i); xcloud.log('helper') end end",path);
    fake_now=20000000;fake_step=0;
    x4_script_engine_test_clock(fake_clock,2000);
    X4Controller c={0};X4GamepadFrame f={0};
    x4_script_engine_process_input(&c,&f,.004f,1);
    X4ScriptMetrics m;x4_script_engine_get_metrics(&m);
    assert(m.active&&m.log_messages==10&&m.log_coalesced==90);
    x4_script_engine_test_clock(NULL,0);unlink(path);
    load_text("function on_input(pad,out,dt) error('reason \"quoted\"' .. string.char(255)) end",path);
    x4_script_engine_process_input(&c,&f,.004f,1);
    x4_script_engine_get_metrics(&m);
    assert(m.disabled_by_error&&m.current_mem_bytes>0&&m.active_script_name[0]);
    assert(x4_script_engine_export_session_json(X4_SCRIPT_TEST_TMPDIR "/metrics.json"));
    x4_script_engine_unload();
    FILE *exported=fopen(X4_SCRIPT_TEST_TMPDIR "/metrics.json","rb");assert(exported);
    char json[4096];size_t n=fread(json,1,sizeof(json)-1,exported);fclose(exported);json[n]=0;
    assert(strstr(json,"\\\"quoted\\\""));
    assert(strstr(json,"function on_input")==NULL);
    unlink(path);
    puts("PASS: print shares10/s budget; escaped metrics exported before unload (Python validates JSON).");
}

static void test_histogram_window(void)
{
    fake_now=30000000;fake_step=0;x4_script_engine_test_clock(fake_clock,2000);
    char path[512];load_text("function on_input(pad,out,dt) end",path);
    X4Controller c={0};X4GamepadFrame f={0};
    for(unsigned i=0;i<1500;++i) {
        fake_now=30000000+(uint64_t)i*3333;
        x4_script_engine_process_input(&c,&f,.003333f,fake_now);
    }
    X4ScriptMetrics m;x4_script_engine_get_metrics(&m);
    assert(m.window_calls==1500&&m.p50_ms==0&&m.p95_ms==0&&m.max_ms==0);
    x4_script_engine_log_metrics_5s(35000001);
    x4_script_engine_get_metrics(&m);assert(m.window_calls==0);
    /* One descheduled-call analogue: >=2048 us falls in the overflow bucket. */
    fake_step=5000;
    x4_script_engine_process_input(&c,&f,.003333f,fake_now);
    x4_script_engine_get_metrics(&m);
    assert(m.window_calls==1&&m.disabled_by_error&&m.max_ms==10.0f);
    assert(m.p50_ms==m.max_ms&&m.p95_ms==m.max_ms);
    puts("PASS: overflow quantiles conservatively equal exact10.000ms maximum.");
    x4_script_engine_test_clock(NULL,0);unlink(path);
    puts("PASS: histogram retains all1500 calls of5s window; rollover clears fixed bins without sorting.");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== RUNNING SCRIPT ENGINE TEST SUITE ===\n");
    x4_script_engine_init();
    assert(x4_script_engine_test_allocator_contract());
    puts("PASS: allocator aligned; shrink at full quota; failed growth preserves old block; growth copy/free; zero live bytes.");
    if (argc == 2 && strcmp(argv[1], "--histogram-only") == 0) {
        test_histogram_window();
        x4_script_engine_shutdown();
        puts("PASS: histogram-only mode; sandbox and timing cases not repeated.");
        return 0;
    }
    assert(argc == 1);

    test_sandbox();
    test_api_and_conversion();
    test_stock_scripts();
    test_local_controls_and_bounded_memory();
    test_persistence();
    test_corrupted_tables();
    test_literal_strings();
    test_time_budget();
    test_c_sort_real_deadline();
    test_turbo_frequency_and_lt();
    test_long_USB_persistence();
    test_escaped_export_and_log_limit();
    test_histogram_window();

    x4_script_engine_shutdown();
    printf("=== ALL SCRIPT ENGINE TESTS PASSED ===\n");
    return 0;
}
