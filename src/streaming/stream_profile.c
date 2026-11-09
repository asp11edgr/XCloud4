/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_profile.h"
#if X4_PUBLIC
#include "../ui/picture_settings.h"
#endif
#include "../ui/lang.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { RECOVERY_SHIFT=21, RECOVERY_OPTIONS=15u, RECOVERY_NACK_SHIFT=4, RECOVERY_RR_SHIFT=6,
    RECOVERY_MASK=255u<<RECOVERY_SHIFT, VSYNC_T27_WORD=1u<<29 };
/* One atomic word publishes all menu selections together. There is no
 * persistence, polling, lock or process-global mutable session description.
 * Default in lab matches Public 1080p Alta:
 * Profile 1080p-G (1), Color Auto 709 (0), CPU 0 (0), Depth Auto (0),
 * Automatic VSync (interaction bits zero),
 * convert_mode: Suave (1u << 12),
 * sharpness: Media (2u << 13),
 * resolution_request_mode: 0 (Cada 300 ms, 0u << 15). */
static atomic_uint settings = (unsigned)X4_REQUEST_1080P_G |
    (1u << 12) |
    (2u << 13) |
    ((unsigned)X4_NACK_POLICY_T46B << (RECOVERY_SHIFT + RECOVERY_NACK_SHIFT));
static bool recovery_valid(unsigned mode,unsigned loss)
{return mode<=2u&&(loss==0u||loss==1u||loss==3u);}
#if !X4_PUBLIC
static unsigned recovery_pack(unsigned mode,unsigned loss)
{return mode|((loss==3u?2u:loss)<<2);}
static unsigned recovery_policies(X4StreamNackPolicy policy,X4StreamRrMode rr)
{
    unsigned encoded_rr=rr==X4_RR_ZERO?1u:rr==X4_RR_REPAIRED?2u:0u;
    return ((unsigned)policy<<RECOVERY_NACK_SHIFT)|(encoded_rr<<RECOVERY_RR_SHIFT);
}
#endif

static bool recovery_policy_valid(X4StreamNackPolicy policy,X4StreamRrMode rr)
{return (unsigned)policy<=(unsigned)X4_NACK_POLICY_T46B&&(unsigned)rr<=X4_RR_EDGE;}

static unsigned interaction_word(unsigned modes)
{
    unsigned word=((modes&15u)<<8)|((modes&240u)<<13);
#if !X4_PUBLIC
    if(modes&X4_INTERACTION_VSYNC_T27)word|=VSYNC_T27_WORD;
#endif
    return word;
}
#if !X4_PUBLIC
static unsigned interaction_from_word(unsigned word)
{ return ((word>>8)&15u)|((word>>13)&240u); }
#endif
static bool interaction_valid(unsigned modes)
{
#if !X4_PUBLIC
    if(modes&X4_INTERACTION_VSYNC_T27){
        if(modes&X4_INTERACTION_VSYNC_MASK)return false;
        modes&=~X4_INTERACTION_VSYNC_T27;
    }
#endif
    return modes<=X4_INTERACTION_MASK;
}
unsigned x4_stream_interaction_with_vsync(unsigned modes,X4StreamVsyncPolicy policy)
{
    unsigned value=policy==X4_VSYNC_ALWAYS?X4_INTERACTION_VSYNC_ALWAYS:
        policy==X4_VSYNC_NEVER?X4_INTERACTION_VSYNC_OFF:
#if !X4_PUBLIC
        policy==X4_VSYNC_SYNCED?X4_INTERACTION_VSYNC_SYNCED:
        policy==X4_VSYNC_AUTO_T27?X4_INTERACTION_VSYNC_T27:
#endif
        0u;
#if !X4_PUBLIC
    modes&=~X4_INTERACTION_VSYNC_T27;
#endif
    return (modes&~X4_INTERACTION_VSYNC_MASK)|value;
}

