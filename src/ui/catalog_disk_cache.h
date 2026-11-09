/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "product_metadata.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define X4_CATALOG_DISK_ROOT "/data/xcloud4/cache/public-v1"
#define X4_CATALOG_DISK_BUDGET (192u * 1024u * 1024u)
#define X4_CATALOG_DISK_PRODUCTS 768u
#define X4_CATALOG_DISK_RECORDS 2048u
#define X4_CATALOG_DISK_PROTECTED 32u
#define X4_CATALOG_DISK_METADATA_TTL (7u * 24u * 60u * 60u)
#define X4_CATALOG_DISK_IMAGE_TTL (30u * 24u * 60u * 60u)

typedef struct X4CatalogDiskCache X4CatalogDiskCache;
typedef uint64_t (*X4CatalogDiskClock)(void *context);
typedef struct {
    uint64_t metadata_hits, image_hits, misses, stores, evictions, expired;
    uint64_t corrupt, io_errors, cancelled, scan_omitted, scan_ms;
    uint64_t miss_absent, miss_expired, miss_corrupt, miss_key, miss_evicted, miss_filtered;
    size_t bytes;
    unsigned records, products;
} X4CatalogDiskStats;

/* Single asset worker ONLY: all calls may perform bounded filesystem I/O.
 * NULL root disables caching (returns NULL). A supplied root must be an
 * absolute POSIX path; each directory is opened without following symlinks.
 * Native PS4 accepts ONLY X4_CATALOG_DISK_ROOT (fixed application directory).
 * It checks the filesystem on open, including the scan, and disables disk
 * caching if no safe operation is available. Only the Lab logs the check.
 * If futimes is unavailable, the record's advisory last-use word is updated
 * independently of its payload, key, and protected header fields.
 * No URL/product string is ever used as a filesystem path. Budget and
 * product count may be lowered from the declared defaults. Zero uses the
 * defaults; a separate fixed record limit bounds the index and startup scan.
 * clock is wall-clock UTC seconds (persistent expiry/LRU), NULL uses time().
 * Scan timing uses the platform monotonic clock independently. */
X4CatalogDiskCache *x4_catalog_disk_cache_open(const char *root, size_t budget,
    unsigned max_products, X4CatalogDiskClock clock, void *clock_context);
void x4_catalog_disk_cache_close(X4CatalogDiskCache *cache);
/* Replace the current worker-owned protection list (up to the declared
 * limit). Protects all stored variants for these products from capacity
 * eviction. Expiry, corruption and explicit clear still remove records.
 * A new worker reasserts its front-page
 * products once their IDs are known. NULL or zero clears the list. */
void x4_catalog_disk_cache_protect_products(X4CatalogDiskCache *cache,
    const char (*ids)[X4_PRODUCT_ID_CAP], unsigned count);
/* 1 = hit/stored; 0 = miss, disabled or cancelled; -1 = bad argument/I/O.
 * Corrupt/expired records are removed and return 0. Callers fall back to the
 * public network path. Only typed public display metadata is persisted;
 * no account, entitlement, token, generation or recent-history fields exist. */
int x4_catalog_disk_cache_get_metadata(X4CatalogDiskCache *cache, const char *id,
    const char *market, const char *language, X4ProductMetadata *out,
    const _Atomic int *cancel);
int x4_catalog_disk_cache_put_metadata(X4CatalogDiskCache *cache, const char *id,
    const char *market, const char *language, const X4ProductMetadata *metadata,
    const _Atomic int *cancel);
int x4_catalog_disk_cache_get_image(X4CatalogDiskCache *cache, const char *id,
    const char *market, const char *language, const char *url, unsigned char *bytes,
    size_t capacity, size_t *length, const _Atomic int *cancel);
/* Store only after successful decode; this layer additionally validates
 * signatures, dimensions, lengths and checksum. It never owns a decoder. */
int x4_catalog_disk_cache_put_image(X4CatalogDiskCache *cache, const char *id,
    const char *market, const char *language, const char *url,
    const unsigned char *bytes, size_t length, const _Atomic int *cancel);
/* A real decoder rejected a disk hit: discard only that public variant,
 * count it as corrupt, then let the caller retry its bounded network path. */
int x4_catalog_disk_cache_remove_image(X4CatalogDiskCache *cache, const char *id,
    const char *market, const char *language, const char *url);
/* Remove only this cache's generated regular-file names, never symlinks or
 * unrelated files. A cancelled clear may remove a prefix of records. */
int x4_catalog_disk_cache_clear(X4CatalogDiskCache *cache, const _Atomic int *cancel);
void x4_catalog_disk_cache_stats(const X4CatalogDiskCache *cache, X4CatalogDiskStats *out);
