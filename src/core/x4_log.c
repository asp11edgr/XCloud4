/* SPDX-License-Identifier: GPL-3.0-only */
#include "x4_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>

static OrbisPthreadMutex log_mutex;
static pthread_once_t log_once = PTHREAD_ONCE_INIT;
static int log_mutex_init_result;

static void init_log_mutex(void)
{
    log_mutex_init_result = scePthreadMutexInit(&log_mutex, NULL, "x4-klog");
}

static int lock_log(void)
{
    /* Use XCloud4's pthread_once ABI bridge, not an SDK object passed
     * directly to scePthreadOnce. */
    int rc = pthread_once(&log_once, init_log_mutex);
    if (!rc) rc = log_mutex_init_result;
    if (!rc) rc = scePthreadMutexLock(&log_mutex);
    return rc < 0 ? (int)((uint32_t)rc & 0xffffu) : rc;
}

static void unlock_log(void)
{
    (void)scePthreadMutexUnlock(&log_mutex);
}
#else
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static int lock_log(void)
{
    return pthread_mutex_lock(&log_mutex);
}

static void unlock_log(void)
{
    (void)pthread_mutex_unlock(&log_mutex);
}
#endif

enum { LOG_CAP = 1024, LOG_MAX = LOG_CAP - 1 };
static const char truncated[] = "\xe2\x80\xa6\n";
enum { LOG_PREFIX = LOG_MAX - (sizeof truncated - 1) };

static int write_log(int fd, const void *data, size_t size)
{
    if (!size) return 0;
    int saved_errno = errno;
    int rc = lock_log();
    if (rc) {
        errno = rc;
        return -1;
    }

#ifdef X4_LOG_TEST
    extern void x4_log_test_mutex_entered(void);
    x4_log_test_mutex_entered();
#endif
    ssize_t written = write(fd, data, size);
    int write_errno = errno;
    unlock_log();

    if (written < 0) {
        errno = write_errno;
        return -1;
    }
    if ((size_t)written != size) {
        /* Never issue a second write for the same diagnostic. */
        errno = EIO;
        return -1;
    }
    errno = saved_errno;
    return (int)written;
}

static int log_vformat(int fd, const char *format, va_list args)
{
    char line[LOG_CAP];
    int size = vsnprintf(line, sizeof line, format, args);
    if (size < 0) return size;
    if (size > LOG_MAX) {
        memcpy(line + LOG_PREFIX, truncated, sizeof truncated);
        size = LOG_MAX;
    }
    return write_log(fd, line, (size_t)size);
}

static int log_format(int fd, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int rc = log_vformat(fd, format, args);
    va_end(args);
    return rc;
}

int x4_log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int rc = log_vformat(STDOUT_FILENO, format, args);
    va_end(args);
    return rc;
}

static int log_bytes(int fd, const void *data, size_t size, int newline)
{
    char line[LOG_CAP];
    if (size > LOG_MAX - (unsigned)newline) {
        memcpy(line, data, LOG_PREFIX);
        memcpy(line + LOG_PREFIX, truncated, sizeof truncated);
        return write_log(fd, line, LOG_MAX) < 0 ? -1 : LOG_PREFIX;
    }

    if (size) memcpy(line, data, size);
    if (newline) line[size++] = '\n';
    line[size] = '\0';
    return write_log(fd, line, size);
}

static int log_fd(FILE *stream)
{
    if (stream == stdout) return STDOUT_FILENO;
    if (stream == stderr) return STDERR_FILENO;
    return -1;
}

extern int __real_vfprintf(FILE *, const char *, va_list);
extern int __real_fputs(const char *, FILE *);
extern int __real_fputc(int, FILE *);
extern size_t __real_fwrite(const void *, size_t, size_t, FILE *);

int __wrap_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int rc = log_vformat(STDOUT_FILENO, format, args);
    va_end(args);
    return rc;
}

int __wrap_vprintf(const char *format, va_list args)
{
    return log_vformat(STDOUT_FILENO, format, args);
}

int __wrap_vfprintf(FILE *stream, const char *format, va_list args)
{
    int fd = log_fd(stream);
    /* Includes musl's vsnprintf FILE, which is local to its stack. */
    return fd < 0 ? __real_vfprintf(stream, format, args) : log_vformat(fd, format, args);
}

int __wrap_fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int rc = __wrap_vfprintf(stream, format, args);
    va_end(args);
    return rc;
}

int __wrap_puts(const char *text)
{
    return log_bytes(STDOUT_FILENO, text, strnlen(text, LOG_CAP), 1);
}

int __wrap_fputs(const char *text, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_fputs(text, stream) : log_bytes(fd, text, strnlen(text, LOG_CAP), 0);
}

static int log_character(int fd, int value)
{
    unsigned char byte = (unsigned char)value;
    return write_log(fd, &byte, 1) < 0 ? EOF : byte;
}

int __wrap_putchar(int value)
{
    return log_character(STDOUT_FILENO, value);
}

int __wrap_fputc(int value, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_fputc(value, stream) : log_character(fd, value);
}

static size_t log_items(int fd, const void *data, size_t size, size_t count)
{
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) {
        errno = EOVERFLOW;
        return 0;
    }
    int written = log_bytes(fd, data, size * count, 0);
    return written < 0 ? 0 : (size_t)written / size;
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_fwrite(data, size, count, stream) : log_items(fd, data, size, count);
}

void __wrap_perror(const char *text)
{
    int saved_errno = errno;
    char description[256];
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char *message = strerror_r(saved_errno, description, sizeof description);
#else
    const char *message = strerror_r(saved_errno, description, sizeof description) ? NULL : description;
#endif
    if (!message) message = "Unknown error";
    if (text && *text) (void)log_format(STDERR_FILENO, "%s: %s\n", text, message);
    else (void)log_format(STDERR_FILENO, "%s\n", message);
    errno = saved_errno;
}

#ifdef X4_OPENORBIS
/* These entry points are present in the linked SDK's libc.a. Preserve the
 * exact libc entry point for other FILEs, including its locking semantics. */
extern int __real_putc(int, FILE *);
extern int __real_putc_unlocked(int, FILE *);
extern int __real_fputc_unlocked(int, FILE *);
extern int __real_fputs_unlocked(const char *, FILE *);
extern size_t __real_fwrite_unlocked(const void *, size_t, size_t, FILE *);

int __wrap_putc(int value, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_putc(value, stream) : log_character(fd, value);
}

int __wrap_putc_unlocked(int value, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_putc_unlocked(value, stream) : log_character(fd, value);
}

int __wrap_fputc_unlocked(int value, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_fputc_unlocked(value, stream) : log_character(fd, value);
}

int __wrap_putchar_unlocked(int value)
{
    return log_character(STDOUT_FILENO, value);
}

int __wrap_fputs_unlocked(const char *text, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_fputs_unlocked(text, stream) : log_bytes(fd, text, strnlen(text, LOG_CAP), 0);
}

size_t __wrap_fwrite_unlocked(const void *data, size_t size, size_t count, FILE *stream)
{
    int fd = log_fd(stream);
    return fd < 0 ? __real_fwrite_unlocked(data, size, count, stream) : log_items(fd, data, size, count);
}
#endif
