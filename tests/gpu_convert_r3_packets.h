/* SPDX-License-Identifier: GPL-3.0-only */
/* Frozen TAREA24 R3 packet schema. Deliberately independent of production
 * acquire/timestamp/descriptor/commands helpers; receives the SAME addresses
 * so the final assertion compares every DCB byte without relocation masks. */
static const Shader r3_shaders[8]={
    {x4_gpu_original_code,sizeof(x4_gpu_original_code),X4_GPU_ORIGINAL_RSRC1,X4_GPU_ORIGINAL_RSRC2},
    {x4_gpu_smooth0_code,sizeof(x4_gpu_smooth0_code),X4_GPU_SMOOTH0_RSRC1,X4_GPU_SMOOTH0_RSRC2},
    {x4_gpu_smooth1_code,sizeof(x4_gpu_smooth1_code),X4_GPU_SMOOTH1_RSRC1,X4_GPU_SMOOTH1_RSRC2},
    {x4_gpu_smooth2_code,sizeof(x4_gpu_smooth2_code),X4_GPU_SMOOTH2_RSRC1,X4_GPU_SMOOTH2_RSRC2},
    {x4_gpu_original720_code,sizeof(x4_gpu_original720_code),X4_GPU_ORIGINAL720_RSRC1,X4_GPU_ORIGINAL720_RSRC2},
    {x4_gpu_smooth720_0_code,sizeof(x4_gpu_smooth720_0_code),X4_GPU_SMOOTH720_0_RSRC1,X4_GPU_SMOOTH720_0_RSRC2},
    {x4_gpu_smooth720_1_code,sizeof(x4_gpu_smooth720_1_code),X4_GPU_SMOOTH720_1_RSRC1,X4_GPU_SMOOTH720_1_RSRC2},
    {x4_gpu_smooth720_2_code,sizeof(x4_gpu_smooth720_2_code),X4_GPU_SMOOTH720_2_RSRC1,X4_GPU_SMOOTH720_2_RSRC2}
};
static uint32_t *r3_acquire(uint32_t *p,uint32_t coherence)
{
    *p++=0xc0055800u;*p++=coherence;*p++=UINT32_MAX;*p++=0;
    *p++=0;*p++=0;*p++=10;return p;
}
static uint32_t *r3_timestamp(uint32_t *p,const volatile uint64_t *slot)
{
    uintptr_t address=(uintptr_t)slot;
    *p++=0xc0044700u;*p++=0x528;*p++=(uint32_t)address;
    *p++=0x60000000u|((uint32_t)(address>>32)&0xffffu);*p++=0;*p++=0;
    return p;
}
static void r3_descriptor(uint32_t words[4],const void *pointer,uint32_t bytes,uint32_t format)
{
    uintptr_t address=(uintptr_t)pointer;
    words[0]=(uint32_t)address;words[1]=(uint32_t)(address>>32)&0xffffu;
    words[2]=bytes;words[3]=format;
}
static size_t r3_commands(const X4GpuConvert *g,const X4GpuConvertFrame *f,unsigned shader,uint32_t *output)
{
    uint32_t users[16];
    r3_descriptor(users,f->source,(uint32_t)f->layout.y_bytes,0x0000cfacu);
    r3_descriptor(users+4,f->source+f->layout.uv_plane_offset,(uint32_t)f->layout.uv_bytes,0x0001cfacu);
    r3_descriptor(users+8,f->destination,(uint32_t)f->destination_bytes,0x00024facu);
    r3_descriptor(users+12,g->parameters,32,0x00024facu);
    uintptr_t code=(uintptr_t)g->code[shader],label=(uintptr_t)g->label;
    uint32_t *p=output;
    p=r3_acquire(p,0x80c40000u);p=r3_acquire(p,0x88400000u);
    *p++=0xc0037602u;*p++=0x204;*p++=0;*p++=0;*p++=0;
    *p++=0xc0037602u;*p++=0x207;*p++=8;*p++=8;*p++=1;
    *p++=0xc0027602u;*p++=0x20c;*p++=(uint32_t)(code>>8);*p++=(uint32_t)(code>>40);
    *p++=0xc0027602u;*p++=0x212;*p++=r3_shaders[shader].rsrc1;*p++=r3_shaders[shader].rsrc2;
    *p++=0xc0037602u;*p++=0x215;*p++=0;*p++=UINT32_MAX;*p++=UINT32_MAX;
    *p++=0xc0017602u;*p++=0x218;*p++=0;
    *p++=0xc0107602u;*p++=0x240;
    for(unsigned i=0;i<16;++i)*p++=users[i];
    p=r3_timestamp(p,g->timestamp_start);
    *p++=0xc0031502u;*p++=240;*p++=135;*p++=1;*p++=1;
    *p++=0xc0004600u;*p++=0x407;
    p=r3_acquire(p,0x80c40000u);p=r3_timestamp(p,g->timestamp_end);
    *p++=0xc0044700u;*p++=0x528;*p++=(uint32_t)label;
    *p++=0x40000000u|((uint32_t)(label>>32)&0xffffu);*p++=g->sequence;*p++=0;
    return (size_t)(p-output)*4;
}
static unsigned off_packet_comparisons;
static void check_r3_layout_and_packets(X4GpuConvert *g)
{
    size_t offset=0;
    for(unsigned i=0;i<8;++i){
        offset=(offset+255u)&~(size_t)255u;assert(g->code[i]==g->arena+offset);
        assert(!memcmp(g->code[i],r3_shaders[i].words,r3_shaders[i].bytes));offset+=r3_shaders[i].bytes;
    }
    offset=(offset+255u)&~(size_t)255u;
    assert((uint8_t *)g->dcb==g->arena+offset);offset+=4096;
    assert((uint8_t *)g->parameters==g->arena+offset);offset+=64;
    assert((uint8_t *)g->label==g->arena+offset);offset+=64;
    assert((uint8_t *)g->timestamp_start==g->arena+offset);offset+=64;
    assert((uint8_t *)g->timestamp_end==g->arena+offset);
    for(unsigned resolution=0;resolution<2;++resolution)for(unsigned mode=0;mode<4;++mode)
    for(unsigned matrix=0;matrix<4;++matrix)for(unsigned padding=0;padding<2;++padding){
        X4GpuConvertFrame f=frame;f.quality_word=0;
        f.color=(X4VideoPipelineColor){(matrix&1)?1u:6u,(matrix&2)!=0,
            mode?X4_CONVERT_SMOOTH:X4_CONVERT_ORIGINAL,mode?mode-1:0};
        unsigned width=resolution?1280u:1920u,height=resolution?720u:1080u;
        unsigned pitch=width+(padding?128u:0u),storage=resolution?720u:1088u;
        f.layout.visible_width=f.layout.coded_width=width;f.layout.visible_height=height;
        f.layout.coded_height=f.layout.storage_height=storage;f.layout.pitch=pitch;
        f.layout.y_bytes=(size_t)pitch*storage;f.layout.uv_bytes=f.layout.y_bytes/2;
        f.layout.uv_plane_offset=f.layout.visible_uv_offset=f.layout.y_bytes;
        f.layout.total_nv12_bytes=f.layout.y_bytes+f.layout.uv_bytes;
        f.destination_pitch=padding?1936u:1920u;
        f.destination_bytes=(size_t)f.destination_pitch*1080*4;
        static const int32_t coefficient[4][6]={
            {9539,13075,-3209,-6660,16525,16},{9539,14686,-1747,-4366,17305,16},
            {8192,11485,-2819,-5850,14516,0},{8192,12901,-1535,-3835,15201,0}};
        uint32_t expected_parameters[8]={pitch,f.destination_pitch};
        for(unsigned i=0;i<6;++i)expected_parameters[i+2]=(uint32_t)coefficient[matrix][i];
        parameters(g,&f);assert(!memcmp(g->parameters,expected_parameters,sizeof(expected_parameters)));
        for(unsigned i=8;i<16;++i)assert(g->parameters[i]==0);
        uint32_t expected[1024];
        size_t expected_bytes=r3_commands(g,&f,mode+(resolution?4u:0u),expected);
        uint32_t actual_bytes=commands(g,&f,mode+(resolution?4u:0u));
        assert(actual_bytes==expected_bytes && !memcmp(expected,g->dcb,actual_bytes));
        ++off_packet_comparisons;
    }
}
