/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifndef X4_STREAM_PROFILE
#define X4_STREAM_PROFILE 0
#endif
#ifndef X4_PUBLIC
#define X4_PUBLIC 0
#endif

/* The compile flag still protects the experimental pipeline. Request settings
 * are process-local and captured by value before each session is started. */
typedef enum X4StreamRequestProfile {
    X4_REQUEST_720P60 = 0, X4_REQUEST_1080P_G = 1,
    X4_REQUEST_1080P_H = 2, X4_REQUEST_1080P_HQ = 3,
    X4_REQUEST_720P_HQ = 4, X4_REQUEST_1080P_G8 = 5,
    X4_REQUEST_PROFILE_MAX = X4_REQUEST_1080P_G8,
    X4_REQUEST_PROFILE_COUNT = 6,
    /* Internal source compatibility: A/B retain their numeric IDs. Protected
     * video/media/RTC use C as an admission ceiling, extended through ID5. */
    X4_REQUEST_1080P_A = X4_REQUEST_1080P_G,
    X4_REQUEST_1080P_B = X4_REQUEST_1080P_H,
    /* C is used only as the protected admission ceiling. It deliberately
     * follows MAX after appending G8; 1080p-HQ keeps its own ID 3. */
    X4_REQUEST_1080P_C = X4_REQUEST_PROFILE_MAX
} X4StreamRequestProfile;
typedef enum X4StreamColorPreference {
    X4_COLOR_AUTO_709 = 0, X4_COLOR_FORCE_601 = 1
} X4StreamColorPreference;
typedef enum X4StreamCpuVariant {
    X4_CPU_0 = 0, X4_CPU_1 = 1, X4_CPU_2 = 2, X4_CPU_3 = 3
} X4StreamCpuVariant;
typedef enum X4StreamDecoderDepth {
    X4_DEPTH_AUTO = 0, X4_DEPTH_1 = 1, X4_DEPTH_2 = 2
} X4StreamDecoderDepth;
/* Local session options. The low four bits retain settings bits8..11;
 * bits4..7 use settings bits17..20, leaving conversion bits12..16 intact.
 * Zero selects automatic VSYNC, GPU preference and enabled dither. */
enum {
    X4_INTERACTION_INPUT_60HZ = 1u,
    X4_INTERACTION_RUMBLE_OFF = 2u,
    X4_INTERACTION_AUDIO_PREVIOUS = 4u,
    X4_INTERACTION_VSYNC_OFF = 8u,
    X4_INTERACTION_VSYNC_ALWAYS = 16u,
    X4_INTERACTION_VSYNC_MASK = 24u,
    X4_INTERACTION_VSYNC_SYNCED = 24u,
    X4_INTERACTION_MAILBOX = 32u,
    X4_INTERACTION_DITHER_OFF = 64u,
    X4_INTERACTION_GPU_FORCE_CPU = 128u,
    X4_INTERACTION_MASK = 255u
};
typedef enum X4StreamVsyncPolicy {
    X4_VSYNC_AUTO = 0, X4_VSYNC_ALWAYS = 1, X4_VSYNC_NEVER = 2, X4_VSYNC_SYNCED = 3
} X4StreamVsyncPolicy;
#ifndef X4_CONVERT_SMOOTH_DEFAULT
#define X4_CONVERT_SMOOTH_DEFAULT 1
#endif
#ifndef X4_SHARPNESS_PUBLIC_DEFAULT
#define X4_SHARPNESS_PUBLIC_DEFAULT 2
#endif

typedef enum {
    X4_NACK_POLICY_T43C = 0, X4_NACK_POLICY_T43 = 1, X4_NACK_POLICY_T43B = 2
} X4StreamNackPolicy;
typedef enum {
    X4_RR_ZERO = 0, X4_RR_REPAIRED = 1, X4_RR_EDGE = 2
} X4StreamRrMode;

