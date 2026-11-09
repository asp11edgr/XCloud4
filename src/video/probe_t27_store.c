/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "probe_t27_store.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
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

#if defined(X4_T27_STORE_TESTING)
static unsigned failure_phase;
static int failure_errno;
void x4_t27_store_test_fail(unsigned phase, int number)
{ failure_phase = phase; failure_errno = number ? number : EIO; }
static int t27_fault(unsigned phase)
{
    if (failure_phase != phase) return 0;
    failure_phase = 0; errno = failure_errno; return -1;
}
#else
enum { X4_T27_STORE_FAULT_FLUSH = 1, X4_T27_STORE_FAULT_SYNC,
    X4_T27_STORE_FAULT_CLOSE, X4_T27_STORE_FAULT_RENAME };
static int t27_fault(unsigned phase) { (void)phase; return 0; }
#endif

static int t27_failed(X4T27Store *st, const char *operation, const char *path,
    int raw, int number)
{
    if (!number) number = EIO;
    if (st) { st->last_rc = raw; st->last_errno = number; }
    fprintf(stderr, "XCloud4: T27 archivo op=%s ruta=%s retorno=%d errno=%d\n",
        operation, path && path[0] ? path : "<sin ruta>", raw, number);
    errno = number; return -1;
}

static bool t27_ascii_root(const char *text)
{
    if (!text || !*text) return false;
    for (size_t n = 0; n < 320; ++n) {
        unsigned char c = (unsigned char)text[n];
        if (!c) return true;
        if (c < 32 || c > 126) return false;
    }
    return false;
}

static bool t27_session_valid(const X4T27Store *st, const char *session)
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

static int t27_path_format(const X4T27Store *st, char *out, size_t capacity,
    const char *kind, unsigned cell)
{
    if (!st || !out || !capacity || !t27_ascii_root(st->root)) { errno = EINVAL; return -1; }
    int n;
    if (!strcmp(kind, "progress")) n = snprintf(out, capacity, "%s/progress.json", st->root);
    else if (!t27_session_valid(st, st->session)) { out[0] = 0; errno = EINVAL; return -1; }
    else if (!strcmp(kind, "cell")) {
        if (cell >= X4_T27_CELL_COUNT) { out[0] = 0; errno = EINVAL; return -1; }
        n = snprintf(out, capacity, "%s/cell-%s-%03u.json", st->root, st->session, cell);
    } else n = snprintf(out, capacity, "%s/results-%s.json", st->root, st->session);
    if (n < 0 || (size_t)n >= capacity) { out[0] = 0; errno = ENAMETOOLONG; return -1; }
    return 0;
}
int x4_t27_store_progress_path(const X4T27Store *st, char *out, size_t cap)
{ return t27_path_format(st, out, cap, "progress", 0); }
int x4_t27_store_cell_path(const X4T27Store *st, unsigned cell, char *out, size_t cap)
{ return t27_path_format(st, out, cap, "cell", cell); }
int x4_t27_store_summary_path(const X4T27Store *st, char *out, size_t cap)
{ return t27_path_format(st, out, cap, "summary", 0); }

static FILE *t27_exclusive(X4T27Store *st, const char *path)
{
    errno = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { t27_failed(st, "open O_EXCL", path, fd, errno); return NULL; }
    errno = 0;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        int number = errno;
        if (close(fd)) (void)t27_failed(st, "close tras fdopen", path, -1, errno);
        t27_failed(st, "fdopen", path, -1, number); return NULL;
    }
    return file;
}

static int t27_synchronize(X4T27Store *st, FILE *file, const char *path)
{
    errno = 0;
    int rc = fflush(file), number = errno;
    if (!rc && t27_fault(X4_T27_STORE_FAULT_FLUSH)) { rc = -1; number = errno; }
    if (rc) return t27_failed(st, "fflush", path, rc, number);
    errno = 0;
    int fd = fileno(file);
    if (fd < 0) return t27_failed(st, "fileno", path, fd, errno);
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
    if (!rc && t27_fault(X4_T27_STORE_FAULT_SYNC)) { rc = -1; number = errno; }
    return rc ? t27_failed(st, "fsync", path, rc, number) : 0;
}

