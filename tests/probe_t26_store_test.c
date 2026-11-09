/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#error "This test requires active assertions"
#endif
#include "video/probe_t26_store.h"
#include "auth/json.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char build[] = "ca0b23e377fa-0.7.62-probe.t26";
static const char manifest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static unsigned tests;

static void root_path(char out[320], const char *base, const char *name)
{
    int n = snprintf(out, 320, "%s/%s", base, name);
    assert(n > 0 && n < 320);
}
static size_t load(const char *path, char *text, size_t capacity)
{
    FILE *f = fopen(path, "rb"); assert(f);
    size_t size = fread(text, 1, capacity - 1u, f);
    assert(!ferror(f) && fgetc(f) == EOF);
    text[size] = 0; assert(fclose(f) == 0); return size;
}
static void create_fixture(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644); assert(fd >= 0);
    size_t size = strlen(text);
    assert(write(fd, text, size) == (ssize_t)size);
    assert(fsync(fd) == 0 && close(fd) == 0);
}
static unsigned temporaries(const char *root)
{
    DIR *dir = opendir(root); assert(dir); unsigned n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t size = strlen(entry->d_name);
        if (size > 4 && !strcmp(entry->d_name + size - 4, ".tmp")) ++n;
    }
    assert(closedir(dir) == 0); return n;
}
static void fresh(const char *root, X4T26Store *store, X4T26State *state)
{
    bool resumed = true;
    assert(x4_t26_store_open_at(store, root, build, manifest, 1000, state, &resumed) == 0);
    assert(!resumed && x4_t26_state_valid(state));
    assert(state->generation == 1 && state->next_cell == 0 && !state->active);
}
static void basic_and_cuts(const char *base)
{
    char root[320], path[X4_T26_STORE_PATH_MAX]; root_path(root, base, "basic-and-cuts");
    X4T26Store st, again; X4T26State state, restored; bool resumed;
    fresh(root, &st, &state);
    assert(!strcmp(st.session, "ca0b23e377fa-1000"));
    X4T26State invalid = state;
    memset(invalid.build_id, 'X', sizeof(invalid.build_id));
    assert(x4_t26_store_write_progress(&st, &invalid) < 0 && errno == EINVAL);
    X4T26Store invalid_store = st;
    memset(invalid_store.build, 'X', sizeof(invalid_store.build));
    assert(x4_t26_store_cell_path(&invalid_store, 0, path, sizeof(path)) < 0 && errno == EINVAL);
    assert(x4_t26_store_cell_read(&st, 0, path, sizeof(path), &(size_t){0}) == 1);
    assert(x4_t26_store_open_at(&again, root, build, manifest, 2000, &restored, &resumed) == 0 && resumed);
    assert(!strcmp(st.session, again.session) && restored.started_us == state.started_us);
    assert(x4_t26_state_begin(&state, 0, X4_T26_MODE_MAXIMUM, X4_T26_PHASE_QUERY_COMPUTE) == 0);
    assert(x4_t26_store_write_progress(&st, &state) == 0);
    FILE *file = x4_t26_store_cell_begin(&st, 0); assert(file);
    assert(fputs("{\"accepted\":true,\"fixture\":\"HOST ONLY\"}", file) >= 0);
    assert(x4_t26_store_cell_finish(&st, file) == 0);
    char *text = malloc(X4_T26_STORE_CELL_MAX + 1u); assert(text); size_t size;
    assert(x4_t26_store_cell_read(&st, 0, text, X4_T26_STORE_CELL_MAX + 1u, &size) == 0 && size);
    X4JsonSpan document, result, accepted; int yes;
    assert(!x4_json_parse(text, size, &document));
    assert(x4_json_member(document, "result", &result) == 1);
    assert(x4_json_member(result, "accepted", &accepted) == 1 && !x4_json_bool(accepted, &yes) && yes);
    assert(!x4_t26_store_cell_begin(&st, 0) && errno == EEXIST);
    assert(x4_t26_store_cell_path(&st, 0, path, sizeof(path)) == 0);
    size_t committed_size = load(path, text, X4_T26_STORE_CELL_MAX + 1u);
    /* Simulated process loss after the durable result but before progress.
     * Reopening leaves core reconciliation to the caller. */
    assert(x4_t26_store_open_at(&again, root, build, manifest, 3000, &restored, &resumed) == 0 && resumed && restored.active);
    assert(x4_t26_store_cell_read(&again, 0, text, X4_T26_STORE_CELL_MAX + 1u, &size) == 0);
    assert(size == committed_size);
    assert(x4_t26_state_resume(&restored, UINT64_C(1), false) == X4_T26_RESUME_COMMITTED);
    assert(restored.next_cell == 1 && !restored.active);
    assert(x4_t26_store_write_progress(&again, &restored) == 0);
    assert(x4_t26_state_begin(&restored, 1, X4_T26_MODE_MAXIMUM, X4_T26_PHASE_QUERY_COMPUTE) == 0);
    assert(x4_t26_store_write_progress(&again, &restored) == 0);
    file = x4_t26_store_cell_begin(&again, 1); assert(file);
    assert(fputs("{\"interrupted\":", file) >= 0);
    assert(fflush(file) == 0 && fsync(fileno(file)) == 0 && fclose(file) == 0);
    /* Store is intentionally abandoned, as on process exit; no reuse of its
     * now-closed FILE pointer occurs. Partial cell remains on disk. */
    assert(x4_t26_store_open_at(&st, root, build, manifest, 4000, &state, &resumed) == 0 && resumed);
    assert(x4_t26_store_cell_read(&st, 1, text, X4_T26_STORE_CELL_MAX + 1u, &size) == 2 && !size);
    assert(!x4_t26_store_cell_begin(&st, 1) && errno == EEXIST);
    assert(x4_t26_state_resume(&state, 0, false) == X4_T26_RESUME_SKIPPED);
    assert(state.next_cell == 2 && (state.closed_app_mask & 2u));
    assert(x4_t26_store_write_progress(&st, &state) == 0);
    file = x4_t26_store_summary_begin(&st); assert(file);
    assert(fputs("{\"schema\":1,\"fixture\":\"HOST ONLY\",\"measurements\":null}", file) >= 0);
    assert(x4_t26_store_summary_finish(&st, file) == 0);
    assert(!x4_t26_store_summary_begin(&st) && errno == EEXIST);
    assert(x4_t26_store_cell_read(&st, 0, path, 2, &size) < 0 && errno == ENOBUFS && !size);
    free(text); ++tests;
}

