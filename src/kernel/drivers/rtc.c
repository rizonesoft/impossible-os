/* ============================================================================
 * rtc.c -- CMOS Real-Time Clock driver
 *
 * Reads the date/time from the MC146818-compatible CMOS RTC chip.
 * The CMOS is accessed via two I/O ports:
 *   - Port 0x70: index register (write the register number to read)
 *   - Port 0x71: data register (read the value)
 *
 * The RTC stores values in BCD format by default.  We check Status Register B
 * to determine if BCD conversion is needed.
 *
 * To avoid reading during an update, we poll Status Register A bit 7
 * (update-in-progress) and wait until it clears before reading.
 * ============================================================================ */

#include "kernel/drivers/rtc.h"
#include "kernel/acpi.h"
#include "kernel/klog.h"

/* ---- Availability latch ----
 * Fail-closed: stays 0 (no CMOS) until rtc_probe_availability() proves a CMOS
 * RTC is present. Written once on the BSP at Phase-1 boot before any consumer
 * reads the clock and never mutated afterward, so plain reads are SMP-safe (no
 * concurrent writer exists once boot has moved past rtc_init()). */
static int s_rtc_available;

/* ---- I/O port helpers ---- */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- CMOS register numbers ---- */

#define CMOS_PORT_INDEX  0x70
#define CMOS_PORT_DATA   0x71

#define RTC_REG_SECONDS  0x00
#define RTC_REG_MINUTES  0x02
#define RTC_REG_HOURS    0x04
#define RTC_REG_WEEKDAY  0x06
#define RTC_REG_DAY      0x07
#define RTC_REG_MONTH    0x08
#define RTC_REG_YEAR     0x09
#define RTC_REG_CENTURY  0x32   /* Not always available */
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

/* ---- Internal helpers ---- */

/* Read a single CMOS register.
 * NMI is disabled by setting bit 7 of the index port.
 *
 * HARD GATE (safety boundary): this is the ONLY site that touches ports
 * 0x70/0x71, so the availability check lives here. On a no-CMOS platform
 * (hardware-reduced ACPI, or FADT CMOS_RTC_NOT_PRESENT) we must not drive the
 * index/data ports at all -- on such firmware those ports are reserved and a
 * write can have undefined side effects. Returning the bus-float value (0xFF)
 * keeps any missed consumer safe without an out-of-bounds port access. */
static uint8_t cmos_read(uint8_t reg)
{
    if (!s_rtc_available)
        return 0xFF;
    outb(CMOS_PORT_INDEX, (uint8_t)(0x80 | reg));  /* Disable NMI + select reg */
    return inb(CMOS_PORT_DATA);
}

/* Convert BCD to binary */
static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd & 0x0F) + ((bcd >> 4) * 10));
}

/* Wait until the RTC update-in-progress flag clears (with timeout) */
static void rtc_wait_ready(void)
{
    uint32_t timeout = 100000;  /* ~1ms on modern CPUs */
    while ((cmos_read(RTC_REG_STATUS_A) & 0x80) && --timeout > 0)
        ;  /* spin */
}

/* ---- Public API ---- */

void rtc_probe_availability(void)
{
    /* acpi_has_cmos_rtc() is fail-OPEN: it reports "present" on a missing or
     * short FADT so legacy PCs keep their RTC. That is the wrong default for a
     * safety gate, so require acpi_is_ready() too -- if ACPI never validated,
     * we cannot trust the FADT CMOS bit and must refuse the ports. On the UEFI
     * platforms this OS targets ACPI is always present, so this only fails
     * closed on genuinely RTC-less (hardware-reduced) firmware. */
    s_rtc_available = (acpi_is_ready() && acpi_has_cmos_rtc()) ? 1 : 0;
}

int rtc_available(void)
{
    return s_rtc_available;
}

uint16_t rtc_resolve_year(uint8_t two_digit_year, uint8_t century,
                          int century_present)
{
    /* A valid RTC year byte is 0-99 in both BCD and binary mode; >99 means a
     * corrupt/absent register (bus-float 0xFF -> 165 BCD / 255 binary). Return
     * year 0 so rtc_time_plausible() REJECTS it -- folding garbage into a
     * plausible year (e.g. 2000 + 255 % 100) would silently seed a wrong clock
     * instead of failing closed to the UEFI GetTime source. */
    if (two_digit_year > 99)
        return 0;
    /* Trust the century only when advertised AND plausible (1900-2199); a
     * century-less register reads bus-float, which would otherwise yield a
     * wild year and -- via rtc_try_read() validation -- wrongly latch a
     * working RTC unavailable. */
    if (century_present && century >= 19 && century <= 21)
        return (uint16_t)(century * 100 + two_digit_year);
    return (uint16_t)(2000 + two_digit_year);
}

