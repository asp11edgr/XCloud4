/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the PS4 I/O test through the 51e journal helpers. */

static const char auth_path[] = "/data/xcloud4/update/xcloud4-update.auth";
static const char target_product_version[] = "0.7.100-lab";
static const uint64_t mutation_offset = 16384;
static char manual_path[160];
static bool fail_register;
static time_t clock_offset;

time_t __wrap_time(time_t *out)
{
    const time_t value = 1700000000 + clock_offset;
    if (out) *out = value;
    return value;
}

typedef struct {
    const char *name;
    unsigned flag;
    int rc;
} NativeCall51f;

static const NativeCall51f bgft_success[] = {
    {"BGFTInit", X4_INSTALL51_BGFT_INIT, 0},
    {"Register", X4_INSTALL51_REGISTER, 0},
    {"Start", X4_INSTALL51_START, 0},
    {"Progress", X4_INSTALL51_PROGRESS, -EAGAIN},
    {"Progress", X4_INSTALL51_PROGRESS, 0},
    {"BGFTTerm", X4_INSTALL51_BGFT_TERM, 0},
};

static const NativeCall51f bgft_failure[] = {
    {"BGFTInit", X4_INSTALL51_BGFT_INIT, 0},
    {"Register", X4_INSTALL51_REGISTER, -EIO},
    {"BGFTTerm", X4_INSTALL51_BGFT_TERM, 0},
};

static void before_native(const char *call)
{
    const NativeCall51f *order = fail_register ? bgft_failure : bgft_success;
    const size_t length = fail_register ? sizeof(bgft_failure) / sizeof(*bgft_failure) :
        sizeof(bgft_success) / sizeof(*bgft_success);
    assert(native_calls < length && !strcmp(order[native_calls].name, call));
    const NativeCall51f *previous = native_calls ? &order[native_calls - 1] : NULL;
    check_native_breadcrumb(call, previous ? previous->name : NULL, previous ? previous->rc : 0);
    native_mask |= order[native_calls++].flag;
}

int32_t sceAppInstUtilInitialize(void)
{
    assert(!"the updater must not initialize AppInstUtil");
    return -EIO;
}

int32_t sceAppInstUtilTerminate(void)
{
    assert(!"the updater must not terminate AppInstUtil");
    return -EIO;
}

int32_t sceAppInstUtilGetTitleIdFromPkg(const char *path, char *title, int32_t *is_app)
{
    (void)path;
    (void)title;
    (void)is_app;
    assert(!"the updater must use the verified title ID");
    return -EIO;
}

int32_t sceAppInstUtilAppInstallPkg(const char *path, void *reserved)
{
    (void)path;
    (void)reserved;
    assert(!"the updater must fall back to manual installation");
    return -EIO;
}

int32_t sceBgftServiceInit(OrbisBgftInitParams *params)
{
    before_native("BGFTInit");
    assert(params->heap && (uintptr_t)params->heap % 16 == 0 && params->heapSize == 1024u * 1024u);
    return 0;
}

int32_t sceBgftServiceIntDownloadRegisterTaskByStorageEx(OrbisBgftDownloadParamEx *params,
    OrbisBgftTaskId *task)
{
    before_native("Register");
    assert(params->params.entitlementType == 5 && params->params.option == ORBIS_BGFT_TASK_OPT_NONE);
    assert(!strcmp(params->params.id, "XCLD00001"));
    assert(!strcmp(params->params.contentUrl, X4_INSTALL51_PKG_PATH));
    assert(params->params.packageSize == package_size && params->slot == 0);
    verify_staged_fixture(params->params.contentUrl);
    if (crash_register) _Exit(CRASH_EXIT);
    *task = 51;
    return fail_register ? -EIO : 0;
}

int32_t sceBgftServiceDownloadStartTask(OrbisBgftTaskId task)
{
    before_native("Start");
    assert(task == 51);
    atomic_fetch_add(&install_calls, 1);
    return 0;
}

int32_t mock_progress51f(OrbisBgftTaskId task, void *out)
    __asm__("sceBgftServiceDownloadGetProgress");
int32_t mock_progress51f(OrbisBgftTaskId task, void *out)
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

uint64_t sceKernelGetProcessTime(void)
{
    return install_clock_us += UINT64_C(1000000);
}

static void flip_package_byte(const char *path)
{
    char real_path[PATH_MAX];
    const int fd = __real_open(mapped(path, real_path), O_RDWR | O_NOFOLLOW);
    assert(fd >= 0);
    unsigned char byte;
    const off_t offset = (off_t)mutation_offset;
    assert(pread(fd, &byte, 1, offset) == 1);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, offset) == 1 && !__real_close(fd));
}

