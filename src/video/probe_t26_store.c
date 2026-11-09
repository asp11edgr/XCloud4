/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "probe_t26_store.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "../auth/json.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#endif

#if defined(X4_T26_STORE_TESTING)
static unsigned failure_phase;
static int failure_errno;
void x4_t26_store_test_fail(unsigned phase, int number)
{ failure_phase = phase; failure_errno = number ? number : EIO; }
static int fault(unsigned phase)
{
    if (failure_phase != phase) return 0;
    failure_phase = 0; errno = failure_errno; return -1;
}
#else
enum { X4_T26_STORE_FAULT_FLUSH = 1, X4_T26_STORE_FAULT_SYNC,
    X4_T26_STORE_FAULT_CLOSE, X4_T26_STORE_FAULT_RENAME };
static int fault(unsigned phase) { (void)phase; return 0; }
#endif

static int failed(X4T26Store *st, const char *operation, const char *path,
    int raw, int number)
{
    if (!number) number = EIO;
    if (st) { st->last_rc = raw; st->last_errno = number; }
    fprintf(stderr, "XCloud4: T26 archivo op=%s ruta=%s retorno=%d errno=%d\n",
        operation, path && path[0] ? path : "<sin ruta>", raw, number);
    errno = number; return -1;
}

static bool ascii_root(const char *text)
{
    if (!text || !*text) return false;
    for (size_t n = 0; n < 320; ++n) {
        unsigned char c = (unsigned char)text[n];
        if (!c) return true;
        if (c < 32 || c > 126) return false;
    }
    return false;
}

static bool session_valid(const X4T26Store *st, const char *session)
{
    size_t prefix = 0;
    while (prefix < sizeof(st->build) && st->build[prefix]) ++prefix;
    if (!prefix || prefix == sizeof(st->build)) return false;
    if (prefix > 12) prefix = 12;
    if (strncmp(session, st->build, prefix) || session[prefix] != '-') return false;
    bool digit = false, counter = false;
    for (size_t i = prefix + 1; i < sizeof(st->session); ++i) {
        unsigned char c = (unsigned char)session[i];
        if (!c) return digit;
        if (c >= '0' && c <= '9') { digit = true; continue; }
        if (c == '-' && digit && !counter) { counter = true; digit = false; continue; }
        return false;
    }
    return false;
}

static int path_format(const X4T26Store *st, char *out, size_t capacity,
    const char *kind, unsigned cell)
{
    if (!st || !out || !capacity || !ascii_root(st->root)) { errno = EINVAL; return -1; }
    int n;
    if (!strcmp(kind, "progress")) n = snprintf(out, capacity, "%s/progress.json", st->root);
    else if (!session_valid(st, st->session)) { out[0] = 0; errno = EINVAL; return -1; }
    else if (!strcmp(kind, "cell")) {
        if (cell >= X4_T26_CELL_COUNT) { out[0] = 0; errno = EINVAL; return -1; }
        n = snprintf(out, capacity, "%s/cell-%s-%03u.json", st->root, st->session, cell);
    } else n = snprintf(out, capacity, "%s/results-%s.json", st->root, st->session);
    if (n < 0 || (size_t)n >= capacity) { out[0] = 0; errno = ENAMETOOLONG; return -1; }
    return 0;
}
int x4_t26_store_progress_path(const X4T26Store *st, char *out, size_t cap)
{ return path_format(st, out, cap, "progress", 0); }
int x4_t26_store_cell_path(const X4T26Store *st, unsigned cell, char *out, size_t cap)
{ return path_format(st, out, cap, "cell", cell); }
int x4_t26_store_summary_path(const X4T26Store *st, char *out, size_t cap)
{ return path_format(st, out, cap, "summary", 0); }

static FILE *exclusive(X4T26Store *st, const char *path)
{
    errno = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { failed(st, "open O_EXCL", path, fd, errno); return NULL; }
    errno = 0;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        int number = errno;
        if (close(fd)) (void)failed(st, "close tras fdopen", path, -1, errno);
        failed(st, "fdopen", path, -1, number); return NULL;
    }
    return file;
}

