/* SPDX-License-Identifier: GPL-3.0-only */
/* Link adapter for historical CPU-only pipeline harnesses. A GPU call is a
 * failure here, not a simulated success; dedicated GPU tests link the module. */
#include "../src/video/gpu_convert.h"
#include "../src/video/gpu_quality_ref.h"
#include "../src/ui/stream_settings.h"
#include <stdio.h>
#include <stdlib.h>

static _Noreturn void unexpected_gpu_call(const char *name)
{
    fprintf(stderr, "FAIL CPU-only pipeline called %s\n", name);
    abort();
}

unsigned x4_stream_settings_conversion_backend(void) { return 0; }
unsigned x4_gpu_quality_settings_get(void) { return 0; }
bool x4_gpu_quality_active(uint32_t word,unsigned sharpness)
{
    (void)sharpness;
    if(word)unexpected_gpu_call("nonzero quality in CPU-only fixture");
    return false;
}
int x4_gpu_quality_reference_rows(const X4VideoNv12Layout *layout,const uint8_t *source,size_t source_bytes,
    uint32_t *destination,size_t destination_bytes,unsigned first,unsigned rows,
    const X4VideoPipelineColor *color,uint32_t word,void *scratch,size_t scratch_bytes)
{
    (void)layout;(void)source;(void)source_bytes;(void)destination;(void)destination_bytes;
    (void)first;(void)rows;(void)color;(void)word;(void)scratch;(void)scratch_bytes;
    unexpected_gpu_call("x4_gpu_quality_reference_rows");
}
X4GpuConvert *x4_gpu_convert_open(void)
{ unexpected_gpu_call("x4_gpu_convert_open"); }
bool x4_gpu_convert_admit(const X4VideoNv12Layout *layout,X4VideoFormat format,
    const X4VideoPipelineColor *color,const char **reason)
{
    (void)layout;(void)format;(void)color;(void)reason;
    unexpected_gpu_call("x4_gpu_convert_admit");
}
int x4_gpu_convert_start(X4GpuConvert *state,const X4GpuConvertFrame *frame,uint64_t now_us)
{
    (void)state;(void)frame;(void)now_us;
    unexpected_gpu_call("x4_gpu_convert_start");
}
X4GpuConvertPoll x4_gpu_convert_poll(X4GpuConvert *state,uint64_t now_us,uint64_t *elapsed_us)
{
    (void)state;(void)now_us;(void)elapsed_us;
    unexpected_gpu_call("x4_gpu_convert_poll");
}
bool x4_gpu_convert_pending(const X4GpuConvert *state)
{ (void)state;unexpected_gpu_call("x4_gpu_convert_pending"); }
bool x4_gpu_convert_label_ready(X4GpuConvert *state)
{ (void)state;unexpected_gpu_call("x4_gpu_convert_label_ready"); }
bool x4_gpu_convert_disabled(const X4GpuConvert *state)
{ (void)state;unexpected_gpu_call("x4_gpu_convert_disabled"); }
const char *x4_gpu_convert_reason(const X4GpuConvert *state)
{ (void)state;unexpected_gpu_call("x4_gpu_convert_reason"); }
void x4_gpu_convert_status(const X4GpuConvert *state,X4GpuConvertStatus *status)
{ (void)state;(void)status;unexpected_gpu_call("x4_gpu_convert_status"); }
bool x4_gpu_convert_close(X4GpuConvert **state,uint64_t now_us)
{ (void)state;(void)now_us;unexpected_gpu_call("x4_gpu_convert_close"); }
int x4_gpu_convert_verification_prepare(X4GpuConvert *state)
{ (void)state;unexpected_gpu_call("x4_gpu_convert_verification_prepare"); }
bool x4_gpu_convert_verification_request(X4GpuConvert *state,unsigned first,unsigned rows)
{ (void)state;(void)first;(void)rows;unexpected_gpu_call("x4_gpu_convert_verification_request"); }
bool x4_gpu_convert_verification_take(X4GpuConvert *state,X4GpuVerifySnapshot *snapshot)
{ (void)state;(void)snapshot;unexpected_gpu_call("x4_gpu_convert_verification_take"); }
int x4_gpu_convert_verification_complete(X4GpuConvert *state,const X4GpuVerifyResult *result)
{ (void)state;(void)result;unexpected_gpu_call("x4_gpu_convert_verification_complete"); }
bool x4_gpu_convert_verification_collect(X4GpuConvert *state,X4GpuVerifyResult *result)
{ (void)state;(void)result;unexpected_gpu_call("x4_gpu_convert_verification_collect"); }
bool x4_gpu_convert_verification_cancel(X4GpuConvert *state)
{ (void)state;unexpected_gpu_call("x4_gpu_convert_verification_cancel"); }
size_t x4_gpu_quality_verify_scratch_bytes(void)
{ return X4_GPU_VERIFY_CPU_BYTES; }
int x4_gpu_quality_verify_snapshot(const X4GpuVerifySnapshot *snapshot,X4GpuVerifyResult *result,
    void *scratch,size_t scratch_bytes)
{ (void)snapshot;(void)result;(void)scratch;(void)scratch_bytes;unexpected_gpu_call("x4_gpu_quality_verify_snapshot"); }
