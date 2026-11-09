/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "probe_files.h"
#if X4_VIDEO_PROBE || defined(X4_PROBE_FILES_TESTING)
#include "../core/build_identity.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <stdbool.h>
#include "probe_t26_samples.h"
#endif
#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#include "probe_case_catalog.h"
#endif
#if defined(X4_PROBE_FILES_TESTING) && defined(X4_OPENORBIS)
#error Offline fixture and fault injection must never enter a PS4 build
#endif
#define BUILD_FLAGS X4_NACK_ID X4_GRACE_ID X4_RUMBLE_ID X4_VIDEO_CAPACITY_ID X4_VIDEO_PROBE_ID X4_INGRESS_ID
/* Idle controller owns bounded selfcheck. File metadata is emitted once
 * per sample open during preparation, never per AU/frame or packet. */
static X4ProbeFileSelfcheck *recording;
#if defined(X4_PROBE_FILES_TESTING)
static char fixture_root[X4_PROBE_FILE_PATH_MAX];
static int injected_fault, injected_errno;
static int fixture_stat_enabled, fixture_manifest_required;
static uint32_t fixture_mode;
static uint64_t fixture_nlink;
static int64_t fixture_size;
#else
enum { X4_PROBE_FILE_FAULT_CREATE=1, X4_PROBE_FILE_FAULT_WRITE,
    X4_PROBE_FILE_FAULT_FLUSH, X4_PROBE_FILE_FAULT_SYNC, X4_PROBE_FILE_FAULT_CLOSE,
    X4_PROBE_FILE_FAULT_SHORT_WRITE, X4_PROBE_FILE_FAULT_OWNER,
    X4_PROBE_FILE_FAULT_OPENAT_INFO };