static void check_manual(X4Update51View view, int code, bool bgft_failed)
{
    assert(view.stage == X4_UPDATE51_MANUAL && !view.busy);
    X4Update51ManualView manual;
    assert(x4_update51_manual_view(&manual) && manual.code == code);
    assert(!strcmp(manual.version, target_product_version) && !strcmp(manual.pkg_name, source_leaf));
    char body[16384];
    const X4JsonSpan row = last_journal(body, sizeof(body));
    check_string(row, "modo", "manual");
    check_string(row, "ruta", manual_path);
    X4JsonSpan field;
    assert(x4_json_member(row, "pending_call", &field) == 0);
    if (bgft_failed) {
        int failed;
        assert(!x4_json_bool(required_member(row, "bgft_failed"), &failed) && failed);
        assert(number_member(row, "register") == -EIO);
    }
}

static X4Update51View choose_install(bool manual_default, bool manual)
{
    allow_remote = true;
    assert(x4_update51_start_checked(remote_tag, false, true, NULL));
    X4Update51View view;
    unsigned polls = 0;
    while ((view = x4_update51_view()).stage != X4_UPDATE51_CHOOSE) {
        assert(view.busy && polls++ < 30000);
        (void)sceKernelUsleep(1000);
    }
    assert(!native_calls && view.manual_selected == manual_default);
    x4_update51_select_install(manual);
    assert(x4_update51_view().manual_selected == manual);
    x4_update51_confirm_install();
    return wait_for_update();
}

static void check_remote(void)
{
    assert(remote_requests == 3 && atomic_load(&verified_manifests) >= 1);
    assert(atomic_load(&false_stats) >= 1 && atomic_load(&short_reads) > 1);
    assert(atomic_load(&short_writes) > 1 && !atomic_load(&tail_bytes));
    assert(query_count == 4);
    for (size_t i = 0; i < query_count; ++i) assert(queries[i] == (SpaceQuery)i);
    char body[16384];
    const X4JsonSpan row = last_journal(body, sizeof(body));
    const X4JsonSpan methods = required_member(row, "space_methods");
    check_call(methods, "fstatvfs", -1, EPERM);
    check_call(methods, "_fstatfs", -1, EPERM);
    check_call(methods, "statvfs", 0, EPERM);
    check_call(methods, "statfs", 13, EPERM);
}

static void run_automatic(void)
{
    const X4Update51View view = choose_install(false, false);
    assert(!crash_register);
    check_remote();
    assert(view.stage == X4_UPDATE51_ACCEPTED && !view.code && atomic_load(&install_calls) == 1);
    assert(native_calls == sizeof(bgft_success) / sizeof(*bgft_success) && progress_calls == 2);
    assert(!path_exists(manual_path));
    char body[16384];
    const X4JsonSpan row = last_journal(body, sizeof(body));
    check_string(row, "last_call", "BGFTTerm");
    assert(!number_member(row, "last_call_rc") && number_member(row, "calls") == native_mask);
    x4_update51_exit();
    assert(number_member(last_journal(body, sizeof(body)), "closed_utc") == 1700000000);
    puts("T51F_BGFT_PASS title=verified native_calls=6 AppInstUtil=0 raw_journal_closed=1");
}

static void run_manual(bool reuse, bool conflict)
{
    struct stat original = {0};
    if (reuse || conflict) {
        copy_local(source_pkg, source_leaf);
        if (conflict) flip_package_byte(manual_path);
        assert(!__wrap_stat(manual_path, &original));
    }
    fail_register = !reuse;
    if (reuse) expected_run = 2; /* History of the previous failed attempt is present. */
    const X4Update51View view = choose_install(reuse, reuse);
    check_remote();
    assert(!atomic_load(&install_calls));
    assert(native_calls == (reuse ? 0 : sizeof(bgft_failure) / sizeof(*bgft_failure)));
    check_manual(view, conflict ? -EEXIST : 0, !reuse);
    assert((manual_short_writes > 1) == (!reuse && !conflict));
    struct stat published;
    assert(!__wrap_stat(manual_path, &published) && S_ISREG(published.st_mode));
    if (reuse || conflict) {
        assert(published.st_dev == original.st_dev && published.st_ino == original.st_ino);
        assert(published.st_mtim.tv_sec == original.st_mtim.tv_sec &&
            published.st_mtim.tv_nsec == original.st_mtim.tv_nsec);
    }
    verify_fixture_bytes(manual_path, conflict ? mutation_offset : UINT64_MAX);
    printf("T51F_MANUAL_PASS case=%s manual_default=%d bytes_verified=1 existing_unchanged=%d code=%d\n",
        conflict ? "different-sha" : reuse ? "same-sha" : "bgft-error",
        reuse, reuse || conflict, conflict ? -EEXIST : 0);
}

