/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Compile-time profile for the future DTLS transport. It intentionally
 * requires mbedtls_hardware_poll from a reviewed native entropy adapter.
 * No fallback seed, counter or pseudo-random stub is provided. These
 * archives cannot be used in XCloud4 until that adapter is implemented. */
#define MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_NET_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
