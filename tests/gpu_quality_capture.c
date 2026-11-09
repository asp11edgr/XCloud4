/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only comparison images. Inputs must be decoded 1080p NV12 fixtures;
 * this program does not call PS4 APIs or execute the GPU. */
#include "../src/video/gpu_quality_ref.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static X4VideoNv12Layout layout(void)
{
    size_t y=1920u*1088u;
    return (X4VideoNv12Layout){.visible_width=1920,.visible_height=1080,
        .coded_width=1920,.coded_height=1088,.storage_height=1088,.pitch=1920,
        .y_bytes=y,.uv_bytes=y/2,.uv_plane_offset=y,.total_nv12_bytes=y+y/2,
        .visible_uv_offset=y,.rgb_pixels=2073600,.rgb_bytes=8294400};
}
int main(int argc,char **argv)
{
    if(argc!=5){fprintf(stderr,"usage: capture fixture.nv12 matrix full output-prefix\n");return 2;}
    X4VideoNv12Layout l=layout();
    uint8_t *source=malloc(l.total_nv12_bytes);
    uint32_t *image=malloc(l.rgb_bytes),*compact=malloc(1920u*16u*4u);
    void *scratch=malloc(X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES);
    assert(source && image && compact && scratch);
    FILE *input=fopen(argv[1],"rb");assert(input);
    assert(fread(source,1,l.total_nv12_bytes,input)==l.total_nv12_bytes);
    assert(fgetc(input)==EOF);assert(!fclose(input));
    X4VideoPipelineColor color={(unsigned)strtoul(argv[2],NULL,10),
        strtoul(argv[3],NULL,10)!=0,X4_CONVERT_SMOOTH,X4_SHARPNESS_MED};
    const unsigned words[5]={0,1,3,5,11};
    const char *names[5]={"actual-suave-media","dither","dither-deband-suave",
        "dither-deband-fuerte","dither-deband-suave-cas-media"};
    for(unsigned variant=0;variant<5;++variant){
        if(!variant){
            for(unsigned i=0;i<2;++i)assert(!x4_video_pipeline_convert_stripe_color(&l,
                source,l.total_nv12_bytes,image,l.rgb_bytes,1920,2,i,&color));
        }else for(unsigned row=0;row<1080;row+=16){
            unsigned count=1080-row<16?1080-row:16;
            assert(!x4_gpu_quality_reference_rows(&l,source,l.total_nv12_bytes,
                compact,(size_t)count*1920*4,row,count,&color,words[variant],scratch,
                X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES));
            memcpy(image+(size_t)row*1920,compact,(size_t)count*1920*4);
        }
        char path[4096];int n=snprintf(path,sizeof(path),"%s-%s.ppm",argv[4],names[variant]);
        assert(n>0 && (size_t)n<sizeof(path));FILE *out=fopen(path,"wb");assert(out);
        assert(fprintf(out,"P6\n1920 1080\n255\n")>0);
        unsigned char rgb[1920*3];
        for(unsigned row=0;row<1080;++row){
            for(unsigned x=0;x<1920;++x){uint32_t p=image[(size_t)row*1920+x];
                rgb[x*3]=(unsigned char)p;rgb[x*3+1]=(unsigned char)(p>>8);rgb[x*3+2]=(unsigned char)(p>>16);}
            assert(fwrite(rgb,1,sizeof(rgb),out)==sizeof(rgb));
        }
        assert(!fclose(out));printf("created %s word=%u\n",path,words[variant]);fflush(stdout);
    }
    free(scratch);free(compact);free(image);free(source);return 0;
}
