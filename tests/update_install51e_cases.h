/* SPDX-License-Identifier: GPL-3.0-only */
/* Real installer with simulated native calls, included by the PS4 I/O test. */

static const char marker_path[] = "/data/xcloud4/update/xcloud4-update.appver";
static const char crash_call[] = "Register";
static const char target_version[] = "01.68";
enum { CRASH_EXIT = 79 };

static unsigned expected_run = 1;
static unsigned native_calls, native_mask, prior_writes, prior_closes, progress_calls;
static uint64_t install_clock_us;
static bool crash_register;

static X4JsonSpan last_journal(char *body, size_t capacity)
{
    char path[PATH_MAX];
    FILE *file = fopen(mapped(journal_path, path), "rb");
    assert(file);
    const size_t size = __real_fread(body, 1, capacity, file);
    assert(size && size < capacity && !ferror(file) && !__real_fclose(file));
    X4JsonSpan root, item, extra;
    assert(!x4_json_parse(body, size, &root));
    const X4JsonSpan rows = required_member(root, "runs");
    size_t cursor = 0;
    for (unsigned run = 1; run <= expected_run; ++run) {
        assert(x4_json_item(rows, &cursor, &item) == 1);
        assert(number_member(item, "run") == run);
    }
    assert(x4_json_item(rows, &cursor, &extra) == 0);
    return item;
}

static void check_string(X4JsonSpan item, const char *key, const char *expected)
{
    char text[160];
    assert(!x4_json_token(required_member(item, key), text, sizeof(text)));
    assert(!strcmp(text, expected));
}

static bool path_exists(const char *path)
{
    struct stat info;
    return !__wrap_stat(path, &info);
}

static void check_native_breadcrumb(const char *call, const char *previous, int previous_rc)
{
    assert(journal_fd == -1 && journal_writes > prior_writes && journal_closes > prior_closes);
    prior_writes = journal_writes;
    prior_closes = journal_closes;

    char body[16384];
    const X4JsonSpan row = last_journal(body, sizeof(body));
    check_string(row, "stage_name",
        native_mask & X4_INSTALL51_START ? "instalacion_aceptada" : "instalacion");
    check_string(row, "target_app_ver", target_version);
    check_string(row, "pending_call", call);
    assert(number_member(row, "calls") == native_mask);
    if (previous) {
        check_string(row, "last_call", previous);
        assert(number_member(row, "last_call_rc") == previous_rc);
    }
}

#if X4_TEST_INSTALL51F
#include "update_install51f_cases.h"
#else

static const struct {
    const char *name;
    unsigned flag;
    int code;
} native_order[] = {
    {"AppInstUtilInit", X4_INSTALL51_APP_INIT, 0},
    {"GetTitleId", X4_INSTALL51_TITLE, 0},
    {"BGFTInit", X4_INSTALL51_BGFT_INIT, 0},
    {"Register", X4_INSTALL51_REGISTER, 0},
    {"Start", X4_INSTALL51_START, 0},
    {"Progress", X4_INSTALL51_PROGRESS, -EAGAIN},
    {"Progress", X4_INSTALL51_PROGRESS, 0},
    {"BGFTTerm", X4_INSTALL51_BGFT_TERM, 0},
    {"AppInstUtilTerm", X4_INSTALL51_APP_TERM, 0},
};

static void before_native(const char *call)
{
    assert(native_calls < sizeof(native_order) / sizeof(native_order[0]));
    assert(!strcmp(native_order[native_calls].name, call));
    check_native_breadcrumb(call, native_calls ? native_order[native_calls - 1].name : NULL,
        native_calls ? native_order[native_calls - 1].code : 0);
    native_mask |= native_order[native_calls++].flag;
}

int32_t sceAppInstUtilInitialize(void)
{
    before_native("AppInstUtilInit");
    return 0;
}

int32_t sceAppInstUtilGetTitleIdFromPkg(const char *path, char *title, int32_t *is_app)
{
    before_native("GetTitleId");
    verify_staged_fixture(path);
    memcpy(title, "XCLD00001", sizeof("XCLD00001"));
    *is_app = 1;
    return 0;
}

