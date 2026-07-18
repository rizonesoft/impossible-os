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
#include "kernel/sched/kworker.h"
#include "kernel/sched/irql.h"
#include "kernel/timer.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/zw.h"
#include "kernel/security/privileges.h"  /* SeSinglePrivilegeCheck, SeSystemtimePrivilege */
#include "kernel/cpu_security.h"
#include "kernel/klog.h"
#include "kernel/smp.h"
#include "kernel/time/ntp_adj.h"

/* ---- State --------------------------------------------------------------- */

static FILETIME  s_base_time;       /* UTC anchor (FILETIME) */
static uint64_t  s_base_mono_ns;    /* mono_ns() at anchor time */
static seqlock_t s_lock = SEQLOCK_INIT;
static int       s_ready;
static int       s_time_sourced;    /* 1 only when seeded from a REAL source
                                     * (UEFI/RTC/KeSetSystemTime), not the
                                     * placeholder -- gates absolute deadlines */

/* ---- NTP discipline state ------------------------------------------------ *
 * Defined up here (ahead of KeSetSystemTimeEx) because every wall-clock anchor
 * writer -- the NTP step path AND the non-NTP KeSetSystemTimeEx setter -- must
 * coordinate on s_ntp_lock so the anchor swap and the NTP status publish are
 * atomic w.r.t. each other (a manual set must not leave a stale "ntp" status). */
static int32_t     s_ntp_freq_ppb;       /* clamped freq correction stored */
static int64_t     s_slew_remaining_ns;  /* phase slew stored (applied by watchdog) */
static FILETIME    s_ntp_last_sync;      /* wall time of last accepted adjtime */
static const char *s_ntp_source = "none";/* discipline source string */
/* Serializes the whole ke_ntp_adjtime() accept path + the ke_ntp_get_status()
 * snapshot + every non-NTP KeSetSystemTimeEx anchor write, so concurrent writers
 * cannot lose an accepted correction and a reader sees a coherent {freq, slew,
 * last_sync, source} set. Order: this lock is OUTER to the s_lock seqlock writer
 * (wall_clock_step_relative / KeSetSystemTimeEx take s_lock inside this lock);
 * no path takes s_lock then this lock.
 * Hold cost: the WRITER paths (KeSetSystemTimeEx, ke_ntp_adjtime step) sample
 * mono_ns() inside this hold, and mono_ns() is a single bounded hardware read
 * that on a non-TSC clocksource routes to HPET MMIO or PMTMR port I/O (sub-us,
 * not the ms-scale I/O the lock-hold rule forbids). Both writers are COLD paths
 * (manual/Zw time set, NTP correction); the reader ke_ntp_get_status holds only
 * for a 4-field copy with no I/O. The future per-tick continuous-discipline
 * consumer must NOT call a mono_ns()-under-lock path at tick frequency -- audited
 * by the clocksource-quality watchdog. */
static spinlock_t  s_ntp_lock = SPINLOCK_INIT;

/* ---- Wall-time monotonic floor (NTP continuous discipline) ---------------- *
 * The per-tick NTP applier re-anchors s_base_time by a freq/slew correction that
 * can be NEGATIVE (slow a fast clock). The floor clamps a wall read UP so a
 * negative correction is realized as a brief slow, not a backward step (the mono
 * floor guards mono_ns only; wall time = base + elapsed needs its own floor).
 * GENERATION-aware so a legitimate -- possibly backward -- wall MOVE (a manual
 * KeSetSystemTime or a large NTP step) is not clamped: the writer bumps s_wf.gen
 * (under s_ntp_lock -> s_lock) and resets the floor to the new time. The floor is
 * WRITER-ONLY -- raised by the NTP applier + reset by the setter/step, both
 * serialized under s_ntp_lock; READERS only read-and-clamp (gen-gated), never
 * write it, so a stale pre-set reader can never raise it. The coarse cache is
 * floored at PUBLISH time (ISR + setter) so KeQuerySystemTimeCoarse stays a
 * single load and never feeds a stale value back into the floor. */
/* Floor + generation packed in one cacheline-SIZED (not just aligned-start)
 * struct: the trailing pad reserves the rest of the 64-byte line so the
 * tick-written coarse-cache fields below do NOT share it and false-share the
 * floor, which the precise hot path (KeQuerySystemTime) reads every call. */
static struct {
    uint64_t floor;   /* the wall floor (hot: read by wall_floor_read) */
    uint32_t gen;     /* generation; bumped on every wall MOVE (manual set or step) */
    uint8_t  _pad[64 - sizeof(uint64_t) - sizeof(uint32_t)];
} __attribute__((aligned(64))) s_wf;