int rtc_time_plausible(const struct rtc_time *t)
{
    static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t maxday;
    int leap;

    if (!t)
        return 0;
    /* FILETIME epoch is 1601; the RTC century register tops out well under
     * 9999. Anything outside this band is a failed/absent read, not a date. */
    if (t->year < 1601 || t->year > 9999)
        return 0;
    if (t->month < 1 || t->month > 12)
        return 0;
    if (t->hour > 23 || t->minute > 59 || t->second > 59)
        return 0;

    leap = (t->year % 4 == 0 && (t->year % 100 != 0 || t->year % 400 == 0));
    maxday = dpm[t->month - 1];
    if (t->month == 2 && leap)
        maxday = 29;
    if (t->day < 1 || t->day > maxday)
        return 0;

    return 1;
}

int rtc_try_read(struct rtc_time *t)
{
    if (!t)
        return 0;
    if (!s_rtc_available)
        return 0;
    rtc_read(t);
    return rtc_time_plausible(t);
}

void rtc_init(void)
{
    struct rtc_time t;

    rtc_probe_availability();
    if (!s_rtc_available) {
        klog(LOG_INFO, "rtc", "RTC: no CMOS (ACPI gate) -- skipped");
        return;
    }

    if (!rtc_try_read(&t)) {
        klog(LOG_WARN, "rtc",
             "RTC: read failed validation -- treating as unavailable");
        s_rtc_available = 0;
        return;
    }

    klog(LOG_INFO, "rtc", "RTC: %u-%u-%u %u:%u:%u",
           (uint64_t)t.year, (uint64_t)t.month, (uint64_t)t.day,
           (uint64_t)t.hour, (uint64_t)t.minute, (uint64_t)t.second);
}

void rtc_read(struct rtc_time *t)
{
    uint8_t status_b;
    uint8_t century = 0;
    uint8_t sec, min, hr, day, mon, yr, wday;
    uint8_t century_idx;

    if (!t)
        return;

    /* No CMOS RTC: zero-fill with a clearly-invalid sentinel (year 0) so a
     * direct caller never mistakes a refused read for midnight in year 2000,
     * and touch no ports. */
    if (!s_rtc_available) {
        t->second = t->minute = t->hour = 0;
        t->day = t->month = t->weekday = 0;
        t->year = 0;
        return;
    }

    /* Wait for any in-progress update to finish */
    rtc_wait_ready();

    /* Read all registers */
    sec  = cmos_read(RTC_REG_SECONDS);
    min  = cmos_read(RTC_REG_MINUTES);
    hr   = cmos_read(RTC_REG_HOURS);
    wday = cmos_read(RTC_REG_WEEKDAY);
    day  = cmos_read(RTC_REG_DAY);
    mon  = cmos_read(RTC_REG_MONTH);
    yr   = cmos_read(RTC_REG_YEAR);

    /* Read the century byte only from the register the FADT advertises (0 =
     * none); never the hardcoded 0x32 on a platform that lacks one. */
    century_idx = acpi_rtc_century_index();
    century = century_idx ? cmos_read(century_idx) : 0;

    /* Check Status Register B to see if values are BCD or binary */
    status_b = cmos_read(RTC_REG_STATUS_B);

    if (!(status_b & 0x04)) {
        /* BCD mode -- convert to binary */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hr   = bcd_to_bin((uint8_t)(hr & 0x7F));  /* mask 12h/24h bit */
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
        if (century_idx)
            century = bcd_to_bin(century);
    }

    /* Handle 12-hour mode: convert to 24-hour */
    if (!(status_b & 0x02) && (hr & 0x80)) {
        hr = (uint8_t)((hr & 0x7F) + 12);
        if (hr == 24) hr = 0;
    }

    t->year = rtc_resolve_year(yr, century, century_idx != 0);

    t->second  = sec;
    t->minute  = min;
    t->hour    = hr;
    t->day     = day;
    t->month   = mon;
    t->weekday = wday;
}

uint8_t rtc_get_second(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return t.second;
}

uint8_t rtc_get_minute(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return t.minute;
}

uint8_t rtc_get_hour(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return t.hour;
}

uint8_t rtc_get_day(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return t.day;
}

uint8_t rtc_get_month(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return t.month;
}

uint16_t rtc_get_year(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return t.year;
}