static int t27_close_file(X4T27Store *st, FILE *file, const char *path)
{
    errno = 0;
    int rc = fclose(file), number = errno;
    if (!rc && t27_fault(X4_T27_STORE_FAULT_CLOSE)) { rc = -1; number = errno; }
    return rc ? t27_failed(st, "fclose", path, rc, number) : 0;
}

/* Return1 is ordinary absence; return2 is a file larger than the policy
 * bound. A small caller buffer is an API error, not a corrupt file. */
static int t27_read_file(X4T27Store *st, const char *path, char *buffer,
    size_t capacity, size_t maximum, size_t *size)
{
    if (!buffer || capacity < 2 || !size) return t27_failed(st, "leer capacidad", path, -1, EINVAL);
    *size = 0; buffer[0] = 0; errno = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? 1 : t27_failed(st, "fopen lectura", path, -1, errno);
    size_t request = capacity - 1u;
    if (request > maximum) request = maximum;
    errno = 0;
    size_t count = fread(buffer, 1, request, file);
    int number = errno, rc = 0;
    if (ferror(file)) rc = t27_failed(st, "fread", path, -1, number);
    else {
        errno = 0;
        int extra = fgetc(file);
        if (ferror(file)) rc = t27_failed(st, "fgetc", path, -1, errno);
        else if (extra != EOF) rc = request < maximum ?
            t27_failed(st, "leer capacidad", path, -1, ENOBUFS) : 2;
    }
    buffer[count] = 0;
    /* fclose is still required after a read/capacity failure, but its success
     * clears errno in t27_close_file. Preserve the first failure for the caller. */
    int saved = errno;
    if (t27_close_file(st, file, path) && !rc) { rc = -1; saved = errno; }
    if (!rc) *size = count;
    if (rc == 2) (void)t27_failed(st, "leer limite", path, 2, EFBIG);
    else if (rc < 0) errno = saved ? saved : EIO;
    return rc;
}

static bool t27_members(X4JsonSpan object, size_t expected)
{
    size_t cursor = 0, count = 0;
    X4JsonSpan key, value;
    int rc;
    while ((rc = x4_json_object_member(object, &cursor, &key, &value)) == 1) ++count;
    return rc == 0 && count == expected;
}
static bool t27_number_member(X4JsonSpan object, const char *key, uint32_t expected)
{
    X4JsonSpan value; uint32_t number;
    return x4_json_member(object, key, &value) == 1 && !x4_json_uint32(value, &number) && number == expected;
}
static bool t27_token_member(X4JsonSpan object, const char *key, const char *expected)
{
    X4JsonSpan value; char text[65];
    return x4_json_member(object, key, &value) == 1 && !x4_json_token(value, text, sizeof(text)) && !strcmp(text, expected);
}
static int t27_progress_parse(X4T27Store *st, const char *text, size_t size,
    X4T27State *out, char session[48])
{
    X4JsonSpan root, value, core;
    if (x4_json_parse(text, size, &root) || !t27_members(root, 3) ||
        !t27_number_member(root, "schema", 1) || x4_json_member(root, "session", &value) != 1 ||
        x4_json_token(value, session, 48) || !t27_session_valid(st, session) ||
        x4_json_member(root, "state", &core) != 1 ||
        x4_t27_state_parse(core.data, core.length, st->build, st->manifest, out)) return -1;
    return 0;
}