typedef struct X4StreamRequestConfig {
    X4StreamRequestProfile profile;
    X4StreamColorPreference color_preference;
    unsigned width, height, fps, bitrate_kbps;
    const char *os_name, *label, *h264_fmtp;
    bool display_info;
    /* Occupies existing padding: keep the captured config's 72-byte ABI. */
    unsigned char cpu_variant;
    unsigned char decoder_depth;
    unsigned char interaction_modes;
    unsigned char convert_mode; /* 0 = Original, 1 = Suave */
    unsigned char sharpness;    /* 0 = No, 1 = Baja, 2 = Media */
    unsigned char resolution_request_mode; /* 0 = Cada 300 ms, 1 = Una vez, 2 = Repetir c/5s */
    /* T43 occupies the final padding byte, not a protocol field. Low 2 bits:
     * NACK/RTX/RTX+FlexFEC; next 2 bits: simulated loss class 0/1/3 percent;
     * bits 4..5 select NACK policy; bits 6..7 select RR. Zero selects T43c/Edge. */
    unsigned char recovery_options;
    unsigned h264_profile, max_fs, max_mbps;
} X4StreamRequestConfig;
#if defined(__cplusplus)
static_assert(sizeof(X4StreamRequestConfig)==72&&
#else
_Static_assert(sizeof(X4StreamRequestConfig)==72&&
#endif
    offsetof(X4StreamRequestConfig,recovery_options)==55&&
    offsetof(X4StreamRequestConfig,h264_profile)==56,
    "T43 retains the captured x86_64 request layout");

/* Invalid enum arguments produce the safe 720p/automatic defaults. No string
 * supplied by a caller is retained; all returned pointers are fixed literals. */
X4StreamRequestConfig x4_stream_request_config(X4StreamRequestProfile, X4StreamColorPreference);
X4StreamRequestConfig x4_stream_settings_get(void);
bool x4_stream_settings_set(X4StreamRequestProfile, X4StreamColorPreference);
bool x4_stream_settings_set_cpu(X4StreamRequestProfile, X4StreamColorPreference, X4StreamCpuVariant);
bool x4_stream_settings_set_all(X4StreamRequestProfile, X4StreamColorPreference,
    X4StreamCpuVariant, X4StreamDecoderDepth);
bool x4_stream_settings_set_extended(X4StreamRequestProfile, X4StreamColorPreference,
    X4StreamCpuVariant, X4StreamDecoderDepth, unsigned interaction_modes);
bool x4_stream_settings_set_lab(X4StreamRequestProfile, X4StreamColorPreference,
    X4StreamCpuVariant, X4StreamDecoderDepth, unsigned interaction_modes,
    unsigned char convert_mode, unsigned char sharpness, unsigned char resolution_request_mode);
bool x4_stream_settings_set_recovery(unsigned mode, unsigned loss_percent);
bool x4_stream_settings_set_recovery_policy(unsigned mode, unsigned loss_percent,
    X4StreamNackPolicy policy, X4StreamRrMode rr_mode);
bool x4_stream_settings_set_lab_recovery(X4StreamRequestProfile, X4StreamColorPreference,
    X4StreamCpuVariant, X4StreamDecoderDepth, unsigned interaction_modes,
    unsigned char convert_mode, unsigned char sharpness, unsigned char resolution_request_mode,
    unsigned recovery_mode, unsigned loss_percent, X4StreamNackPolicy policy, X4StreamRrMode rr_mode);
X4StreamRequestConfig x4_stream_request_capture(const X4StreamRequestConfig *);
X4StreamVsyncPolicy x4_stream_request_vsync_policy(const X4StreamRequestConfig *);
bool x4_stream_request_mailbox(const X4StreamRequestConfig *);
bool x4_stream_request_dither_enabled(const X4StreamRequestConfig *);
bool x4_stream_request_gpu_force_cpu(const X4StreamRequestConfig *);
unsigned x4_stream_request_recovery_mode(const X4StreamRequestConfig *);
unsigned x4_stream_request_loss_percent(const X4StreamRequestConfig *);
bool x4_stream_request_legacy_nack(const X4StreamRequestConfig *);
X4StreamNackPolicy x4_stream_request_nack_policy(const X4StreamRequestConfig *);
X4StreamRrMode x4_stream_request_rr_mode(const X4StreamRequestConfig *);
unsigned x4_stream_interaction_with_vsync(unsigned modes, X4StreamVsyncPolicy);
#if X4_STREAM_PROFILE
#include <stdio.h>
/* Owner resets before a new session after the previous sender has joined;
 * final report writing occurs after the current sender has joined. */
void x4_rtc_input_metrics_reset(void);
void x4_rtc_input_metrics_write(FILE *);
void x4_main_interaction_metrics_write(FILE *);
void x4_live_interaction_metrics_write(FILE *);
#endif
/* Local decoder policy only: never added to /play, headers, aliases or SDP.
 * Auto follows the received geometry on every decoder recreation. With no
 * SPS yet, the captured request's canonical dimensions are the initial hint. */
unsigned x4_stream_request_resolve_depth(const X4StreamRequestConfig *, unsigned width, unsigned height);
/* The verified native 720p header is invariant on titles, MRU and every
 * session endpoint except /play, including SDP/ICE/keepalive/DELETE. The
 * protocol clientAppVersion intentionally remains the baseline value. */
#define X4_STREAM_NATIVE_720_DEVICE_INFO \
    "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\"," \
    "\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\"," \
    "\"model\":\"PS4\"},\"os\":{\"name\":\"android\",\"platform\":\"console\"}}}"
/* Public JSON only; these functions never accept credentials or arbitrary
 * strings except a bounded title ID using the existing ASCII ID grammar. */
/* This constructor is exclusively the /play header, never other endpoints. */
bool x4_stream_request_device_info(const X4StreamRequestConfig *, char *, size_t);
bool x4_stream_request_play_body(const X4StreamRequestConfig *, const char *, char *, size_t);
bool x4_stream_request_capabilities(const X4StreamRequestConfig *, char *, size_t);
bool x4_stream_request_dimensions(const X4StreamRequestConfig *, char *, size_t);
/* Stable enum-derived runtime control value; never an unvalidated string or
 * an extra field in the captured config. NULL means no alias message. */
const char *x4_stream_request_resolution_alias(const X4StreamRequestConfig *);
#if X4_STREAM_PROFILE < 0 || X4_STREAM_PROFILE > 2
#error X4_STREAM_PROFILE must be 0 (legacy), 1 (720p60), or 2 (1080p60)
#endif
#if X4_STREAM_PROFILE == 2
#define X4_STREAM_WIDTH 1920
#define X4_STREAM_HEIGHT 1080
#define X4_STREAM_FPS 60
#define X4_STREAM_BITRATE_KBPS 12000
#define X4_STREAM_OS "windows"
#define X4_STREAM_LABEL "1080p60"
/* RFC 6184 requires max-fs/max-mbps >= the signaled level's ceilings. */
#define X4_STREAM_H264_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240"
#elif X4_STREAM_PROFILE == 1
#define X4_STREAM_WIDTH 1280
#define X4_STREAM_HEIGHT 720
#define X4_STREAM_FPS 60
#define X4_STREAM_BITRATE_KBPS 10000
#define X4_STREAM_OS "android"
#define X4_STREAM_LABEL "720p60"
#define X4_STREAM_H264_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000"
#else
#define X4_STREAM_WIDTH 960
#define X4_STREAM_HEIGHT 540
#define X4_STREAM_FPS 30
#define X4_STREAM_BITRATE_KBPS 5000
#define X4_STREAM_OS "Orbis"
#define X4_STREAM_LABEL "legacy"
#define X4_STREAM_H264_FMTP "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f;max-fs=3600;max-mbps=108000"
#endif

/* Fourth value of the existing two-bit policy field. Keep the captured
 * enum/type declarations above stable for protected consumer objects. */
#define X4_NACK_POLICY_T46B ((X4StreamNackPolicy)3)

typedef enum {
    X4_REMB_OFF = 0,
    X4_REMB_FIXED = 1,
    X4_REMB_ADAPTIVE = 2
} X4RembMode;

typedef struct {
    X4RembMode mode;
    unsigned fixed_mbps;
} X4RembSettings;

#define X4_REMB_DEFAULT_FIXED_MBPS 20u

/* Peer creation snapshots REMB independently of the captured request ABI. */
#if X4_PUBLIC
static inline X4RembSettings x4_stream_settings_remb(void)
{
    X4RembSettings result = {X4_REMB_ADAPTIVE, X4_REMB_DEFAULT_FIXED_MBPS};
    return result;
}
#else
X4RembSettings x4_stream_settings_remb(void);
bool x4_stream_settings_set_remb(X4RembMode mode, unsigned fixed_mbps);
#endif

/* T46c leaves the captured request layout and the four original policy values
 * intact. The fifth Lab value travels in a spare bit of resolution_request_mode,
 * and the wider settings API accepts it alongside the interaction byte. */
#define X4_VSYNC_AUTO_T27 ((X4StreamVsyncPolicy)4)
#define X4_INTERACTION_VSYNC_T27 256u
#define X4_RESOLUTION_REQUEST_VSYNC_T27 128u
#define X4_RESOLUTION_REQUEST_MODE_MASK 3u