/* Pure: the monotonic-clamp result for a wall read of `cand` against `floor`.
 * Clamps UP (returns max) when the read's generation matches the current one;
 * returns `cand` unchanged on a generation MISMATCH (a manual set happened since
 * the read snapshotted its anchor, so this pre-set value must not raise the new
 * floor). Side-effect-free -- unit-tested. */
uint64_t wall_floor_clamp(uint64_t cand, uint64_t floor,
                          uint32_t read_gen, uint32_t cur_gen)
{
    if (read_gen != cur_gen)
        return cand;
    return (cand > floor) ? cand : floor;
}

/* Read-and-clamp the live wall floor (READ-ONLY -- readers never write it, so a
 * stale reader can never poison the floor across a manual set). Clamps `cand` up
 * to s_wf.floor when the snapshot generation still matches; on a generation
 * mismatch (a manual set raced this read) returns `cand` unclamped so a pre-set
 * reader is neither dragged to the new floor (forward set) nor blocked by the old
 * high-water (backward set). The floor is WRITER-ONLY: raised by the NTP applier
 * (to the pre-correction wall) and reset by KeSetSystemTimeEx, both serialized
 * under s_ntp_lock so no CAS is needed on the write side. */
static FILETIME wall_floor_read(FILETIME cand, uint32_t read_gen)
{
    uint64_t floor;
    if (read_gen != __atomic_load_n(&s_wf.gen, __ATOMIC_ACQUIRE))
        return cand;
    floor = __atomic_load_n(&s_wf.floor, __ATOMIC_ACQUIRE);
    return ((uint64_t)cand > floor) ? cand : (FILETIME)floor;
}

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
    uint32_t gen;
    uint64_t seq;

    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE))
        return FILETIME_NOW_PLACEHOLDER;

    do {
        seq = seqlock_read_begin(&s_lock);
        base = s_base_time;
        base_mono = s_base_mono_ns;
        /* Snapshot the wall-floor generation WITH the anchor (both written under
         * the s_lock writer by a manual set) so wall_floor_read can tell whether
         * a set raced this read. */
        gen = s_wf.gen;
    } while (seqlock_read_retry(&s_lock, seq));

    /* Current time = base + elapsed monotonic delta in FILETIME units. Clamp a
     * backward mono_ns() excursion (clocksource demotion / AP TSC glitch) to 0
     * so the wall clock never jumps centuries into the future. */
    uint64_t now = mono_ns();
    uint64_t elapsed_ns = (now > base_mono) ? (now - base_mono) : 0;
    /* Floor the result so an NTP per-tick negative correction (which re-anchors
     * s_base_time downward) is realized as a brief slow, not a backward step. */
    return wall_floor_read(base + (elapsed_ns / 100), gen);
}

FILETIME KeQuerySystemTimePrecise(void)
{
    /* Identical to KeQuerySystemTime() today: both interpolate via mono_ns().
     * Precision is therefore exactly as fine as the selected clocksource --
     * sub-microsecond on TSC/HPET/PMTMR, and only tick-granular on the
     * LAPIC-counter LAST-RESORT source (no sub-us clocksource present, so no
     * API can be more precise there). The two diverge once the coarse
     * KUSER_SHARED_DATA tick path makes KeQuerySystemTime() tick-granular by
     * design; the precise path will keep routing through full mono_ns(). */
    return KeQuerySystemTime();
}

