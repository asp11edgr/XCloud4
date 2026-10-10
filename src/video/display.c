/* SPDX-License-Identifier: GPL-3.0-only */
/* VideoOut sequence adapted from OpenOrbis samples/_common/graphics.cpp. */
#include "display.h"
#include <string.h>
#include <orbis/VideoOut.h>
#include <orbis/GnmDriver.h>
#include <stdio.h>
int x4_display_open(X4Display *d)
{
    memset(d, 0, sizeof(*d));
    d->handle = -1;
    d->frame = 1;
    const size_t alignment = 0x200000;
    const size_t pixels_size = (size_t)X4_WIDTH * X4_HEIGHT * sizeof(uint32_t);
    const size_t buffer_size = (pixels_size + alignment - 1) & ~(alignment - 1);
    d->size = buffer_size * 2;
    d->handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    int rc = d->handle;
    if (rc < 0) goto fail;
    rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), d->size, alignment, 3, &d->offset);
    if (rc < 0) goto fail;
    d->allocated = 1;
    rc = sceKernelMapDirectMemory(&d->memory, d->size, 0x33, 0, d->offset, alignment);
    if (rc < 0) goto fail;
    memset(d->memory, 0, d->size);
    d->buffers[0] = d->memory;
    d->buffers[1] = (uint32_t *)((uint8_t *)d->memory + buffer_size);
    OrbisVideoOutBufferAttribute attr = {0};
    sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
        ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
        X4_WIDTH, X4_HEIGHT, X4_WIDTH);
    rc = sceVideoOutRegisterBuffers(d->handle, 0, (void *const *)d->buffers, 2, &attr);
    if (rc < 0) goto fail;
    rc = sceVideoOutSetFlipRate(d->handle, ORBIS_VIDEO_OUT_FLIP_60HZ);
    if (rc < 0) goto fail;
    return 0;
fail:
    x4_display_close(d);
    return rc;
}
uint32_t *x4_display_pixels(X4Display *d) { return d->buffers[d->index]; }
static int present(X4Display *d, X4Trace *trace, uint64_t generation, bool fresh)
{
    uint16_t flags = fresh ? X4_TRACE_F_NEW : X4_TRACE_F_REPEAT;
    uint64_t submitted = (uint64_t)d->frame;
    x4_trace_monitor_state(trace, X4_MON_ACTOR_DISPLAY, X4_MON_PHASE_FLIP_SUBMIT, 0);
    x4_trace_record(trace, X4_TRACE_FLIP_CALL_BEGIN, flags, generation, submitted);
    int rc = sceVideoOutSubmitFlip(d->handle, d->index, ORBIS_VIDEO_OUT_FLIP_VSYNC, d->frame);
    if (rc < 0) {
        x4_trace_record(trace, X4_TRACE_FLIP_FAIL, flags | X4_TRACE_REASON(X4_TRACE_R_SUBMIT),
                        generation, (uint32_t)rc);
        return rc;
    }
    x4_trace_record(trace, X4_TRACE_FLIP_SUBMIT, flags, generation, submitted);
    /* Complete this frame's graphics submission before waiting for scanout.
     * The 0.7.19 external suspension fault explicitly reported missing
     * submitDone. This does not replace VideoOut's buffer-ownership wait. */
    rc = sceGnmSubmitDone();
    if (d->frame == 1 || rc < 0)
        printf("XCloud4: Gnm submitDone result=0x%08x\n", (unsigned)rc);
    if (rc < 0) {
        x4_trace_record(trace, X4_TRACE_FLIP_FAIL, flags | X4_TRACE_REASON(X4_TRACE_R_GNM),
                        generation, (uint32_t)rc);
        return rc;
    }
    x4_trace_record(trace, X4_TRACE_GNM_DONE, flags, generation, (uint32_t)rc);
    x4_trace_monitor_state(trace, X4_MON_ACTOR_DISPLAY, X4_MON_PHASE_FLIP_WAIT, 0);
    /* Bound the wait and never rewrite a buffer still being scanned out. */
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        x4_trace_poll(trace);
        OrbisVideoOutFlipStatus status = {0};
        rc = sceVideoOutGetFlipStatus(d->handle, &status);
        if (rc < 0) {
            x4_trace_record(trace, X4_TRACE_FLIP_FAIL, flags | X4_TRACE_REASON(X4_TRACE_R_STATUS),
                            generation, (uint32_t)rc);
            return rc;
        }
        if (status.flipArg == d->frame) {
            /* This observation updates NEW cadence even if the event ring
             * cannot admit a record. Repeat/UI matches never reset it. */
            x4_trace_present_complete(trace, generation, fresh, status.num, submitted);
            x4_trace_monitor_state(trace, X4_MON_ACTOR_DISPLAY, X4_MON_PHASE_MAIN_IDLE, 0);
            d->index ^= 1; ++d->frame; return 0;
        }
        sceKernelUsleep(1000);
    }
    x4_trace_record(trace, X4_TRACE_FLIP_FAIL, flags | X4_TRACE_REASON(X4_TRACE_R_TIMEOUT),
                    generation, (uint32_t)-1);
    return -1;
}
int x4_display_present(X4Display *d) { return present(d, NULL, 0, false); }
int x4_display_present_trace(X4Display *d, X4Trace *trace, uint64_t generation, bool fresh)
{
    return present(d, trace, generation, fresh);
}
void x4_display_close(X4Display *d)
{
    if (d->handle >= 0) { sceVideoOutClose(d->handle); d->handle = -1; }
    if (d->memory) { sceKernelMunmap(d->memory, d->size); d->memory = NULL; }
    if (d->allocated) { sceKernelReleaseDirectMemory(d->offset, d->size); d->allocated = 0; }
}