static void progress_failures(const char *base)
{
    for (unsigned phase = X4_T26_STORE_FAULT_FLUSH; phase <= X4_T26_STORE_FAULT_RENAME; ++phase) {
        char name[40], root[320], path[X4_T26_STORE_PATH_MAX];
        (void)snprintf(name, sizeof(name), "progress-failure-%u", phase);
        root_path(root, base, name);
        X4T26Store st, again; X4T26State state, restored; bool resumed;
        fresh(root, &st, &state);
        assert(x4_t26_store_progress_path(&st, path, sizeof(path)) == 0);
        char before[X4_T26_STORE_PROGRESS_MAX + 1], after[sizeof(before)];
        size_t before_size = load(path, before, sizeof(before));
        assert(x4_t26_state_begin(&state, 0, X4_T26_MODE_MAXIMUM, X4_T26_PHASE_QUERY_COMPUTE) == 0);
        x4_t26_store_test_fail(phase, EIO);
        assert(x4_t26_store_write_progress(&st, &state) < 0);
        assert(st.last_errno == EIO && temporaries(root) >= 1);
        assert(load(path, after, sizeof(after)) == before_size && !memcmp(before, after, before_size));
        assert(x4_t26_store_open_at(&again, root, build, manifest, 5000, &restored, &resumed) == 0 && resumed);
        assert(restored.generation == 1 && !restored.active);
        /* A later successful commit is atomic and can be reopened. */
        assert(x4_t26_store_write_progress(&st, &state) == 0);
        assert(x4_t26_store_open_at(&again, root, build, manifest, 6000, &restored, &resumed) == 0 && resumed);
        assert(restored.generation == state.generation && restored.active);
        assert(temporaries(root) >= 1); ++tests;
    }
}

static void collisions_and_invalid(const char *base)
{
    char root[320], path[X4_T26_STORE_PATH_MAX], text[X4_T26_STORE_PROGRESS_MAX + 1];
    root_path(root, base, "collisions-and-checksum"); assert(mkdir(root, 0755) == 0);
    int n = snprintf(path, sizeof(path), "%s/session-ca0b23e377fa-1000.lock", root);
    assert(n > 0 && (size_t)n < sizeof(path)); create_fixture(path, "EXISTING LOCK; DO NOT OVERWRITE\n");
    X4T26Store st, again; X4T26State state, restored; bool resumed;
    fresh(root, &st, &state); assert(!strcmp(st.session, "ca0b23e377fa-1000-1"));
    assert(load(path, text, sizeof(text)) && !strcmp(text, "EXISTING LOCK; DO NOT OVERWRITE\n"));
    assert(x4_t26_state_begin(&state, 0, X4_T26_MODE_MAXIMUM, X4_T26_PHASE_QUERY_COMPUTE) == 0);
    n = snprintf(path, sizeof(path), "%s/progress-%s-%" PRIu64 "-%" PRIu64 ".tmp", root, st.session, state.generation, st.nonce + 1u);
    assert(n > 0 && (size_t)n < sizeof(path)); create_fixture(path, "EXISTING TEMP; DO NOT OVERWRITE\n");
    assert(x4_t26_store_write_progress(&st, &state) == 0);
    assert(load(path, text, sizeof(text)) && !strcmp(text, "EXISTING TEMP; DO NOT OVERWRITE\n"));
    assert(x4_t26_store_progress_path(&st, path, sizeof(path)) == 0);
    size_t size = load(path, text, sizeof(text));
    char *checksum = strstr(text, "\"checksum\":\""); assert(checksum);
    checksum += strlen("\"checksum\":\""); *checksum = *checksum == '0' ? '1' : '0';
    FILE *f = fopen(path, "r+b"); assert(f);
    assert(fwrite(text, 1, size, f) == size && fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0);
    /* Invalid checksum fails closed: neither the old native phase nor a new
     * bank is started, and even the corrupt progress is preserved verbatim. */
    restored = state;
    X4T26State untouched = restored;
    assert(x4_t26_store_open_at(&again, root, build, manifest, 7000, &restored, &resumed) < 0 && !resumed);
    assert(!memcmp(&restored, &untouched, sizeof(restored)) && !again.session[0]);
    char after[sizeof(text)];
    assert(load(path, after, sizeof(after)) == size && !memcmp(text, after, size));
    assert(temporaries(root) >= 1);
    assert(x4_t26_store_cell_path(&st, 0, path, 1) < 0 && errno == ENAMETOOLONG);
    ++tests;
}

