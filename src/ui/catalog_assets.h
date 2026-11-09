/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "product_metadata.h"
#include "home_rows.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X4_CATALOG_ASSET_SLOTS 24u
#define X4_CATALOG_IMAGE_DOWNLOADERS 3u
#define X4_CATALOG_PRELOAD_PRODUCTS 16u
#define X4_CATALOG_THUMBNAIL_BUDGET (24u * 1024u * 1024u)
typedef struct X4CatalogAssets X4CatalogAssets;
enum X4CatalogAssetState {
    X4_ASSET_PENDING, X4_ASSET_DOWNLOADING, X4_ASSET_READY,
    X4_ASSET_TEMPORARY_FAILURE, X4_ASSET_MISSING,
    X4_ASSET_LOADING = X4_ASSET_PENDING
};
enum X4AssetStage { X4_ASSET_QUEUE, X4_ASSET_METADATA, X4_ASSET_IMAGE,
    X4_ASSET_DECODE, X4_ASSET_PUBLISH, X4_ASSET_STAGE_COUNT };
enum X4AssetSource { X4_ASSET_RAM, X4_ASSET_DISK, X4_ASSET_NETWORK };
enum X4AssetFault { X4_ASSET_FAULT_NONE, X4_ASSET_FAULT_ALLOCATION, X4_ASSET_FAULT_CLEANUP };
typedef uint64_t (*X4AssetClock)(void *context);
typedef struct {
    const char *disk_root; /* NULL disables disk (offline fixtures). */
    X4AssetClock clock_us; /* NULL uses the platform monotonic clock. */
    void *clock_context;
#ifdef X4_ASSETS_HOST_TEST
    unsigned image_downloaders; /* Zero uses one. Valid range is one to three. */
#endif
} X4CatalogAssetsConfig;
typedef struct { uint64_t count, total_us, max_us; } X4AssetTiming;
typedef struct {
    char id[X4_PRODUCT_ID_CAP];
    uint32_t mask;
} X4CatalogArtDemand;
#if !X4_PUBLIC
typedef struct {
    unsigned ram, disk, network, fallback, pending;
} X4CatalogArtReady;
#endif
#define X4_ASSET_EVENT_CAP 128u
typedef struct {
    uint64_t start_us, end_us, generation;
    char product_id[X4_PRODUCT_ID_CAP];
    enum X4AssetStage stage;
    enum X4AssetSource source;
} X4AssetEvent;
typedef struct {
    X4ProductMetadata metadata;
    X4CoverRaster surface[X4_COVER_SURFACES]; /* borrowed until release */
    enum X4CatalogAssetState state;
    uint64_t generation;
    unsigned slot;
} X4CatalogAssetView;
typedef struct {
    char product_id[X4_PRODUCT_ID_CAP];
    X4CoverRaster raster;
    uint64_t generation;
    unsigned slot;
} X4CatalogBackgroundView;
typedef struct {
    uint64_t requests, metadata_ready, covers_ready, failures, cancelled, stale_results;
    uint64_t revision;
    size_t thumbnail_bytes, thumbnail_peak_bytes, background_bytes;
    unsigned background_count;
    uint64_t sigl_requests;
    unsigned occupied, pending, pinned;
    int busy, suspended, disk_enabled;
    uint64_t ram_hits, disk_metadata_hits, disk_image_hits;
    uint64_t metadata_network_requests, image_network_requests, batch_splits, partial_responses;
    uint64_t image_download_requests[X4_CATALOG_IMAGE_DOWNLOADERS];
    uint64_t retries, deferred_stages, retry_suppressed, cleanup_failures, events_omitted;
    X4AssetTiming stage[X4_ASSET_STAGE_COUNT];
} X4CatalogAssetStats;

/* Main thread owns these APIs. A coordinator owns metadata, disk, decoding
 * and publication. Each image downloader owns its own HTTP context.
 * Starts suspended. create makes no HTTP request. */
X4CatalogAssets *x4_catalog_assets_create(void);
X4CatalogAssets *x4_catalog_assets_create_config(const X4CatalogAssetsConfig *config);
/* Reset exhausted temporary failures for demanded products. Retry-After
 * deadlines remain enforced. Permanent absence/invalid destinations do not retry. */
void x4_catalog_assets_retry(X4CatalogAssets *assets);
/* Call only at process shutdown; suspend first. The worker cleans up its own
 * HTTP resources before join. Navigation never calls this blocking join. */
void x4_catalog_assets_destroy(X4CatalogAssets *assets);
void x4_catalog_assets_set_generation(X4CatalogAssets *assets, uint64_t generation);
/* Replaces demand: selected first, visible catalog+Recents next, adjacent
 * pages last. Copies <=24 IDs; invalid/duplicate IDs are ignored. No HTTP. */
void x4_catalog_assets_request(X4CatalogAssets *assets, const char *const *product_ids, size_t count);
void x4_catalog_assets_request_surfaces(X4CatalogAssets *assets, const char *const *product_ids,
    const uint32_t *masks, size_t count);