#endif
static const char *data_root(void)
{
#if defined(X4_PROBE_FILES_TESTING)
    if (fixture_root[0]) return fixture_root;
#endif
    return X4_PROBE_REPORT_ROOT;
}
static int fault(int kind)
{
#if defined(X4_PROBE_FILES_TESTING)
    if (injected_fault == kind) { errno = injected_errno; return -1; }
#else
    (void)kind;
#endif
    return 0;
}
static X4ProbeFileCall observe(const char *call, int64_t raw, int saved, int native)
{
    X4ProbeFileCall value = {call, raw, saved, native};
    if (recording) {
        if (recording->call_count < X4_PROBE_FILE_CALL_CAPACITY) recording->calls[recording->call_count++] = value;
        else ++recording->calls_omitted;
    }
    return value;
}
static int status(X4ProbeFileStatus *out, const char *operation, int rc,
    int error_number, X4ProbeFileCall value)
{
    if (out) *out = (X4ProbeFileStatus){rc, rc ? error_number : 0, value.native_result,
        operation, value.raw_return, value.error_number, value.call};
    if (rc) errno = error_number;
    return rc;
}
static int policy(X4ProbeFileStatus *out, const char *operation, int error_number)
{ return status(out, operation, -1, error_number, (X4ProbeFileCall){"validation", -1, error_number, 0}); }
static int failed(X4ProbeFileStatus *out, const char *operation, X4ProbeFileCall call)
{ return status(out, operation, -1, call.error_number ? call.error_number : EIO, call); }
static int close_descriptor(int fd, X4ProbeFileStatus *out)
{
    errno = 0; int raw = close(fd), saved = errno;
    X4ProbeFileCall call = observe("close", raw, saved, 0);
    if (raw && recording) recording->cleanup_failed = 1;
    return raw ? failed(out, "Cerrar descriptor", call) : status(out, "Cerrar descriptor", 0, 0, call);
}
static int checked_stat(int fd, struct stat *st, X4ProbeFileStatus *out)
{
    errno = 0; int raw = fstat(fd, st), saved = errno;
    X4ProbeFileCall call = observe("fstat", raw, saved, 0);
    return raw ? failed(out, "Consultar descriptor (fstat)", call) : status(out, "Consultar descriptor (fstat)", 0, 0, call);
}
static int ascii_id(const char *id)
{
    if (!id || !*id || strnlen(id, 81) > 80) return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
    return 1;
}
int x4_probe_files_report_path(char *out, size_t capacity, const char *case_id, uint64_t sequence)
{
    if (!out || !capacity || capacity > X4_PROBE_FILE_PATH_MAX || !ascii_id(case_id)) { errno = EINVAL; return -1; }
    errno = 0;
    int raw = snprintf(out, capacity, "%s/xcloud4-probe-%.12s%s-%s-%llu.json", data_root(),
        X4_SOURCE_ID, BUILD_FLAGS, case_id, (unsigned long long)sequence), saved = errno;
    (void)observe("snprintf", raw, saved, 0);
    if (raw < 0 || (size_t)raw >= capacity) { out[0] = 0; errno = ENAMETOOLONG; return -1; }
    return 0;
}
static const char *fixed_leaf(const char *path, const char *root)
{
    if (!path || !root) { errno = EINVAL; return NULL; }
    size_t n = strnlen(path, X4_PROBE_FILE_PATH_MAX + 1), prefix = strlen(root);
    if (n > X4_PROBE_FILE_PATH_MAX || n <= prefix + 1 || strncmp(path, root, prefix) || path[prefix] != '/') { errno = EINVAL; return NULL; }
    const char *leaf = path + prefix + 1;
    if (strstr(leaf, "..")) { errno = EINVAL; return NULL; }
    for (const unsigned char *p = (const unsigned char *)leaf; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) { errno = EINVAL; return NULL; }
    return leaf;
}
static int valid_report(const char *path)
{
    const char *leaf = fixed_leaf(path, data_root());
    const size_t prefix_length = sizeof("xcloud4-probe-") - 1;
    if (!leaf || strncmp(leaf, "xcloud4-probe-", prefix_length) || strlen(leaf) < 50) { errno = EINVAL; return 0; }
    const char *p = leaf + prefix_length;
    for (unsigned i = 0; i < 12; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) { errno = EINVAL; return 0; }
    p += 12;
#ifndef X4_PROBE_FILES_TESTING
    if (strncmp(leaf + prefix_length, X4_SOURCE_ID, 12) || strncmp(p, BUILD_FLAGS, strlen(BUILD_FLAGS))) { errno = EINVAL; return 0; }
    p += strlen(BUILD_FLAGS);
#else
    if (strncmp(p, "-n", 2) || !(p[2] == '0' || p[2] == '1')) { errno = EINVAL; return 0; }
    p += 3;
    if (!strncmp(p, "-g45", 4)) p += 4;
    else if (!strncmp(p, "-g0", 3)) p += 3;
    else { errno = EINVAL; return 0; }
    if (strncmp(p, "-r", 2) || !(p[2] == '0' || p[2] == '1')) { errno = EINVAL; return 0; }
    p += 3;
    if (strncmp(p, "-vc", 3) || !(p[3] == '0' || p[3] == '1')) { errno = EINVAL; return 0; }
    p += 4;
    if (strncmp(p, "-p", 2) || !(p[2] == '0' || p[2] == '1')) { errno = EINVAL; return 0; }
    p += 3;
    if (strncmp(p, "-i", 2) || !(p[2] == 'm' || p[2] == 'b')) { errno = EINVAL; return 0; }
    p += 3;
#endif
    if (*p++ != '-') { errno = EINVAL; return 0; }
    const char *last = strrchr(p, '-');
    if (!last || last == p || (size_t)(last - p) > 80) { errno = EINVAL; return 0; }
    for (const char *c = p; c < last; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) { errno = EINVAL; return 0; }
    p = last + 1;
    if (*p < '0' || *p > '9') { errno = EINVAL; return 0; }
    while (*p >= '0' && *p <= '9') ++p;
    if (strcmp(p, ".json")) { errno = EINVAL; return 0; }
    return 1;
}
int x4_probe_files_prepare(X4ProbeFileStatus *out)
{
    errno = 0; int fd = open(data_root(), O_RDONLY | O_DIRECTORY), saved = errno;
    X4ProbeFileCall opened = observe("open(/data)", fd, saved, 0);
    if (fd < 0) return failed(out, "Abrir /data", opened);
    struct stat st; X4ProbeFileStatus current;
    int rc = checked_stat(fd, &st, &current);
    if (!rc && !S_ISDIR(st.st_mode)) rc = policy(&current, "/data no es un directorio", ENOTDIR);
    X4ProbeFileStatus closing; int close_rc = close_descriptor(fd, &closing);
    if (rc) { if (out) *out = current; errno = current.error_number; return -1; }
    if (close_rc) { if (out) *out = closing; errno = closing.error_number; return -1; }
    return status(out, "/data comprobado; sin carpeta nueva", 0, 0, opened);
}
/* Size observations only. The original 24 files remain unchanged; three
 * additive pipeline fixture files follow in the table when probe is enabled.
 * content integrity comes from full SHA-256 in source/reference, not stat.
 * This table does not replace the native fixed descriptor path allowlist. */
