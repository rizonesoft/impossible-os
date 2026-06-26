/* ============================================================================
 * rtc.h -- CMOS Real-Time Clock driver
 *
 * Reads the current date and time from the CMOS RTC chip via I/O ports
 * 0x70 (index) and 0x71 (data).  The RTC keeps time even when the system
 * is powered off (backed by a battery on real hardware, emulated by QEMU).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- RTC time structure ---- */

struct rtc_time {
    uint8_t  second;    /* 0–59 */
    uint8_t  minute;    /* 0–59 */
    uint8_t  hour;      /* 0–23 */
    uint8_t  day;       /* 1–31 */
    uint8_t  month;     /* 1–12 */
    uint16_t year;      /* 4-digit year (e.g. 2026) */
    uint8_t  weekday;   /* 1–7 (Sunday = 1) */
};

/* ---- API ---- */

/* Initialize the RTC (call once at boot) */
void rtc_init(void);

/* Read the current date/time from the CMOS RTC.
 * Handles BCD-to-binary conversion and the update-in-progress flag.
 * On a no-CMOS platform (see rtc_available()) this zero-fills *t with a
 * clearly-invalid sentinel (year 0) and performs NO port I/O -- callers that
 * need a validity signal must use rtc_try_read() instead. */
void rtc_read(struct rtc_time *t);

/* Latch CMOS-RTC availability. Sets the fail-closed s_rtc_available flag true
 * ONLY when ACPI is ready AND the FADT does not report CMOS_RTC_NOT_PRESENT.
 * Must run on the BSP at Phase-1 boot (rtc_init() calls it) before any consumer
 * reads the clock; idempotent. */
void rtc_probe_availability(void);

/* Returns nonzero when a CMOS RTC is present and safe to access. Defaults to 0
 * (fail-closed) until rtc_probe_availability() latches it. */
int rtc_available(void);

/* Status-bearing read: returns 1 and fills *t only when the RTC is available
 * AND the read passes full date-tuple validation (rtc_time_plausible());
 * returns 0 otherwise (no usable time). Preferred over rtc_read() for any
 * caller that must distinguish "no clock" from "midnight year 2000". */
int rtc_try_read(struct rtc_time *t);

/* Pure validator: returns 1 when every field of *t is in range and the day is
 * valid for the month/year (Gregorian). Year accepted in the FILETIME-era
 * range [1601, 9999]. Side-effect-free -- unit-tested directly. */
int rtc_time_plausible(const struct rtc_time *t);

/* Get individual components (convenience wrappers) */
uint8_t  rtc_get_second(void);
uint8_t  rtc_get_minute(void);
uint8_t  rtc_get_hour(void);
uint8_t  rtc_get_day(void);
uint8_t  rtc_get_month(void);
uint16_t rtc_get_year(void);
