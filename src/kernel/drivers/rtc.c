/* ============================================================================
 * rtc.c — CMOS Real-Time Clock driver
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
#include "kernel/printk.h"

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
 * NMI is disabled by setting bit 7 of the index port. */
static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_PORT_INDEX, (uint8_t)(0x80 | reg));  /* Disable NMI + select reg */
    return inb(CMOS_PORT_DATA);
}

/* Convert BCD to binary */
static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd & 0x0F) + ((bcd >> 4) * 10));
}

/* Wait until the RTC update-in-progress flag clears */
static void rtc_wait_ready(void)
{
    /* Poll Status Register A bit 7 */
    while (cmos_read(RTC_REG_STATUS_A) & 0x80)
        ;  /* spin */
}

/* ---- Public API ---- */

void rtc_init(void)
{
    struct rtc_time t;
    rtc_read(&t);
    printk("[OK] RTC: %u-%u-%u %u:%u:%u\n",
           (uint64_t)t.year, (uint64_t)t.month, (uint64_t)t.day,
           (uint64_t)t.hour, (uint64_t)t.minute, (uint64_t)t.second);
}

void rtc_read(struct rtc_time *t)
{
    uint8_t status_b;
    uint8_t century = 0;
    uint8_t sec, min, hr, day, mon, yr, wday;

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
    century = cmos_read(RTC_REG_CENTURY);

    /* Check Status Register B to see if values are BCD or binary */
    status_b = cmos_read(RTC_REG_STATUS_B);

    if (!(status_b & 0x04)) {
        /* BCD mode — convert to binary */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hr   = bcd_to_bin((uint8_t)(hr & 0x7F));  /* mask 12h/24h bit */
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
        century = bcd_to_bin(century);
    }

    /* Handle 12-hour mode → convert to 24-hour */
    if (!(status_b & 0x02) && (hr & 0x80)) {
        hr = (uint8_t)((hr & 0x7F) + 12);
        if (hr == 24) hr = 0;
    }

    /* Calculate full year */
    if (century > 0) {
        t->year = (uint16_t)(century * 100 + yr);
    } else {
        /* Assume 2000s if no century register */
        t->year = (uint16_t)(2000 + yr);
    }

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
