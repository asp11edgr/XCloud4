/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen native sources plus independent, explicit experiment modes. */
#pragma once
#define X4_PRODUCT_VERSION "0.7.34"
#define X4_SOURCE_ID "54d5793d1b50a7ea24ba73e79b258577f51511c0"
#if X4_RECEIVER_NACK
#define X4_NACK_ID "-n1"
#else
#define X4_NACK_ID "-n0"
#endif
#if X4_NACK_GRACE_MS == 45
#define X4_GRACE_ID "-g45"
#else
#define X4_GRACE_ID "-g0"
#endif
#if X4_RUMBLE
#define X4_RUMBLE_ID "-r1"
#else
#define X4_RUMBLE_ID "-r0"
#endif
#define X4_BUILD_ID X4_SOURCE_ID X4_NACK_ID X4_GRACE_ID X4_RUMBLE_ID
