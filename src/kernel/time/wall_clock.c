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
#include "kernel/sched/irql.h"
#include "kernel/timer.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/klog.h"

/* ---- State --------------------------------------------------------------- */

static FILETIME  s_base_time;       /* UTC anchor (FILETIME) */
static uint64_t  s_base_mono_ns;    /* mono_ns() at anchor time */
static seqlock_t s_lock = SEQLOCK_INIT;
static int       s_ready;
static int       s_time_sourced;    /* 1 only when seeded from a REAL source
                                     * (UEFI/RTC/KeSetSystemTime), not the
                                     * placeholder -- gates absolute deadlines */

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

    /* Fallback: CMOS RTC. rtc_try_read() both honors the no-CMOS absence gate
     * (no port I/O on hardware-reduced platforms) and validates the full date
     * tuple, so a zero-filled/garbage read can never masquerade as a year-2000
     * wall time. */
    if (ft == FILETIME_NOW_PLACEHOLDER) {
        struct rtc_time rtc_t;
        if (rtc_try_read(&rtc_t) &&
            rtc_t.year >= 2000 && rtc_t.year <= 2100) {
            ft = filetime_from_rtc(&rtc_t);
            source = "RTC";
        }
    }

    /* Latch anchor. Sample mono_ns() BEFORE the seqlock writer (it runs
     * IRQ-disabled and mono_ns may do HPET/PMTMR I/O). */
    uint64_t anchor_mono = mono_ns();
    seqlock_write_lock(&s_lock);
    s_base_time = ft;
    s_base_mono_ns = anchor_mono;
    seqlock_write_unlock(&s_lock);
    /* Publish s_ready BEFORE s_time_sourced so a reader that observes
     * sourced==1 (acquire) is guaranteed to also observe ready==1 -- the
     * absolute-delay gate relies on that ordering. */
    __atomic_store_n(&s_ready, 1, __ATOMIC_RELEASE);
    if (ft != FILETIME_NOW_PLACEHOLDER)
        __atomic_store_n(&s_time_sourced, 1, __ATOMIC_RELEASE);

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
    uint64_t seq;

    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE))
        return FILETIME_NOW_PLACEHOLDER;

    do {
        seq = seqlock_read_begin(&s_lock);
        base = s_base_time;
        base_mono = s_base_mono_ns;
    } while (seqlock_read_retry(&s_lock, seq));

    /* Current time = base + elapsed monotonic delta in FILETIME units. Clamp a
     * backward mono_ns() excursion (clocksource demotion / AP TSC glitch) to 0
     * so the wall clock never jumps centuries into the future. */
    uint64_t now = mono_ns();
    uint64_t elapsed_ns = (now > base_mono) ? (now - base_mono) : 0;
    return base + (elapsed_ns / 100);  /* ns -> 100 ns FILETIME ticks */
}

FILETIME KeQuerySystemTimePrecise(void)
{
    /* Currently identical to KeQuerySystemTime() since both use mono_ns()
     * for TSC/HPET interpolation. Will diverge once the KUSER_SHARED_DATA
     * coarse tick-granular path for KeQuerySystemTime() lands. */
    return KeQuerySystemTime();
}

void KeSetSystemTime(FILETIME new_time)
{
    /* The placeholder (1601 epoch, value 0) is the "no time" sentinel, never a
     * real wall time. Accepting it would mark the clock sourced over a bogus
     * anchor and re-open the absolute-delay 49-day clamp. Reject it. */
    if (new_time == FILETIME_NOW_PLACEHOLDER) {
        klog(LOG_WARN, "time", "KeSetSystemTime: rejected placeholder time");
        return;
    }

    /* Sample mono_ns() INSIDE the seqlock writer (interrupts already disabled,
     * so no preemption can stretch the anchor pair stale). KeSetSystemTime is
     * a rare explicit set, so the brief clocksource read under the writer lock
     * is the right trade vs an unbounded pre-lock sample-to-publish gap. */
    seqlock_write_lock(&s_lock);
    s_base_time = new_time;
    s_base_mono_ns = mono_ns();
    seqlock_write_unlock(&s_lock);
    __atomic_store_n(&s_time_sourced, 1, __ATOMIC_RELEASE);

    klog(LOG_INFO, "time", "Wall clock set to FILETIME %u",
         (uint64_t)new_time);
}

int wall_clock_ready(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE);
}

/* ---- Kernel time service API ---------------------------------------- */

void KeQueryTickCount(uint64_t *tick_count)
{
    /* Windows contract: this is the timer-tick counter (one per timer
     * interrupt), and KeQueryTimeIncrement() gives the 100ns per tick, so
     * tick_count * increment ~= uptime. Returning 100ns units here would
     * overstate any tick_delta * increment computation by ~1e5. */
    if (tick_count)
        *tick_count = system_get_ticks();
}

void KeQueryTimeIncrement(uint32_t *increment)
{
    /* Timer fires at 100 Hz -> 10 ms per tick -> 100000 * 100 ns units. This
     * is the base increment; a raised rate from timer-resolution management
     * (NtSetTimerResolution) and its dynamic readback are owned there. */
    if (increment)
        *increment = 100000;  /* 10 ms in 100 ns units */
}

uint32_t ke_delay_interval_to_ms(int64_t interval, FILETIME now)
{
    uint64_t rel_100ns;
    uint64_t ms;

    if (interval < 0) {
        rel_100ns = (uint64_t)(-(interval + 1)) + 1;  /* INT64_MIN-safe magnitude */
    } else if (interval == 0) {
        return 0;
    } else {
        if ((uint64_t)interval <= now)
            return 0;  /* absolute deadline already passed */
        rel_100ns = (uint64_t)interval - now;
    }

    /* Round up to whole ms (sub-ms requests still wait at least one tick). */
    ms = (rel_100ns + FILETIME_TICKS_PER_MS - 1) / FILETIME_TICKS_PER_MS;
    if (ms == 0) ms = 1;
    if (ms > 0xFFFFFFFFULL) ms = 0xFFFFFFFFULL;
    return (uint32_t)ms;
}