static int synchronize(X4T26Store *st, FILE *file, const char *path)
{
    errno = 0;
    int rc = fflush(file), number = errno;
    if (!rc && fault(X4_T26_STORE_FAULT_FLUSH)) { rc = -1; number = errno; }
    if (rc) return failed(st, "fflush", path, rc, number);
    errno = 0;
    int fd = fileno(file);
    if (fd < 0) return failed(st, "fileno", path, fd, errno);
    errno = 0;
#ifdef X4_OPENORBIS
    rc = sceKernelFsync(fd); number = errno;
    if (rc && !number) {
        uint32_t bits = (uint32_t)rc;
        if ((bits & UINT32_C(0xffff0000)) == UINT32_C(0x80020000)) number = (int)(bits & 0xffffu);
        else if (rc < 0 && rc >= -4095) number = -rc;
    }
#else
    rc = fsync(fd); number = errno;
#endif
    if (!rc && fault(X4_T26_STORE_FAULT_SYNC)) { rc = -1; number = errno; }
    return rc ? failed(st, "fsync", path, rc, number) : 0;
}

static int close_file(X4T26Store *st, FILE *file, const char *path)
{
    errno = 0;
    int rc = fclose(file), number = errno;
    if (!rc && fault(X4_T26_STORE_FAULT_CLOSE)) { rc = -1; number = errno; }
    return rc ? failed(st, "fclose", path, rc, number) : 0;
}

/* Return1 is ordinary absence; return2 is a file larger than the policy
 * bound. A small caller buffer is an API error, not a corrupt file. */
static int read_file(X4T26Store *st, const char *path, char *buffer,
    size_t capacity, size_t maximum, size_t *size)
{
    if (!buffer || capacity < 2 || !size) return failed(st, "leer capacidad", path, -1, EINVAL);
    *size = 0; buffer[0] = 0; errno = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? 1 : failed(st, "fopen lectura", path, -1, errno);
    size_t request = capacity - 1u;
    if (request > maximum) request = maximum;
    errno = 0;
    size_t count = fread(buffer, 1, request, file);
    int number = errno, rc = 0;
    if (ferror(file)) rc = failed(st, "fread", path, -1, number);
    else {
        errno = 0;
        int extra = fgetc(file);
        if (ferror(file)) rc = failed(st, "fgetc", path, -1, errno);
        else if (extra != EOF) rc = request < maximum ?
            failed(st, "leer capacidad", path, -1, ENOBUFS) : 2;
    }
    buffer[count] = 0;
    /* fclose is still required after a read/capacity failure, but its success
     * clears errno in close_file. Preserve the first failure for the caller. */
    int saved = errno;
    if (close_file(st, file, path) && !rc) { rc = -1; saved = errno; }
    if (!rc) *size = count;
    if (rc == 2) (void)failed(st, "leer limite", path, 2, EFBIG);
    else if (rc < 0) errno = saved ? saved : EIO;
    return rc;
}

static bool members(X4JsonSpan object, size_t expected)
{
    size_t cursor = 0, count = 0;
    X4JsonSpan key, value;
    int rc;
    while ((rc = x4_json_object_member(object, &cursor, &key, &value)) == 1) ++count;
    return rc == 0 && count == expected;
}
static bool number_member(X4JsonSpan object, const char *key, uint32_t expected)
{
    X4JsonSpan value; uint32_t number;
    return x4_json_member(object, key, &value) == 1 && !x4_json_uint32(value, &number) && number == expected;
}
static bool token_member(X4JsonSpan object, const char *key, const char *expected)
{
    X4JsonSpan value; char text[65];
    return x4_json_member(object, key, &value) == 1 && !x4_json_token(value, text, sizeof(text)) && !strcmp(text, expected);
}
static int progress_parse(X4T26Store *st, const char *text, size_t size,
    X4T26State *out, char session[48])
{
    X4JsonSpan root, value, core;
    if (x4_json_parse(text, size, &root) || !members(root, 3) ||
        !number_member(root, "schema", 1) || x4_json_member(root, "session", &value) != 1 ||
        x4_json_token(value, session, 48) || !session_valid(st, session) ||
        x4_json_member(root, "state", &core) != 1 ||
        x4_t26_state_parse(core.data, core.length, st->build, st->manifest, out)) return -1;
    return 0;
}