int x4_t27_store_write_progress(X4T27Store *st, const X4T27State *state)
{
    char path[X4_T27_STORE_PATH_MAX], temporary[X4_T27_STORE_PATH_MAX];
    char core[X4_T27_STATE_TEXT_MAX], envelope[X4_T27_STORE_PROGRESS_MAX];
    if (!st || !state || !x4_t27_state_valid(state) || !t27_session_valid(st, st->session) ||
        strcmp(state->build_id, st->build) || strcmp(state->manifest_sha256, st->manifest) ||
        x4_t27_store_progress_path(st, path, sizeof(path)))
        return t27_failed(st, "progress argumentos", NULL, -1, EINVAL);
    int count = x4_t27_state_serialize(state, core, sizeof(core));
    if (count < 0) return t27_failed(st, "serializar progress", path, count, EINVAL);
    int n = snprintf(envelope, sizeof(envelope), "{\"schema\":1,\"session\":\"%s\",\"state\":%s}\n", st->session, core);
    if (n < 0 || (size_t)n >= sizeof(envelope)) return t27_failed(st, "progress limite", path, -1, EFBIG);
    FILE *file = NULL;
    for (unsigned trial = 0; trial < 1024; ++trial) {
        if (st->nonce == UINT64_MAX) return t27_failed(st, "progress nonce", path, -1, EOVERFLOW);
        ++st->nonce;
        count = snprintf(temporary, sizeof(temporary), "%s/progress-%s-%" PRIu64 "-%" PRIu64 ".tmp",
            st->root, st->session, state->generation, st->nonce);
        if (count < 0 || (size_t)count >= sizeof(temporary)) return t27_failed(st, "progress ruta", path, -1, ENAMETOOLONG);
        file = t27_exclusive(st, temporary);
        if (file) break;
        if (errno != EEXIST) return -1;
    }
    if (!file) return t27_failed(st, "progress colisiones", temporary, -1, EEXIST);
    int rc = 0;
    errno = 0;
    if (fwrite(envelope, 1, (size_t)n, file) != (size_t)n) rc = t27_failed(st, "escribir progress", temporary, -1, errno);
    if (!rc) rc = t27_synchronize(st, file, temporary);
    int saved = errno;
    if (t27_close_file(st, file, temporary) && !rc) { rc = -1; saved = errno; }
    if (rc) { errno = saved; return -1; }
    errno = 0;
    int raw = t27_fault(X4_T27_STORE_FAULT_RENAME);
    if (!raw) raw = rename(temporary, path);
    return raw ? t27_failed(st, "rename progress", temporary, raw, errno) : 0;
}