void KeSetSystemTimeEx(FILETIME new_time, FILETIME *previous_out)
{
    uint64_t now_mono;
    uint64_t irqf;

    /* Reject the placeholder (1601 epoch, value 0 -- the "no time" sentinel) AND
     * any absurd-future value beyond FILETIME_MAX_PLAUSIBLE. This keeps the
     * sourced anchor ALWAYS in (placeholder, MAX_PLAUSIBLE], so no downstream
     * consumer (NTP discipline, interpolation) can be fed a corrupt/wrapping
     * wall-clock base -- the invariant is enforced once, here, at the setter. */
    if (new_time == FILETIME_NOW_PLACEHOLDER ||
        (uint64_t)new_time > FILETIME_MAX_PLAUSIBLE) {
        klog(LOG_WARN, "time", "KeSetSystemTime: rejected implausible time %u",
             (uint64_t)new_time);
        if (previous_out)
            *previous_out = FILETIME_NOW_PLACEHOLDER;
        return;
    }

    /* Take s_ntp_lock (OUTER, before the s_lock seqlock writer) so this non-NTP
     * anchor write cannot interleave with an in-flight ke_ntp_adjtime() step +
     * status publish, and so the anchor swap and the NTP-status invalidation
     * below are atomic w.r.t. each other. A manual / Zw time set SUPERSEDES NTP:
     * the previously accepted slew/freq were computed against the OLD anchor and
     * the clock is no longer NTP-tracked, so leaving "ntp" status (or applying
     * the stale corrections) would mislead a status reader. Order s_ntp_lock ->
     * s_lock matches ke_ntp_adjtime(). */
    spin_lock_irqsave(&s_ntp_lock, &irqf);

    /* Sample mono_ns() INSIDE the seqlock writer (interrupts already disabled,
     * so no preemption can stretch the anchor pair stale). The old effective
     * wall time is captured under the SAME writer hold as the swap, so a
     * racing KeSetSystemTime cannot slip a different value between the
     * previous-time read and the publish. */
    seqlock_write_lock(&s_lock);
    now_mono = mono_ns();
    if (previous_out) {
        if (__atomic_load_n(&s_ready, __ATOMIC_RELAXED)) {
            uint64_t elapsed = (now_mono > s_base_mono_ns)
                                   ? (now_mono - s_base_mono_ns) : 0;
            *previous_out = s_base_time + (elapsed / 100);
        } else {
            *previous_out = FILETIME_NOW_PLACEHOLDER;
        }
    }
    s_base_time = new_time;
    s_base_mono_ns = now_mono;
    /* Bump the wall-floor generation and reset the floor to the new time, under
     * the SAME seqlock writer as the anchor. The generation bump makes every
     * pre-set reader's floor snapshot stale, so a reader that captured the old
     * high anchor cannot raise the new floor past a (possibly backward) manual
     * set; the reset lets the new time stand. */
    s_wf.gen++;
    __atomic_store_n(&s_wf.floor, (uint64_t)new_time, __ATOMIC_RELAXED);
    seqlock_write_unlock(&s_lock);
    /* Publish readiness/sourced AFTER the anchor (seqlock unlock) so an
     * acquire-load observer of s_ready also sees the new anchor. */
    __atomic_store_n(&s_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_time_sourced, 1, __ATOMIC_RELEASE);

    /* Invalidate NTP discipline (still under s_ntp_lock, atomic with the swap):
     * a manual set is not an NTP sync, and the stored slew/freq tracked the OLD
     * anchor. Clear to a clean un-disciplined state. */
    s_slew_remaining_ns = 0;
    s_ntp_freq_ppb      = 0;
    s_ntp_last_sync     = FILETIME_NOW_PLACEHOLDER;
    s_ntp_source        = "none";

    spin_unlock_irqrestore(&s_ntp_lock, irqf);

    /* The coarse cache is deliberately NOT republished here: the next timer tick
     * (<= one tick) refreshes KeQuerySystemTimeCoarse() against the new anchor +
     * the just-reset floor (the ISR snapshot reads the bumped generation, so it
     * floors against the new time and publishes ~new_time), which is exactly the
     * documented one-tick coarse lag. Publishing from this thread context would
     * add a second publisher racing the ISR and could overwrite a newer ISR
     * interrupt-time sample with an older one (coarse interrupt time must stay
     * monotonic). The coarse reader never writes the floor, so a one-tick stale
     * value cannot poison it. Callers needing the new wall time immediately use
     * the precise KeQuerySystemTime(). */
    klog(LOG_INFO, "time", "Wall clock set to FILETIME %u",
         (uint64_t)new_time);
}

void KeSetSystemTime(FILETIME new_time)
{
    KeSetSystemTimeEx(new_time, (FILETIME *)0);
}

int wall_clock_ready(void)
{
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE);
}