int x4_t26_store_write_progress(X4T26Store *st, const X4T26State *state)
{
    char path[X4_T26_STORE_PATH_MAX], temporary[X4_T26_STORE_PATH_MAX];
    char core[X4_T26_STATE_TEXT_MAX], envelope[X4_T26_STORE_PROGRESS_MAX];
    if (!st || !state || !x4_t26_state_valid(state) || !session_valid(st, st->session) ||
        strcmp(state->build_id, st->build) || strcmp(state->manifest_sha256, st->manifest) ||
        x4_t26_store_progress_path(st, path, sizeof(path)))
        return failed(st, "progress argumentos", NULL, -1, EINVAL);
    int count = x4_t26_state_serialize(state, core, sizeof(core));
    if (count < 0) return failed(st, "serializar progress", path, count, EINVAL);
    int n = snprintf(envelope, sizeof(envelope), "{\"schema\":1,\"session\":\"%s\",\"state\":%s}\n", st->session, core);
    if (n < 0 || (size_t)n >= sizeof(envelope)) return failed(st, "progress limite", path, -1, EFBIG);
    FILE *file = NULL;
    for (unsigned trial = 0; trial < 1024; ++trial) {
        if (st->nonce == UINT64_MAX) return failed(st, "progress nonce", path, -1, EOVERFLOW);
        ++st->nonce;
        count = snprintf(temporary, sizeof(temporary), "%s/progress-%s-%" PRIu64 "-%" PRIu64 ".tmp",
            st->root, st->session, state->generation, st->nonce);
        if (count < 0 || (size_t)count >= sizeof(temporary)) return failed(st, "progress ruta", path, -1, ENAMETOOLONG);
        file = exclusive(st, temporary);
        if (file) break;
        if (errno != EEXIST) return -1;
    }
    if (!file) return failed(st, "progress colisiones", temporary, -1, EEXIST);
    int rc = 0;
    errno = 0;
    if (fwrite(envelope, 1, (size_t)n, file) != (size_t)n) rc = failed(st, "escribir progress", temporary, -1, errno);
    if (!rc) rc = synchronize(st, file, temporary);
    int saved = errno;
    if (close_file(st, file, temporary) && !rc) { rc = -1; saved = errno; }
    if (rc) { errno = saved; return -1; }
    errno = 0;
    int raw = fault(X4_T26_STORE_FAULT_RENAME);
    if (!raw) raw = rename(temporary, path);
    return raw ? failed(st, "rename progress", temporary, raw, errno) : 0;
}