static int t27_open_root(X4T27Store *st, const char *root, const char *build,
    const char *manifest, uint64_t now, X4T27State *state, bool *resumed)
{
    X4T27State fresh;
    if (!st || !state || !resumed || !t27_ascii_root(root) ||
        x4_t27_state_init(&fresh, build, manifest, now)) return t27_failed(st, "abrir argumentos", root, -1, EINVAL);
    *resumed = false;
    *st = (X4T27Store){0};
    memcpy(st->root, root, strlen(root) + 1u);
    memcpy(st->build, build, strlen(build) + 1u);
    memcpy(st->manifest, manifest, strlen(manifest) + 1u);
    errno = 0;
    int raw = mkdir(root, 0755);
    if (raw && errno != EEXIST) return t27_failed(st, "mkdir", root, raw, errno);
    struct stat directory;
    errno = 0;
    if (stat(root, &directory) || !S_ISDIR(directory.st_mode)) return t27_failed(st, "stat directorio", root, -1, errno ? errno : ENOTDIR);
    char path[X4_T27_STORE_PATH_MAX], text[X4_T27_STORE_PROGRESS_MAX + 1];
    if (x4_t27_store_progress_path(st, path, sizeof(path))) return t27_failed(st, "ruta progress", root, -1, errno);
    size_t size;
    int result = t27_read_file(st, path, text, sizeof(text), X4_T27_STORE_PROGRESS_MAX, &size);
    if (result < 0) return result;
    if (!result) {
        X4T27State candidate; char session[48] = {0};
        if (!t27_progress_parse(st, text, size, &candidate, session)) {
            memcpy(st->session, session, sizeof(st->session));
            *state = candidate; *resumed = true; return 0;
        }
        return t27_failed(st, "progress invalido o identidad distinta; banco detenido", path, 2, EINVAL);
    }
    if (result == 2) return t27_failed(st, "progress excede limite; banco detenido", path, 2, EFBIG);
    size_t prefix = strlen(build);
    if (prefix > 12) prefix = 12;
    FILE *lock = NULL;
    for (unsigned counter = 0; counter < 1024; ++counter) {
        int n = counter ? snprintf(st->session, sizeof(st->session), "%.*s-%" PRIu64 "-%u", (int)prefix, build, now, counter) :
            snprintf(st->session, sizeof(st->session), "%.*s-%" PRIu64, (int)prefix, build, now);
        if (n < 0 || (size_t)n >= sizeof(st->session)) return t27_failed(st, "session limite", root, -1, ENAMETOOLONG);
        n = snprintf(path, sizeof(path), "%s/session-%s.lock", root, st->session);
        if (n < 0 || (size_t)n >= sizeof(path)) return t27_failed(st, "lock ruta", root, -1, ENAMETOOLONG);
        lock = t27_exclusive(st, path);
        if (lock) break;
        if (errno != EEXIST) return -1;
    }
    if (!lock) return t27_failed(st, "lock colisiones", path, -1, EEXIST);
    errno = 0;
    int rc = fprintf(lock, "{\"schema\":1,\"session\":\"%s\",\"build\":\"%s\"}\n", st->session, build) < 0 ?
        t27_failed(st, "escribir lock", path, -1, errno) : 0;
    if (!rc) rc = t27_synchronize(st, lock, path);
    int saved = errno;
    if (t27_close_file(st, lock, path) && !rc) { rc = -1; saved = errno; }
    if (rc) { errno = saved; return -1; }
    if (x4_t27_store_write_progress(st, &fresh)) return -1;
    *state = fresh; return 0;
}
int x4_t27_store_open(X4T27Store *st, const char *build, const char *manifest,
    uint64_t now, X4T27State *state, bool *resumed)
{ return t27_open_root(st, X4_T27_STORE_ROOT, build, manifest, now, state, resumed); }
#if defined(X4_T27_STORE_TESTING)
int x4_t27_store_open_at(X4T27Store *st, const char *root, const char *build,
    const char *manifest, uint64_t now, X4T27State *state, bool *resumed)
{ return t27_open_root(st, root, build, manifest, now, state, resumed); }
#endif

static int t27_cell_validate(const X4T27Store *st, unsigned cell, const char *text, size_t size)
{
    X4JsonSpan root, value; int complete;
    return x4_json_parse(text, size, &root) || !t27_members(root, 7) ||
        !t27_number_member(root, "schema", 1) || !t27_number_member(root, "cell", cell) ||
        !t27_token_member(root, "build", st->build) || !t27_token_member(root, "manifest", st->manifest) ||
        !t27_token_member(root, "session", st->session) ||
        x4_json_member(root, "result", &value) != 1 || x4_json_type(value) != X4_JSON_T_OBJECT ||
        x4_json_member(root, "complete", &value) != 1 || x4_json_bool(value, &complete) || !complete ? -1 : 0;
}
int x4_t27_store_cell_read(X4T27Store *st, unsigned cell, char *buffer, size_t capacity, size_t *size)
{
    char path[X4_T27_STORE_PATH_MAX];
    if (!size) return t27_failed(st, "cell leer argumentos", NULL, -1, EINVAL);
    *size = 0;
    if (x4_t27_store_cell_path(st, cell, path, sizeof(path))) return t27_failed(st, "cell ruta", NULL, -1, errno);
    size_t length;
    int rc = t27_read_file(st, path, buffer, capacity, X4_T27_STORE_CELL_MAX, &length);
    if (rc) return rc;
    if (t27_cell_validate(st, cell, buffer, length)) {
        (void)t27_failed(st, "cell invalida o parcial", path, 2, EINVAL); return 2;
    }
    *size = length; return 0;
}
FILE *x4_t27_store_cell_begin(X4T27Store *st, unsigned cell)
{
    if (!st || st->cell_file) { t27_failed(st, "cell begin ocupado", NULL, -1, EBUSY); return NULL; }
    if (x4_t27_store_cell_path(st, cell, st->active_cell_path, sizeof(st->active_cell_path))) {
        t27_failed(st, "cell begin ruta", NULL, -1, errno); return NULL;
    }
    FILE *file = t27_exclusive(st, st->active_cell_path);
    if (!file) return NULL;
    errno = 0;
    if (fprintf(file, "{\"schema\":1,\"build\":\"%s\",\"manifest\":\"%s\",\"session\":\"%s\",\"cell\":%u,\"result\":",
        st->build, st->manifest, st->session, cell) < 0) {
        int number = errno;
        (void)t27_close_file(st, file, st->active_cell_path);
        t27_failed(st, "cell escribir prefijo", st->active_cell_path, -1, number); return NULL;
    }
    st->active_cell = cell; st->cell_file = file; return file;
}

