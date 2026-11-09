/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
int x4_native_entropy_prepare(void);
int x4_native_net_prepare(void);
int x4_native_random(void *data, size_t size);
void x4_native_random_or_exit(void *data, size_t size);
void x4_native_rtc_diagnostic(int event, int value);
int x4_native_socket_nonblock(int descriptor);

#include <stdint.h>
struct in_addr;
/* Caller first prepares NetInit/x4_native_net_prepare. Deadline and mutex
 * waits share the absolute monotonic budget; cancel_error is supplied by the
 * caller (HTTPS passes its existing cancellation code). Resolver errors keep
 * their native codes. No host, address or request content is logged here. */
int x4_native_resolve_ipv4_deadline(const char *host, struct in_addr *out,
                                  uint64_t absolute_deadline_us,
                                  const _Atomic int *cancel, int cancel_error);
