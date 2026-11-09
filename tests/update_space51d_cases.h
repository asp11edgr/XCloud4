/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by update_ps4_io51c_test.c for the T51d space and permissions run. */

static void record_query(SpaceQuery query)
{
    assert(query_count < sizeof(queries) / sizeof(queries[0]));
    queries[query_count++] = query;
}

static int eperm_query(SpaceQuery query)
{
    record_query(query);
    errno = EPERM;
    return -1;
}

static void check_space_descriptor(int fd)
{
    struct stat info;
    assert(fd >= 0 && fd == space_directory_fd &&
        !__real_fstat(fd, &info) && S_ISDIR(info.st_mode));
}

int __wrap_fstatvfs(int fd, struct statvfs *result)
{
    assert(result);
    check_space_descriptor(fd);
    return eperm_query(QUERY_FSTATVFS);
}

int _fstatfs(int fd, void *result)
{
    assert(result);
    check_space_descriptor(fd);
    return eperm_query(QUERY_FSTATFS);
}

int __wrap_statvfs(const char *path, struct statvfs *result)
{
    assert(!strcmp(path, "/data") && result);
#if X4_TEST_INSTALL51E
    (void)eperm_query(QUERY_STATVFS);
    return 0; /* The SDK wrapper left EPERM despite its success return. */
#else
    if (space_case != SPACE_LOW) return eperm_query(QUERY_STATVFS);
    record_query(QUERY_STATVFS);
    memset(result, 0, sizeof(*result));
    result->f_frsize = 4096;
    result->f_bavail = 1;
    return 0;
#endif
}

int __wrap_statfs(const char *path, struct statfs *result)
{
    assert(!strcmp(path, "/data") && result);
#if X4_TEST_INSTALL51E
    (void)eperm_query(QUERY_STATFS);
    return 13; /* The SDK wrapper returns the descriptor and keeps errno. */
#else
    return eperm_query(QUERY_STATFS);
#endif
}

int _statfs(const char *path, void *result)
{
    assert(!strcmp(path, "/data") && result);
    return eperm_query(QUERY_STATFS);
}

static X4JsonSpan required_member(X4JsonSpan object, const char *name)
{
    X4JsonSpan value;
    assert(x4_json_member(object, name, &value) == 1);
    return value;
}

static long number_member(X4JsonSpan object, const char *name)
{
    const X4JsonSpan value = required_member(object, name);
    char text[24];
    assert(x4_json_type(value) == X4_JSON_T_NUMBER && value.length && value.length < sizeof(text));
    memcpy(text, value.data, value.length);
    text[value.length] = '\0';
    char *end;
    const long n = strtol(text, &end, 10);
    assert(*end == '\0');
    return n;
}

static void check_call(X4JsonSpan parent, const char *name, int code, int error)
{
    const X4JsonSpan value = required_member(parent, name);
    assert(number_member(value, "code") == code);
    assert(number_member(value, "errno") == error);
}

#if X4_TEST_INSTALL51E
#include "update_install51e_cases.h"
#else
static void check_journal(unsigned attempt, int code)
{
    const char journal[] = "/data/xcloud4/update-log.json";
    char path[PATH_MAX], body[16384];
    FILE *file = fopen(mapped(journal, path), "rb");
    assert(file);
    const size_t count = __real_fread(body, 1, sizeof(body), file);
    assert(count && count < sizeof(body) && !ferror(file) && !__real_fclose(file));
    X4JsonSpan root, item, extra;
    assert(!x4_json_parse(body, count, &root));
    const X4JsonSpan runs = required_member(root, "runs");
    size_t cursor = 0;
    for (unsigned i = 1; i <= attempt; ++i) {
        assert(x4_json_item(runs, &cursor, &item) == 1);
        assert(number_member(item, "run") == i);
    }
    assert(x4_json_item(runs, &cursor, &extra) == 0);
    assert(number_member(item, "code") == code);
    char build[32];
    assert(!x4_json_token(required_member(item, "build"), build, sizeof(build)));
    assert(!strcmp(build, X4_PRODUCT_VERSION));
    const int unknown = space_case != SPACE_LOW;
    assert(number_member(item, "space_code") == (unknown ? -EPERM : 0));
    assert(number_member(item, "space_errno") == (unknown ? EPERM : 0));
    if (code) {
        char stage[64], function[80];
        assert(!x4_json_token(required_member(item, "stage_name"), stage, sizeof(stage)));
        assert(!strcmp(stage, "copia_paquete"));
        assert(!x4_json_token(required_member(item, "function"), function, sizeof(function)));
        assert(X4_TEST_BASE51C || space_case == SPACE_LOW ?
            !strcmp(function, "x4_install51_free_space") : !strncmp(function, "fwrite", 6));
        assert(number_member(item, "errno") == -code);
    }
    if (X4_TEST_BASE51C) return;
    int observed;
    assert(!x4_json_bool(required_member(item, "espacio_desconocido"), &observed));
    assert(observed == unknown);
    const X4JsonSpan methods = required_member(item, "space_methods");
    check_call(methods, "open", 0, 0);
    check_call(methods, "fstatvfs", -1, EPERM);
    check_call(methods, "_fstatfs", -1, EPERM);
    check_call(methods, "statvfs", unknown ? -1 : 0, unknown ? EPERM : 0);
    if (unknown) check_call(methods, "statfs", -1, EPERM);
    else assert(x4_json_member(methods, "statfs", &extra) == 0);
    check_call(methods, "close", 0, 0);
    assert(dir_chmods == 1);
    check_call(item, "chmod_update", space_case == SPACE_ENOSPC ? -1 : 0,
        space_case == SPACE_ENOSPC ? EPERM : 0);
}

