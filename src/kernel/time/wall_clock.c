/* ============================================================================
 * wall_clock.c -- Kernel wall clock (UTC FILETIME)
 *
 * Seeded at boot from UEFI GetTime or RTC. KeQuerySystemTime() reads the
 * wall clock by adding monotonic delta to the anchor point. Protected by
 * a seqlock for lock-free reads and safe writes.
 * ============================================================================ */

#include "kernel/time/wall_clock.h"
#include "kernel/time/mono_clock.h"
#include "kernel/nt/filetime.h"
#include "kernel/drivers/rtc.h"
#include "kernel/uefi_runtime.h"
#include "kernel/sched/seqlock.h"
#include "kernel/timer.h"
#include "kernel/klog.h"

/* ---- State --------------------------------------------------------------- */

static FILETIME  s_base_time;       /* UTC anchor (FILETIME) */
static uint64_t  s_base_mono_ns;    /* mono_ns() at anchor time */
static seqlock_t s_lock = SEQLOCK_INIT;
static int       s_ready;

/* ---- Init ---------------------------------------------------------------- */

void wall_clock_init(void)
{
    struct efi_time efi_t;
    FILETIME ft = FILETIME_NOW_PLACEHOLDER;
    const char *source = "none";

    /* Try UEFI GetTime first */
    {
        uint64_t status = uefi_get_time(&efi_t, (struct efi_time_capabilities *)0);
        if (status == 0 && efi_t.year >= 2000 && efi_t.year <= 2100) {
            ft = filetime_from_efi_time(&efi_t);
            source = "UEFI GetTime";
        }
    }

    /* Fallback: CMOS RTC */
    if (ft == FILETIME_NOW_PLACEHOLDER) {
        struct rtc_time rtc_t;
        rtc_read(&rtc_t);
        if (rtc_t.year >= 2000 && rtc_t.year <= 2100) {
            ft = filetime_from_rtc(&rtc_t);
            source = "RTC";
        }
    }

    /* Latch anchor */
    seqlock_write_lock(&s_lock);
    s_base_time = ft;
    s_base_mono_ns = mono_ns();
    s_ready = 1;
    seqlock_write_unlock(&s_lock);

    /* Log the seeded time */
    if (ft != FILETIME_NOW_PLACEHOLDER) {
        uint64_t unix_sec = filetime_to_unix_seconds(ft);
        /* Compute rough date for log (year/month/day from unix seconds) */
        uint32_t days = (uint32_t)(unix_sec / 86400);
        uint32_t year = 1970;
        static const uint8_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        uint32_t month, day, m;

        while (days >= 365) {
            int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
            uint32_t yd = leap ? 366 : 365;
            if (days < yd) break;
            days -= yd;
            year++;
        }
        month = 0;
        for (m = 0; m < 12; m++) {
            uint32_t d = dpm[m];
            if (m == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
                d = 29;
            if (days < d) break;
            days -= d;
            month++;
        }
        day = days + 1;
        month += 1;

        uint32_t secs_in_day = (uint32_t)(unix_sec % 86400);
        uint32_t h = secs_in_day / 3600;
        uint32_t mi = (secs_in_day / 60) % 60;
        uint32_t s = secs_in_day % 60;

        klog(LOG_INFO, "time",
             "Wall clock: %u-%02u-%02u %02u:%02u:%02u UTC (%s)",
             (uint64_t)year, (uint64_t)month, (uint64_t)day,
             (uint64_t)h, (uint64_t)mi, (uint64_t)s, source);
    } else {
        klog(LOG_WARN, "time", "Wall clock: no source -- using placeholder");
    }
}

/* ---- API ----------------------------------------------------------------- */

FILETIME KeQuerySystemTime(void)
{
    FILETIME base;
    uint64_t base_mono;
    uint32_t seq;

    if (!s_ready)
        return FILETIME_NOW_PLACEHOLDER;

    do {
        seq = seqlock_read_begin(&s_lock);
        base = s_base_time;
        base_mono = s_base_mono_ns;
    } while (seqlock_read_retry(&s_lock, seq));

    /* Current time = base + elapsed monotonic delta in FILETIME units */
    uint64_t elapsed_ns = mono_ns() - base_mono;
    return base + (elapsed_ns / 100);  /* ns -> 100 ns FILETIME ticks */
}

void KeSetSystemTime(FILETIME new_time)
{
    seqlock_write_lock(&s_lock);
    s_base_time = new_time;
    s_base_mono_ns = mono_ns();
    seqlock_write_unlock(&s_lock);

    klog(LOG_INFO, "time", "Wall clock set to FILETIME %u",
         (uint64_t)new_time);
}

int wall_clock_ready(void)
{
    return s_ready;
}

/* ---- Kernel time service API (§6) ---------------------------------------- */

void KeQueryTickCount(uint64_t *tick_count)
{
    if (tick_count)
        *tick_count = mono_filetime_units();
}

void KeQueryTimeIncrement(uint32_t *increment)
{
    /* Timer fires at 100 Hz -> 10 ms per tick -> 100000 * 100 ns units */
    if (increment)
        *increment = 100000;  /* 10 ms in 100 ns units */
}

void KeDelayExecutionThread(FILETIME interval)
{
    /* Convert 100 ns units to milliseconds */
    uint64_t ms = interval / FILETIME_TICKS_PER_MS;
    if (ms == 0) ms = 1;
    sleep_ms((uint32_t)(ms > 0xFFFFFFFF ? 0xFFFFFFFF : ms));
}

int time_service_ready(void)
{
    return s_ready;
}

/* ---- Interrupt time APIs (§7) -------------------------------------------- */

static volatile uint64_t s_interrupt_time_bias;  /* cumulative suspend bias */

uint64_t KeQueryInterruptTime(void)
{
    /* Interrupt time = monotonic ticks + suspend bias */
    return mono_filetime_units() + s_interrupt_time_bias;
}

uint64_t KeQueryInterruptTimePrecise(uint64_t *qpc_value)
{
    uint64_t it = mono_filetime_units() + s_interrupt_time_bias;
    if (qpc_value)
        *qpc_value = mono_filetime_units();  /* QPC = raw monotonic */
    return it;
}

uint64_t KeQueryUnbiasedInterruptTime(void)
{
    /* Unbiased = monotonic only (no suspend bias) */
    return mono_filetime_units();
}

void ke_suspend_bias_update(uint64_t bias_100ns)
{
    __atomic_fetch_add(&s_interrupt_time_bias, bias_100ns, __ATOMIC_SEQ_CST);
}
