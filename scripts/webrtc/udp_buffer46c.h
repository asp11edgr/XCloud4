/* SPDX-License-Identifier: MPL-2.0 */
#ifndef X4_UDP_BUFFER46C_H
#define X4_UDP_BUFFER46C_H

typedef struct x4_udp_buffer_info {
    int rcvbuf;
    int sndbuf;
    int requested;
    int attempts;
} x4_udp_buffer_info;

#ifndef __cplusplus
#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef X4_OPENORBIS
_Static_assert(SOL_SOCKET == 0xffff && SO_SNDBUF == 0x1001 && SO_RCVBUF == 0x1002 &&
               sizeof(int) == 4 && sizeof(socklen_t) == 4, "unexpected OpenOrbis socket ABI");
#endif

static inline int x4_udp_request_buffer(int fd, int option, const int *values,
                                        size_t count, int *attempts)
{
    *attempts = 0;
    for (size_t i = 0; i < count; ++i) {
        ++*attempts;
        if (setsockopt(fd, SOL_SOCKET, option, &values[i], sizeof(values[i])) == 0)
            return values[i];
    }
    return 0;
}

static inline int x4_udp_effective_buffer(int fd, int option)
{
    int value = 0;
    socklen_t length = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, option, &value, &length) != 0 ||
        length != sizeof(value) || value <= 0)
        return 0;
    return value;
}

static inline x4_udp_buffer_info x4_udp_configure_buffers(int fd)
{
    static const int receive_sizes[] = {2097152, 1048576, 524288, 262144, 131072};
    static const int send_sizes[] = {524288, 262144, 131072};
    const int saved_errno = errno;
    x4_udp_buffer_info info = {0};
    int send_attempts;
    info.requested = x4_udp_request_buffer(fd, SO_RCVBUF, receive_sizes,
        sizeof(receive_sizes) / sizeof(receive_sizes[0]), &info.attempts);
    (void)x4_udp_request_buffer(fd, SO_SNDBUF, send_sizes,
        sizeof(send_sizes) / sizeof(send_sizes[0]), &send_attempts);
    info.rcvbuf = x4_udp_effective_buffer(fd, SO_RCVBUF);
    info.sndbuf = x4_udp_effective_buffer(fd, SO_SNDBUF);
    errno = saved_errno;
    return info;
}

enum { X4_UDP_EMPTY, X4_UDP_WRITING, X4_UDP_READY };

typedef struct x4_udp_buffer_sample {
    atomic_uint state;
    x4_udp_buffer_info info;
} x4_udp_buffer_sample;

static inline void x4_udp_buffer_sample_init(x4_udp_buffer_sample *sample)
{
    atomic_init(&sample->state, X4_UDP_EMPTY);
}

static inline void x4_udp_buffer_sample_publish(x4_udp_buffer_sample *sample,
                                                 const x4_udp_buffer_info *info)
{
    unsigned empty = X4_UDP_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(&sample->state, &empty, X4_UDP_WRITING,
                                                  memory_order_relaxed, memory_order_relaxed))
        return;
    sample->info = *info;
    atomic_store_explicit(&sample->state, X4_UDP_READY, memory_order_release);
}

static inline int x4_udp_buffer_sample_read(const x4_udp_buffer_sample *sample,
                                            x4_udp_buffer_info *info)
{
    if (atomic_load_explicit(&sample->state, memory_order_acquire) != X4_UDP_READY)
        return -1;
    *info = sample->info;
    return 0;
}
#endif
#endif
