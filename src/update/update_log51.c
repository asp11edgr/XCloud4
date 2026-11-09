/* SPDX-License-Identifier: GPL-3.0-only */
#include "update_log51.h"

#include "../auth/json.h"
#include "../core/app_identity.h"
#include "diag_file51.h"
#include "update_io51.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#else
#include <sched.h>
#endif

enum { LOG51_ENTRIES = 20, LOG51_MAX_BYTES = 32768 };
typedef struct {
    X4UpdateLog51 summary;
    X4UpdateLog51Detail detail;
    char app_ver[6];
} JournalRow;

static const char journal51[] = "/data/xcloud4/update-log.json";
static atomic_flag journal_lock = ATOMIC_FLAG_INIT;
static const char *const fs_method_names[X4_INSTALL51_FS_COUNT] = {
    [X4_INSTALL51_FS_OPEN] = "open",
    [X4_INSTALL51_FS_FSTATVFS] = "fstatvfs",
    [X4_INSTALL51_FS_FSTATFS] = "_fstatfs",
    [X4_INSTALL51_FS_CLOSE] = "close",
    [X4_INSTALL51_FS_STATVFS] = "statvfs",
    [X4_INSTALL51_FS_STATFS] = "statfs",
};

static void lock_journal(void)
{
    while (atomic_flag_test_and_set_explicit(&journal_lock, memory_order_acquire)) {
#ifdef X4_OPENORBIS
        (void)sceKernelUsleep(50);
#else
        (void)sched_yield();
#endif
    }
}

static void unlock_journal(void)
{
    atomic_flag_clear_explicit(&journal_lock, memory_order_release);
}

static const char *step_name(X4Update51Step step)
{
    static const char *const names[] = {
        [X4_UPDATE51_STEP_VALIDATION] = "validacion",
        [X4_UPDATE51_STEP_START] = "inicio_worker",
        [X4_UPDATE51_STEP_MANIFEST] = "manifiesto",
        [X4_UPDATE51_STEP_DOWNLOAD] = "copia_paquete",
        [X4_UPDATE51_STEP_VERIFY] = "verificacion_paquete",
        [X4_UPDATE51_STEP_INSTALL] = "instalacion",
        [X4_UPDATE51_STEP_ACCEPTED] = "instalacion_aceptada",
        [X4_UPDATE51_STEP_CANCELLED] = "cancelado",
        [X4_UPDATE51_STEP_LEGACY] = "anterior_sin_detalle",
    };
    return (unsigned)step < sizeof(names) / sizeof(names[0]) ? names[step] : "validacion";
}

