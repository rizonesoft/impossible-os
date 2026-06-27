/* ============================================================================
 * filetime.c -- FILETIME conversions (calendar math + struct-dependent)
 *
 * Trivial 1-line math (from/to_unix_seconds) stays inline in filetime.h.
 * Calendar math and struct-dependent conversions live here to avoid
 * duplicating loop code in every translation unit that includes the header.
 * ============================================================================ */

#include "kernel/nt/filetime.h"
#include "kernel/drivers/rtc.h"
#include "kernel/uefi_runtime.h"

/* ---- Calendar math ------------------------------------------------------- */

uint64_t filetime_days_from_date_fn(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint16_t cumdays[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    if (year < 1601) return 0;
    uint64_t y = (uint64_t)year - 1601;
    uint64_t days = y * 365 + y / 4 - y / 100 + y / 400;
    uint8_t m = (month > 0 && month <= 12) ? month - 1 : 0;

    days += cumdays[m];
    if (m >= 2) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (leap) days++;
    }
    days += (uint64_t)day - 1;
    return days;
}

void filetime_to_dos_datetime(FILETIME ft, int tz_bias_minutes,
                               uint16_t *date_out, uint16_t *time_out)
{
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t local_ticks = (int64_t)ft
                        + (int64_t)tz_bias_minutes * 60 * FILETIME_TICKS_PER_SECOND;
    if (local_ticks <= 0) {
        if (date_out) *date_out = (uint16_t)((0 << 9) | (1 << 5) | 1); /* 1980-01-01 */
        if (time_out) *time_out = 0;
        return;
    }
    uint64_t total_secs = (uint64_t)local_ticks / FILETIME_TICKS_PER_SECOND;
    uint32_t secs_in_day = (uint32_t)(total_secs % 86400);
    uint64_t total_days  = total_secs / 86400;
    uint32_t hours = secs_in_day / 3600;
    uint32_t mins  = (secs_in_day / 60) % 60;
    uint32_t s2    = (secs_in_day % 60) / 2;
    uint32_t year = 1601, month, day, m;

    while (total_days >= 365) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t yd = leap ? 366 : 365;
        if (total_days < yd) break;
        total_days -= yd;
        year++;
    }
    month = 0;
    for (m = 0; m < 12; m++) {
        uint32_t d = dpm[m];
        if (m == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
            d = 29;
        if (total_days < d) break;
        total_days -= d;
        month++;
    }
    day = (uint32_t)total_days + 1;
    month += 1;
    if (year < 1980) {
        year = 1980; month = 1; day = 1;
        hours = 0; mins = 0; s2 = 0;
    } else if (year > 2107) {
        year = 2107; month = 12; day = 31;
        hours = 23; mins = 59; s2 = 29; /* 58 seconds / 2 */
    }

    *time_out = (uint16_t)((hours << 11) | (mins << 5) | s2);
    *date_out = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
}

FILETIME filetime_from_dos_datetime(uint16_t date, uint16_t time,
                                     int tz_bias_minutes)
{
    uint32_t year  = ((date >> 9) & 0x7F) + 1980;
    uint32_t month = (date >> 5) & 0x0F;
    uint32_t day   = date & 0x1F;
    uint32_t hours = (time >> 11) & 0x1F;
    uint32_t mins  = (time >> 5) & 0x3F;
    uint32_t secs  = (time & 0x1F) * 2;

    uint64_t days = filetime_days_from_date_fn((uint16_t)year, (uint8_t)month,
                                                (uint8_t)day);
    uint64_t total_secs = days * 86400 + hours * 3600 + mins * 60 + secs;
    FILETIME ft = total_secs * FILETIME_TICKS_PER_SECOND;
    int64_t bias_ticks = (int64_t)tz_bias_minutes * 60 * FILETIME_TICKS_PER_SECOND;
    ft = (FILETIME)((int64_t)ft - bias_ticks);
    return ft;
}

/* ---- Struct-dependent conversions ---------------------------------------- */

FILETIME filetime_from_rtc(const struct rtc_time *t)
{
    uint64_t days = filetime_days_from_date(t->year, t->month, t->day);
    uint64_t secs = days * 86400
                  + (uint64_t)t->hour * 3600
                  + (uint64_t)t->minute * 60
                  + (uint64_t)t->second;
    return secs * FILETIME_TICKS_PER_SECOND;
}

FILETIME filetime_from_efi_time(const struct efi_time *t)
{
    uint64_t days = filetime_days_from_date(t->year, t->month, t->day);
    uint64_t secs = days * 86400
                  + (uint64_t)t->hour * 3600
                  + (uint64_t)t->minute * 60
                  + (uint64_t)t->second;
    FILETIME ft = secs * FILETIME_TICKS_PER_SECOND;

    /* Add nanosecond precision */
    ft += (uint64_t)t->nanosecond / 100;

    /* Apply timezone: EFI stores local time with tz offset in minutes.
     * Convert to UTC by subtracting the bias. 0x07FF = unspecified. */
    if (t->timezone != 0x07FF && t->timezone != 0) {
        int64_t bias_ticks = (int64_t)t->timezone * 60 * FILETIME_TICKS_PER_SECOND;
        ft = (FILETIME)((int64_t)ft - bias_ticks);
    }

    return ft;
}

/* ---- String formatting -------------------------------------------------- */

/* Helper: write a zero-padded decimal of width w into buf. */
static void put_dec(char *buf, uint32_t val, int w)
{
    int i;
    for (i = w - 1; i >= 0; i--) {
        buf[i] = '0' + (char)(val % 10);
        val /= 10;
    }
}

int filetime_to_string(FILETIME ft, char *buf, uint32_t len)
{
    /* "2026-03-25T14:35:22.123Z" = 24 chars + NUL */
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t total_secs, sub_sec_ticks;
    uint32_t secs_in_day, ms;
    uint64_t total_days;
    uint32_t year, month, day, h, mi, s, m;

    if (len < 25 || !buf) {
        if (buf && len > 0) buf[0] = '\0';
        return 0;
    }

    total_secs = ft / FILETIME_TICKS_PER_SECOND;
    sub_sec_ticks = ft % FILETIME_TICKS_PER_SECOND;
    ms = (uint32_t)(sub_sec_ticks / FILETIME_TICKS_PER_MS);

    secs_in_day = (uint32_t)(total_secs % 86400);
    total_days  = total_secs / 86400;
    h  = secs_in_day / 3600;
    mi = (secs_in_day / 60) % 60;
    s  = secs_in_day % 60;  /* always 0..59 -- never ":60" (leap second policy) */

    year = 1601;
    while (total_days >= 365) {
        int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t yd = leap ? 366 : 365;
        if (total_days < yd) break;
        total_days -= yd;
        year++;
    }
    month = 0;
    for (m = 0; m < 12; m++) {
        uint32_t d = dpm[m];
        if (m == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
            d = 29;
        if (total_days < d) break;
        total_days -= d;
        month++;
    }
    day = (uint32_t)total_days + 1;
    month += 1;

    /* "YYYY-MM-DDTHH:MM:SS.mmmZ" */
    put_dec(buf + 0,  year,  4); buf[4]  = '-';
    put_dec(buf + 5,  month, 2); buf[7]  = '-';
    put_dec(buf + 8,  day,   2); buf[10] = 'T';
    put_dec(buf + 11, h,     2); buf[13] = ':';
    put_dec(buf + 14, mi,    2); buf[16] = ':';
    put_dec(buf + 17, s,     2); buf[19] = '.';
    put_dec(buf + 20, ms,    3); buf[23] = 'Z';
    buf[24] = '\0';
    return 24;
}