X4StreamRequestConfig x4_stream_request_config(X4StreamRequestProfile profile,
    X4StreamColorPreference color)
{
    if ((unsigned)profile > X4_REQUEST_PROFILE_MAX) profile = X4_REQUEST_720P60;
    if ((unsigned)color > X4_COLOR_FORCE_601) color = X4_COLOR_AUTO_709;
    bool hd = profile==X4_REQUEST_1080P_G||profile==X4_REQUEST_1080P_H||profile==X4_REQUEST_1080P_HQ||profile==X4_REQUEST_1080P_G8;
    X4StreamRequestConfig c = {.profile=profile, .color_preference=color,
        .width=hd?1920u:1280u, .height=hd?1080u:720u, .fps=60,
        .bitrate_kbps=hd?12000u:10000u,
        .os_name=profile==X4_REQUEST_720P60?"android":"windows",
        .label=profile==X4_REQUEST_720P60?"720p60":profile==X4_REQUEST_1080P_G?"1080p-G":
            profile==X4_REQUEST_1080P_H?"1080p-H":profile==X4_REQUEST_1080P_HQ?"1080p-HQ":
            profile==X4_REQUEST_1080P_G8?"1080p-G8":"720p-HQ",
        .h264_fmtp=hd?
            "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240":
            "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000",
        .display_info=profile==X4_REQUEST_1080P_G||profile==X4_REQUEST_1080P_HQ||profile==X4_REQUEST_720P_HQ||profile==X4_REQUEST_1080P_G8,
#if !X4_PUBLIC
        .recovery_options=(unsigned char)((unsigned)X4_NACK_POLICY_T46B<<RECOVERY_NACK_SHIFT),
#endif
        .h264_profile=hd?0x42e02au:0x42e020u, .max_fs=hd?8704u:5120u,
        .max_mbps=hd?522240u:216000u};
    return c;
}