static bool read_uint51(X4JsonSpan obj, const char *key, uint64_t *out, bool *negative)
{
    X4JsonSpan field;
    if (x4_json_member(obj, key, &field) != 1 || x4_json_type(field) != X4_JSON_T_NUMBER ||
        !field.length) return false;
    const bool minus = field.data[0] == '-';
    if (minus && !negative) return false;
    if (negative) *negative = minus;
    uint64_t value = 0;
    for (size_t i = minus ? 1u : 0u; i < field.length; ++i) {
        const unsigned digit = (unsigned)(field.data[i] - '0');
        if (digit > 9 || value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static bool read_int51(X4JsonSpan obj, const char *key, int32_t *out)
{
    uint64_t value;
    bool negative;
    if (!read_uint51(obj, key, &value, &negative) ||
        value > (uint64_t)INT32_MAX + (negative ? 1u : 0u)) return false;
    *out = negative ? (int32_t)-(int64_t)value : (int32_t)value;
    return true;
}

static bool read_bool51(X4JsonSpan obj, const char *key, bool *out)
{
    X4JsonSpan field;
    int value;
    if (x4_json_member(obj, key, &field) != 1 || x4_json_bool(field, &value)) return false;
    *out = value != 0;
    return true;
}

static bool optional_string(X4JsonSpan obj, const char *key, char *out, size_t cap, const char *allowed)
{
    X4JsonSpan field;
    const int found = x4_json_member(obj, key, &field);
    return found == 0 || (found == 1 && x4_json_type(field) == X4_JSON_T_STRING &&
        !x4_json_token(field, out, cap) && strspn(out, allowed) == strlen(out));
}

static bool read_fs_call(X4JsonSpan obj, const char *key, X4Install51FsCall *call)
{
    X4JsonSpan field;
    const int found = x4_json_member(obj, key, &field);
    if (!found) return true;
    if (found != 1 || !read_int51(field, "code", &call->code) ||
        !read_int51(field, "errno", &call->system_errno) || call->system_errno < 0) return false;
    X4JsonSpan validation;
    if (x4_json_member(field, "validation_errno", &validation) != 0 &&
        (!read_int51(field, "validation_errno", &call->validation_errno) ||
            call->validation_errno < 0)) return false;
    call->attempted = true;
    return true;
}

static bool read_fs_detail(X4JsonSpan obj, X4UpdateLog51Detail *detail)
{
    X4JsonSpan methods;
    const int found = x4_json_member(obj, "space_methods", &methods);
    if (found != 0) {
        if (found != 1 || x4_json_type(methods) != X4_JSON_T_OBJECT ||
            !read_bool51(obj, "espacio_desconocido", &detail->space.unknown)) return false;
        for (unsigned i = 0; i < X4_INSTALL51_FS_COUNT; ++i)
            if (!read_fs_call(methods, fs_method_names[i], &detail->space.calls[i])) return false;
        if (!detail->space.calls[X4_INSTALL51_FS_OPEN].attempted) return false;
    }
    return read_fs_call(obj, "chmod_update", &detail->chmod_update);
}

static bool valid_call(const char *name)
{
    static const char *const calls[] = {"AppInstUtilInit", "GetTitleId", "BGFTInit", "Register",
        "Start", "Progress", "BGFTTerm", "AppInstUtilTerm", "AppInstUtilFallback"};
    if (!name[0]) return true;
    for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); ++i)
        if (!strcmp(name, calls[i])) return true;
    return false;
}

static bool read_install_detail(X4JsonSpan obj, X4UpdateLog51Detail *detail)
{
    if (!optional_string(obj, "target_app_ver", detail->target_app_ver,
            sizeof(detail->target_app_ver), "0123456789.") ||
        !optional_string(obj, "pending_call", detail->pending_call, sizeof(detail->pending_call),
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ||
        !optional_string(obj, "last_call", detail->last_call, sizeof(detail->last_call),
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ||
        !valid_call(detail->pending_call) || !valid_call(detail->last_call)) return false;
    if (detail->target_app_ver[0] && (strlen(detail->target_app_ver) != 5 ||
        strspn(detail->target_app_ver, "0123456789") != 2 || detail->target_app_ver[2] != '.' ||
        strspn(detail->target_app_ver + 3, "0123456789") != 2)) return false;
    if (detail->last_call[0] && !read_int51(obj, "last_call_rc", &detail->last_call_rc)) return false;
    X4JsonSpan closed;
    if (x4_json_member(obj, "closed_utc", &closed) != 0 &&
        !read_uint51(obj, "closed_utc", &detail->closed_utc, NULL)) return false;
    char mode[16] = {0};
    X4JsonSpan failed;
    if (!optional_string(obj, "modo", mode, sizeof(mode), "abcdefghijklmnopqrstuvwxyz") ||
        !optional_string(obj, "ruta", detail->manual_path, sizeof(detail->manual_path),
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz/._-")) return false;
    detail->manual = !strcmp(mode, "manual");
    detail->diagnostic = !strcmp(mode, "diagnostico");
    if (mode[0] && !detail->manual && !detail->diagnostic) return false;
    if (x4_json_member(obj, "bgft_failed", &failed) != 0 &&
        !read_bool51(obj, "bgft_failed", &detail->bgft_failed)) return false;
    return true;
}

static bool read_row51(X4JsonSpan obj, JournalRow *entry)
{
    *entry = (JournalRow){0};
    X4UpdateLog51 *row = &entry->summary;
    X4JsonSpan build;
    uint64_t run, calls;
    if (!read_uint51(obj, "run", &run, NULL) || !run || run > UINT32_MAX ||
        !read_uint51(obj, "utc", &row->utc, NULL) ||
        x4_json_member(obj, "build", &build) != 1 ||
        x4_json_token(build, row->build, sizeof(row->build)) || !row->build[0] ||
        strspn(row->build, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-") != strlen(row->build) ||
        !read_bool51(obj, "local", &row->local) ||
        !read_uint51(obj, "calls", &calls, NULL) || calls > (X4_INSTALL51_APP_TERM * 2u - 1u) ||
        !read_bool51(obj, "accepted", &row->installer.accepted) ||
        !read_bool51(obj, "transfer_complete", &row->installer.transfer_complete) ||
        !read_bool51(obj, "progress_timeout", &row->installer.progress_timeout)) return false;
    row->run = (uint32_t)run;
    row->installer.calls = (unsigned)calls;
    const struct { const char *key; int32_t *target; } fields[] = {
        {"stage", &row->stage}, {"code", &row->code},
        {"space_code", &row->space_code}, {"space_errno", &row->space_errno},
        {"task_id", &row->installer.task_id}, {"app_init", &row->installer.app_init_code},
        {"title", &row->installer.title_code}, {"bgft_init", &row->installer.bgft_init_code},
        {"register", &row->installer.register_code}, {"start", &row->installer.start_code},
        {"progress_query", &row->installer.progress_query_code}, {"progress_error", &row->installer.progress_error},
        {"fallback", &row->installer.fallback_code}, {"bgft_term", &row->installer.bgft_term_code},
        {"app_term", &row->installer.app_term_code},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (!read_int51(obj, fields[i].key, fields[i].target)) return false;
    X4JsonSpan field;
    int32_t step = X4_UPDATE51_STEP_LEGACY;
    if (x4_json_member(obj, "step", &field) != 0 &&
        (!read_int51(obj, "step", &step) || step < 0 || step > X4_UPDATE51_STEP_LEGACY)) return false;
    entry->detail.step = (X4Update51Step)step;
    if (x4_json_member(obj, "errno", &field) != 0 &&
        (!read_int51(obj, "errno", &entry->detail.system_errno) || entry->detail.system_errno < 0)) return false;
    if (!optional_string(obj, "app_ver", entry->app_ver, sizeof(entry->app_ver), "0123456789.") ||
        !optional_string(obj, "function", entry->detail.function, sizeof(entry->detail.function),
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_.")) return false;
    return read_fs_detail(obj, &entry->detail) && read_install_detail(obj, &entry->detail);
}

static bool load_rows51(JournalRow rows[LOG51_ENTRIES], size_t *count)
{
    *count = 0;
    uint64_t length = 0;
    FILE *file = x4_update51_open_regular(journal51, &length, NULL);
    if (!file) return errno == ENOENT;
    if (!length || length > LOG51_MAX_BYTES) {
        (void)fclose(file);
        return false;
    }
    const size_t size = (size_t)length;
    char *body = malloc(size);
    bool ok = body && !x4_update51_read_all(file, body, size) && !x4_update51_expect_eof(file);
    if (fclose(file)) ok = false;
    X4JsonSpan root, array, schema, item;
    uint32_t version;
    if (ok) ok = !x4_json_parse(body, size, &root) &&
        x4_json_member(root, "schema", &schema) == 1 && !x4_json_uint32(schema, &version) && version == 1 &&
        x4_json_member(root, "runs", &array) == 1;
    if (ok) {
        size_t cursor = 0;
        int next;
        while ((next = x4_json_item(array, &cursor, &item)) == 1) {
            if (*count == LOG51_ENTRIES || !read_row51(item, &rows[*count]) ||
                (*count && rows[*count - 1].summary.run >= rows[*count].summary.run)) {
                ok = false;
                break;
            }
            ++*count;
        }
        if (next < 0) ok = false;
    }
    free(body);
    return ok;
}

static const char *json_boolean51(bool value)
{
    return value ? "true" : "false";
}

static bool __attribute__((noinline)) journal_write(int fd, const char *text, size_t size)
{
    while (size) {
        const ssize_t done = write(fd, text, size);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) {
            if (!done) errno = EIO;
            return false;
        }
        text += done;
        size -= (size_t)done;
    }
    return true;
}

static bool journal_puts(int fd, const char *text)
{
    return journal_write(fd, text, strlen(text));
}

static bool journal_printf(int fd, const char *format, ...) __attribute__((format(printf, 2, 3)));
static bool journal_printf(int fd, const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n < 0) return false;
    if ((size_t)n >= sizeof(text)) { errno = EOVERFLOW; return false; }
    return journal_write(fd, text, (size_t)n);
}

static bool write_fs_call(int fd, const char *prefix, const char *name, const X4Install51FsCall *call)
{
    return journal_printf(fd, "%s\"%s\":{\"code\":%" PRId32 ",\"errno\":%" PRId32,
        prefix, name, call->code, call->system_errno) &&
        (!call->validation_errno || journal_printf(fd, ",\"validation_errno\":%" PRId32,
            call->validation_errno)) && journal_puts(fd, "}");
}

static bool write_fs_detail(int fd, const X4UpdateLog51Detail *detail)
{
    if (detail->space.calls[X4_INSTALL51_FS_OPEN].attempted) {
        if (!journal_printf(fd, ",\"espacio_desconocido\":%s,\"space_methods\":{",
            json_boolean51(detail->space.unknown))) return false;
        const char *prefix = "";
        for (unsigned i = 0; i < X4_INSTALL51_FS_COUNT; ++i) {
            const X4Install51FsCall *call = &detail->space.calls[i];
            if (!call->attempted) continue;
            if (!write_fs_call(fd, prefix, fs_method_names[i], call)) return false;
            prefix = ",";
        }
        if (!journal_puts(fd, "}")) return false;
    }
    return !detail->chmod_update.attempted || write_fs_call(fd, ",", "chmod_update", &detail->chmod_update);
}

static bool write_install_detail(int fd, const X4UpdateLog51Detail *detail)
{
    return (!detail->target_app_ver[0] ||
            journal_printf(fd, ",\"target_app_ver\":\"%s\"", detail->target_app_ver)) &&
        (!detail->pending_call[0] ||
            journal_printf(fd, ",\"pending_call\":\"%s\"", detail->pending_call)) &&
        (!detail->last_call[0] || journal_printf(fd, ",\"last_call\":\"%s\",\"last_call_rc\":%" PRId32,
            detail->last_call, detail->last_call_rc)) &&
        (!detail->closed_utc || journal_printf(fd, ",\"closed_utc\":%" PRIu64, detail->closed_utc)) &&
        (!detail->bgft_failed || journal_puts(fd, ",\"bgft_failed\":true")) &&
        (!detail->manual || journal_printf(fd, ",\"modo\":\"manual\",\"ruta\":\"%s\"", detail->manual_path)) &&
        (!detail->diagnostic || journal_puts(fd, ",\"modo\":\"diagnostico\""));
}

static bool write_row51(int fd, const JournalRow *entry)
{
    const X4UpdateLog51 *row = &entry->summary;
    const X4UpdateLog51Detail *detail = &entry->detail;
    const X4Install51Result *r = &row->installer;
    return journal_printf(fd,
        "{\"run\":%" PRIu32 ",\"utc\":%" PRIu64 ",\"build\":\"%s\",\"local\":%s,"
        "\"app_ver\":\"%s\",\"step\":%u,\"stage_name\":\"%s\",\"errno\":%" PRId32 ",\"function\":\"%s\","
        "\"stage\":%" PRId32 ",\"code\":%" PRId32 ",\"space_code\":%" PRId32 ",\"space_errno\":%" PRId32 ","
        "\"task_id\":%" PRId32 ",\"calls\":%u,\"app_init\":%" PRId32 ",\"title\":%" PRId32 ","
        "\"bgft_init\":%" PRId32 ",\"register\":%" PRId32 ",\"start\":%" PRId32 ","
        "\"progress_query\":%" PRId32 ",\"progress_error\":%" PRId32 ",\"fallback\":%" PRId32 ","
        "\"bgft_term\":%" PRId32 ",\"app_term\":%" PRId32 ",\"accepted\":%s,"
        "\"transfer_complete\":%s,\"progress_timeout\":%s",
        row->run, row->utc, row->build, json_boolean51(row->local),
        entry->app_ver, (unsigned)detail->step, step_name(detail->step), detail->system_errno, detail->function,
        row->stage, row->code, row->space_code, row->space_errno,
        r->task_id, r->calls, r->app_init_code, r->title_code, r->bgft_init_code,
        r->register_code, r->start_code, r->progress_query_code, r->progress_error,
        r->fallback_code, r->bgft_term_code, r->app_term_code, json_boolean51(r->accepted),
        json_boolean51(r->transfer_complete), json_boolean51(r->progress_timeout)) &&
        write_fs_detail(fd, detail) && write_install_detail(fd, detail) && journal_puts(fd, "}");
}

static bool store_row(X4UpdateLog51 *row, const X4UpdateLog51Detail *detail)
{
    JournalRow rows[LOG51_ENTRIES];
    size_t count;
    if (!row || !load_rows51(rows, &count)) return false;
    const bool new_run = row->run == 0;
    size_t index;
    if (new_run) {
        const uint32_t previous = count ? rows[count - 1].summary.run : 0;
        if (previous == UINT32_MAX) return false;
        row->run = previous + 1;
        const time_t now = time(NULL);
        row->utc = now > 0 ? (uint64_t)now : 0;
        _Static_assert(sizeof(X4_PRODUCT_VERSION) <= sizeof(row->build), "version does not fit journal");
        memcpy(row->build, X4_PRODUCT_VERSION, sizeof(X4_PRODUCT_VERSION));
        if (count == LOG51_ENTRIES) {
            --count;
            memmove(rows, rows + 1, count * sizeof(rows[0]));
        }
        index = count++;
        rows[index] = (JournalRow){.detail.step = X4_UPDATE51_STEP_LEGACY};
        memcpy(rows[index].app_ver, X4_APP_VERSION, sizeof(X4_APP_VERSION));
    } else {
        for (index = 0; index < count; ++index)
            if (rows[index].summary.run == row->run) break;
        if (index == count) return false;
    }
    rows[index].summary = *row;
    if (detail) {
        const uint64_t closed = rows[index].detail.closed_utc;
        rows[index].detail = *detail;
        if (!detail->closed_utc) rows[index].detail.closed_utc = closed;
    }
    char temporary[X4_DIAG51_PATH_CAPACITY];
    const int fd = x4_diag51_open_fd(journal51, temporary, row->utc);
    bool saved = false;
    if (fd >= 0) {
        bool ok = journal_puts(fd, "{\"schema\":1,\"runs\":[\n");
        for (size_t i = 0; ok && i < count; ++i)
            ok = (!i || journal_puts(fd, ",\n")) && write_row51(fd, &rows[i]);
        if (ok) ok = journal_puts(fd, "\n]}\n");
        const int error = x4_diag51_commit_fd(fd, temporary, journal51, ok);
        if (error) errno = error;
        saved = error == 0;
    }
    if (!saved && new_run) row->run = 0;
    return saved;
}

bool x4_update_log51_store_detail(X4UpdateLog51 *row, const X4UpdateLog51Detail *detail)
{
    lock_journal();
    const bool saved = store_row(row, detail);
    unlock_journal();
    return saved;
}

bool x4_update_log51_store(X4UpdateLog51 *row)
{
    return x4_update_log51_store_detail(row, NULL);
}

bool x4_update_log51_last(X4UpdateLog51 *row)
{
    JournalRow rows[LOG51_ENTRIES];
    size_t count;
    lock_journal();
    const bool found = row && load_rows51(rows, &count) && count;
    if (found) *row = rows[count - 1].summary;
    unlock_journal();
    return found;
}

#if !X4_PUBLIC
static bool same_build(const JournalRow *row)
{
    return !strcmp(row->app_ver, X4_APP_VERSION) && !strcmp(row->summary.build, X4_PRODUCT_VERSION);
}

static bool interrupted(const char *target, char *call, size_t capacity, bool *local)
{
    JournalRow rows[LOG51_ENTRIES];
    size_t count;
    bool found = false;
    if (call && capacity) call[0] = '\0';
    if (local) *local = false;
    lock_journal();
    if (load_rows51(rows, &count)) {
        while (count) {
            const JournalRow *row = &rows[--count];
            if (!same_build(row) ||
                !row->detail.target_app_ver[0] || (target && strcmp(target, row->detail.target_app_ver)) ||
                (!row->detail.pending_call[0] && !row->detail.last_call[0])) continue;
            const char *pending = row->detail.pending_call;
            const size_t length = strlen(pending);
            if (length && (!call || length < capacity)) {
                if (call) memcpy(call, pending, length + 1);
                if (local) *local = row->summary.local;
                found = true;
            }
            break;
        }
    }
    unlock_journal();
    return found;
}

bool x4_update_log51_interrupted(char *call, size_t capacity, bool *local)
{
    return call && capacity && interrupted(NULL, call, capacity, local);
}

bool x4_update_log51_interrupted_target(const char *app_ver, bool *local)
{
    return app_ver && interrupted(app_ver, NULL, 0, local);
}

X4Update51Resume x4_update_log51_resume(const char *target, bool *local, uint64_t *closed_utc)
{
    JournalRow rows[LOG51_ENTRIES];
    size_t count;
    X4Update51Resume found = X4_UPDATE51_RESUME_NONE;
    if (local) *local = false;
    if (closed_utc) *closed_utc = 0;
    lock_journal();
    if (target && load_rows51(rows, &count)) {
        while (count) {
            const JournalRow *row = &rows[--count];
            if (!same_build(row) || strcmp(row->detail.target_app_ver, target)) continue;
            const bool failed = row->detail.bgft_failed ||
                (row->detail.pending_call[0] && !row->summary.installer.accepted);
            if (failed || row->detail.manual) {
                found = X4_UPDATE51_RESUME_MANUAL;
                if (local) *local = row->summary.local;
                break;
            }
            if (row->summary.installer.accepted) {
                found = X4_UPDATE51_RESUME_ACCEPTED;
                if (local) *local = row->summary.local;
                if (closed_utc) *closed_utc = row->detail.closed_utc;
                break;
            }
            if (row->detail.last_call[0]) break;
        }
    }
    unlock_journal();
    return found;
}

bool x4_update_log51_note_exit(uint64_t utc)
{
    JournalRow rows[LOG51_ENTRIES];
    size_t count;
    bool saved = false;
    lock_journal();
    if (utc && load_rows51(rows, &count)) {
        while (count) {
            JournalRow *row = &rows[--count];
            if (!same_build(row) || !row->detail.target_app_ver[0]) continue;
            if (!row->summary.installer.accepted || row->detail.manual || row->detail.bgft_failed) break;
            row->detail.closed_utc = utc;
            saved = store_row(&row->summary, &row->detail);
            break;
        }
    }
    unlock_journal();
    return saved;
}

bool x4_update_log51_diagnostic(char *pending, size_t capacity, int32_t *code)
{
    if (!pending || !capacity || !code) return false;
    pending[0] = '\0';
    *code = 0;
    JournalRow rows[LOG51_ENTRIES];
    size_t count;
    bool found = false;
    lock_journal();
    if (load_rows51(rows, &count)) {
        while (count) {
            const JournalRow *row = &rows[--count];
            if (!same_build(row) || !row->detail.diagnostic) continue;
            const size_t length = strlen(row->detail.pending_call);
            if (length < capacity) {
                memcpy(pending, row->detail.pending_call, length + 1);
                *code = row->summary.code;
                found = true;
            }
            break;
        }
    }
    unlock_journal();
    return found;
}
#endif
