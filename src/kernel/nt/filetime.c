/* ============================================================================
 * filetime.c -- FILETIME conversions requiring full struct definitions
 *
 * The inline math helpers live in filetime.h. These functions need the
 * full struct rtc_time and struct efi_time definitions.
 * ============================================================================ */

#include "kernel/nt/filetime.h"
#include "kernel/drivers/rtc.h"
#include "kernel/uefi_runtime.h"

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
