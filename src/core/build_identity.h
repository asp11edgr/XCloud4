/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen native sources plus explicit receiver experiment mode. */
#pragma once
#define X4_PRODUCT_VERSION "0.7.33"
#if X4_RECEIVER_NACK
#define X4_BUILD_ID "e9a0b21e02d059559c4a57bed8f80db183f86021-nack1"
#else
#define X4_BUILD_ID "e9a0b21e02d059559c4a57bed8f80db183f86021-nack0"
#endif