int wall_clock_time_sourced(void)
{
    return __atomic_load_n(&s_time_sourced, __ATOMIC_ACQUIRE);
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

/* Coarse cache published once per timer tick by wall_clock_tick_cache() (BSP
 * ISR, single publisher) and read lock-free by the Ke*Coarse() APIs. RELEASE
 * store / ACQUIRE load: a reader that sees a fresh value also sees the matching
 * bias/anchor state used to compute it. Both default 0 (= placeholder / no
 * interrupt time) until the first tick after the wall clock is seeded. */
static FILETIME s_coarse_system_ft;
static uint64_t s_coarse_interrupt_time;

/* Shared snapshot math. coarse=1 samples mono_ns_coarse() (no port/MMIO I/O on
 * PMTMR/HPET); coarse=0 samples the precise mono_ns(). ONE clocksource read
 * drives both outputs so they describe the same instant. */
static void wall_clock_snapshot_impl(FILETIME *system_out, uint64_t *interrupt_out,
                                     int coarse)
{
    FILETIME base = FILETIME_NOW_PLACEHOLDER;
    uint64_t base_mono = 0, mono, elapsed;
    uint32_t gen = 0;
    int ready = __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE);

    if (ready) {
        uint64_t seq;
        do {
            seq = seqlock_read_begin(&s_lock);
            base = s_base_time;
            base_mono = s_base_mono_ns;
            gen = s_wf.gen;
        } while (seqlock_read_retry(&s_lock, seq));
    }

    /* Sample mono AFTER accepting a stable anchor (matches KeQuerySystemTime's
     * ordering): pairing the sample with the already-accepted base epoch keeps
     * base_mono <= mono, so both outputs derive from one coherent instant. */
    mono = coarse ? mono_ns_coarse() : mono_ns();

    if (interrupt_out)
        *interrupt_out = (mono / 100) +
            __atomic_load_n(&s_interrupt_time_bias, __ATOMIC_SEQ_CST);

    if (system_out) {
        if (!ready) {
            *system_out = FILETIME_NOW_PLACEHOLDER;
        } else {
            elapsed = (mono > base_mono) ? (mono - base_mono) : 0;
            /* Floor (gen-gated) so the published coarse cache + KUSD precise read
             * never regress across an NTP per-tick negative correction. */
            *system_out = wall_floor_read(base + (elapsed / 100), gen);
        }
    }
}

void wall_clock_snapshot(FILETIME *system_out, uint64_t *interrupt_out)
{
    /* Precise: the KUSD updater would otherwise read mono_ns() twice per tick
     * (two HPET MMIO / PMTMR port-I/O samples). */
    wall_clock_snapshot_impl(system_out, interrupt_out, 0);
}

void wall_clock_tick_cache(FILETIME *system_out, uint64_t *interrupt_out)
{
    FILETIME sys;
    uint64_t intr;

    /* PRECISE sample (one mono_ns() per tick): this same value feeds the KUSD
     * page AND the published coarse cache, so the per-tick clocksource read is
     * NOT removed -- it is amortized. The fast-path win is on the READER side:
     * Ke*Coarse() callers do a single atomic load instead of their own
     * mono_ns()+seqlock. A coarse (mono_ns_coarse) sample was rejected here: on
     * PMTMR its epoch banks only every 8th tick, which would freeze the KUSD
     * SystemTime/TickCount fields for ~80 ms (coarse-time-fast-path review). */
    wall_clock_snapshot_impl(&sys, &intr, 0);

    /* Publish for lock-free coarse readers from the BSP ONLY -- a single
     * publisher keeps interrupt time monotonic and the pair untorn with just a
     * RELEASE store (no seqlock). The tick ISR is BSP-only today; this guard
     * keeps the invariant intact when AP per-CPU tick delivery lands (an AP
     * tick must compute/return its values but must NOT publish, or two CPUs
     * would race the two stores). 64-bit aligned stores are atomic on x86-64. */
    {
        struct per_cpu_data *cpu = smp_this_cpu();
        if (!cpu || cpu->cpu_id == 0) {
            __atomic_store_n(&s_coarse_system_ft, sys, __ATOMIC_RELEASE);
            __atomic_store_n(&s_coarse_interrupt_time, intr, __ATOMIC_RELEASE);
        }
    }

    if (system_out)
        *system_out = sys;
    if (interrupt_out)
        *interrupt_out = intr;
}

FILETIME KeQuerySystemTimeCoarse(void)
{
    return __atomic_load_n(&s_coarse_system_ft, __ATOMIC_ACQUIRE);
}

uint64_t KeQueryInterruptTimeCoarse(void)
{
    return __atomic_load_n(&s_coarse_interrupt_time, __ATOMIC_ACQUIRE);
}

/* ---- NTP clock adjustment (wall-time discipline) ------------------- *
 * WALL TIME ONLY: ke_ntp_adjtime() steps the wall clock for a large offset
 * (KeSetSystemTime, monotonicity-safe) and stores a clamped freq + slew for the
 * status query; the monotonic clock (mono_ns / mono_filetime_units / QPC /
 * interrupt time) is never touched (Win11 QPC-independent contract).
 *
 * The CONTINUOUS freq/slew application (the per-tick anchor discipline) is
 * deferred to the clocksource-quality-watchdog work: applying a sub-tick
 * correction monotonically needs a wall-time monotonic floor -- the same
 * mono-wide atomic floor ("never step backward") machinery the watchdog builds.
 * A discrete per-tick anchor nudge regresses precise wall reads across the
 * tick, so it is intentionally not wired here. ntp_tick_adjust_ns() below is
 * the tested discipline math the watchdog will drive behind that floor. */


