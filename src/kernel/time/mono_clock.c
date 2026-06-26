/* ============================================================================
 * mono_clock.c -- Monotonic nanosecond clock source selection
 *
 * Priority: invariant TSC > HPET > LAPIC timer.
 * Uses integer multiply-shift for ns conversion (no division in hot path).
 * ============================================================================ */

#include "kernel/time/mono_clock.h"
#include "kernel/cpuid.h"
#include "kernel/cpu_security.h"
#include "kernel/boot_timing.h"
#include "kernel/drivers/lapic.h"
#include "kernel/drivers/hpet.h"
#include "kernel/acpi.h"
#include "kernel/smp.h"
#include "kernel/klog.h"
#include "kernel/sched/seqlock.h"

/* ---- State --------------------------------------------------------------- */

static uint32_t s_source = MONO_SRC_NONE;
static uint64_t s_freq_hz;            /* source frequency in Hz */
static uint64_t s_ns_per_tick_num;    /* numerator for ticks -> ns */
static uint64_t s_ns_per_tick_den;    /* denominator */

/* PMTMR 64-bit epoch (wrap extension); see mono_clock_pmtmr_advance() below.
 * Snapshot {epoch_ns, last_raw} is seqlock-protected (writer = timer-ISR
 * advance; readers lock-free). mask/source are set once at init before any
 * reader runs. */
static seqlock_t s_pmtmr_lock = SEQLOCK_INIT;
static uint64_t  s_pmtmr_epoch_ns;   /* banked ns up to s_pmtmr_last_raw */
static uint32_t  s_pmtmr_last_raw;   /* last banked raw PMTMR sample (masked) */
static uint32_t  s_pmtmr_mask;       /* 0xFFFFFFFF (32-bit) or 24-bit mask */
/* Global monotonic floor for the precise PMTMR reader: mono_ns() never returns
 * below any prior return. Guards both a chipset read glitch (a low outlier
 * masks as a near-full-wrap forward jump, then the next read steps back) and a
 * missed wrap across a long tick quiesce. Forward runaway is prevented by the
 * glitch-filtered (median-of-3) read feeding mono_ns. Cacheline-isolated: it is
 * hit (atomic load) by every coarse read while the ISR writes the epoch above,
 * so they must not false-share. */
static uint64_t  s_pmtmr_floor __attribute__((aligned(64)));

