/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Native DTLS-SRTP profile. rtc_native.c supplies mbedtls_hardware_poll
 * from the native secure RNG. Consumers must use this same configuration;
 * no fallback seed, counter or pseudo-random stub is provided. */
#define MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#undef MBEDTLS_NET_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