static const struct { const char *leaf; uint64_t bytes; } sample_manifest_sizes[] = {
    {"control-720p30-cb.h264", UINT64_C(4120327)},
    {"control-720p30-cb.x4au", UINT64_C(7296)},
    {"control-720p30-cb.rgba", UINT64_C(29491200)},
    {"720p60-cb.h264", UINT64_C(5589559)},
    {"720p60-cb.x4au", UINT64_C(14496)},
    {"720p60-cb.rgba", UINT64_C(29491200)},
    {"1080p60-cb.h264", UINT64_C(10971470)},
    {"1080p60-cb.x4au", UINT64_C(14496)},
    {"1080p60-cb.rgba", UINT64_C(66355200)},
    {"1080p60-main.h264", UINT64_C(9695042)},
    {"1080p60-main.x4au", UINT64_C(14496)},
    {"1080p60-main.rgba", UINT64_C(66355200)},
    {"1080p60-high.h264", UINT64_C(9606625)},
    {"1080p60-high.x4au", UINT64_C(14496)},
    {"1080p60-high.rgba", UINT64_C(66355200)},
    {"S720-CB.h264", UINT64_C(12593340)},
    {"S720-CB.x4au", UINT64_C(14496)},
    {"S720-CB.rgba", UINT64_C(29491200)},
    {"S1080-CB.h264", UINT64_C(25298609)},
    {"S1080-CB.x4au", UINT64_C(14496)},
    {"S1080-CB.rgba", UINT64_C(66355200)},
    {"S1080-HIGH.h264", UINT64_C(25295440)},
    {"S1080-HIGH.x4au", UINT64_C(14496)},
    {"S1080-HIGH.rgba", UINT64_C(66355200)},
#if X4_VIDEO_PROBE
    {"1080p60-high-15M.h264", UINT64_C(19415020)},
    {"1080p60-high-15M.x4au", UINT64_C(14496)},
    {"1080p60-high-15M.rgba", UINT64_C(66355200)},
#endif
};
static int sample_manifest_size(const char *leaf, uint64_t *bytes)
{
    for (size_t i = 0; i < sizeof(sample_manifest_sizes)/sizeof(sample_manifest_sizes[0]); ++i) {
        if (!strcmp(leaf, sample_manifest_sizes[i].leaf)) { *bytes = sample_manifest_sizes[i].bytes; return 1; }
    }
    *bytes = 0; return 0;
}