static uint64_t pmtmr_mono_floor(uint64_t cand)
{
    uint64_t cur = __atomic_load_n(&s_pmtmr_floor, __ATOMIC_ACQUIRE);
    while (cand > cur) {
        if (__atomic_compare_exchange_n(&s_pmtmr_floor, &cur, cand, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return cand;
        /* CAS failure reloaded cur; loop re-checks cand > cur */
    }
    return cur;   /* cand <= floor: clamp up so time never moves backward */
}

/* ---- TSC read ------------------------------------------------------------ */

/* Ordered TSC read: the value must not be sampled before prior instructions
 * have executed, or a tight measurement / monotonic read can read "earlier"
 * than its surrounding code (out-of-order CPUs). RDTSCP waits for all prior
 * instructions to retire (and yields TSC_AUX in ECX, which we discard); on
 * CPUs without it, LFENCE serializes before RDTSC. Mirrors Linux
 * rdtsc_ordered(); Win11 QPC abstracts the same. Used by every TSC read here. */
static inline uint64_t rdtsc_ordered(void)
{
    uint32_t lo, hi;
    /* Gate RDTSCP on the all-online feature INTERSECTION, never cpu_has() (which
     * is BSP-global): RDTSCP #UDs on a CPU that lacks it, and APs are an
     * optional-feature-probed set, so a BSP-has/AP-lacks skew would fault this
     * ISR-callable hot path on the skewed AP. Same gate dpc_watchdog_init uses. */
    if (cpu_feature_global_mask() & (1ULL << CPU_FEATURE_RDTSCP)) {
        uint32_t aux;
        __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    } else {
        __asm__ volatile ("lfence" ::: "memory");
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    }
    return ((uint64_t)hi << 32) | lo;
}

/* ---- Init ---------------------------------------------------------------- */

void mono_clock_init(void)
{
    /* Seed the tick-fallback epoch with the live tick frequency: the
     * fallback reader uses ONLY snapshot state (an epoch paired with a
     * live frequency sample would race a resolution change) */
    {
        extern uint32_t system_get_freq(void);
        mono_clock_tick_rebase(system_get_freq());
    }

    /* Try 1: Invariant TSC with known frequency */
    if (cpu_has(CPU_FEATURE_TSC_INV)) {
        uint64_t freq = boot_timing_tsc_freq();
        if (freq > 0) {
            s_source = MONO_SRC_TSC;
            s_freq_hz = freq;
            /* ns = ticks * 1000000000 / freq
             * Store as num/den to avoid 64-bit division in hot path.
             * For typical freq ~4 GHz: num=1000000000, den=freq */
            s_ns_per_tick_num = 1000000000ULL;
            s_ns_per_tick_den = freq;
            klog(LOG_INFO, "time",
                 "Monotonic clock: TSC (%u MHz, invariant)",
                 (uint64_t)(freq / 1000000));
            return;
        }
    }

    /* Try 2: HPET main counter */
    if (hpet_available()) {
        uint64_t freq = hpet_frequency_hz();
        if (freq > 0) {
            s_source = MONO_SRC_HPET;
            s_freq_hz = freq;
            s_ns_per_tick_num = 1000000000ULL;
            s_ns_per_tick_den = freq;
            klog(LOG_INFO, "time",
                 "Monotonic clock: HPET (%u MHz)",
                 (uint64_t)(freq / 1000000));
            return;
        }
    }

    /* Try 3: ACPI PM timer (PMTMR) -- a standalone always-running platform
     * counter (3.579545 MHz). Preferred over the LAPIC fallback below, which
     * is a per-CPU interrupt counter, not a real clocksource. */
    if (acpi_get_pmtimer_port() != 0) {
        /* Publication order: fully initialize the epoch snapshot BEFORE making
         * MONO_SRC_PMTMR visible. Interrupts are already enabled (Phase 1) when
         * this runs in Phase 2, so a concurrent ISR/AP reader entering the
         * PMTMR case must not observe a BSS-zero epoch. Seed under the seqlock,
         * then a release fence, then publish s_source last. */
        /* Seed the epoch from the time already elapsed on the tick fallback so
         * uptime_ns()/mono_ns() are CONTINUOUS across the source switch (a
         * zero-based epoch would make uptime jump backward and elapsed-time
         * subtraction underflow for callers that sampled before this runs). */
        extern uint64_t uptime_ns(void);
        uint64_t base_ns = uptime_ns();
        uint32_t mask = acpi_pmtimer_is_32bit() ? 0xFFFFFFFFu : PMTMR_24BIT_MASK;
        s_freq_hz        = PMTMR_FREQ_HZ;
        s_ns_per_tick_num = 1000000000ULL;
        s_ns_per_tick_den = PMTMR_FREQ_HZ;
        seqlock_write_lock(&s_pmtmr_lock);
        s_pmtmr_mask     = mask;
        s_pmtmr_epoch_ns = base_ns;
        s_pmtmr_last_raw = acpi_pmtimer_read_value() & mask;
        seqlock_write_unlock(&s_pmtmr_lock);
        __atomic_store_n(&s_pmtmr_floor, base_ns, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&s_source, MONO_SRC_PMTMR, __ATOMIC_RELEASE);
        klog(LOG_INFO, "time",
             "Monotonic clock: PMTMR (3.579545 MHz, %s)",
             mask == 0xFFFFFFFFu ? "32-bit" : "24-bit");
        return;
    }

    /* Try 4: LAPIC timer ticks */
    {
        uint32_t ticks_per_ms = lapic_timer_ticks_per_ms();
        if (ticks_per_ms > 0) {
            s_source = MONO_SRC_LAPIC;
            s_freq_hz = (uint64_t)ticks_per_ms * 1000;
            s_ns_per_tick_num = 1000000ULL;  /* ns per ms */
            s_ns_per_tick_den = (uint64_t)ticks_per_ms;
            klog(LOG_INFO, "time",
                 "Monotonic clock: LAPIC (%u ticks/ms)",
                 (uint64_t)ticks_per_ms);
            return;
        }
    }

    /* No clock source available */
    klog(LOG_WARN, "time", "Monotonic clock: no source available");
}


/* ---- CPUID 0x15 TSC frequency cross-check -------------------------------- */

static uint64_t cpuid15_tsc_freq(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x15), "c"(0));
    /* eax = denominator, ebx = numerator, ecx = crystal clock Hz (0 = unknown) */
    if (eax == 0 || ebx == 0)
        return 0;
    if (ecx == 0)
        return 0;  /* crystal freq not reported -- can't compute */
    return (uint64_t)ecx * ebx / eax;
}

