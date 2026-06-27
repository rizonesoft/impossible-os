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

/* ---- Leap second policy -------------------------------------------------- *
 * FILETIME_LEAP_SECOND_POLICY: FILETIME counts SI seconds, NOT UTC seconds.
 *
 * The 100 ns tick count since 1601-01-01 assumes every day has exactly 86400
 * seconds. Leap seconds are NOT counted -- this matches Win32 (FileTimeTo
 * SystemTime returns wSecond 0..59 only; the 60th second of a leap event is
 * never represented) AND POSIX (time_t mandates 86400 s/day, also ignoring
 * leap seconds). Both endpoints of every conversion in this file share that
 * convention, so FILETIME <-> Unix arithmetic and round-trips stay exact.
 *
 * Consequences enforced here:
 *   - filetime_from_unix_seconds()/filetime_to_unix_seconds() are pure linear
 *     scale + offset: no leap-second table, no per-day correction.
 *   - filetime_to_string() derives the seconds field as (secs_in_day % 60),
 *     so it can never emit ":60".
 *   - The NTP adjustment hooks discipline WALL TIME phase/frequency only; they
 *     do not smear or insert leap seconds, and never touch the monotonic clock
 *     (QPC / interrupt time stay raw).
 *
 * Reference: Microsoft FILETIME docs; POSIX.1-2017 4.16 "Seconds Since the
 * Epoch". If true UTC-with-leap-seconds is ever required, it belongs in a
 * separate UTC-aware layer, never retrofitted into these linear conversions.
 * ------------------------------------------------------------------------- */

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

/* Unix epoch seconds -> FILETIME. Pure linear scale + offset: no leap-second
 * correction (both epochs use 86400 s/day -- see FILETIME_LEAP_SECOND_POLICY). */
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
