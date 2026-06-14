/* ============================================================================
 * time_iso.h -- Unix-time to ISO-8601 formatting (freestanding, no libc).
 *
 * Shared civil-date formatter so callers that need a human-readable UTC
 * timestamp string do not each re-implement the days_from_civil inverse.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Format `unix_time` (seconds since 1970-01-01 UTC) as a 20-character ISO-8601
 * string "YYYY-MM-DDTHH:MM:SSZ" plus a NUL terminator into `out` (which MUST be
 * at least 21 bytes). unix_time == 0 yields the epoch. The year is clamped to
 * [1970, 9999]. Howard Hinnant's days_from_civil inverse; no floating point. */
void kdate_iso8601(uint64_t unix_time, char out[21]);
