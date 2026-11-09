/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

enum { KIB = 1024, MIB = 1024 * KIB, FD = 17 };

static int limits[2], effective[2], set_values[2][5], set_count[2], get_count[2];
static int failed_get, short_get;

static int option_index(int fd, int level, int option)
{
    assert(fd == FD && level == SOL_SOCKET);
    assert(option == SO_RCVBUF || option == SO_SNDBUF);
    return option == SO_SNDBUF ? 1 : 0;
}

static int simulated_setsockopt(int fd, int level, int option, const void *value, socklen_t size)
{
    const int index = option_index(fd, level, option);
    assert(size == sizeof(int) && set_count[index] < 5);
    const int request = *(const int *)value;
    set_values[index][set_count[index]++] = request;
    if (request > limits[index]) {
        errno = ENOBUFS;
        return -1;
    }
    /* An effective size different from the accepted request. */
    effective[index] = request + KIB;
    return 0;
}

static int simulated_getsockopt(int fd, int level, int option, void *value, socklen_t *size)
{
    const int index = option_index(fd, level, option);
    assert(*size == sizeof(int));
    ++get_count[index];
    if (index == failed_get) {
        errno = EINVAL;
        return -1;
    }
    *(int *)value = effective[index];
    if (index == short_get)
        *size = sizeof(short);
    return 0;
}

#define setsockopt simulated_setsockopt
#define getsockopt simulated_getsockopt
#include "../scripts/webrtc/udp_buffer46c.h"
#undef getsockopt
#undef setsockopt

static void check_limit(int limit, int rcv_request, int snd_request, int rcv_attempts,
                        int snd_attempts, bool unreadable)
{
    static const int receive[] = {2 * MIB, MIB, 512 * KIB, 256 * KIB, 128 * KIB};
    static const int send[] = {512 * KIB, 256 * KIB, 128 * KIB};
    limits[0] = limits[1] = limit;
    effective[0] = 64 * KIB;
    effective[1] = 48 * KIB;
    memset(set_count, 0, sizeof(set_count));
    memset(get_count, 0, sizeof(get_count));
    failed_get = unreadable ? 0 : -1;
    short_get = unreadable ? 1 : -1;
    errno = EINTR;
    const x4_udp_buffer_info result = x4_udp_configure_buffers(FD);
    assert(errno == EINTR);
    assert(result.requested == rcv_request && result.attempts == rcv_attempts);
    assert(set_count[0] == rcv_attempts && set_count[1] == snd_attempts);
    assert(!memcmp(receive, set_values[0], rcv_attempts * sizeof(int)));
    assert(!memcmp(send, set_values[1], snd_attempts * sizeof(int)));
    assert(get_count[0] == 1 && get_count[1] == 1);
    assert(result.rcvbuf == (unreadable ? 0 : (rcv_request ? rcv_request + KIB : 64 * KIB)));
    assert(result.sndbuf == (unreadable ? 0 : (snd_request ? snd_request + KIB : 48 * KIB)));
    printf("limit=%d rcvbuf=%d sndbuf=%d requested=%d rcv_attempts=%d snd_attempts=%d\n",
           limit, result.rcvbuf, result.sndbuf, result.requested, result.attempts, snd_attempts);
}

static x4_udp_buffer_sample sample;

static void *writer(void *argument)
{
    x4_udp_buffer_sample_publish(&sample, argument);
    return NULL;
}

static void *reader(void *unused)
{
    (void)unused;
    x4_udp_buffer_info info;
    while (x4_udp_buffer_sample_read(&sample, &info) != 0)
        sched_yield();
    assert((info.rcvbuf == 1 || info.rcvbuf == 2) && info.sndbuf == info.rcvbuf * 10 &&
           info.requested == info.rcvbuf * 100 && info.attempts == info.rcvbuf);
    return NULL;
}

static void check_publication(void)
{
    for (unsigned i = 0; i < 100; ++i) {
        pthread_t threads[3];
        x4_udp_buffer_info first = {1, 10, 100, 1}, second = {2, 20, 200, 2}, empty;
        x4_udp_buffer_sample_init(&sample);
        assert(x4_udp_buffer_sample_read(&sample, &empty) == -1);
        assert(pthread_create(&threads[0], NULL, reader, NULL) == 0);
        assert(pthread_create(&threads[1], NULL, writer, &first) == 0);
        assert(pthread_create(&threads[2], NULL, writer, &second) == 0);
        for (unsigned j = 0; j < 3; ++j)
            assert(pthread_join(threads[j], NULL) == 0);
    }
}

int main(void)
{
    check_limit(64 * KIB, 0, 0, 5, 3, false);
    check_limit(256 * KIB, 256 * KIB, 256 * KIB, 4, 2, false);
    check_limit(MIB, MIB, 512 * KIB, 2, 1, false);
    check_limit(2 * MIB, 2 * MIB, 512 * KIB, 1, 1, false);
    check_limit(2 * MIB, 2 * MIB, 512 * KIB, 1, 1, true);
    check_publication();
    puts("UDP buffers: ordered fallbacks, effective values and per-peer publication PASS");
    return 0;
}
