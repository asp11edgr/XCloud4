/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile this test with PROBE/T26 enabled and with each disabled. It makes
 * no decoder, SDK, sysmodule, network or console calls. */
#include "../src/video/probe_t26_v2_abi_check.h"
#include <stdio.h>

#if X4_VIDEO_PROBE && X4_PROBE_T26
#ifndef X4_T26_V2_ABI_CHECKED
#error T26 v2 ABI assertions were not enabled for the probe
#endif
#else
#ifdef X4_T26_V2_ABI_CHECKED
#error T26 v2 ABI must not be introduced in a non-T26 build
#endif
#endif

int main(void)
{
#if X4_VIDEO_PROBE && X4_PROBE_T26
    puts("PASS T26 Videodec2 ABI: 7 sizes/alignments, every field offset/size; OutputInfo=48");
#else
    puts("PASS T26 Videodec2 ABI guard: no probe ABI introduced");
#endif
    return 0;
}