static int open_root(X4T26Store *st, const char *root, const char *build,
    const char *manifest, uint64_t now, X4T26State *state, bool *resumed)
{
    X4T26State fresh;
    if (!st || !state || !resumed || !ascii_root(root) ||
        x4_t26_state_init(&fresh, build, manifest, now)) return failed(st, "abrir argumentos", root, -1, EINVAL);
    *resumed = false;
    *st = (X4T26Store){0};
    memcpy(st->root, root, strlen(root) + 1u);
    memcpy(st->build, build, strlen(build) + 1u);
    memcpy(st->manifest, manifest, strlen(manifest) + 1u);
    errno = 0;
    int raw = mkdir(root, 0755);
    if (raw && errno != EEXIST) return failed(st, "mkdir", root, raw, errno);
    struct stat directory;
    errno = 0;
    if (stat(root, &directory) || !S_ISDIR(directory.st_mode)) return failed(st, "stat directorio", root, -1, errno ? errno : ENOTDIR);
    char path[X4_T26_STORE_PATH_MAX], text[X4_T26_STORE_PROGRESS_MAX + 1];
    if (x4_t26_store_progress_path(st, path, sizeof(path))) return failed(st, "ruta progress", root, -1, errno);
    size_t size;
    int result = read_file(st, path, text, sizeof(text), X4_T26_STORE_PROGRESS_MAX, &size);
    if (result < 0) return result;
    if (!result) {
        X4T26State candidate; char session[48] = {0};
        if (!progress_parse(st, text, size, &candidate, session)) {
            memcpy(st->session, session, sizeof(st->session));
            *state = candidate; *resumed = true; return 0;
        }
        return failed(st, "progress invalido o identidad distinta; banco detenido", path, 2, EINVAL);
    }
    if (result == 2) return failed(st, "progress excede limite; banco detenido", path, 2, EFBIG);
    size_t prefix = strlen(build);
    if (prefix > 12) prefix = 12;
    FILE *lock = NULL;
    for (unsigned counter = 0; counter < 1024; ++counter) {
        int n = counter ? snprintf(st->session, sizeof(st->session), "%.*s-%" PRIu64 "-%u", (int)prefix, build, now, counter) :
            snprintf(st->session, sizeof(st->session), "%.*s-%" PRIu64, (int)prefix, build, now);
        if (n < 0 || (size_t)n >= sizeof(st->session)) return failed(st, "session limite", root, -1, ENAMETOOLONG);
        n = snprintf(path, sizeof(path), "%s/session-%s.lock", root, st->session);
        if (n < 0 || (size_t)n >= sizeof(path)) return failed(st, "lock ruta", root, -1, ENAMETOOLONG);
        lock = exclusive(st, path);
        if (lock) break;
        if (errno != EEXIST) return -1;
    }
    if (!lock) return failed(st, "lock colisiones", path, -1, EEXIST);
    errno = 0;
    int rc = fprintf(lock, "{\"schema\":1,\"session\":\"%s\",\"build\":\"%s\"}\n", st->session, build) < 0 ?
        failed(st, "escribir lock", path, -1, errno) : 0;
    if (!rc) rc = synchronize(st, lock, path);
    int saved = errno;
    if (close_file(st, lock, path) && !rc) { rc = -1; saved = errno; }
    if (rc) { errno = saved; return -1; }
    if (x4_t26_store_write_progress(st, &fresh)) return -1;
    *state = fresh; return 0;
}
int x4_t26_store_open(X4T26Store *st, const char *build, const char *manifest,
    uint64_t now, X4T26State *state, bool *resumed)
{ return open_root(st, X4_T26_STORE_ROOT, build, manifest, now, state, resumed); }
#if defined(X4_T26_STORE_TESTING)
int x4_t26_store_open_at(X4T26Store *st, const char *root, const char *build,
    const char *manifest, uint64_t now, X4T26State *state, bool *resumed)
{ return open_root(st, root, build, manifest, now, state, resumed); }
#endif

static int cell_validate(const X4T26Store *st, unsigned cell, const char *text, size_t size)
{
    X4JsonSpan root, value; int complete;
    return x4_json_parse(text, size, &root) || !members(root, 7) ||
        !number_member(root, "schema", 1) || !number_member(root, "cell", cell) ||
        !token_member(root, "build", st->build) || !token_member(root, "manifest", st->manifest) ||
        !token_member(root, "session", st->session) ||
        x4_json_member(root, "result", &value) != 1 || x4_json_type(value) != X4_JSON_T_OBJECT ||
        x4_json_member(root, "complete", &value) != 1 || x4_json_bool(value, &complete) || !complete ? -1 : 0;
}
int x4_t26_store_cell_read(X4T26Store *st, unsigned cell, char *buffer, size_t capacity, size_t *size)
{
    char path[X4_T26_STORE_PATH_MAX];
    if (!size) return failed(st, "cell leer argumentos", NULL, -1, EINVAL);
    *size = 0;
    if (x4_t26_store_cell_path(st, cell, path, sizeof(path))) return failed(st, "cell ruta", NULL, -1, errno);
    size_t length;
    int rc = read_file(st, path, buffer, capacity, X4_T26_STORE_CELL_MAX, &length);
    if (rc) return rc;
    if (cell_validate(st, cell, buffer, length)) {
        (void)failed(st, "cell invalida o parcial", path, 2, EINVAL); return 2;
    }
    *size = length; return 0;
}
FILE *x4_t26_store_cell_begin(X4T26Store *st, unsigned cell)
{
    if (!st || st->cell_file) { failed(st, "cell begin ocupado", NULL, -1, EBUSY); return NULL; }
    if (x4_t26_store_cell_path(st, cell, st->active_cell_path, sizeof(st->active_cell_path))) {
        failed(st, "cell begin ruta", NULL, -1, errno); return NULL;
    }
    FILE *file = exclusive(st, st->active_cell_path);
    if (!file) return NULL;
    errno = 0;
    if (fprintf(file, "{\"schema\":1,\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\",\"cell\":%u,\"result\":",
        st->build, st->manifest, st->session, cell) < 0) {
        int number = errno;
        (void)close_file(st, file, st->active_cell_path);
        failed(st, "cell escribir prefijo", st->active_cell_path, -1, number); return NULL;
    }
    st->active_cell = cell; st->cell_file = file; return file;
}

