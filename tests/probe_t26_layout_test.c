/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_t26_backend.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    X4StreamSps s={.coded_width=1920,.coded_height=1088,.visible_width=1920,.visible_height=1080,
        .crop_bottom=8,.chroma_format_idc=1,.bit_depth_luma=8,.bit_depth_chroma=8,.frame_mbs_only_flag=1};
    X4VideoNv12Layout out;size_t bytes=(size_t)2048*1088*3/2;
    assert(!x4_t26_backend_nv12_layout(&s,1920,1088,2048,bytes,bytes,&out));
    assert(out.visible_width==1920&&out.visible_height==1080&&out.storage_height==1088&&
        out.uv_plane_offset==(size_t)2048*1088&&out.total_nv12_bytes==bytes);
    assert(x4_t26_backend_nv12_layout(&s,1920,1080,2048,bytes,bytes,&out)==X4_T26_BACKEND_LAYOUT);
    assert(!out.total_nv12_bytes); /* Never infer the missing eight lines. */
    assert(x4_t26_backend_nv12_layout(&s,1920,1088,1918,bytes,bytes,&out));
    assert(x4_t26_backend_nv12_layout(&s,1920,1088,2048,bytes-1,bytes,&out));
    assert(x4_t26_backend_nv12_layout(&s,1920,1088,2048,bytes,bytes-1,&out));
    s.crop_bottom=0;assert(x4_t26_backend_nv12_layout(&s,1920,1088,2048,bytes,bytes,&out));
    s.crop_bottom=8;s.bit_depth_luma=10;assert(x4_t26_backend_nv12_layout(&s,1920,1088,2048,bytes,bytes,&out));
    s.bit_depth_luma=8;s.chroma_format_idc=2;assert(x4_t26_backend_nv12_layout(&s,1920,1088,2048,bytes,bytes,&out));
    s=(X4StreamSps){.coded_width=1280,.coded_height=720,.visible_width=1280,.visible_height=720,
        .chroma_format_idc=1,.bit_depth_luma=8,.bit_depth_chroma=8,.frame_mbs_only_flag=1};
    bytes=(size_t)1280*720*3/2;
    assert(!x4_t26_backend_nv12_layout(&s,1280,720,1280,bytes,bytes,&out));
    assert(out.visible_y_offset==0&&out.visible_uv_offset==(size_t)1280*720);
    s.crop_left=UINT32_MAX;assert(x4_t26_backend_nv12_layout(&s,1280,720,1280,bytes,bytes,&out));
    assert(x4_t26_backend_nv12_layout(NULL,1280,720,1280,bytes,bytes,&out));
    assert(x4_t26_backend_nv12_layout(&s,1280,720,1280,bytes,bytes,NULL));
    puts("PASS T26 NV12 exact coded geometry, explicit SPS crop, bounds, and malformed layouts");return 0;
}