X4StreamRequestConfig x4_stream_settings_get(void)
{
#if X4_PUBLIC
    (void)settings;
    X4PictureSettingsConfig picture=x4_picture_settings_config();
    X4StreamRequestProfile profile=x4_picture_settings_to_profile(picture.quality);
    X4StreamRequestConfig result=x4_stream_request_config(profile,X4_COLOR_AUTO_709);
    result.cpu_variant=X4_CPU_0;
    result.decoder_depth=X4_DEPTH_AUTO;
    result.interaction_modes=(unsigned char)x4_stream_interaction_with_vsync(0u,picture.vsync);
    result.convert_mode=X4_CONVERT_SMOOTH_DEFAULT;
    result.sharpness=X4_SHARPNESS_PUBLIC_DEFAULT;
    result.resolution_request_mode=0;
    return result;
#else
    unsigned word=atomic_load_explicit(&settings,memory_order_acquire);
    X4StreamRequestConfig result=x4_stream_request_config((X4StreamRequestProfile)(word&7u),
        (X4StreamColorPreference)((word>>3)&1u));
    result.cpu_variant=(unsigned char)((word>>4)&3u);
    result.decoder_depth=(unsigned char)((word>>6)&3u);
    result.interaction_modes=(unsigned char)interaction_from_word(word);
    result.convert_mode=(unsigned char)((word>>12)&1u);
    result.sharpness=(unsigned char)((word>>13)&3u);
    result.resolution_request_mode=(unsigned char)((word>>15)&X4_RESOLUTION_REQUEST_MODE_MASK);
    if(word&VSYNC_T27_WORD)result.resolution_request_mode|=X4_RESOLUTION_REQUEST_VSYNC_T27;
    result.recovery_options=(unsigned char)((word&RECOVERY_MASK)>>RECOVERY_SHIFT);
    return result;
#endif
}
bool x4_stream_settings_set(X4StreamRequestProfile profile, X4StreamColorPreference color)
{
    if ((unsigned)profile>X4_REQUEST_PROFILE_MAX || (unsigned)color>X4_COLOR_FORCE_601) return false;
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire), desired;
    do { desired=(old&~0x0fu)|(unsigned)profile|((unsigned)color<<3); }
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_cpu(X4StreamRequestProfile profile,X4StreamColorPreference color,X4StreamCpuVariant cpu)
{
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||(unsigned)cpu>X4_CPU_3)return false;
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire), desired;
    do { desired=(old&~0x3fu)|(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4); }
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_all(X4StreamRequestProfile profile,X4StreamColorPreference color,
    X4StreamCpuVariant cpu,X4StreamDecoderDepth depth)
{
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||
        (unsigned)cpu>X4_CPU_3||(unsigned)depth>X4_DEPTH_2)return false;
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire), desired;
    do { desired=(old&~0xffu)|(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4)|
        ((unsigned)depth<<6); }
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_extended(X4StreamRequestProfile profile,X4StreamColorPreference color,
    X4StreamCpuVariant cpu,X4StreamDecoderDepth depth,unsigned interaction_modes)
{
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire);
    unsigned char convert_mode=(unsigned char)((old>>12)&1u);
    unsigned char sharpness=(unsigned char)((old>>13)&3u);
    unsigned char req_mode=(unsigned char)((old>>15)&3u);
    return x4_stream_settings_set_lab(profile,color,cpu,depth,interaction_modes,convert_mode,sharpness,req_mode);
}
bool x4_stream_settings_set_lab(X4StreamRequestProfile profile,X4StreamColorPreference color,
    X4StreamCpuVariant cpu,X4StreamDecoderDepth depth,unsigned interaction_modes,
    unsigned char convert_mode,unsigned char sharpness,unsigned char resolution_request_mode)
{
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||
        (unsigned)cpu>X4_CPU_3||(unsigned)depth>X4_DEPTH_2||!interaction_valid(interaction_modes)||
        convert_mode>1||sharpness>2||resolution_request_mode>2)return false;
    unsigned base=(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4)|
        ((unsigned)depth<<6)|interaction_word(interaction_modes)|((unsigned)convert_mode<<12)|
        ((unsigned)sharpness<<13)|((unsigned)resolution_request_mode<<15);
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire),desired;
    do {
        desired=base|(old&RECOVERY_MASK);
#if !X4_PUBLIC
        /* Fortnite reapplies the existing two VSync bits during launch. Only
         * an explicit different policy replaces T27 on this legacy setter.
         * The R3 setter below commits the complete choice, including Auto. */
        if(!(interaction_modes&X4_INTERACTION_VSYNC_MASK))desired|=old&VSYNC_T27_WORD;
#endif
    } while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_recovery(unsigned mode,unsigned loss)
{
    if(!recovery_valid(mode,loss))return false;
#if X4_PUBLIC
    return mode==2u&&loss==0u;
#else
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire),desired;
    do {desired=(old&~(RECOVERY_OPTIONS<<RECOVERY_SHIFT))|(recovery_pack(mode,loss)<<RECOVERY_SHIFT);}
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
#endif
}
bool x4_stream_settings_set_recovery_policy(unsigned mode,unsigned loss,
    X4StreamNackPolicy policy,X4StreamRrMode rr_mode)
{
    if(!recovery_valid(mode,loss)||!recovery_policy_valid(policy,rr_mode))return false;
#if X4_PUBLIC
    return mode==2u&&!loss&&policy==X4_NACK_POLICY_T46B&&rr_mode==X4_RR_EDGE;
#else
    unsigned bits=recovery_pack(mode,loss)|recovery_policies(policy,rr_mode);
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire),desired;
    do {desired=(old&~RECOVERY_MASK)|(bits<<RECOVERY_SHIFT);}
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
#endif
}
bool x4_stream_settings_set_lab_recovery(X4StreamRequestProfile profile,X4StreamColorPreference color,
    X4StreamCpuVariant cpu,X4StreamDecoderDepth depth,unsigned modes,
    unsigned char convert,unsigned char sharp,unsigned char resolution,unsigned recovery,unsigned loss,
    X4StreamNackPolicy policy,X4StreamRrMode rr_mode)
{
    if(!recovery_valid(recovery,loss)||!recovery_policy_valid(policy,rr_mode))return false;
#if X4_PUBLIC
    if(recovery!=2u||loss||policy!=X4_NACK_POLICY_T46B||rr_mode!=X4_RR_EDGE)return false;
    return x4_stream_settings_set_lab(profile,color,cpu,depth,modes,convert,sharp,resolution);
#else
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||
        (unsigned)cpu>X4_CPU_3||(unsigned)depth>X4_DEPTH_2||!interaction_valid(modes)||
        convert>1u||sharp>2u||resolution>2u)return false;
    unsigned value=(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4)|((unsigned)depth<<6)|
        interaction_word(modes)|((unsigned)convert<<12)|((unsigned)sharp<<13)|((unsigned)resolution<<15)|
        ((recovery_pack(recovery,loss)|recovery_policies(policy,rr_mode))<<RECOVERY_SHIFT);
    atomic_store_explicit(&settings,value,memory_order_release);return true;
