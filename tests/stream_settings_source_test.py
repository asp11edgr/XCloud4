# SPDX-License-Identifier: GPL-3.0-only
"""Small coordinator source contract, alongside executable model/render tests."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
main=(root/'src/core/main.c').read_text(encoding='utf-8')
assert main.count('X4StreamRequestConfig request_cfg=x4_stream_settings_get();')==1
assert 'x4_live_media_create_request(&live_error,&request_cfg)' in main
assert 'x4_auth_start_session_request(auth,catalog_selected,&request_cfg)' in main
assert '!pending_launch&&!session_back&&!live_retained' in main
assert 'if(stream_settings_consumed) ui_pressed&=ORBIS_PAD_BUTTON_OPTIONS;' in main
assert 'if(stream_settings_consumed) {\n                pending_launch=0;' in main
assert 'stream_received_until=input_at+6000000' in main
assert main.index('x4_stream_settings_ui_handle(')<main.index('x4_screen_update(')
assert main.index('if(stream_settings_consumed) {')<main.index('bool selection_changing =')
settings=(root/'src/ui/stream_settings.c').read_text(encoding='utf-8')
gui=(root/'src/ui/catalog_gui.c').read_text(encoding='utf-8')
assert 'Calidad: %s · CPU-%u · R3' in settings
assert 'Aplicado: %s' in settings
assert 'Profundidad: Auto (720p: 1 · 1080p: 2)' in settings
assert 's->depth=(X4StreamDecoderDepth)current.decoder_depth;' in settings
assert 's->interaction_modes=current.interaction_modes;' in settings
assert 'x4_stream_settings_set_extended(s->profile,s->color,s->cpu,s->depth,s->interaction_modes)' in settings
assert all(label in settings for label in('Envío del mando','Inmediato','60 Hz (anterior)','Vibración','Audio','Anterior','Nuevo','Sincronía vertical','No (menos retraso, puede verse un corte)'))
assert '(s->row+1u)%8u' in settings and '(s->row+7u)%8u' in settings
assert 's->applied = true; s->open = false;' in settings
assert 'x4_stream_settings_summary(video_settings,sizeof(video_settings));' in gui
assert 'txt(p,650,68,28,1,video_settings,580,green);' in gui
assert all(label in settings for label in('1080p-G','1080p-H','1080p-HQ','720p-HQ'))
assert not any(label in settings for label in('1080p-A','1080p-B','1080p-C','1080p-D','1080p-E','1080p-F'))
selection=main[main.index('bool selection_changing ='):main.index('x4_catalog_assets_suspend(catalog_assets);',main.index('bool selection_changing ='))]
assert all(key in selection for key in('ORBIS_PAD_BUTTON_UP','ORBIS_PAD_BUTTON_DOWN',
    'ORBIS_PAD_BUTTON_LEFT','ORBIS_PAD_BUTTON_RIGHT','!catalog_copied','!recent_copied','!metadata_changed','!selection_changing'))
assert main.index('else if ((pressed & ORBIS_PAD_BUTTON_SQUARE)')<main.index('bool selection_changing =')
auth=(root/'src/auth/device_auth.c').read_text(encoding='utf-8')
media=(root/'src/media/live_media.c').read_text(encoding='utf-8')
xbox=(root/'src/auth/xbox_live.c').read_text(encoding='utf-8')
rtc=(root/'src/streaming/rtc_transport.c').read_text(encoding='utf-8')
pipeline=(root/'src/video/stream_pipeline.c').read_text(encoding='utf-8')
assert 'a->session_request = x4_stream_request_capture(request);' in auth
assert 'x4_xbox_work_set_request_config(a->xwork, &a->session_request)' in auth
assert auth.index('x4_xbox_work_set_request_config(a->xwork, &a->session_request)') < auth.index(
    'rc = scePthreadCreate(&a->thread, NULL, worker, a, "x4-auth");',auth.index('int x4_auth_start_session_request'))
assert 'X4StreamRequestConfig selected = x4_stream_request_capture(request);' in media
assert 'x4_stream_pipeline_create_request(m->trace_session, m->trace, &m->stop, &selected)' in media
assert 'config.width=selected.width; config.height=selected.height;' in media
assert 'w->request_config=x4_stream_request_capture(request);' in xbox
assert 'x4_stream_request_device_info(&w->request_config,w->play_device_info,sizeof(w->play_device_info))' in xbox
assert 'x4_stream_request_play_body(&w->request_config,title->id,w->request,sizeof(w->request))' in xbox
assert 'x4_rtc_open_request(&rc,&w->request_config)' in xbox
assert 'rtc->request_config=x4_stream_request_capture(request);' in rtc
assert '.profile=rtc->request_config.h264_fmtp' in rtc
assert 'const X4StreamRequestConfig *c=&rtc->request_config;' in rtc
assert 'legacy=(rtc->request_config.interaction_modes&X4_INTERACTION_INPUT_60HZ)!=0;' in rtc
assert 'm->stream_audio_previous=(selected.interaction_modes&X4_INTERACTION_AUDIO_PREVIOUS)!=0;' in media
assert 'x4_live_audio_start_mode(&m->audio_error, m->audio_payload_type, m->stream_audio_previous)' in media
assert 'interaction_main_begin(request_cfg.interaction_modes,sceKernelGetProcessTime());' in main
assert 'if(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)desired.lgMotor=desired.smMotor=0;' in main
request_source=(root/'src/streaming/stream_profile.c').read_text(encoding='utf-8')
assert 'c.interaction_modes=(unsigned char)(request->interaction_modes&X4_INTERACTION_MASK);' in request_source
assert 'x4_stream_request_capabilities(c,content,sizeof(content))' in rtc
assert 'x4_stream_request_dimensions(c,content,sizeof(content))' in rtc
assert 'p->request=x4_stream_request_capture(request);' in pipeline
for lhs,rhs in [('video_matrix_received','received_matrix'),('video_matrix_selected','effective_matrix'),
    ('video_full_range','full_range'),('video_color_generation','color_notice_started_us'),
    ('video_color_ready','native_ready')]:
    assert f's->{lhs} = stream.{rhs};' in media
assert '#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE' in media
assert '#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE' in auth
assert '#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE' in (root/'src/ui/stream_settings.c').read_text(encoding='utf-8')
assert '#define SETTINGS_ROWS 18u' in settings and '#define LAB_VISIBLE_ROWS 10u' in settings
assert 'quality_draft_owner' in settings and 'atomic_uint quality_settings' in settings
assert 'atomic_store_explicit(&quality_settings,0u,memory_order_release);' in settings
assert settings.index('if(!draft_valid(s))') < settings.index('x4_stream_settings_conversion_set((s->convert_sharp>>5)&1u)')
assert settings.index('x4_stream_settings_set_lab(s->profile') < settings.index('(void)x4_gpu_quality_settings_set(quality_draft)')
assert 'quality&QUALITY_CAS?cas_sharpness:sharpness' in settings
assert 'quality&QUALITY_PAUSE_REPEAT?X4_TR_LAB_GPU_PAUSE_REPEAT:X4_TR_LAB_GPU_PAUSE_CPU' in settings
assert not any(name in (root/'src/ui/stream_settings.h').read_text(encoding='utf-8') for name in ('quality_draft','quality_settings'))
for name in ('src/ui/lang.h','src/ui/lang.c'):
    source=(root/name).read_text(encoding='utf-8')
    for position in (i for i in range(len(source)) if source.startswith('X4_TR_LAB_DITHER',i)):
        assert source.rfind('#if !X4_PUBLIC',0,position)>source.rfind('#endif',0,position)
assert settings.index('if (!s->open) {') < settings.index('script_settings_rescan();')
assert 'if (!game_input) script_previous_us = 0;' in main
assert 'script_previous_us = script_at;' in main and 'if (dt > 0.1f) dt = 0.1f;' in main
assert 'float dt = (script_previous_us && script_at > script_previous_us)' in main
assert main.index('x4_script_engine_export_session_json(') < main.index('x4_script_engine_shutdown();')
assert main.count('x4_script_engine_export_session_json(')==1
print('stream settings/main/auth/media/RTC snapshot source contract passed; not a runtime claim')
