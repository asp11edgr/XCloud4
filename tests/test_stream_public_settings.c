/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/streaming/stream_profile.h"
#if X4_PUBLIC
#include "../src/ui/picture_settings.h"
#endif

int main(void)
{
#if X4_PUBLIC
    /* Public build: default 1080p-G (Alta), Depth Auto, VSync On, Suave, Media, req 0 */
    x4_picture_settings_set_path("test_stream_public_settings.bin");
    remove("test_stream_public_settings.bin");
    x4_picture_settings_init();
    X4StreamRequestConfig cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G);
    assert(cfg.color_preference == X4_COLOR_AUTO_709);
    assert(cfg.cpu_variant == X4_CPU_0);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);
    assert(cfg.convert_mode == X4_CONVERT_SMOOTH_DEFAULT);
    assert(cfg.sharpness == X4_SHARPNESS_PUBLIC_DEFAULT);
    assert(cfg.resolution_request_mode == 0);
    assert(cfg.width == 1920u);
    assert(cfg.height == 1080u);

    /* Verify changing picture setting updates profile, keeping invariant public options */
    x4_picture_settings_set(X4_PIC_QUALITY_1080P);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.convert_mode == X4_CONVERT_SMOOTH_DEFAULT);
    assert(cfg.sharpness == X4_SHARPNESS_PUBLIC_DEFAULT);
    assert(cfg.resolution_request_mode == 0);

    x4_picture_settings_set(X4_PIC_QUALITY_720P);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_720P_HQ);
    assert(cfg.width == 1280u);
    assert(cfg.height == 720u);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.convert_mode == X4_CONVERT_SMOOTH_DEFAULT);
    assert(cfg.sharpness == X4_SHARPNESS_PUBLIC_DEFAULT);
    assert(cfg.resolution_request_mode == 0);

    /* Explicit loop verifying all 4 public qualities have VSync On, Depth Auto, Suave, Media, req 0 */
    X4PictureQualityOption all_qualities[4] = {
        X4_PIC_QUALITY_1080P_ALTA,
        X4_PIC_QUALITY_1080P,
        X4_PIC_QUALITY_720P,
        X4_PIC_QUALITY_1080P_MAX
    };
    for (int i = 0; i < 4; ++i) {
        x4_picture_settings_set(all_qualities[i]);
        X4StreamRequestConfig c = x4_stream_settings_get();
        assert(c.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);
        assert(c.decoder_depth == X4_DEPTH_AUTO);
        assert(c.convert_mode == X4_CONVERT_SMOOTH_DEFAULT);
        assert(c.sharpness == X4_SHARPNESS_PUBLIC_DEFAULT);
        assert(c.resolution_request_mode == 0);
    }

    /* Verify lab setters still do not alter public settings */
    x4_stream_settings_set(X4_REQUEST_720P60, X4_COLOR_FORCE_601);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_HQ);
    assert(cfg.color_preference == X4_COLOR_AUTO_709);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);

    x4_stream_settings_set_extended(X4_REQUEST_720P60, X4_COLOR_FORCE_601, X4_CPU_1, X4_DEPTH_2, 0);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_HQ);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == X4_INTERACTION_VSYNC_ALWAYS);

    remove("test_stream_public_settings.bin");
    puts("PASS test_stream_public_settings (X4_PUBLIC=1: Picture Settings profile, Depth Auto, VSync On, Suave, Media, req 0, lab setters ignored)");
#else
    /* Lab build: starts identical to public (1080p-G, Depth Auto, VSync Auto, Suave, Media, req 0) */
    X4StreamRequestConfig cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G);
    assert(cfg.color_preference == X4_COLOR_AUTO_709);
    assert(cfg.cpu_variant == X4_CPU_0);
    assert(cfg.decoder_depth == X4_DEPTH_AUTO);
    assert(cfg.interaction_modes == 0);
    assert(cfg.convert_mode == 1);
    assert(cfg.sharpness == 2);
    assert(cfg.resolution_request_mode == 0);

    /* Lab build updates when set is called */
    x4_stream_settings_set_extended(X4_REQUEST_1080P_G8, X4_COLOR_AUTO_709, X4_CPU_0, X4_DEPTH_2, 0);
    cfg = x4_stream_settings_get();
    assert(cfg.profile == X4_REQUEST_1080P_G8);
    assert(cfg.decoder_depth == X4_DEPTH_2);
    assert(cfg.interaction_modes == 0);

    puts("PASS test_stream_public_settings (X4_PUBLIC=0: default identical to public 1080p Alta, mutable)");
#endif
    return 0;
}
