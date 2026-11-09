/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "update_io51.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int io_failure(int error)
{
    errno = error ? error : EIO;
    return -errno;
}

static off_t seek_fd(int fd, int whence)
{
    off_t result;
    do result = lseek(fd, 0, whence); while (result < 0 && errno == EINTR);
    return result;
}

FILE *x4_update51_open_regular(const char *path, uint64_t *size, const char **failed_function)
{
    /* The path preflight rejects an existing FIFO before a blocking open.
     * The descriptor itself is checked again and links are not followed. */
    const char *step = "stat";
    struct stat st;
    int fd = -1;
    if (stat(path, &st)) goto failed;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto failed; }
    step = "open";
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) goto failed;
    step = "fstat";
    if (fstat(fd, &st)) goto failed;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto failed; }
    step = "lseek";
    const off_t end = seek_fd(fd, SEEK_END);
    if (end < 0 || seek_fd(fd, SEEK_SET) < 0) goto failed;
    step = "fdopen";
    FILE *file = fdopen(fd, "rb");
    if (!file) goto failed;
    if (failed_function) *failed_function = NULL;
    *size = (uint64_t)end;
    return file;

failed:
    {
        const int error = errno;
        if (fd >= 0) (void)close(fd);
        if (failed_function) *failed_function = step;
        (void)io_failure(error);
        return NULL;
    }
}

int x4_update51_file_size(FILE *file, uint64_t *size)
{
    /* POSIX input fflush synchronizes a seekable stream before using its fd. */
    const int fd = fileno(file);
    if (fd < 0 || fflush(file)) return io_failure(errno);
    const off_t end = seek_fd(fd, SEEK_END);
    if (end < 0 || fseek(file, 0, SEEK_SET)) return io_failure(errno);
    *size = (uint64_t)end;
    return 0;
}

int x4_update51_read_all(FILE *file, void *bytes, size_t count)
{
    unsigned char *at = bytes;
    while (count) {
        errno = 0;
        const size_t read_bytes = fread(at, 1, count, file);
        at += read_bytes;
        count -= read_bytes;
        if (ferror(file)) {
            if (errno != EINTR) return io_failure(errno);
            clearerr(file);
        } else if (!read_bytes) return io_failure(EIO);
    }
    return 0;
}

int x4_update51_write_all(FILE *file, const void *bytes, size_t count)
{
    const unsigned char *at = bytes;
    while (count) {
        errno = 0;
        const size_t written = fwrite(at, 1, count, file);
        at += written;
        count -= written;
        if (ferror(file)) {
            if (errno != EINTR) return io_failure(errno ? errno : written ? EIO : ENOSPC);
            clearerr(file);
        } else if (!written) return io_failure(errno ? errno : ENOSPC);
    }
    return 0;
}

int x4_update51_expect_eof(FILE *file)
{
    for (;;) {
        errno = 0;
        if (fgetc(file) != EOF) return io_failure(EIO);
        if (!ferror(file)) return 0;
        if (errno != EINTR) return io_failure(errno);
        clearerr(file);
    }
}
