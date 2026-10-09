/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_native.h"
#include "../core/module.h"
#include <mbedtls/entropy.h>
#include <orbis/libkernel.h>
#include <stdint.h>
#include <stdio.h>

static int32_t (*native_random)(void *, size_t);
static int32_t (*native_exit)(const char *, const char *const *);

/* Dependency diagnostics accept only numeric events and status values.
 * No exception text, offer, ICE credential or provider response is logged. */
void x4_native_rtc_diagnostic(int event,int value)
{
    const char *label;
    switch(event) {
    case 1: label="invalid_argument"; break;
    case 2: label="system_error"; break;
    case 3: label="runtime_error"; break;
    case 4: label="exception"; break;
    case 5: label="MbedTLS check"; break;
    case 10: label="threadpool begin"; break;
    case 11: label="threadpool ready"; break;
    case 12: label="PSA begin"; break;
    case 13: label="PSA ready"; break;
    case 14: label="SCTP ready"; break;
    case 15: label="DTLS ready"; break;
    case 16: label="SRTP ready"; break;
    case 17: label="ICE ready"; break;
    case 18: label="workers created"; break;
    case 19: label="worker creation failed after"; break;
    default: label="unknown event"; break;
    }
    printf("XCloud4: RTC native %s value=%d (0x%08x)\n",label,value,(unsigned)value);
}

int x4_native_random(void *data, size_t size)
{
    if (!native_random || (!data && size)) return -1;
    uint8_t *out = data;
    while (size) {
        size_t count = size > 64 ? 64 : size;
        int rc = native_random(out, count);
        if (rc != 0) return -1;
        out += count;
        size -= count;
    }
    return 0;
}

int x4_native_entropy_prepare(void)
{
    if (native_random && native_exit) return 0;
    void *address = NULL;
    int handle = x4_module_open("libSceRandom");
    if (handle < 0 || x4_module_symbol(handle, "sceRandomGetRandomNumber", &address) < 0) return -1;
    native_random = (typeof(native_random))address;
    handle = x4_module_open("libSceSystemService");
    if (handle < 0 || x4_module_symbol(handle, "sceSystemServiceLoadExec", &address) < 0) return -1;
    native_exit = (typeof(native_exit))address;
    uint8_t probe[32];
    int rc = x4_native_random(probe, sizeof(probe));
    volatile uint8_t *wipe = probe;
    for (size_t i = 0; i < sizeof(probe); ++i) wipe[i] = 0;
    if (rc < 0) { native_random = NULL; return -1; }
    printf("XCloud4: RTC entropia nativa preparada\n");
    return 0;
}

int mbedtls_hardware_poll(void *context, unsigned char *output, size_t length, size_t *olen)
{
    (void)context;
    if (!olen) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = 0;
    if (x4_native_random(output, length) < 0) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = length;
    return 0;
}

void x4_native_random_or_exit(void *data, size_t size)
{
    if (x4_native_random(data, size) == 0) return;
    /* libjuice exposes a void random hook. Continuing would silently use
     * invalid ICE credentials. Fail closed through the confirmed native exit
     * path, without producing deterministic bytes or falling back to rand. */
    printf("XCloud4: fallo de entropia RTC; salida nativa\n");
    if (native_exit) native_exit("exit", NULL);
    for (;;) sceKernelUsleep(1000000);
}
