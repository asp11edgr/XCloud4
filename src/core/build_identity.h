/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen native sources plus independent, explicit experiment modes. */
#pragma once
#ifndef X4_PUBLIC
#define X4_PUBLIC 0
#endif
#if X4_PUBLIC
#define X4_PRODUCT_VERSION "1.2.1-beta"
#else
#define X4_PRODUCT_VERSION "0.7.58-lab"
#endif
/* Base revision; the task delivery records the complete uncommitted source. */
#define X4_SOURCE_ID "f18bbebac41f8493ef1bfd498f74cf076481cb20"
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
#if X4_VIDEO_CAPACITY_1080
#define X4_VIDEO_CAPACITY_ID "-vc1"
#else
#define X4_VIDEO_CAPACITY_ID "-vc0"
#endif
#if X4_VIDEO_PROBE
#define X4_VIDEO_PROBE_ID "-p1"
#else
#define X4_VIDEO_PROBE_ID "-p0"
#endif
#ifdef X4_INGRESS_BASELINE
#define X4_INGRESS_ID "-ib"
#else
#define X4_INGRESS_ID "-im"
#endif
#if X4_STREAM_PROFILE == 1
#define X4_STREAM_PROFILE_ID "-sp1"
#elif X4_STREAM_PROFILE == 2
#define X4_STREAM_PROFILE_ID "-sp2"
#else
#define X4_STREAM_PROFILE_ID "-sp0"
#endif
#define X4_BUILD_ID X4_SOURCE_ID X4_NACK_ID X4_GRACE_ID X4_RUMBLE_ID X4_VIDEO_CAPACITY_ID X4_VIDEO_PROBE_ID X4_INGRESS_ID X4_STREAM_PROFILE_ID
