/* SPDX-License-Identifier: GPL-3.0-only */
#include "manual51.h"
#include "update_io51.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int io_error(void) { return -(errno ? errno : EIO); }

int x4_update51_auth_save(const X4UpdateSigned51 *ticket, bool *created)
{
    *created = false;
    if (!ticket->json_size || ticket->json_size > sizeof(ticket->json) ||
        !ticket->signature_size || ticket->signature_size > sizeof(ticket->signature)) return -EINVAL;
    const unsigned char header[] = {'X','4','A',1, (unsigned char)(ticket->json_size >> 8),
        (unsigned char)ticket->json_size, (unsigned char)ticket->signature_size};
    const int fd = open(X4_UPDATE51_AUTH_PATH, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return io_error();
    *created = true;
    FILE *file = fdopen(fd, "wb");
    if (!file) { const int rc = io_error(); (void)close(fd); return rc; }
    int rc = x4_update51_write_all(file, header, sizeof(header));
    if (!rc) rc = x4_update51_write_all(file, ticket->json, ticket->json_size);
    if (!rc) rc = x4_update51_write_all(file, ticket->signature, ticket->signature_size);
    if (fclose(file) && !rc) rc = io_error();
    return rc;
}

int x4_update51_auth_read(X4UpdateSigned51 *ticket)
{
    uint64_t size = 0;
    FILE *file = x4_update51_open_regular(X4_UPDATE51_AUTH_PATH, &size, NULL);
    if (!file) return io_error();
    unsigned char header[7] = {0};
    int rc = x4_update51_read_all(file, header, sizeof(header));
    if (!rc && memcmp(header, "X4A\1", 4)) rc = -EINVAL;
    ticket->json_size = (size_t)header[4] * 256u + header[5];
    ticket->signature_size = header[6];
    if (!rc && (!ticket->json_size || ticket->json_size > sizeof(ticket->json) ||
        !ticket->signature_size || ticket->signature_size > sizeof(ticket->signature) ||
        size != sizeof(header) + ticket->json_size + ticket->signature_size)) rc = -EINVAL;
    if (!rc) rc = x4_update51_read_all(file, ticket->json, ticket->json_size);
    if (!rc) rc = x4_update51_read_all(file, ticket->signature, ticket->signature_size);
    if (!rc) rc = x4_update51_expect_eof(file);
    if (fclose(file) && !rc) rc = io_error();
    return rc;
}

static int verify_published(const char *path, const X4UpdateManifest *manifest, const char *title)
{
    uint64_t size = 0;
    FILE *file = x4_update51_open_regular(path, &size, NULL);
    if (!file) return errno == ELOOP || errno == EINVAL ? -EEXIST : io_error();
    const X4UpdateVerifyResult verified = size == manifest->pkg_size ?
        x4_update_pkg_verify(file, manifest, title) : X4_UPDATE_VERIFY_HASH;
    const int close_rc = fclose(file);
    if (verified == X4_UPDATE_VERIFY_IO || close_rc) return -EIO;
    return verified == X4_UPDATE_VERIFY_OK ? 0 : -EEXIST;
}

static bool same_file(const char *path, const struct stat *expected)
{
    struct stat current;
    return !stat(path, &current) && S_ISREG(current.st_mode) &&
        current.st_dev == expected->st_dev && current.st_ino == expected->st_ino;
}

static bool reserved_empty(const char *path, int fd, const struct stat *reservation)
{
    return same_file(path, reservation) && lseek(fd, 0, SEEK_END) == 0 &&
        lseek(fd, 0, SEEK_SET) == 0;
}

/* Remove only a path that still names the file we created. */
static void remove_owned(const char *path, const struct stat *created)
{
    if (same_file(path, created)) (void)unlink(path);
}

static int discard_staged(const char *staged)
{
    return !unlink(staged) || errno == ENOENT ? 0 : io_error();
}

static int copy_new(FILE *input, int fd, const X4UpdateManifest *manifest, const char *title,
    const char *path, const struct stat *owned)
{
    FILE *output = fdopen(fd, "wb");
    int rc = output ? 0 : io_error();
    unsigned char block[4096];
    for (uint32_t left = manifest->pkg_size; !rc && left;) {
        const size_t n = left < sizeof(block) ? left : sizeof(block);
        rc = x4_update51_read_all(input, block, n);
        if (!rc) rc = x4_update51_write_all(output, block, n);
        left -= (uint32_t)n;
    }
    if (!rc) rc = x4_update51_expect_eof(input);
    if (output) {
        if (fclose(output) && !rc) rc = io_error();
    } else (void)close(fd);
    if (!rc) {
        rc = verify_published(path, manifest, title);
        if (rc == -EEXIST) rc = -EIO;
    }
    if (rc) remove_owned(path, owned);
    return rc;
}

int x4_update51_manual_publish(const char *staged, const X4UpdateManifest *manifest,
    const char *title, const char *path)
{
    if (mkdir(X4_UPDATE51_MANUAL_DIR, 0777) && errno != EEXIST) return io_error();
    const int directory = open(X4_UPDATE51_MANUAL_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) return io_error();
    if (close(directory)) return io_error();
    const int existing = verify_published(path, manifest, title);
    if (existing != -ENOENT) return existing ? existing : discard_staged(staged);
    uint64_t size = 0;
    FILE *input = x4_update51_open_regular(staged, &size, NULL);
    if (!input) return io_error();
    struct stat source;
    int rc = size != manifest->pkg_size ? -EIO : fstat(fileno(input), &source) ? io_error() : 0;
    bool discard = false;
    if (!rc) {
        /* rename has replacement semantics on PS4. Reserve the destination
         * exclusively so the move can replace only our own empty file. */
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
        if (fd < 0 && errno == EEXIST) {
            rc = verify_published(path, manifest, title);
            if (rc == -ENOENT) rc = -EEXIST;
            discard = !rc;
        } else if (fd < 0) {
            rc = io_error();
        } else {
            struct stat reservation;
            if (fstat(fd, &reservation)) {
                rc = io_error();
                (void)close(fd);
            } else if (!reserved_empty(path, fd, &reservation)) {
                (void)close(fd);
                rc = -EEXIST;
            } else if (!same_file(staged, &source)) {
                (void)close(fd);
                remove_owned(path, &reservation);
                rc = -EIO;
            } else if (!rename(staged, path)) {
                (void)close(fd);
                rc = verify_published(path, manifest, title);
                if (rc) {
                    remove_owned(path, &source);
                    if (rc == -EEXIST) rc = -EIO;
                }
            } else if (!reserved_empty(path, fd, &reservation)) {
                (void)close(fd);
                rc = -EEXIST;
            } else {
                rc = copy_new(input, fd, manifest, title, path, &reservation);
                discard = !rc;
            }
        }
    }
    if (fclose(input) && !rc) rc = io_error();
    if (!rc && discard) rc = same_file(staged, &source) ? discard_staged(staged) : -EIO;
    return rc;
}
