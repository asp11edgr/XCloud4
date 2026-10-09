/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
int x4_native_entropy_prepare(void);
int x4_native_net_prepare(void);
int x4_native_random(void *data, size_t size);
void x4_native_random_or_exit(void *data, size_t size);
void x4_native_rtc_diagnostic(int event, int value);
int x4_native_socket_nonblock(int descriptor);
