/* ============================================================================
 * filetime.h -- FILETIME type, epoch constants, and conversion math
 *
 * FILETIME is a 64-bit value representing 100-nanosecond intervals since
 * January 1, 1601 (UTC). This is the canonical time representation used by
 * all Windows NT kernel APIs and NTFS timestamps.
 *
 * Reference: Microsoft FILETIME documentation, Windows SDK
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- FILETIME type ------------------------------------------------------- */

typedef uint64_t FILETIME;

/* ---- Epoch constants ----------------------------------------------------- */

/* Seconds between FILETIME epoch (1601-01-01) and Unix epoch (1970-01-01) */
#define FILETIME_EPOCH_OFFSET_SECONDS   11644473600ULL

/* Same offset in 100 ns ticks */
#define FILETIME_EPOCH_OFFSET_100NS     116444736000000000ULL

/* Ticks per time unit */
#define FILETIME_TICKS_PER_SECOND       10000000ULL   /* 100 ns units */
#define FILETIME_TICKS_PER_MS           10000ULL
#define FILETIME_TICKS_PER_US           10ULL

/* Sentinel: returned before wall clock is initialized */
#define FILETIME_NOW_PLACEHOLDER        0ULL

/* ---- Conversion helpers (pure math, no hardware access) ------------------ */

/* Forward declarations */
struct rtc_time;
struct efi_time;

/* Non-inline conversions (in filetime.c) -- calendar math or struct-dependent */
FILETIME filetime_from_rtc(const struct rtc_time *t);
FILETIME filetime_from_efi_time(const struct efi_time *t);
uint64_t filetime_days_from_date_fn(uint16_t year, uint8_t month, uint8_t day);
void     filetime_to_dos_datetime(FILETIME ft, int tz_bias_minutes,
                                   uint16_t *date_out, uint16_t *time_out);
FILETIME filetime_from_dos_datetime(uint16_t date, uint16_t time,
                                     int tz_bias_minutes);

/* Unix epoch seconds -> FILETIME */
static inline FILETIME filetime_from_unix_seconds(uint64_t unix_sec)
{
    return (unix_sec + FILETIME_EPOCH_OFFSET_SECONDS) * FILETIME_TICKS_PER_SECOND;
}

/* FILETIME -> Unix epoch seconds; returns 0 for pre-Unix-epoch FILETIMEs */
static inline uint64_t filetime_to_unix_seconds(FILETIME ft)
{
    if (ft < FILETIME_EPOCH_OFFSET_100NS)
        return 0;
    return (ft / FILETIME_TICKS_PER_SECOND) - FILETIME_EPOCH_OFFSET_SECONDS;
}

/* Format FILETIME as ISO 8601 UTC string: "2026-03-25T14:35:22.123Z"
 * Writes up to len bytes (including NUL). Returns number of chars written. */
int filetime_to_string(FILETIME ft, char *buf, uint32_t len);

/* Convenience macro: call the non-inline function */
#define filetime_days_from_date(y, m, d) filetime_days_from_date_fn((y), (m), (d))