/* On the main menu only. A miss never starts HTTP or changes retry state.
 * Pass NULL/0 when leaving the menu. Foreground demand always wins.
 * Returns 1 if accepted and 0 if contended or invalid. */
int x4_catalog_assets_preload(X4CatalogAssets *assets, const X4CatalogArtDemand *demand,
    size_t count);
/* Replaces the worker's disk-eviction protection for menu and first rows.
 * Updated by the main thread; no filesystem operation here. */
void x4_catalog_assets_protect(X4CatalogAssets *assets, const X4CatalogArtDemand *demand,
    size_t count);
#if !X4_PUBLIC
/* Coherent nonblocking view of visible art, attributed from entry time. */
int x4_catalog_assets_art_ready(X4CatalogAssets *assets, const X4CatalogArtDemand *demand,
    size_t count, uint64_t entered, X4CatalogArtReady *out);
#endif
/* Request after the focus debounce. The previous active image remains visible
 * until the replacement is ready. A NULL product clears that active image. */
void x4_catalog_assets_request_background(X4CatalogAssets *assets, const char *product_id);
/* Nonblocking view of this product's background in the current generation.
 * Returns 0 (without changing state) when unavailable or contended. */
int x4_catalog_assets_background_status(X4CatalogAssets *assets, const char *product_id,
    enum X4CatalogAssetState *state);
int x4_catalog_assets_acquire_background(X4CatalogAssets *assets, X4CatalogBackgroundView *view);
void x4_catalog_assets_release_background(X4CatalogAssets *assets, X4CatalogBackgroundView *view);
#define X4_GAME_SCREENSHOT_THUMB_BYTES (512u * 1024u)
#define X4_GAME_SCREENSHOT_FULL_BYTES (2u * 1024u * 1024u)
typedef struct {
    X4CoverRaster raster; /* borrowed until release, on the same UI draw */
    unsigned slot;
} X4GameScreenshotView;
typedef struct {
    uint64_t metadata_requests, thumbnail_requests, fullscreen_requests, disk_hits, cancelled;
    size_t raster_bytes, peak_raster_bytes;
} X4GameAssetStats;
/* Set one open game and optional fullscreen index. NULL cancels just this
 * view. Repeating the same input does not schedule more work. */
void x4_catalog_assets_game_request(X4CatalogAssets *assets, const char *product_id,
    int fullscreen_index);
uint64_t x4_catalog_assets_game_revision(X4CatalogAssets *assets);
int x4_catalog_assets_game_details(X4CatalogAssets *assets, const char *product_id,
    X4GameDetails *out, enum X4CatalogAssetState *state);
int x4_catalog_assets_game_acquire(X4CatalogAssets *assets, const char *product_id,
    unsigned index, bool fullscreen, X4GameScreenshotView *out);
void x4_catalog_assets_game_release(X4CatalogAssets *assets, X4GameScreenshotView *view);
int x4_catalog_assets_game_stats(X4CatalogAssets *assets, X4GameAssetStats *out);
/* A new account epoch invalidates the previous lists. Navigation alone does
 * not request the same lists again. Zero discards the account's lists. */
void x4_catalog_assets_request_sigls(X4CatalogAssets *assets, uint64_t session);
int x4_catalog_assets_copy_sigls(X4CatalogAssets *assets, X4SiglList out[X4_SIGL_COUNT],
    uint64_t *revision);
void x4_catalog_assets_suspend(X4CatalogAssets *assets);
/* Integration contract: resume only after the existing auth/network worker
 * is idle; suspend and await idle BEFORE starting auth/catalog/session work.
 * Thus existing global module/Net initialization never races. */
void x4_catalog_assets_resume(X4CatalogAssets *assets);
int x4_catalog_assets_idle(X4CatalogAssets *assets);
/* Enabled idle can retain public keep-alive resources. Foreground auth must
 * always suspend first, then await idle. A cleanup fault refuses that handoff. */
enum X4AssetFault x4_catalog_assets_fault(X4CatalogAssets *assets);
/* Nonblocking try-lock: missing/busy returns0; pins ownership before unlock.
 * Always release a successful acquire within the same draw. */
int x4_catalog_assets_acquire(X4CatalogAssets *assets, const char *product_id, X4CatalogAssetView *view);
void x4_catalog_assets_release(X4CatalogAssets *assets, X4CatalogAssetView *view);
/* Nonblocking snapshot of completed public metadata, not auth state. The
 * coordinator can retain names by product across LRU eviction. */
size_t x4_catalog_assets_copy_metadata(X4CatalogAssets *assets, X4ProductMetadata *out,
    size_t capacity, uint64_t *revision);
void x4_catalog_assets_stats(X4CatalogAssets *assets, X4CatalogAssetStats *out);
size_t x4_catalog_assets_events(X4CatalogAssets *assets, X4AssetEvent *out, size_t capacity);
/* Apply the menu's existing bottom gradient to a final-size card once. */
void x4_catalog_assets_compose_card(X4CoverRaster *raster);