static FILE *descriptor_stream(int fd, int writable, const char *sample_leaf,
    uint64_t expected_size, X4ProbeFileStatus *out, X4ProbeFileCall opened)
{
    struct stat st; X4ProbeFileStatus current;
    if (checked_stat(fd, &st, &current)) goto error;
#if defined(X4_PROBE_FILES_TESTING)
    if (fixture_stat_enabled) {
        st.st_mode = (mode_t)fixture_mode; st.st_nlink = (nlink_t)fixture_nlink; st.st_size = (off_t)fixture_size;
    }
#endif
    (void)observe("fstat.st_mode (observed field)", st.st_mode & 07777, current.call_errno, 0);
    (void)observe("fstat.st_nlink (observed field)", (int64_t)st.st_nlink, current.call_errno, 0);
    if (sample_leaf) {
        int size_matches = st.st_size >= 0 && (uint64_t)st.st_size == expected_size;
        (void)observe("fstat.st_size (informational field)", (int64_t)st.st_size, current.call_errno, 0);
        (void)observe("manifest.bytes (informational field)", (int64_t)expected_size, 0, 0);
        (void)observe("stat_size_matches_manifest (informational field)", size_matches, 0, 0);
        if (!recording) {
            int saved_errno = errno;
            printf("XCloud4 probe: sample_stat leaf=%s mode=0%o nlink=%llu stat_size=%lld manifest_size=%llu stat_size_matches=%d informational=1\n",
                sample_leaf, (unsigned)(st.st_mode & 07777), (unsigned long long)st.st_nlink,
                (long long)st.st_size, (unsigned long long)expected_size, size_matches);
            errno = saved_errno;
        }
    }
    /* Reports retain exactly one link; FTP sample descriptors may report 0.
     * Neither sample permissions nor stat size establish content integrity. */
    if (!S_ISREG(st.st_mode) || (sample_leaf ? (st.st_nlink != 0 && st.st_nlink != 1) : st.st_nlink != 1)) {
        policy(&current, !S_ISREG(st.st_mode) ? "Archivo no regular" :
            sample_leaf ? "Clip con número de enlaces distinto de 0 o 1" : "Informe sin exactamente un enlace", EPERM); goto error;
    }
    /* No chmod. A restrictive umask may reduce0644: report that explicitly. */
    if (writable && (st.st_mode & 07777) != 0644) { policy(&current, "Modo efectivo del informe distinto de 0644", EPERM); goto error; }
    errno = 0; FILE *file = fdopen(fd, writable ? "wb" : "rb"); int saved = errno;
    X4ProbeFileCall converted = observe("fdopen", (int64_t)(uintptr_t)file, saved, 0);
    if (!file) { failed(&current, "Preparar FILE (fdopen)", converted); goto error; }
    status(out, writable ? "Informe exclusivo directamente en /data" : "Archivo absoluto en solo lectura", 0, 0, opened);
    return file;
error:
    (void)close_descriptor(fd, NULL); if (out) *out = current; errno = current.error_number; return NULL;
}
FILE *x4_probe_files_report_create(const char *path, X4ProbeFileStatus *out)
{
    if (!valid_report(path)) { policy(out, "Validar nombre fijo del informe", EINVAL); return NULL; }
    errno = 0;
    int fd = fault(X4_PROBE_FILE_FAULT_CREATE) ? -1 : open(path, O_WRONLY | O_CREAT | O_EXCL, 0644), saved = errno;
    X4ProbeFileCall opened = observe("open(report exclusive)", fd, saved, 0);
    if (fd < 0) { failed(out, "Crear informe exclusivo en /data", opened); return NULL; }
    /* O_CREAT|O_EXCL refuses existing names and final-component symlinks.
     * No openat, new directory, truncate, chmod, owner restriction or unlink. */
    return descriptor_stream(fd, 1, NULL, 0, out, opened);
}
FILE *x4_probe_files_report_open(const char *path, X4ProbeFileStatus *out)
{
    if (!valid_report(path)) { policy(out, "Validar nombre del informe para leer", EINVAL); return NULL; }
    errno = 0; int fd = open(path, O_RDONLY), saved = errno;
    X4ProbeFileCall opened = observe("open(report read)", fd, saved, 0);
    if (fd < 0) { failed(out, "Abrir informe absoluto", opened); return NULL; }
    return descriptor_stream(fd, 0, NULL, 0, out, opened);
}
FILE *x4_probe_files_sample_open(const char *path, X4ProbeFileStatus *out)
{
#if X4_VIDEO_PROBE && X4_PROBE_T26
    /* Only the new bank's ten compiled sample paths may use this subfolder.
     * Keep legacy probe/public path validation and manifests unchanged. */
    char t26_root[X4_PROBE_FILE_PATH_MAX];
    int t26_n=snprintf(t26_root,sizeof(t26_root),"%s/xcloud4-probe/t26",data_root());
    const char *t26_leaf=t26_n>0&&(size_t)t26_n<sizeof(t26_root)?fixed_leaf(path,t26_root):NULL;
    if(t26_leaf){
        uint64_t expected=0;
        bool found=false;
        for(unsigned i=0;i<X4_T26_SAMPLE_COUNT;++i){
            const X4T26GeneratedSample *sample=&x4_t26_generated_samples[i];
            const char *clip=strrchr(sample->clip_path,'/');
            const char *index=strrchr(sample->index_path,'/');
            if(clip&&!strcmp(t26_leaf,clip+1)){expected=sample->clip_bytes;found=true;break;}
            if(index&&!strcmp(t26_leaf,index+1)){expected=sample->index_bytes;found=true;break;}
        }
#if defined(X4_PROBE_FILES_TESTING) && !defined(X4_OPENORBIS)
        if(!fixture_manifest_required)found=true;
#endif
        if(!found){policy(out,"T26 clip fuera del manifiesto compilado",EINVAL);return NULL;}
        errno=0;int fd=open(path,O_RDONLY),saved=errno;
        X4ProbeFileCall opened=observe("open(T26 sample read)",fd,saved,0);
        if(fd<0){failed(out,"Abrir muestra T26",opened);return NULL;}
        return descriptor_stream(fd,0,t26_leaf,expected,out,opened);
    }
#endif
    char root[X4_PROBE_FILE_PATH_MAX];
    errno = 0; int n = snprintf(root, sizeof(root), "%s/xcloud4-probe", data_root()), saved = errno;
    (void)observe("snprintf(sample root)", n, saved, 0);
    const char *leaf = n > 0 && (size_t)n < sizeof(root) ? fixed_leaf(path, root) : NULL;
    if (!leaf) { policy(out, "Validar ruta absoluta del clip", EINVAL); return NULL; }
    uint64_t expected_size = 0;
    int size_available = sample_manifest_size(leaf, &expected_size);
#if defined(X4_PROBE_FILES_TESTING)
    if (fixture_manifest_required && !size_available) { policy(out, "Clip no incluido en el manifiesto fijo", EINVAL); return NULL; }
#endif
#ifdef X4_OPENORBIS
    int manifest_match = 0;
    for (unsigned i = 0; i < X4_PROBE_ALL_CASE_COUNT; ++i) {
        const X4ProbeCase *c = x4_probe_case_at(i);
        if (!strcmp(path, c->clip_path) || !strcmp(path, c->index_path) || !strcmp(path, c->reference_path)) { manifest_match = 1; break; }
    }
    if (!manifest_match || !size_available) { policy(out, "Clip no incluido en el manifiesto fijo", EINVAL); return NULL; }
#else
    (void)size_available;
#endif
    errno = 0; int fd = open(path, O_RDONLY), open_errno = errno;
    X4ProbeFileCall opened = observe("open(sample read)", fd, open_errno, 0);
    if (fd < 0) { failed(out, "Abrir clip absoluto", opened); return NULL; }
    /* FTP owns samples. Absolute read-only opens may follow links: fstat is
     * not a symlink check. Require regular + nlink 0/1 + fixed manifest path;
     * full SHA-256 before video use remains mandatory in source/reference. */
    return descriptor_stream(fd, 0, leaf, expected_size, out, opened);
}
int x4_probe_files_close(FILE *file, X4ProbeFileStatus *out)
{
    if (!file) return policy(out, "Cerrar FILE nulo", EINVAL);
    errno = 0; int raw = fclose(file), saved = errno;
    if (fault(X4_PROBE_FILE_FAULT_CLOSE)) { raw = -1; saved = errno; }
    X4ProbeFileCall call = observe("fclose", raw, saved, 0);
    if (raw && recording) recording->cleanup_failed = 1;
    return raw ? failed(out, "Cerrar archivo (fclose)", call) : status(out, "Cerrar archivo (fclose)", 0, 0, call);
}
int x4_probe_files_report_finish(FILE *file, X4ProbeFileFinish *finish)
{
    X4ProbeFileFinish local = {0}; if (!finish) finish = &local;
    if (!file) {
        policy(&finish->flush, "fflush de FILE nulo", EINVAL); policy(&finish->sync, "Sincronización no ejecutada", EINVAL);
        policy(&finish->close, "Cierre no ejecutado", EINVAL); return -1;
    }
    errno = 0; int raw = fflush(file), saved = errno;
    if (fault(X4_PROBE_FILE_FAULT_FLUSH)) { raw = -1; saved = errno; }
    X4ProbeFileCall call = observe("fflush", raw, saved, 0);
    if (raw) failed(&finish->flush, "Vaciar búfer (fflush)", call); else status(&finish->flush, "Vaciar búfer (fflush)", 0, 0, call);
    errno = 0; int fd = fileno(file), fd_errno = errno;
    X4ProbeFileCall descriptor = observe("fileno", fd, fd_errno, 0);
    if (fd < 0) failed(&finish->sync, "Obtener descriptor para sincronizar", descriptor);
    else {
        errno = 0;
#ifdef X4_OPENORBIS
        raw = sceKernelFsync(fd); saved = errno; call = observe("sceKernelFsync", raw, saved, raw);
        int mapped = EIO;
        if (((uint32_t)raw & 0xffff0000u) == 0x80020000u) mapped = (int)((uint32_t)raw & 0xffffu);
        else if (raw < 0 && raw >= -4095) mapped = -raw;
        status(&finish->sync, "Sincronizar (sceKernelFsync)", raw ? -1 : 0, mapped, call);
#else
        raw = fsync(fd); saved = errno;
        if (fault(X4_PROBE_FILE_FAULT_SYNC)) { raw = -1; saved = errno; }
        call = observe("fsync", raw, saved, 0);
        if (raw) failed(&finish->sync, "Sincronizar (fsync)", call); else status(&finish->sync, "Sincronizar (fsync)", 0, 0, call);
#endif
    }
    (void)x4_probe_files_close(file, &finish->close);
    const X4ProbeFileStatus *first = finish->flush.result ? &finish->flush : finish->sync.result ? &finish->sync : finish->close.result ? &finish->close : NULL;
    if (first) { errno = first->error_number; return -1; } return 0;
}
static void record_step(X4ProbeFileSelfcheck *r, unsigned step, const X4ProbeFileStatus *value, X4ProbeFilesProgress progress, void *context)
{ r->ran[step] = 1; r->step[step] = *value; if (progress) progress(r, context); }
/* ONLY exception: called exclusively from idle selfcheck. ELF gate validates
 * callers, aliases and this wrapper's single outgoing openat call/tail. */