#endif
}
X4StreamRequestConfig x4_stream_request_capture(const X4StreamRequestConfig *request)
{
    if(!request)return x4_stream_settings_get();
    X4StreamRequestConfig c=x4_stream_request_config(request->profile,request->color_preference);
    c.cpu_variant=request->cpu_variant<=X4_CPU_3?request->cpu_variant:X4_CPU_0;
    c.decoder_depth=request->decoder_depth<=X4_DEPTH_2?request->decoder_depth:X4_DEPTH_AUTO;
    c.interaction_modes=(unsigned char)(request->interaction_modes&X4_INTERACTION_MASK);
#if X4_PUBLIC
    if((c.interaction_modes&X4_INTERACTION_VSYNC_MASK)==X4_INTERACTION_VSYNC_SYNCED)
        c.interaction_modes=(unsigned char)x4_stream_interaction_with_vsync(c.interaction_modes,X4_VSYNC_AUTO);
    c.interaction_modes&=(unsigned char)~(X4_INTERACTION_GPU_FORCE_CPU|X4_INTERACTION_DITHER_OFF);
    c.recovery_options=0;
    c.convert_mode=X4_CONVERT_SMOOTH_DEFAULT;
    c.sharpness=X4_SHARPNESS_PUBLIC_DEFAULT;
    c.resolution_request_mode=0;
#else
    unsigned recovery=request->recovery_options&3u,loss=(request->recovery_options>>2)&3u;
    c.recovery_options=(unsigned char)((recovery<=2u?recovery:0u)|((loss<=2u?loss:0u)<<2)|
        recovery_policies(x4_stream_request_nack_policy(request),x4_stream_request_rr_mode(request)));
    c.convert_mode=request->convert_mode<=1?request->convert_mode:X4_CONVERT_SMOOTH_DEFAULT;
    c.sharpness=request->sharpness<=2?request->sharpness:0;
    unsigned resolution=request->resolution_request_mode&~X4_RESOLUTION_REQUEST_VSYNC_T27;
    c.resolution_request_mode=(unsigned char)(resolution<=2u?resolution:0u);
    if(!(c.interaction_modes&X4_INTERACTION_VSYNC_MASK)&&
        (request->resolution_request_mode&X4_RESOLUTION_REQUEST_VSYNC_T27))
        c.resolution_request_mode|=X4_RESOLUTION_REQUEST_VSYNC_T27;
#endif
    return c;
}
X4StreamVsyncPolicy x4_stream_request_vsync_policy(const X4StreamRequestConfig *request)
{
    X4StreamRequestConfig current;
    if(!request){current=x4_stream_settings_get();request=&current;}
    unsigned modes=request->interaction_modes&X4_INTERACTION_VSYNC_MASK;
    return modes==X4_INTERACTION_VSYNC_ALWAYS?X4_VSYNC_ALWAYS:
        modes==X4_INTERACTION_VSYNC_OFF?X4_VSYNC_NEVER:
#if !X4_PUBLIC
        modes==X4_INTERACTION_VSYNC_SYNCED?X4_VSYNC_SYNCED:
        request->resolution_request_mode&X4_RESOLUTION_REQUEST_VSYNC_T27?X4_VSYNC_AUTO_T27:
#endif
        X4_VSYNC_AUTO;
}
unsigned x4_stream_request_recovery_mode(const X4StreamRequestConfig *request)
{
#if X4_PUBLIC
    (void)request;return 2;
#else
    unsigned value=(request?request->recovery_options:x4_stream_settings_get().recovery_options)&3u;
    return value<=2u?value:0u;
#endif
}
unsigned x4_stream_request_loss_percent(const X4StreamRequestConfig *request)
{
#if X4_PUBLIC
    (void)request;return 0;
#else
    unsigned value=((request?request->recovery_options:x4_stream_settings_get().recovery_options)>>2)&3u;
    return value==1u?1u:value==2u?3u:0u;
#endif
}
X4StreamNackPolicy x4_stream_request_nack_policy(const X4StreamRequestConfig *request)
{
#if X4_PUBLIC
    (void)request;return X4_NACK_POLICY_T46B;
#else
    unsigned options=request?request->recovery_options:x4_stream_settings_get().recovery_options;
    unsigned policy=(options>>RECOVERY_NACK_SHIFT)&3u;
    return (X4StreamNackPolicy)policy;
#endif
}
bool x4_stream_request_legacy_nack(const X4StreamRequestConfig *request)
{return x4_stream_request_nack_policy(request)==X4_NACK_POLICY_T43;}
X4StreamRrMode x4_stream_request_rr_mode(const X4StreamRequestConfig *request)
{
#if X4_PUBLIC
    (void)request;return X4_RR_EDGE;
#else
    unsigned options=request?request->recovery_options:x4_stream_settings_get().recovery_options;
    unsigned mode=options>>RECOVERY_RR_SHIFT;
    return mode==1u?X4_RR_ZERO:mode==2u?X4_RR_REPAIRED:X4_RR_EDGE;
#endif
}
bool x4_stream_request_mailbox(const X4StreamRequestConfig *request)
{
#if X4_PUBLIC
    (void)request;return false;
#else
    unsigned modes=request?request->interaction_modes:x4_stream_settings_get().interaction_modes;
    return (modes&X4_INTERACTION_MAILBOX)!=0;
#endif
}
bool x4_stream_request_dither_enabled(const X4StreamRequestConfig *request)
{
    unsigned modes=request?request->interaction_modes:x4_stream_settings_get().interaction_modes;
    return !(modes&X4_INTERACTION_DITHER_OFF);
}
bool x4_stream_request_gpu_force_cpu(const X4StreamRequestConfig *request)
{
    unsigned modes=request?request->interaction_modes:x4_stream_settings_get().interaction_modes;
    return (modes&X4_INTERACTION_GPU_FORCE_CPU)!=0;
}
unsigned x4_stream_request_resolve_depth(const X4StreamRequestConfig *request,unsigned width,unsigned height)
{
    X4StreamRequestConfig c=x4_stream_request_capture(request);
    if(c.decoder_depth!=X4_DEPTH_AUTO)return c.decoder_depth;
    if(!width||!height){width=c.width;height=c.height;}
    return width>1280u||height>720u?2u:1u;
}

