/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_native.h"
#include "../core/module.h"
#include <mbedtls/entropy.h>
#include <orbis/libkernel.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

static int32_t (*native_random)(void *, size_t);
static int32_t (*native_exit)(const char *, const char *const *);

/* Dependency diagnostics accept only numeric events and status values.
 * No exception text, offer, ICE credential or provider response is logged. */
void x4_native_rtc_diagnostic(int event,int value)
{
    int saved_errno = errno;
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
    case 20: label="offer ICE begin"; break;
    case 21: label="offer ICE ready"; break;
    case 22: label="offer ICE description ready"; break;
    case 23: label="offer populated media count"; break;
    case 24: label="offer committed"; break;
    case 25: label="offer gathering begin"; break;
    case 26: label="offer gathering ready"; break;
    case 30: label="juice agent begin"; break;
    case 31: label="juice agent result"; break;
    case 32: label="juice local description result"; break;
    case 33: label="juice gathering result"; break;
    case 35: label="ICE underlying exception class"; break;
    case 40: label="certificate begin"; break;
    case 41: label="certificate key ready"; break;
    case 42: label="certificate times ready"; break;
    case 43: label="certificate serial ready"; break;
    case 44: label="certificate DER result"; break;
    case 45: label="certificate ready"; break;
    case 48: label="certificate UTC conversion result"; break;
    case 49: label="certificate time formatted length"; break;
    case 50: label="UDP socket failed errno"; break;
    case 51: label="UDP get flags failed errno"; break;
    case 52: label="UDP set flags failed errno"; break;
    case 53: label="UDP bind failed errno"; break;
    case 54: label="UDP bind address resolution result"; break;
    case 55: label="juice connection result"; break;
    case 56: label="juice host candidate count"; break;
    case 57: label="juice resolver thread result"; break;
    case 58: label="juice poll thread result"; break;
    case 59: label="juice poll pipe errno"; break;
    case 60: label="juice poll read pipe flags errno"; break;
    case 61: label="juice poll write pipe flags errno"; break;
    case 62: label="native interface query result"; break;
    case 63: label="native interface result count"; break;
    case 64: label="socket nonblock errno"; break;
    case 65: label="socket SO_NBIO set errno"; break;
    case 66: label="socket SO_NBIO get errno"; break;
    case 67: label="socket SO_NBIO mode"; break;
    case 68: label="socket SO_NBIO option size"; break;
    case 69: label="DNS resolver create result"; break;
    case 70: label="DNS lookup timeout usec"; break;
    case 71: label="DNS lookup result"; break;
    case 72: label="DNS lookup elapsed usec"; break;
    case 73: label="DNS address nonzero"; break;
    case 74: label="juice clock failed result"; break;
    case 75: label="juice clock failed errno"; break;
    case 76: label="juice ICE failure reason"; break;
    case 77: label="juice ICE failure detail"; break;
    case 78: label="DTLS initialization exception class"; break;
    case 79: label="DTLS initialization exception code"; break;
    case 80: label="DTLS transport state"; break;
    case 81: label="DTLS receive exception class"; break;
    case 82: label="DTLS receive exception code"; break;
    case 83: label="SRTP inbound create failure"; break;
    case 84: label="SRTP outbound create failure"; break;
    case 85: label="SRTP key derivation stage"; break;
    case 86: label="DTLS construction stage"; break;
    case 87: label="SCTP transport state"; break;
    case 88: label="juice failed candidate pair count"; break;
    case 89: label="juice failed remote candidate count"; break;
    case 90: label="juice failed STUN entry count"; break;
    default: label="unknown event"; break;
    }
    printf("XCloud4: RTC native %s value=%d (0x%08x)\n",label,value,(unsigned)value);
    errno = saved_errno;
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