void KeDelayExecutionThread(int64_t interval)
{
    FILETIME now;
    uint32_t ms;

    /* sleep_ms() blocks via a busy-HLT that only advances on a timer tick, so
     * it deadlocks if interrupts are masked. KeDelayExecutionThread is a
     * PASSIVE_LEVEL-only API; refuse a raised-IRQL caller rather than hang.
     * (WaitMode/Alertable NT params deferred -- no alertable-wait infra yet.) */
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        return;

    /* A positive interval is an absolute FILETIME deadline, which is only
     * meaningful against a real wall-clock source. Without one (no UEFI/RTC,
     * or pre-init), KeQuerySystemTime() returns ~0 and the deadline would look
     * decades away and clamp to a ~49-day sleep -- return immediately instead. */
    if (interval > 0) {
        if (!__atomic_load_n(&s_time_sourced, __ATOMIC_ACQUIRE))
            return;
        now = KeQuerySystemTime();
        /* Defensive: even with sourced set, if the query still returns the
         * placeholder (init race / a placeholder set), an absolute deadline
         * would clamp to a ~49-day sleep -- return immediately instead. */
        if (now == FILETIME_NOW_PLACEHOLDER)
            return;
    } else {
        now = 0;
    }
    ms = ke_delay_interval_to_ms(interval, now);
    if (ms != 0)
        sleep_ms(ms);
}

int time_service_ready(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE);
}

/* ---- Interrupt time APIs -------------------------------------------- */

/* Cumulative suspend bias. Atomic-accessed on BOTH sides: __atomic_fetch_add
 * writer + __atomic_load_n readers (plain volatile reads gave no ordering, only
 * no-elision, and broke the file's own atomic discipline). */
static uint64_t s_interrupt_time_bias;

uint64_t KeQueryInterruptTime(void)
{
    /* Interrupt time = monotonic ticks + suspend bias */
    return mono_filetime_units() +
           __atomic_load_n(&s_interrupt_time_bias, __ATOMIC_SEQ_CST);
}

uint64_t KeQueryInterruptTimePrecise(uint64_t *qpc_value)
{
    /* Sample the monotonic counter ONCE so the returned interrupt time and the
     * QPC out-value describe the same instant (the precise contract) -- two
     * mono reads would skew them by a clocksource-read latency. */
    uint64_t qpc = mono_filetime_units();
    uint64_t bias = __atomic_load_n(&s_interrupt_time_bias, __ATOMIC_SEQ_CST);
    if (qpc_value)
        *qpc_value = qpc;  /* QPC = raw monotonic at this instant */
    return qpc + bias;
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

/* ---- SSDT handlers ------------------------------------------------- */

/* NtQuerySystemTime(SystemTime) -- SSDT 0x00F0 */
static NTSTATUS nt_query_system_time(uint64_t out_ptr, uint64_t a2,
                                      uint64_t a3, uint64_t a4,
                                      uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!out_ptr)
        return STATUS_INVALID_PARAMETER;
    *(FILETIME *)out_ptr = KeQuerySystemTime();
    return STATUS_SUCCESS;
}

/* NtSetSystemTime(NewTime, PreviousTime) -- SSDT 0x00F1 */
static NTSTATUS nt_set_system_time(uint64_t new_ptr, uint64_t prev_ptr,
                                    uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6)
{
    FILETIME nt;
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!new_ptr)
        return STATUS_INVALID_PARAMETER;

    /* The placeholder (1601 epoch) is the "no time" sentinel, not a settable
     * wall time -- reject before touching the anchor. */
    nt = *(FILETIME *)new_ptr;
    if (nt == FILETIME_NOW_PLACEHOLDER)
        return STATUS_INVALID_PARAMETER;

    /* Optional: return previous time */
    if (prev_ptr)
        *(FILETIME *)prev_ptr = KeQuerySystemTime();

    KeSetSystemTime(nt);
    return STATUS_SUCCESS;
}

/* NtQueryPerformanceCounter(Count, Frequency) -- SSDT 0x00F2 */
static NTSTATUS nt_query_performance_counter(uint64_t count_ptr,
                                              uint64_t freq_ptr,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!count_ptr)
        return STATUS_INVALID_PARAMETER;

    *(uint64_t *)count_ptr = mono_filetime_units();

    /* Fixed 10 MHz frequency -- hardware-independent, apps don't need to
     * handle variable QPC frequency (competitive edge over Win11) */
    if (freq_ptr)
        *(uint64_t *)freq_ptr = FILETIME_TICKS_PER_SECOND;

    return STATUS_SUCCESS;
}

void wall_clock_register_ssdt(void)
{
    ssdt_register(SSDT_NtQuerySystemTime,
                  (SSDT_HANDLER)nt_query_system_time);
    ssdt_register(SSDT_NtSetSystemTime,
                  (SSDT_HANDLER)nt_set_system_time);
    ssdt_register(SSDT_NtQueryPerformanceCounter,
                  (SSDT_HANDLER)nt_query_performance_counter);

    klog(LOG_INFO, "time",
         "Time syscalls registered (SSDT 0x%03X-0x%03X)",
         (uint64_t)SSDT_NtQuerySystemTime,
         (uint64_t)SSDT_NtQueryPerformanceCounter);
}