static X4StreamRequestConfig normalized(const X4StreamRequestConfig *c)
{
    return x4_stream_request_capture(c);
}
const char *x4_stream_request_resolution_alias(const X4StreamRequestConfig *request)
{
    X4StreamRequestConfig c=normalized(request);
    return c.profile==X4_REQUEST_720P60?NULL:c.profile==X4_REQUEST_720P_HQ?"720HQ":
        c.profile==X4_REQUEST_1080P_HQ?"1080HQ":"1080";
}
static bool fits(int n, char *out, size_t capacity)
{
    if (n>0 && (size_t)n<capacity) return true;
    if (out && capacity) out[0]=0;
    return false;
}
bool x4_stream_request_device_info(const X4StreamRequestConfig *request, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    X4StreamRequestConfig c=normalized(request);
    if (c.profile==X4_REQUEST_720P60)
        return fits(snprintf(out,capacity,"%s",X4_STREAM_NATIVE_720_DEVICE_INFO),out,capacity);
    if (c.profile==X4_REQUEST_1080P_H)
        return fits(snprintf(out,capacity,
            "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\","
            "\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\","
            "\"model\":\"PS4\"},\"os\":{\"name\":\"windows\",\"platform\":\"console\"}}}"),out,capacity);
    /* Public wire structure: Better xCloud (MIT), pinned at
     * f8397043f6d2148d2345d508902a38c69cf1ee20, network.ts:316-337.
     * clientAppId is window.location.host there; this experiment uses
     * www.xbox.com. G/HQ and 720p-HQ keep Windows/4096x2160. No TS
     * interception implementation is copied. See upstream license evidence. */
    unsigned display_width=4096u, display_height=2160u;
    return fits(snprintf(out,capacity,
        "{\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\","
        "\"clientAppVersion\":\"26.1.97\",\"clientSdkVersion\":\"10.3.7\","
        "\"httpEnvironment\":\"prod\",\"sdkInstallId\":\"\"}},\"dev\":{\"os\":{\"name\":\"%s\","
        "\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"hw\":{\"make\":\"Microsoft\","
        "\"model\":\"unknown\",\"sdktype\":\"web\"},\"browser\":{\"browserName\":\"chrome\","
        "\"browserVersion\":\"140.0.3485.54\"},\"displayInfo\":{\"dimensions\":{\"widthInPixels\":%u,"
        "\"heightInPixels\":%u},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}}}}",
        c.os_name,display_width,display_height),out,capacity);
}
bool x4_stream_request_play_body(const X4StreamRequestConfig *request, const char *id,
    char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    out[0]=0;
    size_t length=0;
    if (!id) return false;
    for (;length<=128 && id[length];++length) {
        unsigned char v=(unsigned char)id[length];
        if (!((v>='A'&&v<='Z')||(v>='a'&&v<='z')||(v>='0'&&v<='9')||v=='-'||v=='_'||v=='.')) return false;
    }
    if (!length || length>128) return false;
    X4StreamRequestConfig c=normalized(request);
    return fits(snprintf(out,capacity,
        "{\"clientSessionId\":\"\",\"titleId\":\"%s\",\"systemUpdateGroup\":\"\",\"settings\":{"
        "\"nanoVersion\":\"V3;WebrtcTransport.dll\",\"enableOptionalDataCollection\":false,"
        "\"enableTextToSpeech\":false,\"highContrast\":0,\"locale\":\"%s\",\"useIceConnection\":false,"
        "\"timezoneOffsetMinutes\":-360,\"sdkType\":\"web\",\"osName\":\"%s\"},"
        "\"serverId\":\"\",\"fallbackRegionNames\":[]}",id,x4_lang_locale(),c.os_name),out,capacity);
}
bool x4_stream_request_capabilities(const X4StreamRequestConfig *request, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    X4StreamRequestConfig c=normalized(request);
    if (c.profile!=X4_REQUEST_720P60)
        return fits(snprintf(out,capacity,"{}"),out,capacity);
    return fits(snprintf(out,capacity,
        "{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":%u,"
        "\"maxWidth\":%u,\"maxHeight\":%u,\"maxBitrateKbps\":%u,\"video\":{\"width\":%u,\"height\":%u,"
        "\"maxWidth\":%u,\"maxHeight\":%u,\"maxBitrateKbps\":%u}}",
        c.fps,c.width,c.height,c.bitrate_kbps,c.width,c.height,c.width,c.height,c.bitrate_kbps),out,capacity);
}
bool x4_stream_request_dimensions(const X4StreamRequestConfig *request, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    X4StreamRequestConfig c=normalized(request);
    return fits(snprintf(out,capacity,
        "{\"horizontal\":%u,\"vertical\":%u,\"preferredWidth\":%u,\"preferredHeight\":%u,"
        "\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":%u,\"safeAreaBottom\":%u,"
        "\"supportsCustomResolution\":true}",c.width,c.height,c.width,c.height,c.width,c.height),out,capacity);
}

#if !X4_PUBLIC
/* Publish the mode and its fixed value together. REMB has its own word so
 * the captured request and recovery settings retain their previous layout. */
enum { REMB_VALUE_SHIFT = 8u, REMB_MODE_MASK = 255u };
static atomic_uint remb_settings = X4_REMB_ADAPTIVE | (X4_REMB_DEFAULT_FIXED_MBPS << REMB_VALUE_SHIFT);

X4RembSettings x4_stream_settings_remb(void)
{
    unsigned word = atomic_load_explicit(&remb_settings, memory_order_acquire);
    return (X4RembSettings){(X4RembMode)(word & REMB_MODE_MASK), word >> REMB_VALUE_SHIFT};
}

bool x4_stream_settings_set_remb(X4RembMode mode, unsigned fixed_mbps)
{
    if ((unsigned)mode > X4_REMB_ADAPTIVE ||
        (fixed_mbps != 10u && fixed_mbps != 15u && fixed_mbps != 20u && fixed_mbps != 30u))
        return false;
    atomic_store_explicit(&remb_settings, (unsigned)mode | (fixed_mbps << REMB_VALUE_SHIFT),
        memory_order_release);
    return true;
}
#endif
