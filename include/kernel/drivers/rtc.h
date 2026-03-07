/* ============================================================================
 * rtc.h — CMOS Real-Time Clock driver
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
 * Handles BCD-to-binary conversion and the update-in-progress flag. */
void rtc_read(struct rtc_time *t);

/* Get individual components (convenience wrappers) */
uint8_t  rtc_get_second(void);
uint8_t  rtc_get_minute(void);
uint8_t  rtc_get_hour(void);
uint8_t  rtc_get_day(void);
uint8_t  rtc_get_month(void);
uint16_t rtc_get_year(void);