void mono_clock_crosscheck_tsc(void)
{
    uint64_t cpuid_freq, boot_freq;

    if (s_source != MONO_SRC_TSC)
        return;

    cpuid_freq = cpuid15_tsc_freq();
    if (cpuid_freq == 0)
        return;

    boot_freq = s_freq_hz;

    /* Check within 1% */
    uint64_t delta = (cpuid_freq > boot_freq)
                   ? cpuid_freq - boot_freq : boot_freq - cpuid_freq;
    uint64_t threshold = boot_freq / 100;

    if (delta <= threshold) {
        klog(LOG_DEBUG, "time",
             "TSC freq cross-check: CPUID 0x15 = %u MHz, boot = %u MHz -- OK",
             (uint64_t)(cpuid_freq / 1000000),
             (uint64_t)(boot_freq / 1000000));
    } else {
        klog(LOG_WARN, "time",
             "TSC freq mismatch: CPUID 0x15 = %u MHz, boot = %u MHz (>1%%)",
             (uint64_t)(cpuid_freq / 1000000),
             (uint64_t)(boot_freq / 1000000));
    }
}

/* ---- Fast TSC read with scale -------------------------------------------- */

uint64_t rdtsc_ns(void)
{
    uint64_t ticks;
    uint64_t whole, rem;

    if (s_source != MONO_SRC_TSC || s_ns_per_tick_den == 0)
        return 0;

    ticks = rdtsc_ordered();
    {
        struct per_cpu_data *cpu = smp_this_cpu();
        if (cpu)
            ticks = (uint64_t)((int64_t)ticks + cpu->tsc_offset);
    }

    whole = ticks / s_ns_per_tick_den;
    rem   = ticks % s_ns_per_tick_den;
    return whole * s_ns_per_tick_num
         + (rem * s_ns_per_tick_num) / s_ns_per_tick_den;
}

/* ---- Reads --------------------------------------------------------------- */

/* Pure tick-counter scaling for the LAPIC/PIT fallback source (no
 * hardware access; unit-testable). freq 0 = no tick source yet. */
uint64_t mono_lapic_ticks_to_ns(uint64_t ticks, uint32_t freq_hz)
{
    if (freq_hz == 0)
        return 0;
    /* Split division: ticks * 1e9 overflows u64 after ~106 days at a
     * 2000 Hz tick; (ticks % freq) < freq keeps the partial in range */
    return (ticks / freq_hz) * 1000000000ULL
         + ((ticks % freq_hz) * 1000000000ULL) / freq_hz;
}

/* ---- Tick-frequency epoch (resolution-change monotonicity) --------------
 * A lifetime tick counter must NEVER be rescaled by the live frequency:
 * ticks accumulated at 100 Hz converted at 1000 Hz would rewind the
 * clock 10x. On every frequency change the accumulated time is banked
 * into an ns epoch and only ticks SINCE the change use the new rate.
 * Readers run lock-free (mono_ns is ISR-callable); the writer is the
 * rare KeSetTimerResolution path. */
static seqlock_t s_tick_epoch_lock = SEQLOCK_INIT;
static uint64_t  s_tick_epoch_ns;
static uint64_t  s_tick_epoch_ticks;
static uint32_t  s_tick_epoch_freq;   /* rate for ticks SINCE the epoch;
                                       * part of the snapshot so a reader
                                       * never pairs an old epoch with a
                                       * new live frequency */