int32_t sceBgftServiceInit(OrbisBgftInitParams *params)
{
    before_native("BGFTInit");
    pthread_attr_t attr;
    assert(!pthread_getattr_np(pthread_self(), &attr));
    void *stack;
    size_t stack_bytes;
    assert(!pthread_attr_getstack(&attr, &stack, &stack_bytes) && !pthread_attr_destroy(&attr));
    const uintptr_t heap = (uintptr_t)params->heap;
    const uintptr_t stack_base = (uintptr_t)stack;
    assert(heap && heap % 16 == 0 && params->heapSize == 1024u * 1024u);
    assert(stack_bytes >= 512u * 1024u &&
        (heap >= stack_base + stack_bytes || heap + params->heapSize <= stack_base));
    const unsigned char *data = params->heap;
    assert(!data[0] && !data[params->heapSize - 1]);
    return 0;
}

int32_t sceBgftServiceIntDownloadRegisterTaskByStorageEx(OrbisBgftDownloadParamEx *params,
    OrbisBgftTaskId *task)
{
    before_native("Register");
    assert(params->params.entitlementType == 5 && params->params.option == ORBIS_BGFT_TASK_OPT_NONE);
    assert(!strcmp(params->params.id, "") && !strcmp(params->params.contentUrl, X4_INSTALL51_PKG_PATH));
    assert(!strcmp(params->params.contentName, "XCloud4 Lab"));
    assert(!strcmp(params->params.iconPath, "") && !strcmp(params->params.playgoScenarioId, "0"));
    assert(params->params.packageSize == package_size && params->slot == 0);
    if (crash_register) _Exit(CRASH_EXIT);
    *task = 51;
    return 0;
}

int32_t sceBgftServiceDownloadStartTask(OrbisBgftTaskId task)
{
    before_native("Start");
    assert(task == 51);
    atomic_fetch_add(&install_calls, 1);
    return 0;
}

int32_t mock_progress51e(OrbisBgftTaskId task, void *out)
    __asm__("sceBgftServiceDownloadGetProgress");
int32_t mock_progress51e(OrbisBgftTaskId task, void *out)
{
    before_native("Progress");
    assert(task == 51 && out);
    if (++progress_calls == 1) return -EAGAIN;
    memcpy((unsigned char *)out + 24, &package_size, sizeof(package_size));
    memcpy((unsigned char *)out + 32, &package_size, sizeof(package_size));
    return 0;
}

int32_t sceBgftServiceTerm(void)
{
    before_native("BGFTTerm");
    return 0;
}

int32_t sceAppInstUtilTerminate(void)
{
    before_native("AppInstUtilTerm");
    return 0;
}

int32_t sceAppInstUtilAppInstallPkg(const char *path, void *reserved)
{
    (void)path;
    (void)reserved;
    assert(!"a successful BGFT registration must not fall back");
    return -EIO;
}

uint64_t sceKernelGetProcessTime(void)
{
    return install_clock_us += UINT64_C(1000000);
}

static void completed_install(void)
{
    const X4Update51View view = run_update();
    assert(view.stage == X4_UPDATE51_ACCEPTED && !view.code && atomic_load(&install_calls) == 1);
    assert(native_calls == sizeof(native_order) / sizeof(native_order[0]) && progress_calls == 2);
    assert(query_count == 4);
    for (unsigned i = 0; i < query_count; ++i) assert(queries[i] == (SpaceQuery)i);
    assert(atomic_load(&verified_manifests) == 1 && atomic_load(&false_stats) >= 2);
    assert(atomic_load(&short_reads) > 1 && atomic_load(&short_writes) > 1 && !atomic_load(&tail_bytes));
    assert(path_exists(marker_path) && path_exists(X4_INSTALL51_PKG_PATH));

    char body[16384];
    const X4JsonSpan row = last_journal(body, sizeof(body));
    X4JsonSpan field;
    assert(x4_json_member(row, "pending_call", &field) == 0);
    check_string(row, "last_call", "AppInstUtilTerm");
    assert(!number_member(row, "last_call_rc") && number_member(row, "calls") == native_mask);
    int unknown;
    assert(!x4_json_bool(required_member(row, "espacio_desconocido"), &unknown) && unknown);
    const X4JsonSpan methods = required_member(row, "space_methods");
    check_call(methods, "fstatvfs", -1, EPERM);
    check_call(methods, "_fstatfs", -1, EPERM);
    check_call(methods, "statvfs", 0, EPERM);
    check_call(methods, "statfs", 13, EPERM);
    printf("T51E_INSTALL_PASS run=%u native_calls=%u pending_order=exact raw_journal_closed=1 "
        "stack_request_min_kib=512 bgft_heap_bytes=1048576 progress_first_rc=%d "
        "short_reads=%u short_writes=%u space_eperm=4 part_clean=1\n",
        expected_run, native_calls, -EAGAIN, atomic_load(&short_reads), atomic_load(&short_writes));
}

