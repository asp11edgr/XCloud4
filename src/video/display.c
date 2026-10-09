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
int x4_display_present(X4Display *d)
{
    int rc = sceVideoOutSubmitFlip(d->handle, d->index, ORBIS_VIDEO_OUT_FLIP_VSYNC, d->frame);
    if (rc < 0) return rc;
    /* Complete this frame's graphics submission before waiting for scanout.
     * The 0.7.19 external suspension fault explicitly reported missing
     * submitDone. This does not replace VideoOut's buffer-ownership wait. */
    rc = sceGnmSubmitDone();
    if (d->frame == 1 || rc < 0)
        printf("XCloud4: Gnm submitDone result=0x%08x\n", (unsigned)rc);
    if (rc < 0) return rc;
    /* Bound the wait and never rewrite a buffer still being scanned out. */
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        OrbisVideoOutFlipStatus status = {0};
        rc = sceVideoOutGetFlipStatus(d->handle, &status);
        if (rc < 0) return rc;
        if (status.flipArg == d->frame) { d->index ^= 1; ++d->frame; return 0; }
        sceKernelUsleep(1000);
    }
    return -1;
}
void x4_display_close(X4Display *d)
{
    if (d->handle >= 0) { sceVideoOutClose(d->handle); d->handle = -1; }
    if (d->memory) { sceKernelMunmap(d->memory, d->size); d->memory = NULL; }
    if (d->allocated) { sceKernelReleaseDirectMemory(d->offset, d->size); d->allocated = 0; }
}