void mono_clock_tick_rebase(uint32_t new_freq_hz)
{
    extern uint64_t system_get_ticks(void);
    uint64_t t;

    seqlock_write_lock(&s_tick_epoch_lock);
    /* Sample INSIDE the critical section: a pre-lock sample from a
     * concurrent rebase could be older than the published epoch and
     * underflow the delta. Clamp as a second layer. */
    t = system_get_ticks();
    if (t < s_tick_epoch_ticks)
        t = s_tick_epoch_ticks;
    /* Bank at the OLD stored rate -- the ticks being banked were
     * accumulated under it, never under the incoming rate */
    s_tick_epoch_ns   += mono_lapic_ticks_to_ns(t - s_tick_epoch_ticks,
                                                s_tick_epoch_freq);
    s_tick_epoch_ticks = t;
    s_tick_epoch_freq  = new_freq_hz;
    seqlock_write_unlock(&s_tick_epoch_lock);
}

/* ---- PMTMR 64-bit epoch (wrap extension) --------------------------------
 * The ACPI PM timer is a 24/32-bit free-running counter; the 24-bit form
 * wraps every ~4.69 s. mono_clock_pmtmr_advance() (called from the timer ISR
 * far faster than half the wrap) banks elapsed ns into a 64-bit epoch so a
 * lock-free reader only ever extends a short masked delta since the last
 * advance. Snapshot {epoch_ns, last_raw} (declared in the State section) is
 * seqlock-protected so a reader never pairs a stale epoch with a fresh raw
 * sample. */
uint64_t mono_pmtmr_delta_ns(uint32_t last_raw, uint32_t now_raw, uint32_t mask)
{
    /* Masked subtraction extends across at most one wrap. delta < 2^32, so
     * delta * 1e9 < 4.3e18 fits in u64 (max ~1.8e19) -- no split needed. */
    uint32_t delta = (now_raw - last_raw) & mask;
    return (uint64_t)delta * 1000000000ULL / PMTMR_FREQ_HZ;
}

/* Advance every Nth tick, not every tick: the epoch only has to be refreshed
 * faster than half the 24-bit wrap (~2.34 s). Even at the 64 Hz default tick
 * (15.625 ms) this is 125 ms per refresh -- an ~18x margin -- while doing ZERO
 * PM timer port I/O on the other 7 ticks (port reads are slow ACPI-register
 * I/O; keeping them off every tick avoids loading the timer ISR). Called only
 * from the BSP timer ISR, so the plain counter is single-threaded. */
#define PMTMR_ADVANCE_TICKS 8

void mono_clock_pmtmr_advance(void)
{
    static uint32_t s_skip;
    uint32_t now;

    if (__atomic_load_n(&s_source, __ATOMIC_ACQUIRE) != MONO_SRC_PMTMR)
        return;
    if (++s_skip < PMTMR_ADVANCE_TICKS)
        return;
    s_skip = 0;
    now = acpi_pmtimer_read_value() & s_pmtmr_mask;
    seqlock_write_lock(&s_pmtmr_lock);
    s_pmtmr_epoch_ns += mono_pmtmr_delta_ns(s_pmtmr_last_raw, now, s_pmtmr_mask);
    s_pmtmr_last_raw  = now;
    seqlock_write_unlock(&s_pmtmr_lock);
}