static void recover_manual(bool tamper)
{
    assert(path_exists(marker_path) && path_exists(auth_path));
    assert(path_exists(X4_INSTALL51_PKG_PATH) && !path_exists(manual_path) && !path_exists(part_path));
    char body[16384];
    const X4JsonSpan crashed = last_journal(body, sizeof(body));
    check_string(crashed, "pending_call", crash_call);
    check_string(crashed, "last_call", "BGFTInit");
    assert(number_member(crashed, "calls") == X4_INSTALL51_BGFT_INIT);
    if (tamper) flip_package_byte(X4_INSTALL51_PKG_PATH);

    x4_update51_boot_cleanup();
    const X4Update51View view = wait_for_update();
    assert(!allow_remote && !remote_requests && !native_calls);
    assert(atomic_load(&verified_manifests) == 1);
    if (tamper) {
        X4Update51ManualView manual;
        assert(view.stage == X4_UPDATE51_FAILED && view.error == X4_UPDATE51_BAD_HASH);
        assert(!path_exists(manual_path) && !x4_update51_manual_view(&manual));
        assert(x4_update51_take_manual_offer() && !x4_update51_take_manual_offer());
        puts("T51F_BOOT_TAMPER_PASS signed_manifest_revalidated=1 different_sha_rejected=1");
        return;
    }
    expected_run = 2;
    check_manual(view, 0, false);
    assert(x4_update51_take_manual_offer() && !x4_update51_take_manual_offer());
    assert(manual_short_writes > 1);
    verify_fixture_bytes(manual_path, UINT64_MAX);
    puts("T51F_BOOT_MANUAL_PASS pending_call=Register signature_and_sha=verified network=0 AppInstUtil=0");
}

static void recover_accepted(bool recent)
{
    assert(path_exists(marker_path) && path_exists(auth_path));
    expected_run = 2;
    clock_offset = recent ? 899 : 900;
    x4_update51_boot_cleanup();
    if (recent) {
        X4Update51View view;
        unsigned polls = 0;
        while ((view = x4_update51_view()).stage != X4_UPDATE51_CHOOSE) {
            assert(view.busy && polls++ < 30000);
            (void)sceKernelUsleep(1000);
        }
        assert(view.resuming && x4_update51_take_manual_offer() && !x4_update51_take_manual_offer());
        x4_update51_cancel();
        assert(wait_for_update().stage == X4_UPDATE51_CANCELLED);
        assert(!path_exists(marker_path) && !path_exists(auth_path) && !path_exists(manual_path));
    } else {
        const X4Update51View view = wait_for_update();
        check_manual(view, 0, false);
        assert(x4_update51_take_manual_offer() && !x4_update51_take_manual_offer());
        verify_fixture_bytes(manual_path, UINT64_MAX);
    }
    assert(!allow_remote && !remote_requests && !native_calls && atomic_load(&verified_manifests) == 1);
    printf("T51F_BOOT_ACCEPTED_PASS case=%s\n", recent ? "recent" : "old");
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    (void)umask(0022);
    configure_fixture(argv[1], argv[2], argv[3]);
    const int length = snprintf(manual_path, sizeof(manual_path), "/data/pkg/%s", source_leaf);
    assert(length > 0 && (size_t)length < sizeof(manual_path));
    const char *action = argv[4];
    if (!strcmp(action, "boot-accepted-recent") || !strcmp(action, "boot-accepted-old")) {
        recover_accepted(!strcmp(action, "boot-accepted-recent"));
        return 0;
    }
    if (!strcmp(action, "boot-manual") || !strcmp(action, "boot-tamper")) {
        recover_manual(!strcmp(action, "boot-tamper"));
        return 0;
    }
    char directory[PATH_MAX];
    assert(!__real_mkdir(mapped("/data/pkg", directory), 0700));
    crash_register = !strcmp(action, "crash-register");
    if (!strcmp(action, "complete") || crash_register) run_automatic();
    else {
        assert(!strcmp(action, "same") || !strcmp(action, "conflict") || !strcmp(action, "bgft-error"));
        run_manual(!strcmp(action, "same"), !strcmp(action, "conflict"));
    }
    return 0;
}