int ntp_step_target_valid(uint64_t now_ft, int64_t off_ns, int64_t *target)
{
    /* Validate the current wall time as UNSIGNED first: FILETIME is unsigned and
     * a prior KeSetSystemTime could hold a value above INT64_MAX, where a signed
     * cast is implementation-defined. After this, now_ft <= NTP_FILETIME_MAX
     * (~1e18) so the signed add below cannot overflow. */
    if (now_ft <= (uint64_t)FILETIME_NOW_PLACEHOLDER ||
        now_ft >  (uint64_t)NTP_FILETIME_MAX)
        return 0;

    /* Reject an implausible step magnitude (a network-derived offset of eons). */
    if (off_ns > NTP_STEP_MAX_NS || off_ns < -NTP_STEP_MAX_NS)
        return 0;

    /* now_ft <= 1e18, |off/100| <= ~3.15e15 -> the int64 add is overflow-safe. */
    int64_t t = (int64_t)now_ft + off_ns / 100;   /* ns -> 100 ns FILETIME units */
    if (t <= (int64_t)FILETIME_NOW_PLACEHOLDER || t > NTP_FILETIME_MAX)
        return 0;   /* below the 1601 placeholder or absurd-future: reject */

    if (target)
        *target = t;
    return 1;
}

int64_t ntp_tick_adjust_ns(uint64_t elapsed_ns, int32_t freq_ppb,
                           int64_t slew_remaining, int64_t *slew_consumed)
{
    /* Cap elapsed at 1 s: bounds elapsed*freq_ppb well within int64 even at the
     * extreme |freq_ppb| ~2e9, and stops a timer quiesce / suspend gap from
     * applying a giant one-tick correction (the next tick resumes normally). */
    if (elapsed_ns > 1000000000ULL)
        elapsed_ns = 1000000000ULL;

    /* Frequency: positive freq_ppb means the wall clock runs FAST, so subtract
     * (freq_ppb / 1e9) * elapsed to slow it. */
    int64_t freq_ns = -((int64_t)elapsed_ns * (int64_t)freq_ppb) / 1000000000LL;

    /* Slew: consume the phase correction at NTP_SLEW_MAX_PPM, sign-aware. */
    int64_t cap_ns = (int64_t)NTP_SLEW_MAX_PPM * (int64_t)elapsed_ns / 1000000LL;
    int64_t slew_ns = 0;
    if (slew_remaining > 0)
        slew_ns = (slew_remaining < cap_ns) ? slew_remaining : cap_ns;
    else if (slew_remaining < 0)
        slew_ns = -(((-slew_remaining) < cap_ns) ? (-slew_remaining) : cap_ns);

    if (slew_consumed)
        *slew_consumed = slew_ns;
    return freq_ns + slew_ns;
}

/* Atomic checked relative wall-clock step: under ONE seqlock writer hold, read
 * the effective current wall time from the live anchor, validate now+off, and
 * re-anchor (base_time = target, base_mono = the sampled mono). This eliminates
 * the read-vs-set TOCTOU of a separate KeQuerySystemTime()+KeSetSystemTime(): a
 * concurrent setter cannot slip a newer anchor between the read and the set.
 * Requires a sourced clock (the validator rejects the placeholder). Returns 1
 * on a successful step. */
static int wall_clock_step_relative(int64_t off_ns)
{
    int ok = 0;

    seqlock_write_lock(&s_lock);
    if (__atomic_load_n(&s_ready, __ATOMIC_RELAXED)) {
        uint64_t base = (uint64_t)s_base_time;
        uint64_t mono = mono_ns();
        uint64_t elapsed_units = (mono > s_base_mono_ns)
                                     ? ((mono - s_base_mono_ns) / 100) : 0;
        /* Checked unsigned add: reject an already-corrupt anchor (a prior bad
         * KeSetSystemTime can store an absurd FILETIME) or an elapsed delta that
         * would wrap `now` back into the plausible range past the validator. */
        if (base <= (uint64_t)NTP_FILETIME_MAX &&
            elapsed_units <= (uint64_t)NTP_FILETIME_MAX - base) {
            uint64_t now = base + elapsed_units;
            int64_t target;
            if (ntp_step_target_valid(now, off_ns, &target)) {
                s_base_time = (FILETIME)target;
                s_base_mono_ns = mono;   /* re-anchor so the freq nudge stays consistent */
                /* A step is an intentional wall MOVE (the target may be BELOW the
                 * applier-raised floor for a negative offset). Bump the generation
                 * + reset the floor to the target, exactly like KeSetSystemTimeEx,
                 * so the floor cannot clamp the stepped wall back up (freezing it).
                 * Under the s_lock writer, so a reader's gen snapshot stays
                 * coherent with the new anchor. */
                s_wf.gen++;
                __atomic_store_n(&s_wf.floor, (uint64_t)target, __ATOMIC_RELAXED);
                ok = 1;
            }
        }
    }
    seqlock_write_unlock(&s_lock);

    if (ok)
        __atomic_store_n(&s_time_sourced, 1, __ATOMIC_RELEASE);
    return ok;
}

