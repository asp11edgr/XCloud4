/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "video_pipeline.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    X4_GPU_QUALITY_DITHER=1u,
    X4_GPU_QUALITY_DEBAND_SHIFT=1u,
    X4_GPU_QUALITY_DEBAND_MASK=6u,
    X4_GPU_QUALITY_CAS=8u,
    X4_GPU_QUALITY_REPEAT=16u,
    X4_GPU_QUALITY_MASK=31u,
    X4_GPU_QUALITY_WIDTH=1920,
    X4_GPU_QUALITY_HEIGHT=1080,
    X4_GPU_QUALITY_INTERMEDIATE_PIXEL_BYTES=16,
    X4_GPU_QUALITY_INTERMEDIATE_BYTES=33177600,
    X4_GPU_QUALITY_PARAMETER_BYTES=64,
    X4_GPU_QUALITY_NOISE_OFFSET=64,
    X4_GPU_QUALITY_NOISE_SIDE=64,
    X4_GPU_QUALITY_NOISE_COUNT=4096,
    X4_GPU_QUALITY_NOISE_BYTES=8192,
    X4_GPU_QUALITY_DATA_BYTES=8256,
    X4_GPU_QUALITY_REFERENCE_MAX_ROWS=16,
    X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES=552960
};
/* Shader ABI: Y/UV/output/parameters SRDs, sixteen user SGPRs; 8x8 group.
 * prepare output is RGB linear gamma-2, each pixel four IEEE binary32 words
 * in R,G,B,zero order. direct output and finish output use legacy A8B8G8R8 words.
 * finish SRDs are intermediate/noise/output/parameters; noise is also readable
 * through the parameters SRD at byte offset 64 for the direct shader.
 * The rank table is immutable after initialization. Only bytes 0..63 change.
 */
typedef struct {
    uint32_t source_pitch, destination_pitch;
    int32_t y, rv, gu, gv, bu, y_bias;
    uint32_t quality_word, sharpness, intermediate_pitch;
    uint32_t reserved[5];
} X4GpuQualityParameters;
_Static_assert(sizeof(X4GpuQualityParameters)==64,"quality parameter ABI");
_Static_assert(offsetof(X4GpuQualityParameters,quality_word)==32,"quality flags ABI");
_Static_assert(offsetof(X4GpuQualityParameters,sharpness)==36,"quality sharpness ABI");
_Static_assert(offsetof(X4GpuQualityParameters,intermediate_pitch)==40,"quality pitch ABI");

#if !X4_PUBLIC
unsigned x4_gpu_quality_settings_get(void);
bool x4_gpu_quality_settings_set(unsigned word);
#endif
uint32_t x4_gpu_quality_normalize(uint32_t word);
bool x4_gpu_quality_active(uint32_t word,unsigned sharpness);
/* Applicability is checked by the GPU owner. These helpers do not select CPU
 * or GPU, submit work, allocate memory, change options, or log. */
size_t x4_gpu_quality_reference_scratch_bytes(unsigned rows);
void x4_gpu_quality_parameters(X4GpuQualityParameters *,unsigned source_pitch,
    unsigned destination_pitch,unsigned intermediate_pitch,
    const X4VideoPipelineColor *,uint32_t quality_word);
const uint16_t *x4_gpu_quality_noise_ranks(void);
/* Compact destination: destination row zero corresponds to first_row.
 * Exactly rows*1920 words are written; all source reads clamp visible edges.
 * scratch must be float-aligned and contain scratch_bytes(rows), retained by
 * the caller. No allocation and no full-frame conversion occurs here.
 * Rows may be 1..16; host image generation calls this API repeatedly.
 */
int x4_gpu_quality_reference_rows(const X4VideoNv12Layout *,
    const uint8_t *source,size_t source_bytes,uint32_t *destination,
    size_t destination_bytes,unsigned first_row,unsigned rows,
    const X4VideoPipelineColor *,uint32_t quality_word,
    void *scratch,size_t scratch_bytes);
/* Host/reference primitive, independent of the generated LLVM entry: checks
 * parameters and source layout at the rows API boundary. Exposed for bounded
 * host tests and intermediate/quantization diagnostics only. */
void x4_gpu_quality_prepare_pixel(const uint8_t *y,const uint8_t *uv,
    const X4GpuQualityParameters *,unsigned x,unsigned row,float rgb[3]);
uint32_t x4_gpu_quality_finish_pixel(const float *rgb,unsigned pitch,
    unsigned first_row,unsigned available_rows,unsigned x,unsigned row,
    const X4GpuQualityParameters *);

enum {
    X4_GPU_VERIFY_ROWS=16,
    X4_GPU_VERIFY_NV12_CAPACITY=1920*1088*3/2,
    X4_GPU_VERIFY_RGBA_BYTES=16*1920*4,
    X4_GPU_VERIFY_NATIVE_BYTES=3276800,
    X4_GPU_VERIFY_CONTROL_BYTES=65536,
    X4_GPU_VERIFY_CPU_BYTES=1920*1080*4+X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES
};
/* Immutable values and private WB_ONION pixels, never native/scanout leases.
 * The token is valid from verification_take until verification_complete.
 * Only the copied bands are initialized. Halo bounds are part of the ABI and
 * tested against the unchanged row references with poisoned uncopied rows. */
typedef struct {
    uint64_t token,owner_epoch;
    X4VideoNv12Layout layout;
    X4VideoPipelineColor color;
    uint32_t quality_word;
    unsigned first_row,rows;
    unsigned first_y,rows_y,first_uv,rows_uv;
    const uint8_t *source;
    size_t source_bytes;
    const uint32_t *actual;
} X4GpuVerifySnapshot;
typedef struct {
    uint64_t token,owner_epoch,worker_us;
    int result;
    uint32_t checked_pixels,pixels_outside,max_channel_difference;
} X4GpuVerifyResult;
size_t x4_gpu_quality_verify_scratch_bytes(void);
/* Pure sparse-copy plan. Input has already passed GPU admission; this helper
 * also validates the geometry independently so the halo contract is testable. */
int x4_gpu_quality_verify_plan(const X4VideoNv12Layout *,const X4VideoPipelineColor *,
    uint32_t quality_word,unsigned first_row,unsigned rows,X4GpuVerifySnapshot *);
/* Background worker only. No allocation/native APIs or live-image reads.
 * scratch is exclusive to this worker. A single active streaming pipeline
 * owns the existing 720p reference scratch index 2; close joins this worker
 * before another pipeline may run its reference. */
int x4_gpu_quality_verify_snapshot(const X4GpuVerifySnapshot *,X4GpuVerifyResult *,
    void *scratch,size_t scratch_bytes);