static int finish_document(X4T26Store *st, FILE *file, const char *path, size_t maximum, bool cell)
{
    int rc = 0;
    errno = 0;
    if (ferror(file) || fflush(file)) rc = failed(st, "documento fflush", path, -1, errno);
    errno = 0;
    long offset = rc ? -1 : ftell(file);
    if (!rc && (offset < 0 || (uint64_t)offset > maximum)) rc = failed(st, "documento limite", path, -1, offset < 0 ? errno : EFBIG);
    char *text = NULL;
    if (!rc) {
        text = malloc((size_t)offset + 1u);
        if (!text) rc = failed(st, "documento memoria", path, -1, ENOMEM);
    }
    if (!rc) {
        size_t size;
        int read = read_file(st, path, text, (size_t)offset + 1u, maximum, &size);
        X4JsonSpan document;
        if (read < 0) rc = -1;
        else if (read) rc = failed(st, "documento leer", path, read, read == 1 ? ENOENT : EFBIG);
        else if (cell ? cell_validate(st, st->active_cell, text, size) :
            x4_json_parse(text, size, &document) || x4_json_type(document) != X4_JSON_T_OBJECT)
            rc = failed(st, "documento JSON", path, -1, EINVAL);
    }
    free(text);
    if (!rc) rc = synchronize(st, file, path);
    int saved = errno;
    if (close_file(st, file, path) && !rc) { rc = -1; saved = errno; }
    if (rc) errno = saved;
    return rc;
}
int x4_t26_store_cell_finish(X4T26Store *st, FILE *file)
{
    if (!st || !file || file != st->cell_file) return failed(st, "cell finish argumentos", NULL, -1, EINVAL);
    errno = 0;
    if (fputs(",\"complete\":true}\n", file) == EOF) {
        int number = errno;
        st->cell_file = NULL;
        (void)close_file(st, file, st->active_cell_path);
        return failed(st, "cell finish escribir", st->active_cell_path, -1, number);
    }
    st->cell_file = NULL;
    return finish_document(st, file, st->active_cell_path, X4_T26_STORE_CELL_MAX, true);
}
FILE *x4_t26_store_summary_begin(X4T26Store *st)
{
    if (!st || st->summary_file) { failed(st, "summary begin ocupado", NULL, -1, EBUSY); return NULL; }
    if (x4_t26_store_summary_path(st, st->active_summary_path, sizeof(st->active_summary_path))) {
        failed(st, "summary ruta", NULL, -1, errno); return NULL;
    }
    st->summary_file = exclusive(st, st->active_summary_path);
    return st->summary_file;
}
int x4_t26_store_summary_finish(X4T26Store *st, FILE *file)
{
    if (!st || !file || file != st->summary_file) return failed(st, "summary finish argumentos", NULL, -1, EINVAL);
    st->summary_file = NULL;
    return finish_document(st, file, st->active_summary_path, X4_T26_STORE_SUMMARY_MAX, false);
}
#endif
