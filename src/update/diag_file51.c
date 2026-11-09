/* SPDX-License-Identifier: GPL-3.0-only */
#include "diag_file51.h"
#include "update_io51.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_uint diag_sequence51;

int x4_diag51_open_fd(const char *path, char temporary[X4_DIAG51_PATH_CAPACITY], uint64_t stamp)
{
    if (mkdir("/data/xcloud4", 0777) && errno != EEXIST) return -1;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const unsigned seq = atomic_fetch_add_explicit(&diag_sequence51, 1, memory_order_relaxed);
        const int length = snprintf(temporary, X4_DIAG51_PATH_CAPACITY,
            "%s.%" PRIu64 ".%u.tmp", path, stamp, seq);
        if (length < 0 || length >= X4_DIAG51_PATH_CAPACITY) {
            errno = ENAMETOOLONG;
            return -1;
        }
        const int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return -1;
        }
        return fd;
    }
    return -1;
}

FILE *x4_diag51_open(const char *path, char temporary[X4_DIAG51_PATH_CAPACITY], uint64_t stamp)
{
    const int fd = x4_diag51_open_fd(path, temporary, stamp);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "w");
    if (file) return file;
    const int error = errno;
    (void)close(fd);
    (void)unlink(temporary);
    errno = error;
    return NULL;
}

bool x4_diag51_printf(FILE *file, const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (n < 0) return false;
    if ((size_t)n >= sizeof(text)) { errno = EOVERFLOW; return false; }
    return !x4_update51_write_all(file, text, (size_t)n);
}

bool x4_diag51_puts(FILE *file, const char *text)
{
    return !x4_update51_write_all(file, text, strlen(text));
}

static int finish_commit(const char *temporary, const char *path, int error)
{
    if (!error && rename(temporary, path)) error = errno ? errno : EIO;
    if (error) (void)unlink(temporary);
    return error;
}

int x4_diag51_commit(FILE *file, const char *temporary, const char *path, bool complete)
{
    int error = !complete ? (errno ? errno : EIO) : 0;
    if (fclose(file) && !error) error = errno ? errno : EIO;
    return finish_commit(temporary, path, error);
}

int x4_diag51_commit_fd(int fd, const char *temporary, const char *path, bool complete)
{
    int error = !complete ? (errno ? errno : EIO) : 0;
    if (close(fd) && !error) error = errno ? errno : EIO;
    return finish_commit(temporary, path, error);
}