const char *ntp_source_for_correction(int64_t off_ns, int32_t freq_ppb)
{
    int stepped = (off_ns > NTP_STEP_THRESHOLD_NS || off_ns < -NTP_STEP_THRESHOLD_NS);
    /* Any stored-but-unapplied correction makes the reported state pending: a
     * sub-second slew (kept for the deferred per-tick discipline) OR a nonzero
     * freq (always deferred -- continuous application is not wired yet). A mixed
     * step+freq request thus reports "ntp-pending", not a completed sync, even
     * though the step itself already moved the clock. */
    int pending = (freq_ppb != 0) || (!stepped && off_ns != 0);
    if (pending)
        return "ntp-pending";
    return stepped ? "ntp"   /* pure step applied, nothing residual */
                   : "ntp";  /* zero no-op against a sourced clock */
}

void ke_ntp_adjtime(const ntp_adj_t *adj)
{
    if (!adj)
        return;

    /* Clamp the (network-derived, untrusted) freq to +/-NTP_FREQ_MAX_PPB so a
     * hostile/garbage value cannot drive the per-tick nudge past the natural
     * monotonic advance. */
    int32_t freq = adj->freq_ppb;
    if (freq >  NTP_FREQ_MAX_PPB) freq =  NTP_FREQ_MAX_PPB;
    if (freq < -NTP_FREQ_MAX_PPB) freq = -NTP_FREQ_MAX_PPB;

    int64_t off = adj->offset_ns;
    uint64_t irqf;

    /* Serialize the ENTIRE accept path (gate + step + state publish) so two
     * concurrent adjtime callers cannot interleave and lose an accepted
     * correction, and a get_status reader sees a coherent set. */
    spin_lock_irqsave(&s_ntp_lock, &irqf);

    /* A correction -- step OR slew -- only makes sense against a REAL sourced
     * wall time; with only the 1601 placeholder there is no base to correct.
     * Gate BOTH paths so an unsourced request mutates no NTP state (initial
     * absolute time is set via KeSetSystemTime, not a relative correction). */
    if (__atomic_load_n(&s_time_sourced, __ATOMIC_ACQUIRE)) {
        int ok = 1;
        int64_t new_slew = 0;
        if (off > NTP_STEP_THRESHOLD_NS || off < -NTP_STEP_THRESHOLD_NS) {
            /* Step: atomic read-validate-set (under the s_lock seqlock writer,
             * nested inside s_ntp_lock). On reject the NTP state is preserved. */
            if (wall_clock_step_relative(off))
                new_slew = 0;   /* a successful step supersedes pending slew */
            else
                ok = 0;
        } else {
            new_slew = off;     /* slew supersedes the previous un-consumed one */
        }
        if (ok) {
            /* Commit all accepted NTP state together under the lock. The
             * reported discipline state must not overclaim: a STEP (>1 s) just
             * moved the wall clock via KeSetSystemTime, so it is genuinely
             * disciplined now ("ntp"). A slew/frequency correction (<=1 s) is
             * STORED here but its continuous per-tick application is deferred to
             * the clocksource-quality watchdog -- until that consumer lands the
             * correction is accepted-but-not-yet-applied, so report it as
             * "ntp-pending". A status reader must never mistake a stored-only
             * correction for a completed sync (the discipline math
             * ntp_tick_adjust_ns() is not yet wired into KeQuerySystemTime). */
            s_slew_remaining_ns = new_slew;
            s_ntp_freq_ppb      = freq;
            s_ntp_last_sync     = KeQuerySystemTime();
            s_ntp_source        = ntp_source_for_correction(off, freq);
        }
    }

    spin_unlock_irqrestore(&s_ntp_lock, irqf);
}

void ke_ntp_get_status(struct ntp_status *out)
{
    uint64_t irqf;
    if (!out)
        return;
    /* Coherent snapshot under the NTP writer lock. */
    spin_lock_irqsave(&s_ntp_lock, &irqf);
    out->offset_ns = s_slew_remaining_ns;
    out->freq_ppb  = s_ntp_freq_ppb;
    out->last_sync = s_ntp_last_sync;
    out->source    = s_ntp_source;
    spin_unlock_irqrestore(&s_ntp_lock, irqf);
}

/* ---- NTP continuous wall-time discipline applier ------------------------- */