uint64_t mono_ns(void)
{
    uint64_t ticks;

    /* Acquire-load pairs with the RELEASE publish in mono_clock_init, so a
     * reader that sees MONO_SRC_PMTMR also sees the fully-seeded epoch. */
    switch (__atomic_load_n(&s_source, __ATOMIC_ACQUIRE)) {
    case MONO_SRC_TSC:
        ticks = rdtsc_ordered();
        /* Apply per-CPU TSC offset for SMP coherence */
        {
            struct per_cpu_data *cpu = smp_this_cpu();
            if (cpu)
                ticks = (uint64_t)((int64_t)ticks + cpu->tsc_offset);
        }
        /* Use 128-bit multiply to avoid overflow:
         * result = (ticks * num) / den
         * For ~4 GHz TSC and 64-bit ticks, ticks * 1e9 overflows at ~18 seconds.
         * Split: ns = (ticks / den) * num + ((ticks % den) * num) / den */
        {
            uint64_t whole = ticks / s_ns_per_tick_den;
            uint64_t rem   = ticks % s_ns_per_tick_den;
            return whole * s_ns_per_tick_num
                 + (rem * s_ns_per_tick_num) / s_ns_per_tick_den;
        }

    case MONO_SRC_HPET:
        return hpet_ns();

    case MONO_SRC_PMTMR:
        /* Precise PMTMR read: banked epoch ns + the masked delta since the last
         * ISR advance, glitch-filtered (median-of-3) so a single bad sample
         * cannot fabricate a forward jump, then clamped to the monotonic floor
         * so a residual glitch or a missed wrap can never step backward. The
         * raw counter is sampled INSIDE the read attempt so an advance between
         * the snapshot and the sample forces a retry. Hot/coarse callers use
         * the cheaper mono_ns_coarse() (no port I/O) instead. */
        {
            uint64_t seq, base_ns, cand;
            uint32_t base_raw, now;
            do {
                seq      = seqlock_read_begin(&s_pmtmr_lock);
                base_ns  = s_pmtmr_epoch_ns;
                base_raw = s_pmtmr_last_raw;
                now      = acpi_pmtimer_read_value() & s_pmtmr_mask;
            } while (seqlock_read_retry(&s_pmtmr_lock, seq));
            cand = base_ns + mono_pmtmr_delta_ns(base_raw, now, s_pmtmr_mask);
            return pmtmr_mono_floor(cand);
        }

    case MONO_SRC_LAPIC:
        /* LAPIC tick count is not directly readable as a monotonic counter.
         * Use system_get_ticks() (PIT/LAPIC interrupt counter): banked
         * epoch ns + ticks-since-epoch at the LIVE frequency, so a
         * runtime resolution change can never rewind the clock. */
        {
            extern uint64_t system_get_ticks(void);
            uint64_t seq, base_ns, base_ticks, t;
            uint32_t base_freq;
            do {
                seq        = seqlock_read_begin(&s_tick_epoch_lock);
                base_ns    = s_tick_epoch_ns;
                base_ticks = s_tick_epoch_ticks;
                base_freq  = s_tick_epoch_freq;
                /* Sample INSIDE the read attempt: a rebase between the
                 * snapshot and the tick sample must force a retry, or a
                 * post-rebase tick pairs with the stale rate */
                t = system_get_ticks();
            } while (seqlock_read_retry(&s_tick_epoch_lock, seq));
            if (t < base_ticks)
                t = base_ticks;
            return base_ns + mono_lapic_ticks_to_ns(t - base_ticks,
                                                    base_freq);
        }

    default:
        return 0;
    }
}

uint64_t mono_ns_coarse(void)
{
    /* Cheap monotonic read for the scheduler / uptime hot path. For PMTMR the
     * precise mono_ns() costs 3 port reads (a glitch-filtered median); return
     * the cached epoch ns instead -- accurate to the ISR advance interval,
     * monotonic (the epoch only banks positive masked deltas), and free of port
     * I/O. Other sources are already cheap, so fall through to mono_ns(). */
    if (__atomic_load_n(&s_source, __ATOMIC_ACQUIRE) == MONO_SRC_PMTMR) {
        uint64_t seq, ns;
        do {
            seq = seqlock_read_begin(&s_pmtmr_lock);
            ns  = s_pmtmr_epoch_ns;
        } while (seqlock_read_retry(&s_pmtmr_lock, seq));
        /* Share the SAME monotonic floor as precise mono_ns(): a precise read
         * may have raised the floor past the cached epoch, so clamping here
         * keeps coarse non-decreasing relative to a prior precise read when the
         * two APIs are mixed (uptime_ns / wall_clock do mix them). */
        return pmtmr_mono_floor(ns);
    }
    return mono_ns();
}

uint64_t mono_filetime_units(void)
{
    /* FILETIME units = 100 ns intervals = mono_ns() / 100 */
    return mono_ns() / 100;
}

/* ---- Info ---------------------------------------------------------------- */

const char *mono_clock_source_name(void)
{
    switch (s_source) {
    case MONO_SRC_TSC:   return "TSC";
    case MONO_SRC_HPET:  return "HPET";
    case MONO_SRC_PMTMR: return "PMTMR";
    case MONO_SRC_LAPIC: return "LAPIC";
    default:             return "none";
    }
}

uint32_t mono_clock_source_id(void)
{
    return s_source;
}