static void identity_mismatch_closed(const char *base)
{
    char root[320], path[X4_T26_STORE_PATH_MAX]; root_path(root, base, "identity-mismatch");
    X4T26Store st, again; X4T26State state, restored; bool resumed;
    fresh(root, &st, &state);
    assert(x4_t26_store_progress_path(&st, path, sizeof(path)) == 0);
    char before[X4_T26_STORE_PROGRESS_MAX + 1], after[sizeof(before)];
    size_t size = load(path, before, sizeof(before));
    const char *other_manifest = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    for (unsigned mode = 0; mode < 2; ++mode) {
        restored = state;
        assert(x4_t26_store_open_at(&again, root, mode ? build : "another-build",
            mode ? other_manifest : manifest, 8000, &restored, &resumed) < 0 && !resumed);
        assert(!again.session[0] && !memcmp(&restored, &state, sizeof(state)));
        assert(load(path, after, sizeof(after)) == size && !memcmp(before, after, size));
    }
    ++tests;
}

static void cell_policy_and_limits(const char *base)
{
    char root[320], path[X4_T26_STORE_PATH_MAX]; root_path(root, base, "cell-policy-and-limits");
    X4T26Store st; X4T26State state; fresh(root, &st, &state);
    char *text = malloc(X4_T26_STORE_CELL_MAX + 1u); assert(text); size_t size;
    const char *complete[] = {"false", "true", "true", "true"};
    const char *result[] = {"{}", "null", "{}", "{}"};
    for (unsigned cell = 0; cell < 4; ++cell) {
        assert(x4_t26_store_cell_path(&st, cell, path, sizeof(path)) == 0);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644); assert(fd >= 0);
        FILE *file = fdopen(fd, "wb"); assert(file);
        assert(fprintf(file, "{\"schema\":%u,\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\",\"cell\":%u,\"result\":%s,\"complete\":%s}\n",
            cell == 2 ? 2u : 1u, cell == 3 ? "wrong-build" : st.build, st.manifest, st.session, cell, result[cell], complete[cell]) > 0);
        assert(fclose(file) == 0);
        assert(x4_t26_store_cell_read(&st, cell, text, X4_T26_STORE_CELL_MAX + 1u, &size) == 2 && !size);
        assert(!x4_t26_store_cell_begin(&st, cell) && errno == EEXIST);
    }
    FILE *file = x4_t26_store_cell_begin(&st, 4); assert(file);
    char chunk[1024]; memset(chunk, ' ', sizeof(chunk));
    for (unsigned i = 0; i <= X4_T26_STORE_CELL_MAX / sizeof(chunk); ++i)
        assert(fwrite(chunk, 1, sizeof(chunk), file) == sizeof(chunk));
    assert(x4_t26_store_cell_finish(&st, file) < 0 && errno == EFBIG);
    assert(x4_t26_store_cell_read(&st, 4, text, X4_T26_STORE_CELL_MAX + 1u, &size) == 2);
    file = x4_t26_store_summary_begin(&st); assert(file);
    for (unsigned i = 0; i <= X4_T26_STORE_SUMMARY_MAX / sizeof(chunk); ++i)
        assert(fwrite(chunk, 1, sizeof(chunk), file) == sizeof(chunk));
    assert(x4_t26_store_summary_finish(&st, file) < 0 && errno == EFBIG);
    assert(!x4_t26_store_summary_begin(&st) && errno == EEXIST);
    assert(x4_t26_store_cell_finish(&st, NULL) < 0);
    free(text); ++tests;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct stat root; assert(stat(argv[1], &root) == 0 && S_ISDIR(root.st_mode));
    basic_and_cuts(argv[1]);
    progress_failures(argv[1]);
    collisions_and_invalid(argv[1]);
    identity_mismatch_closed(argv[1]);
    cell_policy_and_limits(argv[1]);
    printf("PASS T26 store %u real-I/O scenarios; HOST fixtures preserved at %s\n", tests, argv[1]);
    return 0;
}