static void check_crash_journal(void)
{
    char body[16384];
    const X4JsonSpan row = last_journal(body, sizeof(body));
    check_string(row, "pending_call", crash_call);
    check_string(row, "last_call", "BGFTInit");
    check_string(row, "target_app_ver", target_version);
    assert(!number_member(row, "last_call_rc") &&
        number_member(row, "calls") == (X4_INSTALL51_APP_INIT | X4_INSTALL51_TITLE | X4_INSTALL51_BGFT_INIT));
    assert(path_exists(marker_path) && path_exists(X4_INSTALL51_PKG_PATH) && !path_exists(part_path));
}

static void simulate_applied_version(void)
{
    char path[PATH_MAX];
    const char marker[] = "XCLD00001 " X4_APP_VERSION;
    const int fd = __real_open(mapped(marker_path, path), O_WRONLY | O_TRUNC | O_NOFOLLOW);
    assert(fd >= 0);
    assert(__real_write(fd, marker, sizeof(marker)) == (ssize_t)sizeof(marker) && !__real_close(fd));
}

static void recover_install(const char *action)
{
    check_crash_journal();
    if (!strcmp(action, "boot-stale")) simulate_applied_version();
    x4_update51_boot_cleanup();
    if (!strcmp(action, "boot-stale")) {
        assert(!x4_update51_pending() && !x4_update51_retry_available(NULL));
        assert(!path_exists(marker_path) && !path_exists(X4_INSTALL51_PKG_PATH) && !native_calls);
        puts("T51E_BOOT_STALE_PASS applied_version_cleanup=1");
        return;
    }

    bool local = false;
    char call[48];
    assert(x4_update_log51_interrupted(call, sizeof(call), &local) && local && !strcmp(call, crash_call));
    local = false;
    assert(x4_update51_pending() && x4_update51_retry_available(&local) && local);
    if (!strcmp(action, "boot-cancel")) {
        assert(x4_update51_cancel_pending());
        assert(!x4_update51_pending() && !x4_update51_retry_available(NULL));
        assert(!path_exists(marker_path) && !path_exists(X4_INSTALL51_PKG_PATH) && !native_calls);
        puts("T51E_BOOT_CANCEL_PASS pending_call=Register retry_offered=1 staging_removed=1");
        return;
    }
    assert(!strcmp(action, "boot-retry"));
    expected_run = 2;
    completed_install();
    assert(!x4_update51_retry_available(NULL));
    assert(!x4_update_log51_interrupted(call, sizeof(call), &local));
    puts("T51E_BOOT_RETRY_PASS pending_call=Register retry_offered=1 verified_again=1");
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    (void)umask(0022);
    if (!strncmp(argv[4], "boot-", 5)) {
        configure_fixture(argv[1], argv[2], argv[3]);
        recover_install(argv[4]);
        return 0;
    }
    prepare_fixture(argv[1], argv[2], argv[3]);
    crash_register = !strcmp(argv[4], "crash-register");
    assert(crash_register || !strcmp(argv[4], "complete"));
    completed_install();
    assert(!crash_register);
    return 0;
}
#endif
