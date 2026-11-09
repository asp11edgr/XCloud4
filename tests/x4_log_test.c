/* SPDX-License-Identifier: GPL-3.0-only */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/x4_log.h"

enum { THREADS = 8, ITERATIONS = 20000, TRUNCATED_SIZE = 1023, EXTRA_WRITES = 5 };
static const char payload[] = "XCloud4 log fixture 0123456789 abcdefghijklmnop";
static const char ellipsis[] = "\xe2\x80\xa6\n";

extern int _IO_putc(int, FILE *);
extern int __wrap_fputc(int, FILE *);

/* Keep relocations to the actual symbols. glibc's extern inlines can
 * otherwise turn putchar into putc and vprintf into vfprintf. */
static int (*volatile putchar_symbol)(int) = putchar;
static int (*volatile vprintf_symbol)(const char *, va_list) = vprintf;
static int (*volatile putc_symbol)(int, FILE *) = putc;
static int (*volatile io_putc_symbol)(int, FILE *) = _IO_putc;

static _Atomic bool capture;
static _Atomic unsigned mutex_entries, writing, writes, overlaps, short_writes;
static _Thread_local bool pending_write_under_mutex;
static unsigned char seen[THREADS][ITERATIONS];
static pthread_barrier_t start_barrier;
static int report_fd = STDERR_FILENO;

#define CHECK(expression) do { \
    if (!(expression)) { \
        dprintf(report_fd, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        _Exit(EXIT_FAILURE); \
    } \
} while (0)

int __wrap_putc(int value, FILE *file)
{
    return __wrap_fputc(value, file);
}

int __wrap__IO_putc(int value, FILE *file)
{
    return __wrap_fputc(value, file);
}

void x4_log_test_mutex_entered(void)
{
    if (!atomic_load(&capture))
        return;
    CHECK(!pending_write_under_mutex);
    pending_write_under_mutex = true;
    atomic_fetch_add(&mutex_entries, 1);
}

ssize_t __real_write(int fd, const void *data, size_t size);
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (!atomic_load(&capture) || (fd != STDOUT_FILENO && fd != STDERR_FILENO))
        return __real_write(fd, data, size);

    CHECK(pending_write_under_mutex);
    pending_write_under_mutex = false;
    if (atomic_fetch_add(&writing, 1) != 0)
        atomic_fetch_add(&overlaps, 1);
    atomic_fetch_add(&writes, 1);
    ssize_t result = __real_write(fd, data, size);
    if (result < 0 || (size_t)result != size)
        atomic_fetch_add(&short_writes, 1);
    atomic_fetch_sub(&writing, 1);
    return result;
}

static int emit_vprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vprintf_symbol(format, args);
    va_end(args);
    return result;
}

static int emit_vfprintf(FILE *file, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(file, format, args);
    va_end(args);
    return result;
}

static bool stderr_line(unsigned sequence)
{
    return sequence % 8 >= 3 && sequence % 8 <= 6;
}

static void await_start(void)
{
    int rc = pthread_barrier_wait(&start_barrier);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *worker(void *arg)
{
    unsigned worker_id = *(const unsigned *)arg;
    await_start();
    for (unsigned sequence = 0; sequence < ITERATIONS; ++sequence) {
        char line[100];
        int len = snprintf(line, sizeof(line), "t%02u:%05u:%s\n", worker_id, sequence, payload);
        CHECK(len > 0 && (size_t)len < sizeof(line));
        if (sequence == ITERATIONS - 1) {
            char long_text[2048];
            memset(long_text, 'x', sizeof(long_text) - 1);
            long_text[sizeof(long_text) - 1] = '\0';
            FILE *file = stderr_line(sequence) ? stderr : stdout;
            CHECK(fprintf(file, "t%02u:%05u:%s\n", worker_id, sequence, long_text) >= TRUNCATED_SIZE);
            continue;
        }
        switch (sequence % 8) {
        case 0:
            CHECK(printf("%s", line) == len);
            break;
        case 1:
            CHECK(emit_vprintf("%s", line) == len);
            break;
        case 2:
            line[len - 1] = '\0';
            CHECK(puts(line) >= 0);
            break;
        case 3:
            CHECK(fprintf(stderr, "%s", line) == len);
            break;
        case 4:
            CHECK(emit_vfprintf(stderr, "%s", line) == len);
            break;
        case 5:
            CHECK(fputs(line, stderr) >= 0);
            break;
        case 6:
            CHECK(fwrite(line, 1, (size_t)len, stderr) == (size_t)len);
            break;
        case 7:
            CHECK(x4_log_printf("%s", line) == len);
            break;
        }
    }
    return NULL;
}

static void verify_regular_file(void)
{
    char *contents = NULL;
    size_t size = 0;
    FILE *file = open_memstream(&contents, &size);
    CHECK(file != NULL);
    CHECK(fprintf(file, "%s", "ab") == 2);
    CHECK(emit_vfprintf(file, "%s", "cd") == 2);
    CHECK(fputs("ef", file) >= 0);
    CHECK(fputc('g', file) == 'g');
    CHECK(fwrite("hijk", 2, 2, file) == 2);
    CHECK(fclose(file) == 0);
    CHECK(size == sizeof("abcdefghijk") - 1 && !strcmp(contents, "abcdefghijk"));
    free(contents);
}

static unsigned read_unsigned(const char *p, size_t digits)
{
    unsigned value = 0;
    for (size_t i = 0; i < digits; ++i) {
        CHECK(p[i] >= '0' && p[i] <= '9');
        value = value * 10 + (unsigned)(p[i] - '0');
    }
    return value;
}

static unsigned verify_capture(const char *path, bool is_stderr, const char *error_line)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    char *line = NULL;
    size_t cap = 0;
    CHECK(getline(&line, &cap, file) == 2);
    CHECK(!strcmp(line, is_stderr ? "#\n" : "@\n"));
    if (is_stderr) {
        CHECK(getline(&line, &cap, file) == (ssize_t)strlen(error_line));
        CHECK(!strcmp(line, error_line));
    }

    unsigned truncated = 0;
    ssize_t length;
    while ((length = getline(&line, &cap, file)) >= 0) {
        CHECK(length >= 11 && line[0] == 't' && line[3] == ':' && line[9] == ':');
        unsigned worker_id = read_unsigned(line + 1, 2);
        unsigned sequence = read_unsigned(line + 4, 5);
        CHECK(worker_id < THREADS && sequence < ITERATIONS && is_stderr == stderr_line(sequence));
        CHECK(seen[worker_id][sequence]++ == 0);
        if (sequence == ITERATIONS - 1) {
            CHECK(length == TRUNCATED_SIZE);
            size_t ending = (size_t)length - (sizeof(ellipsis) - 1);
            CHECK(!memcmp(line + ending, ellipsis, sizeof(ellipsis) - 1));
            for (size_t i = 10; i < ending; ++i)
                CHECK(line[i] == 'x');
            ++truncated;
        } else {
            CHECK((size_t)length == 10 + sizeof(payload));
            CHECK(!memcmp(line + 10, payload, sizeof(payload) - 1));
            CHECK(line[length - 1] == '\n');
        }
    }
    CHECK(feof(file) && fclose(file) == 0);
    free(line);
    return truncated;
}

