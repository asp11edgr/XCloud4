/* SPDX-License-Identifier: GPL-3.0-only */
/* Narrow panel fixture only. Captures link the real script engine instead. */
#include "../src/input/script_engine.h"
#include <stdio.h>
#include <string.h>
static unsigned available, scans, loads, unloads;
static bool load_result=true, error_notice;
static bool accepted;
static char saved[256];
void x4_script_settings_fixture_available(unsigned count) { available=count; }
unsigned x4_script_settings_fixture_scans(void) { return scans; }
unsigned x4_script_settings_fixture_loads(void) { return loads; }
void x4_script_settings_fixture_load_result(bool result) { load_result=result; }
unsigned x4_script_settings_fixture_unloads(void) { return unloads; }
bool x4_script_settings_fixture_error_notice(void) { return error_notice; }
int x4_script_engine_scan(X4ScriptEntry *entries,int capacity)
{
    ++scans;
    if(!available||capacity<1)return 0;
    memset(entries,0,sizeof(*entries));
    snprintf(entries[0].path,sizeof(entries[0].path),"/mnt/usb0/xcloud4/scripts/giroscopio.lua");
    snprintf(entries[0].name,sizeof(entries[0].name),"giroscopio.lua");
    entries[0].size=128;
    return 1;
}
bool x4_script_engine_load_persistence(bool *warning,char *path,size_t size)
{
    if(warning)*warning=accepted;
    if(path&&size)snprintf(path,size,"%s",saved);
    return true;
}
bool x4_script_engine_save_persistence(bool warning,const char *path)
{
    accepted=warning;
    snprintf(saved,sizeof(saved),"%s",path?path:"");
    return true;
}
bool x4_script_engine_load(const char *path)
{ ++loads;bool ok=load_result&&path&&*path;error_notice=!ok;return ok; }
void x4_script_engine_unload(void) { ++unloads;error_notice=false; }