static void reset_case(SpaceCase next)
{
    space_case = next;
    query_count = 0;
    part_opens = dir_chmods = update_mkdir_calls = 0;
    part_create_mode = update_create_mode = 0;
    space_directory_fd = -1;
    atomic_store(&verified_manifests, 0);
    atomic_store(&false_stats, 0);
    atomic_store(&short_reads, 0);
    atomic_store(&short_writes, 0);
    atomic_store(&nonblock_opens, 0);
    atomic_store(&measured_ends, 0);
    atomic_store(&rewinds, 0);
    atomic_store(&tail_bytes, 0);
    atomic_store(&install_calls, 0);
}

static void run_space_case(SpaceCase test, unsigned attempt)
{
    static const char *const cases[] = {"unknown", "enospc", "no-progress", "low"};
    reset_case(test);
    char dir[PATH_MAX];
    if (!X4_TEST_BASE51C && test != SPACE_LOW)
        assert(!__real_chmod(mapped("/data/xcloud4/update", dir), 0700));
    const X4Update51View view = run_update();
    assert(atomic_load(&verified_manifests) == 1);
    assert(!atomic_load(&nonblock_opens));
    if (X4_TEST_BASE51C) {
        assert(query_count == 1 && queries[0] == QUERY_FSTATFS);
        assert(!part_opens && !atomic_load(&install_calls) && !atomic_load(&false_stats));
        assert(view.stage == X4_UPDATE51_FAILED && view.error == X4_UPDATE51_NO_SPACE && view.code == -EPERM);
        check_journal(attempt, -EPERM);
        puts("T51D_PS4_SPACE_BASELINE_PASS code=-1 errno=1 method=_fstatfs part_created=0");
        return;
    }
    const size_t probes = test == SPACE_LOW ? 3u : 4u;
    assert(query_count == probes);
    for (size_t i = 0; i < probes; ++i) assert(queries[i] == (SpaceQuery)i);
    assert(update_mkdir_calls == 1 && update_create_mode == 0777);
    assert(part_opens == (test == SPACE_LOW ? 0u : 1u));
    if (part_opens) assert(part_create_mode == 0666);
    struct stat info;
    assert(!__wrap_stat("/data/xcloud4/update", &info));
    assert((info.st_mode & 0777) == (test == SPACE_ENOSPC ? 0700 : 0777));
    const int code = test == SPACE_UNKNOWN ? 0 : -ENOSPC;
    assert(view.code == code);
    if (test == SPACE_UNKNOWN) {
        assert(view.stage == X4_UPDATE51_ACCEPTED && atomic_load(&install_calls) == 1);
        assert(atomic_load(&false_stats) >= 2 && atomic_load(&measured_ends) >= 3 &&
            atomic_load(&rewinds) >= 2 && !atomic_load(&tail_bytes));
        assert(atomic_load(&short_reads) > 1 && atomic_load(&short_writes) > 1);
    } else {
        assert(view.stage == X4_UPDATE51_FAILED && view.error == X4_UPDATE51_NO_SPACE);
        assert(!atomic_load(&install_calls));
        assert(__wrap_stat(X4_INSTALL51_PKG_PATH, &info) == -1 && errno == ENOENT);
    }
    check_journal(attempt, code);
    printf("T51D_PS4_SPACE_PASS case=%s probes=%zu code=%d short_reads=%u short_writes=%u "
           "part_clean=1 journal=1 chmod_code=%d signed_manifest=1 installed_stub=%u\n",
        cases[test], probes, code, atomic_load(&short_reads), atomic_load(&short_writes),
        test == SPACE_ENOSPC ? -1 : 0, atomic_load(&install_calls));
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    (void)umask(0022);
    prepare_fixture(argv[1], argv[2], argv[3]);
    if (X4_TEST_BASE51C) {
        run_space_case(SPACE_UNKNOWN, 1);
    } else {
        run_space_case(SPACE_LOW, 1);
        run_space_case(SPACE_ENOSPC, 2);
        run_space_case(SPACE_NO_PROGRESS, 3);
        run_space_case(SPACE_UNKNOWN, 4);
    }
    return 0;
}
#endif