static void redirect_capture(int fd, const char *path)
{
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(file >= 0);
    CHECK(dup2(file, fd) == fd);
    CHECK(close(file) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    verify_regular_file();

    char error_line[200];
    int error_length = snprintf(error_line, sizeof(error_line), "x4-log-test: %s\n", strerror(EINVAL));
    CHECK(error_length > 0 && (size_t)error_length < sizeof(error_line));

    int saved_stdout = dup(STDOUT_FILENO);
    int saved_stderr = dup(STDERR_FILENO);
    CHECK(saved_stdout >= 0 && saved_stderr >= 0);
    report_fd = saved_stderr;
    CHECK(setvbuf(stdout, NULL, _IONBF, 0) == 0);
    CHECK(setvbuf(stderr, NULL, _IONBF, 0) == 0);
    redirect_capture(STDOUT_FILENO, argv[1]);
    redirect_capture(STDERR_FILENO, argv[2]);
    atomic_store(&capture, true);
    CHECK(putchar_symbol('@') == '@');
    CHECK(putc_symbol('\n', stdout) == '\n');
    CHECK(io_putc_symbol('#', stderr) == '#');
    CHECK(fputc('\n', stderr) == '\n');
    errno = EINVAL;
    perror("x4-log-test");

    unsigned ids[THREADS];
    pthread_t threads[THREADS];
    CHECK(pthread_barrier_init(&start_barrier, NULL, THREADS + 1) == 0);
    for (unsigned i = 0; i < THREADS; ++i) {
        ids[i] = i;
        CHECK(pthread_create(&threads[i], NULL, worker, &ids[i]) == 0);
    }
    await_start();
    for (unsigned i = 0; i < THREADS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&start_barrier) == 0);
    atomic_store(&capture, false);
    CHECK(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    CHECK(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);

    unsigned expected = THREADS * ITERATIONS + EXTRA_WRITES;
    unsigned actual_writes = atomic_load(&writes);
    unsigned actual_entries = atomic_load(&mutex_entries);
    if (actual_writes != expected || actual_entries != expected)
        dprintf(report_fd, "expected=%u writes=%u mutex_entries=%u\n",
                expected, actual_writes, actual_entries);
    CHECK(actual_writes == expected && actual_entries == expected);
    CHECK(atomic_load(&overlaps) == 0 && atomic_load(&short_writes) == 0);
    unsigned truncated_stdout = verify_capture(argv[1], false, error_line);
    unsigned truncated_stderr = verify_capture(argv[2], true, error_line);
    CHECK(truncated_stdout + truncated_stderr == THREADS);
    for (unsigned i = 0; i < THREADS; ++i)
        for (unsigned j = 0; j < ITERATIONS; ++j)
            CHECK(seen[i][j] == 1);

    CHECK(close(saved_stdout) == 0);
    CHECK(close(saved_stderr) == 0);
    printf("PASS x4_log: %u complete lines, %u writes, %u mutex entries, 0 overlaps/short writes; "
           "%u truncated to %u payload bytes + NUL; nonstandard FILE unchanged\n",
           (unsigned)THREADS * ITERATIONS, actual_writes, actual_entries,
           (unsigned)THREADS, (unsigned)TRUNCATED_SIZE);
    return EXIT_SUCCESS;
}