static uint64_t s_ntp_apply_mono;    /* mono_ns at the last applier tick */
static int      s_ntp_apply_primed;  /* 1 once the elapsed baseline is captured */

/* Per-tick NTP discipline: consume the stored freq/slew (from ke_ntp_adjtime)
 * into the wall clock so a disciplined clock actually tracks the reference.
 * PASSIVE kworker context. Samples mono_ns() OUTSIDE s_ntp_lock (the NTP
 * lock-hold rule: never hold s_ntp_lock across a mono_ns() HPET/PMTMR read),
 * then under s_ntp_lock -> s_lock re-anchors s_base_time by the correction. A
 * NEGATIVE correction (slowing a fast clock) re-anchors downward; the wall floor
 * raised to the pre-correction value turns it into a brief slow, not a backward
 * step. Applying a correction flips the status "ntp-pending" -> "ntp". */
static void ke_ntp_discipline_tick(void *ctx)
{
    uint64_t now_mono, irqf, elapsed, base, cur_wall;
    int64_t  adj, consumed = 0;
    int32_t  freq;
    int64_t  slew;
    (void)ctx;

    now_mono = mono_ns();   /* sample BEFORE the lock */

    spin_lock_irqsave(&s_ntp_lock, &irqf);

    /* Nothing to discipline unless a sourced clock has a stored correction. */
    if (!__atomic_load_n(&s_time_sourced, __ATOMIC_ACQUIRE) ||
        (s_ntp_freq_ppb == 0 && s_slew_remaining_ns == 0)) {
        s_ntp_apply_primed = 0;
        spin_unlock_irqrestore(&s_ntp_lock, irqf);
        return;
    }

    /* Prime the elapsed baseline on the first tick (no application yet). */
    if (!s_ntp_apply_primed) {
        s_ntp_apply_mono   = now_mono;
        s_ntp_apply_primed = 1;
        spin_unlock_irqrestore(&s_ntp_lock, irqf);
        return;
    }

    elapsed = (now_mono > s_ntp_apply_mono) ? (now_mono - s_ntp_apply_mono) : 0;
    freq = s_ntp_freq_ppb;
    slew = s_slew_remaining_ns;
    adj  = ntp_tick_adjust_ns(elapsed, freq, slew, &consumed);

    seqlock_write_lock(&s_lock);
    {
        uint64_t e = (now_mono > s_base_mono_ns) ? (now_mono - s_base_mono_ns) : 0;
        cur_wall = (uint64_t)s_base_time + (e / 100);   /* wall now (FILETIME) */
        /* Raise the floor to the current wall BEFORE the re-anchor, so a negative
         * correction never lets a reader step back. WRITER-ONLY store: the applier
         * (here) and KeSetSystemTimeEx are mutually exclusive under s_ntp_lock, so
         * no CAS is needed; readers only read-and-clamp via wall_floor_read. */
        if (cur_wall > __atomic_load_n(&s_wf.floor, __ATOMIC_RELAXED))
            __atomic_store_n(&s_wf.floor, cur_wall, __ATOMIC_RELEASE);
        /* Apply the correction as a re-anchor. adj is tiny (< 0.05 % of elapsed);
         * guard plausibility so a corrupt value can never become the anchor. */
        base = (uint64_t)((int64_t)cur_wall + adj / 100);
        if (base > (uint64_t)FILETIME_NOW_PLACEHOLDER &&
            base <= FILETIME_MAX_PLAUSIBLE) {
            s_base_time    = (FILETIME)base;
            s_base_mono_ns = now_mono;
        }
    }
    seqlock_write_unlock(&s_lock);

    s_slew_remaining_ns -= consumed;   /* consume the applied phase slew */
    s_ntp_source     = "ntp";          /* correction is now actually applied */
    s_ntp_apply_mono = now_mono;

    spin_unlock_irqrestore(&s_ntp_lock, irqf);
}

void ke_ntp_discipline_init(void)
{
    static int s_registered;
    if (s_registered)
        return;
    /* Phase-3 PASSIVE applier (~0.25 s) -- finer than the clocksource-quality
     * watchdog so the discipline is smooth. No-op each tick unless a correction
     * is stored. */
    if (kworker_register(ke_ntp_discipline_tick, (void *)0, 250) >= 0) {
        s_registered = 1;
        klog(LOG_INFO, "time", "NTP wall-time discipline applier registered");
    }
}

/* ---- SSDT handlers ------------------------------------------------- */

/* Probe (when the caller is user-mode) + copy a uint64_t out to a syscall
 * pointer. Returns STATUS_SUCCESS or a fault status the handler propagates. */
