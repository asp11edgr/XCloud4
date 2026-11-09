/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "controller.h"
#include "gamepad.h"

#define X4_MAX_SCRIPTS 32
#define X4_MAX_SCRIPT_SIZE (64 * 1024)
#define X4_SCRIPT_MAX_MEM_BYTES (2 * 1024 * 1024)
#define X4_SCRIPT_INSTRUCTION_BUDGET 50000

typedef struct {
    char path[256];
    char name[64];
    uint32_t size;
} X4ScriptEntry;

typedef struct {
    bool active;
    bool disabled_by_error;
    char disabled_reason[128];
    int disabled_line;
    uint64_t total_calls;
    uint32_t current_mem_bytes;
    uint32_t peak_mem_bytes;
    float last_call_ms;
    float p50_ms;
    float p95_ms;
    float max_ms;
    uint64_t window_calls;
    uint64_t log_messages;
    uint64_t log_coalesced;
    char active_script_name[64];
} X4ScriptMetrics;

#if !X4_PUBLIC
/* Initialize script engine subsystem */
void x4_script_engine_init(void);

/* Shutdown script engine subsystem and free all resources */
void x4_script_engine_shutdown(void);

/* Scan for available scripts in /app0/assets/scripts/, /data/xcloud4/scripts/, /mnt/usb0/xcloud4/scripts/ */
int x4_script_engine_scan(X4ScriptEntry *out_entries, int max_entries);

/* Load a script by path. If path is NULL or empty, unloads active script. Returns true on success. */
bool x4_script_engine_load(const char *path);

/* Unload active script and reset controller to passthrough */
void x4_script_engine_unload(void);

/* Is a script currently loaded and active (and not disabled by error)? */
bool x4_script_engine_is_active(void);

/* Get active script filename or NULL */
const char *x4_script_engine_get_name(void);

/* Check if active script requires motion sensors (gyro/accel) */
bool x4_script_engine_needs_gyro(void);

/* Process controller input through active script before sending to Xbox */
void x4_script_engine_process_input(
    const X4Controller *controller,
    X4GamepadFrame *gamepad,
    float dt,
    uint64_t session_now_us
);

/* Retrieve live metrics for klog and session JSON */
void x4_script_engine_get_metrics(X4ScriptMetrics *out);

/* Periodic 5-second klog logging in Lab */
void x4_script_engine_log_metrics_5s(uint64_t now_us);

/* Export session metrics to JSON file (without script content) */
bool x4_script_engine_export_session_json(const char *path);

/* Check if an on-screen toast notification is active; copies to out_buf if so */
bool x4_script_engine_get_notification(char *out_buf, size_t buf_size);

/* Set an on-screen toast notification (3s duration) */
void x4_script_engine_set_toast(const char *text);

/* Draw the on-screen toast notification (3s duration) if active */
void x4_script_engine_draw_notification(uint32_t *pixels);

#if X4_SCRIPT_ENGINE_TEST
/* Deterministic host-only deadline/metrics clock. Not compiled into native builds. */
void x4_script_engine_test_clock(uint64_t (*clock_us)(void), uint32_t limit_us);
bool x4_script_engine_test_allocator_contract(void);
#endif

/* Load/save v2 persistence: origin plus filename, reconstructed into a 256-byte path. */
bool x4_script_engine_load_persistence(bool *warning_accepted, char *saved_script, size_t max_len);
bool x4_script_engine_save_persistence(bool warning_accepted, const char *saved_script);

#endif /* !X4_PUBLIC */
