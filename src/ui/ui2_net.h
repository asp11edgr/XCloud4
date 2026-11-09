/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "ui2_shell.h"

/* Only the UI owner calls these. now_us is a monotonic process timestamp.
 * No NetCtl initialization, endpoint reads, DNS, sockets or log output. */
X4Ui2Network x4_ui2_network_type(uint64_t now_us);
int x4_ui2_local_minute(uint64_t now_us);