__attribute__((noinline)) int x4_probe_files_openat_info(int directory, const char *name)
{ return openat(directory, name, O_RDONLY); }
int x4_probe_files_selfcheck(const char *report_path, const char *index_path,
    X4ProbeFileSelfcheck *r, X4ProbeFilesProgress progress, void *context)
{
    if (!r || !report_path || strnlen(report_path, X4_PROBE_FILE_PATH_MAX) >= X4_PROBE_FILE_PATH_MAX) { errno = EINVAL; return -1; }
    memset(r, 0, sizeof(*r)); strcpy(r->report_path, report_path); recording = r;
    X4ProbeFileStatus st, first = {0}; int rc = x4_probe_files_prepare(&st);
    record_step(r, X4_PROBE_FILES_PREPARE, &st, progress, context); if (rc) { first = st; goto done; }
    FILE *file = x4_probe_files_report_create(report_path, &st);
    record_step(r, X4_PROBE_FILES_CREATE, &st, progress, context); if (!file) { first = st; goto done; }
    static const char payload[] = "{\"schema\":\"x4-probe-file-selfcheck-v2\",\"purpose\":\"exclusive absolute write/read test, no decoder\",\"completed_steps\":\"screen and bounded call log; this payload does not assert later steps\"}\n";
    errno = 0; int short_write = fault(X4_PROBE_FILE_FAULT_SHORT_WRITE), short_errno = errno;
    size_t bytes = fault(X4_PROBE_FILE_FAULT_WRITE) ? 0 : fwrite(payload, 1, short_write ? 1 : sizeof(payload)-1, file); int saved = errno;
    if (short_write) saved = short_errno;
    X4ProbeFileCall call = observe("fwrite", (int64_t)bytes, saved, 0);
    if (bytes != sizeof(payload)-1) failed(&st, "Escribir informe de prueba", call); else status(&st, "Escribir informe de prueba", 0, 0, call);
    record_step(r, X4_PROBE_FILES_WRITE, &st, progress, context); if (st.result) first = st;
    X4ProbeFileFinish finished; rc = x4_probe_files_report_finish(file, &finished);
    record_step(r, X4_PROBE_FILES_FLUSH, &finished.flush, progress, context);
    record_step(r, X4_PROBE_FILES_SYNC, &finished.sync, progress, context); record_step(r, X4_PROBE_FILES_CLOSE, &finished.close, progress, context);
    if (!first.result && rc) first = finished.flush.result ? finished.flush : finished.sync.result ? finished.sync : finished.close;
    if (first.result) goto done;
    file = x4_probe_files_report_open(report_path, &st); record_step(r, X4_PROBE_FILES_REOPEN, &st, progress, context); if (!file) { first = st; goto done; }
    char actual[sizeof(payload)] = {0}; errno = 0; bytes = fread(actual, 1, sizeof(actual), file); saved = errno;
    call = observe("fread(report)", (int64_t)bytes, saved, 0); rc = bytes != sizeof(payload)-1;
    if (rc) failed(&st, "Leer informe de prueba", call); else status(&st, "Leer informe de prueba", 0, 0, call);
    record_step(r, X4_PROBE_FILES_READ, &st, progress, context); if (rc) first = st;
    rc = x4_probe_files_close(file, &st); record_step(r, X4_PROBE_FILES_READ_CLOSE, &st, progress, context);
    if (!first.result && rc) first = st;
    if (first.result) goto done;
    errno = 0; rc = memcmp(payload, actual, sizeof(payload)-1); saved = errno;
    call = observe("memcmp", rc, saved, 0); status(&st, "Comparar contenido exacto", rc ? -1 : 0, EIO, call);
    record_step(r, X4_PROBE_FILES_COMPARE, &st, progress, context); if (rc) { first = st; goto done; }
    file = x4_probe_files_sample_open(index_path, &st); record_step(r, X4_PROBE_FILES_INDEX_OPEN, &st, progress, context); if (!file) { first = st; goto done; }
    uint8_t header[96]; errno = 0; bytes = fread(header, 1, sizeof(header), file); saved = errno;
    call = observe("fread(index)", (int64_t)bytes, saved, 0); rc = bytes != sizeof(header);
    if (!rc) {
        errno = 0; int compared = memcmp(header, "X4AU1\0\0\0", 8), compare_errno = errno;
        X4ProbeFileCall comparison = observe("memcmp(index header)", compared, compare_errno, 0);
        if (compared) call = comparison;
        rc = compared != 0;
    }
    if (rc) failed(&st, "Leer cabecera del índice", call); else status(&st, "Leer cabecera del índice", 0, 0, call);
    record_step(r, X4_PROBE_FILES_INDEX_READ, &st, progress, context); if (rc) first = st;
    rc = x4_probe_files_close(file, &st); record_step(r, X4_PROBE_FILES_INDEX_CLOSE, &st, progress, context);
    if (!first.result && rc) first = st;
    if (first.result) goto done;
    char root[X4_PROBE_FILE_PATH_MAX]; errno = 0; int n = snprintf(root, sizeof(root), "%s/xcloud4-probe", data_root()); saved = errno;
    (void)observe("snprintf(info root)", n, saved, 0); const char *name = n > 0 && (size_t)n < sizeof(root) ? fixed_leaf(index_path, root) : NULL;
    char clip_name[96];
    size_t name_length = name ? strlen(name) : 0;
    if (!name || name_length < 5 || name_length >= sizeof(clip_name) || strcmp(name + name_length - 5, ".x4au"))
        policy(&st, "INFO: nombre de clip inválido", EINVAL);
    else {
        memcpy(clip_name, name, name_length + 1);
        memcpy(clip_name + name_length - 5, ".h264", 6);
        errno = 0; int directory = open(root, O_RDONLY | O_DIRECTORY); saved = errno; call = observe("open(info directory)", directory, saved, 0);
        if (directory < 0) failed(&st, "INFO: abrir carpeta de clips", call);
        else {
            errno = 0; int fd = fault(X4_PROBE_FILE_FAULT_OPENAT_INFO) ? -1 : x4_probe_files_openat_info(directory, clip_name); saved = errno;
            call = observe("openat", fd, saved, 0);
            if (fd < 0) failed(&st, "INFO: openat sin requisito de éxito", call); else status(&st, "INFO: openat sin requisito de éxito", 0, 0, call);
            if (fd >= 0 && close_descriptor(fd, NULL)) r->cleanup_failed = 1;
            if (close_descriptor(directory, NULL)) r->cleanup_failed = 1;
        }
    }
    record_step(r, X4_PROBE_FILES_OPENAT_INFO, &st, progress, context);
    if (r->cleanup_failed) { policy(&first, "Cierre inseguro del diagnóstico informativo", EIO); goto done; }
    r->passed = 1;
done:
    recording = NULL; if (progress) progress(r, context); errno = first.result ? first.error_number : 0; return first.result ? -1 : 0;
}
int x4_probe_files_shutdown(X4ProbeFileStatus *out)
{ return status(out, "Sin descriptor de carpeta retenido", 0, 0, (X4ProbeFileCall){"none", 0, 0, 0}); }
#if defined(X4_PROBE_FILES_TESTING)
int x4_probe_files_test_root(const char *root)
{
    if (!root || strncmp(root, "/tmp/", 5) || strstr(root, "..") || strlen(root) >= sizeof(fixture_root)) { errno = EINVAL; return -1; }
    strcpy(fixture_root, root); return 0;
}
void x4_probe_files_test_fault(int kind, int error_number)
{ injected_fault = kind; injected_errno = error_number ? error_number : EIO; }
void x4_probe_files_test_stat(int enabled, uint32_t mode, uint64_t nlink, int64_t size)
{ fixture_stat_enabled = enabled; fixture_mode = mode; fixture_nlink = nlink; fixture_size = size; }
void x4_probe_files_test_manifest(int enabled)
{ fixture_manifest_required = enabled; }
#endif
#endif
