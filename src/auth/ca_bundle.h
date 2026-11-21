/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef X4_AUTH_CA_BUNDLE_H
#define X4_AUTH_CA_BUNDLE_H

#include <stddef.h>
#include <stdint.h>

#if __has_include(<orbis/Ssl.h>)
#  include <orbis/Ssl.h>
#else
typedef struct SslPem {
    char *ptr;
    size_t size;
} SslPem;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define X4_CA_BUNDLE_COUNT 11u
#define X4_CA_BUNDLE_RSA_COUNT 7u

typedef struct X4CaCertEntry {
    const char *name;
    const char *subject;
    const char *sha256_fingerprint;
    const char *pem_data;
    size_t pem_size;
    const uint8_t *der_data;
    size_t der_size;
} X4CaCertEntry;

/* Total count of bundled CA certificates */
size_t x4_ca_bundle_count(void);
size_t x4_ca_bundle_rsa_count(void);

/* Get metadata for a certificate entry by index */
const X4CaCertEntry *x4_ca_bundle_entry(size_t index);

/* Get array of SslPem pointer descriptors for OpenOrbis sceHttpsLoadCert */
const SslPem * const *x4_ca_bundle_get_ssl_pem_ptrs(size_t *out_count);
const SslPem * const *x4_ca_bundle_get_ssl_pem_ptrs_rsa(size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* X4_AUTH_CA_BUNDLE_H */
