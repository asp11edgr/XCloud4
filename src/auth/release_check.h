/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum X4UpdateStatus {
    X4_UPDATE_CHECKING = 0,
    X4_UPDATE_LATEST = 1,
    X4_UPDATE_AVAILABLE = 2,
    X4_UPDATE_FAILED = 3
};

/* Numerical version comparison ignoring any trailing '-suffix'.
 * Strips leading 'v' or 'lab-' if present.
 * Returns: <0 if v1 < v2, 0 if v1 == v2, >0 if v1 > v2. */
int x4_version_parse(const char *str, int *major, int *minor, int *patch);
int x4_version_compare(const char *v1, const char *v2);

/* Extract tag_name from GitHub API release JSON response.
 * - If is_public: parses JSON object, extracts "tag_name" (expected format vX.Y.Z[-suffix]).
 * - If !is_public (lab): parses JSON array of releases, finds first tag_name starting with "lab-".
 * Returns 0 on success, -1 on invalid JSON, missing tag, or no matching release. */
int x4_release_parse_tag(const char *json_body, size_t length, bool is_public,
    char *tag_out, size_t tag_cap, char *ver_out, size_t ver_cap);

/* Background update check lifecycle. Runs once at startup with a 10s total timeout. */
void x4_release_check_start(void);
void x4_release_check_stop(void);
enum X4UpdateStatus x4_release_check_status(char *version_out, size_t cap);

/* Main-thread public copy of the release tag, never a remote URL. Available
 * only for AVAILABLE and a complete 1..63 byte ASCII [A-Za-z0-9._-] tag.
 * Returns false and clears out when absent/invalid or capacity is too small. */
bool x4_release_check_tag(char *out, size_t capacity);

/* Testing/rendering override helper */
void x4_release_check_set_mock(enum X4UpdateStatus status, const char *version);
void x4_release_check_set_mock_tag(enum X4UpdateStatus status, const char *version, const char *tag);