static NTSTATUS write_u64_out(uint64_t ptr, uint64_t value)
{
    NTSTATUS pst;
    pst = ProbeForWriteIfUser((void *)ptr, sizeof(uint64_t), 8);
    if (pst != STATUS_SUCCESS)
        return pst;
    if (copy_to_user((void *)ptr, &value, sizeof(uint64_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* NtQuerySystemTime(SystemTime) -- SSDT 0x00F0 */
static NTSTATUS nt_query_system_time(uint64_t out_ptr, uint64_t a2,
                                      uint64_t a3, uint64_t a4,
                                      uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!out_ptr)
        return STATUS_ACCESS_VIOLATION;   /* required pointer; NULL is unwritable */
    return write_u64_out(out_ptr, (uint64_t)KeQuerySystemTime());
}

/* NtSetSystemTime(NewTime, PreviousTime) -- SSDT 0x00F1 */
static NTSTATUS nt_set_system_time(uint64_t new_ptr, uint64_t prev_ptr,
                                    uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6)
{
    FILETIME nt;
    NTSTATUS pst;
    (void)a3; (void)a4; (void)a5; (void)a6;

    /* Setting the wall clock requires SeSystemtimePrivilege. A KernelMode / Zw
     * caller is trusted (the check returns TRUE); a UserMode caller must hold
     * the enabled privilege in its effective token, else STATUS_PRIVILEGE_NOT_HELD. */
    if (!SeSinglePrivilegeCheck(&SeSystemtimePrivilege, ssdt_previous_mode()))
        return STATUS_PRIVILEGE_NOT_HELD;

    if (!new_ptr)
        return STATUS_ACCESS_VIOLATION;   /* required pointer; NULL is unreadable */

    /* Read the new time through the user-buffer guard -- a raw deref of a
     * user pointer could fault the kernel or read kernel memory. */
    pst = ProbeForReadIfUser((const void *)new_ptr, sizeof(FILETIME), 8);
    if (pst != STATUS_SUCCESS)
        return pst;
    if (copy_from_user(&nt, (const void *)new_ptr, sizeof(FILETIME)) != 0)
        return STATUS_ACCESS_VIOLATION;

    /* The placeholder (1601 epoch) is the "no time" sentinel, not a settable
     * wall time; an absurd-future value beyond the plausibility bound is a
     * corrupt/hostile anchor. Reject both at the ABI surface (returning the
     * NTSTATUS the caller expects) before touching the anchor -- the same
     * bounds KeSetSystemTimeEx enforces internally as a last-resort guard, but
     * the void setter cannot report rejection, so a Zw/kernel caller would
     * otherwise see STATUS_SUCCESS with the wall clock unchanged. */
    if (nt == FILETIME_NOW_PLACEHOLDER || (uint64_t)nt > FILETIME_MAX_PLAUSIBLE)
        return STATUS_INVALID_PARAMETER;

    /* PreviousTime (optional): probe the buffer BEFORE mutating, capture the
     * old effective time atomically with the swap (KeSetSystemTimeEx), then
     * copy out best-effort (address already probed). */
    if (prev_ptr) {
        FILETIME prev;
        pst = ProbeForWriteIfUser((void *)prev_ptr, sizeof(uint64_t), 8);
        if (pst != STATUS_SUCCESS)
            return pst;
        KeSetSystemTimeEx(nt, &prev);
        /* The clock is already swapped; copy_to_user is now fault-recoverable,
         * so a PreviousTime page unmapped/protected after the probe returns an
         * error here instead of bugchecking. Report it rather than falsely
         * claiming success -- the time change is a committed side effect either
         * way (same non-transactional contract as the Windows API). */
        if (copy_to_user((void *)prev_ptr, &prev, sizeof(uint64_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
        return STATUS_SUCCESS;
    }

    KeSetSystemTime(nt);
    return STATUS_SUCCESS;
}

/* NtQueryPerformanceCounter(Count, Frequency) -- SSDT 0x00F2 */
static NTSTATUS nt_query_performance_counter(uint64_t count_ptr,
                                              uint64_t freq_ptr,
                                              uint64_t a3, uint64_t a4,
                                              uint64_t a5, uint64_t a6)
{
    NTSTATUS pst;
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!count_ptr)
        return STATUS_ACCESS_VIOLATION;   /* required pointer; NULL is unwritable */

    pst = write_u64_out(count_ptr, mono_filetime_units());
    if (pst != STATUS_SUCCESS)
        return pst;

    /* Fixed 10 MHz frequency -- hardware-independent, apps don't need to
     * handle variable QPC frequency (competitive edge over Win11) */
    if (freq_ptr)
        return write_u64_out(freq_ptr, FILETIME_TICKS_PER_SECOND);

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
