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

/* Non-inline conversions that need full struct definitions (in filetime.c) */
FILETIME filetime_from_rtc(const struct rtc_time *t);
FILETIME filetime_from_efi_time(const struct efi_time *t);

/* Unix epoch seconds -> FILETIME */
static inline FILETIME filetime_from_unix_seconds(uint64_t unix_sec)
{
    return (unix_sec + FILETIME_EPOCH_OFFSET_SECONDS) * FILETIME_TICKS_PER_SECOND;
}

/* FILETIME -> Unix epoch seconds */
static inline uint64_t filetime_to_unix_seconds(FILETIME ft)
{
    return (ft / FILETIME_TICKS_PER_SECOND) - FILETIME_EPOCH_OFFSET_SECONDS;
}

/* Calendar date/time -> days since 1601-01-01 (helper) */
static inline uint64_t filetime_days_from_date(uint16_t year, uint8_t month,
                                                uint8_t day)
{
    /* Days from 1601-01-01 to the given date.
     * Uses the same algorithm as the Windows kernel. */
    static const uint16_t cumdays[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    uint64_t y = (uint64_t)year - 1601;
    uint64_t days = y * 365 + y / 4 - y / 100 + y / 400;
    uint8_t m = (month > 0 && month <= 12) ? month - 1 : 0;

    days += cumdays[m];
    /* Add leap day if past February in a leap year */
    if (m >= 2) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (leap) days++;
    }
    days += (uint64_t)day - 1;

    return days;
}

/* FILETIME -> FAT32 DOS date/time (local time, 2-second resolution).
 * tz_bias_minutes: UTC offset in minutes (e.g., -120 for UTC+2).
 * FAT32 date: bits 15-9=year-1980, 8-5=month, 4-0=day
 * FAT32 time: bits 15-11=hours, 10-5=minutes, 4-0=2sec-counts */
static inline void filetime_to_dos_datetime(FILETIME ft, int tz_bias_minutes,
                                             uint16_t *date_out,
                                             uint16_t *time_out)
{
    /* Convert to local time by adding bias */
    int64_t local_ticks = (int64_t)ft
                        + (int64_t)tz_bias_minutes * 60 * FILETIME_TICKS_PER_SECOND;
    uint64_t total_secs = (uint64_t)local_ticks / FILETIME_TICKS_PER_SECOND;

    uint32_t secs_in_day = (uint32_t)(total_secs % 86400);
    uint64_t total_days  = total_secs / 86400;

    uint32_t hours = secs_in_day / 3600;
    uint32_t mins  = (secs_in_day / 60) % 60;
    uint32_t s2    = (secs_in_day % 60) / 2;

    /* Convert days since 1601 to year/month/day */
    uint32_t year = 1601;
    uint32_t month, day;
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    while (total_days >= 365) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t yd = leap ? 366 : 365;
        if (total_days < yd) break;
        total_days -= yd;
        year++;
    }

    month = 0;
    {
        uint32_t m;
        for (m = 0; m < 12; m++) {
            uint32_t d = dpm[m];
            if (m == 1 && (year % 4 == 0 &&
                (year % 100 != 0 || year % 400 == 0)))
                d = 29;
            if (total_days < d) break;
            total_days -= d;
            month++;
        }
    }
    day = (uint32_t)total_days + 1;
    month += 1;

    /* FAT32 year is offset from 1980; clamp if before 1980 or after 2107 */
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107;

    *time_out = (uint16_t)((hours << 11) | (mins << 5) | s2);
    *date_out = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

/* FAT32 DOS date/time -> FILETIME.
 * tz_bias_minutes: UTC offset in minutes. */
static inline FILETIME filetime_from_dos_datetime(uint16_t date, uint16_t time,
                                                   int tz_bias_minutes)
{
    uint32_t year  = ((date >> 9) & 0x7F) + 1980;
    uint32_t month = (date >> 5) & 0x0F;
    uint32_t day   = date & 0x1F;
    uint32_t hours = (time >> 11) & 0x1F;
    uint32_t mins  = (time >> 5) & 0x3F;
    uint32_t secs  = (time & 0x1F) * 2;

    uint64_t days = filetime_days_from_date((uint16_t)year, (uint8_t)month,
                                            (uint8_t)day);
    uint64_t total_secs = days * 86400 + hours * 3600 + mins * 60 + secs;
    FILETIME ft = total_secs * FILETIME_TICKS_PER_SECOND;

    /* Convert local time to UTC by subtracting bias */
    int64_t bias_ticks = (int64_t)tz_bias_minutes * 60 * FILETIME_TICKS_PER_SECOND;
    ft = (FILETIME)((int64_t)ft - bias_ticks);

    return ft;
}