static int t27_finish_document(X4T27Store *st, FILE *file, const char *path, size_t maximum, bool cell)
{
    int rc = 0;
    errno = 0;
    if (ferror(file) || fflush(file)) rc = t27_failed(st, "documento fflush", path, -1, errno);
    errno = 0;
    long offset = rc ? -1 : ftell(file);
    if (!rc && (offset < 0 || (uint64_t)offset > maximum)) rc = t27_failed(st, "documento limite", path, -1, offset < 0 ? errno : EFBIG);
    char *text = NULL;
    if (!rc) {
        text = malloc((size_t)offset + 1u);
        if (!text) rc = t27_failed(st, "documento memoria", path, -1, ENOMEM);
    }
    if (!rc) {
        size_t size;
        int read = t27_read_file(st, path, text, (size_t)offset + 1u, maximum, &size);
        X4JsonSpan document;
        if (read < 0) rc = -1;
        else if (read) rc = t27_failed(st, "documento leer", path, read, read == 1 ? ENOENT : EFBIG);
        else if (cell ? t27_cell_validate(st, st->active_cell, text, size) :
            x4_json_parse(text, size, &document) || x4_json_type(document) != X4_JSON_T_OBJECT)
            rc = t27_failed(st, "documento JSON", path, -1, EINVAL);
    }
    free(text);
    if (!rc) rc = t27_synchronize(st, file, path);
    int saved = errno;
    if (t27_close_file(st, file, path) && !rc) { rc = -1; saved = errno; }
    if (rc) errno = saved;
    return rc;
}
int x4_t27_store_cell_finish(X4T27Store *st, FILE *file)
{
    if (!st || !file || file != st->cell_file) return t27_failed(st, "cell finish argumentos", NULL, -1, EINVAL);
    errno = 0;
    if (fputs(",\"complete\":true}\n", file) == EOF) {
        int number = errno;
        st->cell_file = NULL;
        (void)t27_close_file(st, file, st->active_cell_path);
        return t27_failed(st, "cell finish escribir", st->active_cell_path, -1, number);
    }
    st->cell_file = NULL;
    return t27_finish_document(st, file, st->active_cell_path, X4_T27_STORE_CELL_MAX, true);
}
FILE *x4_t27_store_summary_begin(X4T27Store *st)
{
    if (!st || st->summary_file) { t27_failed(st, "summary begin ocupado", NULL, -1, EBUSY); return NULL; }
    if (x4_t27_store_summary_path(st, st->active_summary_path, sizeof(st->active_summary_path))) {
        t27_failed(st, "summary ruta", NULL, -1, errno); return NULL;
    }
    st->summary_file = t27_exclusive(st, st->active_summary_path);
    return st->summary_file;
}
int x4_t27_store_summary_finish(X4T27Store *st, FILE *file)
{
    if (!st || !file || file != st->summary_file) return t27_failed(st, "summary finish argumentos", NULL, -1, EINVAL);
    st->summary_file = NULL;
    return t27_finish_document(st, file, st->active_summary_path, X4_T27_STORE_SUMMARY_MAX, false);
}
#endif
